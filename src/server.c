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

#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/eventfd.h>
#include <uuid/uuid.h>
#include "network.h"
#include "mqtt.h"
#include "util.h"
#include "pack.h"
#include "core.h"
#include "config.h"
#include "server.h"
#include "hashtable.h"

/* Seconds in a Sol, easter egg */
static const double SOL_SECONDS = 88775.24;

struct pending_message {
    int fd;
    int type;
    time_t sent_timestamp;
    unsigned long size;
    union mqtt_packet *packet;
};

/* Outgoing packet tracking for QoS > 0 */
static struct pending_message *outgoing[65535];

/*
 * General informations of the broker, all fields will be published
 * periodically to internal topics
 */
static struct sol_info info;

/* Broker global instance, contains the topic trie and the clients hashtable */
static struct sol sol;

/*
 * TCP server, based on I/O multiplexing but sharing I/O and work loads between
 * thread pools. The main thread have the exclusive responsibility of accepting
 * connections and pass them to IO threads.
 * From now on read and write operations for the connection will be handled by
 * a dedicated thread pool, which after every read will decode the bytearray
 * according to the protocol definition of each packet and finally pass the
 * resulting packet to the worker thread pool, where, according to the OPCODE
 * of the packet, the operation will be executed and the result will be
 * returned back to the IO thread that will write back to the client the
 * response packed into a bytestream.
 *
 *      MAIN              1...N              1...N
 *
 *     [EPOLL]         [IO EPOLL]         [WORK EPOLL]
 *  ACCEPT THREAD    IO THREAD POOL    WORKER THREAD POOL
 *  -------------    --------------    ------------------
 *        |                 |                  |
 *      ACCEPT              |                  |
 *        | --------------> |                  |
 *        |          READ AND DECODE           |
 *        |                 | ---------------> |
 *        |                 |                WORK
 *        |                 | <--------------- |
 *        |               WRITE                |
 *        |                 |                  |
 *      ACCEPT              |                  |
 *        | --------------> |                  |
 *
 * By tuning the number of IO threads and worker threads based on the number of
 * core of the host machine, it is possible to increase the number of served
 * concurrent requests per seconds.
 * The access to shared data strucures on the worker thread pool is guarded by
 * a spinlock, and being generally fast operations it shouldn't suffer high
 * contentions by the threads and thus being really fast.
 */


/*
 * Guards the access to the main database structure, the trie underlying the
 * DB
 */
static pthread_spinlock_t spinlock;

static void lock(void) {
    pthread_spin_lock(&spinlock);
}


static void unlock(void) {
    pthread_spin_unlock(&spinlock);
}

/*
 * IO event strucuture, it's the main information that will be communicated
 * between threads, every request packet will be wrapped into an IO event and
 * passed to the work EPOLL, in order to be handled by the worker thread pool.
 * Then finally, after the execution of the command, it will be updated and
 * passed back to the IO epoll loop to be written back to the requesting client
 */
struct io_event {
    int epollfd;
    eventfd_t eventfd;
    bstring reply;
    struct sol_client *client;
    union mqtt_packet *data;
};

/*
 * Shared epoll object, contains the IO epoll and Worker epoll descriptors,
 * as well as the server descriptor and the timer fd for repeated routines.
 * Each thread will receive a copy of a pointer to this structure, to have
 * access to all file descriptor running the application
 */
struct epoll {
    int io_epollfd;
    int w_epollfd;
    int serverfd;
    int timerfd[2];
    int busfd;
};

/*
 * Statistics topics, published every N seconds defined by configuration
 * interval
 */
#define SYS_TOPICS 14

static const char *sys_topics[SYS_TOPICS] = {
    "$SOL/",
    "$SOL/broker/",
    "$SOL/broker/clients/",
    "$SOL/broker/bytes/",
    "$SOL/broker/messages/",
    "$SOL/broker/uptime/",
    "$SOL/broker/uptime/sol",
    "$SOL/broker/clients/connected/",
    "$SOL/broker/clients/disconnected/",
    "$SOL/broker/bytes/sent/",
    "$SOL/broker/bytes/received/",
    "$SOL/broker/messages/sent/",
    "$SOL/broker/messages/received/",
    "$SOL/broker/memory/used"
};


static void publish_stats(void);

static void pending_message_check(void);

/* Prototype for a command handler */
typedef int handler(struct io_event *);

/* Command handler, each one have responsibility over a defined command packet */
static int connect_handler(struct io_event *);

static int disconnect_handler(struct io_event *);

static int subscribe_handler(struct io_event *);

static int unsubscribe_handler(struct io_event *);

static int publish_handler(struct io_event *);

static int puback_handler(struct io_event *);

static int pubrec_handler(struct io_event *);

static int pubrel_handler(struct io_event *);

static int pubcomp_handler(struct io_event *);

static int pingreq_handler(struct io_event *);

/* Command handler mapped usign their position paired with their type */
static handler *handlers[15] = {
    NULL,
    connect_handler,
    NULL,
    publish_handler,
    puback_handler,
    pubrec_handler,
    pubrel_handler,
    pubcomp_handler,
    subscribe_handler,
    NULL,
    unsubscribe_handler,
    NULL,
    pingreq_handler,
    NULL,
    disconnect_handler
};

