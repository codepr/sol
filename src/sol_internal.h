/*
 * BSD 2-Clause License
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

#include "list.h"
#include "mqtt.h"
#include "network.h"
#include "pack.h"
#include "trie.h"
#include "uthash.h"
#include "util.h"
#include <stdatomic.h>
#include <time.h>

/* Generic return codes without a defined purpose */
#define SOL_OK            0
#define SOL_ERR           1

/*
 * Error codes for packet reception, signaling respectively
 * - client disconnection
 * - error reading packet
 * - error packet sent exceeds size defined by configuration (generally default
 *   to 2MB)
 * - error EAGAIN from a non-blocking read/write function
 * - error sending/receiving data on a connected socket
 * - error OUT OF MEMORY
 */
#define ERRCLIENTDC       1
#define ERRPACKETERR      2
#define ERRMAXREQSIZE     3
#define ERREAGAIN         4
#define ERRSOCKETERR      5
#define ERRNOMEM          6

/*
 * Return code of handler functions, signaling if there's data payload to be
 * sent out or if the server just need to re-arm closure for reading incoming
 * bytes
 */
#define REPLY             0
#define NOREPLY           1

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
    unsigned char *retained_msg;
    struct subscriber *subscribers; /* UTHASH handle pointer, must be NULL */
};

/*
 * Topic store keep track of all topics and wildcards registered, using a
 * trie as underlying data structure
 */
struct topic_store {
    // The main topics Trie structure
    Trie *topics;
    // A list of wildcards subscriptions, as it's not possible to know in
    // advance what topics will match some wildcard subscriptions
    List *wildcards;
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
    struct ref
        refcount; /* Reference counting struct, to share the struct easily */
};

/*
 * Utility struct to store wildcard subscriptions. Just wrap a subscriber
 * paired with a topic name and a flag to indicate if it's a '#' multilevel
 * subscription or not.
 */
struct subscription {
    bool multilevel;               /* Flag for '#' subscriptions */
    const char *topic;             /* Topic name the subscription refers to */
    struct subscriber *subscriber; /* Reference to the subscriber */
};

/*
 * Pending messages remaining to be sent out, they can be either PUBLISH or
 * generic ACKs, fields required are the descriptor of destination, the type
 * of the message, the timestamp of the last send try, the size of the packet
 * and the packet himself.
 * It's meant to be used in a fixed length array.
 */
struct inflight_msg {
    time_t seen; /* Timestamp of the last time we have seen this msg */
    struct mqtt_packet
        *packet;       /* The payload to be written out in case of timeout */
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
    struct ev_ctx
        *ctx;   /* An event context refrence mostly used to fire write events */
    int rc;     /* Return code of the message just handled */
    int status; /* Current status of the client (state machine) */
    volatile atomic_int rpos;    /* The nr of bytes to skip after a complete
                                  * packet has * been read. This because according
                                  * to MQTT, length is encoded on multiple bytes
                                  * according to it's size, using continuation bit
                                  * as a technique to encode it. We don't want to
                                  * decode the length two times when we already
                                  * know it, so we need an offset to know where
                                  * the actual packet will start
                                  */
    volatile atomic_size_t read; /* The number of bytes already read */
    volatile atomic_size_t
        toread;                   /* The number of bytes that have to be read */
    unsigned char *rbuf;          /* The reading buffer */
    volatile atomic_size_t wrote; /* The number of bytes already written */
    volatile atomic_size_t towrite; /* The number of bytes we have to write */
    unsigned char *wbuf;            /* The writing buffer */
    char client_id[MQTT_CLIENT_ID_LEN]; /* The client ID according to MQTT specs
                                         */
    struct connection conn; /* A connection structure, takes care of plain or
                             * TLS encrypted communication by using callbacks
                             */
    struct client_session *session; /* The session associated to the client */
    time_t last_seen; /* The timestamp of the last action performed */
    bool online;      /* Just an online flag */
    bool connected;   /* States if the client has already processed a connection
                         packet */
    bool has_lwt; /* States if the connection packet carried a LWT message */
    bool clean_session;    /* States if the connection packet was set to clean
                              session */
    pthread_mutex_t mutex; /* Inner lock for the client, this avoid
                              race-conditions on shared parts */
    UT_hash_handle hh;     /* UTHASH handle, needed to use UTHASH macros */
};

/*
 * Every client has a session which track his subscriptions, possible missed
 * messages during disconnection time (that iff clean_session is set to false),
 * inflight messages and the message ID for each one.
 * A maximum of 65535 mid can be used at the same time according to MQTT specs,
 * so i_acks, i_msgs, thus being allocated on the heap during the init, will be
 * of 65535 length each.
 *
 * It's a hashable struct that will be tracked during the entire lifetime of
 * the application, governed by the clean_session flag on connection from
 * clients
 */
