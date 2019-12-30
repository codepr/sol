#include <time.h>
#include <string.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include "eventloop.h"
#include "util.h"

/* Epoll management functions */
static int epoll_add(int efd, int fd, int evs, void *data) {

    struct epoll_event ev;
    ev.data.fd = fd;

    // Being ev.data a union, in case of data != NULL, fd will be set to random
    if (data)
        ev.data.ptr = data;

    ev.events = evs | EPOLLHUP | EPOLLERR | EPOLLET | EPOLLONESHOT;

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

    ev.events = evs | EPOLLHUP | EPOLLERR | EPOLLET | EPOLLONESHOT;

    return epoll_ctl(efd, EPOLL_CTL_MOD, fd, &ev);
}

/*
 * Remove a descriptor from an epoll descriptor, making it no-longer monitored
 * for events
 */
static int epoll_del(int efd, int fd) {
    return epoll_ctl(efd, EPOLL_CTL_DEL, fd, NULL);
}

static void ev_add_monitored(struct ev_ctx *ctx, int fd, int mask, void *ptr) {
    if (fd > ctx->maxfd) {
        int i = ctx->maxfd;
        ctx->maxfd = fd + 1;
        ctx->events_monitored = xrealloc(ctx->events_monitored, ctx->maxfd);
        for (; i < ctx->maxfd; ++i)
            ctx->events_monitored[i].mask = EV_NONE;
    }
    ctx->events_monitored[fd].fd = fd;
    ctx->events_monitored[fd].mask = mask;
    ctx->events_monitored[fd].data = ptr;
}

void ev_init(struct ev_ctx *ctx, int events_nr) {
    struct epoll_api *e_api = xmalloc(sizeof(*e_api));
    e_api->fd = epoll_create1(0);
    e_api->events = xcalloc(events_nr, sizeof(struct epoll_event));
    ctx->api = e_api;
    ctx->maxfd = events_nr;
    ctx->events_nr = events_nr;
    ctx->events_monitored = xcalloc(events_nr, sizeof(struct ev));
    for (int i = 0; i < events_nr; ++i)
        ctx->events_monitored[i].mask = EV_NONE;
}

void ev_clone_ctx(struct ev_ctx *ctx, int fd, int events_nr) {
    struct epoll_api *e_api = xmalloc(sizeof(*e_api));
    e_api->fd = fd;
    e_api->events = xcalloc(events_nr, sizeof(struct epoll_event));
    ctx->api = e_api;
    ctx->maxfd = events_nr;
    ctx->events_nr = events_nr;
    ctx->events_monitored = xcalloc(events_nr, sizeof(struct ev));
    for (int i = 0; i < events_nr; ++i)
        ctx->events_monitored[i].mask = EV_NONE;
}

void ev_destroy(struct ev_ctx *ctx) {
    xfree(((struct epoll_api *) ctx->api)->events);
    xfree(ctx->events_monitored);
    xfree(ctx->api);
}

int ev_get_event_type(struct ev_ctx *ctx, int idx) {
    struct epoll_api *e_api = ctx->api;
    int events = e_api->events[idx].events;
    int ev_mask = ((struct ev *) e_api->events[idx].data.ptr)->mask;
    int mask = ev_mask & (EV_CLOSEFD|EV_TIMERFD) ? ev_mask : 0;
    if (events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP))
        mask |= EV_DISCONNECT;
    else
        mask |= (events & EPOLLIN) ? EV_READ : EV_WRITE;
    return mask;
}

int ev_poll(struct ev_ctx *ctx, time_t timeout) {
    struct epoll_api *e_api = ctx->api;
    return epoll_wait(e_api->fd, e_api->events, ctx->events_nr, timeout);
}

int ev_watch_fd(struct ev_ctx *ctx, int fd, int mask) {
    struct epoll_api *e_api = ctx->api;
    ev_add_monitored(ctx, fd, mask, NULL);
    int err = epoll_add(e_api->fd, fd, EPOLLIN, &ctx->events_monitored[fd]);
    return err;
}

int ev_del_fd(struct ev_ctx *ctx, int fd) {
    struct epoll_api *e_api = ctx->api;
    ctx->events_monitored[fd].mask = EV_NONE;
    int err = epoll_del(e_api->fd, fd);
    return err;
}

int ev_register_event(struct ev_ctx *ctx, int fd, int mask, void *data) {
    struct epoll_api *e_api = ctx->api;
    int ret = 0;
    int op = mask & EV_READ ? EPOLLIN : EPOLLOUT;
    ev_add_monitored(ctx, fd, mask, data);
    ret = epoll_add(e_api->fd, fd, op, &ctx->events_monitored[fd]);
    if (mask & EV_EVENTFD)
        (void) eventfd_write(fd, 1);
    return ret;
}

int ev_register_cron(struct ev_ctx *ctx, void (*callback)(struct ev_ctx *),
                     long long s, long long ns) {

    struct epoll_api *e_api = ctx->api;

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
    ctx->events_monitored[timerfd].fd = timerfd;
    ctx->events_monitored[timerfd].mask = EV_TIMERFD;
    ctx->events_monitored[timerfd].callback = callback;
    if (epoll_add(e_api->fd, timerfd, EPOLLIN,
                  &ctx->events_monitored[timerfd]) < 0)
        return -1;

    return timerfd;
}

int ev_fire_event(struct ev_ctx *ctx, int fd, int mask, void *data) {
    struct epoll_api *e_api = ctx->api;
    int ret = 0;
    int op = mask & EV_READ ? EPOLLIN : EPOLLOUT;
    ev_add_monitored(ctx, fd, mask, data);
    if (mask & EV_EVENTFD) {
        (void) epoll_add(e_api->fd, fd, op, &ctx->events_monitored[fd]);
        ret = eventfd_write(fd, 1);
    } else {
        ret = epoll_mod(e_api->fd, fd, op, &ctx->events_monitored[fd]);
    }
    return ret;
}

int ev_read_event(struct ev_ctx *ctx, int idx, int mask, void **ptr) {
    int err = -1;
    struct epoll_api *e_api = ctx->api;
    struct ev *e = e_api->events[idx].data.ptr;
    int fd = e->fd;
    if (!(mask & (EV_CLOSEFD | EV_TIMERFD)))
        *ptr = e->data;
    if (mask & EV_EVENTFD) {
        eventfd_read(fd, &(eventfd_t){0L});
        err = close(fd);
    } else if (mask & EV_TIMERFD) {
        (void) read(fd, &(long int){0L}, sizeof(long int));
        err = epoll_mod(e_api->fd, fd, EPOLLIN, e);
        e->callback(ctx);
    } else if  (mask & EV_CLOSEFD) {
        eventfd_read(fd, &(eventfd_t){0L});
        err = epoll_mod(e_api->fd, fd, EPOLLIN, e);
    }
    return err;
}

int ev_get_fd(struct ev_ctx *ctx, int idx) {
    struct epoll_api *e_api = ctx->api;
    return ((struct ev *) e_api->events[idx].data.ptr)->fd;
}
