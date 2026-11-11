#include "port_table.h"
#include <rte_malloc.h>

/**
 * @brief Initialize port table.
 * Allocate space for the QS variable.
 * Set all elements of the ports array to NULL.
 *
 * @param port_table pointer to the port table
 */
void port_table_init(struct port_table *port_table)
{
    /* Initialize writer's lock */
    rte_rwlock_init(&port_table->lock);

    /* Allocate space for QS variable */
    size_t sz = rte_rcu_qsbr_get_memsize(RTE_MAX_LCORE);
    port_table->qsv = (struct rte_rcu_qsbr *)rte_zmalloc("rte_rcu_qsbr", sz, RTE_CACHE_LINE_SIZE);
    ASSERT_NOT_NULL(port_table->qsv, "Unable to initialize the port table.\n");
    rte_rcu_qsbr_init(port_table->qsv, RTE_MAX_LCORE);

    /* Set all elements of the ports array to NULL. */
    memset(port_table->ports, 0, sizeof(port_table->ports));
}

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
int port_add(struct port_table *port_table, uint16_t portid,
    uint32_t ipv4_addr, uint8_t prefix_len)
{
    if (portid >= RTE_MAX_ETHPORTS) {
        BR_INFO("Port id should be less than %d.\n", RTE_MAX_ETHPORTS);
        return FAILED;
    }

    rte_rwlock_write_lock(&port_table->lock);
    struct port_entry *p_old = port_table->ports[portid];
    struct port_entry *p = rte_zmalloc("rte_rcu_qsbr", sizeof(struct port_entry), 0);
    p->ipv4_addr = ipv4_addr;
    p->prefix_len = prefix_len;
    p->portid = portid;
    __atomic_store_n(&port_table->ports[portid], p, __ATOMIC_RELEASE);
    rte_rcu_qsbr_synchronize(port_table->qsv, rte_lcore_id());
    if (p_old) {
        rte_free(p_old);
    }
    rte_rwlock_write_unlock(&port_table->lock);
    return OK;
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
int port_match_ip(struct port_table *port_table, uint32_t ipv4_addr, uint16_t *portid)
{
    rte_rcu_qsbr_lock(port_table->qsv, rte_lcore_id());
    for (int i = 0; i < RTE_MAX_ETHPORTS; ++i) {
        struct port_entry *p = port_table->ports[i];
        if (!p) {
            continue;
        }
        uint32_t mask = IPPREFIX2MASK(p->prefix_len);
        if ((p->ipv4_addr & mask) == (ipv4_addr & mask)) {
            *portid = p->portid;
            rte_rcu_qsbr_unlock(port_table->qsv, rte_lcore_id());
            rte_rcu_qsbr_quiescent(port_table->qsv, rte_lcore_id());
            return OK;
        }
    }
    rte_rcu_qsbr_unlock(port_table->qsv, rte_lcore_id());
    rte_rcu_qsbr_quiescent(port_table->qsv, rte_lcore_id());
    return FAILED;
}

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
    uint32_t *ipv4_addr, uint8_t *prefix_len)
{
    if (portid >= RTE_MAX_ETHPORTS) {
        BR_LOGF("Port id should be less than %d.\n", RTE_MAX_ETHPORTS);
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
    *prefix_len = p->prefix_len;
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
char *port_config_string(struct port_table *port_table)
{
    StringBuilder *sb = new_sb();
    char *s = NULL;
    rte_rcu_qsbr_lock(port_table->qsv, rte_lcore_id());
    for (int i = 0; i < RTE_MAX_ETHPORTS; ++i) {
        struct port_entry *p = port_table->ports[i];
        if (!p) {
            continue;
        }
        char buf[128];
        sprintf(buf, "port %d ip address %d.%d.%d.%d/%d\r\n", p->portid,
            IPv4TO4DECIMAL(p->ipv4_addr), p->prefix_len);
        sb_append(sb, buf);
    }
    rte_rcu_qsbr_unlock(port_table->qsv, rte_lcore_id());
    rte_rcu_qsbr_quiescent(port_table->qsv, rte_lcore_id());
    s = sb_get(sb);
    free_sb(sb);
    return s;
}