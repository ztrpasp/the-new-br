#ifndef __BR_H__
#define __BR_H__

#include <stdint.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_mbuf.h>
#include "br_conf.h"
#include "br_tables.h"
#include "packet/ann.h"
#include "packet/get.h"
#include "packet/data.h"
#include "packet/ctrl.h"
#include "profiler/profiler.h"
#include "ip/ip.h"
#include "arp/arp.h"

/* Configure how many packets ahead to prefetch, when reading packets */
#define PREFETCH_OFFSET	  3

/* Used to mark destination port as 'invalid'. */
#define	BAD_PORT ((uint16_t)-1)

/**
 * Process CoLoR packets, invoked from process_single_packet. This function
 * inspects the packet in mbuf *pkt*, decides to which port the packet should
 * be forwarded and send the packets to the out interface(port) if the packet
 * is not dropped, or process the control information if the packet is a CONTROL
 * packet.
 *
 * Pre-conditions: The packet *pkt* is a CoLoR packet (DATA, ANN, GET or CONTROL).
 * The behavior of calling this function with non-CoLoR packets is not defined.
 *
 * @param pkt
 *   The pointer to the mbuf of the packet.
 */
static inline void process_color_packet(struct rte_mbuf *pkt,
    struct br_conf *br_conf, struct br_tables *br_tables)
{
    int ret = 0;
    uint32_t dst_ipv4_addr = 0;
    int16_t dst_portid;

    struct rte_ether_hdr * eth_hdr;
    eth_hdr = rte_pktmbuf_mtod(pkt, struct rte_ether_hdr *);
    int is_IPv6 = (eth_hdr->ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV6)) ? 1 : 0;

    void *color_hdr = rte_pktmbuf_mtod_offset(pkt, void *,
        sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr));
    int color_type = GET_PACKAGE_TYPE(*(uint8_t *)color_hdr);
    struct stats_table *stats_table = &br_tables->stats_table[rte_lcore_id()];
    stats_inc(stats_table, pkt->port, color_type, rx, pkt->pkt_len);
    switch (color_type)
    {
    case COLOR_ANN_TYPE:
        ret = process_ann_packet(pkt, &dst_ipv4_addr, br_conf, br_tables);
        break;

    case COLOR_GET_TYPE:
        ret = process_get_packet(pkt, &dst_ipv4_addr, br_conf, br_tables);
        break;

    case COLOR_DATA_TYPE:
        printf("received a color data packet, is IPv6:%d\r\n", is_IPv6);
        ret = process_data_packet(pkt, &dst_ipv4_addr, br_conf, br_tables);
        if (ret & ODC_FAILED_PACKET) {
            send_odc_warning_to_all_proxies(pkt, br_conf, br_tables);
        }
        break;

    case COLOR_CONTROL_TYPE:
        printf("received a color control packet, is IPv6:%d\r\n", is_IPv6);
        if (is_IPv6) {
            rte_pktmbuf_free(pkt);
            return;
        }
        ret = process_control_packet(pkt, br_conf, br_tables);
        if (ret & FREE_THIS_PACKET) {
            rte_pktmbuf_free(pkt);
        }
        return; // DO NOT forward control packet.

    default:
        BR_LOG("Unsupported CoLoR packet type: %d.\n", color_type);
        rte_pktmbuf_free(pkt);
        return; // Unknown type, DO NOT forward this packet.
    }

    if (ret & FREE_THIS_PACKET) {
        BR_LOG("A CoLoR packet is discarded!\n");
        rte_pktmbuf_free(pkt);
    } else {
        BR_LOG("Destination IPv4 address: %d.%d.%d.%d\n",
            IPv4TO4DECIMAL(dst_ipv4_addr));
        PROFILER_BEGIN(send_ip);
        dst_portid = send_pkt_to_dst_ip(pkt, dst_ipv4_addr, br_conf, br_tables);
        PROFILER_END(send_ip);
        if (dst_portid >= 0) {
            stats_inc(stats_table, dst_portid, color_type, tx, pkt->pkt_len);
        }
    }
}

