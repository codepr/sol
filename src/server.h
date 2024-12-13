/* BSD 2-Clause License
 *
 * Copyright (c) 2023, Andrea Giacomo Baldan All rights reserved.
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
#include <openssl/ssl.h>

/*
 * Epoll default settings for concurrent events monitored and timeout, -1
 * means no timeout at all, blocking undefinitely
 */
#define EVENTLOOP_MAX_EVENTS 1024
#define EVENTLOOP_TIMEOUT    -1

/* Initial memory allocation for clients on server start-up, it should be
 * equal to ~40 MB, read and write buffers are initialized lazily
 */
#define BASE_CLIENTS_NUM     1024 * 128

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
    size_t active_connections;
    /* Total number of clients connected since the start */
    size_t total_connections;
    /* Total number of sent messages */
    size_t messages_sent;
    /* Total number of received messages */
    size_t messages_recv;
    /* Timestamp of the start time */
    size_t start_time;
    /* Seconds passed since the start */
    size_t uptime;
    /* Total number of bytes received */
    size_t bytes_sent;
    /* Total number of bytes sent out */
    size_t bytes_recv;
};

#define INIT_INFO                                                              \
    do {                                                                       \
        info.active_connections = 0;                                           \
        info.total_connections  = 0;                                           \
        info.messages_sent      = 0;                                           \
        info.messages_recv      = 0;                                           \
        info.start_time         = 0;                                           \
        info.uptime             = 0;                                           \
        info.bytes_sent         = 0;                                           \
        info.bytes_recv         = 0;                                           \
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
    struct authentication *auths;
    // Application TLS context
    SSL_CTX *ssl_ctx;
};

extern struct server server;

/*
 * Main entry point for the server, to be called with an address and a port
 * to start listening. The function may fail only in the case of Out of memory
 * error occurs or listen call fails, on the other cases it should just log
 * unexpected errors.
 */
int start_server(const char *, const char *);

/*
 * Fire a write callback to reply after a client request, under the hood it
 * schedules an EV_WRITE event with a client pointer set to write carried
 * contents out on the socket descriptor.
 */
void enqueue_event_write(const struct client *);

/*
 * Make the entire process a daemon running in background
 */
void daemonize(void);

#endif
