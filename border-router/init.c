#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_mempool.h>
#include <rte_mbuf.h>
#include "br_conf.h"
#include "br_tables.h"
#include "init.h"
#include "commandline/cmd_parser.h"
#include "load_conf.h"

extern struct br_conf g_br_conf;
extern struct br_tables g_br_tables;

static struct rte_mempool * pktmbuf_pool[NB_SOCKETS];

#ifndef PERFORMANCE_TEST
static void gen_rand_mac_addr(struct rte_ether_addr *rand_addr, uint16_t port_id)
{
    printf("Start to generate random mac address for port %d\n", port_id);
    uint8_t digest[16];
    char const *arp_config_file = "./arp-address";
    int ret = access(arp_config_file, F_OK);
    ASSERT_NOT_NEG(ret, "ARP config file 'arp-address' does not exist.\n");
    FILE *fp;
    char line[MAX_LINE_LEN];
    fp = fopen(arp_config_file, "r");
    ASSERT_NOT_NULL(fp, "Unable to open arp config file 'arp-address'.\n");
    char *s = fgets(line, MAX_LINE_LEN, fp);
    ASSERT_NOT_NULL(s, "Bad arp config file 'arp-address'.\n");
    fclose(fp);
    md5((uint8_t *)line, strlen(line), (uint32_t *)digest);
    rand_addr->addr_bytes[0] = 0x00;
    rand_addr->addr_bytes[1] = 0xA0;
    rand_addr->addr_bytes[2] = digest[1];
    rand_addr->addr_bytes[3] = digest[2];
    rand_addr->addr_bytes[4] = digest[3];
    rand_addr->addr_bytes[5] = port_id;
}
#endif

static int init_mem(struct br_conf *br_conf, unsigned nb_mbuf)
{
    char s[64];
    uint32_t lcore_id;
    RTE_LCORE_FOREACH(lcore_id) {
        int socketid = rte_lcore_to_socket_id(lcore_id);
        ASSERT_LT(socketid, NB_SOCKETS, "Socket %d of lcore %u is out of range %d\n",
            socketid, lcore_id, NB_SOCKETS);

        if (pktmbuf_pool[socketid] == NULL) {
            snprintf(s, sizeof(s), "mbuf_pool_%d", socketid);
            pktmbuf_pool[socketid] = rte_pktmbuf_pool_create(s, nb_mbuf,
                MEMPOOL_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, socketid);
            ASSERT_NOT_NULL(pktmbuf_pool[socketid], 
                "Cannot init mbuf pool on socket %d\n", socketid);
            printf("Allocated mbuf pool on socket %d\n", socketid);
        }

        if (br_conf->tx_pktmbuf_pool == NULL) {
            snprintf(s, sizeof(s), "tx_mbuf_pool");
            br_conf->tx_pktmbuf_pool = rte_pktmbuf_pool_create(s, nb_mbuf,
                MEMPOOL_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, socketid);
            ASSERT_NOT_NULL(br_conf->tx_pktmbuf_pool, "Cannot init tx mbuf pool\n");
            printf("Allocated tx mbuf pool\n");
        }
    }
    return 0;
}

static void setup_tables(struct br_tables *br_tables)
{
    printf("\nInitializing tables...\n");
    port_table_init(&br_tables->port_table);
    peer_table_init(&br_tables->peer_table);
    nid_table_init(&br_tables->nid_table);
    arp_table_init(&br_tables->arp_table);
    odc_table_init(&br_tables->odc_table);
    // ipv6_table_init(&br_tables->ipv6_table);
    br_tables->ip_lpm_table = ip_lpm_table_create();
    br_tables->px_table = px_table_create();
    br_tables->addr_num = 0;
    for (uint32_t lcore_id = 0; lcore_id < RTE_MAX_LCORE; lcore_id++) {
        stats_table_init(&br_tables->stats_table[lcore_id]);
    }
}

