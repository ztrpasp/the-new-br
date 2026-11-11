#include <openssl/pem.h>
#include <sqlite3.h>
#include <arpa/inet.h>
#include <unistd.h>
#include "br_tables.h"
#include "commandline/cmd_parser.h"

static int load_odct_cb(void *br_tables, int nc, char **value, char **name)
{
    uint8_t pnid[NID_SIZE_IN_BYTES];
    uint8_t lsid[LSID_SIZE_IN_BYTES];
    uint8_t cnid[NID_SIZE_IN_BYTES];
    uint8_t pids[PID_SIZE_IN_BYTES * MAX_NUM_PIDS];
    uint32_t rn;
    str_to_hex(value[0], pnid);
    str_to_hex(value[1], lsid);
    str_to_hex(value[2], cnid);
    const uint32_t num_pids = strlen(value[3]) / (PID_SIZE_IN_BYTES * 2);
    const uint32_t pids_len = PID_SIZE_IN_BYTES * num_pids;
    str_to_hex(value[3], pids);
    sscanf(value[4], "%x", &rn);
    // todo: provider's nid set to null temporarily
    odc_add_entry(&((struct br_tables *)br_tables)->odc_table, /*pnid*/ NULL,
        lsid, cnid, pids, pids_len, rn, ODC_ENTRY_EXPIRE_SECONDS);
    return 0;
}

void load_odct(struct br_conf *br_conf, struct br_tables *br_tables)
{
    sqlite3 *db;
    char *errmsg;
    int rc = sqlite3_open(br_conf->db_file, &db);
    if (rc) {
        sqlite3_close(db);
        rte_exit(EXIT_FAILURE, "Can't open database: %s\n", sqlite3_errmsg(db));
    }

    sqlite3_exec(db, "SELECT * FROM odct;", load_odct_cb,
        /* fisrt argument of callback */ br_tables,
        /* error message*/ NULL);
    sqlite3_close(db);
}

static int load_nid_cb(void *br_tables, int nc, char **value, char **name)
{
    uint8_t nid[SIZE_NID];
    uint32_t ipv4_addr;
    uint8_t sk[SIZE_HOST_SK];
    str_to_hex(value[0], nid);
    inet_pton(AF_INET, value[1], &ipv4_addr);
    ipv4_addr = ntohl(ipv4_addr);
    str_to_hex(value[2], sk);
    nid_add(&((struct br_tables *)br_tables)->nid_table, nid, ipv4_addr, sk);
    return 0;
}

void load_nid(struct br_conf *br_conf, struct br_tables *br_tables)
{
    sqlite3 *db;
    char *errmsg;
    int rc = sqlite3_open(br_conf->db_file, &db);
    if (rc) {
        sqlite3_close(db);
        rte_exit(EXIT_FAILURE, "Can't open database: %s\n", sqlite3_errmsg(db));
    }

    sqlite3_exec(db, "SELECT * FROM nid;", load_nid_cb,
        /* fisrt argument of callback */ br_tables,
        /* error message*/ NULL);
    sqlite3_close(db);
}

/**
 * path must not be NULL.
 * Program will exit if error occurs during the execution of this function.
 */
static void load_rsa_key(const char *path, bool is_pubkey, RSA **rsa)
{
    if (access(path, F_OK) < 0) {
        rte_exit(EXIT_FAILURE, "File %s does not exist.\n", path);
    }
    FILE *fp = fopen(path, "rb");
    ASSERT_NOT_NULL(fp, "Can't open file: %s\n", path);
    if (is_pubkey) {
        *rsa = PEM_read_RSA_PUBKEY(fp, rsa, NULL, NULL);
    } else {
        *rsa = PEM_read_RSAPrivateKey(fp, rsa, NULL, NULL);
    }
    fclose(fp);
    ASSERT_NOT_NULL(*rsa, "Bad %s key: %s\n", is_pubkey ? "public" : "private", path);
}

/**
 * @brief Load BR's public key and private key from file.
 * Generate public key and private key by the following command:
 *   > openssl genrsa -out private-key.pem
 * 	 > openssl rsa -in private-key.pem -outform PEM -pubout -out public-key.pem
 */
void load_rsa_keypair(struct br_conf *br_conf)
{
    printf("Public key and private key are not loaded.\n");
    return;
    load_rsa_key(br_conf->public_key_pem, true, &br_conf->rsa_keypair);
    load_rsa_key(br_conf->private_key_pem, false, &br_conf->rsa_keypair);
    load_rsa_key(br_conf->rm_pubkey_pem, true, &br_conf->rsa_rm_pubkey);
    br_conf->rsa_der_len = i2d_RSA_PUBKEY(br_conf->rsa_keypair, &br_conf->rsa_der); // return length
    ASSERT_NOT_NULL(br_conf->rsa_der, "Unable to convert public key to DER format.\n");
}

/**
 * Load RM list, PX table, NID-IP table, interface list, simplified IP routing table
 */
void load_config(struct br_conf *br_conf, struct br_tables *br_tables)
{
    if (access(br_conf->config_path, F_OK) < 0) {
        printf("No configuration is loaded\n");
        return; // file does not exist.
    }

    printf("Loading configurations from file %s\n", br_conf->config_path);
    FILE *fp;
    #define MAX_LINE_LEN 128
    char line[MAX_LINE_LEN];

    fp = fopen(br_conf->config_path, "r");
    if (fp == NULL) {
        rte_exit(EXIT_FAILURE, "Unable to open config file.\n");
    }
    char *ret = fgets(line, sizeof(line), fp);
    while (ret != NULL) {
        if (line[0] == '#') {
            ret = fgets(line, sizeof(line), fp);
            continue;
        }
        parse_line(line);
        ret = fgets(line, 128, fp);
    }
    fclose(fp);

    printf("Loading configurations from file %s\n", br_conf->db_file);
    load_nid(br_conf, br_tables);
    load_odct(br_conf, br_tables);
    br_conf->cmd_silent = false; // enable command parser output
}