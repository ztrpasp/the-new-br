#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>

#include <rte_debug.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_arp.h>
#include <rte_ip.h>
#include <rte_hash.h>
#include <rte_malloc.h>
#include <rte_timer.h>
#include <rte_rwlock.h>
#include <rte_ring.h>

#include "br.h"
#include "common.h"
#include "br_conf.h"
#include "br_tables.h"
#include "send_pkt.h"

extern struct br_conf g_br_conf;
extern struct br_tables g_br_tables;

/**
 * @brief
 * rx loop will NOT access \p br_tables, there's no need to register RCU.
 * @return int
 */
int br_rx_loop(__attribute__((unused)) void *dummy)
{
    uint64_t diff_tsc, cur_tsc, prev_tsc_timer;
    struct br_conf * const br_conf = &g_br_conf;
    struct p0_qconf * const p0_qconf = &br_conf->p0_qconf;
    struct rx_qconf * const rx_qconf = &p0_qconf->rx_qconf;

    int const lcore_id = rte_lcore_id();
    if (lcore_id != p0_qconf->rx_lcore_id) {
        printf("Error: expected lcore %d but rx thread runs on lcore %d\n",
            p0_qconf->rx_lcore_id, lcore_id);
        br_conf->force_quit = true;
        return -1;
    }

    RTE_LOG(INFO, BR, "entering rx loop on lcore %u\n", lcore_id);
    for (int i = 0; i < rx_qconf->n_queues; i++) {
        uint16_t port_id = rx_qconf->queue_list[i].port_id;
        uint16_t queue_id = rx_qconf->queue_list[i].queue_id;
        RTE_LOG(INFO, BR, " -- lcoreid=%d port_id=%d rxqueue_id=%d\n",
            lcore_id, port_id, queue_id);
    }

    prev_tsc_timer = 0;
    struct rte_mbuf *pkts[MAX_PKT_BURST];
    int worker_idx = 0;
    int const n_workers = p0_qconf->n_workers;
    while (!br_conf->force_quit) {
        cur_tsc = rte_rdtsc();

        /* Call the timer handler on each call. Call rte_timer_manage() every
         * ~10ms (at 2Ghz). */
        diff_tsc = cur_tsc - prev_tsc_timer;
        if (unlikely(diff_tsc > TIMER_RESOLUTION_CYCLES)) {
            rte_timer_manage();
            prev_tsc_timer = cur_tsc;
        }

        /* Read packet from RX queues */
        for (int i = 0; i < rx_qconf->n_queues; ++i) {
            int const port_id = rx_qconf->queue_list[i].port_id;
            int const queue_id = rx_qconf->queue_list[i].queue_id;
            int const n_rx = rte_eth_rx_burst(port_id, queue_id, pkts, MAX_PKT_BURST);
            if (n_rx == 0) {
                continue;
            }
            int ret = 0;
            while (ret == 0 && !br_conf->force_quit) {
                int worker_lcore_id = p0_qconf->worker_lcore_id[worker_idx];
                ret = rte_ring_sp_enqueue_bulk(rx_qconf->rings[worker_lcore_id],
                    (void **)pkts, n_rx, NULL);
                int next_worker_idx = worker_idx + 1;
                worker_idx = next_worker_idx >= n_workers ? 0 : next_worker_idx;
            }
        }
    }

    return 0;
}

/**
 * @brief
 * tx loop will NOT access \p br_tables, there's no need to register RCU.
 * @return int
 */
