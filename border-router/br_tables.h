#ifndef __BR_TABLES_H__
#define __BR_TABLES_H__

#include <rte_common.h>
#include "tables/stats_table.h"
#include "tables/nid_table.h"
#include "tables/px_table.h"
#include "tables/peer_table.h"
#include "tables/port_table.h"
#include "tables/arp_table.h"
#include "tables/arp_pending_table.h"
#include "tables/odc_table.h"
#include "tables/ip_lpm_table.h"
// #include "tables/ipv4_to_v6_table.h"

struct br_tables {
    struct nid_table nid_table __rte_cache_aligned;
    struct peer_table peer_table __rte_cache_aligned;
    struct port_table port_table __rte_cache_aligned;
    struct arp_table arp_table __rte_cache_aligned;
    struct arp_pending_table arp_pending_table __rte_cache_aligned;
    struct odc_table odc_table;
    struct px_table *px_table;
    struct ip_lpm_table *ip_lpm_table;
    struct stats_table stats_table[RTE_MAX_LCORE] __rte_cache_aligned;
    // struct ipv6_table ipv6_table __rte_cache_aligned;
    /* ipv4 to ipv6 address transfer */
    uint32_t addr_num;
    uint32_t ipv4_addrs[10];
    uint8_t ipv6_addrs[10][16];
    struct rte_ether_addr hw_addrs[10];
};

void br_tables_rcu_register(struct br_tables *br_tables, uint32_t lcore_id);
void br_tables_rcu_unregister(struct br_tables *br_tables, uint32_t lcore_id);

static inline void br_tables_rcu_online(struct br_tables *br_tables, uint32_t lcore_id)
{
    rte_rcu_qsbr_thread_online(br_tables->port_table.qsv, lcore_id);
    rte_rcu_qsbr_thread_online(br_tables->peer_table.qsv, lcore_id);
    rte_rcu_qsbr_thread_online(br_tables->nid_table.qsv, lcore_id);
    rte_rcu_qsbr_thread_online(br_tables->arp_table.qsv, lcore_id);
    rte_rcu_qsbr_thread_online(br_tables->px_table->qsv, lcore_id);
    rte_rcu_qsbr_thread_online(br_tables->ip_lpm_table->qsv, lcore_id);
    // rte_rcu_qsbr_thread_online(br_tables->ipv6_table.qsv, lcore_id);
}

static inline void br_tables_rcu_offline(struct br_tables *br_tables, uint32_t lcore_id)
{
    rte_rcu_qsbr_thread_offline(br_tables->port_table.qsv, lcore_id);
    rte_rcu_qsbr_thread_offline(br_tables->peer_table.qsv, lcore_id);
    rte_rcu_qsbr_thread_offline(br_tables->nid_table.qsv, lcore_id);
    rte_rcu_qsbr_thread_offline(br_tables->arp_table.qsv, lcore_id);
    rte_rcu_qsbr_thread_offline(br_tables->px_table->qsv, lcore_id);
    rte_rcu_qsbr_thread_offline(br_tables->ip_lpm_table->qsv, lcore_id);
    // rte_rcu_qsbr_thread_offline(br_tables->ipv6_table.qsv, lcore_id);
}

#endif