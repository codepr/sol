/*
 * BSD 2-Clause License
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

#ifndef NETWORK_H
#define NETWORK_H

#include <stdio.h>
#include <stdint.h>
#include "ringbuf.h"


// Socket families
#define UNIX    0
#define INET    1


struct evloop {
    int epollfd;
    int max_events;
    int timeout;
    int status;
    struct epoll_event *events;
} evloop;


typedef void callback_fn(struct evloop *, void *);


struct cb {
    int fd;
    void *args;
    struct bytestring *payload;
    callback_fn *callback;
};


/* Set non-blocking socket */
int set_nonblocking(int);

/*
 * Set TCP_NODELAY flag to true, disabling Nagle's algorithm, no more waiting
 * for incoming packets on the buffer
 */
int set_tcp_nodelay(int);

/* Auxiliary function for creating epoll server */
int create_and_bind(const char *, const char *, int);

/*
 * Create a non-blocking socket and make it listen on the specfied address and
 * port
 */
int make_listen(const char *, const char *, int);

/* Accept a connection and add it to the right epollfd */
int accept_connection(int);

/* Open a connection with a target host:port */
int open_connection(const char *, int);

/* Epoll management functions */
struct evloop *evloop_create(int, int);
void evloop_init(struct evloop *, int, int);
void evloop_free(struct evloop *);
int evloop_wait(struct evloop *);
void evloop_register_callback(struct evloop *, struct cb *);
void evloop_register_periodic_task(struct evloop *, int, struct cb *);
void evloop_unregister_callback(struct evloop *, int);
int add_epoll(int, int, int, void *);
int mod_epoll(int, int, int, void *);
int del_epoll(int, int);

/* I/O management functions */
int sendall(int, const unsigned char *, size_t, size_t *);
ssize_t recvall(int, Ringbuffer *, size_t);
ssize_t recvbytes(int, Ringbuffer *, size_t);


#endif
