#ifndef __BR_IP_H__
#define __BR_IP_H__

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
#include "../arp/arp.h"
#include "../br_conf.h"
#include "../log.h"
#include "../common.h"
#include "../br_tables.h"
#include "../send_pkt.h"

/**
 * Send a packet to IP address `dst_ipv4_addr`.
 * 1. Search IP routing table to find out the portid of destination port and the
 *    next hop IP address.
 * 2. Search the interface table to get the IP address of the output port.
 * 3. The source address field in IP header is set to the IP address of the output
 *    port. The destination address field in IP header is set to `dst_ipv4_addr`.
 * 4. Reset TTL, update checksum of IP header.
 * 5. ether_type field of ethernet header is set to ETHER_TYPE_IPv4.
 * 6. Set source MAC address to the hardware address of the output port.
 * 7. Search the ARP table. Set destination MAC address to the result of ARP table
 *    lookup and send the packet to dst port if searching succeeds. Otherwise,
 *    Send ARP request and add the packet to ARP pending queue.
 *
 * @param pkt
 *   The pointer to the mbuf of the packet to be forwarded.
 * @param dst_ipv4_addr
 *   The destination IP address of this packet.
 * @return
 *   dst port id if destination port is found, else return -1
 */
static inline int16_t send_pkt_to_dst_ip(struct rte_mbuf *pkt,
    uint32_t dst_ipv4_addr, struct br_conf *br_conf, struct br_tables *br_tables)
{
    struct rte_ether_hdr *eth_hdr;
    struct rte_ipv4_hdr *ipv4_hdr;
    struct rte_ipv6_hdr *ipv6_hdr;

    uint8_t src_ipv6_addr[16];
    uint8_t dst_ipv6_addr[16];
    struct rte_ether_addr *dst_hw_addr;

    /* Search IP routing table.*/
    uint16_t dst_portid;
    uint32_t next_hop;
    int ret = ip_lpm_search(br_tables->ip_lpm_table, dst_ipv4_addr, &dst_portid, &next_hop);
    if (ret == FAILED) {
        BR_LOG_L3("Cannot find route entry for: %d.%d.%d.%d\n",
            IPv4TO4DECIMAL(dst_ipv4_addr));
        rte_pktmbuf_free(pkt);
        return -1;
    }

    /* Get the source IP address */
    uint32_t src_ipv4_addr;
    ret = port_get_ip(&br_tables->port_table, dst_portid, &src_ipv4_addr);
    if (ret == FAILED) {
        BR_LOG_L3("Cannot find the IP address of port %d\n", dst_portid);
        rte_pktmbuf_free(pkt);
        return -1;
    }

    pkt->port = dst_portid;

    if (next_hop == src_ipv4_addr) {
        next_hop = dst_ipv4_addr;
    }
    BR_LOG_L3("Found output port: %d, next hop: %d.%d.%d.%d\n", dst_portid,
        IPv4TO4DECIMAL(next_hop));

    int is_IPv6;
    is_IPv6 = 0;
    for (int i = 0; i < br_tables->addr_num; i++) {

        if (src_ipv4_addr == br_tables->ipv4_addrs[i]) {
            is_IPv6 = 1;
            IPv6_copy(src_ipv6_addr, br_tables->ipv6_addrs[i]);
        }

        if (dst_ipv4_addr == br_tables->ipv4_addrs[i]) {
            is_IPv6 = 1;
            IPv6_copy(dst_ipv6_addr, br_tables->ipv6_addrs[i]);
            dst_hw_addr = &br_tables->hw_addrs[i];
        }
    }
    BR_LOG_L3("Whether it is an IPv6 packet: %d", is_IPv6);

    /* Set src and dest IP address */
    if (is_IPv6) {
        BR_LOG_L3("We need send ipv6 packet");
        ipv6_hdr = rte_pktmbuf_mtod_offset(pkt, struct rte_ipv6_hdr *,
            sizeof(struct rte_ether_hdr));
        IPv6_copy(ipv6_hdr->src_addr, src_ipv6_addr);
        IPv6_copy(ipv6_hdr->dst_addr, dst_ipv6_addr);
        
        /* Reset IP TTL */
        ipv6_hdr->hop_limits = IP_DEFTTL;

        /* Set MAC address */
        eth_hdr = rte_pktmbuf_mtod(pkt, struct rte_ether_hdr *);
        eth_hdr->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV6);
        rte_ether_addr_copy(&br_conf->ports_eth_addr[dst_portid], &eth_hdr->src_addr);
        rte_ether_addr_copy(dst_hw_addr, &eth_hdr->dst_addr);

    } else {

        ipv4_hdr = rte_pktmbuf_mtod_offset(pkt, struct rte_ipv4_hdr *,
            sizeof(struct rte_ether_hdr));
        ipv4_hdr->src_addr = rte_cpu_to_be_32(src_ipv4_addr);
        ipv4_hdr->dst_addr = rte_cpu_to_be_32(dst_ipv4_addr);
        
        /* Reset IP TTL */
        ipv4_hdr->time_to_live = IP_DEFTTL;

        /* Update IPv4 header checksum */
        ipv4_hdr->hdr_checksum = 0;
        if (br_conf->ipv4_cksum_tx_offload) {
            pkt->l2_len = sizeof(struct rte_ether_hdr); // in cacheline1
            pkt->l3_len = (ipv4_hdr->version_ihl & 0x0f) * 4; // in cacheline1
            pkt->ol_flags |= RTE_MBUF_F_TX_IP_CKSUM | RTE_MBUF_F_TX_IPV4;
        } else {
            ipv4_hdr->hdr_checksum = rte_ipv4_cksum(ipv4_hdr);
        }
        /* Set MAC address */
        eth_hdr = rte_pktmbuf_mtod(pkt, struct rte_ether_hdr *);
        eth_hdr->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);
        rte_ether_addr_copy(&br_conf->ports_eth_addr[dst_portid], &eth_hdr->src_addr);
        if (br_conf->arp_enabled) {
            ret = arp_search(&br_tables->arp_table, next_hop, &eth_hdr->dst_addr);
            if (ret == FAILED) {
                BR_LOG_L2("Cannot find MAC address for %d.%d.%d.%d in ARP table.\n",
                    IPv4TO4DECIMAL(next_hop));
                send_arp_request(br_conf, dst_portid, src_ipv4_addr,
                    &br_conf->ports_eth_addr[dst_portid], next_hop);
                arp_pending_add_entry(&br_tables->arp_pending_table, br_conf, pkt, next_hop);
                // pkt is not freed here, arp_pending_table should take care.
                return -1; // wait for ARP reply
            }
        } else {
            // use broadcast address
            memset(&eth_hdr->dst_addr, 0xFF, RTE_ETHER_ADDR_LEN);
        }
    }
    
    if (unlikely(br_conf->debugging_l2_on)) {
        char s_addr[32];
        char d_addr[32];
        rte_ether_format_addr(s_addr, 32, &eth_hdr->src_addr);
        rte_ether_format_addr(d_addr, 32, &eth_hdr->dst_addr);
        BR_LOG_L2("src MAC addr: %s, dst MAC addr: %s\n", s_addr, d_addr);
    }

    ret = send_single_packet(br_conf, pkt);

    return (ret == OK) ? dst_portid : -1;
}


#endif