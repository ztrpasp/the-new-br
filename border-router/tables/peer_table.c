#include <rte_malloc.h>
#include "peer_table.h"
#include "../utils/utils.h"

/**
 * @brief Free the space allocated to a peer entry, including the path list.
 *
 * @param e pointer to the peer entry
 */
static void peer_entry_free(struct peer_entry *e)
{
    // free all paths
    struct path *p = e->path_list, *q;
    while (p != NULL) {
        q = p->next; // save p->next before free p
        rte_free(p);
        p = q;
    }

    BR_LOGF("free peer entry: %d.%d.%d.%d.\n", IPv4TO4DECIMAL(e->ipv4_addr));
    rte_free(e); // free the peer entry
}

/**
 * @brief Clone path list \p p
 *
 * @param p pointer to the path list to be cloned
 * @param parent the \p parent field of the cloned path list
 * @return struct path* returns NULL if unable to allocate memory for the cloned
 *         or \p p is NULL.
 */
static struct path *path_list_clone(struct path *p, struct peer_entry *parent)
{
    struct path *r = NULL; /* The cloned path list. */
    struct path *p_new, *p_new_prev = NULL;
    while (p != NULL) {
        p_new = rte_malloc("path", sizeof(struct path), 0);
        if (p_new == NULL) {
            BR_INFO("Unable to allocate memory for path struct.\n");
            return r;
        }
        if (r == NULL) { // return value
            r = p_new;
        }

        /* Copy path p. */
        p_new->as_id = p->as_id;
        p_new->px = p->px;
        p_new->px_len = p->px_len;
        p_new->parent = parent;
        p_new->next = NULL;

        if (p_new_prev) {
            p_new_prev->next = p_new;
        }
        p_new_prev = p_new;
        p = p->next;
    }
    return r;
}

/**
 * @brief Clone a peer entry, including the path list.
 *
 * @param e pointer to the peer entry to be cloned
 * @return struct peer_entry* returns NULL if unable to allocate memory.
 */
static struct peer_entry *peer_entry_clone(struct peer_entry *e)
{
    struct peer_entry *e_new = NULL;
    e_new = rte_zmalloc("peer_entry", sizeof(struct peer_entry), RTE_CACHE_LINE_SIZE);
    if (e_new == NULL) {
        BR_INFO("Unable to allocate memory for peer table.\n");
        return NULL;
    }
    e_new->ipv4_addr = e->ipv4_addr;
    e_new->is_internal = e->is_internal;
    rte_memcpy(e_new->sk, e->sk, SIZE_BR_SK);

    /* clone path list */
    e_new->path_list = path_list_clone(e->path_list, e_new);
    return e_new;
}

/* free_key_data_func of rte_hash_rcu_config */
static void peer_table_data_free(void *p, void *key_data)
{
    peer_entry_free((struct peer_entry *)key_data);
}

/**
 * @brief Initialize peer table.
 * \p lock Initialize writer's lock.
 * \p qsv Allocate space for QS variable.
 * \p len Set length to zero.
 * \p ht Create hash table.
 *
 * @param peer_table pointer to the peer table
 */
void peer_table_init(struct peer_table *peer_table)
{
    /* Initialize writer's lock */
    rte_rwlock_init(&peer_table->lock);

    /* Allocate space for QS variable */
    size_t sz = rte_rcu_qsbr_get_memsize(RTE_MAX_LCORE);
    struct rte_rcu_qsbr *qsv =
        (struct rte_rcu_qsbr *)rte_zmalloc("rte_rcu_qsbr", sz, RTE_CACHE_LINE_SIZE);
    ASSERT_NOT_NULL(qsv, "Unable to initialize the peer table.\n");
    rte_rcu_qsbr_init(qsv, RTE_MAX_LCORE);
    peer_table->qsv = qsv;

    /* Initialize length of peer table */
    peer_table->len = 0;

    /* Create hash table */
    struct rte_hash_parameters hash_params = {
        .name = "peer_table_hash",
        .entries = MAX_NUM_PEER,
        .reserved = 0,
        .key_len = 4, // the length of IP address is 4 bytes.
        .hash_func = NULL,
        .hash_func_init_val = 0,
        .socket_id = rte_socket_id()
    };
    peer_table->ht = rte_hash_create(&hash_params);
    ASSERT_NOT_NULL(peer_table->ht, "Unable to create the peer hash table.\n");

    struct rte_hash_rcu_config rcu_cfg = {
        .v = qsv,
        .mode = RTE_HASH_QSBR_MODE_DQ,
        .dq_size = 0, /* default */
        .trigger_reclaim_limit = 0, /* ? */
        .max_reclaim_size = 0, /* default */
        .key_data_ptr = NULL,
        .free_key_data_func = peer_table_data_free,
    };
    rte_hash_rcu_qsbr_add(peer_table->ht, &rcu_cfg);
}

