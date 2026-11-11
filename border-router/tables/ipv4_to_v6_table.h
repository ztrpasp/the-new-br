#ifndef __IPV6_TABLE_H__
#define __IPV6_TABLE_H__

#include <stdint.h>
#include <stdio.h>
#include <rte_hash.h>
#include <rte_rwlock.h>
#include <rte_rcu_qsbr.h>
#include "../common.h"

#define MAX_NUM_IPV6 (1024*1024*16) /* max number of IP address: 16 million entries */

struct ipv6_entry {
    uint32_t ipv4_addr;
    uint8_t ipv6_addr[IPV6_SIZE];
};

/** key: IPv4 address, data: IPv6 address */
struct ipv6_table {
    rte_rwlock_t lock; /* writer's lock, not used on readers */
    struct rte_rcu_qsbr *qsv; /* QS variable */
    int len; // number of entries in this ip table
    struct rte_hash *ht; // hash table lookup struct
};

/**
 * @brief Initialize ip table.
 * \p lock Initialize writer's lock.
 * \p qsv Allocate space for QS variable.
 * \p len Set length to zero.
 * \p ht Create hash table.
 *
 * @param ipv6_table pointer to the ip table
 */
void ipv6_table_init(struct ipv6_table *ipv6_table);

/**
 * Add a IP entry to IP table. This operation is multi-thread safe.
 * Update IPv6 address if IPv4 address is already in the table.
 *
 * @pre \p ip is not null and points to a valid IPv5 address (uint8_t array of size 16).
 *
 * @post \p ipv6_table->ht and \p ipv6_table->len will be modified if the
 * entry is added to NID-IP table successfully.
 *
 * @param ipv6_table pointer to the NID table
 * @param ipv4_addr the IPv4 address.
 * @param ipv6_addr the IPv6 address associated with the the IPv4 address.
 */
void ipv6_add(struct ipv6_table *ipv6_table, uint32_t ipv4_addr, uint8_t *ipv6_addr);

/**
 * Return the IP pair entry. This operation is multi-thread safe.
 * The content of \p *ipv4_addr is unmodified if search fails.
 *
 * @param ipv6_table pointer to the NID table
 * @param ipv4_addr pointer to the IPv4 address.
 * @param ipv6_addr pointer to return the NID entry.
 *
 * @return
 *   - OK if an entry related to `ip` is found.
 *   - FAILED if the search fails.
 */
static inline int ipv6_search(struct ipv6_table *ipv6_table, const uint32_t ipv4_addr,
    uint8_t *ipv6_addr)
{
    unsigned int lcore_id = rte_lcore_id();
    struct ipv6_entry *entry;
    rte_rcu_qsbr_lock(ipv6_table->qsv, lcore_id);
    int32_t ret = rte_hash_lookup_data(ipv6_table->ht, &ipv4_addr, (void **)&entry);
    if (ret < 0) {
        rte_rcu_qsbr_unlock(ipv6_table->qsv, lcore_id);
        rte_rcu_qsbr_quiescent(ipv6_table->qsv, lcore_id);
        return FAILED;
    }
    IPv6_copy(ipv6_addr, entry->ipv6_addr);
    rte_rcu_qsbr_unlock(ipv6_table->qsv, lcore_id);
    rte_rcu_qsbr_quiescent(ipv6_table->qsv, lcore_id);
    return OK;
}

/**
 * @brief Remove a IPv6 entry.
 *
 * @param ipv6_table pointer to the IPv6 table
 * @param ipv4_addr pointer to the IPv4 address to be deleted
 */
void ipv6_remove(struct ipv6_table *ipv6_table, uint32_t ipv4_addr);

/**
 * Return a string representation of the IPv6 table.
 * The string should be freed by the caller.
 *
 * @param ipv6_table pointer to the IPv6 table
 * @return char * a string representation of the IPv6 table
 */
char *ipv6_table_to_string(struct ipv6_table *ipv6_table);

#endif