struct client_session {
    unsigned next_free_mid; /* The next 'free' message ID */
    List *subscriptions;    /* All the clients subscriptions, stored as topic
                               structs */
    List *outgoing_msgs; /* Outgoing messages during disconnection time, stored
                            as mqtt_packet pointers */
    volatile atomic_ushort inflights; /* Just a counter stating the presence of
                                         inflight messages */
    bool clean_session;               /* Clean session flag */
    char session_id[MQTT_CLIENT_ID_LEN]; /* The client_id the session refers to
                                          */
    struct mqtt_packet
        lwt_msg;    /* A possibly NULL LWT message, will be set on connection */
    time_t *i_acks; /* Inflight ACKs that must be cleared */
    struct inflight_msg *
        i_msgs; /* Inflight MSGs that must be sent out DUP in case of timeout */
    UT_hash_handle hh; /* UTHASH handle, needed to use UTHASH macros */
    struct ref
        refcount; /* Reference counting struct, to share the struct easily */
};

/*
 * Simple mutex for contexted critical areas, mainly used in the handlers
 * module, in server the only useful use are when creating and deactivating
 * clients
 */
extern pthread_mutex_t mutex;

struct server;

/*
 * Checks if a client is subscribed to a topic by trying to fetch the
 * client_session by its ID on the subscribers inner hashmap of the topic.
 */
bool is_subscribed(const struct topic *, const struct client_session *);

/*
 * Allocate memory on the heap to create and return a pointer to a struct
 * subscriber, assigining the passed in QoS, session pointer, and
 * instantiating a reference counter to 0.
 * It may fail as it needs to allocate some bytes on the heap.
 */
struct subscriber *subscriber_new(struct client_session *, unsigned char);

/*
 * Allocate memory on the heap to clone a subscriber pointer, deep copies all
 * fields into the newly allocated pointer except for the reference counter,
 * the new pointer will have its own refcount set to 0. Finally the newly
 * allocated pointer is returned.
 * It may fail as it needs to allocate some bytes on the heap.
 */
struct subscriber *subscriber_clone(const struct subscriber *);

/*
 * Initialize a struct topic pointer by setting its name, subscribers and
 * retained_msg are set to NULL.
 * The function expects a non-null pointer and can't fail, if a null topic
 * is passed, the function return prematurely.
 */
void topic_init(struct topic *, const char *);

/*
 * Allocate a new topic struct on the heap, initialize it then return a pointer
 * to it. The function can fail as a memory allocation is requested, if it
 * fails the program execution graceful crash.
 */
struct topic *topic_new(const char *);

/*
 * Deallocate the topic name, retained_msg and all its subscribers
 */
void topic_destroy(struct topic *);

/*
 * Allocate a new subscriber struct on the heap referring to the passed in
 * topic, client_session and QoS, then add it to the topic map.
 * The function can fail as a memory allocation is requested, if it fails the
 * program execution graceful crash.
 */
struct subscriber *topic_add_subscriber(struct topic *, struct client_session *,
                                        unsigned char);

/*
 * Remove a subscriber from the topic, the subscriber to be removed refers to
 * the client_id belonging to the client pointer passed in.
 * The subscriber deletion is really a reference count subtraction, DECREF
 * macro takes care of the counter, if it reaches 0 it de-allocates the memory
 * reserved to the struct subscriber.
 * The function can't fail.
 */
void topic_del_subscriber(struct topic *, struct client *);

/*
 * Allocate a new store structure on the heap and return it after its
 * initialization, also allocating a new list on the heap to keep track of
 * wildcard topics.
 * The function may gracefully crash as the memory allocation may fail.
 */
struct topic_store *topic_store_new(void);

/*
 * Deallocate heap memory for the list and every wildcard item stored into,
 * also the store is deallocated
 */
void topic_store_destroy(struct topic_store *);

/*
 * Return a topic associated to a topic name from the store, returns NULL if no
 * topic is found.
 */
struct topic *topic_store_get(const struct topic_store *, const char *);

/*
 * Return a topic associated to a topic name from the store, if no topic is
 * insert it into the store before returning it. Like topic_store_get but
 * cannot return NULL.
 * The function may fail as in case of no topic found it tries to allocate
 * space on the heap for the new inserted topic.
 */
struct topic *topic_store_get_or_put(struct topic_store *, const char *);

/*
 * Check if the store contains a topic by name key
 */
bool topic_store_contains(const struct topic_store *, const char *);

/*
 * Insert a topic into the store or update it if already present
 */
void topic_store_put(struct topic_store *, struct topic *);

/*
 * Remove a topic into the store
 */
void topic_store_del(struct topic_store *, const char *);

/*
 * Add a wildcard topic to the topic_store struct, does not check if it already
 * exists
 */
void topic_store_add_wildcard(struct topic_store *, struct subscription *);

/*
 * Remove a wildcard by id key from the topic_store struct
 */
void topic_store_remove_wildcard(struct topic_store *, char *);

/*
 * Run a function to each node of the topic_store trie holding the topic
 * entries
 */
void topic_store_map(struct topic_store *, const char *,
                     void (*fn)(struct trie_node *, void *), void *);

/*
 * Check if the wildcards list of the topic_store is empty
 */
bool topic_store_wildcards_empty(const struct topic_store *);

#define topic_store_wildcards_foreach(item, store)                             \
    list_foreach(item, store->wildcards)

#define has_inflight(session)   ((session)->inflights > 0)

#define inflight_msg_clear(msg) DECREF((msg)->packet, struct mqtt_packet)
