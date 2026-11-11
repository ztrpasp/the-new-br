#ifndef __DATA_H__
#define __DATA_H__

#include <stdint.h>
#include <stdbool.h>
#include <rte_byteorder.h>
#include <rte_mbuf.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <openssl/hmac.h>
#include "rte_CoLoR.h"
#include "../common.h"
#include "../profiler/profiler.h"
#include "../br_tables.h"
#include "pid_verify.h"
#include "../br_conf.h"

/**
 * Check whether the length indicated by the field header_len is consistent with
 * the actual length indicated by field PID_num, flag M, Q, C, S.
 *
 * Pre-conditions:
 * 1. `data_hdr` is a valid pointer to a DATA header of size hdr_len.
 * 2. data_hdr->header_len == hdr_len
 *
 * @param data_hdr
 *   the pointer to a DATA packet
 * @param pkt_len
 *   the length of the DATA packet header
 * @returns
 *   - OK if the actual length of the DATA packet is not consistent with the
 *     length indicated by the content of the packet, otherwise
 *   - FAILED
 */
static inline int
check_data_length(DATA_front_t *data_hdr, uint16_t hdr_len)
{
    uint16_t len = 10;	// from version to flag: 10 bytes
    uint8_t flag = data_hdr->flag;
    ASSERT_NOT_LESS_THAN(hdr_len, len);
    if (COLOR_DATA_FLAG_M(flag)) {
        len += 2; // Minimal PID CP
    }
    len += 52; // SID, NID
    /** Normal DATA packet(not ACK) that needs backward PID calculation has
     * \p source_nid field following \p nid (the first NID field, nid of consumer).
     * ACK packet has \p source_nid field following \p nid (the first
     * NID field, nid of provider).
     */
    if (COLOR_DATA_FLAG_B(flag) || COLOR_DATA_FLAG_R(flag)) {
        len += 16;
    }
    if (COLOR_DATA_FLAG_Q(flag)) { // QoS
        ASSERT_NOT_LESS_THAN(hdr_len, len + 1);
        uint8_t qoslen = *((uint8_t *)data_hdr + len);
        len += 1 + qoslen;
    }
    if (COLOR_DATA_FLAG_C(flag)) {
        len += 4; // HMAC
    }
    if (COLOR_DATA_FLAG_S(flag)) {
        len += 4; // seg_id
    }
    len += data_hdr->PID_num * 4;
    // Not ACK and need backward PID calculation
    // ACK packet cannot have R = 1
    if (!COLOR_DATA_FLAG_B(flag) && COLOR_DATA_FLAG_R(flag)) {
        len += 4; // backward PID,
    }
    ASSERT_EQUAL(hdr_len, len);
    return OK;
}

/**
 * @brief Compute 4-bytes HMAC for \p data of size \p data_len
 *
 * @param sk pointer to the secret key
 * @param sk_len length of the secret key
 * @param data data to compute HMAC
 * @param data_len length of the data
 * @param hmac pointer to the computed HMAC, \p *hmac is set to the computed HMAC
 * 			   if no error occurs. The computed HMAC is stored in little endian.
 * @return int returns FAILED if an error occurred else returns OK.
 */
static inline int compute_hmac(const void *sk, int sk_len, const void *data,
    int data_len, uint32_t *hmac)
{
    unsigned int md_len;
    uint8_t md[EVP_MAX_MD_SIZE];
    uint8_t *ret = HMAC(EVP_md5(), sk, sk_len, data, data_len, md, &md_len);
    if (unlikely(ret == NULL)) {
        BR_LOG("An error occurred when compute hmac.\n");
        return FAILED;
    }
    *hmac = *(uint32_t *)(md + 12);
    return OK;
}

/**
 * @brief Return pointer to the HMAC field in the DATA packet.
 * @todo QoS is NOT considered.
 *
 * @param data_hdr pointer to the DATA packet
 * @return uint32_t* pointer to the HMAC field
 */
static inline uint32_t *pointer_to_hmac(DATA_front_t const *data_hdr)
{
    return (uint32_t *)(data_hdr->nid +
        (COLOR_DATA_FLAG_R(data_hdr->flag) ? SIZE_NID * 2 : SIZE_NID));
}

/**
 * @brief Returns pointer to the PID sequence in the DATA packet.
 *
 * @param data_hdr pointer to the DATA packet
 * @return uint8_t* pointer to the PID sequence
 */