/**
 * @brief Insert an entry to peer table. This operation is multi-thread safe.
 * Peer is identified by IP address. If the peer is in peer table, then the IP
 * address, is_internal, and secret key are updated, but the path list remains
 * the same.
 *
 * @param peer_table pointer to the peer table
 * @param ipv4_addr IP address
 * @param is_internal whether the peer is in the same AS as this BR
 * @param sk the secret key of the peer
 */
void peer_add(struct peer_table *peer_table, uint32_t ipv4_addr,
    bool is_internal, const uint8_t *sk)
{
    int32_t ret;
    struct peer_entry *e;
    rte_rwlock_write_lock(&peer_table->lock);

    ret = rte_hash_lookup_data(peer_table->ht, &ipv4_addr, (void **)&e);
    if (ret > 0) { /* existing entry */
        struct peer_entry *e_new = peer_entry_clone(e);
        if (!e_new) {
            BR_INFO("Cannot add to the peer table.\n");
            rte_rwlock_write_unlock(&peer_table->lock);
            return;
        }
        e_new->ipv4_addr = ipv4_addr;
        e_new->is_internal = is_internal;
        rte_memcpy(e_new->sk, sk, SIZE_BR_SK);
        ret = rte_hash_add_key_data(peer_table->ht, &ipv4_addr, e_new);
        RTE_ASSERT(ret > 0);
        /* Wait for quiescent state change */
        rte_rcu_qsbr_synchronize(peer_table->qsv, rte_lcore_id());
        peer_entry_free(e);
    } else { /* no existing entry */
        if (peer_table->len >= MAX_NUM_PEER) {
            BR_INFO("Peer table is full. Cannot add more peer entries.\n");
            rte_rwlock_write_unlock(&peer_table->lock);
            return;
        }
        // cacheline alignment, zero the allocated space
        e = rte_zmalloc("peer_entry", sizeof(struct peer_entry), RTE_CACHE_LINE_SIZE);
        if (!e) {
            BR_INFO("Unable to add allocate memory for peer entry.\n");
            rte_rwlock_write_unlock(&peer_table->lock);
            return;
        }
        e->ipv4_addr = ipv4_addr;
        e->is_internal = is_internal;
        rte_memcpy(e->sk, sk, SIZE_BR_SK);
        e->path_list = NULL;
        ret = rte_hash_add_key_data(peer_table->ht, &ipv4_addr, e);
        if (ret < 0) {
            BR_INFO("Unable to add entry to the peer table.\n");
            rte_free(e);
            rte_rwlock_write_unlock(&peer_table->lock);
            return;
        }
        peer_table->len++;
    }

    rte_rwlock_write_unlock(&peer_table->lock);
}

/**
 * @brief Delete a peer from the peer table.
 * Use internal RCU to manage the deleted entry.
 *
 * @param peer_table pointer to the peer table
 * @param ipv4_addr the IP address of the peer to be deleted
 */
void peer_remove(struct peer_table *peer_table, uint32_t ipv4_addr)
{
    int32_t ret;
    struct peer_entry *e;
    rte_rwlock_write_lock(&peer_table->lock);
    ret = rte_hash_lookup_data(peer_table->ht, &ipv4_addr, (void **)&e);
    if (ret < 0) { // no existing entry or error
        BR_INFO("Unable to find peer %d.%d.%d.%d in peer table.\n",
            IPv4TO4DECIMAL(ipv4_addr));
        rte_rwlock_write_unlock(&peer_table->lock);
        return;
    }
    rte_hash_del_key(peer_table->ht, &ipv4_addr); // delete entry from hash table
    rte_rwlock_write_unlock(&peer_table->lock);
}

/**
 * @brief Returns true if the given path prefix is conflict with another path
 * prefix in the peer table. This operation is \b NOT multi-thread safe.
 *
 * @param peer_table pointer to the peer table
 * @param px path prefix
 * @param px_len path prefix
 * @param conflict_path pointer to return the conflict path
 * @return true if the path prefix is conflict with another path prefix in
 *         \p peer_table else false
 */
