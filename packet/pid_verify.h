#ifndef __PID_VERIFY_H__
#define __PID_VERIFY_H__

#include "pid.h"
#include "ctrl.h"

/**
 * Increment the number of inbound DATA packet with bad PID.
 * This operation is multi-thread safe.
 * If the incremented number exceeds limit(500, 1000, 1500, ...),
 * A warning CONTROL packet will be sent to all proxies.
 *
 * @param px
 *   PX
 * @param unknown_px
 *   if true, `px` is not used.
 */
static inline void br_stats_inc_bad_inbound_pid(uint32_t pid, bool is_unknown_px,
    struct br_conf *br_conf, struct br_tables *br_tables)
{
    uint32_t num;
    uint32_t asid = 0;
    bool is_unknown_as = is_unknown_px;
    if (!is_unknown_px) {
        int ret = px_search_as_id(br_tables->px_table, pid, &asid);
        is_unknown_as = (ret == OK) ? false : true;
    }
    if (is_unknown_as) {
        num = ++br_tables->stats_table[0].num_bad_inbound_pkt_pid_unknown;
    } else {
        num = ++br_tables->stats_table[0].num_bad_inbound_pkt_pid[asid];
    }

    if (num % 500 == 0) {
        send_attack_summary_to_all_proxies(asid, num, is_unknown_as, br_conf, br_tables);
    }
}

/**
 * Perform PID verification for inbound DATA packet.
 * We need to decide if the PID pointer is valid before the verification:
 * 0 < PID pointer <= n
 * When verifying PID of inbound DATA packet, the PID pointer must not be
 * 0(points to nothing). Invalid PID pointer causes this function to return
 * PID_VERIFY_FAILED.
 *
 * The verification procedure:
 * 1. Take the NID, SID field of the DATA packet.
 * 2. Set PIDo to the PID which (pt - 1) points to if (pt - 1) is not zero.
 * 3. Get PX from the PID which pt points to.
 * 4. Get SK.
 * 5. Calculate the PID = PX + hash(NID, SID, SK, PX, PIDo), and compare it
 * with the PID which pt points to.
 *
 * Pre-conditions:
 *   1. `data_hdr` points to the header of a DATA packet.
 *   2. The packet is received from a border router which is not in the same domain
 *   as this border router. That is to say, the packet is an inbound DATA packet.
 *   Whether a DATA packet is an inbound packet or outbound packet can be determined
 *   by examining the *F* flag. However, checking the *F* flag should be done by
 *   the caller of this function.
 *
 * @param data_hdr
 *   The pointer to the header of the DATA packet.
 * @return
 *   OK if PIDs[pt] is valid
 *   FAILED if PIDs[pt] is invalid
 */
static inline int
verify_pid_inbound(const DATA_front_t *data_hdr, uint32_t sk,
    struct br_conf *br_conf, struct br_tables *br_tables)
{
    /* check if PID pointer is valid */
    if (!(0 < data_hdr->PID_pt && data_hdr->PID_pt <= data_hdr->PID_num)) {
        BR_LOG("verify_pid_inbound(): Invalid PID pointer: pt = %d, nb_pid = %d\n",
            data_hdr->PID_pt, data_hdr->PID_num);
        br_stats_inc_bad_inbound_pid(0, true, br_conf, br_tables);
        return FAILED;
    }

    /* 1. Get NID, SID from DATA header */
    const uint8_t *nid = data_hdr->nid;   // 16 bytes
    const uint8_t *sid = data_hdr->N_sid; // 16 + 20 = 36 bytes

    /* 2. Set PIDo to the PID which (pt - 1) points to if (pt - 1) is not zero. */
    uint32_t pido = 0;
    if (data_hdr->PID_pt - 1 != 0) {
        pido = deref_pid_pt_with_offset(data_hdr, -1);
    }