static inline uint8_t *pointer_to_pids(DATA_front_t const *data_hdr)
{
    uint32_t const pids_len = data_hdr->PID_num * PID_SIZE_IN_BYTES;
    int pid_offset;
    if (COLOR_DATA_FLAG_R(data_hdr->flag)) {
        pid_offset = data_hdr->header_len - ((data_hdr->PID_num + 1) * PID_SIZE_IN_BYTES);
    } else {
        pid_offset = data_hdr->header_len - (data_hdr->PID_num * PID_SIZE_IN_BYTES);
    }
    return (uint8_t *)data_hdr + pid_offset;
}

/**
 * Perform outbound data control. Lookup the outbound data control table if the
 * provider of the DATA packet is in the same domain as the border router,
 * else verify the last used PID.
 *
 * The provider of the DATA packet is in the same domain as the border router
 * if the PID pointer points to the last PID in the packet(number of PIDs ==
 * PID pointer).
 *
 * This function is not responsible for checking whether ODC is enabled.
 *
 * Pre-conditions:
 *   *data_hdr* points to the header of an DATA packet (NOT ACK). The border router
 *   received the packet from a host or another border router in the same domain.
 * Post-conditions:
 *   The timer in ODCT is reset if ODCT lookup hits.
 *   This function does NOT modify the packet.
 *
 * @param pkt
 *   The pointer to the mbuf of the packet.
 * @param data_hdr
 *   The pointer to the header of the DATA packet.
 * @return
 *   OK if outbound data check succeeds or ODC is not required.
 *   FAILED if ODCT lookup failed or PID verification failed.
 */
static inline int
outbound_data_control(const struct rte_mbuf *pkt, DATA_front_t *data_hdr,
    struct odc_table *odc_table, const struct br_conf *br_conf)
{
    int32_t ret;

    /* Data packet from this domain: ODC table */
    /* Normal DATA packet(not ACK) that needs backward PID calculation has
    * nid_provider field following nid_consumer (the first NID field).*/
    uint8_t const * const l_sid = data_hdr->L_sid;
    uint8_t const * const nid_consumer = data_hdr->nid;
    // todo: sometimes nid_provider is not presented in DATA packet(optional)
    // uint8_t const * const nid_provider = COLOR_DATA_FLAG_R(data_hdr->flag)
    // 	? (data_hdr->nid + SIZE_NID) : NULL;
    uint8_t const * const nid_provider = NULL;
    uint32_t * const phmac = pointer_to_hmac(data_hdr);

    uint32_t pids_len = data_hdr->PID_num * PID_SIZE_IN_BYTES;
    uint8_t const * const pids = pointer_to_pids(data_hdr);
    struct odc_entry const *p = NULL;
    /* Perform ODC table lookup */
    PROFILER_BEGIN(odct);
    ret = odc_search(odc_table, nid_provider, l_sid, nid_consumer, pids, pids_len, &p);
    PROFILER_END(odct);
    if (unlikely(ret == FAILED)) {
        BR_LOG("No ODC entry!\r\n");
        return FAILED;
    }
    BR_LOG("The random number is 0x%08x.\r\n", p->rn);

