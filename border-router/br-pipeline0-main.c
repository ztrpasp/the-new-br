#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <unistd.h>
#include <rte_log.h>
#include <rte_eal.h>
#include <rte_launch.h>
#include <rte_lcore.h>
#include <rte_debug.h>
#include <rte_ethdev.h>
#include "commandline/cli.h"
#include "common.h"
#include "br_conf.h"
#include "br_tables.h"
#include "init.h"
#include "br-pipeline0.h"

extern struct br_conf g_br_conf;
extern struct br_tables g_br_tables;

/* Exit on error.*/
static void init_lcore_conf(struct br_conf *br_conf)
{
    uint16_t port_id;
    struct p0_qconf *p0_qconf = &br_conf->p0_qconf;
    struct rx_qconf *rx_qconf = &p0_qconf->rx_qconf;
    struct tx_qconf *tx_qconf = &p0_qconf->tx_qconf;
    struct worker_qconf *workers_qconf = p0_qconf->workers_qconf;

    /* rtc_qconf is not used in pipeline0 mode */
    memset(&br_conf->rtc_qconf, 0, sizeof(br_conf->rtc_qconf));
    memset(&br_conf->n_port_rx_queues, 0, sizeof(br_conf->n_port_rx_queues));
    memset(rx_qconf, 0, sizeof(struct rx_qconf));
    memset(tx_qconf, 0, sizeof(struct tx_qconf));
    memset(workers_qconf, 0, sizeof(p0_qconf->workers_qconf));

    /* rx/tx lcore config */
    RTE_ETH_FOREACH_DEV(port_id) {
        if (port_not_enabled(br_conf, port_id)) {
            continue;
        }
        int n = rx_qconf->n_queues;
        rx_qconf->queue_list[n].port_id = port_id;
        rx_qconf->queue_list[n].queue_id = 0; /* one rx queue per port */
        rx_qconf->n_queues++;

        br_conf->n_port_rx_queues[port_id] = 1;

        tx_qconf->port_id[tx_qconf->n_ports] = port_id;
        tx_qconf->queue_id[port_id] = 0; /* one tx queue per port */
        tx_qconf->n_ports++;
    }

    /* Create rings */
    for (int i = 0; i < p0_qconf->n_workers; i++) {
        int const worker_lcore_id = p0_qconf->worker_lcore_id[i];
        int const socket_id = rte_lcore_to_socket_id(worker_lcore_id);
        char name[32];
        snprintf(name, sizeof(name), "rx ring of worker %d", worker_lcore_id);
        struct rte_ring *rx_ring = rte_ring_create(name, WORKER_RING_SIZE,
            socket_id, RING_F_SP_ENQ | RING_F_SC_DEQ);
        ASSERT_NOT_NULL(rx_ring, "Cannot create rx ring for worker %d", worker_lcore_id);
        snprintf(name, sizeof(name), "tx ring of worker %d", worker_lcore_id);
        struct rte_ring *tx_ring = rte_ring_create(name, WORKER_RING_SIZE,
            socket_id, RING_F_SP_ENQ | RING_F_SC_DEQ);
        ASSERT_NOT_NULL(tx_ring, "Cannot create tx ring for worker %d", worker_lcore_id);
        rx_qconf->rings[worker_lcore_id] = rx_ring;
        tx_qconf->rings[worker_lcore_id] = tx_ring;
        workers_qconf[worker_lcore_id].rx_ring = rx_ring;
        workers_qconf[worker_lcore_id].tx_ring = tx_ring;
    }
}

/* display usage */
static void print_usage(const char *prgname)
{
    printf("%s [EAL options] --"
        " -p PORTMASK"
        " --config (port,queue,lcore)[,(port,queue,lcore)]"
        "  -p PORTMASK: Hexadecimal bitmask of ports to configure\n"
        "  --config_file FILEPATH: configuration file.",
        prgname);
}

/* Returns 0 on error. */
static int parse_portmask(const char *portmask)
{
    char *end = NULL;

    /* parse hexadecimal string */
    uint32_t pm = strtoul(portmask, &end, 16);
    if ((portmask[0] == '\0') || (end == NULL) || (*end != '\0')) {
        pm = 0;
    }

    return pm;
}

static const char short_options[] =
    "p:"  /* portmask */
    ;

#define CMD_LINE_OPT_CONFIG_FILE "config-file"
#define CMD_LINE_OPT_DB_FILE "db-file"

enum {
    /* long options mapped to a short option */
    /* first long only option value must be >= 256, so that we won't
     * conflict with short options */
    CMD_LINE_OPT_MIN_NUM = 256,
    CMD_LINE_OPT_CONFIG_FILE_NUM,
    CMD_LINE_OPT_DB_FILE_NUM,
};

static const struct option lgopts[] = {
    {CMD_LINE_OPT_CONFIG_FILE, 1, 0, CMD_LINE_OPT_CONFIG_FILE_NUM},
    {CMD_LINE_OPT_DB_FILE, 1, 0, CMD_LINE_OPT_DB_FILE_NUM},
    {NULL, 0, 0, 0}
};

/* Parse the argument given in the command line of the application.
    Returns FAILED on error. */
