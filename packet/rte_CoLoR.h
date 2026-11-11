#ifndef _RTE_COLOR_H_
#define _RTE_COLOR_H_

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rte_memory.h>
#include <rte_memcpy.h>
#include <rte_malloc.h>
#include <rte_mempool.h>

// version
#define COLOR_PROTOCOL 0x7

// package
#define COLOR_ANN_TYPE 0x1
#define COLOR_GET_TYPE 0x2
#define COLOR_DATA_TYPE 0x3
#define COLOR_CONTROL_TYPE 0x4

// length of optional fields (byte)
#define COLOR_SK_LEN 4
#define COLOR_PX_LEN 2
#define COLOR_PID_LEN 4

#define COLOR_N_SID_LEN 16
#define COLOR_L_SID_LEN 20
#define COLOR_SID_LEN (COLOR_N_SID_LEN + COLOR_L_SID_LEN)
#define COLOR_NID_LEN 16

#define COLOR_PUBLICKEY_FRONT_LEN 2
#define COLOR_QOS_FRONT_LEN 1
#define COLOR_SEG_ID_LEN 4
#define COLOR_HMAC_LEN 4
#define COLOR_MINIMAL_PID_CP_LEN 2

// flag F
#define COLOR_FLAG_F(flag) ((flag) & 0x80)
#define COLOR_REV_FLAG_F(flag) ((flag) ^ 0x80)
#define COLOR_DATA_SET_FLAG_F(flag) ((flag) | 0x80)

// flag of GET packet
#define COLOR_GET_FLAG_F(flag) COLOR_FLAG_F(flag)
#define COLOR_GET_REV_FLAG_F(flag) COLOR_REV_FLAG_F(flag)
#define COLOR_GET_FLAG_K(flag) ((flag) & 0x40)
#define COLOR_GET_FLAG_Q(flag) ((flag) & 0x20)
#define COLOR_GET_FLAG_S(flag) ((flag) & 0x10)
#define COLOR_GET_FLAG_A(flag) ((flag) & 0x08)

// flag of DATA packet
#define COLOR_DATA_FLAG_F(flag) COLOR_FLAG_F(flag)
#define COLOR_DATA_REV_FLAG_F(flag) COLOR_REV_FLAG_F(flag)
#define COLOR_DATA_FLAG_B(flag) ((flag) & 0x40)
#define COLOR_DATA_FLAG_R(flag) ((flag) & 0x20)
#define COLOR_DATA_FLAG_M(flag) ((flag) & 0x10)
#define COLOR_DATA_FLAG_Q(flag) ((flag) & 0x08)
#define COLOR_DATA_FLAG_C(flag) ((flag) & 0x04)
#define COLOR_DATA_FLAG_S(flag) ((flag) & 0x02)

// flag of ANN packet
#define COLOR_ANN_FLAG_F(flag) COLOR_FLAG_F(flag)
#define COLOR_ANN_REV_FLAG_F(flag) COLOR_REV_FLAG_F(flag)
#define COLOR_ANN_FLAG_K(flag) ((flag) & 0x40)
#define COLOR_ANN_FLAG_P(flag) ((flag) & 0x20)

#define COLOR_ANN_FLAG_N(flag) ((flag) & 0x80)
#define COLOR_ANN_FLAG_L(flag) ((flag) & 0x40)
#define COLOR_ANN_FLAG_I(flag) ((flag) & 0x20)

/* ------------------------------Package format of ANN---------------------------------- */
// ANN = front + ANN_unit[unit_num] + (publickey) + (AS_PATH) + PX[px_num]
// flag = F/K/P
struct ANN_front
{
    uint8_t version_package;
    uint8_t ttl;
    uint16_t pkg_len;
    uint16_t checksum;
    uint8_t flag;
    uint8_t unit_px_num;
} __attribute__((__packed__));
typedef struct ANN_front ANN_front_t;

// ANN_unit = front + (N_sid) + (L_sid) + (nid) + ANN_strategy_unit[strategy_num]
struct ANN_unit_front
{
    uint8_t flag;
    uint8_t unit_len;
    uint8_t strategy_num;
} __attribute__((__packed__));
typedef struct ANN_unit_front ANN_unit_front_t;

// ANN_strategy_unit = front + value[length]
struct ANN_strategy_front
{
    uint8_t tag;
    uint8_t len;
} __attribute__((__packed__));
typedef struct ANN_strategy_front ANN_strategy_front_t;

/* ------------------------------Package format of GET---------------------------------- */
// GET = front + (publickey) + (QoS) + (Seg_ID) + PID[PID_num] + (PID_RES)
// flag = F/K/Q/S/A
struct GET_front
{
    uint8_t version_package;
    uint8_t ttl;
    uint16_t pkg_len;
    uint16_t checksum;
    uint16_t MTU;
    uint8_t PID_num;
    uint8_t flag;
    uint16_t Minimal_PID_CP;
    uint8_t N_sid[16];
    uint8_t L_sid[20];
    uint8_t nid[16];
} __attribute__((__packed__));
typedef struct GET_front GET_front_t;