static struct rte_eth_conf const *default_port_conf()
{
    static struct rte_eth_conf const port_conf = {
        .rxmode = {
            .mq_mode = RTE_ETH_MQ_RX_RSS,
            .max_lro_pkt_size = RTE_ETHER_MAX_LEN,
            //.split_hdr_size = 0,
            .mtu = RTE_ETHER_MTU,
            .offloads = RTE_ETH_RX_OFFLOAD_IPV4_CKSUM,
            //.offloads = 0,
        },
        .rx_adv_conf = {
            .rss_conf = {
                .rss_key = NULL,
                .rss_hf = RTE_ETH_RSS_NONFRAG_IPV4_OTHER,
            },
        },
        .txmode = {
            .mq_mode = RTE_ETH_MQ_TX_NONE,
            .offloads = RTE_ETH_TX_OFFLOAD_IPV4_CKSUM, /**< IP checksum offload enabled */
            //.offloads = 0,
        },
    };
    return &port_conf;
}

/**
 * @brief check the \p dev_info of \p port_id
 * returns dev_info and port_conf
 * 
 * @param port_id 
 * @param dev_info pointer to return value, cannot be NULL
 * @param port_conf pointer to return value, cannot be NULL
 * @param br_conf 
 */
static void check_port_conf(uint16_t port_id, struct rte_eth_dev_info *dev_info, 
    struct rte_eth_conf *port_conf, struct br_conf *br_conf)
{
    *port_conf = *default_port_conf();
    int ret = rte_eth_dev_info_get(port_id, dev_info);
    ASSERT_NOT_NEG(ret, "Error during getting device (port %u) info: %s\n",
        port_id, strerror(-ret));

    if (dev_info->tx_offload_capa & RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE) {
        port_conf->txmode.offloads |= RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE;
    }
    
    if (!(port_conf->rx_adv_conf.rss_conf.rss_hf & dev_info->flow_type_rss_offloads)) {
        printf("RSS HF not supported\n");
    }

    port_conf->rx_adv_conf.rss_conf.rss_hf &= dev_info->flow_type_rss_offloads;

    if (dev_info->max_rx_queues == 1) {
        port_conf->rxmode.mq_mode = RTE_ETH_MQ_RX_NONE;
    }

    if (!(dev_info->tx_offload_capa & RTE_ETH_TX_OFFLOAD_IPV4_CKSUM)) {
        printf("Tx IPv4 checksum offload not supported, port=%d\n", port_id);
        br_conf->ipv4_cksum_tx_offload = false;
    } else {
        br_conf->ipv4_cksum_tx_offload = true;
    }

    if (!(dev_info->rx_offload_capa & RTE_ETH_RX_OFFLOAD_IPV4_CKSUM)) {
        printf("Rx IPv4 checksum offload not supported, port=%d\n", port_id);
        br_conf->ipv4_cksum_rx_offload = false;
    } else {
        br_conf->ipv4_cksum_rx_offload = true;
    }
}

/* Configurable number of RX/TX ring descriptors */
static int adjusted_n_rxd(uint16_t port_id)
{
    uint16_t n_rxd = RTE_TEST_RX_DESC_DEFAULT;
    uint16_t n_txd = RTE_TEST_RX_DESC_DEFAULT;
    int ret = rte_eth_dev_adjust_nb_rx_tx_desc(port_id, &n_rxd, &n_txd);
    ASSERT_NOT_NEG(ret, "Cannot adjust number of descriptors: err=%d, "
        "port=%d\n", ret, port_id);
    return n_rxd;
}

static int adjusted_n_txd(uint16_t port_id)
{
    uint16_t n_rxd = RTE_TEST_RX_DESC_DEFAULT;
    uint16_t n_txd = RTE_TEST_RX_DESC_DEFAULT;
    int ret = rte_eth_dev_adjust_nb_rx_tx_desc(port_id, &n_rxd, &n_txd);
    ASSERT_NOT_NEG(ret, "Cannot adjust number of descriptors: err=%d, "
        "port=%d\n", ret, port_id);
    return n_txd;
}

