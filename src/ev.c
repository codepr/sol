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

#include <time.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include "ev.h"
#include "util.h"
#include "config.h"

#ifdef EPOLL

struct epoll_api {
    int fd;
    struct epoll_event *events;
};

/* Epoll management functions */
static int epoll_add(int efd, int fd, int evs, void *data) {

    struct epoll_event ev;
    ev.data.fd = fd;

    // Being ev.data a union, in case of data != NULL, fd will be set to random
    if (data)
        ev.data.ptr = data;

    ev.events = evs | EPOLLHUP | EPOLLERR | EPOLLET;

    return epoll_ctl(efd, EPOLL_CTL_ADD, fd, &ev);
}

/*
 * Modify an epoll-monitored descriptor, automatically set EPOLLONESHOT in
 * addition to the other flags, which can be EPOLLIN for read and EPOLLOUT for
 * write
 */
static int epoll_mod(int efd, int fd, int evs, void *data) {

    struct epoll_event ev;
    ev.data.fd = fd;

    // Being ev.data a union, in case of data != NULL, fd will be set to random
    if (data)
        ev.data.ptr = data;

    ev.events = evs | EPOLLHUP | EPOLLERR | EPOLLET;

    return epoll_ctl(efd, EPOLL_CTL_MOD, fd, &ev);
}

/*
 * Remove a descriptor from an epoll descriptor, making it no-longer monitored
 * for events
 */
static int epoll_del(int efd, int fd) {
    return epoll_ctl(efd, EPOLL_CTL_DEL, fd, NULL);
}

static void ev_api_init(struct ev_ctx *ctx, int events_nr) {
    struct epoll_api *e_api = xmalloc(sizeof(*e_api));
    e_api->fd = epoll_create1(0);
    e_api->events = xcalloc(events_nr, sizeof(struct epoll_event));
    ctx->api = e_api;
    ctx->maxfd = events_nr;
}

static void ev_api_destroy(struct ev_ctx *ctx) {
    close(((struct epoll_api *) ctx->api)->fd);
    xfree(((struct epoll_api *) ctx->api)->events);
    xfree(ctx->api);
}

static int ev_api_get_event_type(struct ev_ctx *ctx, int idx) {
    struct epoll_api *e_api = ctx->api;
    int events = e_api->events[idx].events;
    int ev_mask = ((struct ev *) e_api->events[idx].data.ptr)->mask;
    // We want to remember the previous events only if they're not of type
    // CLOSE or TIMER
    int mask = ev_mask & (EV_CLOSEFD|EV_TIMERFD) ? ev_mask : 0;
    if (events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP))
        mask |= EV_DISCONNECT;
    else
        mask |= (events & EPOLLIN) ? EV_READ : EV_WRITE;
    return mask;
}

static int ev_api_poll(struct ev_ctx *ctx, time_t timeout) {
    struct epoll_api *e_api = ctx->api;
    return epoll_wait(e_api->fd, e_api->events, ctx->events_nr, timeout);
}

static int ev_api_watch_fd(struct ev_ctx *ctx, int fd) {
    struct epoll_api *e_api = ctx->api;
    return epoll_add(e_api->fd, fd, EPOLLIN, &ctx->events_monitored[fd]);
}

static int ev_api_del_fd(struct ev_ctx *ctx, int fd) {
    struct epoll_api *e_api = ctx->api;
    return epoll_del(e_api->fd, fd);
}

static int ev_api_register_event(struct ev_ctx *ctx, int fd, int mask) {
    struct epoll_api *e_api = ctx->api;
    int op = mask & EV_READ ? EPOLLIN : EPOLLOUT;
    return epoll_add(e_api->fd, fd, op, &ctx->events_monitored[fd]);
}

static int ev_api_fire_event(struct ev_ctx *ctx, int fd, int mask) {
    struct epoll_api *e_api = ctx->api;
    int ret = 0;
    int op = mask & EV_READ ? EPOLLIN : EPOLLOUT;
    if (mask & EV_EVENTFD) {
        (void) epoll_add(e_api->fd, fd, op, &ctx->events_monitored[fd]);
        ret = eventfd_write(fd, 1);
    } else {
        ret = epoll_mod(e_api->fd, fd, op, &ctx->events_monitored[fd]);
    }
    return ret;
}

