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
#include "hashtable.h"

/*
 * Epoll default settings for concurrent events monitored and timeout, -1
 * means no timeout at all, blocking undefinitely
 */
#define EVENTLOOP_MAX_EVENTS    1024
#define EVENTLOOP_TIMEOUT       -1

/*
 * Error codes for packet reception, signaling respectively
 * - client disconnection
 * - error reading packet
 * - error packet sent exceeds size defined by configuration (generally default
 *   to 2MB)
 * - error EAGAIN from a non-blocking read/write function
 */
#define ERRCLIENTDC         1
#define ERRPACKETERR        2
#define ERRMAXREQSIZE       3
#define ERREAGAIN           4

/*
 * Return code of handler functions, signaling if there's data payload to be
 * sent out or if the server just need to re-arm closure for reading incoming
 * bytes
 */
#define REPLY               0
#define NOREPLY             1

/* The maximum number of pensing/not acknowledged packets for each client */
#define MAX_INFLIGHT_MSGS 65536

/* Initial memory allocation for clients on server start-up */
#define BASE_CLIENTS_NUM  1024

/*
 * IO event strucuture, it's the main information that will be communicated
 * between threads, every request packet will be wrapped into an IO event and
 * passed to the work EPOLL, in order to be handled by the worker thread pool.
 * Then finally, after the execution of the command, it will be updated and
 * passed back to the IO epoll loop to be written back to the requesting client
 */
struct io_event {
    struct ev_ctx *ctx;
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
    int maxfd;
    struct client *clients;
    struct memorypool *refpool;
    struct memorypool *mqttpool;
    struct memorypool *sessionpool;
    Trie topics;
    HashTable *sessions;
    HashTable *authentications;
    SSL_CTX *ssl_ctx;
};

extern struct server server;

/*
 * Pending messages remaining to be sent out, they can be either PUBLISH or
 * generic ACKs, fields required are the descriptor of destination, the type
 * of the message, the timestamp of the last send try, the size of the packet
 * and the packet himself
 */
struct inflight_msg {
    struct client *client;
    int in_use;
    time_t sent_timestamp;
    size_t size;
    struct refobj *refobj;
    // struct mqtt_packet *packet;
};

struct topic {
    const char *name;
    bstring retained_msg;
    HashTable *subscribers;
};

struct subscriber {
    unsigned qos;
    struct client *client;
    unsigned refs;
};

enum client_status {
    WAITING_HEADER,
    WAITING_LENGTH,
    WAITING_DATA,
    SENDING_DATA
};

/*
 * Wrapper structure around a connected client, each client can be a publisher
 * or a subscriber, it can be used to track sessions too.
 */
struct client {
    bool online;  // just a boolean will be fine for now
    bool has_lwt;
    bool clean_session;
    int rc;
    int status;
    int rpos;
    size_t read;
    size_t toread;
    unsigned char *rbuf;
    size_t wrote;
    size_t towrite;
    unsigned char *wbuf;
    char client_id[MQTT_CLIENT_ID_LEN];
    struct connection conn;
    struct client_session *session;
    unsigned long last_seen;
};

struct client_session {
    bool has_inflight;
    int next_free_mid;
    List *subscriptions;
    List *outgoing_msgs;
    struct mqtt_packet lwt_msg;
    struct inflight_msg *i_acks;
    struct inflight_msg *i_msgs;
    struct inflight_msg *in_i_acks;
};

void inflight_msg_init(struct inflight_msg *, struct client *,
                       struct refobj *, size_t);

void inflight_msg_clear(struct inflight_msg *);

void topic_add_subscriber(struct topic *, struct client *, unsigned);

void topic_del_subscriber(struct topic *, struct client *);

bool topic_exists(const struct server *, const char *);

void topic_put(struct server *, struct topic *);

void topic_del(struct server *, const char *);

/* Find a topic by name and return it */
struct topic *topic_get(const struct server *, const char *);

/* Get or create a new topic if it doesn't exists */
struct topic *topic_get_or_create(struct server *, const char *);

unsigned next_free_mid(struct client *);

void session_init(struct client_session *);

int start_server(const char *, const char *);

void enqueue_event_write(struct ev_ctx *, struct client *);

void daemonize(void);

#endif
