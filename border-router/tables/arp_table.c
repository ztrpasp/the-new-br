#include <rte_malloc.h>
#include <rte_cycles.h>
#include "arp_table.h"
#include "../utils/utils.h"
#include "../log.h"

/**
 * @brief Free the space allocated to an ARP entry.
 * The timer should be stopped before calling this function.
 *
 * @param e pointer to the ARP entry
 */
static void arp_entry_free(struct arp_entry *e)
{
    BR_LOG("Free ARP entry: %d.%d.%d.%d\n", IPv4TO4DECIMAL(e->ipv4_addr));
    rte_free(e);
}

/* free_key_data_func of rte_hash_rcu_config */
static void arp_table_data_free(void *p, void *key_data)
{
    arp_entry_free((struct arp_entry *)key_data);
}

/**
 * @brief Initialize arp table.
 * \p lock Initialize writer's lock.
 * \p qsv Allocate space for QS variable.
 * \p len Set length to zero.
 * \p ht Create hash table.
 *
 * @param arp_table pointer to the arp table
 */
void arp_table_init(struct arp_table *arp_table)
{
    /* Initialize writer's lock */
    rte_rwlock_init(&arp_table->lock);

    /* Allocate space for QS variable */
    size_t sz = rte_rcu_qsbr_get_memsize(RTE_MAX_LCORE);
    struct rte_rcu_qsbr *qsv = rte_zmalloc("rte_rcu_qsbr", sz, RTE_CACHE_LINE_SIZE);
    ASSERT_NOT_NULL(qsv, "Unable to initialize the ARP table.\n");
    rte_rcu_qsbr_init(qsv, RTE_MAX_LCORE);
    arp_table->qsv = qsv;

    /* Initialize length of arp table */
    arp_table->len = 0;

    /* Create hash table */
    struct rte_hash_parameters hash_params = {
        .name = "arp_table_hash",
        .entries = MAX_NUM_ARP_ENTRY,
        .reserved = 0,
        .key_len = 4, // the length of IP address is 4 bytes.
        .hash_func = NULL,
        .hash_func_init_val = 0,
        .socket_id = rte_socket_id()
    };
    arp_table->ht = rte_hash_create(&hash_params);
    ASSERT_NOT_NULL(arp_table->ht, "Unable to create the arp hash table.\n");

    struct rte_hash_rcu_config rcu_cfg = {
        .v = qsv,
        .mode = RTE_HASH_QSBR_MODE_DQ,
        .dq_size = 0, /* default */
        .trigger_reclaim_limit = 0, /* ? */
        .max_reclaim_size = 0, /* default */
        .key_data_ptr = NULL,
        .free_key_data_func = arp_table_data_free,
    };
    rte_hash_rcu_qsbr_add(arp_table->ht, &rcu_cfg);
}

/**
 * @brief Remove an entry in ARP table. This operation is multi-thread safe.
 * Use internal RCU to manage the deleted entry.
 *
 * @param arp_table pointer to the ARP table
 * @param ipv4_addr the IP address of the ARP entry to be removed
 * @return int returns OK if an entry is removed else FAILED(if no entry is removed)
 */
static int arp_remove_entry(struct arp_table *arp_table, uint32_t ipv4_addr)
{
    int32_t ret;
    struct arp_entry *e;
    rte_rwlock_write_lock(&arp_table->lock);
    ret = rte_hash_lookup_data(arp_table->ht, &ipv4_addr, (void **)&e);
    if (ret < 0) {
        BR_LOG("Remove ARP entry: failed, IP address: %d.%d.%d.%d\n",
            IPv4TO4DECIMAL(ipv4_addr));
        rte_rwlock_write_unlock(&arp_table->lock);
        return FAILED;
    }

    rte_timer_stop_sync(&e->timer);
    if (unlikely(g_br_conf.debugging_on)) {
        char mac_addr[32];
        rte_ether_format_addr(mac_addr, 32, &e->hw_addr);
        BR_LOG("ARP entry removed: IP address: %d.%d.%d.%d, MAC address: %s\n",
            IPv4TO4DECIMAL(ipv4_addr), mac_addr);
    }
    ret = rte_hash_del_key(arp_table->ht, &ipv4_addr);
    RTE_ASSERT(ret > 0);
    rte_rwlock_write_unlock(&arp_table->lock);
    return OK;
}

