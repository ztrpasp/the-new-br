#ifndef __NID_TABLE_H__
#define __NID_TABLE_H__

#include <stdint.h>
#include <stdio.h>
#include <rte_hash.h>
#include <rte_rwlock.h>
#include <rte_rcu_qsbr.h>
#include "../common.h"

#define MAX_NUM_NID (1024*1024*16) /* max number of NIDs: 16 million entries */
#define SIZE_HOST_SK 32 /* size of shared secret key in bytes */

struct nid_entry {
    uint8_t nid[SIZE_NID];
    uint32_t ipv4_addr;
    uint8_t sk[SIZE_HOST_SK];
};

/** key: NID, data: IP address, sk */
struct nid_table {
    rte_rwlock_t lock; /* writer's lock, not used on readers */
    struct rte_rcu_qsbr *qsv; /* QS variable */
    int len; // number of entries in this nid table
    struct rte_hash *ht; // hash table lookup struct
};

/**
 * @brief Initialize nid table.
 * \p lock Initialize writer's lock.
 * \p qsv Allocate space for QS variable.
 * \p len Set length to zero.
 * \p ht Create hash table.
 *
 * @param nid_table pointer to the nid table
 */
void nid_table_init(struct nid_table *nid_table);

/**
 * Add a NID entry to NID table. This operation is multi-thread safe.
 * Update IP address and secret key if NID is already in NIP table.
 *
 * @pre \p nid is not null and points to a valid NID (uint8_t array of size 16).
 *
 * @post \p nid_table->ht and \p nid_table->len will be modified if the
 * entry is added to NID-IP table successfully.
 *
 * @param nid_table pointer to the NID table
 * @param nid pointer to NID.
 * @param ipv4_addr the IPv4 address associated with the NID.
 * @param sk pointer to an uint8_t array of size \b SIZE_NID_SK
 */
void nid_add(struct nid_table *nid_table, const uint8_t *nid, uint32_t ipv4_addr, const uint8_t *sk);

/**
 * Return the NID entry. This operation is multi-thread safe.
 * The content of \p *ipv4_addr is unmodified if search fails.
 *
 * @param nid_table pointer to the NID table
 * @param nid pointer to the NID
 * @param ipv4_addr pointer to return the NID entry.
 * @param sk pointer to space to return the sk of the host
 *
 * @return
 *   - OK if an entry related to `nid` is found.
 *   - FAILED if the search fails.
 */
static inline int nid_search(struct nid_table *nid_table, const uint8_t *nid,
    uint32_t *ipv4_addr, uint8_t *sk)
{
    unsigned int lcore_id = rte_lcore_id();
    struct nid_entry *entry;
    rte_rcu_qsbr_lock(nid_table->qsv, lcore_id);
    int32_t ret = rte_hash_lookup_data(nid_table->ht, nid, (void **)&entry);
    if (ret < 0) {
        rte_rcu_qsbr_unlock(nid_table->qsv, lcore_id);
        rte_rcu_qsbr_quiescent(nid_table->qsv, lcore_id);
        return FAILED;
    }
    *ipv4_addr = entry->ipv4_addr;
    rte_memcpy(sk, entry->sk, SIZE_HOST_SK);
    rte_rcu_qsbr_unlock(nid_table->qsv, lcore_id);
    rte_rcu_qsbr_quiescent(nid_table->qsv, lcore_id);
    return OK;
}

/**
 * @brief Remove a NID entry.
 *
 * @param nid_table pointer to the NID table
 * @param nid pointer to the NID to be deleted
 */
void nid_remove(struct nid_table *nid_table, const uint8_t *nid);

/**
 * Return a string representation of the NID table.
 * The string should be freed by the caller.
 *
 * @param nid_table pointer to the NID table
 * @return char * a string representation of the NID table
 */
char *nid_table_to_string(struct nid_table *nid_table);

#endif