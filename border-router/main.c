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
#include <rte_branch_prediction.h>
#include <rte_debug.h>
#include <rte_ethdev.h>
#include "commandline/cli.h"
#include "common.h"
#include "br_conf.h"
#include "br_tables.h"
#include "init.h"
#include "br-rtc.h"

extern struct br_conf g_br_conf;
extern struct br_tables g_br_tables;

#define MAX_LCORE_PARAMS 1024

struct lcore_params {
    struct {
        uint16_t port_id;
        uint8_t queue_id;
        uint8_t lcore_id;
    } data[MAX_LCORE_PARAMS];
    int len;
};

/* Check the validity of queue_id, lcore_id, port_id of lcore_params.
   Returns FAILED on error. */
static int check_lcore_params(struct lcore_params const *lcore_params,
    struct br_conf *br_conf)
{
    int nb_ports = rte_eth_dev_count_avail();
    for (int i = 0; i < lcore_params->len; ++i) {
        uint16_t queue_id = lcore_params->data[i].queue_id;
        if (queue_id >= MAX_RX_QUEUE_PER_PORT) {
            printf("Error: invalid queue number: %d\n", queue_id);
            return FAILED;
        }

        uint32_t lcore_id = lcore_params->data[i].lcore_id;
        if (!rte_lcore_is_enabled(lcore_id)) {
            printf("Error: lcore %d is not enabled in lcore mask\n", lcore_id);
            return FAILED;
        }

        uint16_t port_id = lcore_params->data[i].port_id;
        if ((br_conf->enabled_port_mask & (1 << port_id)) == 0) {
            printf("Error: port %d is not enabled in port mask\n", port_id);
            return FAILED;
        }
        if (port_id >= nb_ports) {
            printf("Error: port %d is not present on the board\n", port_id);
            return FAILED;
        }
    }
    return OK;
}

/* Fill lcore_params. Returns FAILED on error. */
static int parse_config(const char *q_arg, struct lcore_params *lcore_params,
    struct br_conf *br_conf)
{
    char s[256];
    const char *p, *p0 = q_arg;
    char *end;
    enum fieldnames {
        FLD_PORT = 0,
        FLD_QUEUE,
        FLD_LCORE,
        _NUM_FLD
    };
    unsigned long int_fld[_NUM_FLD];
    char *str_fld[_NUM_FLD];

    int lcore_param_len = 0;
    while ((p = strchr(p0,'(')) != NULL) {
        ++p;
        if((p0 = strchr(p,')')) == NULL)
            return FAILED;

        int size = p0 - p;
        if(size >= sizeof(s))
            return FAILED;

        snprintf(s, sizeof(s), "%.*s", size, p);
        if (rte_strsplit(s, sizeof(s), str_fld, _NUM_FLD, ',') != _NUM_FLD)
            return FAILED;
        for (int i = 0; i < _NUM_FLD; i++){
            errno = 0;
            int_fld[i] = strtoul(str_fld[i], &end, 0);
            if (errno != 0 || end == str_fld[i] || int_fld[i] > 255)
                return FAILED;
        }
        if (lcore_param_len >= MAX_LCORE_PARAMS) {
            printf("exceeded max number of lcore params: %d\n", lcore_param_len);
            return FAILED;
        }
        lcore_params->data[lcore_param_len].port_id = (uint8_t)int_fld[FLD_PORT];
        lcore_params->data[lcore_param_len].queue_id = (uint8_t)int_fld[FLD_QUEUE];
        lcore_params->data[lcore_param_len].lcore_id = (uint8_t)int_fld[FLD_LCORE];
        ++lcore_param_len;
    }
    lcore_params->len = lcore_param_len;
    return check_lcore_params(lcore_params, br_conf);
}

/**
 * @brief Returns FAILED on error
 */
static int init_lcore_conf(struct lcore_params const *lcore_params,
    struct br_conf *br_conf)
{
    memset(&br_conf->rtc_qconf, 0, sizeof(br_conf->rtc_qconf));

    /* Convert rx queue information in lcore_params to rtc_qconf and n_port_rx_queues. */
    for (int i = 0; i < lcore_params->len; i++) {
        uint32_t lcore_id = lcore_params->data[i].lcore_id;
        struct rtc_qconf *rtc_qconf = &br_conf->rtc_qconf[lcore_id];
        struct rx_qconf *rx_qconf = &rtc_qconf->rx_qconf;
        int n_rx_queue = rx_qconf->n_queues;
        if (n_rx_queue >= MAX_RX_QUEUE_PER_LCORE) {
            printf("error: too many queues (%u) for lcore: %d\n",
                n_rx_queue + 1, lcore_id);
            return FAILED;
        }
        rx_qconf->queue_list[n_rx_queue].port_id = lcore_params->data[i].port_id;
        rx_qconf->queue_list[n_rx_queue].queue_id = lcore_params->data[i].queue_id;
        rx_qconf->n_queues++;
    }

    /* init rtc_qconf->has_rx_queue and rtc_qconf->has_tx_queue for each lcore  */
    uint32_t lcore_id;
    uint16_t port_id;
    RTE_LCORE_FOREACH(lcore_id) {
        struct rtc_qconf *rtc_qconf = &br_conf->rtc_qconf[lcore_id];
        rtc_qconf->has_rx_queue = (rtc_qconf->rx_qconf.n_queues != 0) ? 1 : 0;
        /* all lcore (except cli lcore) shave tx queue in run-to-completion mode */
        rtc_qconf->has_tx_queue = 1;
    }

