#ifndef __MAC_TABLE_H__
#define __MAC_TABLE_H__

#include <stdint.h>
#include <rte_timer.h>
#include <rte_ether.h>
#include <rte_hash.h>
#include <rte_rwlock.h>
#include <rte_rcu_qsbr.h>
#include "../common.h"

#define MAX_NUM_MAC_ENTRY (1024 * 4)
#define MAC_EXPIRE_SECONDS (60 * 20)

struct mac_table;

struct mac_entry {
    uint8_t ip[IPV6_SIZE]; /* key */
    struct rte_ether_addr hw_addr; /* value */
    struct rte_timer timer; /* the timer associated with this MAC entry */
    struct mac_table *owner; /* for deleting the expired MAC entry */
};

struct mac_table {
    rte_rwlock_t lock; /* writer's lock, not used on readers */
    struct rte_rcu_qsbr *qsv; /* QS variable */
    uint32_t len; /** number of entries in this mac table */
    struct rte_hash *ht;  /** hash table lookup struct */
};

/**
 * @brief Initialize mac table.
 * \p lock Initialize writer's lock.
 * \p qsv Allocate space for QS variable.
 * \p len Set length to zero.
 * \p ht Create hash table.
 *
 * @param mac_table pointer to the mac table
 */
void mac_table_init(struct mac_table *mac_table);

/**
 * @brief Add an entry to MAC table. This operation is multi-thread safe.
 *
 * @param mac_table pointer to the MAC table
 * @param ip_addr IP address tobe added
 * @param hw_addr hardware address to be added
 * @return int returns OK if the entry is added to the MAC table successfully
 * 		   else FAILED.
 */
int mac_add(struct mac_table *mac_table, uint8_t *ipv6_addr,
    const struct rte_ether_addr *hw_addr);


/**
 * @brief Return a string representation of the MAC table.
 * The returned string should be freed by the caller.
 *
 * @param mac_table pointer to the MAC table
 * @return char* a string representation of the MAC table
 */
char *mac_table_to_string(struct mac_table *mac_table);

/**
 * @brief Search the MAC table and returns the MAC address.
 * This operation is multi-thread safe.
 *
 * @param mac_table pointer to the MAC table
 * @param ipv6_addr the IP address to search
 * @param hw_addr pointer to space that hold hardware address. If search succeeds,
 * 				  the hardware address is copied to \p hw_addr
 * @return int returns OK if a match for \p ipv4_addr is found else FAILED.
 */
static inline int mac_search(struct mac_table *mac_table, uint8_t *ipv6_addr,
    struct rte_ether_addr *hw_addr)
{
    struct mac_entry *e;
    unsigned int lcore_id = rte_lcore_id();
    rte_rcu_qsbr_lock(mac_table->qsv, lcore_id);
    int32_t ret = rte_hash_lookup_data(mac_table->ht, ipv6_addr, (void **)&e);
    if (ret < 0) {
        rte_rcu_qsbr_unlock(mac_table->qsv, lcore_id);
        rte_rcu_qsbr_quiescent(mac_table->qsv, lcore_id);
        return FAILED;
    }
    rte_ether_addr_copy(&e->hw_addr, hw_addr);
    rte_rcu_qsbr_unlock(mac_table->qsv, lcore_id);
    rte_rcu_qsbr_quiescent(mac_table->qsv, lcore_id);
    return OK;
}

#endif