static int ev_api_read_event(struct ev_ctx *ctx, int idx, int mask) {
    int err = 0;
    struct epoll_api *e_api = ctx->api;
    struct ev *e = e_api->events[idx].data.ptr;
    int fd = e->fd;
    if (mask & EV_EVENTFD) {
        eventfd_read(fd, &(eventfd_t){0L});
        err = close(fd);
    } else if (mask & EV_TIMERFD) {
        (void) read(fd, &(unsigned long int){0L}, sizeof(unsigned long int));
        e->callback(ctx, e->data);
    } else if (mask & EV_CLOSEFD) {
        eventfd_read(fd, &(eventfd_t){0L});
    } else {
        e->callback(ctx, e->data);
    }
    return err;
}

#else // EPOLL

struct select_api {
    fd_set rfds, wfds;
    fd_set _rfds, _wfds;
};

static void ev_api_init(struct ev_ctx *ctx, int events_nr) {
    (void) events_nr;
    struct select_api *s_api = xmalloc(sizeof(*s_api));
    FD_ZERO(&s_api->rfds);
    FD_ZERO(&s_api->wfds);
    ctx->api = s_api;
    ctx->maxfd = 0;
}

static void ev_api_destroy(struct ev_ctx *ctx) {
    xfree(ctx->api);
}

static int ev_api_get_event_type(struct ev_ctx *ctx, int idx) {
    struct select_api *s_api = ctx->api;
    int ev_mask = ctx->events_monitored[idx].mask;
    // We want to remember the previous events only if they're not of type
    // CLOSE or TIMER
    int mask = ev_mask & (EV_CLOSEFD|EV_TIMERFD) ? ev_mask : 0;
    if (FD_ISSET(idx, &s_api->_rfds)) mask |= EV_READ;
    if (FD_ISSET(idx, &s_api->_wfds)) mask |= EV_WRITE;
    return mask;
}

static int ev_api_poll(struct ev_ctx *ctx, time_t timeout) {
    struct timeval *tv = timeout > 0 ? &(struct timeval){ timeout, 0 } : NULL;
    struct select_api *s_api = ctx->api;
    memcpy(&s_api->_rfds, &s_api->rfds, sizeof(fd_set));
    memcpy(&s_api->_wfds, &s_api->wfds, sizeof(fd_set));
    int err = select(ctx->maxfd + 1, &s_api->_rfds, &s_api->_wfds, NULL, tv);
    if (err < 0)
        return err;
    return ctx->maxfd + 1;
}

static int ev_api_watch_fd(struct ev_ctx *ctx, int fd) {
    struct select_api *s_api = ctx->api;
    FD_SET(fd, &s_api->rfds);
    return 0;
}

static int ev_api_del_fd(struct ev_ctx *ctx, int fd) {
    struct select_api *s_api = ctx->api;
    FD_CLR(fd, &s_api->rfds);
    return 0;
}

static int ev_api_register_event(struct ev_ctx *ctx, int fd, int mask) {
    struct select_api *s_api = ctx->api;
    FD_SET(fd, mask & EV_READ ? &s_api->rfds : &s_api->wfds);
    return 0;
}

static int ev_api_fire_event(struct ev_ctx *ctx, int fd, int mask) {
    struct select_api *s_api = ctx->api;
    int ret = 0;
    FD_SET(fd, mask & EV_READ ? &s_api->rfds : &s_api->wfds);
    if (mask & EV_EVENTFD)
        ret = eventfd_write(fd, 1);
    return ret;
}

static int ev_api_read_event(struct ev_ctx *ctx, int idx, int mask) {
    if (mask == EV_NONE)
        return 0;
    int err = 0;
    struct ev *e = &ctx->events_monitored[idx];
    int fd = e->fd;
    if (mask & EV_EVENTFD) {
        eventfd_read(fd, &(eventfd_t){0L});
        err = close(fd);
    } else if (mask & EV_TIMERFD) {
        err = read(fd, &(unsigned long int){0L}, sizeof(unsigned long int));
        if (err > 0)
            e->callback(ctx, e->data);
    } else if (mask & EV_CLOSEFD) {
        eventfd_read(fd, &(eventfd_t){0L});
    } else {
        e->callback(ctx, e->data);
    }
    return err;
}

#endif // SELECT