static bool is_conflict(struct peer_table *peer_table, uint32_t px,
    uint32_t px_len, struct path **conflict_path)
{
    uint32_t next = 0;
    struct peer_entry *e;
    uint32_t *ipv4_addr;
    while (true) {
        int ret = rte_hash_iterate(peer_table->ht, (const void **)&ipv4_addr,
            (void **)&e, &next);
        if (ret == -ENOENT) {
            break;
        }

        struct path *p = e->path_list;
        while (p != NULL) {
            uint32_t mask = PX_PREFIX_MASK(min(p->px_len, px_len));
            if ((p->px & mask) == (px & mask)) {
                *conflict_path = p;
                return true;
            }
            p = p->next;
        }
    }
    *conflict_path = NULL;
    return false;
}

/**
 * @brief Add a path to peer. This operation is multi-thread safe.
 *
 * @param peer_table pointer to the peer table
 * @param ipv4_addr peer's IP address
 * @param as_id AS ID of this path
 * @param px_len path prefix, valid range is \b [0,32]
 * @param px path prefix, the lower ( 32 - \p px_len ) bits should be zero
 */
void peer_add_path(struct peer_table *peer_table, uint32_t ipv4_addr, uint32_t as_id,
    uint32_t px_len, uint32_t px)
{
    int32_t ret;
    struct peer_entry *e;
    rte_rwlock_write_lock(&peer_table->lock);
    ret = rte_hash_lookup_data(peer_table->ht, &ipv4_addr, (void **)&e);
    if (ret < 0) { // no existing entry or error
        BR_INFO("Unable to find peer %d.%d.%d.%d in peer table.\n",
            IPv4TO4DECIMAL(ipv4_addr));
        rte_rwlock_write_unlock(&peer_table->lock);
        return;
    }
    // If the peer is of type external, there should be one AS ID.
    if (!e->is_internal && e->path_list != NULL && e->path_list->as_id != as_id) {
        BR_INFO("External peer %d.%d.%d.%d should have consistent AS ID, expect %d,"
            " but %d is given.\n", IPv4TO4DECIMAL(ipv4_addr), e->path_list->as_id, as_id);
        rte_rwlock_write_unlock(&peer_table->lock);
        return;
    }

    // see if the path to be inserted is valid
    struct path *conflict_path;
    if (is_conflict(peer_table, px, px_len, &conflict_path)) {
        BR_INFO("Conflict path prefix with peer %d.%d.%d.%d, path: as %d px %08X/%d.\r\n",
            IPv4TO4DECIMAL(conflict_path->parent->ipv4_addr), conflict_path->as_id, conflict_path->px,
            conflict_path->px_len);
        rte_rwlock_write_unlock(&peer_table->lock);
        return;
    }

    struct peer_entry *e_new = peer_entry_clone(e);
    if (e_new == NULL) {
        rte_rwlock_write_unlock(&peer_table->lock);
        return;
    }

    // Allocate space for a path, no special alignment requirement.
    struct path *p = rte_malloc("path", sizeof(struct path), 0);
    if (p == NULL) {
        BR_INFO("Unable to add allocate memory for path struct.\n");
        peer_entry_free(e_new);
        rte_rwlock_write_unlock(&peer_table->lock);
        return;
    }
    p->as_id = as_id;
    p->px_len = px_len;
    p->px = px & PX_PREFIX_MASK(px_len);
    p->parent = e_new;
    p->next = NULL;

    struct path *prev = NULL; // insert p after prev
    for (struct path *q = e_new->path_list; q; prev = q, q = q->next) {
        if (q->as_id > p->as_id) {
            break; // q->as_id is greater than p->as_id, insert before q (after prev)
        }
    }

    if (prev == NULL) { // insert at the beginning of the list
        p->next = e_new->path_list;
        e_new->path_list = p;
    } else { // insert after prev
        p->next = prev->next;
        prev->next = p;
    }

    ret = rte_hash_add_key_data(peer_table->ht, &ipv4_addr, e_new);
    RTE_ASSERT(ret > 0);
    /* Wait for quiescent state change */
    rte_rcu_qsbr_synchronize(peer_table->qsv, rte_lcore_id());
    peer_entry_free(e);

    rte_rwlock_write_unlock(&peer_table->lock);
    BR_LOGF("peer %d.%d.%d.%d added a path: AS %d, path prefix 0x%08X/%d.\n",
        IPv4TO4DECIMAL(ipv4_addr), as_id, p->px, p->px_len);
}

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
    uint32_t px_len, uint32_t px)
{
    int32_t ret;
    struct peer_entry *e;
    rte_rwlock_write_lock(&peer_table->lock);
    ret = rte_hash_lookup_data(peer_table->ht, &ipv4_addr, (void **)&e);
    if (ret < 0) { // no existing entry or error
        BR_INFO("Unable to find peer %d.%d.%d.%d in peer table.\n",
            IPv4TO4DECIMAL(ipv4_addr));
        rte_rwlock_write_unlock(&peer_table->lock);
        return;
    }

    struct peer_entry *e_new = peer_entry_clone(e);
    if (e_new == NULL) {
        rte_rwlock_write_unlock(&peer_table->lock);
        return;
    }

    struct path *p = e_new->path_list, *prev = NULL;
    uint32_t mask = PX_PREFIX_MASK(px_len);
    px = px & mask;
    for (p = e_new->path_list; p; prev = p, p = p->next) {
        if (p->as_id == as_id && p->px_len == px_len && (p->px & mask) == px) {
            // delete p
            if (prev == NULL) { // p is the first element
                e_new->path_list = p->next;
            } else {
                prev->next = p->next;
            }
            rte_free(p);
            break;
        }
    }

    if (p == NULL) {
        BR_INFO("No such path: AS %d, path prefix 0x%08X/%d.\n", as_id, px, px_len);
        peer_entry_free(e_new);
        rte_rwlock_write_unlock(&peer_table->lock);
        return;
    }

    ret = rte_hash_add_key_data(peer_table->ht, &ipv4_addr, e_new);
    RTE_ASSERT(ret > 0);
    /* Wait for quiescent state change */
    rte_rcu_qsbr_synchronize(peer_table->qsv, rte_lcore_id());
    peer_entry_free(e);

    rte_rwlock_write_unlock(&peer_table->lock);
    BR_LOGF("Peer %d.%d.%d.%d deleted a path: AS %d, path prefix 0x%08X/%d.\n",
        IPv4TO4DECIMAL(ipv4_addr), as_id, px, px_len);
}

