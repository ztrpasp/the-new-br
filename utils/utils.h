#ifndef __BR_UTILS_H__
#define __BR_UTILS_H__

#include <stdbool.h>
#include <stdint.h>
#include <ctype.h>
#include <string.h>

// StringBuilder
typedef struct {
    char *data;
    int capacity;
    int len;
} StringBuilder;

StringBuilder *new_sb(void);

/* Pre-conditions: sb != NULL, s != NULL.
 * Post-conditions: The string s is append to sb.*/
void sb_append(StringBuilder *sb, const char *s);

// Returns a copy of the string.
char *sb_get(StringBuilder *sb);

void free_sb(StringBuilder *sb);

static inline int max(int a, int b)
{
    return a > b ? a : b;
}

static inline int min(int a, int b)
{
    return a < b ? a : b;
}

static inline bool is_alpha(char c)
{
    return isalpha(c) || c == '-' || c == '?' || c == '_' || isdigit(c);
}

static inline bool streql(const char *s1, const char *s2)
{
    return strcmp(s1, s2) == 0 ? true : false;
}

void str_to_hex(const char *s, uint8_t *p);

char *hex_to_str(char *s, uint8_t *p, uint32_t len, char delim);

bool startswith(const char *s1, const char *s2);

bool nstreql(const char *s1, const char *s2, int n);

double rand_exp(double lambda);

#endif