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

#ifndef SERVER_H
#define SERVER_H

#include "mqtt.h"
#include "pack.h"
#include "trie.h"
#include "network.h"

/*
 * Number of worker threads to be created. Each one will host his own ev_ctx
 * loop. This doesn't take into account the main thread, so to know the total
 * number of running loops +1 must be added to the THREADSNR value.
 */
#define THREADSNR 2

/*
 * Epoll default settings for concurrent events monitored and timeout, -1
 * means no timeout at all, blocking undefinitely
 */
#define EVENTLOOP_MAX_EVENTS    1024
#define EVENTLOOP_TIMEOUT       -1

/* Initial memory allocation for clients on server start-up, it should be
 * equal to ~40 MB, read and write buffers are initialized lazily
 */
#define BASE_CLIENTS_NUM  1024 * 128

/*
 * IO event strucuture, it's the main information that will be communicated
 * between threads, every request packet will be wrapped into an IO event and
 * passed to the work EPOLL, in order to be handled by the worker thread pool.
 * Then finally, after the execution of the command, it will be updated and
 * passed back to the IO epoll loop to be written back to the requesting client
 */
struct io_event {
    struct client *client;
    struct mqtt_packet data;
};

/* Global informations statistics structure */
struct sol_info {
    /* Number of clients currently connected */
    atomic_size_t nclients;
    /* Total number of clients connected since the start */
    atomic_size_t nconnections;
    /* Total number of sent messages */
    atomic_size_t messages_sent;
    /* Total number of received messages */
    atomic_size_t messages_recv;
    /* Timestamp of the start time */
    atomic_size_t start_time;
    /* Seconds passed since the start */
    atomic_size_t uptime;
    /* Total number of requests served */
    atomic_size_t nrequests;
    /* Total number of bytes received */
    atomic_size_t bytes_sent;
    /* Total number of bytes sent out */
    atomic_size_t bytes_recv;
};

#define INIT_INFO do { \
    info.nclients = ATOMIC_VAR_INIT(0);         \
    info.nconnections = ATOMIC_VAR_INIT(0);     \
    info.messages_sent = ATOMIC_VAR_INIT(0);    \
    info.messages_recv = ATOMIC_VAR_INIT(0);    \
    info.start_time = ATOMIC_VAR_INIT(0);       \
    info.uptime = ATOMIC_VAR_INIT(0);           \
    info.nrequests = ATOMIC_VAR_INIT(0);        \
    info.bytes_sent = ATOMIC_VAR_INIT(0);       \
    info.bytes_recv = ATOMIC_VAR_INIT(0);       \
} while (0)

/*
 * General informations of the broker, all fields will be published
 * periodically to internal topics
 */
extern struct sol_info info;

/*
 * Main structure, a global instance will be instantiated at start, tracking
 * topics, connected clients and registered closures.
 *
 * pending_msgs and pendings_acks are two arrays used to track remaining
 * messages to push out and acks respectively.
 */
struct server {
    // The main topics store
    struct topic_store *store;
    // A memory pool for clients allocation
    struct memorypool *pool;
    // Our clients map, it's a handle pointer for UTHASH APIs, must be set to
    // NULL
    struct client *clients_map;
    // The global session map, another UTHASH handle pointer, must be set to
    // NULL
    struct client_session *sessions;
    // UTHASH handle pointer for authentications
    struct authentication *authentications;
    // Application TLS context
    SSL_CTX *ssl_ctx;
};

extern struct server server;

int start_server(const char *, const char *);
void enqueue_event_write(const struct client *);
void daemonize(void);

#endif
