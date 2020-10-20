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

#include <errno.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#else
#include <sys/socket.h>
#endif
#include "ev.h"
#include "util.h"
#include "config.h"

#if defined(EPOLL)

/*
 * =========================
 *  Epoll backend functions
 * =========================
 *
 * The epoll_api structure contains the epoll fd and the events array needed to
 * wait on events with epoll_wait(2) blocking call. It's the best multiplexing
 * IO api available on Linux systems and thus the optimal choice.
 */

#include <sys/epoll.h>

struct epoll_api {
    int fd;
    struct epoll_event *events;
};

/*
 * Epoll management function, register a file descriptor to an EPOLL
 * descriptor, to be monitored for read/write events
 */
static int epoll_add(int efd, int fd, int evs, void *data) {

    struct epoll_event ev;
    ev.data.fd = fd;

    // Being ev.data a union, in case of data != NULL, fd will be set to random
    if (data)
        ev.data.ptr = data;

    ev.events = evs | EPOLLHUP | EPOLLERR;

    return epoll_ctl(efd, EPOLL_CTL_ADD, fd, &ev);
}

/*
 * Modify an epoll-monitored descriptor, can be set to EPOLLIN for read and
 * EPOLLOUT for write
 */
static int epoll_mod(int efd, int fd, int evs, void *data) {

    struct epoll_event ev;
    ev.data.fd = fd;

    // Being ev.data a union, in case of data != NULL, fd will be set to random
    if (data)
        ev.data.ptr = data;

    ev.events = evs | EPOLLHUP | EPOLLERR;

    return epoll_ctl(efd, EPOLL_CTL_MOD, fd, &ev);
}

/*
 * Remove a descriptor from an epoll descriptor, making it no-longer monitored
 * for events
 */
static int epoll_del(int efd, int fd) {
    return epoll_ctl(efd, EPOLL_CTL_DEL, fd, NULL);
}

static int ev_api_init(struct ev_ctx *ctx, int events_nr) {
    struct epoll_api *e_api = xmalloc(sizeof(*e_api));
    if (!e_api)
        return -EV_OOM;
    e_api->fd = epoll_create1(0);
    e_api->events = xcalloc(events_nr, sizeof(struct epoll_event));
    ctx->api = e_api;
    ctx->maxfd = events_nr;
    return EV_OK;
}

static void ev_api_destroy(struct ev_ctx *ctx) {
    close(((struct epoll_api *) ctx->api)->fd);
    xfree(((struct epoll_api *) ctx->api)->events);
    xfree(ctx->api);
}

static int ev_api_get_event_type(struct ev_ctx *ctx, int idx) {
    struct epoll_api *e_api = ctx->api;
    int events = e_api->events[idx].events;
    int ev_mask = ctx->events_monitored[e_api->events[idx].data.fd].mask;
    // We want to remember the previous events only if they're not of type
    // CLOSE or TIMER
    int mask = ev_mask & (EV_CLOSEFD|EV_TIMERFD) ? ev_mask : EV_NONE;
    if (events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) mask |= EV_DISCONNECT;
    if (events & EPOLLIN) mask |= EV_READ;
    if (events & EPOLLOUT) mask |= EV_WRITE;
    return mask;
}

static int ev_api_poll(struct ev_ctx *ctx, time_t timeout) {
    struct epoll_api *e_api = ctx->api;
    return epoll_wait(e_api->fd, e_api->events, ctx->events_nr, timeout);
}

static int ev_api_watch_fd(struct ev_ctx *ctx, int fd) {
    struct epoll_api *e_api = ctx->api;
    return epoll_add(e_api->fd, fd, EPOLLIN, NULL);
}

static int ev_api_del_fd(struct ev_ctx *ctx, int fd) {
    struct epoll_api *e_api = ctx->api;
    return epoll_del(e_api->fd, fd);
}

static int ev_api_register_event(struct ev_ctx *ctx, int fd, int mask) {
    struct epoll_api *e_api = ctx->api;
    int op = 0;
    if (mask & EV_READ) op |= EPOLLIN;
    if (mask & EV_WRITE) op |= EPOLLOUT;
    return epoll_add(e_api->fd, fd, op, NULL);
}

