#ifndef __ODC_TABLE_H__
#define __ODC_TABLE_H__

#include <stdint.h>
#include <rte_timer.h>
#include <rte_hash.h>
#include <rte_malloc.h>
#include <rte_memcpy.h>
#include <rte_cycles.h>
#include <rte_rwlock.h>
#include "../log.h"
#include "../common.h"
#include "../packet/pid.h"

#ifdef PERFORMANCE_TEST
#define MAX_NUM_ODC_ENTRY (1024 * 1024) // 128 millions
#else
#define MAX_NUM_ODC_ENTRY (1024 * 1024) // 1 million
#endif

// NIDprovider(16 bytes), L_SID(20 bytes), NIDconsumer (16 bytes), hash(PIDs) (4 bytes)
#define ODC_TABLE_KEY_LEN (SIZE_NID * 2 + SIZE_LSID + 4)
#define ODC_ENTRY_EXPIRE_SECONDS 800

struct odc_table;
struct odc_entry {
    uint8_t key[ODC_TABLE_KEY_LEN];
    uint32_t rn; // random number
    struct rte_timer timer;
    struct odc_table *owner;
    uint32_t pids_len;
    uint32_t pids[0];
};

struct odc_table {
    rte_rwlock_t lock;
    int len;
    struct rte_hash *ht;
};

/**
 * @brief Initialize the ODC table.
 * \p lock Initialize writer's lock.
 * \p len Set length to zero.
 * \p ht Create hash table.
 *
 * @param odc_table pointer to the ODC table
 */
void odc_table_init(struct odc_table *odc_table);

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
    uint32_t pids_len, uint32_t rn, int expire_seconds);

/**
 * @brief Generate key of ODC table.
 *
 * @param key pointer to space to store the key
 * @param nid_provider pointer to the NID of provider
 * @param l_sid pointer to LSID
 * @param nid_consumer pointer to the NID of consumer
 * @param pids pointer to the PID sequence
 * @param pids_len size of the PID sequence in bytes
 */
static inline void odc_key(uint8_t *key, const uint8_t *nid_provider, const uint8_t *l_sid,
    const uint8_t *nid_consumer, const uint8_t *pids, uint32_t pids_len)
{
    const uint32_t pids_hash = calculate_pids_hash(pids, pids_len);
    if (nid_provider == NULL) { // 16 bytes
        memset(key, 0, SIZE_NID);
    } else {
        rte_memcpy(key, nid_provider, SIZE_NID);
    }
    rte_memcpy(key + SIZE_NID, l_sid, SIZE_LSID);
    rte_memcpy(key + SIZE_NID + SIZE_LSID, nid_consumer, SIZE_NID);
    *(uint64_t *)(key + SIZE_NID * 2 + SIZE_LSID) = pids_hash;
}

void odc_timer_cb(__attribute__((unused)) struct rte_timer *timer, void *arg);

/**
 * @brief Search the ODC table. This operation is multi-thread safe.
 * The caller must NOT modifiy the returned ODC entry.
 *
 * @param odc_table pointer to the ODC table
 * @param nid_provider pointer to the NID of provider
 * @param l_sid pointer to LSID
 * @param nid_consumer pointer to the NID of consumer
 * @param pids pointer to the PID sequence
 * @param pids_len size of the PID sequence in bytes
 * @param p pointer to return the ODC entry
 * @return int \p FAILED if the ODC entry cannot be found else \p OK
 */
static inline int odc_search(struct odc_table *odc_table, const uint8_t *nid_provider,
    const uint8_t *l_sid, const uint8_t *nid_consumer, const uint8_t *pids,
    uint32_t pids_len, struct odc_entry const **p)
{
    struct odc_entry *odc_entry;
    uint8_t key[ODC_TABLE_KEY_LEN];
    odc_key(key, nid_provider, l_sid, nid_consumer, pids, pids_len);
    rte_rwlock_read_lock(&odc_table->lock);
    int32_t ret = rte_hash_lookup_data(odc_table->ht, key, (void **)&odc_entry);
    if (ret < 0) {
        rte_rwlock_read_unlock(&odc_table->lock);
        return FAILED;
    }
    rte_rwlock_read_unlock(&odc_table->lock);
    *p = odc_entry;
    rte_rwlock_write_lock(&odc_table->lock);
    /* Reset timer. The callback function is multi-thread safe and can be called
     * on different lcores. */
    const uint64_t ticks = rte_get_timer_hz() * ODC_ENTRY_EXPIRE_SECONDS;
    const unsigned int lcore_id = rte_lcore_id();
    rte_timer_reset_sync(&odc_entry->timer, ticks, SINGLE, lcore_id, odc_timer_cb, odc_entry->key);
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
char *odc_table_to_string(struct odc_table *odc_table);

#endif