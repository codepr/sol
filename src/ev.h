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

#ifndef EV_H
#define EV_H

#include <sys/time.h>

enum ev_type {
    EV_NONE       = 0x00,
    EV_READ       = 0x01,
    EV_WRITE      = 0x02,
    EV_DISCONNECT = 0x04,
    EV_EVENTFD    = 0x08,
    EV_TIMERFD    = 0x10,
    EV_CLOSEFD    = 0x20
};

struct ev_ctx;

struct ev {
    int fd;
    int mask;
    union {
        void *data;
        void (*callback)(struct ev_ctx *);
    };
};

struct ev_ctx {
    int events_nr;
    int maxfd;
    struct ev *events_monitored;
    void *api;
};

void ev_init(struct ev_ctx *, int);

void ev_clone_ctx(struct ev_ctx *, const struct ev_ctx *);

void ev_destroy(struct ev_ctx *);

int ev_poll(struct ev_ctx *, time_t);

int ev_watch_fd(struct ev_ctx *, int, int);

int ev_del_fd(struct ev_ctx *, int);

int ev_get_event_type(struct ev_ctx *, int );

int ev_register_event(struct ev_ctx *, int, int, void *);

int ev_register_cron(struct ev_ctx *, void (*callback)(struct ev_ctx *),
                     long long, long long);

int ev_fire_event(struct ev_ctx *, int, int, void *);

int ev_read_event(struct ev_ctx *, int, int, void **);

int ev_get_fd(const struct ev_ctx *, int);

#endif