/*
 * Auxiliary function, update FD, mask and data in monitored events array.
 * Monitored events are the same number as the maximum FD registered in the
 * context.
 */
static void ev_add_monitored(struct ev_ctx *ctx, int fd, int mask,
                             void (*callback)(struct ev_ctx *, void *),
                             void *ptr) {
    if (fd > ctx->maxfd) {
        int i = ctx->maxfd;
        ctx->maxfd = fd;
        if (fd > ctx->events_nr) {
            ctx->events_monitored =
                xrealloc(ctx->events_monitored, (fd + 1) * sizeof(struct ev));
            for (; i < ctx->maxfd; ++i)
                ctx->events_monitored[i].mask = EV_NONE;
        }
    }
    ctx->events_monitored[fd].fd = fd;
    ctx->events_monitored[fd].mask = mask;
    ctx->events_monitored[fd].data = ptr;
    ctx->events_monitored[fd].callback = callback;
}

void ev_init(struct ev_ctx *ctx, int events_nr) {
    ev_api_init(ctx, events_nr);
    ctx->events_nr = events_nr;
    ctx->events_monitored = xcalloc(events_nr, sizeof(struct ev));
    for (int i = 0; i < events_nr; ++i)
        ctx->events_monitored[i].mask = EV_NONE;
}

void ev_destroy(struct ev_ctx *ctx) {
    for (int i = 0; i < ctx->maxfd; ++i) {
        if (!(ctx->events_monitored[i].mask & EV_CLOSEFD) &&
            ctx->events_monitored[i].mask != EV_NONE)
            ev_del_fd(ctx, ctx->events_monitored[i].fd);
    }
    xfree(ctx->events_monitored);
    ev_api_destroy(ctx);
}

int ev_get_event_type(struct ev_ctx *ctx, int idx) {
    return ev_api_get_event_type(ctx, idx);
}

int ev_poll(struct ev_ctx *ctx, time_t timeout) {
    return ev_api_poll(ctx, timeout);
}

int ev_run(struct ev_ctx *ctx) {
    int n = 0, events = 0;
    while (1) {
        n = ev_poll(ctx, -1);
        if (n < 0)
            break;
        for (int i = 0; i < n; ++i) {
            events = ev_get_event_type(ctx, i);
            ev_read_event(ctx, i, events);
        }
    }
    return n;
}

int ev_watch_fd(struct ev_ctx *ctx, int fd, int mask) {
    ev_add_monitored(ctx, fd, mask, NULL, NULL);
    return ev_api_watch_fd(ctx, fd);
}

int ev_del_fd(struct ev_ctx *ctx, int fd) {
    ctx->events_monitored[fd].mask = EV_NONE;
    ctx->events_monitored[fd].data = NULL;
    ctx->events_monitored[fd].callback = NULL;
    return ev_api_del_fd(ctx, fd);
}

int ev_register_event(struct ev_ctx *ctx, int fd, int mask,
                      void (*callback)(struct ev_ctx *, void *), void *data) {
    ev_add_monitored(ctx, fd, mask, callback, data);
    int ret = 0;
    ret = ev_api_register_event(ctx, fd, mask);
    if (mask & EV_EVENTFD)
        (void) eventfd_write(fd, 1);
    return ret;
}

int ev_register_cron(struct ev_ctx *ctx,
                     void (*callback)(struct ev_ctx *, void *),
                     long long s, long long ns) {
    struct itimerspec timer;
    memset(&timer, 0x00, sizeof(timer));
    timer.it_value.tv_sec = s;
    timer.it_value.tv_nsec = ns;
    timer.it_interval.tv_sec = s;
    timer.it_interval.tv_nsec = ns;

    int timerfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);

    if (timerfd_settime(timerfd, 0, &timer, NULL) < 0)
        return -1;

    // Add the timer to the event loop
    ev_add_monitored(ctx, timerfd, EV_TIMERFD, callback, NULL);
    return ev_api_watch_fd(ctx, timerfd);
}

int ev_fire_event(struct ev_ctx *ctx, int fd, int mask,
                  void (*callback)(struct ev_ctx *, void *), void *data) {
    ev_add_monitored(ctx, fd, mask, callback, data);
    return ev_api_fire_event(ctx, fd, mask);
}

int ev_read_event(struct ev_ctx *ctx, int idx, int mask) {
    return ev_api_read_event(ctx, idx, mask);
}
