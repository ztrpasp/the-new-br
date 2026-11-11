#include <stdio.h>
#include <rte_ethdev.h>
#include "../utils/utils.h"
#include "../ansi_escape_seq.h"
#include "../common.h"
#include "../br_conf.h"
#include "stats_table.h"

void stats_table_init(struct stats_table *p)
{
    memset(p, 0, sizeof(struct stats_table));
}

/* Convert to readable values */
static const char * to_unit(double x)
{
    const char *r = "";
    if (x >= 1e3 && x < 1e6) {
        r = "K";
    } else if (x >= 1e6 && x < 1e9) {
        r = "M";
    } else if (x >= 1e9 && x < 1e12) {
        r = "G";
    } else if (x >= 1e12 && x < 1e15) {
        r = "T";
    } else {
        r = "";
    }
    return r;
}

static double to_val(double x)
{
    double y;
    if (x >= 1e3 && x < 1e6) {
        y = x / 1e3;
    } else if (x >= 1e6 && x < 1e9) {
        y = x / 1e6;
    } else if (x >= 1e9 && x < 1e12) {
        y = x / 1e9;
    } else if (x >= 1e12 && x < 1e15) {
        y = x / 1e12;
    } else {
        y = x;
    }
    return y;
}

extern struct br_conf g_br_conf;

static void stats_table_sum(struct stats_table tables[], int n, struct stats_table *stats,
    struct stats_sum *stats_sum)
{
    uint16_t const nb_ports = rte_eth_dev_count_avail();
    int const num_as = sizeof(stats_sum->num_bad_inbound_pkt_pid)
        / sizeof(stats_sum->num_bad_inbound_pkt_pid[0]);
    memset(stats, 0, sizeof(struct stats_table));
    memset(stats_sum, 0, sizeof(struct stats_sum));
    for (int i = 0; i < n; i++) {
#ifndef NO_RATE_RECORDING
        for (int j = 0; j < nb_ports; j++) {
            for (int t = 0; t < 4; t++) {
                stats->port[j].type[t].rx.pkts += tables[i].port[j].type[t].rx.pkts;
                stats->port[j].type[t].rx.bytes += tables[i].port[j].type[t].rx.bytes;
                stats->port[j].type[t].tx.pkts += tables[i].port[j].type[t].tx.pkts;
                stats->port[j].type[t].tx.bytes += tables[i].port[j].type[t].tx.bytes;

                stats_sum->port[j].rx.pkts += tables[i].port[j].type[t].rx.pkts;
                stats_sum->port[j].rx.bytes += tables[i].port[j].type[t].rx.bytes;
                stats_sum->port[j].tx.pkts += tables[i].port[j].type[t].tx.pkts;
                stats_sum->port[j].tx.bytes += tables[i].port[j].type[t].tx.bytes;
            }
        }
#endif
        stats_sum->num_bad_inbound_pkt_pid_unknown += tables[i].num_bad_inbound_pkt_pid_unknown;
        stats->num_bad_inbound_pkt_pid_unknown += tables[i].num_bad_inbound_pkt_pid_unknown;
        for (int k = 0; k < num_as; k++) {
            stats_sum->num_bad_inbound_pkt_pid[k] += tables[i].num_bad_inbound_pkt_pid[k];
            stats->num_bad_inbound_pkt_pid[k] += tables[i].num_bad_inbound_pkt_pid[k];
        }
    }
}

#ifndef NO_RATE_RECORDING

