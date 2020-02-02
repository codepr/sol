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

/*
 * An MQTT topic is composed by a name which identify it, a retained message
 * which must be forwarded to all subscribing clients and a map of subscribers,
 * the handle is a struct subscriber pointer which have to be initialized at
 * NULL.
 *
 * See https://troydhanson.github.io/uthash/userguide.html for more info
 */
struct topic {
    const char *name;
    bstring retained_msg;
    struct subscriber *subscribers; /* UTHASH handle pointer, must be NULL */
};

/*
 * An MQTT subscriber wraps a client session and is composed by a granted QoS
 * which is the QoS given by the server for each topic it's subscribed, an ID
 * which is the same of the client it refers to and two utility members to
 * handle it's sharing between structures.
 *
 * It's hashable according to UTHASH APIs. For more info check
 * https://troydhanson.github.io/uthash/userguide.html
 */
struct subscriber {
    struct client_session *session; /* Session referring to a client */
    unsigned char granted_qos; /* The QoS given by the server for each topic */
    char id[MQTT_CLIENT_ID_LEN]; /* Client ID key */
    UT_hash_handle hh; /* UTHASH handle, needed to use UTHASH macros */
    struct ref refcount; /* Reference counting struct, to share the struct easily */
};

/*
 * Utility struct to store wildcard subscriptions. Just wrap a subscriber
 * paired with a topic name and a flag to indicate if it's a '#' multilevel
 * subscription or not.
 */
struct subscription {
    bool multilevel; /* Flag for '#' subscriptions */
    const char *topic; /* Topic name the subscription refers to */
    struct subscriber *subscriber; /* Reference to the subscriber */
};

/*
 * Pending messages remaining to be sent out, they can be either PUBLISH or
 * generic ACKs, fields required are the descriptor of destination, the type
 * of the message, the timestamp of the last send try, the size of the packet
 * and the packet himself.
 * It's meant to be used in a fixed length array, that's why we add an 'in_use'
 * flag.
 */
struct inflight_msg {
    int in_use; /* Just a flag stating the use of the inflight_msg struct */
    time_t seen; /* Timestamp of the last time we have seen this msg */
    size_t size; /* The size of the message at the time of the store */
    struct client *client; /* Client reference to write the payload to */
    struct mqtt_packet *packet; /* The payload to be written out in case of timeout */
    unsigned char qos; /* The QoS at the time of the publish */
};

/*
 * The client actions can be summarized as a roughly simple state machine,
 * comprised by 4 states:
 * - WAITING_HEADER it's the base state, waiting for the next packet to be
 *                  received
 * - WAITING_LENGTH the second state, a packet has arrived but it's not
 *                  complete yet. Accorting to MQTT protocol, after the first
 *                  byte we need to wait 1 to 4 more bytes based on the
 *                  encoded length (use continuation bit to state the number
 *                  of bytes needed, see http://docs.oasis-open.org/mqtt/mqtt/
 *                  v3.1.1/os/mqtt-v3.1.1-os.html for more info)
 * - WAITING_DATA   it's the step required to receive the full byte stream as
 *                  the encoded length describe. We wait for the effective
 *                  payload in this state.
 * - SENDING_DATA   the last status, a complete packet has been received and
 *                  has to be processed and reply back if needed.
 */
enum client_status {
    WAITING_HEADER,
    WAITING_LENGTH,
    WAITING_DATA,
    SENDING_DATA
};

/*
 * Wrapper structure around a connected client, each client can be a publisher
 * or a subscriber, it can be used to track sessions too.
 * As of now, no allocations will be fired, jsut a big pool of memory at the
 * start of the application will serve us a client pool, read and write buffers
 * are initialized lazily.
 *
 * It's an hashable struct which will be tracked during the execution of the
 * application, see https://troydhanson.github.io/uthash/userguide.html.
 */
struct client {
    int rc;  /* Return code of the message just handled */
    int status; /* Current status of the client (state machine) */
    int rpos; /* The nr of bytes to skip after a complete packet has been read.
               * This because according to MQTT, length is encoded on multiple
               * bytes according to it's size, using continuation bit as a
               * technique to encode it. We don't want to decode the length two
               * times when we already know it, so we need an offset to know
               * where the actual packet will start
               */
    size_t read; /* The number of bytes already read */
    size_t toread; /* The number of bytes that have to be read */
    unsigned char *rbuf; /* The reading buffer */
    size_t wrote; /* The number of bytes already written */
    size_t towrite; /* The number of bytes we have to write */
    unsigned char *wbuf; /* The writing buffer */
    char client_id[MQTT_CLIENT_ID_LEN]; /* The client ID according to MQTT specs */
    struct connection conn; /* A connection structure, takes care of plain or
                             * TLS encrypted communication by using callbacks
                             */
    struct client_session *session; /* The session associated to the client */
    unsigned long last_seen; /* The timestamp of the last action performed */
    bool online;  /* Just an online flag */
    bool connected; /* States if the client has already processed a connection packet */
    bool has_lwt; /* States if the connection packet carried a LWT message */
    bool clean_session; /* States if the connection packet was set to clean session */
    UT_hash_handle hh; /* UTHASH handle, needed to use UTHASH macros */
};

/*
 * Every client has a session which track his subscriptions, possible missed
 * messages during disconnection time (that iff clean_session is set to false),
 * inflight messages and the message ID for each one.
 * A maximum of 65535 mid can be used at the same time according to MQTT specs,
 * so i_acks, i_msgs and in_i_acks, thus being allocated on the heap during the
 * init, will be of 65535 length each.
 *
 * It's a hashable struct that will be tracked during the entire lifetime of
 * the application, governed by the clean_session flag on connection from
 * clients
 */
struct client_session {
    int next_free_mid; /* The next 'free' message ID */
    List *subscriptions; /* All the clients subscriptions */
    List *outgoing_msgs; /* Outgoing messages during disconnection time */
    bool has_inflight; /* Just a flag stating the presence of inflight messages */
    bool clean_session; /* Clean session flag */
    char session_id[MQTT_CLIENT_ID_LEN]; /* The client_id the session refers to */
    struct mqtt_packet lwt_msg; /* A possibly NULL LWT message, will be set on connection */
    struct inflight_msg *i_acks; /* Inflight ACKs that must be cleared */
    struct inflight_msg *i_msgs; /* Inflight MSGs that must be sent out DUP in case of timeout */
    struct inflight_msg *in_i_acks; /* Inflight input ACKs that must be cleared by the client */
    UT_hash_handle hh; /* UTHASH handle, needed to use UTHASH macros */
    struct ref refcount; /* Reference counting struct, to share the struct easily */
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