    /* 3. Get PX from the PID which pt points to. */
    uint32_t pid_a = deref_pid_pt(data_hdr);
    uint16_t px = PID2PX(pid_a);

    /* 4. Get SK. Already in parameter. */

    /* 5. Calculate the PID and compare it with the PID which pt points to.*/
    uint32_t pid_b = calculate_pid(px, nid, sid, sk, pido);
    BR_LOG("PID in packet=%x, calculated PID=%x\n", pid_a, pid_b);
    if (pid_a != pid_b) {
        BR_LOG("verify_pid_inbound failed.\n");
        br_stats_inc_bad_inbound_pid(pid_a, false, br_conf, br_tables);
        return FAILED;
    }

    BR_LOG("verify_pid_inbound succeeded.\n");
    return OK;
}

/**
 * Perform PID verification for outbound DATA packet. Can be invoked when the
 * DATA packet is an outbound packet and the provider of the DATA packet is in
 * another domain.
 *
 * We need to decide if the PID pointer is valid before the verification:
 * 0 < PID pointer < n
 * The PID pointer must not be 0(points to nothing), and must not point to the
 * last PID.
 * Invalid PID pointer causes this function to return PID_VERIFY_FAILED.
 *
 * The verification procedure:
 * 1. Take the NID, SID field of the DATA packet.
 * 2. Set PIDo to the PID which pt points to.
 * 3. Get PX from the PID which (pt + 1) points to.
 * 4. Get SK.
 * 5. Calculate the PID = PX + hash(NID, SID, SK, PX, PIDo), and compare it with
 * the PID which (pt + 1) points to.
 *
 * Pre-conditions:
 *   1. `data_hdr` points to the header of a DATA packet.
 *   2. The packet is received from a border router which is in the same domain
 *   as this border router. That is to say, the packet is an outbound DATA packet.
 *   Whether a DATA packet is an inbound packet or outbound packet can be determined
 *   by examining the *F* flag. However, checking the *F* flag should be done by
 *   the caller of this function.
 *   3. The provider of the DATA packet must be in another domain, or else we have
 *   nothing to verify. The caller must make sure this condition(pt < n) is satisfied
 *   before calling this function. However, this function will still check whether the
 *   condition is satisfied or not even if the condition is listed as a pre-condition
 *   here.
 *
 * @param data_hdr
 *   The pointer to the header of the DATA packet.
 * @return
 *   OK if PIDs[pt+1] is valid
 *   FAILED if PIDs[pt+1] is invalid
 */
static inline int
verify_pid_outbound(const DATA_front_t *data_hdr, uint32_t sk)
{
    /* check if PID pointer is valid. 0 < PID pointer < n. */
    uint8_t pt = data_hdr->PID_pt;
    uint8_t n = data_hdr->PID_num;
    if (!(0 < pt && pt < n)) {
        BR_LOG("verify_pid_outbound(): Invalid PID pointer: pt = %d, nb_pid = %d\n",
            pt, n);
        return FAILED;
    }

    /* 1. Get NID, SID from DATA header */
    const uint8_t *nid = data_hdr->nid;   // 16 bytes
    const uint8_t *sid = data_hdr->N_sid; // 16 + 20 = 36 bytes

    /* 2. Set PIDo to the PID to which pt points.*/
    uint32_t pido = deref_pid_pt(data_hdr);

    /* 3. Get PX from the PID which (pt + 1) points to. */
    uint32_t pid_a = deref_pid_pt_with_offset(data_hdr, 1);
    uint16_t px = PID2PX(pid_a);

    /* 4. Get SK. Already in parameter. */

    /* 5. Calculate the PID and compare it with the PID which pt points to.*/
    uint32_t pid_b = calculate_pid(px, nid, sid, sk, pido);
    if (pid_a != pid_b) {
        BR_LOG("verify_pid_outbound failed.\n");
        return FAILED;
    }

    BR_LOG("verify_pid_outbound succeeded\n");
    return OK;
}

#endif