static int ev_api_fire_event(struct ev_ctx *ctx, int fd, int mask) {
    struct epoll_api *e_api = ctx->api;
    int op = 0;
    if (mask & EV_READ) op |= EPOLLIN;
    if (mask & EV_WRITE) op |= EPOLLOUT;
    if (mask & EV_EVENTFD)
        return epoll_add(e_api->fd, fd, op, NULL);
    return epoll_mod(e_api->fd, fd, op, NULL);
}

/*
 * Get the event on the idx position inside the events map. The event can also
 * be an unset one (EV_NONE)
 */
static inline struct ev *ev_api_fetch_event(const struct ev_ctx *ctx,
                                            int idx, int mask) {
    int fd = ((struct epoll_api *) ctx->api)->events[idx].data.fd;
    return ctx->events_monitored + fd;
}

#elif defined(POLL)

/*
 * =========================
 *  Poll backend functions
 * =========================
 *
 * The poll_api structure contains the number of fds to monitor and the array
 * of pollfd structures associated. This number must be adjusted everytime a
 * client disconnect or a new connection have an fd > nfds to avoid iterating
 * over closed fds everytime a new event is triggered.
 * As select, poll iterate linearly over all the triggered events, without the
 * hard limit of 1024 connections. It's the second best option available if no
 * epoll or kqueue for Mac OSX are not present.
 */

#include <poll.h>

struct poll_api {
    int nfds;
    int events_monitored;
    struct pollfd *fds;
};

static int ev_api_init(struct ev_ctx *ctx, int events_nr) {
    struct poll_api *p_api = xmalloc(sizeof(*p_api));
    if (!p_api)
        return -EV_OOM;
    p_api->nfds = 0;
    p_api->fds = xcalloc(events_nr, sizeof(struct pollfd));
    p_api->events_monitored = events_nr;
    ctx->api = p_api;
    ctx->maxfd = events_nr;
    return EV_OK;
}

static void ev_api_destroy(struct ev_ctx *ctx) {
    xfree(((struct poll_api *) ctx->api)->fds);
    xfree(ctx->api);
}

static int ev_api_get_event_type(struct ev_ctx *ctx, int idx) {
    struct poll_api *p_api = ctx->api;
    int ev_mask = ctx->events_monitored[p_api->fds[idx].fd].mask;
    // We want to remember the previous events only if they're not of type
    // CLOSE or TIMER
    int mask = ev_mask & (EV_CLOSEFD|EV_TIMERFD) ? ev_mask : 0;
    if (p_api->fds[idx].revents & (POLLHUP|POLLERR)) mask |= EV_DISCONNECT;
    if (p_api->fds[idx].revents & POLLIN) mask |= EV_READ;
    if (p_api->fds[idx].revents & POLLOUT) mask |= EV_WRITE;
    return mask;
}

static int ev_api_poll(struct ev_ctx *ctx, time_t timeout) {
    struct poll_api *p_api = ctx->api;
    int err = poll(p_api->fds, p_api->nfds, timeout);
    if (err < 0)
        return -EV_ERR;
    return p_api->nfds;
}

/*
 * Poll maintain in his state the number of file descriptor it monitor in a
 * fixed size array just like the events we monitor over the primitive. If a
 * resize is needed cause the number of fds have reached the length of the fds
 * array, we must increase its size.
 */
static int ev_api_watch_fd(struct ev_ctx *ctx, int fd) {
    struct poll_api *p_api = ctx->api;
    p_api->fds[p_api->nfds].fd = fd;
    p_api->fds[p_api->nfds].events = POLLIN;
    p_api->nfds++;
    if (p_api->nfds >= p_api->events_monitored) {
        p_api->events_monitored *= 2;
        p_api->fds = xrealloc(p_api->fds,
                                 p_api->events_monitored * sizeof(struct pollfd));
    }
    return EV_OK;
}

