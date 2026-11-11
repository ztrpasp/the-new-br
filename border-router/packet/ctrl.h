#ifndef __CTRL_H__
#define __CTRL_H__

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <rte_byteorder.h>
#include <rte_mbuf.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_rwlock.h>
#include <openssl/rsa.h>
#include <openssl/pem.h>
#include <openssl/hmac.h>
#include "rte_CoLoR.h"
#include "../identifier/nid.h"
#include "../br_conf.h"
#include "../br_tables.h"
#include "../ip/ip.h"
#include "../common.h"

static inline struct rte_mbuf *new_ctrl_pkt(struct br_conf *br_conf, uint8_t tag,
     uint8_t *data, size_t data_len)
{
    struct rte_mbuf *pkt = rte_pktmbuf_alloc(br_conf->tx_pktmbuf_pool);
    if (pkt == NULL) {
        BR_LOG("Failed to allocate mbuf for CONTROL packet\n");
        return NULL;
    }

    /* IPv4 header + DATA header */
    size_t pkt_size = sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) +
        sizeof(CONTROL_front_t) + data_len;
    pkt->data_len = pkt_size;
    pkt->pkt_len = pkt_size;

    /* Setup IPv4 header */
    struct rte_ipv4_hdr *ipv4_hdr = rte_pktmbuf_mtod_offset(pkt,
        struct rte_ipv4_hdr *, sizeof(struct rte_ether_hdr));
    ipv4_hdr->version_ihl = IP_VHL_DEF;
    ipv4_hdr->type_of_service = 0;
    ipv4_hdr->total_length = rte_cpu_to_be_16(sizeof(struct rte_ipv4_hdr) +
        sizeof(CONTROL_front_t) + data_len);
    ipv4_hdr->packet_id = 0;
    ipv4_hdr->fragment_offset = 0x40;
    ipv4_hdr->time_to_live = IP_DEFTTL;
    ipv4_hdr->next_proto_id = IPPROTO_COLOR;

    /* Control packet */
    CONTROL_front_t *ctrl_hdr = (CONTROL_front_t *)
        ((uint8_t *)ipv4_hdr + sizeof(struct rte_ipv4_hdr));
    ctrl_hdr->version_package = 0x74;
    ctrl_hdr->ttl = 1;
    ctrl_hdr->header_len = sizeof(CONTROL_front_t);
    ctrl_hdr->checksum = 0;
    ctrl_hdr->tag = tag;
    ctrl_hdr->len = data_len;
    uint8_t *pdata = (uint8_t *)ctrl_hdr + sizeof(CONTROL_front_t);
    rte_memcpy(pdata, data, data_len);
    ctrl_hdr->checksum = ~rte_raw_cksum((void *)ctrl_hdr, ctrl_hdr->header_len + ctrl_hdr->len);
    return pkt;
}


/**
 * Send CONTROL packet to dst IPv4 address
 *
 * @param tag
 *   tag field in CONTROL packet
 * @param ctrl_data
 *   pointer to data of CONTROL packet
 * @param ctrl_data_len
 *   length of data of CONTROL packet
 * @param ipv4_addr
 *   Destination IPv4 address
 */
static inline void send_ctrl_data_to_dst_ip(uint8_t tag, uint8_t *data,
    size_t data_len, uint32_t ipv4_addr, struct br_conf *br_conf,
    struct br_tables *br_tables)
{
    struct rte_mbuf *pkt = new_ctrl_pkt(br_conf, tag, data, data_len);
    size_t pkt_size = sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) +
        sizeof(CONTROL_front_t) + data_len;
    /* The setting of header checksum ,src addr, dst addr and ethernet header
       are left to send_pkt_to_dst_ip() */
    int16_t dst_portid = send_pkt_to_dst_ip(pkt, ipv4_addr, br_conf, br_tables);
    if (dst_portid >= 0) {
        struct stats_table *stats_table = &br_tables->stats_table[rte_lcore_id()];
        stats_inc(stats_table, dst_portid, COLOR_CONTROL_TYPE, tx, pkt_size);
    }
}

