#include <rte_malloc.h>
#include <arpa/inet.h>
#include "ipv4_to_v6_table.h"
#include "../utils/utils.h"
#include "../log.h"

/**
 * @brief Free the space allocated to a ipv6 entry.
 *
 * @param e pointer to the ipv6 entry
 */
static void ipv6_entry_free(struct ipv6_entry *e)
{
    BR_LOG("Free IPv6 entry: %d.%d.%d.%d.\n", IPv4TO4DECIMAL(e->ipv4_addr));
    rte_free(e);
}

/**
 * @brief Clone a ipv6 entry.
 *
 * @param e pointer to the ipv6 entry to be cloned
 * @return struct ipv6_entry* returns NULL if unable to allocate memory.
 */
static struct ipv6_entry * ipv6_entry_clone(struct ipv6_entry *e)
{
    struct ipv6_entry *e_new = NULL;
    e_new = rte_zmalloc("ipv6_entry", sizeof(struct ipv6_entry), RTE_CACHE_LINE_SIZE);
    if (e_new == NULL) {
        BR_INFO("Unable to allocate memory for ipv6 entry.\n");
        return NULL;
    }
    e_new->ipv4_addr = e->ipv4_addr;
    IPv6_copy(e_new->ipv6_addr, e->ipv6_addr);
    return e_new;
}

static void ipv6_table_data_free(void *p, void *key_data)
{
    ipv6_entry_free(key_data);
}

/**
 * @brief Initialize ipv6 table.
 * \p lock Initialize writer's lock.
 * \p qsv Allocate space for QS variable.
 * \p len Set length to zero.
 * \p ht Create hash table.
 *
 * @param ipv6_table pointer to the ipv6 table
 */
void ipv6_table_init(struct ipv6_table *ipv6_table)
{
    /* Initialize writer's lock */
    rte_rwlock_init(&ipv6_table->lock);

    /* Allocate space for QS variable */
    size_t sz = rte_rcu_qsbr_get_memsize(RTE_MAX_LCORE);
    struct rte_rcu_qsbr *qsv =
        (struct rte_rcu_qsbr *)rte_zmalloc("rte_rcu_qsbr", sz, RTE_CACHE_LINE_SIZE);
    ASSERT_NOT_NULL(qsv, "Unable to initialize the peer table.\n");
    rte_rcu_qsbr_init(qsv, RTE_MAX_LCORE);
    ipv6_table->qsv = qsv;

    /* Initialize length of peer table */
    ipv6_table->len = 0;

    /* Create hash table */
    struct rte_hash_parameters hash_params = {
        .name = "ipv6_table_hash",
        .entries = MAX_NUM_IPV6,
        .reserved = 0,
        .key_len = SIZE_NID, // the length of NID is 16 bytes.
        .hash_func = NULL,
        .hash_func_init_val = 0,
        .socket_id = rte_socket_id()
    };
    ipv6_table->ht = rte_hash_create(&hash_params);
    ASSERT_NOT_NULL(ipv6_table->ht, "Unable to create the NID hash table.\n");

    struct rte_hash_rcu_config rcu_cfg = {
        .v = qsv,
        .mode = RTE_HASH_QSBR_MODE_DQ,
        .dq_size = 0, /* default */
        .trigger_reclaim_limit = 0, /* ? */
        .max_reclaim_size = 0, /* default */
        .key_data_ptr = NULL,
        .free_key_data_func = ipv6_table_data_free,
    };
    rte_hash_rcu_qsbr_add(ipv6_table->ht, &rcu_cfg);
}

/**
 * @brief Add a NID entry to NID table. This operation is multi-thread safe.
 * Update IP address and secret key if NID is already in NIP table.
 *
 * @pre \p ipv4_addr is not null and points to a valid IPv6 (uint8_t array of size 16).
 *
 * @post \p ipv6_table->ht and \p ipv6_table->len will be modified if the
 * entry is added to NID-IP table successfully.
 *
 * @param ipv6_table pointer to the NID table
 * @param ipv4_addr pointer to NID.
 * @param ipv4_addr the IPv4 address associated with the NID.
 * @param sk pointer to an uint8_t array of size \b SIZE_NID_SK
 */