static int ev_api_del_fd(struct ev_ctx *ctx, int fd) {
    struct poll_api *p_api = ctx->api;
    for (int i = 0; i < p_api->nfds; ++i) {
        if (p_api->fds[i].fd == fd) {
            p_api->fds[i].fd = -1;
            p_api->fds[i].events = 0;
            // Resize fds array
            for(int j = i; j < p_api->nfds-1; ++j)
                p_api->fds[j].fd = p_api->fds[j + 1].fd;
            p_api->nfds--;
            break;
        }
    }
    return EV_OK;
}

/*
 * We have to check for resize even here just like ev_api_watch_fd.
 */
static int ev_api_register_event(struct ev_ctx *ctx, int fd, int mask) {
    struct poll_api *p_api = ctx->api;
    p_api->fds[p_api->nfds].fd = fd;
    if (mask & EV_READ) p_api->fds[p_api->nfds].events |= POLLIN;
    if (mask & EV_WRITE) p_api->fds[p_api->nfds].events |= POLLOUT;
    p_api->nfds++;
    if (p_api->nfds >= p_api->events_monitored) {
        p_api->events_monitored *= 2;
        p_api->fds = xrealloc(p_api->fds,
                                 p_api->events_monitored * sizeof(struct pollfd));
    }
    return EV_OK;
}

static int ev_api_fire_event(struct ev_ctx *ctx, int fd, int mask) {
    struct poll_api *p_api = ctx->api;
    for (int i = 0; i < p_api->nfds; ++i) {
        if (p_api->fds[i].fd == fd) {
            p_api->fds[i].events = mask & EV_READ ? POLLIN : POLLOUT;
            break;
        }
    }
    return EV_OK;
}

/*
 * Get the event on the idx position inside the events map. The event can also
 * be an unset one (EV_NONE)
 */
static inline struct ev *ev_api_fetch_event(const struct ev_ctx *ctx,
                                            int idx, int mask) {
    return ctx->events_monitored + ((struct poll_api *) ctx->api)->fds[idx].fd;
}

#elif defined(SELECT)

/*
 * ==========================
 *  Select backend functions
 * ==========================
 *
 * The select_api structure contains two copies of the read/write fdset, it's
 * a measure to reset the original monitored fd_set after each select(2) call
 * as it's not safe to iterate over the already selected sets, select(2) make
 * side-effects on the passed in fd_sets.
 * At each new event all the monitored fd_set are iterated and check for read
 * or write readiness; the number of monitored sockets is hard-capped at 1024.
 * It's the oldest multiplexing IO and practically obiquitous, making it the
 * perfect fallback for every system.
 */
struct select_api {
    fd_set rfds, wfds;
    // Copy of the original fdset arrays to re-initialize them after each cycle
    // we'll call them "service" fdset
    fd_set _rfds, _wfds;
};

static int ev_api_init(struct ev_ctx *ctx, int events_nr) {
    /*
     * fd_set is an array of 32 i32 and each FD is represented by a bit so
     * 32 x 32 = 1024 as hard limit
     */
    assert(events_nr <= 1024);
    struct select_api *s_api = xmalloc(sizeof(*s_api));
    if (!s_api)
        return -EV_OOM;
    FD_ZERO(&s_api->rfds);
    FD_ZERO(&s_api->wfds);
    ctx->api = s_api;
    ctx->maxfd = 0;
    return EV_OK;
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
    /*
     * Select checks all FDs by looping to the highest registered FD it
     * currently monitor. Even non set or non monitored FDs are inspected, we
     * have to ensure that the FD is currently ready for IO, otherwise we'll
     * end up looping all FDs and calling callbacks everytime, even when
     * there's no need to.
     *
     * Also we have to check for ready FDs on "service" _fdsets, cause they're
     * the ones employed on the select call to avoid side-effects on the
     * originals.
     */
    if (!FD_ISSET(idx, &s_api->_rfds) && !FD_ISSET(idx, &s_api->_wfds))
        return EV_NONE;
    if (FD_ISSET(idx, &s_api->_rfds)) mask |= EV_READ;
    if (FD_ISSET(idx, &s_api->_wfds)) mask |= EV_WRITE;
    return mask;
}

