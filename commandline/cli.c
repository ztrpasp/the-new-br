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

#include "cli.h"
#include "cmd_parser.h"
#include "../utils/utils.h"
#include "../ansi_escape_seq.h"
#include "../profiler/profiler.h"
#include "../common.h"
#include "../br_conf.h"
#include "../br_tables.h"

enum cli_state {STATE_NORMAL, STATE_TUI} cli_state = STATE_NORMAL;

extern struct br_conf g_br_conf;
extern struct br_tables g_br_tables;

/** Less than 1023 characters. */
static void writefd(const char *s)
{
    int n_retries = 1e6;
    ssize_t n_written;
    do {
        n_written = write(1, s, strlen(s));
        if (n_written >= 0) {
            s += n_written;
        } else { // error
            if (n_retries-- <= 0) {
                return;
            }
        }
    } while (*s != '\0' && !g_br_conf.force_quit);
}

static ssize_t readfd(void *buf, size_t n)
{
    return read(0, buf, n);
}

// interface
void print_to_cli(const char *msg)
{
    writefd(msg);
}

static struct rx_buffer rx_buffer;
static struct cmd_hist cmd_hist;

static bool rx_buffer_put(struct rx_buffer *rxb, char c)
{
    if (rxb->len + 1 > RX_BUFFER_SIZE) {
        return false;
    }
    for (int i = rxb->len - 1; i >= rxb->cursor; i--) {
        rxb->data[i + 1] = rxb->data[i];
    }
    rxb->data[rxb->cursor] = c;
    rxb->cursor++;
    rxb->len++;
    return true;
}

/* set rx_buffer to string s, no '\0' copied */
static void rx_buffer_set(struct rx_buffer *rxb, const char *s)
{
    size_t i;
    for (i = 0; i < strlen(s) && i < RX_BUFFER_SIZE; i++) {
        rxb->data[i] = s[i];
    }
    rxb->cursor = i;
    rxb->len = i;
}

static void rx_buffer_move_cursor(struct rx_buffer *rxb, int offset)
{
    rxb->cursor = max(0, min(rxb->cursor + offset, rxb->len));
}

static void rx_buffer_move_cursor_one_word_left(struct rx_buffer *rxb)
{
    int i = rxb->cursor - 1;
    // skip space
    while (i >= 0 && i < rxb->len && isspace(rxb->data[i])) {
        i--;
    }
    while (i >= 0 && i < rxb->len && !isspace(rxb->data[i])) {
        i--;
    }
    rxb->cursor = i + 1;
}

static void rx_buffer_move_cursor_one_word_right(struct rx_buffer *rxb)
{
    int i = rxb->cursor;
    // skip space
    while (i >= 0 && i < rxb->len && isspace(rxb->data[i])) {
        i++;
    }
    while (i >= 0 && i < rxb->len && !isspace(rxb->data[i])) {
        i++;
    }
    rxb->cursor = i;
}

static void rx_buffer_move_cursor_leftmost(struct rx_buffer *rxb)
{
    rxb->cursor = 0;
}

static void rx_buffer_move_cursor_rightmost(struct rx_buffer *rxb)
{
    rxb->cursor = rxb->len;
}

/** delete the byte at cursor */
static void rx_buffer_delete(struct rx_buffer *rxb)
{
    if (rxb->cursor == rxb->len) {
        return;
    }
    for (int i = rxb->cursor; i < rxb->len - 1; i++) {
        rxb->data[i] = rxb->data[i + 1];
    }
    rxb->len--;
}

/** delete the byte at cursor - 1 */
static void rx_buffer_backspace(struct rx_buffer *rxb)
{
    if (rxb->cursor == 0) {
        return;
    }
    for (int i = rxb->cursor; i < rxb->len; i++) {
        rxb->data[i - 1] = rxb->data[i];
    }
    rxb->cursor--;
    rxb->len--;
}

static void rx_buffer_reset(struct rx_buffer *rxb)
{
    rxb->len = 0;
    rxb->cursor = 0;
}

/** dst should have enough space. */
static void rx_buffer_copystr(struct rx_buffer *rxb, char *dst)
{
    // '\0' is copied to dst
    for (int i = 0; i < rxb->len; i++) {
        dst[i] = rxb->data[i];
    }
    dst[rxb->len] = '\0';
}

static void cmd_hist_append(struct cmd_hist *ch, const char *cmd)
{
    struct cmd_hist_entry *p =
        (struct cmd_hist_entry *)malloc(sizeof(struct cmd_hist_entry));
    strcpy(p->cmd, cmd);
    p->next = NULL;
    p->prev = ch->tail;
    if (ch->tail != NULL) {
        ch->tail->next = p;
    }
    ch->tail = p;
    ch->cursor = NULL;
}

