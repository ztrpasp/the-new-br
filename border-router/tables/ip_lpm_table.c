#include <rte_malloc.h>
#include "ip_lpm_table.h"
#include "../log.h"
#include "../utils/utils.h"

/**
 * @brief Allocate space for IP LPM table and initialize it.
 * \p lock Initialize writer's lock.
 * \p qsv Allocate space for QS variable.
 * This function may cause program exit if there's not enough memory to allocate.
 * Thus, \p ip_lpm_table_create should only be called during program initialization.
 *
 * @return struct ip_lpm_table* returns pointer to the created IP LPM table if
 *         nothing goes wrong else the program will exit.
 */
struct ip_lpm_table *ip_lpm_table_create(void)
{
    int const align = 16;
    size_t lpm_entry_size = RTE_ALIGN(sizeof(struct ip_lpm_entry), align);
    size_t lpm_table_size = RTE_ALIGN(sizeof(struct ip_lpm_table), align);
    size_t size = lpm_table_size + lpm_entry_size * MAX_NUM_IP_ROUTE;
    struct ip_lpm_table *lpm_table =
        rte_zmalloc("ip_lpm_table", size, RTE_CACHE_LINE_SIZE);
    ASSERT_NOT_NULL(lpm_table, "Unable to allocate memory for ip lpm table.\n");

    /* Initialize writer's lock */
    rte_rwlock_init(&lpm_table->lock);

    /* Create rte_lpm */
    struct rte_lpm_config lpm_cfg = {
        .flags = 0, /* This field is currently unused */
        .max_rules = MAX_NUM_IP_ROUTE,
        .number_tbl8s = NUM_TBL8S
    };
    lpm_table->lpm = rte_lpm_create("ip_lpm", rte_socket_id(), &lpm_cfg);
    ASSERT_NOT_NULL(lpm_table->lpm, "Unable to allocate memory for ip rte_lpm.\n");

    /* Allocate space for QS variable and initialize it */
    size_t sz = rte_rcu_qsbr_get_memsize(RTE_MAX_LCORE);
    struct rte_rcu_qsbr *qsv = rte_zmalloc("rte_rcu_qsbr", sz, RTE_CACHE_LINE_SIZE);
    ASSERT_NOT_NULL(qsv, "Unable to allocate space for QS variable.\n");
    rte_rcu_qsbr_init(qsv, RTE_MAX_LCORE);
    lpm_table->qsv = qsv;
    struct rte_lpm_rcu_config rcu_cfg = {
        .v = qsv,
        .mode = RTE_LPM_QSBR_MODE_DQ,
        .dq_size = 0, /* RCU defer queue size, default: lpm->number_tbl8s. */
        .reclaim_thd = 0, /* Threshold to trigger auto reclaim. */
        .reclaim_max = 0 /* Max entries to reclaim in one go: default*/
    };
    rte_lpm_rcu_qsbr_add(lpm_table->lpm, &rcu_cfg);

    return lpm_table;
}

/**
 * @brief Find the first free entry in \p lpm_table->entries.
 * This function is \b NOT multi-thread safe and should be called once at a time.
 * Whether an entry is free is marked by \p is_valid flag. This function is
 * usually called when adding/updating entries.
 *
 * @param lpm_table pointer to the IP LPM table
 * @param pos pointer to the space to store the index of free entry
 * @return int returns OK if a free entry is found else FAILED
 */
static int ip_lpm_entry_find_free(struct ip_lpm_table *lpm_table, int *pos)
{
    for (int i = 0; i < MAX_NUM_IP_ROUTE; i++) {
        if (!lpm_table->entries[i].is_valid) {
            *pos = i;
            return OK;
        }
    }
    return FAILED;
}

/**
 * @brief Insert an entry (identified by \p ipv4_addr and \p prefix_len ) to IP
 * LPM table. This operation is multi-thread safe.
 *
 * Invalid \p prefix_len, no free entry in \p lpm_table will cause the operation
 * to fail, and \p FAILED is returned to the caller.
 *
 * Parameter \p is_autogen represents whether the entry is auto-generated during
 * port configuration, and will be recorded by this function faithfully. It is
 * the caller's responsibility to maintain the value of \p is_autogen. The value
 * of parameter \p portid may be -1 if the portid cannot be decided yet. You may
 * call \p ip_lpm_update_portid later to update the portid.
 *
 * This function guarantees that, when an entry identified by \p ipv4_addr and
 * \p prefix_len is already in the table,
 *
 * @param lpm_table pointer to the IP LPM table
 * @param ipv4_addr IP address
 * @param prefix_len prefix length, valid range is [1, 32]
 * @param next_hop IP address of next hop
 * @param portid the ID of the port to next hop, negative value -1 means no port id
 * @param is_autogen whether the entry is auto-generated during port configuration
 * @return int returns OK if the entry is inserted to IP LPM table successfully
 *             else FAILED
 */