static int ev_api_poll(struct ev_ctx *ctx, time_t timeout) {
    struct timeval *tv =
        timeout > 0 ? &(struct timeval){ 0, timeout * 1000 } : NULL;
    struct select_api *s_api = ctx->api;
    // Re-initialize fdset arrays cause select call side-effect the originals
    memcpy(&s_api->_rfds, &s_api->rfds, sizeof(fd_set));
    memcpy(&s_api->_wfds, &s_api->wfds, sizeof(fd_set));
    int err = select(ctx->maxfd + 1, &s_api->_rfds, &s_api->_wfds, NULL, tv);
    if (err < 0)
        return -EV_ERR;
    return ctx->maxfd + 1;
}

static int ev_api_watch_fd(struct ev_ctx *ctx, int fd) {
    struct select_api *s_api = ctx->api;
    FD_SET(fd, &s_api->rfds);
    // Check for a possible new max fd, we don't want to miss events on FDs
    if (fd > ctx->maxfd)
        ctx->maxfd = fd;
    return EV_OK;
}

static int ev_api_del_fd(struct ev_ctx *ctx, int fd) {
    struct select_api *s_api = ctx->api;
    if (FD_ISSET(fd, &s_api->rfds)) FD_CLR(fd, &s_api->rfds);
    if (FD_ISSET(fd, &s_api->wfds)) FD_CLR(fd, &s_api->wfds);
    /*
     * To remove  FD from select we must determine the new maximum descriptor
     * value based on the bits that are still turned on in the rfds set.
     */
    if (fd == ctx->maxfd) {
        while (!FD_ISSET(ctx->maxfd, &s_api->rfds)
               && !FD_ISSET(ctx->maxfd, &s_api->wfds))
            ctx->maxfd -= 1;
    }
    return EV_OK;
}

static int ev_api_register_event(struct ev_ctx *ctx, int fd, int mask) {
    struct select_api *s_api = ctx->api;
    if (mask & EV_READ) FD_SET(fd, &s_api->rfds);
    if (mask & EV_WRITE) FD_SET(fd, &s_api->wfds);
    // Check for a possible new max fd, we don't want to miss events on FDs
    if (fd > ctx->maxfd)
        ctx->maxfd = fd;
    return EV_OK;
}

static int ev_api_fire_event(struct ev_ctx *ctx, int fd, int mask) {
    struct select_api *s_api = ctx->api;
    if (mask & EV_READ) FD_SET(fd, &s_api->rfds);
    if (mask & EV_WRITE) FD_SET(fd, &s_api->wfds);
    return EV_OK;
}

/*
 * Get the event on the idx position inside the events map. The event can also
 * be an unset one (EV_NONE)
 */
static inline struct ev *ev_api_fetch_event(const struct ev_ctx *ctx,
                                            int idx, int mask) {
    return ctx->events_monitored + idx;
}

#elif defined(KQUEUE)

/*
 * ==========================
 *  Kqueue backend functions
 * ==========================
 *
 * The Epoll counterpart on BSD systems, including Mac OSX, it's the older of
 * the two Mux IO implementations and on par in terms of performances, a bit
 * more versatile as it's possible to schedule timers directly as events
 * instead of relying on support mechanisms like `timerfd` on linux.
 */

#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>

struct kqueue_api {
    int fd;
    struct kevent *events;
};

static int ev_api_init(struct ev_ctx *ctx, int events_nr) {
    struct kqueue_api *k_api = xmalloc(sizeof(*k_api));
    if (!k_api)
        return -EV_OOM;
    k_api->fd = kqueue();
    k_api->events = xcalloc(events_nr, sizeof(struct kevent));
    ctx->api = k_api;
    ctx->maxfd = events_nr;
    return EV_OK;
}

static void ev_api_destroy(struct ev_ctx *ctx) {
    close(((struct kqueue_api *) ctx->api)->fd);
    xfree(((struct kqueue_api *) ctx->api)->events);
    xfree(ctx->api);
}

