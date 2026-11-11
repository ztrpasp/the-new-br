#ifndef __PID_H__
#define __PID_H__

#include <stdint.h>
#include <rte_byteorder.h>
#include "rte_CoLoR.h"
#include "../common.h"
#include "../log.h"
#include "../crypto/md5.h"

/**
 * Calculate the offset of (PID pointer + `offset`) in `data_hdr`.
 *
 * Pre-conditions:
 *   1. `data_hdr` points to a DATA packet.
 *   2. All fields of the header are in little endian.
 *   3. (PID pointer + offset) points to a PID. (0 < (pt + offset) <= n)
 *
 * @param data_hdr
 *   The pointer to the header of the DATA packet.
 * @return
 *   The offset of the PID to which PID pointer points.
 */
static inline uint32_t
calculate_pid_pt_offset(const DATA_front_t *data_hdr, int offset)
{
    /* R = 0: offset = header_length - (n - (pt + offset) + 1) * 4
     * R = 1: offset = header_length - (n - (pt + offset) + 2) * 4
     */
    uint16_t pid_offset;
    uint8_t n = data_hdr->PID_num;
    uint8_t pt = data_hdr->PID_pt + offset;
    if (COLOR_DATA_FLAG_R(data_hdr->flag)) {
        pid_offset = data_hdr->header_len - ((n - pt + 2) * 4);
    } else {
        pid_offset = data_hdr->header_len - ((n - pt + 1) * 4);
    }
    return pid_offset;
}

/**
 * Dereference PID pointer.
 *
 * Pre-conditions:
 *   1. `data_hdr` points to a DATA packet.
 *   2. All fields of the header are in little endian.
 *   3. (PID pointer + offset) points to a PID. (0 < (pt + offset) <= n)
 *
 * @param data_hdr
 *   The pointer to the header of the DATA packet.
 * @param offset
 *   The offset of PID pointer.
 * @return
 *   The PID to which (PID pointer + offset) in DATA packet points.
 */
static inline uint32_t
deref_pid_pt_with_offset(const DATA_front_t *data_hdr, int offset)
{
    uint16_t pid_offset = calculate_pid_pt_offset(data_hdr, offset);
    uint32_t pid = *(uint32_t *)((uint8_t *)data_hdr + pid_offset);
    return rte_le_to_cpu_32(pid);
}

/**
 * Dereference PID pointer.
 *
 * Pre-conditions:
 *   1. `data_hdr` points to a DATA packet.
 *   2. All fields of the header are in little endian.
 *   3. PID pointer points to a PID. (0 < pt <= n and n > 0)
 *
 * @param data_hdr
 *   The pointer to the header of the DATA packet.
 * @return
 *   The PID to which the PID pointer in DATA packet points.
 */
static inline uint32_t
deref_pid_pt(const DATA_front_t *data_hdr)
{
    return deref_pid_pt_with_offset(data_hdr, 0);
}

/**
 * Calculate PID = PX + hash(NID, SID, SK, PX, PIDo)
 *
 * Return a 32-bit value with the highest 16-bit set to PX and the lowest 16-bit
 * set to the lowest 16-bit of hash(NID, SID, PIDo, SK, PX).
 *
 * The NID, SID, PIDo, SK, PX are concatenated to form the message. PIDo is
 * ignored if PIDo is 0.
 *
 * The byte order of pido, sk, px is little endian. The byte order of the lower
 * two bytes of PID is also little endian.
 *
 * MD5 hash is used.
 *
 * @param px
 *   2 bytes PX
 * @param nid
 *   The pointer to NID. The length of NID is 16 bytes.
 * @param sid
 *   The pointer to SID. The length of SID is 16 + 20 = 36 bytes.
 * @param sk
 *   4 bytes SK
 * @param pido
 *   PIDo
 * @return
 *   A 32-bit value with the highest 16-bit set to PX and the lowest 16-bit
 *   set to the lowest 16-bit of hash(NID, SID, PIDo, SK, PX).
 */
