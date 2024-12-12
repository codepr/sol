/* BSD 2-Clause License
 *
 * Copyright (c) 2023, Andrea Giacomo Baldan All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "util.h"
#include "memory.h"
#include "mqtt.h"
#include <assert.h>
#include <string.h>
// #include <crypt.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define SOL_PREFIX "sol"

/* Auxiliary function to check wether a string is an integer */
bool is_integer(const char *string)
{
    for (; *string; ++string)
        if (!isdigit(*string))
            return false;
    return true;
}

/* Parse the integer part of a string, by effectively iterate through it and
   converting the numbers found */
int parse_int(const char *string)
{
    int n = 0;

    while (*string && isdigit(*string)) {
        n = (n * 10) + (*string - '0');
        string++;
    }
    return n;
}

char *remove_occur(char *str, char c)
{
    char *p  = str;
    char *pp = str;

    while (*p) {
        *pp = *p++;
        pp += (*pp != c);
    }

    *pp = '\0';

    return str;
}

char *update_integer_string(char *str, int num)
{

    int n = parse_int(str);
    n += num;
    /*
     * Check for realloc if the new value is "larger" then
     * previous
     */
    char tmp[number_len(n) + 1]; // max size in bytes
    sprintf(tmp, "%d", n);       // XXX Unsafe
    size_t len = strlen(tmp);
    str        = try_realloc(str, len + 1);
    memcpy(str, tmp, len + 1);

    return str;
}

/*
 * Append a string to another for the destination part that will be appended
 * the function require the length, the resulting string will be heap alloced
 * and nul-terminated.
 */
char *append_string(const char *src, char *dst, size_t chunklen)
{
    size_t srclen = strlen(src);
    char *ret     = try_alloc(srclen + chunklen + 1);
    memcpy(ret, src, srclen);
    memcpy(ret + srclen, dst, chunklen);
    ret[srclen + chunklen] = '\0';
    return ret;
}

/*
 * Return the 'length' of a positive number, as the number of chars it would
 * take in a string
 */
int number_len(size_t number)
{
    int len = 1;
    while (number) {
        len++;
        number /= 10;
    }
    return len;
}

unsigned long unix_time_ns(void)
{
    struct timeval time;
    gettimeofday(&time, NULL);
    return time.tv_sec * (int)1e6 + time.tv_usec;
}

void generate_random_id(char *dest)
{
    unsigned long utime_ns = unix_time_ns();
    snprintf(dest, MQTT_CLIENT_ID_LEN - 1, "%s-%lu", SOL_PREFIX, utime_ns);
}

bool check_passwd(const char *passwd, const char *salt)
{
    return STREQ(crypt(passwd, salt), salt, strlen(salt));
}

long get_fh_soft_limit(void)
{
    struct rlimit limit;
    if (getrlimit(RLIMIT_NOFILE, &limit)) {
        perror("Failed to get limit");
        return -1;
    }
    return limit.rlim_cur;
}