    /* HMAC */
#ifdef PERFORMANCE_TEST
    // uint8_t msg[2048];
    // uint32_t digest[4] = {0};
    /* Message: from NSID to the end of payload.
                            the beginning of message	the length of message
        No Minimal_PID_CP:     	data_hdr + 10				len(DATA) - 10
        With Minimal_PID_CP:    	data_hdr + 12				len(DATA) - 12
    */
    uint32_t offset = COLOR_DATA_FLAG_M(data_hdr->flag) ? 12 : 10;
    uint8_t *msg_begin = (uint8_t *)data_hdr + offset;
    uint32_t msg_len = data_hdr->pkg_len - offset;
    uint32_t hmac = rte_le_to_cpu_32(*phmac);
    *phmac = 0; /* set HMAC field to 0 */
    uint32_t hmac_to_compare;
    ret = compute_hmac(&(p->rn), 4, msg_begin, msg_len, &hmac_to_compare);
    *phmac = rte_cpu_to_le_32(hmac); // restore HMAC in DATA packet
    BR_LOG("HMAC in packet: 0x%08x, calculated HMAC: 0x%08x.\n",
    hmac, hmac_to_compare);
    // if (unlikely(hmac != hmac_to_compare)) {
    //     return FAILED;
    // }
#else
    uint8_t msg[2048];
    uint32_t digest[4] = {0};
    /* Message: from NSID to the end of payload.
                            the beginning of message	the length of message
        No Minimal_PID_CP:     	data_hdr + 10				len(DATA) - 10
        With Minimal_PID_CP:    	data_hdr + 12				len(DATA) - 12
    */
    uint32_t offset = COLOR_DATA_FLAG_M(data_hdr->flag) ? 12 : 10;
    uint8_t *msg_begin = (uint8_t *)data_hdr + offset;
    uint32_t msg_len = data_hdr->pkg_len - offset;
    uint32_t hmac = rte_le_to_cpu_32(*phmac);
    *phmac = 0; /* set HMAC field to 0 */
    rte_memcpy(msg, msg_begin, msg_len);
    *phmac = rte_cpu_to_le_32(hmac); // restore HMAC in DATA packet
    *(uint32_t *)(msg + msg_len) = rte_cpu_to_le_32(p->rn);
    msg_len += 4;
    md5(msg, msg_len, digest);
    uint32_t hmac_to_compare = digest[3];
    BR_LOG("HMAC in packet: 0x%08x, calculated HMAC: 0x%08x.\n",
    hmac, hmac_to_compare);
    if (unlikely(hmac != hmac_to_compare)) {
        return FAILED;
    }
#endif

    return OK;
}

static inline int verify_hmac(struct br_tables *br_tables, uint8_t *data,
    uint32_t data_len, uint32_t last_pid, uint32_t *phmac)
{
    uint32_t last_ipv4_addr;
    uint32_t hmac_a =  rte_le_to_cpu_32(*phmac);
    int ret = px_search_ipv4_addr(br_tables->px_table, last_pid, &last_ipv4_addr);
    if (unlikely(ret == FAILED)) {
        BR_LOGF("Last PID = 0x%08x is not found.\n", last_pid);
        return FAILED;
    }
    uint8_t last_sk[SIZE_BR_SK];
    ret = peer_search(&br_tables->peer_table, last_ipv4_addr, NULL, last_sk);
    if (unlikely(ret == FAILED)) {
        BR_LOGF("Peer %d.%d.%d.%d cannot be found.\n", IPv4TO4DECIMAL(last_ipv4_addr));
        return FAILED;
    }
    /* Set HMAC field to zero, recompute HMAC and check if it is correct. */
    uint32_t hmac_b;
    *phmac = rte_cpu_to_le_32(0);
    ret = compute_hmac(last_sk, SIZE_BR_SK, data, data_len, &hmac_b);
    if (unlikely(ret == FAILED)) {
        BR_LOGF("An error occurs when verifying hmac.\n");
        return FAILED;
    }
    BR_LOGF("HMAC in packet=%08x, calculated HMAC=%08x\n", hmac_a, hmac_b);
    if (unlikely(hmac_a != hmac_b)) {
        BR_LOGF("HMAC verification failed.\n");
        return FAILED;
    }
    return OK;
}

/**
 * Process the DATA packet.
 *
 * This function verifies the checksum field of the DATA packet, performs PID
 * verification and outbound data control, calculate backward PID, and determines
 * the destination IPv4 address for this GET packet. The destination IPv4 address
 * is returned by parameter *dst_ipv4_addr*.
 *
 * Zero TTL, incorrect checksum, false PID, or failure of outbound data control will
 * cause the packet to be discarded. If the packet should be discarded, this function
 * returns BAD_PACKET, and the content of *dst_ipv4_addr* is left unmodified.
 *
 * Pre-conditions:
 * *pkt* points to an DATA packet. *dst_ipv4_addr* is a valid pointer.
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
process_data_packet(struct rte_mbuf *pkt, uint32_t *dst_ipv4_addr,
    struct br_conf *br_conf, struct br_tables *br_tables)
{
    PROFILER_BEGIN(process_data_pkt);

    void *color_hdr;
    struct rte_ether_hdr * eth_hdr;
    uint32_t offset = sizeof(struct rte_ether_hdr);

    eth_hdr = rte_pktmbuf_mtod(pkt, struct rte_ether_hdr *);

    if (eth_hdr->ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4)) {
        offset += sizeof (struct rte_ipv4_hdr);
    } else if (eth_hdr->ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV6)) {
        offset += sizeof (struct rte_ipv6_hdr);
    } else {
        BR_LOG("received a DATA packet with unsupported layer 3 protocol checksum\n");
        return FREE_THIS_PACKET;
    }
    color_hdr = rte_pktmbuf_mtod_offset(pkt, void *, offset);
    DATA_front_t * const data_hdr = (DATA_front_t *)color_hdr;

    if (unlikely(br_conf->debugging_on)) {
        BR_LOGF("Receive DATA packet:\n");
        CoLoR_show_pkg(color_hdr);
    }

    /** From \p version to \p flag field: 10 bytes */
    if (unlikely(rte_pktmbuf_data_len(pkt) < offset + 10 ||
            offset + rte_le_to_cpu_32(data_hdr->pkg_len) != rte_pktmbuf_pkt_len(pkt) ||
            data_hdr->header_len > rte_pktmbuf_pkt_len(pkt))) {
        BR_LOGF("received a DATA packet with incorrect header length\n");
        return FREE_THIS_PACKET;
    }

    /** Check if \p Checksum is correct. */
