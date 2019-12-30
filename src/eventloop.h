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

struct epoll_api {
    int fd;
    struct epoll_event *events;
};

void ev_init(struct ev_ctx *, int);

void ev_clone_ctx(struct ev_ctx *, int, int);

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

int ev_get_fd(struct ev_ctx *, int);

#endif
