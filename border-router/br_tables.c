#include "br_tables.h"

struct br_tables g_br_tables;

void br_tables_rcu_register(struct br_tables *br_tables, uint32_t lcore_id)
{
    rte_rcu_qsbr_thread_register(br_tables->port_table.qsv, lcore_id);
    rte_rcu_qsbr_thread_register(br_tables->peer_table.qsv, lcore_id);
    rte_rcu_qsbr_thread_register(br_tables->nid_table.qsv, lcore_id);
    rte_rcu_qsbr_thread_register(br_tables->arp_table.qsv, lcore_id);
    rte_rcu_qsbr_thread_register(br_tables->ip_lpm_table->qsv, lcore_id);
    rte_rcu_qsbr_thread_register(br_tables->px_table->qsv, lcore_id);
    // rte_rcu_qsbr_thread_register(br_tables->ipv6_table.qsv, lcore_id);
}

void br_tables_rcu_unregister(struct br_tables *br_tables, uint32_t lcore_id)
{
    rte_rcu_qsbr_thread_unregister(br_tables->port_table.qsv, lcore_id);
    rte_rcu_qsbr_thread_unregister(br_tables->peer_table.qsv, lcore_id);
    rte_rcu_qsbr_thread_unregister(br_tables->nid_table.qsv, lcore_id);
    rte_rcu_qsbr_thread_unregister(br_tables->arp_table.qsv, lcore_id);
    rte_rcu_qsbr_thread_unregister(br_tables->ip_lpm_table->qsv, lcore_id);
    rte_rcu_qsbr_thread_unregister(br_tables->px_table->qsv, lcore_id);
    // rte_rcu_qsbr_thread_unregister(br_tables->ipv6_table.qsv, lcore_id);
}