static int ev_api_get_event_type(struct ev_ctx *ctx, int idx) {
    struct kqueue_api *k_api = ctx->api;
    int events = k_api->events[idx].flags;
    int ev_mask = ctx->events_monitored[k_api->events[idx].ident].mask;
    // We want to remember the previous events only if they're not of type
    // CLOSE or TIMER
    int mask = ev_mask & (EV_CLOSEFD | EV_TIMERFD) ? ev_mask : EV_NONE;
    if (events & (EV_EOF | EV_ERROR)) mask |= EV_DISCONNECT;
    if (events & EVFILT_READ) mask |= EV_READ;
    if (events & EVFILT_WRITE) mask |= EV_WRITE;
    return mask;
}

static int ev_api_poll(struct ev_ctx *ctx, time_t timeout) {
    struct kqueue_api *k_api = ctx->api;
    struct timespec ts_timeout;
    ts_timeout.tv_sec = timeout;
    ts_timeout.tv_nsec = 0;
    int err = kevent(k_api->fd, NULL, 0,
                     k_api->events, ctx->maxevents, &ts_timeout);
    if (err < 0)
        return -EV_ERR;
    return err;
}

static int ev_api_del_fd(struct ev_ctx *ctx, int fd) {
    struct kqueue_api *k_api = ctx->api;
    struct kevent ke;
    int ev_mask = ctx->events_monitored[fd].mask;
    int mask = 0;
    if (ev_mask & EV_READ) mask |= EVFILT_READ;
    if (ev_mask & EV_WRITE) mask |= EVFILT_WRITE;
    if (ev_mask & EV_TIMERFD) mask |= EVFILT_TIMER;
    EV_SET(&ke, fd, mask, EV_DELETE, 0, 0, NULL);
    if (kevent(k_api->fd, &ke, 1, NULL, 0, NULL) == -1)
        return -EV_ERR;
    return EV_OK;
}

static int ev_api_register_event(struct ev_ctx *ctx, int fd, int mask) {
    struct kqueue_api *k_api = ctx->api;
    struct kevent ke;
    int op = 0;
    if (mask & EV_READ) op |= EVFILT_READ;
    if (mask & EV_WRITE) op |= EVFILT_WRITE;
    EV_SET(&ke, fd, op, EV_ADD, 0, 0, NULL);
    if (kevent(k_api->fd, &ke, 1, NULL, 0, NULL) == -1)
        return -EV_ERR;
    return EV_OK;
}

static int ev_api_watch_fd(struct ev_ctx *ctx, int fd) {
    return ev_api_register_event(ctx, fd, EV_READ);
}

static int ev_api_fire_event(struct ev_ctx *ctx, int fd, int mask) {
    struct kqueue_api *k_api = ctx->api;
    struct kevent ke;
    int op = 0;
    if (mask & (EV_READ | EV_EVENTFD)) op |= EVFILT_READ;
    if (mask & EV_WRITE) op |= EVFILT_WRITE;
    EV_SET(&ke, fd, op, EV_ADD | EV_ENABLE, 0, 0, NULL);
    if (kevent(k_api->fd, &ke, 1, NULL, 0, NULL) == -1)
        return -EV_ERR;
    return EV_OK;
}

/*
 * Get the event on the idx position inside the events map. The event can also
 * be an unset one (EV_NONE)
 */
static inline struct ev *ev_api_fetch_event(const struct ev_ctx *ctx,
                                            int idx, int mask) {
    (void) mask; // silence compiler warning
    int fd = ((struct kqueue_api *) ctx->api)->events[idx].ident;
    return ctx->events_monitored + fd;
}

#endif // KQUEUE

/*
 * Process the event at the position idx in the events_monitored array. Read or
 * write events can be executed on the same iteration, differentiating just
 * on EV_CLOSEFD or EV_EVENTFD.
 * Returns the number of fired callbacks.
 */