#ifndef NO_DATA_CKSUM
    PROFILER_BEGIN(data_cksum);
    uint16_t const cksum = rte_raw_cksum((void *)data_hdr, data_hdr->header_len);
    PROFILER_END(data_cksum);
    if (unlikely(cksum != 0xFFFF)) {
        BR_LOGF("received a DATA packet with incorrect checksum\n");
        return FREE_THIS_PACKET;
    }
#endif

    /** Current version of BR require HMAC field. */
    //if (br_conf->odc_enabled && !COLOR_DATA_FLAG_C(data_hdr->flag) && !COLOR_DATA_FLAG_B(data_hdr->flag)) {
    //	BR_LOGF("HMAC field does not exist.\n");
    //	return FAILED;
    //}

    /* Check packet length consistency */
    PROFILER_BEGIN(pkt_len_check);
    int ret = check_data_length(data_hdr, data_hdr->header_len);
    PROFILER_END(pkt_len_check);
    if (unlikely(ret == FAILED)) {
        BR_LOGF("received a DATA packet with invalid header length: %d bytes\n",
            data_hdr->header_len);
        return FREE_THIS_PACKET;
    }

    /* Make sure TTL > 0 and TTL - 1 > 0. */
    if (unlikely(data_hdr->ttl <= 1)) {
        BR_LOGF("received a DATA packet with TTL = %d\n", data_hdr->ttl);
        return FREE_THIS_PACKET;
    }
    data_hdr->ttl = data_hdr->ttl - 1;

    /** Check \p PID_pointer (p) and \p Number_of_PIDs (n).
     *  p and n is valid if 0 < p <= n and n > 0 */
    uint8_t const flag = data_hdr->flag;
    int const n = data_hdr->PID_num;
    int const p = data_hdr->PID_pt;
    if ( unlikely(!(n > 0 && p > 0 && p <= n)) ) {
        BR_LOGF("received a DATA packet with bad PID_num = %d, PID_pt = %d\n", n, p);
        return FREE_THIS_PACKET;
    }

    /** Determine the type of last hop and next hop.
     *  F = 1, the packet is from outside the AS.
     *  F = 0, the packet is from inside the AS.
     * -------------------------+----------------------------
     *  F		n		p		|	last hop		next hop
     * -------------------------+----------------------------
     *  1		n>=1	p=1		|	eBR				Consumer
     *  1		n>=2	1<p<=n	|	eBR				eBR/iBR
     * -------------------------+----------------------------
     *  0		n>=1	p=n		|	Provider		eBR
     *  0		n>=2	1<=p<n	|	iBR				eBR
     * -------------------------+----------------------------
     */
    int const PROVIDER = 0x0001;
    int const CONSUMER = 0x0002;
    int const EBR = 0x0004;
    int const IBR = 0x0008;
    int const BR = 0x0010; // set this flag when either EBR or IBR is set

    int const last_hop = COLOR_DATA_FLAG_F(flag)
        ? (EBR | BR)
        : ((p == n) ? PROVIDER : (IBR | BR));

    /* If next_hop == BR, further table lookup is required to determine whether
     * next_hop is IBR or EBR */
    int next_hop = COLOR_DATA_FLAG_F(flag)
        ? ((p == 1) ? CONSUMER : BR)
        : (EBR | BR);

    uint32_t const hmac_offset = COLOR_DATA_FLAG_M(flag) ? 12 : 10;
    uint8_t * const data = (uint8_t *)data_hdr + hmac_offset; // data to be verified
    uint32_t const data_len = data_hdr->pkg_len - hmac_offset; // length of data to be verified
    uint32_t * const phmac = pointer_to_hmac(data_hdr);  // pointer to HMAC in packet
    uint32_t const hmac_a = rte_le_to_cpu_32(*phmac); // hmac in packet

    /** Verify HMAC. */