/**
 * @brief Loop over the path list of peer \p ipv4_addr
 * This operation is multi-thread safe.
 *
 * @param peer_table pointer to the peer table
 * @param ipv4_addr peer's IP address
 * @param path_callback callback function pointer of type \p peer_path_callback_t
 */
void peer_for_each_path(struct peer_table *peer_table, uint32_t ipv4_addr,
    peer_path_callback_t path_callback)
{
    unsigned int lcore_id = rte_lcore_id();
    struct peer_entry *p;
    rte_rcu_qsbr_lock(peer_table->qsv, lcore_id);
    int32_t ret = rte_hash_lookup_data(peer_table->ht, &ipv4_addr, (void **)&p);
    if (ret < 0) {
        rte_rcu_qsbr_unlock(peer_table->qsv, lcore_id);
        rte_rcu_qsbr_quiescent(peer_table->qsv, lcore_id);
        return;
    }
    struct path *path = p->path_list;
    while (path) {
        path_callback(path->px, path->px_len, ipv4_addr);
        path = path->next;
    }
    rte_rcu_qsbr_unlock(peer_table->qsv, lcore_id);
    rte_rcu_qsbr_quiescent(peer_table->qsv, lcore_id);
}

/**
 * @brief Return a string representation of the peer table.
 * The returned string can be used in configuration file.
 * The returned string should be freed by the caller.
 *
 * @param peer_table pointer to the peer table
 * @return char* a string representation of the peer table
 */
char *peer_table_config_string(struct peer_table *peer_table)
{
    uint32_t next = 0;
    StringBuilder *sb = new_sb();
    struct peer_entry *e;
    uint32_t *ipv4_addr;
    char buf[128];
    unsigned int lcore_id = rte_lcore_id();
    rte_rcu_qsbr_lock(peer_table->qsv, lcore_id);
    while (true) {
        int ret = rte_hash_iterate(peer_table->ht, (const void **)&ipv4_addr,
            (void **)&e, &next);
        if (ret == -ENOENT) {
            break;
        }
        char *type = e->is_internal ? "internal" : "external";
        char sk[SIZE_BR_SK * 2 + 1];
        hex_to_str(sk, e->sk, SIZE_BR_SK, 0);
        snprintf(buf, sizeof(buf), "peer add %s %d.%d.%d.%d sk %s\r\n", type,
            IPv4TO4DECIMAL(e->ipv4_addr), sk);
        sb_append(sb, buf);
        struct path *p = e->path_list;
        while (p != NULL) {
            snprintf(buf, sizeof(buf), "peer add-path %d.%d.%d.%d as %d px %08X/%d\r\n",
                IPv4TO4DECIMAL(e->ipv4_addr), p->as_id, p->px, p->px_len);
            sb_append(sb, buf);
            p = p->next;
        }
    }
    rte_rcu_qsbr_unlock(peer_table->qsv, lcore_id);
    rte_rcu_qsbr_quiescent(peer_table->qsv, lcore_id);
    char *s = sb_get(sb);
    free_sb(sb);
    return s;
}
