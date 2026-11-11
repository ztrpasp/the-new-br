#ifndef _BR_LOG_H_
#define _BR_LOG_H_
#include <string.h>
#include <time.h>
#include "commandline/cli.h"
#include "stdio.h"
#include "br_conf.h"

extern struct br_conf g_br_conf;

#define BR_LOG(...)\
{\
    if (unlikely(g_br_conf.debugging_on)) {\
        if (likely(g_br_conf.cli_enabled)) {\
            char buf[512];\
            time_t t = time(NULL);\
            struct tm tm;\
            localtime_r(&t, &tm);\
            snprintf(buf, sizeof(buf), "%s: %d-%02d-%02d %02d:%02d:%02d ", \
                g_br_conf.sysname, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,\
                tm.tm_hour, tm.tm_min, tm.tm_sec);\
            print_to_cli(buf);\
            snprintf(buf, sizeof(buf), __VA_ARGS__);\
            if (buf[strlen(buf) - 1] == '\n')\
                strcat(buf, "\r");\
            print_to_cli(buf);\
        } else {\
            RTE_LOG(DEBUG, BR, __VA_ARGS__);\
        }\
    }\
}

#define BR_LOGF(...)\
{\
    if (unlikely(g_br_conf.debugging_on)) {\
        if (likely(g_br_conf.cli_enabled)) {\
            char buf[512];\
            time_t t = time(NULL);\
            struct tm tm;\
            localtime_r(&t, &tm);\
            snprintf(buf, sizeof(buf), "%s: %d-%02d-%02d %02d:%02d:%02d %s: ", \
                g_br_conf.sysname, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,\
                tm.tm_hour, tm.tm_min, tm.tm_sec, __func__);\
            print_to_cli(buf);\
            snprintf(buf, sizeof(buf), __VA_ARGS__);\
            if (buf[strlen(buf) - 1] == '\n')\
                strcat(buf, "\r");\
            print_to_cli(buf);\
        } else {\
            RTE_LOG(DEBUG, BR, __VA_ARGS__);\
        }\
    }\
}

// Layer 2
#define BR_LOG_L2(...)\
{\
    if (unlikely(g_br_conf.debugging_l2_on)) {\
        if (likely(g_br_conf.cli_enabled)) {\
            char buf[512];\
            time_t t = time(NULL);\
            struct tm tm;\
            localtime_r(&t, &tm);\
            snprintf(buf, sizeof(buf), "%s L2: %d-%02d-%02d %02d:%02d:%02d ", \
                g_br_conf.sysname, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,\
                tm.tm_hour, tm.tm_min, tm.tm_sec);\
            print_to_cli(buf);\
            snprintf(buf, sizeof(buf), __VA_ARGS__);\
            if (buf[strlen(buf) - 1] == '\n')\
                strcat(buf, "\r");\
            print_to_cli(buf);\
        } else {\
            RTE_LOG(DEBUG, BR, __VA_ARGS__);\
        }\
    }\
}

// Layer 3
#define BR_LOG_L3(...)\
{\
    if (unlikely(g_br_conf.debugging_l3_on)) {\
        if (likely(g_br_conf.cli_enabled)) {\
            char buf[512];\
            time_t t = time(NULL);\
            struct tm tm;\
            localtime_r(&t, &tm);\
            snprintf(buf, sizeof(buf), "%s L3: %d-%02d-%02d %02d:%02d:%02d ", \
                g_br_conf.sysname, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,\
                tm.tm_hour, tm.tm_min, tm.tm_sec);\
            print_to_cli(buf);\
            snprintf(buf, sizeof(buf), __VA_ARGS__);\
            if (buf[strlen(buf) - 1] == '\n')\
                strcat(buf, "\r");\
            print_to_cli(buf);\
        } else {\
            RTE_LOG(DEBUG, BR, __VA_ARGS__);\
        }\
    }\
}

#define BR_INFO(...)\
{\
    if (likely(g_br_conf.cli_enabled)) {\
        char buf[512];\
        snprintf(buf, sizeof(buf), __VA_ARGS__);\
        if (buf[strlen(buf) - 1] == '\n')\
            strcat(buf, "\r");\
        print_to_cli(buf);\
    } else {\
        RTE_LOG(DEBUG, BR, __VA_ARGS__);\
    }\
}

#define BR_INFOF(...)\
{\
    if (likely(g_br_conf.cli_enabled)) {\
        char buf[512];\
        time_t t = time(NULL);\
        struct tm tm;\
        localtime_r(&t, &tm);\
        snprintf(buf, sizeof(buf), "%s: %d-%02d-%02d %02d:%02d:%02d %s: ", \
            g_br_conf.sysname, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,\
            tm.tm_hour, tm.tm_min, tm.tm_sec, __func__);\
        print_to_cli(buf);\
        snprintf(buf, sizeof(buf), __VA_ARGS__);\
        if (buf[strlen(buf) - 1] == '\n')\
            strcat(buf, "\r");\
        print_to_cli(buf);\
    } else {\
        RTE_LOG(DEBUG, BR, __VA_ARGS__);\
    }\
}

#endif // _BR_LOG_H_