static int ev_process_event(struct ev_ctx *ctx, int idx, int mask) {
    if (mask == EV_NONE) return EV_OK;
    struct ev *e = ev_api_fetch_event(ctx, idx, mask);
    int err = 0, fired = 0, fd = e->fd;
    if (mask & EV_CLOSEFD) {
#ifdef __linux__
        err = eventfd_read(fd, &(eventfd_t){0});
#else
        err = read(fd, &(unsigned long){0}, sizeof(unsigned long));
#endif // __linux__
        if (err < 0) return EV_OK;
        e->rcallback(ctx, e->rdata);
        ++fired;
    } else {
        if (mask & EV_EVENTFD) {
#ifdef __linux__
            err = eventfd_read(fd, &(eventfd_t){0L});
#else
            err = read(fd, &(unsigned long){0}, sizeof(unsigned long));
#endif // __linux__
            close(fd);
        } else if (mask & EV_TIMERFD) {
            err = read(fd, &(unsigned long int){0L}, sizeof(unsigned long int));
        }
        if (err < 0) return EV_OK;
        if (mask & EV_READ) {
            e->rcallback(ctx, e->rdata);
            ++fired;
        }
        if (mask & EV_WRITE) {
            if (!fired || e->wcallback != e->rcallback) {
                e->wcallback(ctx, e->wdata);
                ++fired;
            }
        }
    }
    return fired;
}

/*
 * Auxiliary function, update FD, mask and data in monitored events array.
 * Monitored events are the same number as the maximum FD registered in the
 * context.
 */
static void ev_add_monitored(struct ev_ctx *ctx, int fd, int mask,
                             void (*callback)(struct ev_ctx *, void *),
                             void *ptr) {
    /*
     * TODO check for fd <= 1024 if using SELECT
     * That is because FD_SETSIZE is fixed to 1024, fd_set is an array of 32
     * i32 and each FD is represented by a bit so 32 x 32 = 1024 as hard limit
     */
    if (fd > ctx->maxevents) {
        int i = ctx->maxevents;
        ctx->maxevents = fd;
        if (fd > ctx->events_nr) {
            ctx->events_monitored =
                xrealloc(ctx->events_monitored, (fd + 1) * sizeof(struct ev));
            for (; i < ctx->maxevents; ++i)
                ctx->events_monitored[i].mask = EV_NONE;
        }
    }
    ctx->events_monitored[fd].fd = fd;
    ctx->events_monitored[fd].mask |= mask;
    if (mask & EV_READ) {
        ctx->events_monitored[fd].rdata = ptr;
        ctx->events_monitored[fd].rcallback = callback;
    }
    if (mask & EV_WRITE) {
        ctx->events_monitored[fd].wdata = ptr;
        ctx->events_monitored[fd].wcallback = callback;
    }
}

static inline int ev_get_event_type(struct ev_ctx *ctx, int idx) {
    return ev_api_get_event_type(ctx, idx);
}

int ev_init(struct ev_ctx *ctx, int events_nr) {
    int err = ev_api_init(ctx, events_nr);
    if (err < 0)
        return err;
    ctx->stop = 0;
    ctx->fired_events = 0;
    ctx->maxevents = events_nr;
    ctx->events_nr = events_nr;
    ctx->events_monitored = xcalloc(events_nr, sizeof(struct ev));
    return EV_OK;
}

void ev_destroy(struct ev_ctx *ctx) {
    for (int i = 0; i < ctx->maxevents; ++i) {
        if (!(ctx->events_monitored[i].mask & EV_CLOSEFD) &&
            ctx->events_monitored[i].mask != EV_NONE)
            ev_del_fd(ctx, ctx->events_monitored[i].fd);
    }
    xfree(ctx->events_monitored);
    ev_api_destroy(ctx);
}

int ev_poll(struct ev_ctx *ctx, time_t timeout) {
    return ev_api_poll(ctx, timeout);
}

int ev_run(struct ev_ctx *ctx) {
    int n = 0, events = 0;
    /*
     * Start an infinite loop, can be stopped only by scheduling an ev_stop
     * callback or if an error on the underlying backend occur
     */
    while (!ctx->stop) {
        /*
         * blocks polling for events, -1 means forever. Returns only in case of
         * valid events ready to be processed or errors
         */
        n = ev_poll(ctx, -1);
        if (n < 0) {
            /* Signals to all threads. Ignore it for now */
            if (errno == EINTR)
                continue;
            /* Error occured, break the loop */
            break;
        }
        for (int i = 0; i < n; ++i) {
            events = ev_get_event_type(ctx, i);
            ctx->fired_events += ev_process_event(ctx, i, events);
        }
    }
    return n;
}

void ev_stop(struct ev_ctx *ctx) {
   ctx->stop = 1;
}