    /* br_conf->n_port_rx_queues */
    memset(&br_conf->n_port_rx_queues, 0, sizeof(br_conf->n_port_rx_queues));
    RTE_ETH_FOREACH_DEV(port_id) {
        if (port_not_enabled(br_conf, port_id)) {
            continue;
        }
        int n = 0;
        for (int i = 0; i < lcore_params->len; i++) {
            if (lcore_params->data[i].port_id != port_id) {
                continue;
            }
            if (lcore_params->data[i].queue_id != n) {
                printf("queue ids of the port %d must be in sequence and must "
                    "start with 0\n", lcore_params->data[i].port_id);
                return FAILED;
            }
            n++;
        }
        br_conf->n_port_rx_queues[port_id] = n;
    }

    /* Find a lcore for processing command. */
    RTE_LCORE_FOREACH(lcore_id) {
        struct rtc_qconf *rtc_qconf = &br_conf->rtc_qconf[lcore_id];
        /* all lcore (except cli lcore) shave tx queue in run-to-completion mode */
        if (rtc_qconf->rx_qconf.n_queues == 0) {
            br_conf->cli_enabled = true;
            br_conf->cli_lcore_id = lcore_id;
            rtc_qconf->has_tx_queue = 0;
            rtc_qconf->tx_qconf.n_ports = 0;
            break;
        }
    }
    if (!br_conf->cli_enabled) {
        RTE_LOG(WARNING, BR, "No free lcore to process command. Command line "
            "interface is disabled.\n");
    } else {
        printf("CLI is processing on lcore %d.\n", br_conf->cli_lcore_id);
    }

    /* initialize tx_qconf */
    RTE_ETH_FOREACH_DEV(port_id) {
        if (port_not_enabled(br_conf, port_id)) {
            continue;
        }
        uint16_t queue_id = 0;
        RTE_LCORE_FOREACH(lcore_id) {
            if (br_conf->cli_enabled && br_conf->cli_lcore_id == lcore_id) {
                continue;
            }
            struct tx_qconf *tx_qconf = &br_conf->rtc_qconf[lcore_id].tx_qconf;
            tx_qconf->port_id[tx_qconf->n_ports] = port_id;
            tx_qconf->queue_id[port_id] = queue_id++;
            tx_qconf->n_ports++;
        }
    }

    return OK;
}

/* display usage */
static void print_usage(const char *prgname)
{
    printf("%s [EAL options] --"
        " -p PORTMASK"
        " [-P]"
        " --config (port,queue,lcore)[,(port,queue,lcore)]"
        "  -p PORTMASK: Hexadecimal bitmask of ports to configure\n"
        "  --config (port,queue,lcore): Rx queue configuration\n"
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
    "P"   /* promiscuous */
    ;

#define CMD_LINE_OPT_CONFIG "config"
#define CMD_LINE_OPT_CONFIG_FILE "config-file"
#define CMD_LINE_OPT_DB_FILE "db-file"

enum {
    /* long options mapped to a short option */
    /* first long only option value must be >= 256, so that we won't
     * conflict with short options */
    CMD_LINE_OPT_MIN_NUM = 256,
    CMD_LINE_OPT_CONFIG_NUM,
    CMD_LINE_OPT_CONFIG_FILE_NUM,
    CMD_LINE_OPT_DB_FILE_NUM,
};

static const struct option lgopts[] = {
    {CMD_LINE_OPT_CONFIG, 1, 0, CMD_LINE_OPT_CONFIG_NUM},
    {CMD_LINE_OPT_CONFIG_FILE, 1, 0, CMD_LINE_OPT_CONFIG_FILE_NUM},
    {CMD_LINE_OPT_DB_FILE, 1, 0, CMD_LINE_OPT_DB_FILE_NUM},
    {NULL, 0, 0, 0}
};

/* Parse the argument given in the command line of the application.
    Returns FAILED on error. */
static int parse_args(int argc, char **argv, struct lcore_params *lcore_params,
    struct br_conf *br_conf)
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

        case 'P':
            // Promiscuous mode selected:default
            break;

        /* long options */
        case CMD_LINE_OPT_CONFIG_NUM:
            ret = parse_config(optarg, lcore_params, br_conf);
            if (ret == FAILED) {
                printf("BR: Invalid config\n");
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

    /* init EAL */
    ret = rte_eal_init(argc, argv);
    ASSERT_NOT_NEG(ret, "Invalid EAL parameters\n");

    argc -= ret;
    argv += ret;

    br_conf->force_quit = false;
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* parse application arguments (after the EAL ones) */
    struct lcore_params lcore_params;
    memset(&lcore_params, 0, sizeof(lcore_params));
    ret = parse_args(argc, argv, &lcore_params, &g_br_conf);
    ASSERT_NOT_NEG(ret, "Invalid BR parameters\n");

    ret = init_lcore_conf(&lcore_params, &g_br_conf);
    ASSERT_NOT_NEG(ret, "init_lcore_rx_queues failed\n");

    br_init(br_conf, &g_br_tables);

    /* launch per-lcore init on every lcore */
    RTE_LCORE_FOREACH_WORKER(lcore_id) {
        if (br_conf->cli_enabled && lcore_id == br_conf->cli_lcore_id) {
            rte_eal_remote_launch(cli_main_loop, NULL, lcore_id);
        } else {
            rte_eal_remote_launch(br_main_loop, NULL, lcore_id);
        }
    }

    /* launch on master lcore */
    if (br_conf->cli_enabled && br_conf->cli_lcore_id == rte_lcore_id()) {
        cli_main_loop(NULL);
    } else {
        br_main_loop(NULL);
    }

    ret = 0;
    RTE_LCORE_FOREACH_WORKER(lcore_id) {
        if (rte_eal_wait_lcore(lcore_id) < 0) {
            ret = -1;
            break;
        }
    }

    /* stop ports */
    RTE_ETH_FOREACH_DEV(portid) {
        if ((br_conf->enabled_port_mask & (1 << portid)) == 0)
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