/* ------------------------------Package format of DATA---------------------------------- */
// DATA = front + (QoS) + (HMAC) + (Seg_ID) + (Minimal_PID_CP) + PID[PID_num]
// flag = F/B/R/M/Q/C/S
struct DATA_front
{
    uint8_t version_package;
    uint8_t ttl;
    uint16_t pkg_len;
    uint16_t checksum;
    uint8_t header_len;
    uint8_t PID_pt;
    uint8_t PID_num;
    uint8_t flag;
    uint8_t N_sid[16];
    uint8_t L_sid[20];
    uint8_t nid[16];
} __attribute__((__packed__));
typedef struct DATA_front DATA_front_t;

struct CONTROL_front
{
    uint8_t version_package;
    uint8_t ttl;
    uint16_t checksum;
    uint8_t header_len;
    uint8_t tag;
    uint16_t len;
} __attribute__((__packed__));
typedef struct CONTROL_front CONTROL_front_t;

/* ------------------------------Function of CoLoR---------------------------------- */

#define DEFAULT_TTL 128

#define GET_PROTOCOL_TYPE(x) (((x)&0xf0)>>4)
#define GET_PACKAGE_TYPE(x) ((x)&0x0f)

#define GET_ANN_UNIT_NUM(x) ((x)>>4)
#define ADD_ANN_UNIT_NUM(x) ((x)=(x)+0x10)
#define GET_ANN_PX_NUM(x) ((x)&0x0f)
#define ADD_ANN_PX_NUM(x) ((x)=(x)+0x01)

#define CHECK_ANN_UNIT_FLAG_N(flag) (((flag)&0x80)==0x80)
#define CHECK_ANN_UNIT_FLAG_L(flag) (((flag)&0x40)==0x40)
#define CHECK_ANN_UNIT_FLAG_I(flag) (((flag)&0x20)==0x20)
#define GET_ANN_UNIT_FLAG_AM(flag) (((flag)<<3)>>6)
#define SET_ANN_UNIT_FLAG_N(flag) ((flag)=((flag)|0x80))
#define SET_ANN_UNIT_FLAG_L(flag) ((flag)=((flag)|0x40))
#define SET_ANN_UNIT_FLAG_I(flag) ((flag)=((flag)|0x20))

#define CHECK_FLAG_F(flag) (((flag)&0x80)==0x80)
#define REVERSE_FLAG_F(flag) ((flag)=((flag)^0x80))

// stage 1 & 2
#define CONTROL_TAG_SYN_RM_CONFIG 1
#define ACK_CONTROL_TAG_SYN_RM_CONFIG 2
#define CONTROL_TAG_SYN_BR_CONFIG 3
#define ACK_CONTROL_TAG_SYN_BR_CONFIG 4
#define CONTROL_TAG_SYN_RM_STRATEGY 10
#define ACK_CONTROL_TAG_SYN_RM_STRATEGY 11
// stage 3
#define CONTROL_TAG_REG_PROXY 5
#define CONTROL_TAG_RET_PROXY_CONFIG 6
#define CONTROL_TAG_ACK_PROXY_CONFIG 7
#define CONTROL_TAG_SYN_BR_PROXY_CONFIG 8
#define ACK_CONTROL_TAG_SYN_BR_PROXY_CONFIG 9

#define CONTROL_TAG_ODC 16
#define CONTROL_TAG_ODC_WARNING 17
#define CONTROL_TAG_ATTACK_WARNING 18

uint16_t calculate_cksum(uint16_t *buf, int count);

// tool
void show_segment(uint8_t *buf, int len);

void ANN_init(uint8_t *buf);
int ANN_update_checksum(uint8_t *buf);
int ANN_check_checksum(uint8_t *buf);
void ANN_append_unit(uint8_t *buf, uint8_t *unit);
void ANN_unit_construct(uint8_t *ubuf, uint8_t *n_sid, uint8_t *l_sid, uint8_t *nid);
void ANN_unit_append_strategy(uint8_t *ubuf, uint8_t t, uint8_t l, uint8_t *v);
void ANN_append_px(uint8_t *buf, uint16_t px);
int ANN_get_pkg_length(uint8_t *buf);

void GET_init(uint8_t *buf, uint8_t *n_sid, uint8_t *l_sid, uint8_t *nid);
void GET_append_pid(uint8_t *buf, uint32_t pid);
uint32_t GET_calculate_PID(uint8_t *pkt, uint32_t sk, uint16_t to_px);
void GET_update_checksum(uint8_t *buf);
int GET_check_checksum(uint8_t *buf);

void DATA_init(uint8_t *buf, uint8_t *n_sid, uint8_t *l_sid, uint8_t *nid);
void DATA_append_pid(uint8_t *buf, uint32_t pid);
void DATA_append_data(uint8_t *buf, uint8_t *data, int len);
void DATA_update_checksum(uint8_t *buf);
int DATA_check_checksum(uint8_t *buf);

int CoLoR_get_pkg_protocol(uint8_t *buf);
int CoLoR_get_pkg_type(uint8_t *buf);
int CoLoR_get_pkg_length(uint8_t *buf);
void CoLoR_show_pkg(uint8_t *buf);

#endif