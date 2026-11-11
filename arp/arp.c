#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>

#include <rte_debug.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_arp.h>
#include <rte_ip.h>
#include <rte_malloc.h>
#include <rte_timer.h>
#include <rte_rwlock.h>
#include "arp.h"
#include "../br_conf.h"
#include "../log.h"
#include "../common.h"
#include "../br_tables.h"
#include "../send_pkt.h"

/**
 * Send an ARP request.
 */
void send_arp_request(struct br_conf *br_conf, uint16_t portid,
    uint32_t sender_ipv4_addr, struct rte_ether_addr *sender_hw_addr,
    uint32_t target_ipv4_addr)
{
    struct rte_mbuf *pkt = rte_pktmbuf_alloc(br_conf->tx_pktmbuf_pool);
    if (pkt == NULL) {
        BR_LOG("Failed to allocate mbuf for ARP request\n");
        return;
    }
    size_t pkt_size = sizeof(struct rte_ether_hdr) + sizeof(struct rte_arp_hdr);
    pkt->data_len = pkt_size;
    pkt->pkt_len = pkt_size;
    pkt->port = portid;

    /* ethernet header */
    struct rte_ether_hdr *eth_hdr = rte_pktmbuf_mtod(pkt, struct rte_ether_hdr *);
    rte_ether_addr_copy(sender_hw_addr, &eth_hdr->src_addr);
    memset(&eth_hdr->dst_addr, 0xFF, RTE_ETHER_ADDR_LEN);
    eth_hdr->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_ARP);

    /* ARP header */
    struct rte_arp_hdr *arp_hdr = (struct rte_arp_hdr *)((char *)eth_hdr +
        sizeof(struct rte_ether_hdr));
    arp_hdr->arp_hardware = rte_cpu_to_be_16(RTE_ARP_HRD_ETHER);
    arp_hdr->arp_protocol = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);
    arp_hdr->arp_hlen = RTE_ETHER_ADDR_LEN;
    arp_hdr->arp_plen = sizeof(uint32_t);
    arp_hdr->arp_opcode = rte_cpu_to_be_16(RTE_ARP_OP_REQUEST);

    /* ARP data */
    arp_hdr->arp_data.arp_sip = rte_cpu_to_be_32(sender_ipv4_addr);
    arp_hdr->arp_data.arp_tip = rte_cpu_to_be_32(target_ipv4_addr);
    rte_ether_addr_copy(sender_hw_addr, &arp_hdr->arp_data.arp_sha);
    memset(&arp_hdr->arp_data.arp_tha, 0, RTE_ETHER_ADDR_LEN);

    if (unlikely(br_conf->debugging_l2_on)) {
        char s_addr[32];
        char t_addr[32];
        rte_ether_format_addr(s_addr, 32, &arp_hdr->arp_data.arp_sha);
        rte_ether_format_addr(t_addr, 32, &arp_hdr->arp_data.arp_tha);
        BR_LOG_L2("Sending ARP request: port id: %d, sender MAC address: %s, "
            "sender IP address: %d.%d.%d.%d, target MAC address: %s, target IP address:"
            "%d.%d.%d.%d\n", portid, s_addr, IPv4TO4DECIMAL(sender_ipv4_addr),
            t_addr, IPv4TO4DECIMAL(target_ipv4_addr));
    }

    send_single_packet(br_conf, pkt);
    return;
}

/**
 * Inspect each pending packet and see if they can be sent out (by searching
 * ARP table).
 * This operation is multi-thread safe.
 */
static inline void
arp_pending_flush(struct br_conf *br_conf, struct br_tables *br_tables)
{
    struct arp_pending_table *arp_pending_table = &br_tables->arp_pending_table;
    rte_rwlock_write_lock(&arp_pending_table->lock);

    for (int i = 0; i < MAX_NUM_ARP_PENDING; i++) {
        if (!arp_pending_table->entries[i].valid) {
            continue;
        }

        // find a valid entry
        struct arp_pending_entry *p = &arp_pending_table->entries[i];
        struct rte_ether_hdr *eth_hdr = rte_pktmbuf_mtod(p->pkt, struct rte_ether_hdr *);
        int ret = arp_search(&br_tables->arp_table, p->next_hop, &eth_hdr->dst_addr);
        if (ret == FAILED) {
            continue;
        }
        if (unlikely(br_conf->debugging_l2_on)) {
            char s_addr[32];
            char d_addr[32];
            rte_ether_format_addr(s_addr, 32, &eth_hdr->src_addr);
            rte_ether_format_addr(d_addr, 32, &eth_hdr->dst_addr);
            BR_LOG_L2("Send ARP pending packet: src MAC addr: %s, dst MAC addr: %s\n",
                s_addr, d_addr);
        }
        send_single_packet(br_conf, p->pkt);
        rte_timer_stop_sync(&p->timer);
        p->valid = 0;
    }

    rte_rwlock_write_unlock(&arp_pending_table->lock);
}