static void rx_queue_setup(uint32_t lcore_id, uint16_t port_id, uint16_t queue_id)
{
    int const socket_id = rte_lcore_to_socket_id(lcore_id);
    printf("(portid=%d, queueid=%d, socketid=%d); \n", port_id, queue_id, socket_id);
    struct rte_eth_dev_info dev_info;
    int ret = rte_eth_dev_info_get(port_id, &dev_info);
    ASSERT_NOT_NEG(ret, "Error during getting device (port %u) info: %s\n",
        port_id, strerror(-ret));

    struct rte_eth_rxconf rxq_conf = dev_info.default_rxconf;
    rxq_conf.offloads = default_port_conf()->rxmode.offloads;
    ret = rte_eth_rx_queue_setup(port_id, queue_id, adjusted_n_rxd(port_id), 
        socket_id, &rxq_conf, pktmbuf_pool[socket_id]);
    ASSERT_NOT_NEG(ret, "rte_eth_rx_queue_setup: err=%d, port=%d\n", ret, port_id);
}

static void rx_queues_setup(struct br_conf *br_conf)
{
    printf("Initializing rx queues on lcores\n");
    if (br_conf->mode == RUN_TO_COMPLETION) {
        uint32_t lcore_id;
        RTE_LCORE_FOREACH(lcore_id) {
            struct rtc_qconf *rtc_qconf = &br_conf->rtc_qconf[lcore_id];
            struct rx_qconf *rx_qconf = &rtc_qconf->rx_qconf;
            if (!rtc_qconf->has_rx_queue) {
                printf("  lcore %d, no rx queues\n", lcore_id);
                continue;
            }
            for(int i = 0; i < rx_qconf->n_queues; ++i) {
                int const port_id = rx_qconf->queue_list[i].port_id;
                int const queue_id = rx_qconf->queue_list[i].queue_id;
                printf("  lcore %d, n=%d, ", lcore_id, rx_qconf->n_queues);
                rx_queue_setup(lcore_id, port_id, queue_id);
            }
        }
    } else if (br_conf->mode == PIPELINE0) {
        struct rx_qconf const *rx_qconf = &br_conf->p0_qconf.rx_qconf;
        uint32_t rx_lcore_id = br_conf->p0_qconf.rx_lcore_id;
        for(int i = 0; i < rx_qconf->n_queues; ++i) {
            uint16_t port_id = rx_qconf->queue_list[i].port_id;
            uint16_t queue_id = rx_qconf->queue_list[i].queue_id;
            printf("  lcore %d, n=%d, ", rx_lcore_id, rx_qconf->n_queues);
            rx_queue_setup(rx_lcore_id, port_id, queue_id);
        }
    } else {
        rte_exit(EXIT_FAILURE, "%s, line %d: undefined br_mode\n", __func__, __LINE__);
    }
}

static void tx_queue_setup(uint32_t lcore_id, uint16_t port_id, uint16_t queue_id, 
    struct rte_eth_conf const *port_conf, struct rte_eth_dev_info const *dev_info)
{
    uint32_t socket_id = rte_lcore_to_socket_id(lcore_id);
    printf("  lcore %d, tx_queueid=%d, socketid=%d\n", lcore_id, 
        queue_id, socket_id);
    struct rte_eth_txconf txconf = dev_info->default_txconf;
    txconf.offloads = port_conf->txmode.offloads;
    int ret = rte_eth_tx_queue_setup(port_id, queue_id, adjusted_n_txd(port_id), 
        socket_id, &txconf);
    ASSERT_NOT_NEG(ret, "rte_eth_tx_queue_setup: err=%d, port=%d\n", ret, port_id);
}