int br_tx_loop(__attribute__((unused)) void *dummy)
{
    uint64_t prev_tsc, diff_tsc, cur_tsc, prev_tsc_timer;
    struct br_conf * const br_conf = &g_br_conf;
    struct p0_qconf * const p0_qconf = &br_conf->p0_qconf;
    struct tx_qconf * const tx_qconf = &p0_qconf->tx_qconf;
    const uint64_t drain_tsc = (rte_get_tsc_hz() + US_PER_S - 1) /
        US_PER_S * BURST_TX_DRAIN_US;

    const int lcore_id = rte_lcore_id();
    if (lcore_id != p0_qconf->tx_lcore_id) {
        printf("Error: expected lcore %d but tx thread runs on lcore %d\n",
            p0_qconf->tx_lcore_id, lcore_id);
        br_conf->force_quit = true;
        return -1;
    }

    #define TX_DEQUEUE_SIZE 32
    struct rte_mbuf *pkts[TX_DEQUEUE_SIZE];
    struct pkt_list pkt_lists[RTE_MAX_ETHPORTS];
    memset(pkt_lists, 0, sizeof(pkt_lists));

    RTE_LOG(INFO, BR, "entering tx loop on lcore %u\n", lcore_id);
    prev_tsc = 0;
    prev_tsc_timer = 0;
    while (!br_conf->force_quit) {
        cur_tsc = rte_rdtsc();

        /* fetch processed packets from workers to tx_mbufs */
        for (int i = 0; i < p0_qconf->n_workers; i++) {
            int worker_lcore_id = p0_qconf->worker_lcore_id[i];
            int n_pkts = rte_ring_sc_dequeue_burst(tx_qconf->rings[worker_lcore_id],
                (void **)pkts, sizeof(pkts) / sizeof(*pkts), NULL);
            if (n_pkts == 0) {
                continue;
            }

            uint16_t n_unsent = 0;
            for (int j = 0; j < n_pkts; j++) {
                int port_id = pkts[j]->port;
                pkt_lists[port_id].pkts[pkt_lists[port_id].len++] = pkts[j];
                int n = pkt_lists[port_id].len;
                if (unlikely(n >= MAX_PKT_BURST)) {
                    int queue_id = tx_qconf->queue_id[port_id];
                    n_unsent = p0_tx_burst(pkt_lists[port_id].pkts, n, port_id, queue_id);
                    pkt_lists[port_id].len = 0;
                }
            }
        }

        /* send packets every ~10ms (at 2Ghz). */
        diff_tsc = cur_tsc - prev_tsc;
        if (unlikely(diff_tsc > TIMER_RESOLUTION_CYCLES)) {
            for (int i = 0; i < tx_qconf->n_ports; ++i) {
                int port_id = tx_qconf->port_id[i];
                int n_pkts_to_send = pkt_lists[port_id].len;
                if (n_pkts_to_send == 0) {
                    continue;
                }
                int queue_id = tx_qconf->queue_id[port_id];
                int n_unsent = p0_tx_burst(pkt_lists[port_id].pkts, n_pkts_to_send, port_id, queue_id);
                pkt_lists[port_id].len = 0;
            }
            prev_tsc = cur_tsc;
        }
    }

    return 0;
}

int br_worker_loop(__attribute__((unused)) void *dummy)
{
    uint64_t prev_tsc, diff_tsc, cur_tsc, prev_tsc_timer;
    int const lcore_id = rte_lcore_id();
    struct br_conf * const br_conf = &g_br_conf;
    struct p0_qconf * const p0_qconf = &br_conf->p0_qconf;
    struct br_tables *br_tables = &g_br_tables;
    struct worker_qconf * const worker_qconf = &p0_qconf->workers_qconf[lcore_id];

    /* Check if this lcore is a worker. */
    bool is_worker = false;
    for (int i = 0; i < p0_qconf->n_workers; i++) {
        if (lcore_id == p0_qconf->worker_lcore_id[i]) {
            is_worker = true;
        }
    }
    if (!is_worker) {
        printf("Error: expected lcore %d is not a worker\n", lcore_id);
        br_conf->force_quit = true;
        return -1;
    }

    struct rte_mbuf *pkts[WORKER_RING_SIZE];

    RTE_LOG(INFO, BR, "entering worker loop on lcore %u\n", lcore_id);
    prev_tsc = 0;
    prev_tsc_timer = 0;
    while (!br_conf->force_quit) {
        cur_tsc = rte_rdtsc();
        int n_pkts = rte_ring_sc_dequeue_burst(worker_qconf->rx_ring,
            (void **)pkts, 32, NULL); // do not have to be WORKER_RING_SIZE
        if (n_pkts > 0) {
            /* process packets here */
            process_packets(n_pkts, pkts, br_conf, br_tables);
        }

        /* Call the timer handler on each call. Call rte_timer_manage() every
         * ~10ms (at 2Ghz). */
        diff_tsc = cur_tsc - prev_tsc_timer;
        if (unlikely(diff_tsc > TIMER_RESOLUTION_CYCLES)) {
            rte_timer_manage();
            prev_tsc_timer = cur_tsc;
        }
    }

    return 0;
}