/*
 * Command handlers
 */

static int connect_handler(struct io_event *e) {

#if WORKERPOOLSIZE > 1
    lock();
#endif

    if (!e->data->connect.payload.client_id
        && e->data->connect.bits.clean_session == false)
        goto clientdc;

    if (!e->data->connect.payload.client_id)
        e->data->connect.payload.client_id = sol_calloc(1, 1);

    // TODO just return error_code and handle it on `on_read`
    if (hashtable_exists(sol.clients,
                         (const char *) e->data->connect.payload.client_id)) {

        // Already connected client, 2 CONNECT packet should be interpreted as
        // a violation of the protocol, causing disconnection of the client

        sol_info("Received double CONNECT from %s, disconnecting client",
                 e->data->connect.payload.client_id);

        close(e->client->fd);
        hashtable_del(sol.clients,
                      (const char *) e->data->connect.payload.client_id);

        goto clientdc;
    }

    sol_info("New client connected as %s (c%i, k%u)",
             e->data->connect.payload.client_id,
             e->data->connect.bits.clean_session,
             e->data->connect.payload.keepalive);

    /*
     * Add the new connected client to the global map, if it is already
     * connected, kick him out accordingly to the MQTT v3.1.1 specs.
     */
    size_t cid_len = strlen((const char *) e->data->connect.payload.client_id);
    e->client->client_id = sol_malloc(cid_len + 1);
    memcpy(e->client->client_id,
           e->data->connect.payload.client_id, cid_len + 1);
    hashtable_put(sol.clients, e->client->client_id, e->client);

#if WORKERPOOLSIZE > 1
    unlock();
#endif

    /* Respond with a connack */
    union mqtt_packet *response = sol_malloc(sizeof(*response));

    // TODO check for session already present

    if (e->data->connect.bits.clean_session == false)
        e->client->session.subscriptions = list_new(NULL);

    unsigned char session_present = 0;
    unsigned char connect_flags = 0 | (session_present & 0x1) << 0;
    unsigned char rc = 0;  // 0 means connection accepted

    response->connack = *mqtt_packet_connack(CONNACK_B, connect_flags, rc);

    e->reply = bstring_copy(pack_mqtt_packet(response, 2), MQTT_ACK_LEN);

    sol_debug("Sending CONNACK to %s (%u, %u)",
              e->data->connect.payload.client_id,
              session_present, rc);

    sol_free(response);

    return REPLY;

clientdc:

    // Update stats
    info.nclients--;
    info.nconnections--;

#if WORKERPOOLSIZE > 1
        unlock();
#endif

    return CLIENTDC;
}


static void rec_sub(struct trie_node *node, void *arg) {
    if (!node || !node->data)
        return;
    struct topic *t = node->data;
    sol_debug("Adding subscriber to topic %s", t->name);
    t->subscribers = list_push(t->subscribers, arg);
}


static int disconnect_handler(struct io_event *e) {

    sol_debug("Received DISCONNECT from %s", e->client->client_id);

#if WORKERPOOLSIZE > 1
    lock();
#endif
    /* Handle disconnection request from client */
    close(e->client->fd);
    hashtable_del(sol.clients, e->client->client_id);

    // Update stats
    info.nclients--;
    info.nconnections--;
#if WORKERPOOLSIZE > 1
    unlock();
#endif
    // TODO remove from all topic where it subscribed
    return CLIENTDC;
}


static int subscribe_handler(struct io_event *e) {

    bool wildcard = false;
    bool alloced = false;

    /*
     * We respond to the subscription request with SUBACK and a list of QoS in
     * the same exact order of reception
     */
    unsigned char rcs[e->data->subscribe.tuples_len];

    struct sol_client *c = e->client;

    /* Subscribe packets contains a list of topics and QoS tuples */
    for (unsigned i = 0; i < e->data->subscribe.tuples_len; i++) {

        sol_debug("Received SUBSCRIBE from %s", c->client_id);

        /*
         * Check if the topic exists already or in case create it and store in
         * the global map
         */
        char *topic = (char *) e->data->subscribe.tuples[i].topic;

        sol_debug("\t%s (QoS %i)", topic, e->data->subscribe.tuples[i].qos);
#if WORKERPOOLSIZE > 1
        lock();
#endif
        /* Recursive subscribe to all children topics if the topic ends with "/#" */
        if (topic[e->data->subscribe.tuples[i].topic_len - 1] == '#' &&
            topic[e->data->subscribe.tuples[i].topic_len - 2] == '/') {
            topic = remove_occur(topic, '#');
            wildcard = true;
        } else if (topic[e->data->subscribe.tuples[i].topic_len - 1] != '/') {
            topic = append_string((char *) e->data->subscribe.tuples[i].topic, "/", 1);
            alloced = true;
        }

        struct topic *t = sol_topic_get_or_create(&sol, topic);

        if (wildcard == true) {
            struct subscriber *sub = sol_malloc(sizeof(*sub));
            sub->client = e->client;
            sub->qos = e->data->subscribe.tuples[i].qos;
            trie_prefix_map(sol.topics.root, topic, rec_sub, sub);
        }

        // Clean session true for now
        topic_add_subscriber(t, e->client,
                             e->data->subscribe.tuples[i].qos, true);
#if WORKERPOOLSIZE > 1
        unlock();
#endif
        if (alloced)
            sol_free(topic);

        rcs[i] = e->data->subscribe.tuples[i].qos;
    }

    struct mqtt_suback *suback =
        mqtt_packet_suback(SUBACK_B, e->data->subscribe.pkt_id,
                           rcs, e->data->subscribe.tuples_len);

    union mqtt_packet pkt = { .suback = *suback };
    unsigned char *packed = pack_mqtt_packet(&pkt, SUBACK);
    size_t len = MQTT_HEADER_LEN
        + sizeof(uint16_t)
        + e->data->subscribe.tuples_len;

    sol_debug("Sending SUBACK to %s", c->client_id);

    e->reply = bstring_copy(packed, len);

    return REPLY;
}


