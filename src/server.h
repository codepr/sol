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

#ifndef SERVER_H
#define SERVER_H

#include <pthread.h>
#include <sys/types.h>
#include <sys/eventfd.h>
#include "mqtt.h"
#include "pack.h"

/*
 * Epoll default settings for concurrent events monitored and timeout, -1
 * means no timeout at all, blocking undefinitely
 */
#define EPOLL_MAX_EVENTS    1024
#define EPOLL_TIMEOUT       -1

/* Error codes for packet reception, signaling respectively
 * - client disconnection
 * - error reading packet
 * - error packet sent exceeds size defined by configuration (generally default
 *   to 2MB)
 */
#define ERRCLIENTDC         1
#define ERRPACKETERR        2
#define ERRMAXREQSIZE       3

/* Return code of handler functions, signaling if there's data payload to be
 * sent out or if the server just need to re-arm closure for reading incoming
 * bytes
 */
#define REPLY               0
#define NOREPLY             1
#define CLIENTDC            2

/*
 * Number of I/O workers to start, in other words the size of the IO thread
 * pool
 */
#define IOPOOLSIZE 4

/* Number of Worker threads, or the size of the worker pool */
#define WORKERPOOLSIZE 4

/*
 * IO event strucuture, it's the main information that will be communicated
 * between threads, every request packet will be wrapped into an IO event and
 * passed to the work EPOLL, in order to be handled by the worker thread pool.
 * Then finally, after the execution of the command, it will be updated and
 * passed back to the IO epoll loop to be written back to the requesting client
 */
struct io_event {
    int epollfd;
    int rc;
    eventfd_t eventfd;
    bstring reply;
    struct client *client;
    union mqtt_packet data;
};

/* Global informations statistics structure */
struct sol_info {
    /* Number of clients currently connected */
    unsigned int nclients;
    /* Total number of clients connected since the start */
    unsigned int nconnections;
    /* Total number of sent messages */
    unsigned long long messages_sent;
    /* Total number of received messages */
    unsigned long long messages_recv;
    /* Timestamp of the start time */
    unsigned long long start_time;
    /* Seconds passed since the start */
    unsigned long long uptime;
    /* Total number of requests served */
    unsigned int nrequests;
    /* Total number of bytes received */
    unsigned long long bytes_sent;
    /* Total number of bytes sent out */
    unsigned long long bytes_recv;
};

/*
 * General informations of the broker, all fields will be published
 * periodically to internal topics
 */
extern struct sol_info info;

/*
 * Guards the access to the main database structure, the trie underlying the
 * topic DB and all the hashtable/lists involved
 */
extern pthread_spinlock_t w_spinlock;

/* Guards the EPOLL event changing between different threads */
extern pthread_spinlock_t io_spinlock;

int start_server(const char *, const char *);

#endif
