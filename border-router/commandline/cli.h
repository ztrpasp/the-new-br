#ifndef __BR_CLI_H__
#define __BR_CLI_H__

#define RX_BUFFER_SIZE 128
struct rx_buffer {
    char data[RX_BUFFER_SIZE];
    int cursor;
    int len;
};

// command history
struct cmd_hist {
    struct cmd_hist_entry *tail;
    struct cmd_hist_entry *cursor;
};

struct cmd_hist_entry {
    char cmd[RX_BUFFER_SIZE + 1];
    struct cmd_hist_entry *prev;
    struct cmd_hist_entry *next;
};

void print_to_cli(const char *msg);

int cli_main_loop(__attribute__((unused)) void *dummy);

#endif