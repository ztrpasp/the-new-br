#ifndef __BR_INIT_H__
#define __BR_INIT_H__
#include <rte_common.h>

#define RTE_TEST_RX_DESC_DEFAULT 1024
#define RTE_TEST_TX_DESC_DEFAULT 1024
#define MAX_TX_QUEUE_PER_PORT RTE_MAX_ETHPORTS
#define MAX_RX_QUEUE_PER_PORT 128
#define MEMPOOL_CACHE_SIZE 256

void br_init(struct br_conf *br_conf, struct br_tables *br_tables);

#endif