static struct cmd_hist_entry *cmd_hist_find(struct cmd_hist *ch, const char *cmd)
{
    struct cmd_hist_entry *p = ch->tail;
    while (p != NULL) {
        if (streql(p->cmd, cmd)) {
            return p;
        }
        p = p->prev;
    }
    return NULL;
}

static bool cmd_hist_remove(struct cmd_hist *ch, const char *cmd)
{
    struct cmd_hist_entry *p = cmd_hist_find(ch, cmd);
    if (p == NULL) {
        return false;
    }
    if (p->next != NULL) {
        p->next->prev = p->prev;
    }
    if (p->prev != NULL) {
        p->prev->next = p->next;
    }
    if (ch->tail == p) {
        ch->tail = p->prev;
    }
    free(p);
    return true;
}

/* cursor == NULL: beyond tail */
static void cmd_hist_cursor_backward(struct cmd_hist *ch)
{
    if (ch->cursor == NULL) {
        ch->cursor = ch->tail;
        return;
    }
    if (ch->cursor->prev != NULL) {
        ch->cursor = ch->cursor->prev;
    }
}

static void cmd_hist_cursor_forward(struct cmd_hist *ch)
{
    if (ch->cursor == NULL) {
        return;
    }
    ch->cursor = ch->cursor->next;
}

static void cmd_hist_cursor_reset(struct cmd_hist *ch)
{
    ch->cursor = NULL;
}

static const char *cmd_hist_get(struct cmd_hist *ch)
{
    if (ch->cursor == NULL) {
        return "";
    } else {
        return ch->cursor->cmd;
    }
}

static void refresh_fd_line(struct rx_buffer *rxb, struct br_conf const *br_conf)
{
    /* 1. carriage return,
     * 2. erase line (from the cursor position to the end of the line),
     * 3. print line,
     * 4. move the cursor */
    char line[RX_BUFFER_SIZE + 1];
    rx_buffer_copystr(rxb, line);
    writefd("\r" CLEAR_CURRENT_LINE);
    writefd(br_conf->sysname_with_brackets);
    writefd(line);
    // assert rxb->cursor + strlen(sysname) > 0
    char buf[16];
    snprintf(buf, 16, "\r\x1b[%uC",
        (unsigned int) (rxb->cursor + strlen(br_conf->sysname_with_brackets)));
    writefd(buf);
}

static void display_br_stats(struct br_conf const *br_conf)
{
    writefd(CLEAR_FROM_CURSOR_TO_BEGINNING MOVE_CURSOR_HOME);
    writefd(br_conf->sysname_with_brackets); writefd("stats\r\n");
    char *s = stats_table_to_string(g_br_tables.stats_table, RTE_MAX_LCORE, true);
    writefd(s); free(s);
#ifdef PROFILER_ENABLE
    char buf[512];
    for (int i = 0; i < 32; i++) {
        if (rte_lcore_is_enabled(i)) {
            #pragma GCC diagnostic ignored "-Wnested-externs"
            PROFILER_DATA(process_pkt, i);
            PROFILER_DATA(process_data_pkt, i);
            PROFILER_DATA(pid_verify, i);
            PROFILER_DATA(data_cksum, i);
            PROFILER_DATA(data_px_search, i);
            PROFILER_DATA(data_nid_ip_search, i);
            PROFILER_DATA(cal_back_pid, i);
            PROFILER_DATA(odct, i);
            PROFILER_DATA(process_get_pkt, i);
            PROFILER_DATA(get_cksum, i);
            PROFILER_DATA(get_px_search, i);
            PROFILER_DATA(send_ip, i);
            PROFILER_DATA(pkt_len_check, i);
            #define TIME_FORMAT "%.2f"
            sprintf(buf, "Lcore %d\r\ntotal: "TIME_FORMAT", sdip: "TIME_FORMAT
                ", plenck: "TIME_FORMAT
                ", pdata: "TIME_FORMAT", pidv: "TIME_FORMAT", dcksum: "
                TIME_FORMAT", dpx: "TIME_FORMAT", dnid: "TIME_FORMAT
                ", backpid: "TIME_FORMAT", odct: "TIME_FORMAT", pget: "
                TIME_FORMAT", gcksum: "TIME_FORMAT", gpx: "TIME_FORMAT"\r\n",
                i, t_process_pkt, t_send_ip, t_pkt_len_check, t_process_data_pkt,
                t_pid_verify, t_data_cksum, t_data_px_search, t_data_nid_ip_search,
                t_cal_back_pid, t_odct, t_process_get_pkt, t_get_cksum, t_get_px_search);
            writefd(buf);
        }
    }
#endif
    writefd("Press 'q' to quit.\r\n");
}