int ev_watch_fd(struct ev_ctx *ctx, int fd, int mask) {
    ev_add_monitored(ctx, fd, mask, NULL, NULL);
    return ev_api_watch_fd(ctx, fd);
}

int ev_del_fd(struct ev_ctx *ctx, int fd) {
    memset(ctx->events_monitored + fd, 0x00, sizeof(struct ev));
    return ev_api_del_fd(ctx, fd);
}

/*
 * Set a callback and an argument to be passed to for the next loop cycle,
 * associating it to a file descriptor, ultimately resulting in an event to be
 * dispatched and processed.
 *
 * The difference with ev_fire_event is that this function should be called
 * when the file descriptor is not registered in the loop yet.
 *
 * - mask: bitmask used to describe what type of event we're going to fire
 * - callback:  is a function pointer to the routine we want to execute
 * - data:  an opaque pointer to the arguments for the callback.
 */
int ev_register_event(struct ev_ctx *ctx, int fd, int mask,
                      void (*callback)(struct ev_ctx *, void *), void *data) {
    ev_add_monitored(ctx, fd, mask, callback, data);
    int ret = 0;
    ret = ev_api_register_event(ctx, fd, mask);
    if (ret < 0) return -EV_ERR;
    if (mask & EV_EVENTFD)
#ifdef __linux__
        (void) eventfd_write(fd, 1);
#else
        (void) write(fd, &(unsigned long){1}, sizeof(unsigned long));
#endif
    return EV_OK;
}

/*
 * Register a periodically repeate callback and args to be passed to a running
 * loop, specifying, seconds and/or nanoseconds defining how often the callback
 * should be executed.
 */
int ev_register_cron(struct ev_ctx *ctx,
                     void (*callback)(struct ev_ctx *, void *),
                     void *data,
                     long long s, long long ns) {
#ifdef __linux__
    struct itimerspec timer;
    memset(&timer, 0x00, sizeof(timer));
    timer.it_value.tv_sec = s;
    timer.it_value.tv_nsec = ns;
    timer.it_interval.tv_sec = s;
    timer.it_interval.tv_nsec = ns;

    int timerfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);

    if (timerfd_settime(timerfd, 0, &timer, NULL) < 0)
        return -EV_ERR;

    // Add the timer to the event loop
    ev_add_monitored(ctx, timerfd, EV_TIMERFD|EV_READ, callback, data);
    return ev_api_watch_fd(ctx, timerfd);
#else
    struct kqueue_api *k_api = ctx->api;
    // milliseconds
    unsigned period = (s * 1000)  + (ns / 100);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    ev_add_monitored(ctx, fd, EV_TIMERFD|EV_READ, callback, data);
    struct kevent ke;
    EV_SET(&ke, fd, EVFILT_TIMER, EV_ADD | EV_ENABLE, 0, period, 0);
    if (kevent(k_api->fd, &ke, 1, NULL, 0, NULL) == -1)
        return -EV_ERR;
    return EV_OK;
#endif // __linux__
}

/*
 * Set a callback and an argument to be passed to for the next loop cycle,
 * associating it to a file descriptor, ultimately resulting in an event to be
 * dispatched and processed.
 *
 * Behave like ev_register_event but it's meant to be called when the file
 * descriptor is already registered in the loop.
 *
 * - mask: bitmask used to describe what type of event we're going to fire
 * - callback:  is a function pointer to the routine we want to execute
 * - data:  an opaque pointer to the arguments for the callback.
 */
int ev_fire_event(struct ev_ctx *ctx, int fd, int mask,
                  void (*callback)(struct ev_ctx *, void *), void *data) {
    int ret = 0;
    ev_add_monitored(ctx, fd, mask, callback, data);
    ret = ev_api_fire_event(ctx, fd, mask);
    if (ret < 0) return -EV_ERR;
    if (mask & EV_EVENTFD) {
#ifdef __linux__
        ret = eventfd_write(fd, 1);
#else
        ret = write(fd, &(unsigned long){1}, sizeof(unsigned long));
#endif // __linux__
        if (ret < 0) return -EV_ERR;
    }
    return EV_OK;
}