static inline void send_ctrl_pkt_to_dst_ip(struct rte_mbuf *pkt, uint32_t ipv4_addr,
    struct br_conf *br_conf, struct br_tables *br_tables)
{
    size_t pkt_size = pkt->pkt_len;
    /* The setting of header checksum ,src addr, dst addr and ethernet header
       are left to send_pkt_to_dst_ip() */
    int16_t dst_portid = send_pkt_to_dst_ip(pkt, ipv4_addr, br_conf, br_tables);
    if (dst_portid >= 0) {
        struct stats_table *stats_table = &br_tables->stats_table[rte_lcore_id()];
        stats_inc(stats_table, dst_portid, COLOR_CONTROL_TYPE, tx, pkt_size);
    }
}

/**
 * Send AS Attack Summary warning to dst IPv4 address
 * Tag: CONTROL_TAG_ATTACK_WARNING 18
 *
 * @param asid
 *   AS ID of attacks
 * @param num_attacks
 *   The number of attacks
 * @param ipv4_addr
 *   Destination IPv4 address
 */
static inline void send_attack_summary_to_dst_ip(uint32_t asid, uint32_t num_attacks,
    uint32_t ipv4_addr, struct br_conf *br_conf, struct br_tables *br_tables)
{
    const int ctrl_data_len = 21;
    uint8_t ctrl_data[ctrl_data_len];
    nid_copy(&br_conf->br_nid, (struct nid *)(ctrl_data));
    ctrl_data[16] = (uint8_t)asid;
    *(uint32_t *)(ctrl_data + 17) = rte_cpu_to_le_32(num_attacks);
    send_ctrl_data_to_dst_ip(CONTROL_TAG_ATTACK_WARNING, ctrl_data,
        ctrl_data_len, ipv4_addr, br_conf, br_tables);
}

/**
 * Send Attack warning to all of the proxies
 * Tag: CONTROL_TAG_ATTACK_WARNING 18
 *
 * @param asid
 *   AS ID of attacks
 * @param num_attacks
 *   The number of attacks
 * @param unknown_as
 *   `asid` is not valid if `unknown_as` is true
 */
static inline void send_attack_summary_to_all_proxies(uint32_t asid, uint32_t num_attacks,
    bool unknown_as, struct br_conf *br_conf, struct br_tables *br_tables)
{
    uint32_t next = 0;
    int32_t ret;
    uint8_t *nid;
    struct nid_entry *entry;
    rte_rwlock_read_lock(&br_tables->nid_table.lock);
    while (true) {
        ret = rte_hash_iterate(br_tables->nid_table.ht, (const void **)&nid,
            (void **)&entry, &next);
        if (ret == -ENOENT) {
            break;
        }
        send_attack_summary_to_dst_ip(unknown_as ? 0 : asid, num_attacks,
            entry->ipv4_addr, br_conf, br_tables);
    }
    rte_rwlock_read_unlock(&br_tables->nid_table.lock);
}

/**
 * Send ODC warning to dst IPv4 address
 * Tag: CONTROL_TAG_ODC_WARNING 17
 *
 * @param pkt
 *   The pointer to the mbuf of the packet.
 * @param data_hdr
 *   The pointer to the header of the DATA packet.
 * @param ipv4_addr
 *   Destination IPv4 address
 */
static inline void send_odc_warning_to_dst_ip(struct rte_mbuf *pkt,
    uint32_t ipv4_addr, struct br_conf *br_conf, struct br_tables *br_tables)
{
    const int MAX_CTRL_DATA_LEN = 1024;
    uint8_t ctrl_data[MAX_CTRL_DATA_LEN];
    struct rte_ipv4_hdr *ipv4_hdr = rte_pktmbuf_mtod_offset(pkt,
        struct rte_ipv4_hdr *, sizeof(struct rte_ether_hdr));
    uint32_t offset = sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr);
    DATA_front_t *data_hdr = rte_pktmbuf_mtod_offset(pkt, DATA_front_t *, offset);
    /* max(ctrl_data_len): 20 + 256 = 276 bytes */
    int ctrl_data_len = sizeof(struct rte_ipv4_hdr) + data_hdr->header_len;
    rte_memcpy(ctrl_data, ipv4_hdr, sizeof(struct rte_ipv4_hdr));
    rte_memcpy(ctrl_data + sizeof(struct rte_ipv4_hdr), data_hdr, data_hdr->header_len);
    send_ctrl_data_to_dst_ip(CONTROL_TAG_ODC_WARNING, ctrl_data,
        ctrl_data_len, ipv4_addr, br_conf, br_tables);
}