static int parse_args(int argc, char **argv, struct br_conf *br_conf)
{
    int opt, ret;
    char **argvopt;
    int option_index;
    char *prgname = argv[0];

    argvopt = argv;
    while ((opt = getopt_long(argc, argvopt, short_options,
                lgopts, &option_index)) != EOF) {

        switch (opt) {
        /* portmask */
        case 'p':
            br_conf->enabled_port_mask = parse_portmask(optarg);
            if (br_conf->enabled_port_mask == 0) {
                printf("BR: Invalid portmask\n");
                print_usage(prgname);
                return FAILED;
            }
            break;

        case CMD_LINE_OPT_CONFIG_FILE_NUM:
            br_conf->config_path = optarg;
            break;

        case CMD_LINE_OPT_DB_FILE_NUM:
            br_conf->db_file = optarg;
            break;

        default:
            print_usage(prgname);
            return FAILED;
        }
    }

    if (optind >= 0)
        argv[optind - 1] = prgname;

    ret = optind - 1;
    optind = 1; /* reset getopt lib */
    return ret;
}

static void signal_handler(int signum)
{
    if (signum == SIGINT || signum == SIGTERM) {
        g_br_conf.force_quit = true;
    }
}

int main(int argc, char **argv)
{
    int ret;
    uint16_t portid;
    unsigned lcore_id;
    struct br_conf *br_conf = &g_br_conf;

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* init EAL */
    ret = rte_eal_init(argc, argv);
    ASSERT_NOT_NEG(ret, "Invalid EAL parameters\n");

    argc -= ret;
    argv += ret;

    if (rte_lcore_count() < 4) {
        rte_exit(EXIT_FAILURE, "Require no less than 4 lcores, got %d lcores\n",
            rte_lcore_count());
    }
    /* parse application arguments (after the EAL ones) */
    ret = parse_args(argc, argv, br_conf);
    ASSERT_NOT_NEG(ret, "Invalid BR parameters\n");

    struct p0_qconf *p0_qconf = &br_conf->p0_qconf;
    p0_qconf->n_workers = 0;
    br_conf->mode = PIPELINE0;
    /**
     * main lcore runs cli
     * The first lcore in lcore_list: rx thread
     * The second lcore in lcore_list: tx thread
     * The rest of lcores in lcore_list: packet processing threads
     */
    int lcore_list[RTE_MAX_LCORE];
    int n_lcores = 0;

#if defined(NIDT_TEST) || defined(ODCT_TEST)
    RTE_LCORE_FOREACH(lcore_id) {
        if (lcore_id == rte_get_main_lcore()) {
            continue;
        }
        lcore_list[n_lcores++] = lcore_id;
        if (n_lcores > 3) {
            p0_qconf->worker_lcore_id[p0_qconf->n_workers++] = lcore_id;
        }
    }
    ASSERT_GE(n_lcores, 4, "Require no less than 5 lcores, got %d lcores\n", n_lcores + 1);
#else
    RTE_LCORE_FOREACH(lcore_id) {
        if (lcore_id == rte_get_main_lcore()) {
            continue;
        }
        lcore_list[n_lcores++] = lcore_id;
        if (n_lcores > 2) {
            p0_qconf->worker_lcore_id[p0_qconf->n_workers++] = lcore_id;
        }
    }
    ASSERT_GE(n_lcores, 3, "Require no less than 4 lcores, got %d lcores\n", n_lcores + 1);
#endif

    br_conf->cli_enabled = true;
    br_conf->cli_lcore_id = rte_get_main_lcore();
    br_conf->p0_qconf.rx_lcore_id = lcore_list[0];
    br_conf->p0_qconf.tx_lcore_id = lcore_list[1];

    printf("CLI thread is on lcore %d\n", br_conf->cli_lcore_id);
    printf("Rx thread is on lcore %d\n", br_conf->p0_qconf.rx_lcore_id);
    printf("Tx thread is on lcore %d\n", br_conf->p0_qconf.tx_lcore_id);
#if defined(NIDT_TEST) || defined(ODCT_TEST)
    uint32_t test_lcore_id = lcore_list[2];
    printf("Test thread is on lcore %d\n", test_lcore_id);
#endif
    printf("Worker threads: ");
    for (int i = 0; i < p0_qconf->n_workers; i++) {
        printf("%d ", p0_qconf->worker_lcore_id[i]);
    }
    printf("\n\n");

    init_lcore_conf(br_conf);

    br_init(br_conf, &g_br_tables);

    /* launch rx thread */
    rte_eal_remote_launch(br_rx_loop, NULL, br_conf->p0_qconf.rx_lcore_id);

    /* launch tx thread */
    rte_eal_remote_launch(br_tx_loop, NULL, br_conf->p0_qconf.tx_lcore_id);

/* launch test thread */
#if defined(NIDT_TEST)
    rte_eal_remote_launch(br_test_loop, NULL, test_lcore_id);
#elif defined(ODCT_TEST)
    rte_eal_remote_launch(br_odct_test_loop, NULL, test_lcore_id);
#endif

    /* launch worker threads */
    for (int i = 0; i < p0_qconf->n_workers; i++) {
        rte_eal_remote_launch(br_worker_loop, NULL, p0_qconf->worker_lcore_id[i]);
    }

    /* launch CLI on master lcore */
    cli_main_loop(NULL);

    ret = 0;
    RTE_LCORE_FOREACH_WORKER(lcore_id) {
        if (rte_eal_wait_lcore(lcore_id) < 0) {
            ret = -1;
            break;
        }
    }

    /* stop ports */
    RTE_ETH_FOREACH_DEV(portid) {
        if (port_not_enabled(br_conf, portid))
            continue;
        ret = rte_eth_dev_stop(portid);
        if (ret != 0){
            printf("rte_eth_dev_stop: err=%d, port=%u\n", ret, portid);
        }
        rte_eth_dev_close(portid);
    }

    /* clean up the EAL */
    rte_eal_cleanup();

    return ret;
}