/**
 * ARP reply.
 *
 * Pre-conditions: The packet *pkt* is an ARP packet. The behavior of calling
 * this function with non-ARP packets is not defined.
 *
 * @param pkt
 *   The pointer to the mbuf of the packet to be processed.
 */
void process_arp_packet(struct rte_mbuf *pkt, struct br_conf *br_conf, struct br_tables *br_tables)
{
    uint16_t portid = pkt->port;
    struct rte_ether_hdr *eth_hdr = rte_pktmbuf_mtod(pkt, struct rte_ether_hdr *);
    struct rte_arp_hdr *arp_hdr =
        (struct rte_arp_hdr *)((uint8_t *)eth_hdr + sizeof(struct rte_ether_hdr));

    uint32_t ipv4_addr;
    int ret = port_get_ip(&br_tables->port_table, portid, &ipv4_addr);
    if (ret == FAILED) {
        BR_LOG_L2("ARP: Cannot find the IP address of port %d\n", portid);
        rte_pktmbuf_free(pkt);
        return;
    }

    printf("Start to process arp packet\r\n");

    if (arp_hdr->arp_data.arp_tip == rte_cpu_to_be_32(ipv4_addr) ||
        arp_hdr->arp_data.arp_sip == arp_hdr->arp_data.arp_tip
    ) {
        BR_LOG_L2("Received an ARP packet, opcode = %d\n",
            rte_cpu_to_be_16(arp_hdr->arp_opcode));
        if (arp_hdr->arp_opcode == rte_cpu_to_be_16(RTE_ARP_OP_REQUEST)) {
            /* Add sender's MAC address to ARP table. */
            arp_add(&br_tables->arp_table, rte_be_to_cpu_32(arp_hdr->arp_data.arp_sip), &arp_hdr->arp_data.arp_sha);

            arp_hdr->arp_opcode = rte_cpu_to_be_16(RTE_ARP_OP_REPLY);

            /* Set MAC addresses of ethernet header. */
            rte_ether_addr_copy(&eth_hdr->src_addr, &eth_hdr->dst_addr);
            rte_eth_macaddr_get(portid, &eth_hdr->src_addr);

            /* Set target and sender hardware addresses. */
            rte_ether_addr_copy(&arp_hdr->arp_data.arp_sha, &arp_hdr->arp_data.arp_tha);
            rte_eth_macaddr_get(portid, &arp_hdr->arp_data.arp_sha);

            /* Switch target and sender IP addresses. */
            arp_hdr->arp_data.arp_tip = arp_hdr->arp_data.arp_sip;
            arp_hdr->arp_data.arp_sip = rte_cpu_to_be_32(ipv4_addr);

            pkt->port = portid;
            send_single_packet(br_conf, pkt);
            if (unlikely(br_conf->debugging_on)) {
                char s_addr[32];
                char t_addr[32];
                rte_ether_format_addr(s_addr, 32, &arp_hdr->arp_data.arp_sha);
                rte_ether_format_addr(t_addr, 32, &arp_hdr->arp_data.arp_tha);
                BR_LOG_L2("Sending ARP reply: port id: %d, MAC address: %s, "
                    "IP address: %d.%d.%d.%d, target MAC address: %s, target IP address:"
                    "%d.%d.%d.%d\n", portid, s_addr, IPv4TO4DECIMAL(ipv4_addr),
                    t_addr, IPv4TO4DECIMAL(rte_be_to_cpu_32(arp_hdr->arp_data.arp_tip)));
            }
        } else if (arp_hdr->arp_opcode == rte_cpu_to_be_16(RTE_ARP_OP_REPLY)) {
            /* Add sender's MAC address to ARP table. */
            arp_add(&br_tables->arp_table, rte_be_to_cpu_32(arp_hdr->arp_data.arp_sip),
                &arp_hdr->arp_data.arp_sha);
            arp_pending_flush(br_conf, br_tables);
            rte_pktmbuf_free(pkt);
        } else { // ARP packets of other types except ARP request are not handled.
            
            rte_pktmbuf_free(pkt);
        }
    } else { // target IPv4 address not matched
#ifdef DEBUG_BR_VERBOSE
        BR_LOG_L2("Port %d IP address: %d.%d.%d.%d, and target IP address: "
            "%d.%d.%d.%d do not match\n", portid, IPv4TO4DECIMAL(ipv4_addr),
            IPv4TO4DECIMAL(rte_be_to_cpu_32(arp_hdr->arp_data.arp_tip)));
#endif
        rte_pktmbuf_free(pkt);
    }
}