static int unsubscribe_handler(struct io_event *e) {

    struct sol_client *c = e->client;

    sol_debug("Received UNSUBSCRIBE from %s", c->client_id);

    e->data->ack = *mqtt_packet_ack(UNSUBACK_B, e->data->unsubscribe.pkt_id);

    unsigned char *packed = pack_mqtt_packet(e->data, UNSUBACK);

    sol_debug("Sending UNSUBACK to %s", c->client_id);

    e->reply = bstring_copy(packed, MQTT_ACK_LEN);

    return REPLY;
}


static int publish_handler(struct io_event *e) {

    int rc = NOREPLY;
    struct sol_client *c = e->client;

    sol_debug("Received PUBLISH from %s (d%i, q%u, r%i, m%u, %s, ... (%llu bytes))",
              c->client_id,
              e->data->publish.header.bits.dup,
              e->data->publish.header.bits.qos,
              e->data->publish.header.bits.retain,
              e->data->publish.pkt_id,
              e->data->publish.topic,
              e->data->publish.payloadlen);

#if WORKERPOOLSIZE > 1
    lock();
#endif

    info.messages_recv++;

    char *topic = (char *) e->data->publish.topic;
    bool alloced = false;
    unsigned char qos = e->data->publish.header.bits.qos;

    /*
     * For convenience we assure that all topics ends with a '/', indicating a
     * hierarchical level
     */
    if (topic[e->data->publish.topiclen - 1] != '/') {
        topic = append_string((char *) e->data->publish.topic, "/", 1);
        alloced = true;
    }

    /*
     * Retrieve the topic from the global map, if it wasn't created before,
     * create a new one with the name selected
     */
    struct topic *t = sol_topic_get_or_create(&sol, topic);

    // Not the best way to handle this
    if (alloced == true)
        sol_free(topic);

    if (t->subscribers->len > 0) {

        unsigned char *pub = pack_mqtt_packet(e->data, PUBLISH);

        size_t publen = MQTT_HEADER_LEN + sizeof(uint16_t) +
            e->data->publish.topiclen + e->data->publish.payloadlen;

        if (e->data->publish.header.bits.qos > AT_MOST_ONCE)
            publen += sizeof(uint16_t);

        bstring payload = bstring_copy(pub, publen);

        struct list_node *cur = t->subscribers->head;

        for (; cur; cur = cur->next) {

            struct subscriber *sub = cur->data;
            struct sol_client *sc = sub->client;

            /* Update QoS according to subscriber's one */
            e->data->publish.header.bits.qos = sub->qos;

            if (e->data->publish.header.bits.qos > AT_MOST_ONCE) {
                publen += sizeof(uint16_t);
                struct pending_message *pm = sol_malloc(sizeof(*pm));
                pm->fd = sc->fd;
                pm->sent_timestamp = time(NULL);
                pm->packet = e->data;
                pm->type = PUBLISH;
                pm->size = publen;
                outgoing[e->data->publish.pkt_id] = pm;
            }

            ssize_t sent;
            if ((sent = send_bytes(sc->fd, payload, bstring_len(payload))) < 0)
                sol_error("Error publishing to %s: %s",
                          sc->client_id, strerror(errno));

            info.messages_sent++;

            // Update information stats
            info.bytes_sent += sent;

            sol_debug("Sending PUBLISH to %s (d%i, q%u, r%i, m%u, %s, ... (%i bytes))",
                      sc->client_id,
                      e->data->publish.header.bits.dup,
                      e->data->publish.header.bits.qos,
                      e->data->publish.header.bits.retain,
                      e->data->publish.pkt_id,
                      e->data->publish.topic,
                      e->data->publish.payloadlen);
        }
    }

    if (qos == AT_MOST_ONCE)
        goto exit;

    int ptype = -1;

    if (qos == AT_LEAST_ONCE) {
        sol_debug("Sending PUBACK to %s", c->client_id);
        e->data->ack = *mqtt_packet_ack(PUBACK_B, e->data->publish.pkt_id);
        ptype = PUBACK;
    } else if (qos == EXACTLY_ONCE) {
        // TODO add to a hashtable to track PUBREC clients last
        sol_debug("Sending PUBREC to %s", c->client_id);
        e->data->ack = *mqtt_packet_ack(PUBREC_B, e->data->publish.pkt_id);
        ptype = PUBREC;
    }

    unsigned char *packed = pack_mqtt_packet(e->data, ptype);
    e->reply = bstring_copy(packed, MQTT_ACK_LEN);
    rc = REPLY;

exit:

#if WORKERPOOLSIZE > 1
    unlock();
#endif

    /*
     * We're in the case of AT_MOST_ONCE QoS level, we don't need to sent out
     * any byte, it's a fire-and-forget.
     */
    return rc;
}


