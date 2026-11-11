#ifndef __ARP_PENDING_TABLE_H__
#define __ARP_PENDING_TABLE_H__

#include <stdint.h>
#include <rte_timer.h>
#include <rte_ether.h>
#include <rte_cycles.h>
#include <rte_mbuf.h>
#include <rte_lcore.h>
#include <rte_rwlock.h>
#include <rte_ip.h>
#include "../log.h"
#include "../common.h"
#include "../utils/utils.h"
#include "../br_conf.h"
#include "../send_pkt.h"

#define ARP_PENDING_MILLISECONDS (100)

/* packets waiting for ARP response
 * valid = 0: this entry is not valid; next > 0, this entry is valid.
 * To find a valid entry, you need to loop through arp_pending_table->
 */
#define MAX_NUM_ARP_PENDING 16

struct arp_pending_table;
struct arp_pending_entry {
    uint32_t valid;
    struct rte_mbuf *pkt;
    struct br_conf *br_conf;
    uint32_t next_hop;
    struct rte_timer timer;
    struct arp_pending_table *owner;
};

struct arp_pending_table {
    rte_rwlock_t lock;
    struct arp_pending_entry entries[MAX_NUM_ARP_PENDING];
};

/* ARP pending table timer callback. This operation is multi-thread safe. */
static void
arp_pending_timer_cb(__attribute__((unused)) struct rte_timer *timer, void *arg)
{
    struct arp_pending_entry *p = (struct arp_pending_entry *)arg;
    struct arp_pending_table *arp_pending_table = p->owner;
    rte_rwlock_write_lock(&arp_pending_table->lock);

    /* Log */
    struct rte_ipv4_hdr *ipv4_hdr = rte_pktmbuf_mtod_offset(
        p->pkt, struct rte_ipv4_hdr *, sizeof(struct rte_ether_hdr));
    BR_LOG("ARP pending entry timeout. Dst IP address: %d.%d.%d.%d\n",
        IPv4TO4DECIMAL(rte_be_to_cpu_32(ipv4_hdr->dst_addr)));

    // No ARP reply received. discard the packet or send it out with broadcast
    // MAC address (for debugging).
    if (1) {
        struct rte_ether_hdr *eth_hdr = rte_pktmbuf_mtod(p->pkt, struct rte_ether_hdr *);
        memset(&eth_hdr->dst_addr, 0xFF, RTE_ETHER_ADDR_LEN);
        send_single_packet(p->br_conf, p->pkt);
        BR_LOG("Packet is sent with broadcast MAC address.\n");
    } else {
        rte_pktmbuf_free(p->pkt);
    }
    rte_timer_stop_sync(&p->timer);
    p->valid = 0;
    rte_rwlock_write_unlock(&arp_pending_table->lock);
}

/**
 * Add a packet waiting for ARP response to ARP pending table.
 * This operation is multi-thread safe.
 *
 * @param dst_portid
 * 	 destination port ID for this packet.
 * @param pkt
 *   The pointer to the mbuf of the packet to be buffered.
 * @return
 *   - OK if the packet is added to ARP pending table.
 *   - FAILED if there is no space for the packet.
 */
static inline int
arp_pending_add_entry(struct arp_pending_table *arp_pending_table, struct br_conf *br_conf,
    struct rte_mbuf *pkt, uint32_t next_hop)
{
    rte_rwlock_write_lock(&arp_pending_table->lock);
    for (int i = 0; i < MAX_NUM_ARP_PENDING; i++) {
        if (!arp_pending_table->entries[i].valid) {
            arp_pending_table->entries[i].valid = 1;
            arp_pending_table->entries[i].pkt = pkt;
            arp_pending_table->entries[i].br_conf = br_conf;
            arp_pending_table->entries[i].next_hop = next_hop;
            arp_pending_table->entries[i].owner = arp_pending_table;
            /* Setup timer
            * The callback function is multi-thread safe and can be called on different
            * lcores. */
            struct rte_timer *timer = &arp_pending_table->entries[i].timer;
            rte_timer_init(timer);
            uint64_t ticks = rte_get_timer_hz() * ARP_PENDING_MILLISECONDS / 1000;
            unsigned int lcore_id = rte_lcore_id();
            rte_timer_reset_sync(timer, ticks, SINGLE, lcore_id,
                arp_pending_timer_cb, (void *)&arp_pending_table->entries[i]);
            rte_rwlock_write_unlock(&arp_pending_table->lock);
            // Remember pkt is not freed yet. Be careful.
            // pkt ownership is transfered to arp_pending_table
            return OK;
        }
    }
    rte_rwlock_write_unlock(&arp_pending_table->lock);

    // No space, should free the packet.
    rte_pktmbuf_free(pkt);
    BR_LOG("No space for ARP pending entry.\n");
    return FAILED;
}

#endif