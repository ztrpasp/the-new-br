#include <rte_malloc.h>
#include <rte_cycles.h>
#include "mac_table.h"
#include "../utils/utils.h"
#include "../log.h"

/**
 * @brief Free the space allocated to an MAC entry.
 * The timer should be stopped before calling this function.
 *
 * @param e pointer to the MAC entry
 */
static void mac_entry_free(struct mac_entry *e)
{
    BR_LOG("Free MAC entry: %x:%x:%x:%x:%x:%x:%x:%x:%x:%x:%x:%x:%x:%x:%x:%x\n", IPv6TO16HEX(e->ip));
    rte_free(e);
}

/* free_key_data_func of rte_hash_rcu_config */
static void mac_table_data_free(void *p, void *key_data)
{
    mac_entry_free((struct mac_entry *)key_data);
}

/**
 * @brief Initialize mac table.
 * \p lock Initialize writer's lock.
 * \p qsv Allocate space for QS variable.
 * \p len Set length to zero.
 * \p ht Create hash table.
 *
 * @param mac_table pointer to the mac table
 */
void mac_table_init(struct mac_table *mac_table)
{
    /* Initialize writer's lock */
    rte_rwlock_init(&mac_table->lock);

    /* Allocate space for QS variable */
    size_t sz = rte_rcu_qsbr_get_memsize(RTE_MAX_LCORE);
    struct rte_rcu_qsbr *qsv = rte_zmalloc("rte_rcu_qsbr", sz, RTE_CACHE_LINE_SIZE);
    ASSERT_NOT_NULL(qsv, "Unable to initialize the MAC table.\n");
    rte_rcu_qsbr_init(qsv, RTE_MAX_LCORE);
    mac_table->qsv = qsv;

    /* Initialize length of mac table */
    mac_table->len = 0;

    /* Create hash table */
    struct rte_hash_parameters hash_params = {
        .name = "mac_table_hash",
        .entries = MAX_NUM_MAC_ENTRY,
        .reserved = 0,
        .key_len = 16, // the length of IP address is 4 bytes.
        .hash_func = NULL,
        .hash_func_init_val = 0,
        .socket_id = rte_socket_id()
    };
    mac_table->ht = rte_hash_create(&hash_params);
    ASSERT_NOT_NULL(mac_table->ht, "Unable to create the mac hash table.\n");

    struct rte_hash_rcu_config rcu_cfg = {
        .v = qsv,
        .mode = RTE_HASH_QSBR_MODE_DQ,
        .dq_size = 0, /* default */
        .trigger_reclaim_limit = 0, /* ? */
        .max_reclaim_size = 0, /* default */
        .key_data_ptr = NULL,
        .free_key_data_func = mac_table_data_free,
    };
    rte_hash_rcu_qsbr_add(mac_table->ht, &rcu_cfg);
}

/**
 * @brief Remove an entry in MAC table. This operation is multi-thread safe.
 * Use internal RCU to manage the deleted entry.
 *
 * @param mac_table pointer to the MAC table
 * @param ipv6_addr the IP address of the MAC entry to be removed
 * @return int returns OK if an entry is removed else FAILED(if no entry is removed)
 */
static int mac_remove_entry(struct mac_table *mac_table, uint8_t *ipv6_addr)
{
    int32_t ret;
    struct mac_entry *e;
    rte_rwlock_write_lock(&mac_table->lock);
    ret = rte_hash_lookup_data(mac_table->ht, ipv6_addr, (void **)&e);
    if (ret < 0) {
        BR_LOG("Remove MAC entry: failed, IP address: %x:%x:%x:%x:%x:%x:%x:%x:%x:%x:%x:%x:%x:%x:%x:%x\n",
            IPv6TO16HEX(ipv6_addr));
        rte_rwlock_write_unlock(&mac_table->lock);
        return FAILED;
    }

    rte_timer_stop_sync(&e->timer);
    if (unlikely(g_br_conf.debugging_on)) {
        char mac_addr[32];
        rte_ether_format_addr(mac_addr, 32, &e->hw_addr);
        BR_LOG("MAC entry removed: IP address: %x:%x:%x:%x:%x:%x:%x:%x:%x:%x:%x:%x:%x:%x:%x:%x, MAC address: %s\n",
            IPv6TO16HEX(ipv6_addr), mac_addr);
    }
    ret = rte_hash_del_key(mac_table->ht, ipv6_addr);
    RTE_ASSERT(ret > 0);
    rte_rwlock_write_unlock(&mac_table->lock);
    return OK;
}

/* MAC timer callback. This operation is multi-thread safe. */
static void mac_timer_cb(__attribute__((unused)) struct rte_timer *timer, void *arg)
{
    BR_LOG("An MAC entry expires.\n");
    struct mac_entry *p = (struct mac_entry *)arg;
    mac_remove_entry(p->owner, p->ip);
}

