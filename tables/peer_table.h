#ifndef __PEER_TABLE_H__
#define __PEER_TABLE_H__

#include <stdint.h>
#include <stdbool.h>
#include <rte_rwlock.h>
#include <rte_rcu_qsbr.h>
#include <rte_hash.h>
#include "../common.h"

#define MAX_NUM_PEER 128
#define SIZE_BR_SK 32 // size of secret key of border router

struct path {
    uint32_t as_id; /** the AS ID to which this path leads if the type of the path
                        is internal else the ID of the AS to which the border router
                        belongs. */
    uint32_t px_len; /** prefix length, valid range is \b [0,32] */
    uint32_t px; /** path prefix, the lower ( 32 - \p px_len ) bits are not used. */
    struct peer_entry *parent;
    struct path *next; /** pointer to the next path in the list. */
};

/** Key: ipv4_address */
struct peer_entry {
    uint32_t ipv4_addr; /** the IP address which uniquely identify a peer */
    bool is_internal; /** type of the peer, peer is in the same domain if true */
    uint8_t sk[SIZE_BR_SK]; /** secret key of the peer */
    struct path *path_list; /** path prefix list */
};

// peer database, the peer_entry is dynamically allocated.
struct peer_table {
    rte_rwlock_t lock; /* writer's lock, not used on readers */
    struct rte_rcu_qsbr *qsv; /* QS variable */
    int len; /** number of entries in this peer table */
    struct rte_hash *ht; /** hash table lookup struct */
};

/**
 * @brief Initialize peer table.
 * \p lock Initialize writer's lock.
 * \p qsv Allocate space for QS variable.
 * \p len Set length to zero.
 * \p ht Create hash table.
 *
 * @param peer_table pointer to the peer table
 */
void peer_table_init(struct peer_table *peer_table);

/**
 * @brief Insert an entry to peer table. This operation is multi-thread safe.
 * Peer is identified by IP address.
 *
 * @param peer_table pointer to the peer table
 * @param ipv4_addr IP address
 * @param is_internal whether the peer is in the same AS as this BR
 * @param sk the secret key of the peer
 */
void peer_add(struct peer_table *peer_table, uint32_t ipv4_addr,
    bool is_internal, const uint8_t *sk);

/**
 * @brief Delete a peer from the peer table.
 *
 * @param peer_table pointer to the peer table
 * @param ipv4_addr the IP address of the peer to be deleted
 */
void peer_remove(struct peer_table *peer_table, uint32_t ipv4_addr);

/**
 * @brief Add a path to peer.
 *
 * @param peer_table pointer to the peer table
 * @param ipv4_addr peer's IP address
 * @param as_id AS ID of this path
 * @param px_len prefix length, valid range is \b [0,32]
 * @param px path prefix, the lower ( 32 - \p px_len ) bits should be zero
 */
void peer_add_path(struct peer_table *peer_table, uint32_t ipv4_addr, uint32_t as_id,
    uint32_t px_len, uint32_t px);

/**
 * @brief Delete a path from a peer. This operation is multi-thread safe.
 *
 * @param peer_table pointer to the peer table
 * @param ipv4_addr IP address of the peer
 * @param as_id AS ID of the path to be deleted
 * @param px_len prefix length of the path to be deleted
 * @param px path prefix of the path to be deleted
 */
void peer_remove_path(struct peer_table *peer_table, uint32_t ipv4_addr, uint32_t as_id,
    uint32_t px_len, uint32_t px);

/**
 * @brief path prefix, prefix length, ipv4_addr
 */
typedef void (*peer_path_callback_t)(uint32_t, uint32_t, uint32_t);

/**
 * @brief Loop over the path list of peer \p ipv4_addr
 * This operation is multi-thread safe.
 *
 * @param peer_table pointer to the peer table
 * @param ipv4_addr peer's IP address
 * @param path_callback callback function pointer of type \p peer_path_callback_t
 */
void peer_for_each_path(struct peer_table *peer_table, uint32_t ipv4_addr,
    peer_path_callback_t path_callback);

/**
 * @brief Return a string representation of the peer table.
 * The returned string can be used in configuration file.
 * The returned string should be freed by the caller.
 *
 * @param peer_table pointer to the peer table
 * @return char* a string representation of the peer table
 */
char *peer_table_config_string(struct peer_table *peer_table);

/**
 * @brief Search peer by IP address.
 * This operation is multi-thread safe.
 *
 * @param peer_table pointer to the peer table
 * @param ipv4_addr IP address of the peer to be searched
 * @param p pointer to return the peer entry
 * @return int
 */

/**
 * @brief Search peer by IP address.
 * This operation is multi-thread safe.
 *
 * @param peer_table pointer to the peer table
 * @param ipv4_addr IP address of the peer to be searched
 * @param is_internal pointer to space for storing the returned result
 * @param sk pointer to space for storing the returned security key
 * @return int \p FAILED if the peer cannot be found else \p OK
 */
static inline int peer_search(struct peer_table *peer_table, uint32_t ipv4_addr,
    bool *is_internal, uint8_t *sk)
{
    unsigned int lcore_id = rte_lcore_id();
    struct peer_entry *p;
    rte_rcu_qsbr_lock(peer_table->qsv, lcore_id);
    int32_t ret = rte_hash_lookup_data(peer_table->ht, &ipv4_addr, (void **)&p);
    if (ret < 0) {
        rte_rcu_qsbr_unlock(peer_table->qsv, lcore_id);
        rte_rcu_qsbr_quiescent(peer_table->qsv, lcore_id);
        return FAILED;
    }
    if (is_internal) {
        *is_internal = p->is_internal;
    }
    if (sk) {
        rte_memcpy(sk, p->sk, SIZE_BR_SK);
    }
    rte_rcu_qsbr_unlock(peer_table->qsv, lcore_id);
    rte_rcu_qsbr_quiescent(peer_table->qsv, lcore_id);
    return OK;
}

#endif