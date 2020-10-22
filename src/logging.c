/* BSD 2-Clause License
 *
 * Copyright (c) 2020, Andrea Giacomo Baldan All rights reserved.
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

#include <time.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <assert.h>
#include "logging.h"

#define MAX_LOG_SIZE 120

static FILE *fh = NULL;
static int logging_level = DEBUG;

void sol_log_init(const char *file, int level) {
    if (!file) return;
    fh = fopen(file, "a+");
    logging_level = level;
    if (!fh)
        printf("%lu * WARNING: Unable to open file %s\n",
               (unsigned long) time(NULL), file);
}

void sol_log_close(void) {
    if (fh) {
        fflush(fh);
        fclose(fh);
    }
}

void sol_log(int level, const char *fmt, ...) {

    if (level < logging_level)
        return;

    assert(fmt);

    va_list ap;
    char msg[MAX_LOG_SIZE + 4];

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    /* Truncate message too long and copy 3 bytes to make space for 3 dots */
    memcpy(msg + MAX_LOG_SIZE, "...", 3);
    msg[MAX_LOG_SIZE + 3] = '\0';

    // Open two handler, one for standard output and a second for the
    // persistent log file
    FILE *fp = stdout;

    if (!fp)
        return;

    fprintf(fp, "%lu %s\n", (unsigned long) time(NULL), msg);
    if (fh)
        fprintf(fh, "%lu %s\n", (unsigned long) time(NULL), msg);
}
