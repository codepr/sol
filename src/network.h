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
#include "util.h"


// Socket families
#define UNIX    0
#define INET    1

/* Event loop wrapper structure, define an EPOLL loop and his status. The
 * EPOLL instance use EPOLLONESHOT for each event and must be re-armed
 * manually, in order to allow future uses on a multithreaded architecture.
 */
struct evloop {
    int epollfd;
    int max_events;
    int timeout;
    int status;
    struct epoll_event *events;
} evloop;


typedef void callback(struct evloop *, void *);

/*
 * Callback object, represents a callback function with an associated
 * descriptor if needed, args is a void pointer which can be a structure
 * pointing to callback parameters.
 * The last two fields are payload, a serialized version of the result of
 * a callback, ready to be sent through wire and a function pointer to the
 * callback function to execute.
 */
struct closure {
    int fd;
    void *obj;
    void *args;
    char closure_id[UUID_LEN];
    struct bytestring *payload;
    callback *call;
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

struct evloop *evloop_create(int, int);

void evloop_init(struct evloop *, int, int);

void evloop_free(struct evloop *);

/*
 * Blocks in a while(1) loop awaiting for events to be raised on monitored
 * file descriptors and executing the paired callback previously registered
 */
int evloop_wait(struct evloop *);

/*
 * Register a closure with a function to be executed every time the
 * paired descriptor is re-armed.
 */
void evloop_add_callback(struct evloop *, struct closure *);

/*
 * Register a periodic closure with a function to be executed every
 * defined interval of time.
 */
void evloop_add_periodic_task(struct evloop *, int, struct closure *);

/*
 * Unregister a closure by removing the associated descriptor from the
 * EPOLL loop
 */
int evloop_del_callback(struct evloop *, struct closure *);

/*
 * Rearm the file descriptor associated with a closure for read action,
 * making the event loop to monitor the callback for reading events
 */
int evloop_rearm_callback_read(struct evloop *, struct closure *);

/*
 * Rearm the file descriptor associated with a closure for write action,
 * making the event loop to monitor the callback for writing events
 */
int evloop_rearm_callback_write(struct evloop *, struct closure *);

/* Epoll management functions */
int epoll_add(int, int, int, void *);

/*
 * Modify an epoll-monitored descriptor, automatically set EPOLLONESHOT in
 * addition to the other flags, which can be EPOLLIN for read and EPOLLOUT for
 * write
 */
int epoll_mod(int, int, int, void *);

/*
 * Remove a descriptor from an epoll descriptor, making it no-longer monitored
 * for events
 */
int epoll_del(int, int);

/* I/O management functions */

/*
 * Send all data in a loop, avoiding interruption based on the kernel buffer
 * availability
 */
ssize_t send_bytes(int, const unsigned char *, size_t);

/*
 * Receive (read) an arbitrary number of bytes from a file descriptor and
 * store them in a buffer
 */
ssize_t recv_bytes(int, unsigned char *, size_t);


#endif
