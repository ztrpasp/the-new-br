#ifndef __GET_H__
#define __GET_H__

#include <stdint.h>
#include <rte_byteorder.h>
#include <rte_mbuf.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include "rte_CoLoR.h"
#include "../common.h"
#include "../profiler/profiler.h"
#include "../br_conf.h"
#include "../br_tables.h"

/**
 * Check whether the length indicated by the field pkg_len is consistent with
 * the actual length indicated by field PID_num, flag K, Q, S.
 *
 * Pre-conditions:
 * 1. `get_hdr` is a valid pointer to a GET packet of size pkt_len.
 * 2. get_hdr->pkg_len == pkt_len
 *
 * @param get_hdr
 *   the pointer to a GET packet
 * @param pkt_len
 *   the length of the GET packet
 * @returns
 *   - OK if the actual length of the GET packet is not consistent with the
 *     length indicated by the content of the packet, otherwise
 *   - FAILED
 */
static inline int
check_get_length(GET_front_t *get_hdr, uint16_t pkt_len)
{
    uint16_t len = 64;	// from version to NID: 64 bytes
    ASSERT_NOT_LESS_THAN(pkt_len, len);
    uint8_t flag = get_hdr->flag;
    if (COLOR_GET_FLAG_K(flag)) {
        ASSERT_NOT_LESS_THAN(pkt_len, len + 2);
        uint16_t pklen = rte_le_to_cpu_16(*(uint16_t *)((uint8_t *)get_hdr + len));
        len += 2 + pklen;
    }
    if (COLOR_GET_FLAG_Q(flag)) {
        ASSERT_NOT_LESS_THAN(pkt_len, len + 1);
        uint8_t qoslen = *((uint8_t *)get_hdr + len);
        len += 1 + qoslen;
    }
    if (COLOR_GET_FLAG_S(flag)) {
        len += 4; // seg_id
    }
    ASSERT_NOT_LESS_THAN(get_hdr->PID_num, 1); // should not be zero
    len += get_hdr->PID_num * 4;
    ASSERT_EQUAL(pkt_len, len);
    return OK;
}

/**
 * Process the GET packet.
 * This function verifies the checksum field of the GET packet, determines the
 * destination IPv4 address for this GET packet. The destination IPv4 address
 * is returned by parameter *dst_ipv4_addr*.
 * If the packet should be dropped, this function returns BAD_PACKET, and the
 * content of *dst_ipv4_addr* is left unmodified.
 *
 * Pre-conditions:
 * *pkt* points to an GET packet. *dst_ipv4_addr* is a valid pointer.
 *
 * @param pkt
 *   The pointer to the mbuf of the packet to be forwarded.
 * @param dst_ipv4_addr
 *   The pointer to an IPv4 address. The destination address is returned by this
 *   parameter.
 * @return
 *   - BAD_PACKET to indicate the packet should be dropped.
 *   - GOOD_PACKET if the destination address is found successfully.
 */
static inline int
process_get_packet(struct rte_mbuf *pkt, uint32_t *dst_ipv4_addr,
    struct br_conf *br_conf, struct br_tables *br_tables)
{
    PROFILER_BEGIN(process_get_pkt);

    void *color_hdr;
    struct rte_ether_hdr * eth_hdr;
    uint32_t offset = sizeof(struct rte_ether_hdr);

    eth_hdr = rte_pktmbuf_mtod(pkt, struct rte_ether_hdr *);

    if (eth_hdr->ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4)) {
        offset += sizeof (struct rte_ipv4_hdr);
    } else if (eth_hdr->ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV6)) {
        offset += sizeof (struct rte_ipv6_hdr);
    } else {
        BR_LOG("received an ANN packet with unsupported layer 3 protocol checksum\n");
        return FREE_THIS_PACKET;
    }
    color_hdr = rte_pktmbuf_mtod_offset(pkt, void *, offset);
    if (br_conf->debugging_on)
        CoLoR_show_pkg((uint8_t *)color_hdr);

    GET_front_t *get_hdr = (GET_front_t *)color_hdr;

    /* Check length consistency. From version to nid field: 64 bytes. */
    PROFILER_BEGIN(pkt_len_check);
    if (rte_pktmbuf_pkt_len(pkt) < 64 ||
            offset + rte_le_to_cpu_16(get_hdr->pkg_len) != rte_pktmbuf_pkt_len(pkt)) {
        BR_LOG("received a GET packet with incorrect packet length\n");
        return FREE_THIS_PACKET;
    }
    uint16_t packet_length = rte_le_to_cpu_16(get_hdr->pkg_len);
    int ret = check_get_length(get_hdr, packet_length);
    if (ret == FAILED) {
        BR_LOG("received a GET packet with invalid length: %d bytes.\n",
            packet_length);
        return FREE_THIS_PACKET;
    }
    PROFILER_END(pkt_len_check);

    /* Checksum */
#ifndef NO_GET_CKSUM
    PROFILER_BEGIN(get_cksum);
    uint16_t cksum = rte_raw_cksum((void *)get_hdr, packet_length);
    PROFILER_END(get_cksum);
    if (cksum != 0xFFFF) {
        BR_LOG("received a GET packet with incorrect checksum\n");
        return FREE_THIS_PACKET;
    }
#endif

    /* Make sure TTL > 0 and TTL - 1 > 0. */
    if (get_hdr->ttl <= 1) {
        BR_LOG("received a GET packet with TTL = %d\n", get_hdr->ttl);
        return FREE_THIS_PACKET;
    }
    get_hdr->ttl = get_hdr->ttl - 1;

    /* F = 1, the GET packet is from another AS. */
    uint8_t flag = get_hdr->flag;
    get_hdr->flag = COLOR_GET_REV_FLAG_F(flag);

    if (COLOR_GET_FLAG_F(flag)) {
        *dst_ipv4_addr = br_conf->rm_ip_addr;
    } else { /* F = 0, the GET packet is sent to another BR. */
        uint32_t pid = *(uint32_t *)((uint8_t *)get_hdr + packet_length - 4);
        pid = rte_le_to_cpu_32(pid);
        PROFILER_BEGIN(get_px_search);
        int ret = px_search_ipv4_addr(br_tables->px_table, pid, dst_ipv4_addr);
        PROFILER_END(get_px_search);
        if (ret == FAILED) {
            BR_LOG("Destination IPv4 address for PID = 0x%08X is not found.\n", pid);
            return FREE_THIS_PACKET;
        }
    }

    /* Checksum */
    get_hdr->checksum = 0;

#ifndef NO_GET_CKSUM
    {
        PROFILER_BEGIN(get_cksum);
        get_hdr->checksum = ~rte_raw_cksum((void *)get_hdr, packet_length);
        PROFILER_END(get_cksum);
    }
#endif

    BR_LOG("Processed GET packet: TLL = %d, flag = %#04x\n", get_hdr->ttl,
        get_hdr->flag);
    PROFILER_END(process_get_pkt);

    return GOOD_PACKET;
}

#endif