static inline uint32_t
calculate_pid(uint16_t px, const uint8_t *nid, const uint8_t *sid, uint32_t sk, uint32_t pido)
{
    // NID 16 bytes + SID 36 bytes + PIDo 4 bytes + SK 4 bytes + PX 2 bytes = 62B
    uint8_t msg[128] __rte_cache_aligned;
    uint8_t digest[16] = {0};
    uint32_t pid;
    rte_memcpy(msg, nid, 16);
    rte_memcpy(msg + 16, sid, 36);
    if (pido != 0) {
        *(uint32_t *)(msg + 52) = rte_cpu_to_le_32(pido);
        *(uint32_t *)(msg + 56) = rte_cpu_to_le_32(sk);
        *(uint16_t *)(msg + 60) = rte_cpu_to_le_16(px);
        md5(msg, 62, (uint32_t *)digest);
    } else {
        *(uint32_t *)(msg + 52) = rte_cpu_to_le_32(sk);
        *(uint16_t *)(msg + 56) = rte_cpu_to_le_16(px);
        md5(msg, 58, (uint32_t *)digest);
    }
    pid = ((uint32_t)px << 16) | ((uint32_t)(digest[15]) << 8) | (digest[14]);
    return pid;
}

/**
 * Calculate backward PID for any DATA packet that needs ACK.
 *
 * The calculation procedure:
 * 1. Take the NID, SID field of the DATA packet.
 * 2. Set PIDo to the PID which (pt + 2) points to if (pt + 2) <= n.
 * 3. Get PX from the PID which pt points to.
 * 4. Get SK.
 * 5. Calculate the PID = PX + hash(NID, SID, SK, PX, PIDo).
 *
 * Pre-conditions:
 *   1. `data_hdr` points to the header of a DATA packet.
 *   2. The border router received the packet from a host or another border router
 *   in the same domain.
 *   3. 0 < PID pointer < number of PIDs
 *
 * Post-conditions:
 *   None. This function does NOT modify the packet. The calculated backward PID
 *   is returned to the caller and it's the caller's responsibility to determine
 *   whether backward PID calculation is necessary and to insert the backward PID
 *   to the DATA packet.
 *
 * @param data_hdr
 *   The pointer to the header of the DATA packet.
 * @param ppid
 *   The pointer to backward PID. The result is returned by `ppid`.
 * @return
 *   - OK if calculation succeeded.
 *   - FAILED if PID pointer is not valid or PX entry is not found.
 */
static inline int
calculate_backward_pid(const DATA_front_t *data_hdr, uint32_t *ppid, uint32_t sk)
{
    /* check if PID pointer is valid. */
    uint8_t pt = data_hdr->PID_pt;
    uint8_t n = data_hdr->PID_num;
    if (!(0 < pt && pt <= n)) {
        BR_LOG("calculate_backward_pid(): Invalid PID pointer: pt = %d, "
            "nb_pid = %d\n", pt, n);
        return FAILED;
    }

    /* 1. Get NID, SID from DATA header */
    const uint8_t *nid = data_hdr->nid + 16;   // use provider NID, 16 bytes
    const uint8_t *sid = data_hdr->N_sid; // 16 + 20 = 36 bytes

    /* 2. Set PIDo to the PID which (pt + 2) points to if (pt + 2) <= n + 1.
       pt + 2 <= n + 1 -> pt < n*/
    uint32_t pido = 0;
    if (pt < n) {
        pido = deref_pid_pt_with_offset(data_hdr, 2);
    }

    /* 3. Get PX from the PID which pt points to. */
    uint16_t px = PID2PX(deref_pid_pt(data_hdr));

    /* 4. Get SK. Already in parameter. */

    /* 5. Calculate the backward PID.*/
    *ppid = calculate_pid(px, nid, sid, sk, pido);

    return OK;
}

/* outbound data control */
static inline uint32_t calculate_pids_hash_in_hdr(const DATA_front_t *data_hdr)
{
    uint8_t msg[1024];
    uint32_t digest[4] = {0};

    const uint8_t n = data_hdr->PID_num;

    /* calculate pids_hash */
    for (int i = 1; i <= n; i++) {
        uint8_t pt = i;
        uint16_t pid_offset;
        if (COLOR_DATA_FLAG_R(data_hdr->flag)) {
            pid_offset = data_hdr->header_len - ((n - pt + 2) * 4);
        } else {
            pid_offset = data_hdr->header_len - ((n - pt + 1) * 4);
        }
        uint32_t pid = rte_le_to_cpu_32(*(uint32_t *)((uint8_t *)data_hdr + pid_offset));
        *(uint32_t *)(msg + (i - 1) * 4) = pid;
    }
    md5(msg, n * sizeof(uint32_t), digest);

    return digest[3];
}

static inline uint32_t calculate_pids_hash(const uint8_t *pids, uint32_t len)
{
    uint32_t digest[4] = {0};
    md5(pids, len, digest);
    return digest[3];
}

#endif