#ifndef __PX_TABLE_H__
#define __PX_TABLE_H__

#include <stdint.h>
#include <stdbool.h>
#include <rte_rwlock.h>
#include <rte_lpm.h>
#include <rte_rcu_qsbr.h>
#include "../common.h"

/* PX table */
#define MAX_NUM_PX (1 << 10)
#define NUM_TBL8S_PX 256

struct px_entry {
    uint32_t px; /* path prefix */
    uint32_t prefix_len; /* length of path prefix */
    uint32_t as_id; /* ID of the AS to which the path connects */
    uint32_t ipv4_addr; /* IP address of the next-hop/last-hop BR */
    bool is_valid; /* whether this entry is in use */
};

struct px_table {
    rte_rwlock_t lock; /* writer's lock */
    struct rte_lpm *lpm;
    struct rte_rcu_qsbr *qsv; /* QS variable */
    struct px_entry entries[0] __rte_cache_aligned; /* num: MAX_NUM_PX */
};

/**
 * @brief Allocate space for PX table and initialize it.
 * \p lock Initialize writer's lock.
 * \p qsv Allocate space for QS variable.
 * This function may cause program exit if there's not enough memory to allocate.
 * Thus, \p px_table_create should only be called during program initialization.
 *
 * @return struct px_table* pointer to the created PX table if
 *         nothing goes wrong else the program will exit.
 */
struct px_table *px_table_create(void);

/**
 * @brief Insert an entry (identified by \p px and \p prefix_len ) to PX table.
 * This operation is multi-thread safe.
 *
 * @param px_table pointer to the PX table
 * @param px path prefix to add
 * @param prefix_len prefix length, valid range is [1, 32]
 * @param as_id ID of the AS to which the path connects
 * @param ipv4_addr IP address of the peer BR
 * @return int OK if the entry is inserted to PX table successfully
 *             else FAILED
 */
int px_add(struct px_table *px_table, uint32_t px, uint32_t prefix_len,
    uint32_t as_id, uint32_t ipv4_addr);

/**
 * @brief Delete a PX entry identified by \p px and \p prefix_len in PX
 * table. This operation is multi-thread safe. Parameter \p next_hop is not
 * checked in this function, so use a value of zero is OK.
 *
 * @param px_table pointer to the PX table
 * @param px path prefix to delete
 * @param prefix_len prefix length, valid range is [1, 32]
 * @param ipv4_addr IP address of the ipv4_addr BR
 * @return int OK if the PX entry is removed successfully else FAILED
 */
int px_remove(struct px_table *px_table, uint32_t px, uint32_t prefix_len,
    uint32_t ipv4_addr);

/**
 * @brief Search IP address by PX/PID
 * This operation is multi-thread safe.
 *
 * @param px_table pointer to the PX table
 * @param px can be path prefix or PID
 * @param ipv4_addr pointer for returning IP address of the peer BR
 * @return int \p OK is a matching path prefix is found else \p FAILED
 */
static inline int px_search_ipv4_addr(struct px_table *px_table, uint32_t px,
    uint32_t *ipv4_addr)
{
    int entry_idx;
    unsigned int lcore_id = rte_lcore_id();
    rte_rcu_qsbr_lock(px_table->qsv, lcore_id);
    int ret = rte_lpm_lookup(px_table->lpm, px, &entry_idx);
    struct px_entry *px_entry = &px_table->entries[entry_idx];
    if (ret != 0) {
        rte_rcu_qsbr_unlock(px_table->qsv, lcore_id);
        rte_rcu_qsbr_quiescent(px_table->qsv, lcore_id);
        return FAILED;
    }
    *ipv4_addr = px_entry->ipv4_addr;
    rte_rcu_qsbr_unlock(px_table->qsv, lcore_id);
    rte_rcu_qsbr_quiescent(px_table->qsv, lcore_id);
    return OK;
}

/**
 * @brief Search AS by PX/PID
 * This operation is multi-thread safe.
 *
 * @param px_table pointer to the PX table
 * @param px can be path prefix or PID
 * @param as_id pointer for returning AS ID
 * @return int \p OK is a matching path prefix is found else \p FAILED
 */
static inline int px_search_as_id(struct px_table *px_table, uint32_t px,
    uint32_t *as_id)
{
    int entry_idx;
    unsigned int lcore_id = rte_lcore_id();
    rte_rcu_qsbr_lock(px_table->qsv, lcore_id);
    int ret = rte_lpm_lookup(px_table->lpm, px, &entry_idx);
    struct px_entry *px_entry = &px_table->entries[entry_idx];
    if (ret != 0) {
        rte_rcu_qsbr_unlock(px_table->qsv, lcore_id);
        rte_rcu_qsbr_quiescent(px_table->qsv, lcore_id);
        return FAILED;
    }
    *as_id = px_entry->as_id;
    rte_rcu_qsbr_unlock(px_table->qsv, lcore_id);
    rte_rcu_qsbr_quiescent(px_table->qsv, lcore_id);
    return OK;
}

/**
 * @brief Return a string representation of the PX table.
 * The returned string should be freed by the caller.
 *
 * @param px_table pointer to the PX table
 * @return char* returns a string representation of the PX table
 */
char *px_table_to_string(struct px_table *px_table);

#endif