/**
 * @brief Add an entry to MAC table. This operation is multi-thread safe.
 *
 * @param mac_table pointer to the MAC table
 * @param ipv6_addr IP address tobe added
 * @param hw_addr hardware address to be added
 * @return int returns OK if the entry is added to the MAC table successfully
 * 		   else FAILED.
 */
int mac_add(struct mac_table *mac_table, uint8_t *ipv6_addr,
    const struct rte_ether_addr *hw_addr)
{
    struct mac_entry *e = NULL;
    unsigned int lcore_id = rte_lcore_id();

    char mac_addr[32];
    if (unlikely(g_br_conf.debugging_on)) {
        rte_ether_format_addr(mac_addr, sizeof(mac_addr), hw_addr);
    }

    rte_rwlock_write_lock(&mac_table->lock);
    int32_t ret = rte_hash_lookup_data(mac_table->ht, ipv6_addr, (void **)&e);

    if (ret < 0 && mac_table->len >= MAX_NUM_MAC_ENTRY) {
        BR_INFO("MAC table is full. Cannot add more MAC entries.\n");
        rte_rwlock_write_unlock(&mac_table->lock);
        return FAILED;
    }

    struct mac_entry *e_new =
        rte_malloc("mac_entry", sizeof(struct mac_entry), RTE_CACHE_LINE_SIZE);
    if (!e_new) {
        BR_INFO("Unable to add allocate memory for MAC entry.\n");
        rte_rwlock_write_unlock(&mac_table->lock);
        return FAILED;
    }
    IPv6_copy(e_new->ip, ipv6_addr);
    e_new->owner = mac_table;
    rte_ether_addr_copy(hw_addr, &e_new->hw_addr);
    rte_timer_init(&e_new->timer);
    ret = rte_hash_add_key_data(mac_table->ht, ipv6_addr, e_new);

    if (e) {
        RTE_ASSERT(ret > 0);
        rte_timer_stop_sync(&e->timer);
        /* Wait for quiescent state change */
        rte_rcu_qsbr_synchronize(mac_table->qsv, lcore_id);
        mac_entry_free(e);
        BR_LOG("MAC entry updated: IP address: %x:%x:%x:%x:%x:%x:%x:%x:%x:%x:%x:%x:%x:%x:%x:%x, MAC address: %s\n",
            IPv6TO16HEX(ipv6_addr), mac_addr);
    } else { /* no existing entry */
        if (ret < 0) {
            BR_INFO("Unable to add entry to the MAC table.\n");
            mac_entry_free(e_new);
            rte_rwlock_write_unlock(&mac_table->lock);
            return FAILED;
        }
        mac_table->len++;
        BR_LOG("MAC entry added: IP address:%x:%x:%x:%x:%x:%x:%x:%x:%x:%x:%x:%x:%x:%x:%x:%x, MAC address: %s\n",
            IPv6TO16HEX(ipv6_addr), mac_addr);
    }

    /* Setup timer. The callback function is multi-thread safe and can be called
     * on different lcores. */
    uint64_t ticks = rte_get_timer_hz() * MAC_EXPIRE_SECONDS;
    rte_timer_reset_sync(&e_new->timer, ticks, SINGLE, lcore_id, mac_timer_cb, e_new);
    rte_rwlock_write_unlock(&mac_table->lock);
    return OK;
}

/**
 * @brief Return a string representation of the MAC table.
 * The returned string should be freed by the caller.
 *
 * @param mac_table pointer to the MAC table
 * @return char* a string representation of the MAC table
 */
char *mac_table_to_string(struct mac_table *mac_table)
{
    uint32_t next = 0;
    uint8_t ipv6_addr[IPV6_SIZE];
    struct mac_entry *e;
    StringBuilder *sb = new_sb();
    unsigned int lcore_id = rte_lcore_id();
    rte_rcu_qsbr_lock(mac_table->qsv, lcore_id);
    char buf[128];
    char ip_addr_buf[64];
    char mac_addr_buf[64];
    sprintf(buf, "%-20sMAC address\r\n", "IP address");
    sb_append(sb, buf);
    while (true) {
        int ret = rte_hash_iterate(mac_table->ht, (const void **)&ipv6_addr,
            (void **)&e, &next);
        if (ret == -ENOENT) {
            break;
        }
        sprintf(ip_addr_buf, "%s", e->ip);
        rte_ether_format_addr(mac_addr_buf, sizeof(mac_addr_buf), &e->hw_addr);
        sprintf(buf, "%-20s%s\r\n", ip_addr_buf, mac_addr_buf);
        sb_append(sb, buf);
    }
    rte_rcu_qsbr_unlock(mac_table->qsv, lcore_id);
    rte_rcu_qsbr_quiescent(mac_table->qsv, lcore_id);
    char *s = sb_get(sb);
    free_sb(sb);
    return s;
}