static void tx_queues_setup(uint16_t port_id, struct br_conf *br_conf,
    struct rte_eth_conf const *port_conf, struct rte_eth_dev_info const *dev_info)
{
    if (br_conf->mode == RUN_TO_COMPLETION) {
        /* init one TX queue per couple (lcore,port) */
        uint16_t queue_id = 0;
        uint32_t lcore_id;
        RTE_LCORE_FOREACH(lcore_id) {
            struct rtc_qconf *rtc_qconf = &br_conf->rtc_qconf[lcore_id];
            struct tx_qconf *tx_qconf = &rtc_qconf->tx_qconf;
            if (!rtc_qconf->has_tx_queue) {
                printf("  lcore %d, no tx queue on this port\n", lcore_id);
                continue;
            }
            tx_queue_setup(lcore_id, port_id, queue_id, port_conf, dev_info);
            queue_id++;
        }
    } else if (br_conf->mode == PIPELINE0) {
        uint32_t tx_lcore_id = br_conf->p0_qconf.tx_lcore_id;
        struct tx_qconf const *tx_qconf = &br_conf->p0_qconf.tx_qconf;
        uint16_t queue_id = tx_qconf->queue_id[port_id];
        tx_queue_setup(tx_lcore_id, port_id, queue_id, port_conf, dev_info);
    } else {
        rte_exit(EXIT_FAILURE, "%s, line %d: undefined br_mode\n", __func__, __LINE__);
    }
}

static void mac_addr_setup(struct br_conf *br_conf, uint16_t port_id)
{
        /* Generate random mac address when running on IAR 2802, since IAR 2082 
         * have same mac addresses. */
    printf("Start mac address setting up\n");
#ifndef PERFORMANCE_TEST
    gen_rand_mac_addr(&br_conf->ports_eth_addr[port_id], port_id);
    int ret = rte_eth_dev_default_mac_addr_set(port_id, &br_conf->ports_eth_addr[port_id]);
    ASSERT_NOT_NEG(ret, "Cannot set MAC address of port %u: err=%d\n",
        port_id, ret);
    printf("MAC address is %s\n", buf);
}
#endif
    rte_eth_macaddr_get(port_id, &br_conf->ports_eth_addr[port_id]);
    char buf[RTE_ETHER_ADDR_FMT_SIZE];
    rte_ether_format_addr(buf, RTE_ETHER_ADDR_FMT_SIZE, &br_conf->ports_eth_addr[port_id]);
    printf("MAC address is %s\n", buf);
}

static void poll_resource_setup(struct br_conf *br_conf)
{
    uint16_t port_id, lcore_id;
    int ret;

    uint32_t const n_lcores = rte_lcore_count();

    /* initialize all ports */
    RTE_ETH_FOREACH_DEV(port_id) {
        /* skip ports that are not enabled */
        if (port_not_enabled(br_conf, port_id)) {
            printf("\nSkipping disabled port %d\n", port_id);
            continue;
        }

        printf("Initializing port %d: ", port_id);
        fflush(stdout);

        /* specified by command line */
        uint8_t n_rx_queue = br_conf->n_port_rx_queues[port_id]; 
        uint8_t n_tx_queue = 0;
        if (br_conf->mode == RUN_TO_COMPLETION) {
            RTE_LCORE_FOREACH(lcore_id) {
                if (br_conf->rtc_qconf[lcore_id].has_tx_queue) {
                    n_tx_queue++;
                }
            }
            n_tx_queue = min(n_tx_queue, MAX_TX_QUEUE_PER_PORT);
        } else if (br_conf->mode == PIPELINE0) {
            n_tx_queue = 1;
        }

        struct rte_eth_conf port_conf;
        struct rte_eth_dev_info dev_info;
        check_port_conf(port_id, &dev_info, &port_conf, br_conf);

        ret = rte_eth_dev_configure(port_id, n_rx_queue, n_tx_queue, &port_conf);
        ASSERT_NOT_NEG(ret, "Cannot configure device: err=%d, port=%d\n",
            ret, port_id);
        printf("n_rxq=%d, n_txq=%d, ", n_rx_queue, n_tx_queue);
        
        mac_addr_setup(br_conf, port_id);
        
        /*
        * This expression is used to calculate the number of mbufs needed
        * depending on user input, taking  into account memory for rx and
        * tx hardware rings, cache per lcore and mtable per port per lcore.
        * RTE_MAX is used to ensure that NB_MBUF never goes below a minimum
        * value of 8192
        */
        int const n_rxd = adjusted_n_rxd(port_id);
        int const n_txd = adjusted_n_txd(port_id);
        int const n_ports = rte_eth_dev_count_avail();
        int nb_mbuf = RTE_MAX(8192,
            ( n_ports * n_rx_queue * n_rxd + n_ports * n_lcores * MAX_PKT_BURST 
            + n_ports * n_tx_queue * n_txd + n_lcores * MEMPOOL_CACHE_SIZE ));
        ret = init_mem(br_conf, nb_mbuf);
        ASSERT_NOT_NEG(ret, "init_mem failed\n");
        
        tx_queues_setup(port_id, br_conf, &port_conf, &dev_info);

        printf("\n");
    }

    rx_queues_setup(br_conf);
}