static int puback_handler(struct io_event *e) {

    sol_debug("Received PUBACK from %s", e->client->client_id);

    // TODO Remove from pending PUBACK clients map

#if WORKERPOOLSIZE > 1
    lock();
#endif

    if (outgoing[e->data->publish.pkt_id]) {
        sol_free(outgoing[e->data->ack.pkt_id]);
        outgoing[e->data->ack.pkt_id] = NULL;
    }

#if WORKERPOOLSIZE > 1
    unlock();
#endif

    return REPLY;
}


static int pubrec_handler(struct io_event *e) {

    struct sol_client *c = e->client;

    sol_debug("Received PUBREC from %s", c->client_id);

    mqtt_pubrel *pubrel = mqtt_packet_ack(PUBREL_B, e->data->ack.pkt_id);

    e->data->ack = *pubrel;

    unsigned char *packed = pack_mqtt_packet(e->data, PUBREC);
    e->reply = bstring_copy(packed, MQTT_ACK_LEN);
    sol_free(packed);

    sol_debug("Sending PUBREL to %s", c->client_id);

    return REPLY;
}


static int pubrel_handler(struct io_event *e) {

    sol_debug("Received PUBREL from %s", e->client->client_id);

    mqtt_pubcomp *pubcomp = mqtt_packet_ack(PUBCOMP_B, e->data->ack.pkt_id);

    e->data->ack = *pubcomp;

    unsigned char *packed = pack_mqtt_packet(e->data, PUBCOMP);

    sol_debug("Sending PUBCOMP to %s", e->client->client_id);

    e->reply = bstring_copy(packed, MQTT_ACK_LEN);

    sol.pending_packets[e->data->ack.pkt_id] = false;

    return REPLY;
}

/* Utility macro to handle base case on each EPOLL loop */
#define EPOLL_ERR(e) if ((e.events & EPOLLERR) || (e.events & EPOLLHUP) || \
                         (!(e.events & EPOLLIN) && !(e.events & EPOLLOUT)))

static int pubcomp_handler(struct io_event *e) {

    sol_debug("Received PUBCOMP from %s", e->client->client_id);

    // TODO Remove from pending PUBACK clients map

    return NOREPLY;
}


static int pingreq_handler(struct io_event *e) {

    sol_debug("Received PINGREQ from %s", e->client->client_id);

    unsigned char *packed = pack_mqtt_packet(e->data, PINGRESP);
    e->reply = bstring_copy(packed, MQTT_HEADER_LEN);

    sol_free(packed);

    sol_debug("Received PINGRESP from %s", e->client->client_id);

    return REPLY;
}

/*
 * Handle incoming connections, create a a fresh new struct client structure
 * and link it to the fd, ready to be set in EPOLLIN event, then pass the
 * connection to the IO EPOLL loop, waited by the IO thread pool.
 */
static void accept_loop(struct epoll *epoll) {

    int events = 0;

    struct epoll_event *e_events =
        sol_malloc(sizeof(struct epoll_event) * EPOLL_MAX_EVENTS);

    int epollfd = epoll_create1(0);

    /*
     * We want to watch for events incoming on the server descriptor (e.g. new
     * connections)
     */
    epoll_add(epollfd, epoll->serverfd, EPOLLIN | EPOLLONESHOT, NULL);

    /*
     * And also to the global event fd, this one is useful to gracefully
     * interrupt polling and thread execution
     */
    epoll_add(epollfd, conf->run, EPOLLIN, NULL);

    while (1) {

        events = epoll_wait(epollfd, e_events, EPOLL_MAX_EVENTS, EPOLL_TIMEOUT);

        if (events < 0) {

            /* Signals to all threads. Ignore it for now */
            if (errno == EINTR)
                continue;

            /* Error occured, break the loop */
            break;
        }

        for (int i = 0; i < events; ++i) {

            /* Check for errors */
            EPOLL_ERR(e_events[i]) {

                /*
                 * An error has occured on this fd, or the socket is not
                 * ready for reading, closing connection
                 */
                perror("accept_loop :: epoll_wait(2)");
                close(e_events[i].data.fd);

            } else if (e_events[i].data.fd == conf->run) {

                /* And quit event after that */
                eventfd_t val;
                eventfd_read(conf->run, &val);

                sol_debug("Stopping epoll loop. Thread %p exiting.",
                          (void *) pthread_self());

                goto exit;

            } else if (e_events[i].data.fd == epoll->serverfd) {

                while (1) {

                    /*
                     * Accept a new incoming connection assigning ip address
                     * and socket descriptor to the connection structure
                     * pointer passed as argument
                     */

                    int fd = accept_connection(epoll->serverfd);
                    if (fd < 0)
                        break;

                    /*
                     * Create a client structure to handle his context
                     * connection
                     */
                    struct sol_client *client = sol_malloc(sizeof(*client));
                    if (!client)
                        return;

                    /* Populate client structure */
                    client->fd = fd;

                    /* Record last action as of now */
                    client->last_action_time = time(NULL);

                    /* Add it to the epoll loop */
                    epoll_add(epoll->io_epollfd, fd,
                              EPOLLIN | EPOLLONESHOT, client);

                    /* Rearm server fd to accept new connections */
                    epoll_mod(epollfd, epoll->serverfd, EPOLLIN, NULL);

                    /* Record the new client connected */
                    info.nclients++;
                    info.nconnections++;

                }
            }
        }
    }

exit:

    sol_free(e_events);
}