#ifndef NO_HMAC_VERIFICATION
    if (br_conf->odc_enabled) {

        // last hop is provider and the packet is not ACK
        if ((last_hop & PROVIDER) && !COLOR_DATA_FLAG_B(data_hdr->flag)) {
            int ret;
            if (COLOR_DATA_FLAG_C(data_hdr->flag)) {
                ret = outbound_data_control(pkt, data_hdr, &br_tables->odc_table, br_conf);
            } else {
                ret = FAILED;
                BR_LOGF("HMAC field does not exist! ");
            }
            if (unlikely(ret == FAILED)) {
                BR_LOGF("ODC failed!\n");
                return (FREE_THIS_PACKET | ODC_FAILED_PACKET);
            }
        }

        // last hop is internal BR
        if (last_hop & IBR) { // use the PX of PID[p + 1]
            if (COLOR_DATA_FLAG_C(data_hdr->flag)) {
                uint32_t last_pid = deref_pid_pt_with_offset(data_hdr, 1);
                int ret = verify_hmac(br_tables, data, data_len, last_pid, phmac);
                if (unlikely(ret == FAILED)) {
                    return FREE_THIS_PACKET;
                }
            } else {
                BR_LOGF("HMAC field does not exist! The packet is not verified(ODC).\n");
            }
        }
    }

    // idhv is enabled and last hop is external BR
    if ( br_conf->interdomain_hmac_verify_enabled && (last_hop & EBR) ) {
        if (COLOR_DATA_FLAG_C(data_hdr->flag)) {
            uint32_t last_pid = deref_pid_pt(data_hdr);
            int ret = verify_hmac(br_tables, data, data_len, last_pid, phmac);
            if (unlikely(ret == FAILED)) {
                return FREE_THIS_PACKET;
            }
        } else {
            BR_LOGF("HMAC field does not exist! The packet is not verified(IDHV).\n");
        }
    }
