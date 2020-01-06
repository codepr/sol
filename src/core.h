/* BSD 2-Clause License
 *
 * Copyright (c) 2019, Andrea Giacomo Baldan
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice, this
 *   list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef CORE_H
#define CORE_H

#include <openssl/ssl.h>
#include <arpa/inet.h>
#include "trie.h"
#include "list.h"
#include "mqtt.h"
#include "network.h"
#include "hashtable.h"
#include "pack.h"

#define MAX_INFLIGHT_MSGS 65536
#define BASE_CLIENTS_NUM  1024

/*
 * Pending messages remaining to be sent out, they can be either PUBLISH or
 * generic ACKs, fields required are the descriptor of destination, the type
 * of the message, the timestamp of the last send try, the size of the packet
 * and the packet himself
 */
struct inflight_msg {
    struct client *client;
    int type;
    time_t sent_timestamp;
    unsigned long size;
    struct mqtt_packet *packet;
};

struct topic {
    const char *name;
    bstring retained_msg;
    HashTable *subscribers;
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
    bool clean_session;
    int rc;
    int status;
    int rpos;
    int read;
    int toread;
    unsigned char *rbuf;
    int wrote;
    int towrite;
    unsigned char *wbuf;
    char client_id[MQTT_CLIENT_ID_LEN];
    struct connection conn;
    struct session *session;
    unsigned long last_action_time;
    struct mqtt_publish *lwt_msg;
    struct inflight_msg *i_acks[MAX_INFLIGHT_MSGS];
    struct inflight_msg *i_msgs[MAX_INFLIGHT_MSGS];
    struct inflight_msg *in_i_acks[MAX_INFLIGHT_MSGS];
};

/*
 * Main structure, a global instance will be instantiated at start, tracking
 * topics, connected clients and registered closures.
 *
 * pending_msgs and pendings_acks are two arrays used to track remaining
 * messages to push out and acks respectively.
 */
struct sol {
    int maxfd;
    struct client *clients;
    Trie topics;
    HashTable *sessions;
    HashTable *authentications;
    SSL_CTX *ssl_ctx;
};

extern struct sol sol;

struct session {
    List *subscriptions;
    size_t msg_queue_size;
    size_t msg_queue_next;
    struct inflight_msg **msg_queue;
};

struct subscriber {
    unsigned qos;
    struct client *client;
    unsigned refs;
};

void client_init(struct client *);

struct client *sol_client_new(struct connection *);

struct inflight_msg *inflight_msg_new(struct client *,
                                      struct mqtt_packet *, int, size_t);

struct topic *topic_new(const char *);

void topic_init(struct topic *, const char *);

void topic_add_subscriber(struct topic *, struct client *, unsigned, bool);

void topic_del_subscriber(struct topic *, struct client *, bool);

bool sol_topic_exists(struct sol *, const char *);

void sol_topic_put(struct sol *, struct topic *);

void sol_topic_del(struct sol *, const char *);

struct session *sol_session_new(void);

/* Find a topic by name and return it */
struct topic *sol_topic_get(struct sol *, const char *);

/* Get or create a new topic if it doesn't exists */
struct topic *sol_topic_get_or_create(struct sol *, const char *);

unsigned next_free_mid(struct inflight_msg **);

void sol_session_append_imsg(struct session *, struct inflight_msg *);

#endif
