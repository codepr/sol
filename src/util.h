/* BSD 2-Clause License
 *
 * Copyright (c) 2019, Andrea Giacomo Baldan All rights reserved.
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

#ifndef UTIL_H
#define UTIL_H

#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define MAX_LOG_SIZE 119

#define SOL_PREFIX   "sol"

enum log_level { DEBUG, INFORMATION, WARNING, ERROR };

bool is_integer(const char *);
int parse_int(const char *);
int number_len(size_t);
void generate_random_id(char *);

/* Logging */
void sol_log_init(const char *);
void sol_log_close(void);
void sol_log(int, const char *, ...);

/* Memory management */
void *xmalloc(size_t);
void *xcalloc(size_t, size_t);
void *xrealloc(void *, size_t);
size_t xmalloc_size(void *);
void xfree(void *);
char *xstrdup(const char *);
char *remove_occur(char *, char);
char *append_string(const char *, char *, size_t);
bool check_passwd(const char *, const char *);

size_t memory_used(void);

long get_fh_soft_limit(void);

#define log(...) sol_log( __VA_ARGS__ )
#define log_debug(...) log(DEBUG, __VA_ARGS__)
#define log_warning(...) log(WARNING, __VA_ARGS__)
#define log_error(...) log(ERROR, __VA_ARGS__)
#define log_info(...) log(INFORMATION, __VA_ARGS__)

#define STREQ(s1, s2, len) strncasecmp(s1, s2, len) == 0 ? true : false

#define container_of(ptr, type, field) \
    ((type *)((char *)(ptr) - offsetof(type, field)))

#endif