static void execute(const char *cmd, struct br_conf const *br_conf)
{
    cmd_hist_remove(&cmd_hist, cmd);
    cmd_hist_append(&cmd_hist, cmd);
    cmd_hist_cursor_reset(&cmd_hist);
    if (streql(cmd, "stats")) {
        display_br_stats(br_conf);
        cli_state = STATE_TUI;
    } else {
        parse_line(cmd);
    }
}

int cli_main_loop(__attribute__((unused)) void *dummy)
{
    usleep(100 * 1000);
    int const lcore_id = rte_lcore_id();
    uint64_t cur_tsc, prev_tsc_tui = 0, prev_tsc_timer = 0;
    struct br_conf *br_conf = &g_br_conf;
    struct br_tables *br_tables = &g_br_tables;
    const uint64_t drain_tsc = (rte_get_tsc_hz() + US_PER_S - 1) /
        US_PER_S * BURST_TX_DRAIN_US;

    RTE_LOG(DEBUG, BR, "entering cli loop on lcore %u\n", lcore_id);

    int ret;

    struct termios opt;
    struct termios saved_opt;
    struct termios saved_stdin_opt;

    tcgetattr(0, &opt);
    saved_stdin_opt = opt;
    cfmakeraw(&opt);
    ret = tcsetattr(0, TCSANOW, &opt);
    if (ret != 0) {
        RTE_LOG(WARNING, BR, "Failed to set attribute %d, %s\n", ret,
            strerror(errno));
        return -1;
    }
    fcntl(0, F_SETFL, fcntl(0, F_GETFL) | O_NONBLOCK);
    setbuf(stdout, NULL);

    br_tables_rcu_register(br_tables, lcore_id);

    br_conf->cmd_silent = false; // enable command parser output
    ssize_t nread = 1;
    char buff[64];
    char line[MAX_LINE_LEN];

    rx_buffer_reset(&rx_buffer);
    refresh_fd_line(&rx_buffer, br_conf);
    while(!br_conf->force_quit) {
        cur_tsc = rte_rdtsc();
        if (cli_state == STATE_TUI) {
            if (cur_tsc - prev_tsc_tui >= rte_get_tsc_hz()) {
                display_br_stats(br_conf);
                prev_tsc_tui = cur_tsc;
            }
        }

        /* Call the timer handler on each call. Call rte_timer_manage() every
         * ~10ms (at 2Ghz). */
        if (unlikely(cur_tsc - prev_tsc_timer > TIMER_RESOLUTION_CYCLES)) {
            br_tables_rcu_online(br_tables, lcore_id);
            rte_timer_manage();
            br_tables_rcu_offline(br_tables, lcore_id);
            prev_tsc_timer = cur_tsc;
        }

        nread = readfd(buff, 64);
        if (nread <= 0) {
            continue;
        }
        br_tables_rcu_online(br_tables, lcore_id);
        for (int i = 0; i < nread; i++) {
            if (cli_state == STATE_TUI) {
                if (buff[i] == 'q') {
                    cli_state = STATE_NORMAL;
                    refresh_fd_line(&rx_buffer, br_conf);
                }
                continue;
            }
            if (32 <= buff[i] && buff[i] <= 126) {
                // printable character
                bool put = rx_buffer_put(&rx_buffer, buff[i]);
                refresh_fd_line(&rx_buffer, br_conf);
                if (!put) { // buffer is full
                    // blink on; reverse video on; red; sleep 0.1s, reset
                    writefd("\x1b[5;7;31m");
                    refresh_fd_line(&rx_buffer, br_conf);
                    usleep(100 * 1000);
                    writefd("\x1b[0m");
                    refresh_fd_line(&rx_buffer, br_conf);
                }
                if (buff[i] == '?') {
                    goto rx_buffer_enter;
                }
            } else if (buff[i] == KEY_ENTER) { // 13, enter key
            rx_buffer_enter:
                rx_buffer_copystr(&rx_buffer, line);
                rx_buffer_reset(&rx_buffer);
                writefd("\r\n");
                if (strlen(line) != 0) {
                    execute(line, br_conf);
                }
                // if force_quit, do not output sysname
                if (!br_conf->cmd_exit && cli_state != STATE_TUI) {
                    refresh_fd_line(&rx_buffer, br_conf);
                }
            } else if (buff[i] == KEY_BACKSPACE_1 || buff[i] == KEY_BACKSPACE_2) {
                rx_buffer_backspace(&rx_buffer);
                refresh_fd_line(&rx_buffer, br_conf);
            } else if (buff[i] == ESC) {
                if (i + 3 < nread && startswith(&buff[i], KEY_DELETE)) {
                    rx_buffer_delete(&rx_buffer);
                    refresh_fd_line(&rx_buffer, br_conf);
                    i += 3;
                } else if (i + 2 < nread && startswith(&buff[i], KEY_UP)) {
                    cmd_hist_cursor_backward(&cmd_hist);
                    const char *cmd = cmd_hist_get(&cmd_hist);
                    rx_buffer_set(&rx_buffer, cmd);
                    refresh_fd_line(&rx_buffer, br_conf);
                    i += 2;
                } else if (i + 2 < nread && startswith(&buff[i], KEY_DOWN)) {
                    cmd_hist_cursor_forward(&cmd_hist);
                    const char *cmd = cmd_hist_get(&cmd_hist);
                    rx_buffer_set(&rx_buffer, cmd);
                    refresh_fd_line(&rx_buffer, br_conf);
                    i += 2;
                } else if (i + 2 < nread && startswith(&buff[i], KEY_RIGHT)) {
                    rx_buffer_move_cursor(&rx_buffer, 1);
                    refresh_fd_line(&rx_buffer, br_conf);
                    i += 2;
                } else if (i + 2 < nread && startswith(&buff[i], KEY_LEFT)) {
                    rx_buffer_move_cursor(&rx_buffer, -1);
                    refresh_fd_line(&rx_buffer, br_conf);
                    i += 2;
                } else if (i + 5 < nread && (startswith(&buff[i], KEY_CTRL_UP)
                        || startswith(&buff[i], KEY_CTRL_DOWN))) {
                    i += 5; // do nothing
                } else if (i + 5 < nread && startswith(&buff[i], KEY_CTRL_RIGHT)) {
                    rx_buffer_move_cursor_one_word_right(&rx_buffer);
                    refresh_fd_line(&rx_buffer, br_conf);
                    i += 5;
                } else if (i + 5 < nread && startswith(&buff[i], KEY_CTRL_LEFT)) {
                    rx_buffer_move_cursor_one_word_left(&rx_buffer);
                    refresh_fd_line(&rx_buffer, br_conf);
                    i += 5;
                } else if (i + 5 < nread && (
                    startswith(&buff[i], KEY_ALT_UP) ||
                    startswith(&buff[i], KEY_ALT_DOWN) ||
                    startswith(&buff[i], KEY_SHIFT_UP) ||
                    startswith(&buff[i], KEY_SHIFT_DOWN) ||
                    startswith(&buff[i], KEY_SHIFT_RIGHT) ||
                    startswith(&buff[i], KEY_SHIFT_LEFT))) {
                    i += 5;
                } else if (i + 5 < nread && startswith(&buff[i], KEY_ALT_RIGHT)) {
                    rx_buffer_move_cursor_rightmost(&rx_buffer);
                    refresh_fd_line(&rx_buffer, br_conf);
                    i += 5;
                } else if (i + 5 < nread && startswith(&buff[i], KEY_ALT_LEFT)) {
                    rx_buffer_move_cursor_leftmost(&rx_buffer);
                    refresh_fd_line(&rx_buffer, br_conf);
                    i += 5;
                }
            } else if (buff[i] == KEY_CTRL_C) {
                kill(getpid(), SIGTERM);
                rx_buffer_reset(&rx_buffer);
                refresh_fd_line(&rx_buffer, br_conf);
                writefd("exit\r\nBye...\r\n");
            } else if (buff[i] == 7) {
                rx_buffer_reset(&rx_buffer);
                refresh_fd_line(&rx_buffer, br_conf);
                writefd("display\r\n");
                execute("display", br_conf);
                refresh_fd_line(&rx_buffer, br_conf);
            } else if (buff[i] == 19) {
                rx_buffer_reset(&rx_buffer);
                refresh_fd_line(&rx_buffer, br_conf);
                writefd("save\r\n");
                execute("save", br_conf);
                refresh_fd_line(&rx_buffer, br_conf);
            } else if (buff[i] == KEY_CTRL_D) {
                rx_buffer_reset(&rx_buffer);
                refresh_fd_line(&rx_buffer, br_conf);
                writefd("reset-saved\r\n");
                execute("reset-saved", br_conf);
                refresh_fd_line(&rx_buffer, br_conf);
            }
        }
        br_tables_rcu_offline(br_tables, lcore_id);

        // printf("read:");
        // for (int i = 0; i < nread; i++) {
        //     int x = buff[i];
        //     printf("%d ", x);
        // }
        // printf("\n");
    }

    setlinebuf(stdout); // enable buffering for each line
    tcsetattr(0, TCSANOW, &saved_stdin_opt);
    br_tables_rcu_unregister(br_tables, lcore_id);

    return 0;
}