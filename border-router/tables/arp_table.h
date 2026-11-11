#ifndef __ARP_TABLE_H__
#define __ARP_TABLE_H__

#include <stdint.h>
#include <rte_timer.h>
#include <rte_ether.h>
#include <rte_hash.h>
#include <rte_rwlock.h>
#include <rte_rcu_qsbr.h>
#include "../common.h"

#define MAX_NUM_ARP_ENTRY (1024 * 4)
#define ARP_EXPIRE_SECONDS (60 * 20)

struct arp_table;

struct arp_entry {
    uint32_t ipv4_addr; /* key */
    struct rte_ether_addr hw_addr; /* value */
    struct rte_timer timer; /* the timer associated with this ARP entry */
    struct arp_table *owner; /* for deleting the expired ARP entry */
};

struct arp_table {
    rte_rwlock_t lock; /* writer's lock, not used on readers */
    struct rte_rcu_qsbr *qsv; /* QS variable */
    uint32_t len; /** number of entries in this arp table */
    struct rte_hash *ht;  /** hash table lookup struct */
};

/**
 * @brief Initialize arp table.
 * \p lock Initialize writer's lock.
 * \p qsv Allocate space for QS variable.
 * \p len Set length to zero.
 * \p ht Create hash table.
 *
 * @param arp_table pointer to the arp table
 */
void arp_table_init(struct arp_table *arp_table);

/**
 * @brief Add an entry to ARP table. This operation is multi-thread safe.
 *
 * @param arp_table pointer to the ARP table
 * @param ipv4_addr IP address tobe added
 * @param hw_addr hardware address to be added
 * @return int returns OK if the entry is added to the ARP table successfully
 * 		   else FAILED.
 */
int arp_add(struct arp_table *arp_table, uint32_t ipv4_addr,
    const struct rte_ether_addr *hw_addr);


/**
 * @brief Return a string representation of the ARP table.
 * The returned string should be freed by the caller.
 *
 * @param arp_table pointer to the ARP table
 * @return char* a string representation of the ARP table
 */
char *arp_table_to_string(struct arp_table *arp_table);

/**
 * @brief Search the ARP table and returns the MAC address.
 * This operation is multi-thread safe.
 *
 * @param arp_table pointer to the ARP table
 * @param ipv4_addr the IP address to search
 * @param hw_addr pointer to space that hold hardware address. If search succeeds,
 * 				  the hardware address is copied to \p hw_addr
 * @return int returns OK if a match for \p ipv4_addr is found else FAILED.
 */
static inline int arp_search(struct arp_table *arp_table, uint32_t ipv4_addr,
    struct rte_ether_addr *hw_addr)
{
    struct arp_entry *e;
    unsigned int lcore_id = rte_lcore_id();
    rte_rcu_qsbr_lock(arp_table->qsv, lcore_id);
    int32_t ret = rte_hash_lookup_data(arp_table->ht, &ipv4_addr, (void **)&e);
    if (ret < 0) {
        rte_rcu_qsbr_unlock(arp_table->qsv, lcore_id);
        rte_rcu_qsbr_quiescent(arp_table->qsv, lcore_id);
        return FAILED;
    }
    rte_ether_addr_copy(&e->hw_addr, hw_addr);
    rte_rcu_qsbr_unlock(arp_table->qsv, lcore_id);
    rte_rcu_qsbr_quiescent(arp_table->qsv, lcore_id);
    return OK;
}

#endif