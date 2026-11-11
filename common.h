#ifndef _BR_COMMON_H_
#define _BR_COMMON_H_

#include <stdbool.h>
#include "log.h"

#define RTE_LOGTYPE_BR RTE_LOGTYPE_USER1
#define ASSERT_NOT_NULL(p, ...) if (p == NULL) { \
    rte_exit(EXIT_FAILURE, __VA_ARGS__); \
}
#define ASSERT_NOT_NEG(p, ...) if (p < 0) { \
    rte_exit(EXIT_FAILURE, __VA_ARGS__); \
}
#define ASSERT_LT(p, q, ...) if (p >= q) { \
    rte_exit(EXIT_FAILURE, __VA_ARGS__); \
}
#define ASSERT_GE(p, q, ...) if (p < q) { \
    rte_exit(EXIT_FAILURE, __VA_ARGS__); \
}

#define ASSERT_NOT_LESS_THAN(a, b) if (a < b) { \
    BR_LOG(#a": %d, should be no less than %d\n", a, b);\
    return FAILED; }
#define ASSERT_EQUAL(a, b) if (a != b) {\
    BR_LOG(#a": %d, should be equal to %d\n", a, b);\
    return FAILED;}

/* General indicator of success/failure */
#define FAILED -1
#define OK 0

/* Used to indicate whether a packet should be dropped. */
#define GOOD_PACKET 0x1
#define FREE_THIS_PACKET 0x2
#define ODC_FAILED_PACKET 0x4

#define PID2PX(pid) ((pid) >> 16)

#define IPv4TO4DECIMAL(addr) ((addr) & 0xFF000000) >> 24, ((addr) & 0x00FF0000) >> 16, \
            ((addr) & 0x0000FF00) >> 8, ((addr) & 0x000000FF)

#define IPv6TO16HEX(addr) addr[0], addr[1], addr[2], addr[3], \
            addr[4], addr[5], addr[6], addr[7], \
            addr[8], addr[9], addr[10], addr[11], \
            addr[12], addr[13], addr[14], addr[15]
			

#define IPV6_SIZE 16	/* size of ipv6 address (bytes) */

static void
IPv6(uint8_t *ip, uint8_t b1, uint8_t b2, uint8_t b3, uint8_t b4, uint8_t b5,
		uint8_t b6, uint8_t b7, uint8_t b8, uint8_t b9, uint8_t b10,
		uint8_t b11, uint8_t b12, uint8_t b13, uint8_t b14, uint8_t b15,
		uint8_t b16)
{
	ip[0] = b1;
	ip[1] = b2;
	ip[2] = b3;
	ip[3] = b4;
	ip[4] = b5;
	ip[5] = b6;
	ip[6] = b7;
	ip[7] = b8;
	ip[8] = b9;
	ip[9] = b10;
	ip[10] = b11;
	ip[11] = b12;
	ip[12] = b13;
	ip[13] = b14;
	ip[14] = b15;
	ip[15] = b16;
}

static void
IPv6_copy(uint8_t *ip1, uint8_t *ip2)
{
	ip1[0] = ip2[0];
	ip1[1] = ip2[1];
	ip1[2] = ip2[2];
	ip1[3] = ip2[3];
	ip1[4] = ip2[4];
	ip1[5] = ip2[5];
	ip1[6] = ip2[6];
	ip1[7] = ip2[7];
	ip1[8] = ip2[8];
	ip1[9] = ip2[9];
	ip1[10] = ip2[10];
	ip1[11] = ip2[11];
	ip1[12] = ip2[12];
	ip1[13] = ip2[13];
	ip1[14] = ip2[14];
	ip1[15] = ip2[15];
}

#define IPPREFIX2MASK(prefix) (0xFFFFFFFF << (32 - (prefix)))
#define PX_PREFIX_MASK(px_len) (0xFFFFFFFF << (32 - (px_len)))

#define NID_SIZE_IN_BYTES 16
#define LSID_SIZE_IN_BYTES 20
#define SID_SIZE_IN_BYTES (NID_SIZE_IN_BYTES + LSID_SIZE_IN_BYTES)
#define PID_SIZE_IN_BYTES 4
#define MAX_NUM_PIDS 128
#define SIZE_NID 16
#define SIZE_LSID 20

#define TIMER_RESOLUTION_CYCLES 20000000ULL /* around 10ms at 2Ghz */

#define IPPROTO_COLOR 150
#define IP_DEFTTL  64   /* from RFC 1340. */
#define IP_VERSION 0x40
#define IP_HDRLEN  0x05 /* default IP header length == five 32-bits words. */
#define IP_VHL_DEF (IP_VERSION | IP_HDRLEN)

#define NB_SOCKETS        8

#endif /* _BR_COMMON_H_ */