/* ARP timer callback. This operation is multi-thread safe. */
static void arp_timer_cb(__attribute__((unused)) struct rte_timer *timer, void *arg)
{
    BR_LOG("An ARP entry expires.\n");
    struct arp_entry *p = (struct arp_entry *)arg;
    arp_remove_entry(p->owner, p->ipv4_addr);
}

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
    const struct rte_ether_addr *hw_addr)
{
    struct arp_entry *e = NULL;
    unsigned int lcore_id = rte_lcore_id();

    char mac_addr[32];
    if (unlikely(g_br_conf.debugging_on)) {
        rte_ether_format_addr(mac_addr, sizeof(mac_addr), hw_addr);
    }

    rte_rwlock_write_lock(&arp_table->lock);
    int32_t ret = rte_hash_lookup_data(arp_table->ht, &ipv4_addr, (void **)&e);

    if (ret < 0 && arp_table->len >= MAX_NUM_ARP_ENTRY) {
        BR_INFO("ARP table is full. Cannot add more ARP entries.\n");
        rte_rwlock_write_unlock(&arp_table->lock);
        return FAILED;
    }

    struct arp_entry *e_new =
        rte_malloc("arp_entry", sizeof(struct arp_entry), RTE_CACHE_LINE_SIZE);
    if (!e_new) {
        BR_INFO("Unable to add allocate memory for ARP entry.\n");
        rte_rwlock_write_unlock(&arp_table->lock);
        return FAILED;
    }
    e_new->ipv4_addr = ipv4_addr;
    e_new->owner = arp_table;
    rte_ether_addr_copy(hw_addr, &e_new->hw_addr);
    rte_timer_init(&e_new->timer);
    ret = rte_hash_add_key_data(arp_table->ht, &ipv4_addr, e_new);

    if (e) {
        RTE_ASSERT(ret > 0);
        rte_timer_stop_sync(&e->timer);
        /* Wait for quiescent state change */
        rte_rcu_qsbr_synchronize(arp_table->qsv, lcore_id);
        arp_entry_free(e);
        BR_LOG("ARP entry updated: IP address: %d.%d.%d.%d, MAC address: %s\n",
            IPv4TO4DECIMAL(ipv4_addr), mac_addr);
    } else { /* no existing entry */
        if (ret < 0) {
            BR_INFO("Unable to add entry to the ARP table.\n");
            arp_entry_free(e_new);
            rte_rwlock_write_unlock(&arp_table->lock);
            return FAILED;
        }
        arp_table->len++;
        BR_LOG("ARP entry added: IP address: %d.%d.%d.%d, MAC address: %s\n",
            IPv4TO4DECIMAL(ipv4_addr), mac_addr);
    }

    /* Setup timer. The callback function is multi-thread safe and can be called
     * on different lcores. */
    uint64_t ticks = rte_get_timer_hz() * ARP_EXPIRE_SECONDS;
    rte_timer_reset_sync(&e_new->timer, ticks, SINGLE, lcore_id, arp_timer_cb, e_new);
    rte_rwlock_write_unlock(&arp_table->lock);
    return OK;
}

/**
 * @brief Return a string representation of the ARP table.
 * The returned string should be freed by the caller.
 *
 * @param arp_table pointer to the ARP table
 * @return char* a string representation of the ARP table
 */
char *arp_table_to_string(struct arp_table *arp_table)
{
    uint32_t next = 0;
    uint32_t *ipv4_addr;
    struct arp_entry *e;
    StringBuilder *sb = new_sb();
    unsigned int lcore_id = rte_lcore_id();
    rte_rcu_qsbr_lock(arp_table->qsv, lcore_id);
    char buf[128];
    char ip_addr_buf[64];
    char mac_addr_buf[64];
    sprintf(buf, "%-20sMAC address\r\n", "IP address");
    sb_append(sb, buf);
    while (true) {
        int ret = rte_hash_iterate(arp_table->ht, (const void **)&ipv4_addr,
            (void **)&e, &next);
        if (ret == -ENOENT) {
            break;
        }
        sprintf(ip_addr_buf, "%d.%d.%d.%d", IPv4TO4DECIMAL(e->ipv4_addr));
        rte_ether_format_addr(mac_addr_buf, sizeof(mac_addr_buf), &e->hw_addr);
        sprintf(buf, "%-20s%s\r\n", ip_addr_buf, mac_addr_buf);
        sb_append(sb, buf);
    }
    rte_rcu_qsbr_unlock(arp_table->qsv, lcore_id);
    rte_rcu_qsbr_quiescent(arp_table->qsv, lcore_id);
    char *s = sb_get(sb);
    free_sb(sb);
    return s;
}