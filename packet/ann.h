#ifndef __ANN_H__
#define __ANN_H__

#include <stdint.h>
#include <rte_byteorder.h>
#include <rte_mbuf.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include "rte_CoLoR.h"
#include "../common.h"
#include "../br_conf.h"
#include "../br_tables.h"

/**
 * Check whether the length indicated by the field pkg_len is consistent with
 * the actual length indicated by field unit_num, px_num, flag K, P.
 *
 * Pre-conditions:
 * 1. `ann_hdr` is a valid pointer to a ANN packet of size pkt_len.
 * 2. ann_hdr->pkg_len == pkt_len
 *
 * @param ann_hdr
 *   the pointer to a ANN packet
 * @param pkt_len
 *   the length of the ANN packet
 * @returns
 *   - OK if the actual length of the ANN packet is not consistent with the
 *     length indicated by the content of the packet, otherwise
 *   - FAILED
 */
static inline int
check_ann_length(ANN_front_t *ann_hdr, uint16_t pkt_len)
{
    uint16_t len = 8;	// from version to PX_num: 8 bytes
    ASSERT_NOT_LESS_THAN(pkt_len, len);
    uint8_t flag = ann_hdr->flag;
    uint8_t unit_num = ann_hdr->unit_px_num >> 4;
    uint8_t px_num = ann_hdr->unit_px_num & 0x0F;
    // announce units
    for (int i = 0; i < unit_num; i++) {
        ASSERT_NOT_LESS_THAN(pkt_len, len + 2);
        uint8_t unit_len = *(uint8_t *)((uint8_t *)ann_hdr + len + 1);
        len += unit_len;
    }
    if (COLOR_ANN_FLAG_K(flag)) {
        ASSERT_NOT_LESS_THAN(pkt_len, len + 2);
        uint16_t pklen = rte_le_to_cpu_16(*(uint16_t *)((uint8_t *)ann_hdr + len));
        len += 2 + pklen;
    }
    if (COLOR_ANN_FLAG_P(flag)) {
        ASSERT_NOT_LESS_THAN(pkt_len, len + 1);
        uint8_t aplen = *((uint8_t *)ann_hdr + len);
        len += 1 + aplen;
    }
    ASSERT_NOT_LESS_THAN(px_num, 1); // should not be zero
    len += px_num * 2;
    ASSERT_EQUAL(pkt_len, len);
    return OK;
}

/**
 * Process the ANN packet.
 * This function verifies the checksum field of the ANN packet, determines the
 * destination IPv4 address for this ANN packet. The destination IPv4 address
 * is returned by parameter *dst_ipv4_addr*.
 * If the packet should be dropped, this function returns BAD_PACKET and the
 * content of *dst_ipv4_addr* is left unmodified.
 *
 * Pre-conditions:
 * *pkt* points to an ANN packet. *dst_ipv4_addr* is a valid pointer.
 *
 * @param pkt
 *   The pointer to the mbuf of the packet to be forwarded.
 * @param dst_ipv4_addr
 *   The pointer to an IPv4 address. The destination address is return by this
 *   parameter.
 * @return
 *   - BAD_PACKET to indicate the packet should be dropped.
 *   - GOOD_PACKET if the destination address is found successfully.
 */
static inline int
process_ann_packet(struct rte_mbuf *pkt, uint32_t *dst_ipv4_addr, const struct br_conf *br_conf, struct br_tables *br_tables)
{
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
        CoLoR_show_pkg(color_hdr);

    ANN_front_t *ann_hdr = (ANN_front_t *)color_hdr;

    /* Check packet length consistency. From version to px_num: 8 bytes. */
    if (rte_pktmbuf_pkt_len(pkt) < 8 ||
            offset + rte_le_to_cpu_16(ann_hdr->pkg_len) != rte_pktmbuf_pkt_len(pkt)) {
        BR_LOG("received an ANN packet with incorrect packet length\n");
        return FREE_THIS_PACKET;
    }
    uint16_t packet_length = rte_le_to_cpu_16(ann_hdr->pkg_len);

    int ret = check_ann_length(ann_hdr, packet_length);
    if (ret == FAILED) {
        BR_LOG("received an ANN packet with invalid length: %d bytes.\n",
            packet_length);
        return FREE_THIS_PACKET;
    }

    /* Checksum */
#ifndef NO_ANN_CKCUM
    uint16_t cksum = rte_raw_cksum((void *)ann_hdr, packet_length);
    if (cksum != 0xFFFF) {
        BR_LOG("received an ANN packet with incorrect checksum\n");
        return FREE_THIS_PACKET;
    }
#endif

    /* Make sure TTL > 0 and TTL - 1 > 0. */
    if (ann_hdr->ttl <= 1) {
        BR_LOG("received an ANN packet with TTL = %d\n", ann_hdr->ttl);
        return FREE_THIS_PACKET;
    }
    ann_hdr->ttl = ann_hdr->ttl - 1;

    /* F = 1, the ANN packet is from another AS. */
    uint8_t flag = ann_hdr->flag;
    ann_hdr->flag = COLOR_ANN_REV_FLAG_F(flag);

    if (COLOR_ANN_FLAG_F(flag)) {
        *dst_ipv4_addr = br_conf->rm_ip_addr;
    } else { /* F = 0, the ANN packet is sent to another BR. */
        uint32_t px_raw = *(uint16_t *)((uint8_t *)ann_hdr + packet_length - 2);
        px_raw = rte_le_to_cpu_16(px_raw);
        uint32_t px = px_raw << 16;
        int ret = px_search_ipv4_addr(br_tables->px_table, px, dst_ipv4_addr);
        if (ret == FAILED) {
            BR_LOG("Destination IPv4 address for PX = %#06x is not found.\n", px);
            return FREE_THIS_PACKET;
        }
    }

    /* Checksum */
    ann_hdr->checksum = 0;

#ifndef NO_ANN_CKCUM
    ann_hdr->checksum = ~rte_raw_cksum((void *)ann_hdr, packet_length);
#endif

    BR_LOG("Processed ANN packet: TLL = %d, flag = %#04x\n", ann_hdr->ttl,
        ann_hdr->flag);

    return GOOD_PACKET;
}

#endif