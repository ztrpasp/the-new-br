#include <rte_malloc.h>
#include "../utils/utils.h"
#include "px_table.h"

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
struct px_table *px_table_create(void)
{
    size_t size = sizeof(struct px_table) + sizeof(struct px_entry) * MAX_NUM_PX;
    struct px_table *px_table = rte_zmalloc("px_table", size, 0);
    ASSERT_NOT_NULL(px_table, "Unable to allocate memory for px table.\n");

    /* Initialize writer's lock */
    rte_rwlock_init(&px_table->lock);

    /* Create rte_lpm */
    struct rte_lpm_config lpm_cfg = {
        .flags = 0, /* This field is currently unused */
        .max_rules = MAX_NUM_PX,
        .number_tbl8s = NUM_TBL8S_PX
    };
    px_table->lpm = rte_lpm_create("px_lpm", rte_socket_id(), &lpm_cfg);
    ASSERT_NOT_NULL(px_table->lpm, "Unable to allocate memory for px rte_lpm.\n");

    /* Allocate space for QS variable and initialize it */
    size_t sz = rte_rcu_qsbr_get_memsize(RTE_MAX_LCORE);
    struct rte_rcu_qsbr *qsv = rte_zmalloc("rte_rcu_qsbr", sz, RTE_CACHE_LINE_SIZE);
    ASSERT_NOT_NULL(qsv, "Unable to allocate space for QS variable.\n");
    rte_rcu_qsbr_init(qsv, RTE_MAX_LCORE);
    px_table->qsv = qsv;
    struct rte_lpm_rcu_config rcu_cfg = {
        .v = qsv,
        .mode = RTE_LPM_QSBR_MODE_DQ,
        .dq_size = 0, /* RCU defer queue size, default: lpm->number_tbl8s. */
        .reclaim_thd = 0, /* Threshold to trigger auto reclaim. */
        .reclaim_max = 0 /* Max entries to reclaim in one go: default*/
    };
    rte_lpm_rcu_qsbr_add(px_table->lpm, &rcu_cfg);

    return px_table;
}

/**
 * @brief Find the first free entry in \p px_table->entries.
 * This function is \b NOT multi-thread safe and should be called once at a time.
 * Whether an entry is free is marked by \p is_valid flag. This function is
 * usually called when adding/updating entries.
 *
 * @param px_table pointer to the PX table
 * @param pos pointer to the space to store the index of free entry
 * @return int OK if a free entry is found else FAILED
 */
static int px_entry_find_free(struct px_table *px_table, int *pos)
{
    for (int i = 0; i < MAX_NUM_PX; i++) {
        if (!px_table->entries[i].is_valid) {
            *pos = i;
            return OK;
        }
    }
    return FAILED;
}

/**
 * @brief Returns true if the given path prefix is conflict with another path
 * prefix in the PX table. This operation is \b NOT multi-thread safe.
 *
 * @param px_table pointer to the PX table
 * @param px path prefix
 * @param prefix_len prefix length
 * @param conflict_px_entry pointer to return the conflict PX entry
 * @return true if the path prefix is conflict with another path prefix in
 *         \p px_table else false
 */
static bool is_conflict(struct px_table *px_table, uint32_t px, uint32_t prefix_len,
    struct px_entry **conflict_px_entry)
{
    for (int i = 0; i < MAX_NUM_PX; i++) {
        struct px_entry *px_entry = &px_table->entries[i];
        if (px_entry->is_valid) {
            uint32_t mask = PX_PREFIX_MASK(min(px_entry->prefix_len, prefix_len));
            if ((px_entry->px & mask) == (px & mask)) {
                *conflict_px_entry = px_entry;
                return true;
            }
        }
    }
    *conflict_px_entry = NULL;
    return false;
}

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
    uint32_t as_id, uint32_t ipv4_addr)
{
    int used_entry_idx, free_entry_idx;
    int ret, rp;

    if (prefix_len == 0 || prefix_len > 32) {
        BR_LOGF("invalid prefix_len\n");
        return FAILED;
    }
    px &= IPPREFIX2MASK(prefix_len);

    rte_rwlock_write_lock(&px_table->lock);

    /* Check if the path prefix to add is conflict with any entry in PX table */
    struct px_entry *cpe;
    if (is_conflict(px_table, px, prefix_len, &cpe)) {
        BR_LOGF("Conflict with entry %08X/%d\n", cpe->px, cpe->prefix_len);
        rte_rwlock_write_unlock(&px_table->lock);
        return FAILED;
    }

    /* Create a new entry. */
    ret = px_entry_find_free(px_table, &free_entry_idx);
    if (ret == FAILED) {
        BR_LOGF("PX table is full\n");
        rte_rwlock_write_unlock(&px_table->lock);
        return FAILED;
    }
    struct px_entry *px_entry = &px_table->entries[free_entry_idx];
    px_entry->px = px;
    px_entry->prefix_len = prefix_len;
    px_entry->as_id = as_id;
    px_entry->ipv4_addr = ipv4_addr;
    px_entry->is_valid = true;

    rp = rte_lpm_is_rule_present(px_table->lpm, px, prefix_len, &used_entry_idx);

    ret = rte_lpm_add(px_table->lpm, px, prefix_len, free_entry_idx);
    if (ret != 0) {
        BR_LOGF("lpm add failed\n");
        px_entry->is_valid = false;
        rte_rwlock_write_unlock(&px_table->lock);
        return FAILED;
    }

    /* The entry in use is freed by setting is_valid to false. Other fields in
     * in entry are not changed. The entry can be freed only when the key
     * (px, prefix_len) of the entry is the same as the key to be added. */
    if (rp == 1) {
        rte_rcu_qsbr_synchronize(px_table->qsv, rte_lcore_id());
        px_table->entries[used_entry_idx].is_valid = false;
        BR_LOGF("remove: %d\n", used_entry_idx);
    }

    BR_LOGF("add entry index=%d, 0x%08X/%d, as_id=%d, %d.%d.%d.%d\n", free_entry_idx,
        px, prefix_len, as_id, IPv4TO4DECIMAL(ipv4_addr));
    rte_rwlock_write_unlock(&px_table->lock);
    return OK;
}

