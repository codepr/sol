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
#include <stdint.h>
#include <stdbool.h>
#include <strings.h>


#define UUID_LEN     37

#define MAX_LOG_SIZE 119


enum log_level { DEBUG, INFORMATION, WARNING, ERROR };


bool is_integer(const char *);
int parse_int(const char *);
int number_len(size_t);
int generate_uuid(char *);

/* Logging */
void sol_log_init(const char *);
void sol_log_close(void);
void sol_log(int, const char *, ...);

/* Memory management */
void *sol_malloc(size_t);
void *sol_calloc(size_t, size_t);
void *sol_realloc(void *, size_t);
size_t malloc_size(void *);
void sol_free(void *);
char *sol_strdup(const char *);
char *update_integer_string(char *, int);

size_t memory_used(void);


#define log(...) sol_log( __VA_ARGS__ )
#define sol_debug(...) log(DEBUG, __VA_ARGS__)
#define sol_warning(...) log(WARNING, __VA_ARGS__)
#define sol_error(...) log(ERROR, __VA_ARGS__)
#define sol_info(...) log(INFORMATION, __VA_ARGS__)


#define STREQ(s1, s2, len) strncasecmp(s1, s2, len) == 0 ? true : false


#endif