/* Check the link status of all ports in up to 2s, and print them finally */
static void check_all_ports_link_status(struct br_conf const *br_conf)
{
    int const check_interval = 100; /* 100ms */
    int const max_check_time = 20; /* 2s (20 * 100ms) in total */
    uint16_t port_id;
    
    printf("\nChecking link status");
    fflush(stdout);
    for (int count = 0; count <= max_check_time; count++) {
        if (br_conf->force_quit) {
            return;
        }
        bool all_ports_up = true;
        RTE_ETH_FOREACH_DEV(port_id) {
            if (port_not_enabled(br_conf, port_id)) {
                continue;
            }
            struct rte_eth_link link;
            memset(&link, 0, sizeof(link));
            int ret = rte_eth_link_get_nowait(port_id, &link);
            /* clear all_ports_up flag if any link down */
            if (ret < 0 || link.link_status == RTE_ETH_LINK_DOWN) {
                all_ports_up = false;
                break;
            }
        }

        if (all_ports_up) {
            break;
        } else {
            printf(".");
            fflush(stdout);
            rte_delay_ms(check_interval);
        }
    }

    printf(" done\n");

    RTE_ETH_FOREACH_DEV(port_id) {
        if (port_not_enabled(br_conf, port_id)) {
            continue;
        }
        struct rte_eth_link link;
        memset(&link, 0, sizeof(link));
        int ret = rte_eth_link_get_nowait(port_id, &link);
        
        if (ret < 0) {
            printf("Port %u link get failed: %s\n", port_id, rte_strerror(-ret));
            continue;
        }

        char link_status_text[RTE_ETH_LINK_MAX_STR_LEN];
        rte_eth_link_to_str(link_status_text, sizeof(link_status_text), &link);
        printf("Port %d %s\n", port_id, link_status_text);
    }
}

static void start_ports(struct br_conf const *br_conf)
{
    uint16_t port_id;

    RTE_ETH_FOREACH_DEV(port_id) {
        if (port_not_enabled(br_conf, port_id)) {
            continue;
        }
        /* Start device */
        int ret = rte_eth_dev_start(port_id);
        
        ASSERT_NOT_NEG(ret, "rte_eth_dev_start: err=%d, port=%d\n", ret, port_id);

        // int a = rte_eth_dev_set_link_up(port_id);
        // int b = rte_eth_dev_set_link_down(port_id);
        // int c = rte_eth_dev_set_link_up(port_id);

        

        /* Put device in promiscuous mode. */
        ret = rte_eth_promiscuous_enable(port_id);
        ASSERT_NOT_NEG(ret, "rte_eth_promiscuous_enable: err=%s, port=%u\n",
            rte_strerror(-ret), port_id);
    }
}

void br_init(struct br_conf *br_conf, struct br_tables *br_tables)
{
    /* init RTE timer library */
    rte_timer_subsystem_init();
    poll_resource_setup(br_conf);
    setup_tables(br_tables);
    start_ports(br_conf);
    check_all_ports_link_status(br_conf);

    /* Load RM list, PX table, NID-IP table, interface list, simplified IP routing table */
    load_config(br_conf, br_tables);
    load_rsa_keypair(br_conf);
}