void ipv6_add(struct ipv6_table *ipv6_table, uint32_t ipv4_addr,
    uint8_t *ipv6_addr)
{
    int32_t ret;
    struct ipv6_entry *e;
    rte_rwlock_write_lock(&ipv6_table->lock);
    ret = rte_hash_lookup_data(ipv6_table->ht, &ipv4_addr, (void **)&e);
    if (ret > 0) { /* existing entry */
        struct ipv6_entry *e_new = ipv6_entry_clone(e);
        if (!e_new) {
            BR_INFO("Cannot add to the peer table.\n");
            rte_rwlock_write_unlock(&ipv6_table->lock);
            return;
        }
        e_new->ipv4_addr = ipv4_addr;
        IPv6_copy(e_new->ipv6_addr, e->ipv6_addr);
        ret = rte_hash_add_key_data(ipv6_table->ht, &ipv4_addr, e_new);
        RTE_ASSERT(ret > 0);
        /* Wait for quiescent state change */
        rte_rcu_qsbr_synchronize(ipv6_table->qsv, rte_lcore_id());
        ipv6_entry_free(e);
    } else { /* no existing entry */
        if (ipv6_table->len >= MAX_NUM_IPV6) {
            BR_INFO("IPv6 table is full. Cannot add more IPv6 entries.\n");
            rte_rwlock_write_unlock(&ipv6_table->lock);
            return;
        }
        // cacheline alignment
        e = rte_malloc("ipv6_entry", sizeof(struct ipv6_entry), RTE_CACHE_LINE_SIZE);
        if (e == NULL) {
            BR_INFO("Unable to add allocate memory for IPv6 table.\n");
            rte_rwlock_write_unlock(&ipv6_table->lock);
            return;
        }
        e->ipv4_addr = ipv4_addr;
        IPv6_copy(e->ipv6_addr, ipv6_addr);
        ret = rte_hash_add_key_data(ipv6_table->ht, &ipv4_addr, e);
        if (ret < 0) {
            BR_INFO("Unable to add entry to the IPv6 table.\n");
            rte_free(e);
            rte_rwlock_write_unlock(&ipv6_table->lock);
            return;
        }
        ipv6_table->len++;
    }

    rte_rwlock_write_unlock(&ipv6_table->lock);
}

/**
 * @brief Remove a IPv6 entry.
 * Use internal RCU to manage the deleted entry.
 *
 * @param ipv6_table pointer to the IPv6 table
 * @param ipv6 pointer to the IPv6 to be deleted
 */
void ipv6_remove(struct ipv6_table *ipv6_table, uint32_t ipv4_addr)
{
    rte_rwlock_write_lock(&ipv6_table->lock);
    int ret = rte_hash_del_key(ipv6_table->ht, &ipv4_addr);
    if (ret < 0) { // no existing entry or error
        BR_INFO("Unable to remove the ipv6 in ipv6 table.\n");
        rte_rwlock_write_unlock(&ipv6_table->lock);
        return;
    }
    ipv6_table->len--;
    rte_rwlock_write_unlock(&ipv6_table->lock);
}

/**
 * Return a string representation of the NID table.
 * The string should be freed by the caller.
 *
 * @param ipv6_table pointer to the NID table
 * @return char * a string representation of the NID table
 */
char *ipv6_table_to_string(struct ipv6_table *ipv6_table)
{
    uint32_t next = 0;
    int32_t ret;
    uint32_t ipv4_addr;
    struct ipv6_entry *entry;
    char *s = NULL;
    char buf[128] = {0};
    // StringBuilder *sb = new_sb();
    // int cnt = 0;
    // sprintf(buf, "%-36s%-16sSK\r\n", "IPv6 address", "IPv4 address");
    // sb_append(sb, buf);
    // unsigned int lcore_id = rte_lcore_id();
    // rte_rcu_qsbr_lock(ipv6_table->qsv, lcore_id);
    // while (true) {
    //     ret = rte_hash_iterate(ipv6_table->ht, (const void **)&ipv4_addr,
    //         (void **)&entry, &next);
    //     if (ret == -ENOENT) {
    //         break;
    //     }
    //     char ipv6_addr[SIZE_NID * 2 + 1];
    //     char ipv4_addr_c[16];
    //     hex_to_str(ipv6_addr, entry->ipv6_addr, SIZE_NID, 0);
    //     uint32_t ip = htonl(entry->ipv4_addr);
    //     inet_ntop(AF_INET, &ip, ipv4_addr, sizeof(ipv4_addr));
    //     sprintf(buf, "%-36s%-16s\r\n", ipv6_addr, ipv4_addr);
    //     sb_append(sb, buf);
    //     cnt++;
    // }
    // rte_rcu_qsbr_unlock(ipv6_table->qsv, lcore_id);
    // rte_rcu_qsbr_quiescent(ipv6_table->qsv, lcore_id);
    // sprintf(buf, "Number of NID entries: %d\r\n", cnt);
    // sb_append(sb, buf);
    // s = sb_get(sb);
    // free_sb(sb);
    return s;
}