/*
 * Parse packet header, it is required at least the Fixed Header of each
 * packed, which is contained in the first 2 bytes in order to read packet
 * type and total length that we need to recv to complete the packet.
 *
 * This function accept a socket fd, a buffer to read incoming streams of
 * bytes and a structure formed by 2 fields:
 *
 * - buf -> a byte buffer, it will be malloc'ed in the function and it will
 *          contain the serialized bytes of the incoming packet
 * - flags -> flags pointer, copy the flag setting of the incoming packet,
 *            again for simplicity and convenience of the caller.
 */
ssize_t recv_packet(int clientfd, unsigned char **buf, unsigned char *header) {

    ssize_t nbytes = 0;
    unsigned char *tmpbuf = *buf;

    /* Read the first byte, it should contain the message type code */
    if ((nbytes = recv_bytes(clientfd, *buf, 4)) <= 0)
        return -ERRCLIENTDC;

    *header = *tmpbuf;
    tmpbuf++;

    /* Check for OPCODE, if an unknown OPCODE is received return an error */
    if (DISCONNECT < (*header >> 4) || CONNECT > (*header >> 4))
        return -ERRPACKETERR;

    /*
     * Read remaning length bytes which starts at byte 2 and can be long to 4
     * bytes based on the size stored, so byte 2-5 is dedicated to the packet
     * length.
     */
    int n = 0;

    unsigned pos = 0;
    unsigned long long tlen = mqtt_decode_length(&tmpbuf, &pos);

    /*
     * Set return code to -ERRMAXREQSIZE in case the total packet len exceeds
     * the configuration limit `max_request_size`
     */
    if (tlen > conf->max_request_size) {
        nbytes = -ERRMAXREQSIZE;
        goto exit;
    }

    if (tlen <= 4)
        goto exit;

    int offset = 4 - pos -1;

    unsigned long long remaining_bytes = tlen - offset;

    /* Read remaining bytes to complete the packet */
    while (remaining_bytes > 0) {

        if ((n = recv_bytes(clientfd, tmpbuf + offset, remaining_bytes)) < 0)
            goto err;

        remaining_bytes -= n;
        nbytes += n;
        offset += n;
    }

    nbytes -= (pos + 1);

exit:

    *buf += pos + 1;

    return nbytes;

err:

    close(clientfd);

    return nbytes;

}

/* Handle incoming requests, after being accepted or after a reply */
static int read_data(int fd, unsigned char *buf, union mqtt_packet *pkt) {

    ssize_t bytes = 0;
    unsigned char header = 0;

    /*
     * We must read all incoming bytes till an entire packet is received. This
     * is achieved by following the TrieDB protocol specifications, which
     * send the size of the remaining packet as the second byte. By knowing it
     * we know if the packet is ready to be deserialized and used.
     */
    bytes = recv_packet(fd, &buf, &header);

    /*
     * Looks like we got a client disconnection.
     *
     * TODO: Set a error_handler for ERRMAXREQSIZE instead of dropping client
     *       connection, explicitly returning an informative error code to the
     *       client connected.
     */
    if (bytes == -ERRCLIENTDC || bytes == -ERRMAXREQSIZE)
        goto errdc;

    /*
     * If a not correct packet received, we must free the buffer and reset the
     * handler to the request again, setting EPOLL to EPOLLIN
     */
    if (bytes == -ERRPACKETERR)
        goto exit;

    info.bytes_recv += bytes;

    /*
     * Unpack received bytes into a triedb_request structure and execute the
     * correct handler based on the type of the operation.
     */
    unpack_mqtt_packet(buf, pkt, header, bytes);

    return 0;

    // Disconnect packet received

exit:

    return -ERRPACKETERR;

errdc:

    sol_error("Dropping client");
    close(fd);

    info.nclients--;
    info.nconnections--;

    return -ERRCLIENTDC;
}