// #ifndef NIDT_TEST

struct nidt_test_entry {
    struct nid_table *owner;
    struct rte_timer timer;
    uint8_t nid[SIZE_NID];
};

/* NIDT test timer callback. This operation is multi-thread safe. */
void nidt_timer_cb(__attribute__((unused)) struct rte_timer *timer, void *arg)
{
    struct nidt_test_entry *p = (struct nidt_test_entry *)arg;
    rte_timer_stop_sync(timer);
    nid_remove(p->owner, p->nid);
    rte_free(p);
}

/* performance test */
int br_test_loop(__attribute__((unused)) void *dummy)
{
    uint64_t diff_tsc, cur_tsc, prev_tsc_timer;
    struct br_conf * const br_conf = &g_br_conf;
    struct nid_table * const nid_table = &g_br_tables.nid_table;
    struct p0_qconf * const p0_qconf = &br_conf->p0_qconf;
    const uint64_t cpu_cycles_per_sec = rte_get_tsc_hz();
    const uint64_t cpu_cycles_per_ten_milliseconds = cpu_cycles_per_sec / 100;
    double nidt_add_rate = 100; // add/delete nidt_add_rate NID entries per second
    double nidt_entry_expire_seconds = 0.01; // add/delete nidt_add_per_cycle NID entries per CPU cycle

    int const lcore_id = rte_lcore_id();

    #define MAX_LINE_LEN 128
    char line[MAX_LINE_LEN];
    FILE *fp = fopen(br_conf->config_path, "r");
    if (fp == NULL) {
        BR_INFO("Cannot open file %s\n", br_conf->config_path);
        br_conf->force_quit = true;
        return -1;
    }
    char *ret = fgets(line, sizeof(line), fp);
    while (ret != NULL) {
        if (startswith(line, "@nidt-test")) {
            sscanf(line, "@nidt-test %lf %lf", &nidt_add_rate, &nidt_entry_expire_seconds);
            BR_INFO("nidt_add_rate: %.2f, nidt_entry_expire_seconds: %.2f",
                nidt_add_rate, nidt_entry_expire_seconds);
            break;
        }
        ret = fgets(line, sizeof(line), fp);
    }
    fclose(fp);
    double nidt_add_per_cycle = nidt_add_rate / cpu_cycles_per_sec;

    RTE_LOG(INFO, BR, "entering test loop on lcore %u\n", lcore_id);

    uint64_t cnt = 0, cnt_last = 0;
    uint64_t prev_tsc_rate_report = 0;
    prev_tsc_timer = 0;
    uint64_t next_nidt_add_tsc = 0;
    uint8_t sk[SIZE_HOST_SK];
    uint32_t ipv4_addr;
    while (!br_conf->force_quit) {
        cur_tsc = rte_rdtsc();

        if (cur_tsc > next_nidt_add_tsc) {
            next_nidt_add_tsc = cur_tsc + rand_exp(nidt_add_per_cycle); // schedule next event
            // prev_tsc = cur_tsc;
            struct nidt_test_entry *p = rte_malloc("nidt_test_entry",sizeof(struct nidt_test_entry), 0);
            p->owner = nid_table;
            *(uint64_t *)p->nid = ~0;
            *(uint32_t *)(p->nid + 8) = rand();
            *(uint32_t *)(p->nid + 12) = rand();
            ipv4_addr = rand();
            for (int i = 0; i < 32; i += 4) {
                *(uint32_t *)(sk + i) = rand();
            }
            nid_add(nid_table, p->nid, ipv4_addr, sk);
            cnt++;
            rte_timer_init(&p->timer);
            uint64_t ticks = rte_get_timer_hz() * nidt_entry_expire_seconds;
            rte_timer_reset_sync(&p->timer, ticks, SINGLE, lcore_id, nidt_timer_cb, p);
        }

        /* Call the timer handler on each call. Call rte_timer_manage() every
         * ~10ms (at 2Ghz). */
        diff_tsc = cur_tsc - prev_tsc_timer;
        if (unlikely(diff_tsc > cpu_cycles_per_ten_milliseconds)) {
            rte_timer_manage();
            prev_tsc_timer = cur_tsc;
        }

        diff_tsc = cur_tsc - prev_tsc_rate_report;
        if (unlikely(diff_tsc > cpu_cycles_per_sec * 4)) {
            printf("%lu\r\n", (cnt - cnt_last) / 4);
            cnt_last = cnt;
            prev_tsc_rate_report = cur_tsc;
        }
    }

    return 0;
}
// #endif

