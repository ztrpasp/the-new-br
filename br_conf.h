#ifndef __BR_CONF_H__
#define __BR_CONF_H__

#include <stdbool.h>
#include <stdint.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_mempool.h>
#include <openssl/rsa.h>
#include "identifier/nid.h"

#define BURST_TX_DRAIN_US 100 /* TX drain every ~100us */
#define MAX_PKT_BURST     32
#define MAX_RX_QUEUE_PER_LCORE 16
/* Try to avoid TX buffering if we have at least MAX_TX_BURST packets to send. */
#define	MAX_TX_BURST	  (MAX_PKT_BURST / 2)

struct pkt_list {
    uint16_t len;
    struct rte_mbuf *pkts[MAX_PKT_BURST];
};

struct lcore_rx_queue {
    uint16_t port_id;
    uint16_t queue_id;
};

struct rx_qconf {
    uint16_t n_queues;
    struct lcore_rx_queue queue_list[MAX_RX_QUEUE_PER_LCORE];
    struct rte_ring *rings[RTE_MAX_LCORE];
};

struct tx_qconf {
    uint16_t n_ports;
    uint16_t port_id[RTE_MAX_ETHPORTS];
    uint16_t queue_id[RTE_MAX_ETHPORTS]; /* indexed by port_id */
    struct rte_ring *rings[RTE_MAX_LCORE];
    struct pkt_list tx_mbufs[RTE_MAX_ETHPORTS];
};

struct rtc_qconf {
    uint8_t has_rx_queue : 1;
    uint8_t has_tx_queue : 1;
    struct rx_qconf rx_qconf __rte_cache_aligned;
    struct tx_qconf tx_qconf __rte_cache_aligned;
};

#define WORKER_RING_SIZE 128
struct worker_qconf {
    struct rte_ring *rx_ring;
    struct rte_ring *tx_ring;
    struct pkt_list tx_mbufs;
};

/* pipeline0 */
struct p0_qconf {
    uint32_t tx_lcore_id;
    uint32_t rx_lcore_id;

    /* lcore_id of workers */
    uint16_t n_workers;
    uint32_t worker_lcore_id[RTE_MAX_LCORE];

    struct rx_qconf rx_qconf __rte_cache_aligned;
    struct tx_qconf tx_qconf __rte_cache_aligned;
    struct worker_qconf workers_qconf[RTE_MAX_LCORE];
};

enum br_mode { RUN_TO_COMPLETION, PIPELINE0 };

struct br_conf {
    int mode;
    bool odc_enabled;     /* outbound data control enabled */
    bool pid_verify_enabled;
    bool interdomain_hmac_verify_enabled;
    bool arp_enabled;
    bool cli_enabled;     /* command line interface */
    uint32_t cli_lcore_id;
    char sysname[64];
    char sysname_with_brackets[64];
    bool cmd_exit;
    bool cmd_silent;
    bool debugging_on;
    bool debugging_l2_on;    /* debug layer 2*/
    bool debugging_l3_on;    /* debug layer 2*/
    struct nid br_nid;    /* Node identifier of this border router */
    uint32_t rm_ip_addr;  /* IP address of resource managers */
    uint32_t sk;
    const char *config_path;
    const char *db_file;
    volatile bool force_quit;
    struct rte_ether_addr ports_eth_addr[RTE_MAX_ETHPORTS];
    bool ipv4_cksum_tx_offload; /* IPv4 checksum offload */
    bool ipv4_cksum_rx_offload;
    struct rtc_qconf rtc_qconf[RTE_MAX_LCORE];
    struct p0_qconf p0_qconf; /* pipeline0 lcore conf */
    uint8_t n_port_rx_queues[RTE_MAX_ETHPORTS];
    uint32_t enabled_port_mask; /* mask of enabled ports */
    struct rte_mempool *tx_pktmbuf_pool; /* mempool for ARP packet */
    
    /* Public/Private Key */
    const char *public_key_pem;
    const char *private_key_pem;
    RSA *rsa_keypair;
    int rsa_der_len;
    uint8_t *rsa_der;

    const char *rm_pubkey_pem;
    RSA *rsa_rm_pubkey;
};

bool port_is_enabled(struct br_conf const *br_conf, uint16_t port_id);
bool port_not_enabled(struct br_conf const *br_conf, uint16_t port_id);

#endif