int ip_lpm_add(struct ip_lpm_table *lpm_table, uint32_t ipv4_addr, uint8_t prefix_len,
    uint32_t next_hop, int16_t portid, bool is_autogen)
{
    int used_entry_idx, free_entry_idx;
    int ret, rp;

    if (prefix_len == 0 || prefix_len > 32) {
        BR_LOGF("invalid prefix_len\n");
        return FAILED;
    }
    ipv4_addr &= IPPREFIX2MASK(prefix_len);

    rte_rwlock_write_lock(&lpm_table->lock);

    /* Create a new entry. */
    ret = ip_lpm_entry_find_free(lpm_table, &free_entry_idx);
    if (ret == FAILED) {
        BR_LOGF("ip lpm table is full\n");
        rte_rwlock_write_unlock(&lpm_table->lock);
        return FAILED;
    }
    struct ip_lpm_entry *ip_lpm_entry = &lpm_table->entries[free_entry_idx];
    ip_lpm_entry->ipv4_addr = ipv4_addr;
    ip_lpm_entry->prefix_len = prefix_len;
    ip_lpm_entry->next_hop = next_hop;
    ip_lpm_entry->portid = portid;
    ip_lpm_entry->is_autogen = is_autogen;
    ip_lpm_entry->is_valid = true;

    rp = rte_lpm_is_rule_present(lpm_table->lpm, ipv4_addr, prefix_len, &used_entry_idx);

    ret = rte_lpm_add(lpm_table->lpm, ipv4_addr, prefix_len, free_entry_idx);
    if (ret != 0) {
        BR_LOGF("lpm add failed\n");
        ip_lpm_entry->is_valid = false;
        rte_rwlock_write_unlock(&lpm_table->lock);
        return FAILED;
    }

    /* The entry in use is freed by setting is_valid to false. Other fields in
     * in entry are not changed. The entry can be freed only when the key
     * (ipv4_addr, prefix_len) of the entry is the same as the key to be added. */
    if (rp == 1) {
        rte_rcu_qsbr_synchronize(lpm_table->qsv, rte_lcore_id());
        lpm_table->entries[used_entry_idx].is_valid = false;
        BR_LOGF("remove: %d\n", used_entry_idx);
    }

    BR_LOGF("add entry index=%d %d.%d.%d.%d/%d %d.%d.%d.%d portid=%d\n", free_entry_idx,
        IPv4TO4DECIMAL(ipv4_addr), prefix_len, IPv4TO4DECIMAL(next_hop), portid);
    rte_rwlock_write_unlock(&lpm_table->lock);
    return OK;
}

/**
 * @brief Delete a route identified by \p ipv4_addr and \p prefix_len in IP LPM
 * table. This operation is multi-thread safe. Parameter \p next_hop is not
 * checked in this function, so use a value of zero is OK.
 *
 * @param lpm_table pointer to the IP LPM table
 * @param ipv4_addr IP address
 * @param prefix_len prefix length, valid range is [1, 32]
 * @param next_hop IP address of next hop, the value is not used in this function
 * @return int returns OK if the route is removed successfully else FAILED
 */
int ip_lpm_remove(struct ip_lpm_table *lpm_table, uint32_t ipv4_addr,
    uint8_t prefix_len, uint32_t next_hop)
{
    int rp, ret, used_entry_idx;

    if (prefix_len == 0 || prefix_len > 32) {
        BR_LOGF("invalid prefix_len\n");
        return FAILED;
    }
    ipv4_addr &= IPPREFIX2MASK(prefix_len);

    rte_rwlock_write_lock(&lpm_table->lock);

    rp = rte_lpm_is_rule_present(lpm_table->lpm, ipv4_addr, prefix_len, &used_entry_idx);
    RTE_ASSERT(rp == 0 || rp == 1);
    if (rp == 0) {
        BR_LOGF("no such entry\n");
        BR_INFO("No route for %d.%d.%d.%d/%d\n", IPv4TO4DECIMAL(ipv4_addr), prefix_len);
        rte_rwlock_write_unlock(&lpm_table->lock);
        return OK;
    }

    ret = rte_lpm_delete(lpm_table->lpm, ipv4_addr, prefix_len);
    if (ret != 0) {
        BR_INFO("Failed to delete route for %d.%d.%d.%d/%d\n",
            IPv4TO4DECIMAL(ipv4_addr), prefix_len);
        rte_rwlock_write_unlock(&lpm_table->lock);
        return FAILED;
    }

    /* The entry can be marked as freed only when the key of the entry
     * (ipv4_addr, prefix_len) is the same as the key to be deleted. */
    if (rp == 1) {
        rte_rcu_qsbr_synchronize(lpm_table->qsv, rte_lcore_id());
        lpm_table->entries[used_entry_idx].is_valid = false;
        BR_LOGF("remove index: %d\n", used_entry_idx);
    }

    BR_LOGF("delete entry %d.%d.%d.%d/%d %d.%d.%d.%d\n",
        IPv4TO4DECIMAL(ipv4_addr), prefix_len, IPv4TO4DECIMAL(next_hop));

    rte_rwlock_write_unlock(&lpm_table->lock);
    return OK;
}

