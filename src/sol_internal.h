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

#include <time.h>
#include "util.h"
#include "pack.h"
#include "list.h"
#include "mqtt.h"
#include "uthash.h"
#include "network.h"

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
#define ERRNOMEM            5

/*
 * Return code of handler functions, signaling if there's data payload to be
 * sent out or if the server just need to re-arm closure for reading incoming
 * bytes
 */
#define REPLY               0
#define NOREPLY             1

/* The maximum number of pending/not acknowledged packets for each client */
#define MAX_INFLIGHT_MSGS 65536

struct topic {
    const char *name;
    bstring retained_msg;
    struct subscriber *subscribers;
};

struct subscriber {
    struct client_session *session;
    unsigned char granted_qos;
    char id[MQTT_CLIENT_ID_LEN];
    UT_hash_handle hh;
    struct ref refcount;
};

struct subscription {
    bool multilevel;
    const char *topic;
    struct subscriber *subscriber;
};

/*
 * Pending messages remaining to be sent out, they can be either PUBLISH or
 * generic ACKs, fields required are the descriptor of destination, the type
 * of the message, the timestamp of the last send try, the size of the packet
 * and the packet himself
 */
struct inflight_msg {
    int in_use;
    time_t seen;
    size_t size;
    struct client *client;
    struct mqtt_packet *packet;
    unsigned char qos;
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
    int poolnr;
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
    bool online;  // just a boolean will be fine for now
    bool connected;
    bool has_lwt;
    bool clean_session;
    UT_hash_handle hh;
};

struct client_session {
    int next_free_mid;
    List *subscriptions;
    List *outgoing_msgs;
    bool has_inflight;
    bool clean_session;
    char session_id[MQTT_CLIENT_ID_LEN];
    struct mqtt_packet lwt_msg;
    struct inflight_msg *i_acks;
    struct inflight_msg *i_msgs;
    struct inflight_msg *in_i_acks;
    UT_hash_handle hh;
    struct ref refcount;
};

struct server;

void inflight_msg_init(struct inflight_msg *, struct client *,
                       struct mqtt_packet *, size_t);
void inflight_msg_clear(struct inflight_msg *);
bool is_subscribed(const struct topic *, const struct client_session *);
struct subscriber *subscriber_new(struct topic *,
                                  struct client_session *, unsigned char);
struct subscriber *subscriber_clone(const struct subscriber *);
struct subscriber *topic_add_subscriber(struct topic *,
                                        struct client_session *, unsigned char);
void topic_del_subscriber(struct topic *, struct client *);
bool topic_exists(const struct server *, const char *);
void topic_put(struct server *, struct topic *);
void topic_del(struct server *, const char *);
/* Find a topic by name and return it */
struct topic *topic_get(const struct server *, const char *);
/* Get or create a new topic if it doesn't exists */
struct topic *topic_get_or_create(struct server *, const char *);
unsigned next_free_mid(struct client_session *);
void session_init(struct client_session *, char *);
struct client_session *client_session_alloc(char *);
