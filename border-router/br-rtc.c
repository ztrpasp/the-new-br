#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>

#include <rte_debug.h>
#include <rte_ethdev.h>
#include <rte_lcore.h>
#include <rte_timer.h>
#include <rte_mbuf.h>
#include <rte_cycles.h>
#include "common.h"
#include "br_conf.h"
#include "br_tables.h"
#include "br.h"

/* Idle sleep (meson option idle_sleep_us, default 0 = pure busy polling as on
 * the physical deployment). When > 0, the lcore sleeps that many microseconds
 * after BR_IDLE_SPIN_ROUNDS consecutive empty polls, so idle BRs in the
 * container lab do not keep a whole CPU at 100%. Costs tens of microseconds of
 * latency on the first packet after an idle period. */
#ifndef BR_IDLE_SLEEP_US
#define BR_IDLE_SLEEP_US 0
#endif
#define BR_IDLE_SPIN_ROUNDS 64

extern struct br_conf g_br_conf;
extern struct br_tables g_br_tables;

/* main processing loop */
int br_main_loop(__attribute__((unused)) void *dummy)
{
    uint32_t lcore_id = rte_lcore_id();
    uint64_t prev_tsc, cur_tsc, prev_tsc_timer;
    struct br_conf *br_conf = &g_br_conf;
    struct br_tables *br_tables = &g_br_tables;
    struct rtc_qconf *rtc_qconf = &br_conf->rtc_qconf[lcore_id];
    struct rx_qconf *rx_qconf = &rtc_qconf->rx_qconf;
    struct tx_qconf *tx_qconf = &rtc_qconf->tx_qconf;
    const uint64_t drain_tsc = (rte_get_tsc_hz() + US_PER_S - 1) /
        US_PER_S * BURST_TX_DRAIN_US;

    prev_tsc = 0;
    prev_tsc_timer = 0;

    if (rx_qconf->n_queues == 0) {
        RTE_LOG(INFO, BR, "lcore %u has nothing to do\n", lcore_id);
        return 0;
    }

    RTE_LOG(INFO, BR, "entering main loop on lcore %u\n", lcore_id);

    for (int i = 0; i < rx_qconf->n_queues; i++) {
        int portid = rx_qconf->queue_list[i].port_id;
        int queueid = rx_qconf->queue_list[i].queue_id;
        RTE_LOG(INFO, BR, " -- lcoreid=%d portid=%d rxqueueid=%d\n",
            lcore_id, portid, queueid);
    }

    br_tables_rcu_register(br_tables, lcore_id);
    

    struct rte_mbuf *pkts[MAX_PKT_BURST];
#if BR_IDLE_SLEEP_US > 0
    uint32_t idle_rounds = 0;
#endif
    while (!br_conf->force_quit) {
#if BR_IDLE_SLEEP_US > 0
        bool got_packets = false;
#endif
        cur_tsc = rte_rdtsc();
        /* TX burst queue drain */
        uint64_t diff_tsc = cur_tsc - prev_tsc;
        if (unlikely(diff_tsc > drain_tsc)) {

            for (int i = 0; i < tx_qconf->n_ports; ++i) {
                uint16_t port_id = tx_qconf->port_id[i];
                uint16_t queue_id = tx_qconf->queue_id[port_id];
                struct pkt_list *pkt_list = &tx_qconf->tx_mbufs[port_id];
                if (pkt_list->len == 0)
                    continue;
                rtc_tx_burst(pkt_list, port_id, queue_id);
            }

            prev_tsc = cur_tsc;
        }

        /* Call the timer handler on each call. Call rte_timer_manage() every
         * ~10ms (at 2Ghz). */
        diff_tsc = cur_tsc - prev_tsc_timer;
        if (unlikely(diff_tsc > TIMER_RESOLUTION_CYCLES)) {
            br_tables_rcu_online(br_tables, lcore_id);
            rte_timer_manage();
            br_tables_rcu_offline(br_tables, lcore_id);
            prev_tsc_timer = cur_tsc;
        }

        /* Read packet from RX queues */
        for (uint16_t i = 0; i < rx_qconf->n_queues; ++i) {
            uint16_t port_id = rx_qconf->queue_list[i].port_id;
            uint16_t queue_id = rx_qconf->queue_list[i].queue_id;
            uint16_t n_rx = rte_eth_rx_burst(port_id, queue_id, pkts, MAX_PKT_BURST);
            if (n_rx == 0) {
                continue;
            }
#if BR_IDLE_SLEEP_US > 0
            got_packets = true;
#endif
            //printf("[lcf] RX is not empty\n");
            br_tables_rcu_online(br_tables, lcore_id);
            process_packets(n_rx, pkts, br_conf, br_tables);
            br_tables_rcu_offline(br_tables, lcore_id);
        }

#if BR_IDLE_SLEEP_US > 0
        if (got_packets) {
            idle_rounds = 0;
        } else if (++idle_rounds >= BR_IDLE_SPIN_ROUNDS) {
            rte_delay_us_sleep(BR_IDLE_SLEEP_US);
        }
#endif
    }

    br_tables_rcu_unregister(br_tables, lcore_id);

    return 0;
}