/**
 * Process a single packet. May be invoked from main_loop. This function the
 * packet in mbuf *pkt*, decides to which port the packet should be forwarded
 * or if the packet should be dropped or else(respond ARP request...).
 *
 * @param pkt
 *   The pointer to the mbuf of the packet to be forwarded.
 */
static inline void process_single_packet(struct rte_mbuf *pkt,
    struct br_conf *br_conf, struct br_tables *br_tables)
{
    struct rte_ether_hdr *eth_hdr;
    struct rte_ipv4_hdr *ipv4_hdr;
    struct rte_ipv6_hdr *ipv6_hdr;
    
    int check_dst_ipv4_addr = 1;
    uint16_t portid = pkt->port;

    /* MAC address */
    eth_hdr = rte_pktmbuf_mtod(pkt, struct rte_ether_hdr *);
    // printf("packet eht_hdr type:0x%02x%02x\r\n", eth_hdr->ether_type >> 8, eth_hdr->ether_type & 0x00FF);
    if (!rte_is_same_ether_addr(&eth_hdr->dst_addr, &br_conf->ports_eth_addr[portid]) &&
        !rte_is_broadcast_ether_addr(&eth_hdr->dst_addr)) {
#ifdef DEBUG_BR_VERBOSE
        char dst_mac_addr[32];
        char port_mac_addr[32];
        rte_ether_format_addr(dst_mac_addr, 32, &eth_hdr->dst_addr);
        rte_ether_format_addr(port_mac_addr, 32, &br_conf->ports_eth_addr[portid]);
        BR_LOG_L2("Received a packet of dst MAC addr: %s, MAC address "
            "of port %d is %s, packet discarded\n", dst_mac_addr, portid,
            port_mac_addr);
#endif
        rte_pktmbuf_free(pkt);
        return;
    }

    if (eth_hdr->ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4)) {
        printf("received an ipv4 packet\r\n");
        ipv4_hdr = (struct rte_ipv4_hdr *)((uint8_t *)eth_hdr + sizeof(struct rte_ether_hdr));
        /* IP header checksum */
        if (br_conf->ipv4_cksum_rx_offload) {
            uint64_t rx_ol_flags = pkt->ol_flags;
            if ((rx_ol_flags & RTE_MBUF_F_RX_IP_CKSUM_MASK) == RTE_MBUF_F_RX_IP_CKSUM_BAD) {
                BR_LOG_L3("Bad IP checksum.\n");
                rte_pktmbuf_free(pkt);
                return;
            }
        } else { // calculate checksum by software
            uint16_t cksum = rte_raw_cksum((void *)ipv4_hdr, sizeof(struct rte_ipv4_hdr));
            if (cksum != 0xFFFF) {
                BR_LOG_L3("Bad IP checksum.\n");
                rte_pktmbuf_free(pkt);
                return;
            }
        }

        /* Destination IP address */
        if (check_dst_ipv4_addr) {
            uint32_t ipv4_addr = 0;
            port_get_ip(&br_tables->port_table, portid, &ipv4_addr);
            if (ipv4_hdr->dst_addr != rte_cpu_to_be_32(ipv4_addr) &&
                    ipv4_hdr->dst_addr != 0xFFFFFFFF) {
#ifdef DEBUG_BR_VERBOSE
                BR_LOG_L3("Received a packet of dst IP addr: %d.%d.%d.%d, "
                    "IP address of port %d is %d.%d.%d.%d, packet discarded.\n",
                    IPv4TO4DECIMAL(rte_be_to_cpu_32(ipv4_hdr->dst_addr)), portid,
                    IPv4TO4DECIMAL(ipv4_addr));
#endif
                rte_pktmbuf_free(pkt);
                return;
            }
        }

        int hdr_len = (ipv4_hdr->version_ihl & RTE_IPV4_HDR_IHL_MASK) * RTE_IPV4_IHL_MULTIPLIER;
        if (hdr_len == sizeof(struct rte_ipv4_hdr)) {
            if (ipv4_hdr->next_proto_id == IPPROTO_COLOR) {
                process_color_packet(pkt, br_conf, br_tables);
            } else {
#ifdef DEBUG_BR_VERBOSE
                BR_LOG("Non-CoLoR packets are not supported: "
                    "protocol number = %#04x.\n", ipv4_hdr->next_proto_id);
#endif
                rte_pktmbuf_free(pkt);
                return;
            }
        } else {
#ifdef DEBUG_BR_VERBOSE
            BR_LOG_L3("IPv4 extension header is not supported.\n");
#endif
            rte_pktmbuf_free(pkt);
            return;
        }
    } else if (eth_hdr->ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_ARP)) {
        //printf("received an arp packet\r\n");
        process_arp_packet(pkt, br_conf, br_tables);
    } else if (eth_hdr->ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV6)) {
        // printf("received an ipv6 packet\r\n");
        ipv6_hdr = (struct rte_ipv6_hdr *)((uint8_t *)eth_hdr + sizeof(struct rte_ether_hdr));
        if (ipv6_hdr->proto == IPPROTO_COLOR) {
                process_color_packet(pkt, br_conf, br_tables);
            } else {
                rte_pktmbuf_free(pkt);
                return;
            }
    } else {
#ifdef DEBUG_BR_VERBOSE
        BR_LOG_L2("Non-IPv4 packets or non-ARP packets are not supported: "
            "ether type = %#06x.\n", rte_be_to_cpu_16(eth_hdr->ether_type));
#endif
        rte_pktmbuf_free(pkt);
        return;
    }
}