/**
 * Send ODC warning to all of the proxies
 * Tag: CONTROL_TAG_ODC_WARNING 17
 *
 * @param pkt
 *   The pointer to the mbuf of the packet.
 * @param data_hdr
 *   The pointer to the header of the DATA packet.
 */
static inline void send_odc_warning_to_all_proxies(struct rte_mbuf *pkt,
    struct br_conf *br_conf, struct br_tables *br_tables)
{
    uint32_t next = 0;
    int32_t ret;
    uint8_t *nid;
    struct nid_entry *entry;
    rte_rwlock_read_lock(&br_tables->nid_table.lock);
    while (true) {
        ret = rte_hash_iterate(br_tables->nid_table.ht, (const void **)&nid,
            (void **)&entry, &next);
        if (ret == -ENOENT) {
            break;
        }
        send_odc_warning_to_dst_ip(pkt, entry->ipv4_addr, br_conf, br_tables);
    }
    rte_rwlock_read_unlock(&br_tables->nid_table.lock);
}


/**
 * @brief Add inter-domain path described in path_desc
 * path_desc has data of 13 bytes.
 *
 * @param path_desc
 */
static inline void add_interdomain_path(const uint8_t *path_desc, uint16_t portid,
    struct br_tables *br_tables)
{
    uint32_t as_id = rte_le_to_cpu_32(*(uint32_t *)path_desc);
    uint8_t prefix_len = *(path_desc += 4);
    uint32_t prefix = rte_le_to_cpu_32(*(uint32_t *)(path_desc += 1));
    uint32_t ipv4_addr = rte_le_to_cpu_32(*(uint32_t *)(path_desc += 4));
    uint16_t px = (uint16_t)(prefix >> 16);
    if (prefix_len != 16) {
        BR_LOG("Only prefix length 16 is supported, got %08x/%d\n, skipped...", prefix, prefix_len);
        return;
    }
    uint32_t port_ipv4_addr;
    if (port_get_ip(&br_tables->port_table, portid, &port_ipv4_addr) == FAILED) {
        BR_LOG("Cannot find IP address for port %d\n", portid);
        return;
    }
    if (port_has_ip(&br_tables->port_table, ipv4_addr)) {// TODO
        // Insert AS-PX to AS-PX table.
        // as_px_table_add(&br_tables->as_px_table, as_id, px);
        // px_table_update(&br_tables->px_table, &br_tables->peer_table, &br_tables->as_px_table);
        BR_LOG("IP address of AS-PX-IP is mine: AS = %u, PX = 0x%04x, IP address: %d.%d.%d.%d\n",
            as_id, px, IPv4TO4DECIMAL(ipv4_addr));
    } else {// TODO
        // Insert AS-PX to AS-PX table, AS-IP to peer table(internal peer).
        // PX-IP will be generated automatically.
        // as_px_table_add(&br_tables->as_px_table, as_id, px);
        // peer_table_add(&br_tables->peer_table, ipv4_addr, true, as_id);
        // px_table_update(&br_tables->px_table, &br_tables->peer_table, &br_tables->as_px_table);
        BR_LOG("Peer %d.%d.%d.%d is added. AS = %u, PX = 0x%04x\n",
            IPv4TO4DECIMAL(ipv4_addr), as_id, px);
    }
}

