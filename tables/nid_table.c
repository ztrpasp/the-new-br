#include <rte_malloc.h>
#include <arpa/inet.h>
#include "nid_table.h"
#include "../utils/utils.h"
#include "../log.h"

/**
 * @brief Free the space allocated to a nid entry.
 *
 * @param e pointer to the nid entry
 */
static void nid_entry_free(struct nid_entry *e)
{
    BR_LOG("Free NID entry: %d.%d.%d.%d.\n", IPv4TO4DECIMAL(e->ipv4_addr));
    rte_free(e);
}

/**
 * @brief Clone a nid entry.
 *
 * @param e pointer to the nid entry to be cloned
 * @return struct nid_entry* returns NULL if unable to allocate memory.
 */
static struct nid_entry * nid_entry_clone(struct nid_entry *e)
{
    struct nid_entry *e_new = NULL;
    e_new = rte_zmalloc("nid_entry", sizeof(struct nid_entry), RTE_CACHE_LINE_SIZE);
    if (e_new == NULL) {
        BR_INFO("Unable to allocate memory for nid entry.\n");
        return NULL;
    }
    e_new->ipv4_addr = e->ipv4_addr;
    rte_memcpy(e_new->nid, e->nid, SIZE_NID);
    rte_memcpy(e_new->sk, e->sk, SIZE_HOST_SK);
    return e_new;
}

static void nid_table_data_free(void *p, void *key_data)
{
    nid_entry_free(key_data);
}

/**
 * @brief Initialize nid table.
 * \p lock Initialize writer's lock.
 * \p qsv Allocate space for QS variable.
 * \p len Set length to zero.
 * \p ht Create hash table.
 *
 * @param nid_table pointer to the nid table
 */
void nid_table_init(struct nid_table *nid_table)
{
    /* Initialize writer's lock */
    rte_rwlock_init(&nid_table->lock);

    /* Allocate space for QS variable */
    size_t sz = rte_rcu_qsbr_get_memsize(RTE_MAX_LCORE);
    struct rte_rcu_qsbr *qsv =
        (struct rte_rcu_qsbr *)rte_zmalloc("rte_rcu_qsbr", sz, RTE_CACHE_LINE_SIZE);
    ASSERT_NOT_NULL(qsv, "Unable to initialize the peer table.\n");
    rte_rcu_qsbr_init(qsv, RTE_MAX_LCORE);
    nid_table->qsv = qsv;

    /* Initialize length of peer table */
    nid_table->len = 0;

    /* Create hash table */
    struct rte_hash_parameters hash_params = {
        .name = "nid_table_hash",
        .entries = MAX_NUM_NID,
        .reserved = 0,
        .key_len = SIZE_NID, // the length of NID is 16 bytes.
        .hash_func = NULL,
        .hash_func_init_val = 0,
        .socket_id = rte_socket_id()
    };
    nid_table->ht = rte_hash_create(&hash_params);
    ASSERT_NOT_NULL(nid_table->ht, "Unable to create the NID hash table.\n");

    struct rte_hash_rcu_config rcu_cfg = {
        .v = qsv,
        .mode = RTE_HASH_QSBR_MODE_DQ,
        .dq_size = 0, /* default */
        .trigger_reclaim_limit = 0, /* ? */
        .max_reclaim_size = 0, /* default */
        .key_data_ptr = NULL,
        .free_key_data_func = nid_table_data_free,
    };
    rte_hash_rcu_qsbr_add(nid_table->ht, &rcu_cfg);
}

