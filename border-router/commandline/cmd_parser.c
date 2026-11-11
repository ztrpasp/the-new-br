#include <rte_debug.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_cycles.h>
#include <rte_mbuf.h>
#include <rte_arp.h>
#include <rte_ip.h>
#include <rte_tcp.h>
#include <rte_udp.h>
#include <rte_hash.h>
#include <rte_jhash.h>
#include <rte_malloc.h>
#include <rte_timer.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <termios.h>
#include <errno.h>
#include <string.h>
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>

#include "../br.h"
#include "cli.h"
#include "cmd_parser.h"
#include "../utils/utils.h"
#include "../ansi_escape_seq.h"
#include "../profiler/profiler.h"
#include "../common.h"
#include "../identifier/nid.h"
#include "../br_conf.h"
#include "../br_tables.h"
#include "../packet/ctrl.h"
#include "../ip/ip.h"

static char line[MAX_LINE_LEN];
static int p;
static int p_last;

/* Whether config file is read-only */
static bool read_only = false;

extern struct br_conf g_br_conf;
extern struct br_tables g_br_tables;

static void error(const char *msg, int pos)
{
    if (g_br_conf.cmd_silent) {
        return;
    }
    char buf[256] = {0};
    if (pos < 0) {
        strcat(buf, BRED "Error: " RESET_STYLE);
        strcat(buf, msg);
        print_to_cli(buf);
        return;
    }
    int idx = 0;
    for (size_t i = 0; i < strlen(g_br_conf.sysname_with_brackets) + pos; i++) {
        buf[idx++] = ' ';
    }
    strcat(buf, "^\r\n" BRED "Error: " RESET_STYLE);
    strcat(buf, msg);
    print_to_cli(buf);
}

static void info(const char *msg)
{
    if (g_br_conf.cmd_silent) {
        return;
    }
    print_to_cli(msg);
}

/** peek char */
static char peekc(void)
{
    return line[p];
}

static char nextc(void)
{
    if (line[p] == '\0') {
        return '\0';
    }
    return line[p++];
}

static void skip_spaces(void)
{
    while (peekc() == ' ' || peekc() == '\t') {
        nextc();
    }
}

static void save_pos(void)
{
    p_last = p;
}

static int last_pos(void)
{
    return p_last;
}

static int cur_pos(void)
{
    return p;
}

/* keyword is composed of letters or numbers */
static const char *keyword(void)
{
    static char buf[MAX_LINE_LEN + 1];
    int i = 0;

    save_pos();
    while (is_alpha(peekc())) {
        buf[i++] = nextc();
    }
    buf[i] = '\0';

    return buf;
}

/* Pre-conditions: peekc() is digit */
static int number(void)
{
    int value = 0;
    char c;

    save_pos();
    while (isdigit(peekc())) {
        c = nextc();
        value = value * 10 + c - '0';
    }

    return value;
}