/**
 * Process the Control packet.
 *
 * This function verifies the checksum field of the CONTROL packet, extracts
 * control information from the CONTROL packet. The control information of
 * CONTROL packet with following tags will be processed by BR.
 *
 * Tag: CONTROL_TAG_SYN_BR_CONFIG 3
 * +---+--+---+---+--+--+----+
 * |NID|SK|NUM|[AS|PX|IP|]...|
 * +---+--+---+---+--+--+----+
 * NID: 16 bytes, NID of BR;
 * SK: 4 bytes, SK of the AS;
 * NUM: 1 bytes, number of AS-PX-IP
 * AS: 1 bytes, ASID;
 * PX: 2 bytes;
 * IP: 4 bytes;
 * Set the BR's NID, SK; Add AS-PX mapping; Add AS-IP (peer) if IP != BR's IP
 *
 * Tag: CONTROL_TAG_SYN_BR_PROXY_CONFIG 8
 * +----+-----+
 * | IP | NID |
 * +----+-----+
 * IP: 4 bytes, the IPv4 address associated with the NID;
 * NID: 16 bytes, the NID of a PROXY;
 * Add this mapping to NID-IP table.
 *
 * Tag: CONTROL_TAG_ODC 16
 * +-----+-------+
 * | NID | L_SID |
 * +-----+-------+
 * L_SID: 20 bytes
 * NID: 16 bytes, the NID of consumer
 * Add this entry to ODC table.
 *
 * Zero TTL, incorrect checksum, or inconsistent length will cause the packet
 * to be discarded.
 *
 * Pre-conditions:
 * *pkt* points to a CONTROL packet.
 *
 * @param pkt
 *   The pointer to the mbuf of the packet.
 * @return
 *   FREE_THIS_PACKET
 */