/**
 * @brief Add a NID entry to NID table. This operation is multi-thread safe.
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
void nid_add(struct nid_table *nid_table, const uint8_t *nid,
    uint32_t ipv4_addr, const uint8_t *sk)
{
    int32_t ret;
    struct nid_entry *e;
    rte_rwlock_write_lock(&nid_table->lock);
    ret = rte_hash_lookup_data(nid_table->ht, nid, (void **)&e);
    if (ret > 0) { /* existing entry */
        struct nid_entry *e_new = nid_entry_clone(e);
        if (!e_new) {
            BR_INFO("Cannot add to the peer table.\n");
            rte_rwlock_write_unlock(&nid_table->lock);
            return;
        }
        e_new->ipv4_addr = ipv4_addr;
        rte_memcpy(e_new->nid, nid, SIZE_NID);
        rte_memcpy(e_new->sk, sk, SIZE_HOST_SK);
        ret = rte_hash_add_key_data(nid_table->ht, nid, e_new);
        RTE_ASSERT(ret > 0);
        /* Wait for quiescent state change */
        rte_rcu_qsbr_synchronize(nid_table->qsv, rte_lcore_id());
        nid_entry_free(e);
    } else { /* no existing entry */
        if (nid_table->len >= MAX_NUM_NID) {
            BR_INFO("NID table is full. Cannot add more NID entries.\n");
            rte_rwlock_write_unlock(&nid_table->lock);
            return;
        }
        // cacheline alignment
        e = rte_malloc("nid_entry", sizeof(struct nid_entry), RTE_CACHE_LINE_SIZE);
        if (e == NULL) {
            BR_INFO("Unable to add allocate memory for NID table.\n");
            rte_rwlock_write_unlock(&nid_table->lock);
            return;
        }
        e->ipv4_addr = ipv4_addr;
        rte_memcpy(e->nid, nid, SIZE_NID);
        rte_memcpy(e->sk, sk, SIZE_HOST_SK);
        ret = rte_hash_add_key_data(nid_table->ht, nid, e);
        if (ret < 0) {
            BR_INFO("Unable to add entry to the NID table.\n");
            rte_free(e);
            rte_rwlock_write_unlock(&nid_table->lock);
            return;
        }
        nid_table->len++;
    }

    rte_rwlock_write_unlock(&nid_table->lock);
}

/**
 * @brief Remove a NID entry.
 * Use internal RCU to manage the deleted entry.
 *
 * @param nid_table pointer to the NID table
 * @param nid pointer to the NID to be deleted
 */
void nid_remove(struct nid_table *nid_table, const uint8_t *nid)
{
    rte_rwlock_write_lock(&nid_table->lock);
    int ret = rte_hash_del_key(nid_table->ht, nid);
    if (ret < 0) { // no existing entry or error
        BR_INFO("Unable to remove the NID in nid table.\n");
        rte_rwlock_write_unlock(&nid_table->lock);
        return;
    }
    nid_table->len--;
    rte_rwlock_write_unlock(&nid_table->lock);
}

/**
 * Return a string representation of the NID table.
 * The string should be freed by the caller.
 *
 * @param nid_table pointer to the NID table
 * @return char * a string representation of the NID table
 */
char *nid_table_to_string(struct nid_table *nid_table)
{
    uint32_t next = 0;
    int32_t ret;
    uint8_t *nid;
    struct nid_entry *entry;
    char *s = NULL;
    char buf[128] = {0};
    StringBuilder *sb = new_sb();
    int cnt = 0;
    sprintf(buf, "%-36s%-16sSK\r\n", "Node Identifier", "IP address");
    sb_append(sb, buf);
    unsigned int lcore_id = rte_lcore_id();
    rte_rcu_qsbr_lock(nid_table->qsv, lcore_id);
    while (true) {
        ret = rte_hash_iterate(nid_table->ht, (const void **)&nid,
            (void **)&entry, &next);
        if (ret == -ENOENT) {
            break;
        }
        char nid[SIZE_NID * 2 + 1];
        char ipv4_addr[16];
        char sk[SIZE_HOST_SK * 2 + 1];
        hex_to_str(nid, entry->nid, SIZE_NID, 0);
        uint32_t ip = htonl(entry->ipv4_addr);
        inet_ntop(AF_INET, &ip, ipv4_addr, sizeof(ipv4_addr));
        hex_to_str(sk, entry->sk, SIZE_HOST_SK, 0);
        sprintf(buf, "%-36s%-16s%s\r\n", nid, ipv4_addr, sk);
        sb_append(sb, buf);
        cnt++;
    }
    rte_rcu_qsbr_unlock(nid_table->qsv, lcore_id);
    rte_rcu_qsbr_quiescent(nid_table->qsv, lcore_id);
    sprintf(buf, "Number of NID entries: %d\r\n", cnt);
    sb_append(sb, buf);
    s = sb_get(sb);
    free_sb(sb);
    return s;
}
