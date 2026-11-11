#ifndef __NID_H__
#define __NID_H__

#include <stdint.h>
#include <stdio.h>

struct nid {
    uint8_t nid_bytes[16];
} __attribute__((__packed__));

/**
 * Convert NID to string.
 * @param nid
 *   Pointer to a NID structure to be converted.
 * @param s
 *   Pointer to buffer. Size of the buffer should be no less than 64 bytes.
 * @return
 *   The number of bytes of the formatted NID.
 */
static inline int nid_to_string(const struct nid *nid, char *s)
{
    char *p = s;
    for (int i = 0; i < 16; i++) {
        sprintf(p, "%02X", nid->nid_bytes[i]);
        p += 2;
        if (i < 15) {
            *p++ = '-';
        } else {
            *p = '\0';
        }
    }
    return p - s;
}

/**
 * Fast copy a NID.
 *
 * @param nid_from
 *   A pointer to a NID structure holding the NID to copy.
 * @param nid_to
 *   A pointer to a NID structure where to copy the NID.
 */
static inline void nid_copy(const struct nid *__restrict nid_from,
    struct nid *__restrict nid_to)
{
    *nid_to = *nid_from;
}

/**
 * @brief Compare nid_a and nid_b.
 *
 * @param nid_a
 * @param nid_b
 * @return int
 * 	 Returns 0 if nid_a and nid_b are the same else returns non-zero value.
 */
static inline int nid_cmp(const struct nid *__restrict nid_a,
    struct nid *__restrict nid_b)
{
    return memcmp(nid_a, nid_b, sizeof(struct nid));
}

#endif