static void *io_worker(void *arg) {

    struct epoll *epoll = arg;
    int events = 0;
    ssize_t sent = 0;

    struct epoll_event *e_events =
        sol_malloc(sizeof(struct epoll_event) * EPOLL_MAX_EVENTS);

    /* Raw bytes buffer to handle input from client */
    unsigned char *buffer = sol_malloc(conf->max_request_size);

    // UDP bus communication client handler
    struct sockaddr_in node;
    memset(&node, 0, sizeof(node)); ;

    while (1) {

        events = epoll_wait(epoll->io_epollfd, e_events,
                            EPOLL_MAX_EVENTS, EPOLL_TIMEOUT);

        if (events < 0) {

            /* Signals to all threads. Ignore it for now */
            if (errno == EINTR)
                continue;

            /* Error occured, break the loop */
            break;
        }

        for (int i = 0; i < events; ++i) {

            /* Check for errors */
            EPOLL_ERR(e_events[i]) {

                /* An error has occured on this fd, or the socket is not
                   ready for reading, closing connection */
                perror("io_worker :: epoll_wait(2)");
                close(e_events[i].data.fd);

            } else if (e_events[i].data.fd == conf->run) {

                /* And quit event after that */
                eventfd_t val;
                eventfd_read(conf->run, &val);

                sol_debug("Stopping epoll loop. Thread %p exiting.",
                          (void *) pthread_self());

                goto exit;

            } else if (e_events[i].events & EPOLLIN) {

                struct io_event *event = sol_malloc(sizeof(*event));
                event->epollfd = epoll->io_epollfd;
                event->data = sol_malloc(sizeof(*event->data));
                event->client = e_events[i].data.ptr;
                /*
                 * Received a bunch of data from a client, after the creation
                 * of an IO event we need to read the bytes and encoding the
                 * content according to the protocol
                 */
                int rc = read_data(event->client->fd, buffer, event->data);
                if (rc == 0) {
                    /*
                     * All is ok, raise an event to the worker poll EPOLL and
                     * link it with the IO event containing the decode payload
                     * ready to be processed
                     */
                    eventfd_t ev = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
                    event->eventfd = ev;
                    epoll_add(epoll->w_epollfd, ev,
                              EPOLLIN | EPOLLONESHOT, event);

                    /* Record last action as of now */
                    event->client->last_action_time = time(NULL);

                    /* Fire an event toward the worker thread pool */
                    eventfd_write(ev, 1);

                } else if (rc == -ERRCLIENTDC || rc == -ERRPACKETERR) {

                    /*
                     * We got an unexpected error or a disconnection from the
                     * client side, remove client from the global map and
                     * free resources allocated such as io_event structure and
                     * paired payload
                     */

                    close(event->client->fd);
                    hashtable_del(sol.clients, event->client->client_id);
                    sol_free(event->data);
                    sol_free(event);
                }
            } else if (e_events[i].events & EPOLLOUT) {

                struct io_event *event = e_events[i].data.ptr;

                /*
                 * Write out to client, after a request has been processed in
                 * worker thread routine. Just send out all bytes stored in the
                 * reply buffer to the reply file descriptor.
                 */
                if ((sent = send_bytes(event->client->fd,
                                       event->reply,
                                       bstring_len(event->reply))) < 0) {
                    close(event->client->fd);
                } else {
                    /*
                     * Rearm descriptor, we're using EPOLLONESHOT feature to avoid
                     * race condition and thundering herd issues on multithreaded
                     * EPOLL
                     */
                    epoll_mod(epoll->io_epollfd,
                              event->client->fd, EPOLLIN, event->client);
                }

                // Update information stats
                info.bytes_sent += sent < 0 ? 0 : sent;

                /* Free resource, ACKs will be free'd closing the server */
                bstring_destroy(event->reply);

                mqtt_packet_release(event->data, event->data->header.bits.type);

                close(event->eventfd);

                sol_free(event);
            }
        }
    }

exit:

    sol_free(e_events);
    sol_free(buffer);

    return NULL;
}


static void *worker(void *arg) {

    struct epoll *epoll = arg;
    int events = 0;
    long int timers = 0;
    eventfd_t val;

    struct epoll_event *e_events =
        sol_malloc(sizeof(struct epoll_event) * EPOLL_MAX_EVENTS);

    while (1) {

        events = epoll_wait(epoll->w_epollfd, e_events,
                            EPOLL_MAX_EVENTS, EPOLL_TIMEOUT);

        if (events < 0) {

            /* Signals to all threads. Ignore it for now */
            if (errno == EINTR)
                continue;

            /* Error occured, break the loop */
            break;
        }

        for (int i = 0; i < events; ++i) {

            /* Check for errors */
            EPOLL_ERR(e_events[i]) {

                /*
                 * An error has occured on this fd, or the socket is not
                 * ready for reading, closing connection
                 */
                perror("worker :: epoll_wait(2)");
                close(e_events[i].data.fd);

            } else if (e_events[i].data.fd == conf->run) {

                /* And quit event after that */
                eventfd_read(conf->run, &val);

                sol_debug("Stopping epoll loop. Thread %p exiting.",
                          (void *) pthread_self());

                goto exit;

            } else if (e_events[i].data.fd == epoll->timerfd[0]) {
                (void) read(e_events[i].data.fd, &timers, sizeof(timers));
                // Check for keys about to expire out
                publish_stats();
            } else if (e_events[i].data.fd == epoll->timerfd[1]) {
                (void) read(e_events[i].data.fd, &timers, sizeof(timers));
                // Check for keys about to expire out
                pending_message_check();
            } else if (e_events[i].events & EPOLLIN) {
                struct io_event *event = e_events[i].data.ptr;
                eventfd_read(event->eventfd, &val);
                // TODO free client and remove it from the global map in case
                // of QUIT command (check return code)
                int reply = handlers[event->data->header.bits.type](event);
                if (reply == REPLY)
                    epoll_mod(event->epollfd, event->client->fd, EPOLLOUT, event);
                else if (reply != CLIENTDC) {
                    epoll_mod(epoll->io_epollfd,
                              event->client->fd, EPOLLIN, event->client);
                    close(event->eventfd);
                }
            }
        }
    }

exit:

    sol_free(e_events);

    return NULL;
}