#endif

    /** Verify PID. */
    /** F = 1, the DATA packet is from outside the AS, verify PID */
    if (br_conf->pid_verify_enabled && (last_hop & EBR)) {
        PROFILER_BEGIN(pid_verify);
        if (verify_pid_inbound(data_hdr, br_conf->sk, br_conf, br_tables) == FAILED) {
            return FREE_THIS_PACKET;
        }
        PROFILER_END(pid_verify);
    }

    /** F = 0, the DATA packet is from other domains: verify last PID */
    /* It's not necessary to verify last PID if HMAC is verified. */
    // if (br_conf->pid_verify_enabled && (last_hop & IBR)) {
    // 	PROFILER_BEGIN(pid_verify);
    // 	if (verify_pid_outbound(data_hdr, br_conf->sk) == FAILED) {
    // 		return FREE_THIS_PACKET;
    // 	}
    // 	PROFILER_END(pid_verify);
    // }

    if (last_hop & EBR) {
        data_hdr->PID_pt--;
    }

    uint8_t next_br_sk[SIZE_BR_SK];
    uint8_t next_host_sk[SIZE_HOST_SK];
    void const *next_sk;
    int size_sk;

    /** Table lookup. */
    if (next_hop & CONSUMER) {
        PROFILER_BEGIN(data_nid_ip_search);
        int ret = nid_search(&br_tables->nid_table, data_hdr->nid, dst_ipv4_addr, next_host_sk);
        PROFILER_END(data_nid_ip_search);
        if (unlikely(ret == FAILED)) {
            char s[64];
            hex_to_str(s, data_hdr->nid, SIZE_NID, 0);
            BR_LOGF("Destination IPv4 address for NID = %s is not found.\n", s);
            return FREE_THIS_PACKET;
        }
        /* Recomputation of HMAC: eBR -> Consumer, using BR-Host sk */
        next_sk = next_host_sk;
        size_sk = SIZE_HOST_SK;
    } else {
        /* next_hop can be IBR or EBR. Need to determine whether it's IBR or EBR. */
        PROFILER_BEGIN(data_px_search);
        bool is_internal;
        uint32_t const next_pid = deref_pid_pt(data_hdr);
        int ret = px_search_ipv4_addr(br_tables->px_table, next_pid, dst_ipv4_addr);
        PROFILER_END(data_px_search);
        if (unlikely(ret == FAILED)) {
            BR_LOGF("Destination IPv4 address for PID = 0x%08x is not found.\n", next_pid);
            return FREE_THIS_PACKET;
        }
        ret = peer_search(&br_tables->peer_table, *dst_ipv4_addr, &is_internal, next_br_sk);
        if (unlikely(ret == FAILED)) {
            BR_LOGF("Peer %d.%d.%d.%d cannot be found.\n", IPv4TO4DECIMAL(*dst_ipv4_addr));
            return FREE_THIS_PACKET;
        }
        next_hop |= is_internal ? IBR : EBR;
        /* Recomputation of HMAC: eBR -> eBR/iBR, using BR-BR sk */
        next_sk = next_br_sk;
        size_sk = SIZE_BR_SK;
    }

    /* The following step (backward PID calculation) must be after table lookup. */
    /* next hop is external BR and need to calculate backward PID for this packet */
    if ((next_hop & EBR) && COLOR_DATA_FLAG_R(data_hdr->flag)) {
        uint32_t pid;
        PROFILER_BEGIN(cal_back_pid);
        int ret = calculate_backward_pid(data_hdr, &pid, br_conf->sk);
        PROFILER_END(cal_back_pid);
        if (unlikely(ret == FAILED)) {
            return FREE_THIS_PACKET;
        }
        /* PID[pt + 1] <- backward PID */
        uint16_t pid_offset = calculate_pid_pt_offset(data_hdr, 1);
        *(uint32_t *)((uint8_t *)data_hdr + pid_offset) = rte_cpu_to_le_32(pid);
    }

    /** Set flag \p F according to the following rules.
     *  'in' means the packet is from inside the AS (F=0),
     *  'out' means the packet is from outside the AS (F=1)
     *  	case 1. in -> out: reverse F (set to 1)
     * 				last hop is IBR/PROVIDER and next hop is EBR
     *  	case 2. in -> in: no such case
     * 				last hop is IBR/PROVIDER and next hop is IBR/CONSUMER
     *  	case 3. out -> in: reverse F (set to 0)
     * 				last hop is EBR and next hop is IBR/CONSUMER
     *  	case 4. out -> out: leave F unchanged (set to 1)
     * 				last hop is EBR and next hop is EBR
     *
     *  To summarize:
     *  If both the last hop and the next hop is EBR, then don't change flag F,
     *  else reverse flag F.
     */
    if ( !(last_hop & next_hop & EBR) ) {
        data_hdr->flag = COLOR_DATA_REV_FLAG_F(flag);
    }

    /* Recompute HMAC. */
#ifndef NO_HMAC_COMPUTATION
#ifdef PERFORMANCE_TEST //  no next_hop & CONSUMER
    if ( (br_conf->odc_enabled && (next_hop & IBR))
            || (br_conf->interdomain_hmac_verify_enabled && (next_hop & EBR)) ) {
#else
    if ( (br_conf->odc_enabled && (next_hop & IBR))
            || (br_conf->interdomain_hmac_verify_enabled && (next_hop & EBR || next_hop & CONSUMER)) ) {
#endif
        if (COLOR_DATA_FLAG_C(data_hdr->flag)) {
            *phmac = rte_cpu_to_le_32(0);
            ret = compute_hmac(next_sk, size_sk, data, data_len, phmac);
            if (unlikely(ret == FAILED)) {
                BR_LOGF("An error occurs when generating hmac.\n");
                return FREE_THIS_PACKET;
            }
        } else {
            BR_LOGF("HMAC field does not exist. No recomputation of HMAC is performed.\n");
        }
    }
#endif

    /* Recompute checksum */
    data_hdr->checksum = 0;

#ifndef NO_DATA_CKSUM
    {
        PROFILER_BEGIN(data_cksum);
        data_hdr->checksum = ~rte_raw_cksum((void *)data_hdr, data_hdr->header_len);
        PROFILER_END(data_cksum);
    }
#endif

    BR_LOGF("Processed DATA packet: TLL = %d, flag = %#04x, pt = %d\n",
        data_hdr->ttl, data_hdr->flag, data_hdr->PID_pt);
    PROFILER_END(process_data_pkt);
    return GOOD_PACKET;
}

#endif
