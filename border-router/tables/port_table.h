#ifndef __INTERFACE_LIST_H__
#define __INTERFACE_LIST_H__

#include <stdint.h>
#include <stdbool.h>
#include <rte_rwlock.h>
#include <rte_rcu_qsbr.h>
#include "../utils/utils.h"
#include "../common.h"
#include "../log.h"

struct port_entry {
    uint16_t portid;
    uint8_t prefix_len; /* prefix length */
    uint32_t ipv4_addr; /* IP address of the port */
};

struct port_table {
    rte_rwlock_t lock; /* writer's lock, not used on readers */
    struct rte_rcu_qsbr *qsv;
    struct port_entry *ports[RTE_MAX_ETHPORTS]; /* A port entry is not valid  if it's NULL. */
};

/**
 * @brief Initialize port table.
 * Allocate space for the QS variable.
 * Set all elements of the ports array to NULL.
 *
 * @param port_table pointer to the port table
 */
void port_table_init(struct port_table *port_table);

/**
 * @brief Add/update IP address configuration of a port.
 * This operation is multi-thread safe.
 *
 * @param port_table pointer to the port table
 * @param portid id of the port to update
 * @param ipv4_addr IP address of the port
 * @param prefix_len prefix length
 * @return int returns OK if the port is added successfully else FAILED
 */
int port_add(struct port_table *port_table, uint16_t portid, uint32_t ipv4_addr,
    uint8_t prefix_len);

/**
 * @brief Returns the IPv4 address associated with the specified port.
 * This operation is multi-thread safe.
 *
 * @param port_table pointer to the port table
 * @param portid id of the port to query
 * @param ipv4_addr pointer to the space for returning result
 * @return int
 *   - OK if the port is found.
 *   - FAILED if the port is not in interface list.
 */
static inline int port_get_ip(struct port_table *port_table, uint16_t portid,
    uint32_t *ipv4_addr)
{
    if (portid >= RTE_MAX_ETHPORTS) {
        BR_LOG("Port id should be less than %d.\n", RTE_MAX_ETHPORTS);
        return FAILED;
    }
    unsigned int lcore_id = rte_lcore_id();
    rte_rcu_qsbr_lock(port_table->qsv, lcore_id);
    struct port_entry *p = port_table->ports[portid];
    if (!p) {
        BR_LOGF("Port %d is not valid.\n", portid);
        rte_rcu_qsbr_unlock(port_table->qsv, lcore_id);
        rte_rcu_qsbr_quiescent(port_table->qsv, lcore_id);
        return FAILED;
    }
    *ipv4_addr = p->ipv4_addr;
    rte_rcu_qsbr_unlock(port_table->qsv, lcore_id);
    rte_rcu_qsbr_quiescent(port_table->qsv, lcore_id);
    return OK;
}

/**
 * @brief Returns configuration string of the port table.
 *
 * @param port_table pointer to the port table
 * @return char*
 */
char *port_config_string(struct port_table *port_table);

/**
 * @brief Check if \p ipv4_addr is in the port table.
 *
 * @param port_table pointer to the port table
 * @param ipv4_addr
 * @return true if \p ipv4_addr is in the port table
 * @return false if \p ipv4_addr is not the port table
 */
static inline bool port_has_ip(struct port_table *port_table, uint32_t ipv4_addr)
{
    rte_rcu_qsbr_lock(port_table->qsv, rte_lcore_id());
    for (int i = 0; i < RTE_MAX_ETHPORTS; ++i) {
        struct port_entry *p = port_table->ports[i];
        if (p && p->ipv4_addr == ipv4_addr) {
            rte_rcu_qsbr_unlock(port_table->qsv, rte_lcore_id());
            rte_rcu_qsbr_quiescent(port_table->qsv, rte_lcore_id());
            return true;
        }
    }
    rte_rcu_qsbr_unlock(port_table->qsv, rte_lcore_id());
    rte_rcu_qsbr_quiescent(port_table->qsv, rte_lcore_id());
    return false;
}

/**
 * @brief Determine the ID of the port where \p ipv4_addr is in the same net as
 * that port. The port should satisfy the following condition:
 * (IP address of the port & mask) == (ipv4_addr & mask)
 *
 * @param port_table pointer to the port table
 * @param ipv4_addr IP address to match
 * @param portid pointer to store the ID of the port
 * @return int returns OK if a port is found else FAILED
 */
int port_match_ip(struct port_table *port_table, uint32_t ipv4_addr, uint16_t *portid);


/**
 * @brief Returns the IP prefix associated with the specified port.
 * This operation is multi-thread safe.
 *
 * @param port_table pointer to the port table
 * @param portid id of the port to query
 * @param ipv4_addr pointer to the space for returning result
 * @param prefix_len pointer to store prefix_len of the port
 * @return int
 *   - OK if the port is found.
 *   - FAILED if the port is not in port list.
 */
int port_get_ip_prefix(struct port_table *port_table, uint16_t portid,
    uint32_t *ipv4_addr, uint8_t *prefix_len);

#endif