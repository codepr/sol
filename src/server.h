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
#include <openssl/ssl.h>
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
    unsigned int nclients;
    /* Total number of clients connected since the start */
    unsigned long long nconnections;
    /* Total number of sent messages */
    unsigned long long messages_sent;
    /* Total number of received messages */
    unsigned long long messages_recv;
    /* Timestamp of the start time */
    unsigned long long start_time;
    /* Seconds passed since the start */
    unsigned long long uptime;
    /* Total number of requests served */
    unsigned long long nrequests;
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
 * Main structure, a global instance will be instantiated at start, tracking
 * topics, connected clients and registered closures.
 *
 * pending_msgs and pendings_acks are two arrays used to track remaining
 * messages to push out and acks respectively.
 */
struct server {
    Trie topics; /* The main topics Trie structure */
    struct memorypool *pool; /* A memory pool for clients allocation */
    struct client *clients_map; /* Our clients map, it's a handle pointer for
                                 * UTHASH APIs, must be set to NULL
                                 */
    struct client_session *sessions; /* The global session map, another UTHASH
                                      * handle pointer, must be set to NULL
                                      */
    struct authentication *authentications; /* UTHASH handle pointer for
                                             * authentications
                                             */
    List *wildcards; /* A list of wildcards subscriptions, as it's not
                      * possible to know in advance what topics will match some
                      * wildcard subscriptions
                      */
    SSL_CTX *ssl_ctx; /* Application TLS context */
};

extern struct server server;

int start_server(const char *, const char *);
void enqueue_event_write(const struct client *);
void daemonize(void);

#endif