char *stats_table_to_string(struct stats_table tables[], int n, bool display_rate)
{
    StringBuilder *sb = new_sb();
    char buf[128];

    /* statistics for each port on last function call */
    static uint64_t prev_num_tx_pkt[RTE_MAX_ETHPORTS] = {0};
    static uint64_t prev_num_tx_bytes[RTE_MAX_ETHPORTS] = {0};
    static uint64_t prev_num_rx_pkt[RTE_MAX_ETHPORTS] = {0};
    static uint64_t prev_num_rx_bytes[RTE_MAX_ETHPORTS] = {0};
    static struct stats_sum stats_sum_prev;
    static uint64_t prev_tsc = 0;

    /* Calculate elapsed time */
    uint64_t cur_tsc = rte_rdtsc();
    double time = (double)(cur_tsc - prev_tsc) / rte_get_tsc_hz();
    prev_tsc = cur_tsc;

    struct stats_table stats;
    struct stats_sum stats_sum;
    stats_table_sum(tables, n, &stats, &stats_sum);

    unsigned nb_ports = rte_eth_dev_count_avail();
    for (uint16_t i = 0; i < nb_ports; i++) {
        /* skip ports that are not enabled */
        if ((g_br_conf.enabled_port_mask & (1 << i)) == 0) {
            continue;
        }

        /* bold(\x1b[1m) green(\x1b[38;5;40m) front, reset(\x1b[0m) */
        sprintf(buf, BGRN "Port %u" RESET_STYLE "\r\n", i);
        sb_append(sb, buf);

        /* total tx/rx rate for the port */
        if (display_rate) {
            double tx_pps, tx_bps, rx_pps, rx_bps;
            tx_pps = (stats_sum.port[i].tx.pkts - stats_sum_prev.port[i].tx.pkts) / time;
            tx_bps = (stats_sum.port[i].tx.bytes - stats_sum_prev.port[i].tx.bytes) * 8 / time;
            rx_pps = (stats_sum.port[i].rx.pkts - stats_sum_prev.port[i].rx.pkts) / time;
            rx_bps = (stats_sum.port[i].rx.bytes - stats_sum_prev.port[i].rx.bytes) * 8 / time;
            char rate_buf[64];
            sprintf(rate_buf, "Tx pps: %.2f %spps", to_val(tx_pps), to_unit(tx_pps));
            sprintf(buf, "%-20s", rate_buf);
            sprintf(rate_buf, "Tx bps L2: %.2f %sbps", to_val(tx_bps), to_unit(tx_bps));
            sprintf(buf + strlen(buf), "%-20s\r\n", rate_buf);
            sprintf(rate_buf, "Rx pps: %.2f %spps", to_val(rx_pps), to_unit(rx_pps));
            sprintf(buf + strlen(buf), "%-20s", rate_buf);
            sprintf(rate_buf, "Rx bps L2: %.2f %sbps", to_val(rx_bps), to_unit(rx_bps));
            sprintf(buf + strlen(buf), "%-20s\r\n", rate_buf);
            sb_append(sb, buf);
        }

        /* update stats_sum_prev */
        stats_sum_prev.port[i].tx.pkts = stats_sum.port[i].tx.pkts;
        stats_sum_prev.port[i].tx.bytes = stats_sum.port[i].tx.bytes;
        stats_sum_prev.port[i].rx.pkts = stats_sum.port[i].rx.pkts;
        stats_sum_prev.port[i].rx.bytes = stats_sum.port[i].rx.bytes;

        /* detailed rx/tx stats */
        sprintf(buf, "%-10s %-14s %-14s %-14s %-14s\r\n", " ",
        "GET", "DATA", "ANN", "CONTROL");
        sb_append(sb, buf);
        sprintf(buf, "%-10s %-14ld %-14ld %-14ld %-14ld\r\n", "tx pkts",
            stats.port[i].type[GET].tx.pkts,
            stats.port[i].type[DATA].tx.pkts,
            stats.port[i].type[ANN].tx.pkts,
            stats.port[i].type[CONTROL].tx.pkts);
        sb_append(sb, buf);
        sprintf(buf, "%-10s %-14ld %-14ld %-14ld %-14ld\r\n", "tx bytes",
            stats.port[i].type[GET].tx.bytes,
            stats.port[i].type[DATA].tx.bytes,
            stats.port[i].type[ANN].tx.bytes,
            stats.port[i].type[CONTROL].tx.bytes);
        sb_append(sb, buf);
        sprintf(buf, "%-10s %-14ld %-14ld %-14ld %-14ld\r\n", "rx pkts",
            stats.port[i].type[GET].rx.pkts,
            stats.port[i].type[DATA].rx.pkts,
            stats.port[i].type[ANN].rx.pkts,
            stats.port[i].type[CONTROL].rx.pkts);
        sb_append(sb, buf);
        sprintf(buf, "%-10s %-14ld %-14ld %-14ld %-14ld\r\n\r\n", "rx bytes",
            stats.port[i].type[GET].rx.bytes,
            stats.port[i].type[DATA].rx.bytes,
            stats.port[i].type[ANN].rx.bytes,
            stats.port[i].type[CONTROL].rx.bytes);
        sb_append(sb, buf);
    }

    for (int i = 0; i < 256; i++) {
        if (stats_sum.num_bad_inbound_pkt_pid[i] > 0) {
            sprintf(buf, "Number of attack from AS %d: %ld\r\n", i,
                stats_sum.num_bad_inbound_pkt_pid[i]);
            sb_append(sb, buf);
        }
    }
    if (stats_sum.num_bad_inbound_pkt_pid_unknown > 0) {
        sprintf(buf, "Number of attack from unknown AS: %ld\r\n",
            stats_sum.num_bad_inbound_pkt_pid_unknown);
        sb_append(sb, buf);
    }

    char *s = sb_get(sb);
    free_sb(sb);
    return s;
}
#else // NO_RATE_RECORDING is defined

char *stats_table_to_string(struct stats_table tables[], int n, bool display_rate)
{
    char buf[128];
    StringBuilder *sb = new_sb();
    sb_append(sb, "stats is disabled\r\n");
    struct stats_table stats;
    struct stats_sum stats_sum;
    stats_table_sum(tables, n, &stats, &stats_sum);
    for (int i = 0; i < 256; i++) {
        if (stats_sum.num_bad_inbound_pkt_pid[i] > 0) {
            sprintf(buf, "Number of attack from AS %d: %ld\r\n", i,
                stats_sum.num_bad_inbound_pkt_pid[i]);
            sb_append(sb, buf);
        }
    }
    if (stats_sum.num_bad_inbound_pkt_pid_unknown > 0) {
        sprintf(buf, "Number of attack from unknown AS: %ld\r\n",
            stats_sum.num_bad_inbound_pkt_pid_unknown);
        sb_append(sb, buf);
    }
    char *s = sb_get(sb);
    free_sb(sb);
    return s;
}

#endif