static inline int process_control_packet(struct rte_mbuf *pkt,
    struct br_conf *br_conf, struct br_tables *br_tables)
{
    uint32_t offset = sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr);
    void *color_hdr = rte_pktmbuf_mtod_offset(pkt, void *, offset);
    CONTROL_front_t *ctrl_hdr = (CONTROL_front_t *)color_hdr;

    /* Check length consistency. From version to nid field: 64 bytes. */
    if (rte_pktmbuf_pkt_len(pkt) < sizeof(CONTROL_front_t) + offset ||
            rte_le_to_cpu_16(ctrl_hdr->header_len) != sizeof(CONTROL_front_t) ||
            offset + sizeof(CONTROL_front_t) + rte_le_to_cpu_16(ctrl_hdr->len)
            != rte_pktmbuf_pkt_len(pkt)) {
        BR_LOG("Received a CONTROL packet with incorrect packet length\n");
        return FREE_THIS_PACKET;
    }

    /* Checksum */
    uint16_t cksum = rte_raw_cksum((void *)ctrl_hdr, ctrl_hdr->header_len + ctrl_hdr->len);
    if (cksum != 0xFFFF) {
        BR_LOG("Received a CONTROL packet with incorrect checksum\n");
        return FREE_THIS_PACKET;
    }

    /* Make sure TTL > 0. */
    if (ctrl_hdr->ttl == 0) {
        BR_LOG("Received a CONTROL packet with TTL = %d\n", ctrl_hdr->ttl);
        return FREE_THIS_PACKET;
    }

    uint16_t len = rte_le_to_cpu_16(ctrl_hdr->len);
    if (ctrl_hdr->tag == CONTROL_TAG_SYN_BR_CONFIG) {
        if (len < 21) { // NID: 16 bytes + SK: 4 bytes + NUM: 1 byte
            BR_LOG("CONTROL packet: type = CONTROL_TAG_SYN_BR_CONFIG(3), "
                "len = %u, should no less than 21.\n", len);
            return FREE_THIS_PACKET;
        }
        // NID is ignored since BR doesn't need it.
        uint8_t *pnid = (uint8_t *)ctrl_hdr + sizeof(CONTROL_front_t);
        nid_copy((struct nid *)pnid, &br_conf->br_nid);
        uint32_t sk = rte_le_to_cpu_32(*(uint32_t *)(pnid + 16));
        uint8_t num = *(pnid + 20);
        const int size_path = 13;
        if (len != 21 + size_path * num) { // ASID: 4 byte + PX: 4+1 bytes + IP: 4 bytes
            BR_LOG("CONTROL packet: type = CONTROL_TAG_SYN_BR_CONFIG(3), "
                "len = %u, should be %u.\n", len, 21 + size_path * num);
            return FREE_THIS_PACKET;
        }
        br_conf->sk = sk;
        BR_LOG("CONTROL packet: set SK to 0x%08x.\n", sk);
        uint8_t *p = pnid + 21; // points to AS-PX-IP
        for (uint8_t i = 0; i < num; i++) {
            uint32_t asid = rte_le_to_cpu_32(*(uint32_t *)p);
            uint8_t px_len = *(p + 4);
            uint32_t px = rte_le_to_cpu_32(*(uint32_t *)(p + 5));
            uint32_t ipv4_addr = rte_le_to_cpu_32(*(uint32_t *)(p + 9));
            uint32_t port_ipv4_addr;
            if (port_get_ip(&br_tables->port_table, pkt->port, &port_ipv4_addr) == FAILED) {
                BR_LOG("CONTROL packet: cannot find IP address for port %d\n",
                    pkt->port);
                continue;
            }
            uint8_t br_sk[SIZE_BR_SK] = {0};
            if (port_has_ip(&br_tables->port_table, ipv4_addr)) {
                BR_LOG("CONTROL packet: IP address of AS-PX-IP is mine: "
                    "AS = %u, PX = 0x%04x, IP address: %d.%d.%d.%d"
                    ", CANNOT add to peer table because the IP address of peer is"
                    " unknown, do this MANUALLY!\n",
                    asid, px, IPv4TO4DECIMAL(ipv4_addr));
            } else {
                peer_add(&br_tables->peer_table, ipv4_addr, true, br_sk);
                peer_add_path(&br_tables->peer_table, ipv4_addr, asid, px_len, px);
                // px_table_update(&br_tables->px_table, &br_tables->peer_table); todo
                BR_LOG("CONTROL packet: Peer %d.%d.%d.%d is added. "
                    "AS = %u, PX = 0x%04x\n",
                    IPv4TO4DECIMAL(ipv4_addr), asid, px);
            }
            p += size_path;
        }
    } else if (ctrl_hdr->tag == CONTROL_TAG_SYN_BR_PROXY_CONFIG) {
        if (len != 20) { // NID: 16 bytes + IP address: 4 bytes
            BR_LOG("CONTROL packet: type = CONTROL_TAG_SYN_BR_PROXY_CONFIG(8), "
                "len = %u, should be 20.\n", len);
            return FREE_THIS_PACKET;
        }
        uint8_t *pip = (uint8_t *)ctrl_hdr + sizeof(CONTROL_front_t);
        uint8_t *pnid = pip + 4;
        uint32_t ipv4_addr = rte_le_to_cpu_32(*(uint32_t *)(pip));
        uint8_t sk[SIZE_HOST_SK];
        nid_add(&br_tables->nid_table, pnid, ipv4_addr, sk); // todo: extract sk
        BR_LOG("Received a CONTROL packet. A NID-IP entry is added.\n");
    } else if (ctrl_hdr->tag == CONTROL_TAG_ODC) {
        uint32_t *rn = (uint32_t *)((uint8_t *)ctrl_hdr + sizeof(CONTROL_front_t));
        uint8_t *nid_provider = (uint8_t *)rn + sizeof(uint32_t);
        uint8_t *l_sid = nid_provider + 16;
        uint8_t *nid_consumer = l_sid + 20;
        uint8_t *pids = nid_consumer + 16; // PID sequence
        uint32_t pids_len = len - 4 - 16 - 20 - 16;
        BR_LOG("The random number is 0x%08x.\n", *rn);
        nid_provider = NULL; // todo: sometimes nid_provider is not presented in DATA packet.
        odc_add_entry(&br_tables->odc_table, nid_provider, l_sid, nid_consumer,
            pids, pids_len, *rn, ODC_ENTRY_EXPIRE_SECONDS);
        BR_LOG("Received a CONTROL packet. An ODC entry is added.\n");
    }
    else {
        BR_LOG("Received a CONTROL packet with tag = %d. The packet is ignored.\n",
            ctrl_hdr->tag);
    }

    return FREE_THIS_PACKET;
}

#endif