/**
 * Process the received packets, invoked from main_loop. This function inspects
 * each packet in array *pkts_burst*, decides to which port a packet should be
 * forwarded or if a packet should be dropped or else(respond ARP request...).
 *
 * @param nb_rx
 *   Size of the array *pkts_burst*. The number of packets to be forwarded.
 * @param pkts_burst
 *   The array of packets to be forwarded. The size of *pkts_busrt* is specified
 *   by parameter *nb_rx*.
 */
static inline void process_packets(int nb_rx, struct rte_mbuf **pkts_burst,
    struct br_conf *br_conf, struct br_tables *br_tables)
{
    //printf("[lcf] received packet\n");
    PROFILER_BEGIN(process_pkt);
    int32_t j;

    /* Prefetch first packets */
    for (j = 0; j < PREFETCH_OFFSET && j < nb_rx; j++) {
        rte_prefetch0(&pkts_burst[j]->cacheline1); // tx offload field
#if defined(PREFETCH_NONE)
#elif defined(PREFETCH_ONE)
        rte_prefetch0(rte_pktmbuf_mtod(pkts_burst[j], void *));
#elif defined(PREFETCH_TWO)
        rte_prefetch0(rte_pktmbuf_mtod(pkts_burst[j], void *));
        rte_prefetch0(rte_pktmbuf_mtod_offset(pkts_burst[j], void *, 64));
#else
        rte_prefetch0(rte_pktmbuf_mtod(pkts_burst[j], void *));
        rte_prefetch0(rte_pktmbuf_mtod_offset(pkts_burst[j], void *, 64));
        rte_prefetch0(rte_pktmbuf_mtod_offset(pkts_burst[j], void *, 128));
#endif
    }

    /* Prefetch and forward already prefetched packets. */
    for (j = 0; j < (nb_rx - PREFETCH_OFFSET); j++) {
        rte_prefetch0(&pkts_burst[j]->cacheline1);
#if defined(PREFETCH_NONE)
#elif defined(PREFETCH_ONE)
        rte_prefetch0(rte_pktmbuf_mtod(pkts_burst[j], void *));
#elif defined(PREFETCH_TWO)
        rte_prefetch0(rte_pktmbuf_mtod(pkts_burst[j], void *));
        rte_prefetch0(rte_pktmbuf_mtod_offset(pkts_burst[j], void *, 64));
#else
        rte_prefetch0(rte_pktmbuf_mtod(pkts_burst[j], void *));
        rte_prefetch0(rte_pktmbuf_mtod_offset(pkts_burst[j], void *, 64));
        rte_prefetch0(rte_pktmbuf_mtod_offset(pkts_burst[j], void *, 128));
#endif
        process_single_packet(pkts_burst[j], br_conf, br_tables);
    }

    /* Forward remaining prefetched packets */
    for (; j < nb_rx; j++)
        process_single_packet(pkts_burst[j], br_conf, br_tables);
    PROFILER_END(process_pkt);
}

#endif  /* __BR_H__ */