/**
 * @brief Update the portid in the IP LPM table.
 * This operation is multi-thread safe.
 *
 * This function will modify \p lpm_table therefore the writer's lock of
 * \p lpm_table is aquired so that no other thread can delete or add entries
 * in this table during the execution of this function.
 *
 * When updating portid, the old entry will not be marked as free. Instead,
 * the old entry is still valid for readers as the store of a 2-byte
 * variable in one entry is atomic. So the integrity of that entry is guaranteed.
 * The readers either read the old value of portid or read the new value of
 * portid providing the store operation is atomic.
 *
 * This function should be called when the port table changes.
 *
 * @param lpm_table pointer to the IP LPM table
 * @param get_portid function pointer to get portid
 */
void ip_lpm_update_portid(struct ip_lpm_table *lpm_table, get_portid_t get_portid)
{
    struct ip_lpm_entry *lpm_entry;
    unsigned int lcore_id = rte_lcore_id();
    rte_rwlock_write_lock(&lpm_table->lock);
    for (int i = 0; i < MAX_NUM_IP_ROUTE; ++i) {
        lpm_entry = &lpm_table->entries[i];
        if (!lpm_entry->is_valid) {
            continue;
        }
        int16_t portid = get_portid(lpm_entry->next_hop);
        lpm_entry->portid = portid;
    }
    rte_rwlock_write_unlock(&lpm_table->lock);
}

/**
 * @brief Return a string representation of the IP LPM table.
 * The returned string should be freed by the caller.
 *
 * @param lpm_table pointer to the IP LPM table
 * @return char* returns a string representation of the IP LPM table
 */
char *ip_lpm_to_string(struct ip_lpm_table *lpm_table)
{
    char *s = NULL;
    char buf[128];
    char ipv4_addr_mask[32];
    char next_hop[32];
    struct ip_lpm_entry *lpm_entry;
    unsigned int lcore_id = rte_lcore_id();
    StringBuilder *sb = new_sb();

    sprintf(buf, "%-20s%-15s%-10s\r\n", "Destination/Mask", "NextHop", "PortID");
    sb_append(sb, buf);
    rte_rcu_qsbr_lock(lpm_table->qsv, lcore_id);

    for (int i = 0; i < MAX_NUM_IP_ROUTE; ++i) {
        lpm_entry = &lpm_table->entries[i];
        if (!lpm_entry->is_valid) {
            continue;
        }
        sprintf(ipv4_addr_mask, "%d.%d.%d.%d/%d",
            IPv4TO4DECIMAL(lpm_entry->ipv4_addr), lpm_entry->prefix_len);
        sprintf(next_hop, "%d.%d.%d.%d",
            IPv4TO4DECIMAL(lpm_entry->next_hop));
        if (lpm_entry->portid >= 0) {
            sprintf(buf, "%-20s%-15s%-10d\r\n", ipv4_addr_mask, next_hop,
                lpm_entry->portid);
        } else {
            sprintf(buf, "%-20s%-15s%-10s\r\n", ipv4_addr_mask, next_hop, "None");
        }
        sb_append(sb, buf);
    }
    rte_rcu_qsbr_unlock(lpm_table->qsv, lcore_id);
    rte_rcu_qsbr_quiescent(lpm_table->qsv, lcore_id);
    s = sb_get(sb);
    free_sb(sb);
    return s;
}

/**
 * @brief Return a string representing the configuration of the IP LPM table.
 * The returned string can be used in configuration file.
 * The returned string should be freed by the caller.
 *
 * @param lpm_table pointer to the IP LPM table
 * @return char* returns a configuration string of the IP LPM table
 */
char *ip_lpm_config_string(struct ip_lpm_table *lpm_table)
{
    char *s = NULL;
    char buf[128];
    struct ip_lpm_entry *lpm_entry;
    unsigned int lcore_id = rte_lcore_id();
    StringBuilder *sb = new_sb();
    rte_rcu_qsbr_lock(lpm_table->qsv, lcore_id);
    for (int i = 0; i < MAX_NUM_IP_ROUTE; ++i) {
        lpm_entry = &lpm_table->entries[i];
        if (!lpm_entry->is_valid || lpm_entry->is_autogen) {
            continue;
        }
        sprintf(buf, "ip-route %d.%d.%d.%d/%d %d.%d.%d.%d\r\n",
            IPv4TO4DECIMAL(lpm_entry->ipv4_addr),
            lpm_entry->prefix_len,
            IPv4TO4DECIMAL(lpm_entry->next_hop));
        sb_append(sb, buf);
    }
    rte_rcu_qsbr_unlock(lpm_table->qsv, lcore_id);
    rte_rcu_qsbr_quiescent(lpm_table->qsv, lcore_id);
    s = sb_get(sb);
    free_sb(sb);
    return s;
}