// #ifndef ODCT_TEST

/* performance test */
int br_odct_test_loop(__attribute__((unused)) void *dummy)
{
    uint64_t diff_tsc, cur_tsc, prev_tsc_timer;
    struct br_conf * const br_conf = &g_br_conf;
    struct odc_table * const odc_table = &g_br_tables.odc_table;
    struct p0_qconf * const p0_qconf = &br_conf->p0_qconf;
    const uint64_t cpu_cycles_per_sec = rte_get_tsc_hz();
    const uint64_t cpu_cycles_per_ten_milliseconds = cpu_cycles_per_sec / 100;
    double odct_add_rate = 100; // add/delete odct_add_rate ODCT entries per second
    double odct_entry_expire_seconds = 10; // add/delete odct_add_per_cycle ODCT entries per CPU cycle

    int const lcore_id = rte_lcore_id();

    #define MAX_LINE_LEN 128
    char line[MAX_LINE_LEN];
    FILE *fp = fopen(br_conf->config_path, "r");
    if (fp == NULL) {
        BR_INFO("Cannot open file %s\n", br_conf->config_path);
        br_conf->force_quit = true;
        return -1;
    }
    char *ret = fgets(line, sizeof(line), fp);
    while (ret != NULL) {
        if (startswith(line, "@odct-test")) {
            sscanf(line, "@odct-test %lf %lf", &odct_add_rate, &odct_entry_expire_seconds);
            BR_INFO("odct_add_rate: %.2f, odct_entry_expire_seconds: %.2f",
                odct_add_rate, odct_entry_expire_seconds);
            break;
        }
        ret = fgets(line, sizeof(line), fp);
    }
    fclose(fp);
    double odct_add_per_cycle = odct_add_rate / cpu_cycles_per_sec;

    RTE_LOG(INFO, BR, "entering ODCT test loop on lcore %u\n", lcore_id);

    uint64_t cnt = 0, cnt_last = 0;
    uint64_t prev_tsc_rate_report = 0;
    prev_tsc_timer = 0;
    uint64_t next_odct_add_tsc = 0;

    uint8_t nid_provider[SIZE_NID] = {0};
    uint8_t l_sid[SIZE_LSID] = {0};
    uint8_t nid_consumer[SIZE_NID] = {0};
    const uint32_t pids_len = 16;
    uint8_t pids[16] = {0};
    uint32_t rn;

    while (!br_conf->force_quit) {
        cur_tsc = rte_rdtsc();

        if (cur_tsc > next_odct_add_tsc) {
            next_odct_add_tsc = cur_tsc + rand_exp(odct_add_per_cycle); // schedule next event
            *(uint64_t *)nid_provider = ~0;
            *(uint32_t *)(nid_provider + 8) = rand();
            *(uint64_t *)l_sid = ~0;
            *(uint32_t *)(l_sid + 8) = rand();
            *(uint64_t *)nid_consumer = ~0;
            *(uint32_t *)(nid_consumer + 8) = rand();
            for (int i = 0; i < pids_len; i += 4) {
                *(uint32_t *)(pids + i) = rand();
            }
            rn = rand();
            odc_add_entry(odc_table, nid_provider, l_sid, nid_consumer, pids,
                pids_len, rn, odct_entry_expire_seconds);
            cnt++;
        }

        /* Call the timer handler on each call. Call rte_timer_manage() every
         * ~10ms (at 2Ghz). */
        diff_tsc = cur_tsc - prev_tsc_timer;
        if (unlikely(diff_tsc > cpu_cycles_per_ten_milliseconds)) {
            rte_timer_manage();
            prev_tsc_timer = cur_tsc;
        }

        diff_tsc = cur_tsc - prev_tsc_rate_report;
        if (unlikely(diff_tsc > cpu_cycles_per_sec * 4)) {
            printf("%lu\r\n", (cnt - cnt_last) / 4);
            cnt_last = cnt;
            prev_tsc_rate_report = cur_tsc;
        }
    }

    return 0;
}
// #endif