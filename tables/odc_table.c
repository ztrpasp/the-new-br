#include "odc_table.h"
#include "../utils/utils.h"
#include "../ansi_escape_seq.h"

/**
 * @brief Initialize the ODC table.
 * \p lock Initialize writer's lock.
 * \p len Set length to zero.
 * \p ht Create hash table.
 *
 * @param odc_table pointer to the ODC table
 */
void odc_table_init(struct odc_table *odc_table)
{
    /* Initialize writer's lock */
    rte_rwlock_init(&odc_table->lock);

    /* Initialize length of peer table */
    odc_table->len = 0;

    /* Create hash table */
    struct rte_hash_parameters odc_hash_params = {
        .name = "odc-hash",
        .entries = MAX_NUM_ODC_ENTRY,
        .reserved = 0,
        .key_len = ODC_TABLE_KEY_LEN,
        .hash_func = NULL,
        .hash_func_init_val = 0,
        .socket_id = rte_socket_id()
    };
    odc_table->ht = rte_hash_create(&odc_hash_params);
    ASSERT_NOT_NULL(odc_table->ht, "Unable to create the odc hash.\n");
}

/**
 * @brief Remove an entry in ODC table. This operation is multi-thread safe.
 * The timer associated with the entry will be stopped.
 *
 * @param odc_table pointer to the ODCT
 * @param key concatenation of pNID, LSID, cNID and hash of PIDs
 * @return int returns \p OK if the entry is removed else \p FAILED
 */
static int odc_remove(struct odc_table *odc_table, uint8_t *key)
{
    int32_t ret;
    struct odc_entry *p;
    rte_rwlock_write_lock(&odc_table->lock);
    ret = rte_hash_lookup_data(odc_table->ht, key, (void **)&p);
    if (ret < 0) {
        rte_rwlock_write_unlock(&odc_table->lock);
        BR_LOG("Remove ODC entry failed: no such entry.\n");
        return FAILED;
    }
    rte_timer_stop_sync(&p->timer);

    ret = rte_hash_del_key(odc_table->ht, key);
    if (ret < 0) {
        rte_rwlock_write_unlock(&odc_table->lock);
        BR_LOG("Remove ODC entry failed.\n");
        return FAILED;
    }
    rte_free(p); // free p after calling rte_hash_del_key()
    odc_table->len--;
    rte_rwlock_write_unlock(&odc_table->lock);
    return OK;
}

/* ODC timer callback. This operation is multi-thread safe. */
void odc_timer_cb(__attribute__((unused)) struct rte_timer *timer, void *arg)
{
    BR_LOG("An ODC entry expires.\n");
    struct odc_entry *p = (struct odc_entry *)arg;
    odc_remove(p->owner, p->key);
}

/**
 * @brief Add an entry to ODC table. This operation is multi-thread safe.
 *
 * @param odc_table pointer to the ODC table
 * @param nid_provider pointer to the NID of provider
 * @param l_sid pointer to LSID
 * @param nid_consumer pointer to the NID of consumer
 * @param pids pointer to the PID sequence
 * @param pids_len size of the PID sequence in bytes
 * @param rn the random number
 * @param expire_seconds how much time this entry will be in ODC table
 * @return int returns \p OK if the entry is added to the ODC table successfully
 *         else \p FAILED.
 */
int odc_add_entry(struct odc_table *odc_table, const uint8_t *nid_provider,
    const uint8_t *l_sid, const uint8_t *nid_consumer, const uint8_t *pids,
    uint32_t pids_len, uint32_t rn, int expire_seconds)
{
    struct odc_entry *p = NULL;
    uint8_t key[ODC_TABLE_KEY_LEN];
    odc_key(key, nid_provider, l_sid, nid_consumer, pids, pids_len);

    rte_rwlock_write_lock(&odc_table->lock);
    int32_t ret = rte_hash_lookup_data(odc_table->ht, key, (void **)&p);
    if (ret < 0) { // no existing entry
        /* max number of entries */
        if (odc_table->len >= MAX_NUM_ODC_ENTRY) {
            BR_INFO("ODC table is full. Cannot add more entries.\n");
            rte_rwlock_write_unlock(&odc_table->lock);
            return FAILED;
        }

        /* allocate odc_entry */
        p = rte_malloc("odc_entry", sizeof(struct odc_entry) + pids_len, 0);
        if (p == NULL) {
            BR_INFO("Unable to add allocate memory for ODC entry.\n");
            rte_rwlock_write_unlock(&odc_table->lock);
            return FAILED;
        }
        rte_memcpy(p->key, key, ODC_TABLE_KEY_LEN);
        p->rn = rn;
        p->owner = odc_table;
        p->pids_len = pids_len;
        rte_memcpy(p->pids, pids, pids_len);
        rte_timer_init(&p->timer);
        ret = rte_hash_add_key_data(odc_table->ht, key, p);
        if (ret < 0) {
            BR_INFO("Unable to add entry to ODC table.\n");
            rte_free(p);
            rte_rwlock_write_unlock(&odc_table->lock);
            return FAILED;
        }
        odc_table->len++;
        BR_LOG("ODCT: An ODC entry added.\n");
    } else { // existing entry, do not re-init timer
        p->rn = rn;
        BR_LOG("ODCT: ODC entry updated.\n");
    }

    /* Setup timer. The callback function is multi-thread safe and can be called
     * on different lcores. */
    const uint64_t ticks = rte_get_timer_hz() * expire_seconds;
    const unsigned int lcore_id = rte_lcore_id();
    rte_timer_reset_sync(&p->timer, ticks, SINGLE, lcore_id, odc_timer_cb, p);
    rte_rwlock_write_unlock(&odc_table->lock);
    return OK;
}

/**
 * @brief Return a string representation of the ODC table.
 * The returned string should be freed by the caller.
 *
 * @param odc_table pointer to the ODC table
 * @return char* a string representation of ODC table
 */
char *odc_table_to_string(struct odc_table *odc_table)
{
    uint32_t next = 0;
    StringBuilder *sb = new_sb();
    char buf[512];
    rte_rwlock_read_lock(&odc_table->lock);
    while (true) {
        uint8_t *key;
        struct odc_entry *pentry;
        int32_t ret = rte_hash_iterate(odc_table->ht, (const void **)&key,
            (void **)&pentry, &next);
        if (ret == -ENOENT) {
            break;
        }
        sb_append(sb, "- ");

        // NID provider, L_SID, NID provider
        int offset = 0;
        int lens[3] = {16, 20, 16};
        char *name[3] = {
            YEL "NID of provider: " RESET_STYLE,
            YEL "  LSID: " RESET_STYLE,
            YEL "  NID of consumer: " RESET_STYLE
        };
        for (int i = 0; i < 3; i++) {
            sb_append(sb, name[i]);
            hex_to_str(buf, pentry->key + offset, lens[i], '-');
            sb_append(sb, buf);
            sb_append(sb, ",\r\n");
            offset += lens[i];
        }

        // PID sequence
        sb_append(sb, YEL "  PID sequence: " RESET_STYLE);
        for (int i = 0; i < pentry->pids_len / PID_SIZE_IN_BYTES; i++) {
            sprintf(buf, "0x%08X, ", pentry->pids[i]);
            sb_append(sb, buf);
        }
        sprintf(buf, YEL "\r\n  Random number:" RESET_STYLE " 0x%08X\r\n\r\n", pentry->rn);
        sb_append(sb, buf);
    }
    rte_rwlock_read_unlock(&odc_table->lock);
    char *s = sb_get(sb);
    free_sb(sb);
    return s;
}