static void publish_message(unsigned short pkt_id,
                            unsigned short topiclen,
                            const char *topic,
                            unsigned short payloadlen,
                            unsigned char *payload) {

    /* Retrieve the Topic structure from the global map, exit if not found */
    struct topic *t = sol_topic_get(&sol, topic);

    if (!t)
        return;

    /* Build MQTT packet with command PUBLISH */
    union mqtt_packet pkt;
    struct mqtt_publish *p = mqtt_packet_publish(PUBLISH_B,
                                                 pkt_id,
                                                 topiclen,
                                                 (unsigned char *) topic,
                                                 payloadlen,
                                                 payload);

    pkt.publish = *p;

    size_t len;
    unsigned char *packed;

    /* Send payload through TCP to all subscribed clients of the topic */
    struct list_node *cur = t->subscribers->head;
    ssize_t sent = 0L;
    for (; cur; cur = cur->next) {

        sol_debug("Sending PUBLISH (d%i, q%u, r%i, m%u, %s, ... (%i bytes))",
                  pkt.publish.header.bits.dup,
                  pkt.publish.header.bits.qos,
                  pkt.publish.header.bits.retain,
                  pkt.publish.pkt_id,
                  pkt.publish.topic,
                  pkt.publish.payloadlen);

        len = MQTT_HEADER_LEN + sizeof(uint16_t) +
            pkt.publish.topiclen + pkt.publish.payloadlen;

        struct subscriber *sub = cur->data;
        struct sol_client *sc = sub->client;

        /* Update QoS according to subscriber's one */
        pkt.publish.header.bits.qos = sub->qos;

        if (pkt.publish.header.bits.qos > AT_MOST_ONCE)
            len += sizeof(uint16_t);

        packed = pack_mqtt_packet(&pkt, PUBLISH);

        if ((sent = send_bytes(sc->fd, packed, len)) < 0)
            sol_error("Error publishing to %s: %s",
                      sc->client_id, strerror(errno));

        // Update information stats
        info.bytes_sent += sent;
        info.messages_sent++;

        sol_free(packed);
    }

    sol_free(p);
}

/*
 * Publish statistics periodic task, it will be called once every N config
 * defined seconds, it publish some informations on predefined topics
 */
static void publish_stats(void) {

    char cclients[number_len(info.nclients) + 1];
    sprintf(cclients, "%d", info.nclients);

    char bsent[number_len(info.bytes_sent) + 1];
    sprintf(bsent, "%lld", info.bytes_sent);

    char msent[number_len(info.messages_sent) + 1];
    sprintf(msent, "%lld", info.messages_sent);

    char mrecv[number_len(info.messages_recv) + 1];
    sprintf(mrecv, "%lld", info.messages_recv);

    long long uptime = time(NULL) - info.start_time;
    char utime[number_len(uptime) + 1];
    sprintf(utime, "%lld", uptime);

    double sol_uptime = (double)(time(NULL) - info.start_time) / SOL_SECONDS;
    char sutime[16];
    sprintf(sutime, "%.4f", sol_uptime);

    long long memory = memory_used();
    char mem[number_len(memory)];
    sprintf(mem, "%lld", memory);

    publish_message(0, strlen(sys_topics[5]), sys_topics[5],
                    strlen(utime), (unsigned char *) &utime);
    publish_message(0, strlen(sys_topics[6]), sys_topics[6],
                    strlen(sutime), (unsigned char *) &sutime);
    publish_message(0, strlen(sys_topics[7]), sys_topics[7],
                    strlen(cclients), (unsigned char *) &cclients);
    publish_message(0, strlen(sys_topics[9]), sys_topics[9],
                    strlen(bsent), (unsigned char *) &bsent);
    publish_message(0, strlen(sys_topics[11]), sys_topics[11],
                    strlen(msent), (unsigned char *) &msent);
    publish_message(0, strlen(sys_topics[12]), sys_topics[12],
                    strlen(mrecv), (unsigned char *) &mrecv);
    publish_message(0, strlen(sys_topics[13]), sys_topics[13],
                    strlen(mem), (unsigned char *) &mem);

}

