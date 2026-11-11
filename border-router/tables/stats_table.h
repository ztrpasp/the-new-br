#ifndef _BR_STATS_TABLE_H_
#define _BR_STATS_TABLE_H_

#include <stdint.h>
#include <rte_rwlock.h>
#include "../packet/rte_CoLoR.h"

/* Statistics of color packets
 * May not be accurate when ARP fails */
#define SIZE_STATS_TYPE 8

#define ANN (COLOR_ANN_TYPE - 1)
#define GET (COLOR_GET_TYPE - 1)
#define DATA (COLOR_DATA_TYPE - 1)
#define CONTROL (COLOR_CONTROL_TYPE - 1)

struct stats_pkts_bytes {
    uint64_t pkts;
    uint64_t bytes;
};

struct stats_tx_rx {
    struct stats_pkts_bytes tx;
    struct stats_pkts_bytes rx;
};

struct stats_sum {
    struct stats_tx_rx port[RTE_MAX_ETHPORTS];
    uint64_t num_bad_inbound_pkt_pid_unknown;
    uint64_t num_bad_inbound_pkt_pid[256];
};

struct stats_port {
    struct stats_tx_rx type[4];
};

struct stats_table {
#ifndef NO_RATE_RECORDING
    struct stats_port port[RTE_MAX_ETHPORTS];
#endif
    uint64_t num_bad_inbound_pkt_pid_unknown;
    uint64_t num_bad_inbound_pkt_pid[256]; // for each AS
} __rte_cache_aligned;

void stats_table_init(struct stats_table *p);

#define stats_inc_bad_pid(p, as_id, is_unkown_as) { \
    if (is_unkown_as) { \
        (p)->num_bad_inbound_pkt_pid_unknown++; \
    } else { \
        (p)->num_bad_inbound_pkt_pid[asid]++; \
    } \
}

#ifndef NO_RATE_RECORDING

/**
 * @brief pkt_type: ann, get, data or ctrl
 * tr_type: tx or rx
 *
 */
#define stats_inc(p, portid, pkt_type, tr_type, pkt_size) { \
    (p)->port[portid].type[pkt_type - 1].tr_type.pkts++; \
    (p)->port[portid].type[pkt_type - 1].tr_type.bytes += pkt_size; \
}

#define stats_pkts(p, portid, pkt_type, tr_type, pkt_size) \
    (p)->port[portid].type[pkt_type - 1].tr_type.pkts

#define stats_bytes(p, portid, pkt_type, tr_type, pkt_size) \
    (p)->port[portid].type[pkt_type - 1].tr_type.bytes

#else
#define stats_inc(...)
#define stats_inc(...)
#endif

char *stats_table_to_string(struct stats_table tables[], int n, bool display_rate);



#endif /* _BR_STATS_TABLE_H_ */