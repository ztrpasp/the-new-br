#include "br_conf.h"

struct br_conf g_br_conf = {
    .mode = RUN_TO_COMPLETION, /* default mode is run-to-completion */
    .odc_enabled = true,
    .pid_verify_enabled = true,
    .interdomain_hmac_verify_enabled = true,
    .arp_enabled = true,
    .cli_enabled = false, /* command line interface */
    .cli_lcore_id = 0,
    .sysname = "[BR]",
    .cmd_exit = false,
    .cmd_silent = true,
    .debugging_on = false,
    .debugging_l2_on = false,
    .debugging_l3_on = false,
    // .br_nid; /* Node identifier of this border router */
    .rm_ip_addr = 0, /* IP address of resource managers */
    .sk = 0,
    .config_path = "./config-br",
    .db_file = "./br.sqlite",
    .force_quit = false,
    // struct rte_ether_addr ports_eth_addr[RTE_MAX_ETHPORTS]
    .ipv4_cksum_tx_offload = true,
    .ipv4_cksum_rx_offload = true,
    // struct rtc_conf rtc_conf[RTE_MAX_LCORE]
    // uint32_t enabled_port_mask
    .tx_pktmbuf_pool = NULL,
    
    /* Public/Private Key */
    .public_key_pem = "./public-key.pem",
    .private_key_pem = "./private-key.pem",
    .rsa_keypair = NULL,
    .rsa_der_len = 0,
    .rsa_der = NULL,

    .rm_pubkey_pem = "./rm-public-key.pem",
    .rsa_rm_pubkey = NULL,
};

bool port_is_enabled(struct br_conf const *br_conf, uint16_t port_id)
{
    return br_conf->enabled_port_mask & (1 << port_id);
}

bool port_not_enabled(struct br_conf const *br_conf, uint16_t port_id)
{
    return !(br_conf->enabled_port_mask & (1 << port_id));
}

