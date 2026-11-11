#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <float.h>
#include "utils.h"

StringBuilder *new_sb(void)
{
    StringBuilder *sb = malloc(sizeof(StringBuilder));
    sb->capacity = 16;
    sb->data = malloc(sizeof (char) * sb->capacity);
    sb->len = 0;
    return sb;
}

/* Pre-conditions: sb != NULL, s != NULL.
 * Post-conditions: The string s is append to sb.*/
void sb_append(StringBuilder *sb, const char *s)
{
    int n = strlen(s);
    while (sb->len + n > sb->capacity) {
        sb->capacity *= 2;
    }
    sb->data = realloc(sb->data, sb->capacity);
    memcpy(sb->data + sb->len, s, n);
    sb->len += n;
}

// Returns a copy of the string.
char *sb_get(StringBuilder *sb)
{
    char *s = (char *)malloc(sb->len + 1);
    memcpy(s, sb->data, sb->len);
    s[sb->len] = '\0';
    return s;
}

void free_sb(StringBuilder *sb)
{
    free(sb->data);
    free(sb);
}

/** Convert string to hex array
 *  Make sure s is a valid hex string.
 *
 * Pre-conditions:
 *   s is not NULL and len(s) is even.
 *   Each character in *s* is in {0-9,a-f}.
 *   The array *p* has enough space for the result. (size(p) >= len(s) / 2)
 */

/**
 * @brief Convert string to hex array.
 *
 * @pre Each element of \p s is a valid hex digit.
 * @param s the hex string
 * @param p converted result
 */
void str_to_hex(const char *s, uint8_t *p)
{
    int i = 0; // index of p
    int value;
    char buf[4] = {0};

    int len = strlen(s);
    if (len & 1) { // len(s) is odd
        buf[0] = *s++;
        sscanf(buf, "%x", &value);
        p[i++] = (uint8_t)value;

    }

    while (*s != '\0') {
        buf[0] = *s++;
        buf[1] = *s++;
        sscanf(buf, "%x", &value);
        p[i++] = (uint8_t)value;
    }
}

char *hex_to_str(char *s, uint8_t *p, uint32_t len, char delim)
{
    for (int i = 0; i < len; i++) {
        sprintf(s, "%02X", p[i]);
        s += 2;
        if (i < len - 1 && delim != 0) {
            *s++ = delim;
        }
    }
    *s = '\0';
    return s;
}

/* s1 startswith s2 */
bool startswith(const char *s1, const char *s2)
{
    while (*s2 != '\0') {
        if (*s1++ != *s2++) {
            return false;
        }
    }
    return true;
}

/* s1 == s2 for n characters */
bool nstreql(const char *s1, const char *s2, int n)
{
    const char *p = s2;
    while (s2 - p < n && *s1++ == *s2++) ;
    return s2 - p == n;
}

#define RAND_UNIFORM(low, high) ((double)rand() / RAND_MAX * (high - low) + low)

/* Return a sample from the exponential distribution.
    The rate parameter is specified by lambda.
    Its cumulative distribution function is
    F(x)=1-exp(-lambda*x) for x > 0 and 0 elsewhere.*/
double rand_exp(double lambda)
{
    double u;
    do {
        u = RAND_UNIFORM(0, 1);
    } while (u < DBL_EPSILON);

    return -log(u) / lambda;
}