static bool is_hex_digit(char c)
{
    return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/* Pre-conditions: 0 < num_bytes <= 4*/
static bool hex_number(uint32_t num_bytes, uint32_t *value)
{
    char buf[MAX_LINE_LEN] = {0};
    int i = 0;

    save_pos();
    while (is_hex_digit(peekc())) {
        buf[i++] = tolower(nextc());
    }
    if (strlen(buf) != num_bytes * 2) {
        return false;
    }
    sscanf(buf, "%x", value);

    return true;
}

/**
 * @brief A sequence of hex digits
 *
 * @param p pointer to the array to store hex bytes, the array must be of size \p size
 * @param size size of the hex bytes
 * @return bool false if the size of hex bytes read is greater than \p size or equals 0
 */
static bool hex_bytes(uint8_t *p, uint32_t size)
{
    char buf[MAX_LINE_LEN] = {0};
    int i = 0;

    save_pos();
    while (is_hex_digit(peekc())) {
        buf[i++] = tolower(nextc());
    }
    if ((strlen(buf) + 1) / 2 > size || strlen(buf) == 0) {
        return false;
    }
    str_to_hex(buf, p + size - (strlen(buf) + 1) / 2);
    return true;
}

static bool match(char c)
{
    if (peekc() == c) {
        nextc();
        return true;
    } else {
        return false;
    }
}

/* Pre-conditions: peekc() is digit */
static bool ipv4_address(uint32_t *ipv4_addr)
{
    save_pos();
    *ipv4_addr = 0;
    for (int i = 0; i < 4; i++) {
        int a = number();
        if (a > 255)
            return false;
        *ipv4_addr |= a << (24 - i * 8);
        if (i != 3) {
            if (match('.') == false)
                return false;
            if (!isdigit(peekc())) {
                return false;
            }
        }
    }
    char buf[128];
    sprintf(buf, "received ipv4 addr: %d.%d.%d.%d\r\n", IPv4TO4DECIMAL(*ipv4_addr));
    print_to_cli(buf);
    return true;
}

static bool cidr_block(uint32_t *ipv4_addr, int *prefix)
{
    int pos = p;
    if (!ipv4_address(ipv4_addr)) {
        error("Expect a valid IPv4 address at '^' position.\n", last_pos());
        return false;
    }
    if (!match('/')) {
        error("Expect '/' at '^' position.\n", cur_pos());
        return false;
    }
    if (!isdigit(peekc())) {
        error("Expect length of IP address mask at '^' position.\n", cur_pos());
        return false;
    }
    *prefix = number();
    if (*prefix > 32) {
        error("Expect length of IP address mask at '^' position.\n", last_pos());
        return false;
    }
    return true;
}

static bool ip_addr_str(void)
{
    const char *s = keyword();
    if (!streql(s, "ip")) {
        error("Expect \"ip\" at '^' position.\n", p - strlen(s));
        return false;
    }
    skip_spaces();
    s = keyword();
    if (!streql(s, "address")) {
        error("Expect \"address\" at '^' position.\n", p - strlen(s));
        return false;
    }
    skip_spaces();
    return true;
}

/* Pre-conditions: last token is "rm, and peekc() is not a whitespace */
static void rm(void)
{
    if (!ip_addr_str()) {
        return;
    }
    uint32_t ipv4_addr;
    if (!ipv4_address(&ipv4_addr)) {
        error("Expect a valid IPv4 address at '^' position.\n", last_pos());
        return;
    }
    g_br_conf.rm_ip_addr = ipv4_addr;
}

int16_t get_portid(uint32_t ipv4_addr)
{
    int16_t portid;
    int ret = port_match_ip(&g_br_tables.port_table, ipv4_addr, &portid);
    return (ret == FAILED) ? -1 : portid;
}

static void port(void)
{
    int portid;
    uint32_t ipv4_addr;
    int prefix;
    uint32_t mask;

    if (!isdigit(peekc())) {
        error("Expect port id at '^' position.\n", cur_pos());
        return;
    }
    portid = number();
    skip_spaces();
    if (!ip_addr_str()) {
        return;
    }
    if (!cidr_block(&ipv4_addr, &prefix)) {
        return;
    }
    mask = IPPREFIX2MASK(prefix);
    uint32_t ipv4_addr_old;
    uint8_t prefix_len_old;
    int ret;
    ret = port_get_ip_prefix(&g_br_tables.port_table, portid, &ipv4_addr_old, &prefix_len_old);
    if (ret == OK) { /* delete old entry in IP LPM table */
        ip_lpm_remove(g_br_tables.ip_lpm_table, ipv4_addr_old, prefix_len_old, 0);
    }

    ret = port_add(&g_br_tables.port_table, portid, ipv4_addr, prefix);
    if (ret == FAILED) {
        return;
    }

    ip_lpm_add(g_br_tables.ip_lpm_table, ipv4_addr, prefix, ipv4_addr, portid, true);
    ip_lpm_update_portid(g_br_tables.ip_lpm_table, get_portid);
}

static void parse_ip_route(bool is_undo)
{
    uint32_t ipv4_addr;
    int prefix;
    uint32_t next_hop;
    int ret;
    if (!cidr_block(&ipv4_addr, &prefix)) {
        return;
    }
    skip_spaces();
    if (!ipv4_address(&next_hop)) {
        error("Expect a valid IPv4 address at '^' position.\n", last_pos());
        return;
    }

    if (is_undo) {
        ret = ip_lpm_remove(g_br_tables.ip_lpm_table, ipv4_addr, prefix, next_hop);
        if (ret == FAILED) {
            info("Cannot delete this route.\r\n");
        }
    } else {
        int16_t portid;
        ret = port_match_ip(&g_br_tables.port_table, next_hop, &portid);
        if (ret == FAILED) {
            portid = -1;
        }
        bool is_autogen = port_has_ip(&g_br_tables.port_table, ipv4_addr);
        ret = ip_lpm_add(g_br_tables.ip_lpm_table, ipv4_addr, prefix, next_hop, portid, is_autogen);
        if (ret == FAILED) {
            info("Cannot add this route.\r\n");
        }
    }
}

static bool ipv6_address(uint8_t *ipv6_addr)
{
    save_pos();

    for (int i = 0; i < 16; i++) {
        if (peekc() == ':') {
            p++ ;
        } 
        if (is_hex_digit(peekc())) {
            uint8_t c = tolower(nextc());
            if (isdigit(c)) {
                c = c - '0';
            }
            else {
                c = c - 'a' + 10;
            }
            ipv6_addr[i] = c << 4;
            if (is_hex_digit(peekc())) {
                c = tolower(nextc());
                if (isdigit(c)) {
                    c = c - '0';
                }
                else {
                    c = c - 'a' + 10;
                }
                ipv6_addr[i] += c;

            } else {
                return false;
            }
        } else {
            return false;
        }
    }

    char buf[128];
    sprintf(buf, "received ipv6 addr: %02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x\r\n", IPv6TO16HEX(ipv6_addr));
    print_to_cli(buf);
    return true;
}

static bool hw_address(uint8_t *hw_addr)
{
    save_pos();

    for (int i = 0; i < 6; i++) {
        if (peekc() == '-') {
            p++ ;
        } 
        if (is_hex_digit(peekc())) {
            uint8_t c = tolower(nextc());
            if (isdigit(c)) {
                c = c - '0';
            }
            else {
                c = c - 'a' + 10;
            }
            hw_addr[i] = c << 4;
            if (is_hex_digit(peekc())) {
                c = tolower(nextc());
                if (isdigit(c)) {
                    c = c - '0';
                }
                else {
                    c = c - 'a' + 10;
                }
                hw_addr[i] += c;

            } else {
                return false;
            }
        } else {
            return false;
        }
    }

    char buf[128];
    sprintf(buf, "received mac addr: %02x-%02x-%02x-%02x-%02x-%02x\r\n", hw_addr[0], hw_addr[1], hw_addr[2],
    hw_addr[3], hw_addr[4], hw_addr[5]);
    print_to_cli(buf);
    return true;
}

static void parse_ipv6(bool is_undo)
{
    uint32_t ipv4_addr;
    uint8_t ipv6_addr[16];
    uint8_t hw_addr[6];
    int ret;
    skip_spaces();
    if (!ipv4_address(&ipv4_addr)) {
        error("Expect a valid IPv4 address at '^' position.\n", last_pos());
        return;
    }
    skip_spaces();
    if(!ipv6_address(&ipv6_addr)) {
        error("Expect a valid IPv6 address at '^' position.\n", last_pos());
        return;
    }
    skip_spaces();
    if(!hw_address(&hw_addr)) {
        error("Expect a valid MAC address at '^' position.\n", last_pos());
        return;
    }
    if (is_undo) {
        info("Cannot delete this mapping. please restart\r\n");
        ret = FAILED;
    } else {
        char buf1[] = "start to add ipv6 entry";
        print_to_cli(buf1);
        int index;
        index = g_br_tables.addr_num;
        char buf2[] = "get current table number";
        print_to_cli(buf2);
        g_br_tables.ipv4_addrs[index] = ipv4_addr;
        char buf3[] = "added ipv4 part";
        print_to_cli(buf3);
        // g_br_tables.ipv6_addrs[index] = ipv4_addr;
        IPv6_copy(g_br_tables.ipv6_addrs[index], ipv6_addr);
        char buf4[] = "added ipv6 part";
        print_to_cli(buf4);
        g_br_tables.hw_addrs[index].addr_bytes[0] = hw_addr[0];
        g_br_tables.hw_addrs[index].addr_bytes[1] = hw_addr[1];
        g_br_tables.hw_addrs[index].addr_bytes[2] = hw_addr[2];
        g_br_tables.hw_addrs[index].addr_bytes[3] = hw_addr[3];
        g_br_tables.hw_addrs[index].addr_bytes[4] = hw_addr[4];
        g_br_tables.hw_addrs[index].addr_bytes[5] = hw_addr[5];
        char buf5[] = "added mac part";
        print_to_cli(buf5);
        g_br_tables.addr_num ++;
        char buf6[] = "updated number";
        print_to_cli(buf6);
    }
}


static void parse_sk(void)
{
    uint32_t sk;
    if (!hex_number(sizeof(sk), &sk)) {
        error("Expect a hexadecimal number of 8 digits at '^' position.\n", last_pos());
        return;
    }
    g_br_conf.sk = sk;
}

static void parse_on_off(bool *b, bool report_failure)
{
    const char *s = keyword();
    if (streql(s, "on")) {
        *b = true;
    } else if (streql(s, "off")) {
        *b = false;
    } else {
        if (report_failure) {
            error("Expect \"on\" or \"off\" at '^' position.\n", last_pos());
        }
    }
}

static void parse_debugging(void)
{
    const char *s = keyword();
    char buf[MAX_LINE_LEN + 1];
    if (streql(s, "on")) {
        g_br_conf.debugging_on = true;
    } else if (streql(s, "off")) {
        g_br_conf.debugging_on = false;
    } else if (streql(s, "l2")) {
        skip_spaces();
        parse_on_off(&g_br_conf.debugging_l2_on, true);
    } else if (streql(s, "l3")) {
        skip_spaces();
        parse_on_off(&g_br_conf.debugging_l3_on, true);
    } else if (streql(s, "")) {
        sprintf(buf, "debugging is %s; l2: %s; l3: %s.\n",
            g_br_conf.debugging_on ? "on" : "off",
            g_br_conf.debugging_l2_on ? "on" : "off",
            g_br_conf.debugging_l3_on ? "on" : "off");
        info(buf);
    } else {
        error("Expect \"on\" or \"off\" or \"l2\" or \"l3\" at '^' position.\n", last_pos());
    }
}

/* outbound data control */
static void parse_odc(void)
{
    int pos = p;
    const char *s = keyword();
    skip_spaces();
    char buf[MAX_LINE_LEN + 1];
    if (streql(s, "enable")) {
        g_br_conf.odc_enabled = true;
    } else if (streql(s, "disable")) {
        g_br_conf.odc_enabled = false;
    } else if (streql(s, "")) {
        sprintf(buf, "Outbound data control is %s.\n",
            g_br_conf.odc_enabled ? "enabled" : "disabled");
        info(buf);
    } else {
        error("Expect \"enable\" or \"disable\" at '^' position.\n", pos);
    }
}

/* inter-domain HMAC verification */
static void parse_idhv(void)
{
    int pos = p;
    const char *s = keyword();
    skip_spaces();
    char buf[MAX_LINE_LEN + 1];
    if (streql(s, "enable")) {
        g_br_conf.interdomain_hmac_verify_enabled = true;
    } else if (streql(s, "disable")) {
        g_br_conf.interdomain_hmac_verify_enabled = false;
    } else if (streql(s, "")) {
        sprintf(buf, "Inter-domain HMAC verification is %s.\n",
            g_br_conf.interdomain_hmac_verify_enabled ? "enabled" : "disabled");
        info(buf);
    } else {
        error("Expect \"enable\" or \"disable\" at '^' position.\n", pos);
    }
}

static void parse_pid_verify(void)
{
    int pos = p;
    const char *s = keyword();
    skip_spaces();
    char buf[MAX_LINE_LEN + 1];
    if (streql(s, "enable")) {
        g_br_conf.pid_verify_enabled = true;
    } else if (streql(s, "disable")) {
        g_br_conf.pid_verify_enabled = false;
    } else if (streql(s, "")) {
        sprintf(buf, "PID verification is %s.\n",
            g_br_conf.pid_verify_enabled ? "enabled" : "disabled");
        info(buf);
    } else {
        error("Expect \"enable\" or \"disable\" at '^' position.\n", pos);
    }
}

static void parse_arp(void)
{
    int pos = p;
    const char *s = keyword();
    skip_spaces();
    char buf[MAX_LINE_LEN + 1];
    if (streql(s, "enable")) {
        g_br_conf.arp_enabled = true;
    } else if (streql(s, "disable")) {
        g_br_conf.arp_enabled = false;
    } else if (streql(s, "add")) {
        pos = p;
        char buf[64];
        int i = 0;
        struct rte_ether_addr eth_addr;
        uint32_t ipv4_addr;
        while (is_hex_digit(peekc())) {
            buf[i++] = tolower(nextc());
        }
        buf[i] = '\0';
        if (strlen(buf) != 12) {
            error("Expect a MAC address at '^' position.\n", pos);
            return;
        }
        str_to_hex(buf, (uint8_t *)&eth_addr);
        skip_spaces();
        pos = p;
        if (!ipv4_address(&ipv4_addr)) {
            error("Expect an IPv4 address at '^' position.\n", pos);
            return;
        }
        arp_add(&g_br_tables.arp_table, ipv4_addr, &eth_addr);
    }
    else if (streql(s, "")) {
        sprintf(buf, "ARP resolution is %s.\n",
            g_br_conf.arp_enabled ? "enabled" : "disabled");
        info(buf);
    } else {
        error("Expect \"enable\" or \"disable\" or \"add\" at '^' position.\n", pos);
    }
}

static void parse_read_only(void)
{
    int pos = p;
    const char *s = keyword();
    char buf[MAX_LINE_LEN + 1];
    if (streql(s, "on")) {
        read_only = true;
    } else if (streql(s, "off")) {
        read_only = false;
    } else if (streql(s, "")) {
        sprintf(buf, "Configuration file is %s.\n",
            read_only ? "read-only" : "not read-only");
        info(buf);
    } else {
        error("Expect \"on\" or \"off\" at '^' position.\n", pos);
    }
}

static void peer_path_callback(uint32_t px, uint32_t prefix_len, uint32_t ipv4_addr)
{
    px_remove(g_br_tables.px_table, px, prefix_len, ipv4_addr);
}

/**
 * @brief Parse peer command.
 *
 * peer add <type> <ip_addr> sk <sk>
 * peer remove <ip_addr>
 * peer add-path <ip_addr> as <as_id> px <px>
 * peer remove-path <ip_addr> as <as_id> px <px>
 *
 * <type> -> internal | external
 * <ip_addr> -> INT<0,255>.INT<0,255>.INT<0,255>.INT<0,255>
 * <sk> -> HEX<32 hex digits>
 * <px> -> HEX<8 hex digits>/INT<0,32>
 *
 * @param is_undo
 */
static void parse_peer(bool is_undo)
{
    if (is_undo) {
        info("'undo' is not supported for peer command.\r\n");
        return;
    }
    bool is_internal;
    uint32_t ipv4_addr, as_id, px_len, px;
    uint8_t sk[SIZE_BR_SK] = {0};
    int ret;
    const char *subcmd = keyword();
    skip_spaces();
    if (streql(subcmd, "add")) {
        const char *type = keyword();
        skip_spaces();
        if (streql(type, "internal")) {
            is_internal = true;
        } else if (streql(type, "external")) {
            is_internal = false;
        } else {
            error("Expect 'internal' or 'external' at '^' position.\n", last_pos());
            return;
        }

        // ip address
        if (!ipv4_address(&ipv4_addr)) {
            error("Expect a valid IPv4 address at '^' position.\n", last_pos());
            return;
        }
        skip_spaces();

        // 'sk'
        const char *s = keyword();
        skip_spaces();
        if (!streql(s, "sk")) {
            error("Expect 'sk' at '^' position.\n", last_pos());
            return;
        }

        // sk
        if (!hex_bytes(sk, SIZE_BR_SK)) {
            error("Expect 32-bytes hex at '^' position.\n", last_pos());
            return;
        }
        peer_add(&g_br_tables.peer_table, ipv4_addr, is_internal, sk);
    } else if (streql(subcmd, "remove")) {
        // ip address
        if (!ipv4_address(&ipv4_addr)) {
            error("Expect a valid IPv4 address at '^' position.\n", last_pos());
            return;
        }
        // remove px in px table first
        peer_for_each_path(&g_br_tables.peer_table, ipv4_addr, peer_path_callback);
        peer_remove(&g_br_tables.peer_table, ipv4_addr);
    } else if (streql(subcmd, "add-path") || streql(subcmd, "remove-path")) {
        bool add_path = streql(subcmd, "add-path");

        // ip address
        if (!ipv4_address(&ipv4_addr)) {
            error("Expect a valid IPv4 address at '^' position.\n", last_pos());
            return;
        }
        skip_spaces();

        // 'as'
        const char *s = keyword();
        skip_spaces();
        if (!streql(s, "as")) {
            error("Expect 'as' at '^' position.\n", last_pos());
            return;
        }

        // as id
        if (!isdigit(peekc())) {
            error("Expect a valid AS ID at '^' position.\n", cur_pos());
            return;
        }
        as_id = number();
        skip_spaces();

        // 'px'
        s = keyword();
        skip_spaces();
        if (!streql(s, "px")) {
            error("Expect 'px' at '^' position.\n", last_pos());
            return;
        }

        // px
        if (!hex_number(sizeof(px), &px)) {
            error("Expect 4-bytes hex at '^' position.\n", last_pos());
            return;
        }
        if (!match('/')) {
            error("Expect '/' at '^' position.\n", cur_pos());
            return;
        }
        if (!isdigit(peekc())) {
            error("Expect prefix length at '^' position.\n", cur_pos());
            return;
        }
        px_len = number();
        if (px_len > 32) {
            error("Expect valid range [0,32] of prefix length at '^' position.\n", last_pos());
            return;
        }

        if (add_path) {
            peer_add_path(&g_br_tables.peer_table, ipv4_addr, as_id, px_len, px);
            px_add(g_br_tables.px_table, px, px_len, as_id, ipv4_addr);
        } else {
            peer_remove_path(&g_br_tables.peer_table, ipv4_addr, as_id, px_len, px);
            px_remove(g_br_tables.px_table, px, px_len, ipv4_addr);
        }
    } else {
        error("Expect command 'add', 'remove', 'add-path' or 'remove-path' at"
            " '^' position.\n", last_pos());
    }
}

/* Post-conditions: table_str will be freed */
static void display_table(const char *table_name, char *table_str)
{
    info("#\r\n");
    info(BGRN); info(table_name); info(RESET_STYLE);
    info("\r\n--------------------------------------------------"
        "-------------\r\n");
    info(table_str);
    free(table_str);
}

static void display_configuration(void)
{
    char buf[MAX_LINE_LEN + 1];

    // BR's NID
    char br_nid_str[64];
    nid_to_string(&g_br_conf.br_nid, br_nid_str);
    sprintf(buf, "# BR NID\r\n%s\r\n", br_nid_str);
    info(buf);

    // debugging
    sprintf(buf, "#\r\ndebugging %s\r\n", g_br_conf.debugging_on ? "on" : "off");
    info(buf);

    // outbound data control
    sprintf(buf, "#\r\nodc %s\r\n", g_br_conf.odc_enabled ? "enable" : "disable");
    info(buf);

    // PID verification
    sprintf(buf, "#\r\npid-verify %s\r\n", g_br_conf.pid_verify_enabled ? "enable" : "disable");
    info(buf);

    // ARP
    sprintf(buf, "#\r\narp %s\r\n", g_br_conf.arp_enabled ? "enable" : "disable");
    info(buf);

    // RM IP address
    sprintf(buf, "#\r\nrm ip address %d.%d.%d.%d\r\n", IPv4TO4DECIMAL(g_br_conf.rm_ip_addr));
    info(buf);

    // SK
    sprintf(buf, "#\r\nsk %08x\r\n", g_br_conf.sk);
    info(buf);

    display_table("Port", port_config_string(&g_br_tables.port_table));
    display_table("Peer Table", peer_table_config_string(&g_br_tables.peer_table));
    display_table("PX Table", px_table_to_string(g_br_tables.px_table));
    display_table("NID Table", nid_table_to_string(&g_br_tables.nid_table));
    display_table("ODC Table", odc_table_to_string(&g_br_tables.odc_table));
    display_table("IP Routing Table", ip_lpm_to_string(g_br_tables.ip_lpm_table));
    display_table("ARP Table", arp_table_to_string(&g_br_tables.arp_table));
}

static void parse_display(void)
{
    int pos = p;
    const char *s = keyword();
    if (streql(s, "arp")) {
        display_table("ARP Table", arp_table_to_string(&g_br_tables.arp_table));
    } else if (streql(s, "config") || streql(s, "")) {
        display_configuration();
    } else if (streql(s, "ip-routing-table") || streql(s, "iprt")) {
        display_table("IP Routing Table", ip_lpm_to_string(g_br_tables.ip_lpm_table));
    } else if (streql(s, "nid-table") || streql(s, "nidt")) {
        display_table("NID Table", nid_table_to_string(&g_br_tables.nid_table));
    } else if (streql(s, "odct")) {
        display_table("ODC Table", odc_table_to_string(&g_br_tables.odc_table));
    } else if (streql(s, "peer")) {
        display_table("Peer Table", peer_table_config_string(&g_br_tables.peer_table));
    } else if (streql(s, "px")) {
        display_table("PX Table", px_table_to_string(g_br_tables.px_table));
    } else if (streql(s, "ports")) {
        display_table("Port", port_config_string(&g_br_tables.port_table));
    } else if (streql(s, "stats")) {
        char *s = stats_table_to_string(g_br_tables.stats_table, RTE_MAX_LCORE, false);
        info(s);
        free(s);
    } else if (streql(s, "ipv6_table")) {
        char buf[128];
        StringBuilder *sb = new_sb();
        for (int i = 0; i < g_br_tables.addr_num; i++)
        {
            u_int32_t ipv4_addr = g_br_tables.ipv4_addrs[i];
            uint8_t *ipv6_addr = g_br_tables.ipv6_addrs[i];
            struct rte_ether_addr *hw_addr = &(g_br_tables.hw_addrs[i]);
            sprintf(buf, "entry %02d/%02d: ", i, g_br_tables.addr_num);
            sb_append(sb, buf);
            sprintf(buf, "ipv4:%d.%d.%d.%d; ", IPv4TO4DECIMAL(ipv4_addr));
            sb_append(sb, buf);
            sprintf(buf, "ipv6: %02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x; ", IPv6TO16HEX(ipv6_addr));
            sb_append(sb, buf);
            sprintf(buf, "mac: %02x-%02x-%02x-%02x-%02x-%02x \r\n", hw_addr->addr_bytes[0],
            hw_addr->addr_bytes[1], hw_addr->addr_bytes[2], hw_addr->addr_bytes[3],
            hw_addr->addr_bytes[4], hw_addr->addr_bytes[5]);
            sb_append(sb, buf);
        }
        s = sb_get(sb);
        free_sb(sb);
        info(s);
        free(s);
    } else if (streql(s, "port_table")) {
        char buf[128];
        StringBuilder *sb = new_sb();
        for (int i = 0; i < RTE_MAX_ETHPORTS; i++)
        {
            struct rte_ether_addr *hw_addr = &(g_br_conf.ports_eth_addr[i]);
            sprintf(buf, "port: %d  mac: %02x-%02x-%02x-%02x-%02x-%02x \r\n", i, hw_addr->addr_bytes[0],
            hw_addr->addr_bytes[1], hw_addr->addr_bytes[2], hw_addr->addr_bytes[3],
            hw_addr->addr_bytes[4], hw_addr->addr_bytes[5]);
            sb_append(sb, buf);
        }
        s = sb_get(sb);
        free_sb(sb);
        info(s);
        free(s);
    } else {
        error("Expect \"arp\", \"config\", "
            "\"ip-routing-table\",\r\n \"nid-table\", \"odct\", \"peer\", "
            "\"px\" or \"stats\" at '^' position.\n", pos);
    }
}

static void save_config(void)
{
    if (read_only) {
        error("Configuration file is read-only.\r\n", -1);
        return;
    }

    int fd_save = open(g_br_conf.config_path, O_WRONLY|O_CREAT|O_TRUNC, 0644);
    if (fd_save < 0) {
        info("Cannot open file config-br to save current configuration.\r\n");
        return;
    }

    char buf[MAX_LINE_LEN + 1];
    StringBuilder *sb = new_sb();

    sprintf(buf, "#\r\nsysname %s\r\n", g_br_conf.sysname);
    sb_append(sb, buf);

    // BR's NID
    char br_nid_str[64];
    nid_to_string(&g_br_conf.br_nid, br_nid_str);
    sprintf(buf, "#\r\nbr-nid %s\r\n", br_nid_str);
    sb_append(sb, buf);

    // debugging
    sprintf(buf, "#\r\ndebugging %s\r\n", g_br_conf.debugging_on ? "on" : "off");
    sb_append(sb, buf);

    // outbound data control
    sprintf(buf, "#\r\nodc %s\r\n", g_br_conf.odc_enabled ? "enable" : "disable");
    sb_append(sb, buf);

    // arp resolution
    sprintf(buf, "#\r\narp %s\r\n", g_br_conf.arp_enabled ? "enable" : "disable");
    sb_append(sb, buf);


    sprintf(buf, "#\r\nrm ip address %d.%d.%d.%d\r\n", IPv4TO4DECIMAL(g_br_conf.rm_ip_addr));
    sb_append(sb, buf);

    // SK
    sprintf(buf, "#\r\nsk %08x\r\n", g_br_conf.sk);
    sb_append(sb, buf);

    char *configs[] = {
        port_config_string(&g_br_tables.port_table),
        peer_table_config_string(&g_br_tables.peer_table),
        ip_lpm_config_string(g_br_tables.ip_lpm_table)
    };

    for (int i = 0; i < sizeof(configs) / sizeof(configs[0]); i++) {
        sb_append(sb, "#\r\n"); sb_append(sb, configs[i]); free(configs[i]);
    }

    char *s = sb_get(sb);
    if (write(fd_save, s, strlen(s))) {};
    free(s);
    free_sb(sb);
}

static void delete_config(void)
{
    if (access("config-br", F_OK) < 0) {
        return; // file does not exist.
    }
    if (unlink("config-br") < 0) {
        info("Cannot reset current configuration.\r\n");
    }
}

static void parse_br_nid(void)
{
    static bool is_set = false; // modification count

    if (is_set) {
        error("You cannot modify BR's NID.\n", p);
        return;
    }

    struct nid new_nid;
    for (int i = 0; i < 16; i++) {
        if (!is_hex_digit(peekc())) {
            error("Expect HEX number at '^' position.\n", p);
            return;
        } else {
            uint32_t k;
            if (!hex_number(1, &k)) {
                error("Expect HEX number at '^' position.\n", p);
                return;
            }
            new_nid.nid_bytes[i] = (uint8_t)k;
        }
        if (i < 15 && nextc() != '-') {
            error("Expect \"-\" at '^' position.\n", p);
            return;
        }
    }
    nid_copy(&new_nid, &g_br_conf.br_nid);
    is_set = true;
}

void parse_line(const char *s)
{
    strcpy(line, s);
    p = 0;
    skip_spaces();
    int pos = p;
    const char *cmd = keyword();
    skip_spaces();
    bool is_undo = false;

    if (streql(cmd, "undo")) {
        is_undo = true;
        pos = p;
        cmd = keyword();
        skip_spaces();
        if (!streql(cmd, "px") && !streql(cmd, "as") && !streql(cmd, "peer")
                && !streql(cmd, "nid") && !streql(cmd, "ip-route")
                && !streql(cmd, "ipv6")) {
            error("Cannot undo command found at '^' position.\n", pos);
            return;
        }
    }
    if (streql(cmd, "rm")) {
        rm();
    } else if (streql(cmd, "port")) {
        port();
    } else if (streql(cmd, "ip-route")) {
        parse_ip_route(is_undo);
    } else if (streql(cmd, "sk")) {
        parse_sk();
    } else if (streql(cmd, "peer")) {
        parse_peer(is_undo);
    } else if (streql(cmd, "ipv6")) {
        parse_ipv6(is_undo);
    }else if (streql(cmd, "sysname")) {
        if (isalnum(peekc()) || isdigit(peekc())) {
            int len = 0;
            while (isalnum(peekc()) || isdigit(peekc())) {
                g_br_conf.sysname[len++] = nextc();
            }
            g_br_conf.sysname[len] = '\0';
            g_br_conf.sysname_with_brackets[0] = '[';
            strcpy(g_br_conf.sysname_with_brackets + 1, g_br_conf.sysname);
            strcat(g_br_conf.sysname_with_brackets, "]");
        }
    } else if (streql(cmd, "?") || streql(cmd, "help")) {
        const char *help =
            "  sysname TEXT<\"...\">(no more than 64 characters)\r\n"
            "      Set the host name.\r\n"
            "  rm ip address IP_ADDR<X.X.X.X>\r\n"
            "      Set the IP address of RM.\r\n"
            "  port INTEGER<0-32> ip address IP_ADDR<X.X.X.X>/INTEGER<0-32>\r\n"
            "      Set IP address of a port.\r\n"
            "  ip-route IP_ADDR<X.X.X.X>/INTEGER<0-32> IP_ADDR<X.X.X.X>\r\n"
            "      Add static routing entry.\r\n"
            "  sk HEX(8 hex digits)\r\n"
            "      Set SK.\r\n"
            "  peer add internal|external IP_ADDR<X.X.X.X> sk HEX<32 bytes>\r\n"
            "      Add a peer BR. Use 'external' for peer BR not in the same AS.\r\n"
            "  peer remove IP_ADDR<X.X.X.X>\r\n"
            "      Remove a peer BR. All path associated with this peer will be removed.\r\n"
            "  peer add-path|remove-path IP_ADDR<X.X.X.X> as INT px HEX<4 bytes>/INT<0,32>\r\n"
            "      Add/remove path.\r\n"
            "  save\r\n"
            "      Save current configuration to file 'config-br'. Shortcut: Ctrl + S.\r\n"
            "  reset-saved\r\n"
            "      Delete current configuration file 'config-br'. Shortcut: Ctrl + D.\r\n"
            "  debugging on|off\r\n"
            "      Turn on or turn off debugging.\r\n"
            "  odc enable|disable\r\n"
            "      Enable or disable outbound data control.\r\n"
            "  idhv enable|disable\r\n"
            "      Enable or disable interdomain HMAC verification.\r\n"
            "  pid-verify enable|disable\r\n"
            "      Enable or disable PID verification. Enabled by default.\r\n"
            "  arp enable|disable\r\n"
            "      Enable or disable ARP resolution. Use broadcast MAC address if ARP is disabled.\r\n"
            "  arp add HEX(12 hex digits)(MAC address) IP_ADDR<X.X.X.X>\r\n"
            "      Add ARP entry mannually.\r\n"
            "  display\r\n"
            "      Display current configuration. Shortcut: Ctrl + G.\r\n"
            "  display arp|config|ip-routing-table|nid-table|odct|peer|px|stats|ports\r\n"
            "      Display table.\r\n"
            "  stats\r\n"
            "      Display real-time statistics.\r\n"
            "  help\r\n"
            "      Display usage of available commands.\r\n"
            "  exit\r\n"
            "      Shutdown border router. Shortcut: Ctrl + C.\r\n";
        info(help);
    } else if (streql(cmd, "exit")) {
        kill(getpid(), SIGTERM);
        info("Bye...\r\n");
        g_br_conf.cmd_exit = true;
    } else if (streql(cmd, "debugging")) {
        parse_debugging();
    } else if (streql(cmd, "odc")) {
        parse_odc();
    } else if (streql(cmd, "idhv")) {
        parse_idhv();
    } else if (streql(cmd, "pid-verify")) {
        parse_pid_verify();
    } else if (streql(cmd, "display")) {
        parse_display();
    } else if (streql(cmd, "save")) {
        save_config();
    } else if (streql(cmd, "reset-saved")) {
        delete_config();
    } else if (streql(cmd, "br-nid")) {
        parse_br_nid();
    } else if (streql(cmd, "arp")) {
        parse_arp();
    } else if (streql(cmd, "read-only")) {
        parse_read_only();
    }
    else if (!streql(cmd, "")) {
        error("Unrecognized command found at '^' position.\n", pos);
    }

}