/*
 * Check for pending messages in the ingoing and outgoing maps (actually
 * arrays), each position between 0-65535 contains either NULL or a pointer
 * to a pending_packet stucture with a timestamp of the sending action, the
 * target file descriptor (e.g. the client) and the payload to be sent
 * unserialized, this way it's possible to set the DUP flag easily at the cost
 * of additional packing before re-sending it out
 */
static void pending_message_check(void) {
    time_t now = time(NULL);
    for (int i = 0; i < 65535; ++i) {
        // TODO Remove hard-coded values, 65535 and 20
        if (outgoing[i] && (now - outgoing[i]->sent_timestamp) > 20) {
            // Set DUP flag to 1
            mqtt_set_dup(outgoing[i]->packet, outgoing[i]->type);
            // Serialize the packet and send it out again
            unsigned char *pub = pack_mqtt_packet(outgoing[i]->packet,
                                                  outgoing[i]->type);
            bstring payload = bstring_copy(pub, outgoing[i]->size);
            ssize_t sent;
            if ((sent = send_bytes(outgoing[i]->fd,
                                   payload, bstring_len(payload))) < 0)
                sol_error("Error re-sending %s", strerror(errno));

            // Update information stats
            info.messages_sent++;
            info.bytes_sent += sent;
        }
    }
}

/*
 * Cleanup function to be passed in as destructor to the Hashtable for
 * connecting clients
 */
static int client_destructor(struct hashtable_entry *entry) {

    if (!entry)
        return -1;

    struct sol_client *client = entry->val;

    if (client->client_id)
        sol_free(client->client_id);

    sol_free(client);

    return 0;
}


int start_server(const char *addr, const char *port) {

    /* Initialize global Sol instance */
    trie_init(&sol.topics, NULL);
    sol.clients = hashtable_new(client_destructor);
    sol.pending_packets = sol_malloc(65535);
    for (int i = 0; i < 65535; ++i)
        sol.pending_packets[i] = false;

    pthread_spin_init(&spinlock, PTHREAD_PROCESS_SHARED);

    /* Generate stats topics */
    for (int i = 0; i < SYS_TOPICS; i++)
        sol_topic_put(&sol, topic_create(sol_strdup(sys_topics[i])));

    /* Start listening for new connections */
    int sfd = make_listen(addr, port, conf->socket_family);

    struct epoll epoll = {
        .io_epollfd = epoll_create1(0),
        .w_epollfd = epoll_create1(0),
        .serverfd = sfd,
        .busfd = -1
    };

    /* Start the expiration keys check routine */
    struct itimerspec expiration_timer;

    memset(&expiration_timer, 0x00, sizeof(expiration_timer));

    expiration_timer.it_value.tv_sec = conf->stats_pub_interval;
    expiration_timer.it_value.tv_nsec = 0;
    expiration_timer.it_interval.tv_sec = conf->stats_pub_interval;
    expiration_timer.it_interval.tv_nsec = 0;

    /* And one for the pending ingoing and outgoing messages with QoS > 0 */
    struct itimerspec pending_timer;

    memset(&pending_timer, 0x00, sizeof(pending_timer));

    pending_timer.it_value.tv_sec = 0;
    pending_timer.it_value.tv_nsec = 1e8;
    pending_timer.it_interval.tv_sec = 0;
    pending_timer.it_interval.tv_nsec = 1e8;

    // add expiration keys cron task and pending messages cron task
    int exptimerfd = add_cron_task(epoll.w_epollfd, &expiration_timer);
    int pendingfd = add_cron_task(epoll.w_epollfd, &pending_timer);

    epoll.timerfd[0] = exptimerfd;
    epoll.timerfd[1] = pendingfd;

    /*
     * We need to watch for global eventfd in order to gracefully shutdown IO
     * thread pool and worker pool
     */
    epoll_add(epoll.io_epollfd, conf->run, EPOLLIN, NULL);
    epoll_add(epoll.w_epollfd, conf->run, EPOLLIN, NULL);

    pthread_t iothreads[IOPOOLSIZE];
    pthread_t workers[WORKERPOOLSIZE];

    /* Start I/O thread pool */

    for (int i = 0; i < IOPOOLSIZE; ++i)
        pthread_create(&iothreads[i], NULL, &io_worker, &epoll);

    /* Start Worker thread pool */

    for (int i = 0; i < WORKERPOOLSIZE; ++i)
        pthread_create(&workers[i], NULL, &worker, &epoll);

    sol_info("Server start");
    info.start_time = time(NULL);

    // Main thread for accept new connections
    accept_loop(&epoll);

    // Stop expire keys check routine
    epoll_del(epoll.w_epollfd, epoll.timerfd[0]);

    // Stop pending messages timer fd
    epoll_del(epoll.w_epollfd, epoll.timerfd[1]);

    /* Join started thread pools */
    for (int i = 0; i < IOPOOLSIZE; ++i)
        pthread_join(iothreads[i], NULL);

    for (int i = 0; i < WORKERPOOLSIZE; ++i)
        pthread_join(workers[i], NULL);

    hashtable_destroy(sol.clients);
    sol_free(sol.pending_packets);

    pthread_spin_destroy(&spinlock);

    sol_info("Sol v%s exiting", VERSION);

    return 0;
}