/**
 * @brief Delete a PX entry identified by \p px and \p prefix_len in PX
 * table. This operation is multi-thread safe. Parameter \p ipv4_addr is not
 * checked in this function, so use a value of zero is OK.
 *
 * @param px_table pointer to the PX table
 * @param px path prefix to delete
 * @param prefix_len prefix length, valid range is [1, 32]
 * @param ipv4_addr IP address of the peer BR
 * @return int OK if the PX entry is removed successfully else FAILED
 */
int px_remove(struct px_table *px_table, uint32_t px, uint32_t prefix_len,
    uint32_t ipv4_addr)
{
    int rp, ret, used_entry_idx;

    if (prefix_len == 0 || prefix_len > 32) {
        BR_LOGF("invalid prefix_len\n");
        return FAILED;
    }
    px &= IPPREFIX2MASK(prefix_len);

    rte_rwlock_write_lock(&px_table->lock);

    rp = rte_lpm_is_rule_present(px_table->lpm, px, prefix_len, &used_entry_idx);
    RTE_ASSERT(rp == 0 || rp == 1);
    if (rp == 0) {
        BR_INFO("No such entry 0x%08X/%d, %d.%d.%d.%d\n", px, prefix_len,
            IPv4TO4DECIMAL(ipv4_addr));
        rte_rwlock_write_unlock(&px_table->lock);
        return OK;
    }

    ret = rte_lpm_delete(px_table->lpm, px, prefix_len);
    if (ret != 0) {
        BR_INFO("Failed to delete PX entry 0x%08X/%d\n", px, prefix_len);
        rte_rwlock_write_unlock(&px_table->lock);
        return FAILED;
    }

    /* The entry can be marked as freed only when the key of the entry
     * (px, prefix_len) is the same as the key to be deleted. */
    if (rp == 1) {
        rte_rcu_qsbr_synchronize(px_table->qsv, rte_lcore_id());
        px_table->entries[used_entry_idx].is_valid = false;
        BR_LOGF("remove index: %d\n", used_entry_idx);
    }

    BR_LOGF("delete entry 0x%08X/%d, %d.%d.%d.%d\n", px, prefix_len,
        IPv4TO4DECIMAL(ipv4_addr));

    rte_rwlock_write_unlock(&px_table->lock);
    return OK;
}

/**
 * @brief Return a string representation of the PX table.
 * The returned string should be freed by the caller.
 *
 * @param px_table pointer to the PX table
 * @return char* returns a string representation of the PX table
 */
char *px_table_to_string(struct px_table *px_table)
{
    char buf[128];
    char px[32];
    char ipv4_addr[32];
    struct px_entry *px_entry;
    unsigned int lcore_id = rte_lcore_id();
    StringBuilder *sb = new_sb();
    sprintf(buf, "%-20s%-16sAS\r\n", "PX", "IP address");
    sb_append(sb, buf);
    rte_rcu_qsbr_lock(px_table->qsv, lcore_id);
    for (int i = 0; i < MAX_NUM_PX; ++i) {
        px_entry = &px_table->entries[i];
        if (!px_entry->is_valid) {
            continue;
        }
        sprintf(px, "0x%08X/%d", px_entry->px, px_entry->prefix_len);
        sprintf(ipv4_addr, "%d.%d.%d.%d", IPv4TO4DECIMAL(px_entry->ipv4_addr));
        sprintf(buf, "%-20s%-16s%d\r\n", px, ipv4_addr, px_entry->as_id);
        sb_append(sb, buf);
    }
    rte_rcu_qsbr_unlock(px_table->qsv, lcore_id);
    rte_rcu_qsbr_quiescent(px_table->qsv, lcore_id);
    char *s = sb_get(sb);
    free_sb(sb);
    return s;
}