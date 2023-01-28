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

#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include "ev.h"
#include "network.h"
#include "config.h"
#include "server.h"
#include "memory.h"
#include "logging.h"
#include "handlers.h"
#include "memorypool.h"
#include "sol_internal.h"

pthread_mutex_t mutex;

/*
 * Auxiliary structure to be used as init argument for eventloop, fd is the
 * listening socket we want to share between multiple instances, cronjobs is
 * just a flag to signal if we want to register cronjobs on that particular
 * instance or not (to not repeat useless cron jobs on multiple threads)
 */
struct listen_payload {
    int fd;
    atomic_bool cronjobs;
};

/* Seconds in a Sol, easter egg */
static const double SOL_SECONDS = 88775.24;

/*
 * General informations of the broker, all fields will be published
 * periodically to internal topics
 */
struct sol_info info;

/* Broker global instance, contains the topic trie and the clients hashtable */
struct server server;

/*
 * TCP server, based on I/O multiplexing abstraction called ev_ctx. Each thread
 * (if any) should have his own ev_ctx and thus being responsible of a subset
 * of clients.
 * At the init of the server, the ev_ctx will be instructed to run some
 * periodic tasks and to run a callback on accept on new connections. From now
 * on start a simple juggling of callbacks to be scheduled on the event loop,
 * typically after being accepted a connection his handle (fd) will be added to
 * the backend of the loop (this case we're using EPOLL as a backend but also
 * KQUEUE or SELECT/POLL should be easy to plug-in) and read_callback will be
 * run every time there's new data incoming. If a complete packet is received
 * and correctly parsed it will be processed by calling the right handler from
 * the handler module, based on the command it carries and a response will be
 * fired back.
 *
 *                             MAIN THREAD
 *                              [EV_CTX]
 *
 *    ACCEPT_CALLBACK         READ_CALLBACK         WRITE_CALLBACK
 *  -------------------    ------------------    --------------------
 *        |                        |                       |
 *      ACCEPT                     |                       |
 *        | ---------------------> |                       |
 *        |                  READ AND DECODE               |
 *        |                        |                       |
 *        |                        |                       |
 *        |                     PROCESS                    |
 *        |                        |                       |
 *        |                        |                       |
 *        |                        | --------------------> |
 *        |                        |                     WRITE
 *      ACCEPT                     |                       |
 *        | ---------------------> | <-------------------- |
 *        |                        |                       |
 *
 * The whole method could be easily distributed across a threadpool, by paying
 * attention to the shared critical parts on handler module.
 * The access to shared data strucures could be guarded by a mutex or a
 * spinlock, and being generally fast operations it shouldn't suffer high
 * contentions by the threads and thus being really fast.
 * The drawback of this approach is that the concurrency is not equally
 * distributed across all threads, each thread has it's own eventloop and thus
 * is responsible for a subset of connections, without any possibility of
 * cooperation from other threads. This should be mitigated by the fact that
 * this application mainly deals with short-lived connections so there's
 * frequent turnover of monitored FDs, increasing the number of connections
 * for each different thread.
 */

static void client_init(struct client *);

static void client_deactivate(struct client *);

// CALLBACKS for the eventloop
static void accept_callback(struct ev_ctx *, void *);

static void read_callback(struct ev_ctx *, void *);

static void write_callback(struct ev_ctx *, void *);

/*
 * Processing message function, will be applied on fully formed mqtt packet
 * received on read_callback callback
 */
static void process_message(struct ev_ctx *, struct client *);

/* Periodic routine to publish general stats about the broker on $SOL topics */
static void publish_stats(struct ev_ctx *, void *);

/*
 * Periodic routine to check for incomplete transactions on QoS > 0 to be
 * concluded
 */
static void inflight_msg_check(struct ev_ctx *, void *);

/*
 * Statistics topics, published every N seconds defined by configuration
 * interval
 */
#define SYS_TOPICS 11

/*
 * Utility struct for information topics. Just the name of the topic and his
 * length, to avoid computing length of const strings
 */
struct sys_topic {
    const char *name;
    int len;
};

/* Information topics mapping, accessed by publish_stats periodic routine */
static const struct sys_topic sys_topics[SYS_TOPICS] = {
    { "$SOL/broker/clients/", 20 },
    { "$SOL/broker/messages/", 21 },
    { "$SOL/broker/uptime/", 19 },
    { "$SOL/broker/uptime/sol", 22 },
    { "$SOL/broker/clients/connected/", 30 },
    { "$SOL/broker/clients/disconnected/", 34 },
    { "$SOL/broker/bytes/sent/", 23 },
    { "$SOL/broker/bytes/received/", 27 },
    { "$SOL/broker/messages/sent/", 26 },
    { "$SOL/broker/messages/received/", 30 },
    { "$SOL/broker/memory/used", 23 }
};

/* Simple error_code to string function, to be refined */
static const char *solerr(int rc) {
    switch (rc) {
        case -ERRCLIENTDC:
            return "Client disconnected";
        case -ERRSOCKETERR:
            return strerror(errno);
        case -ERRPACKETERR:
            return "Error reading packet";
        case -ERRMAXREQSIZE:
            return "Packet sent exceeds max size accepted";
        case -ERREAGAIN:
            return "Socket FD EAGAIN";
        case -ERRNOMEM:
            return "Out of memory";
        case MQTT_UNACCEPTABLE_PROTOCOL_VERSION:
            return "[MQTT] Unknown protocol version";
        case MQTT_IDENTIFIER_REJECTED:
            return "[MQTT] Wrong identifier";
        case MQTT_SERVER_UNAVAILABLE:
            return "[MQTT] Server unavailable";
        case MQTT_BAD_USERNAME_OR_PASSWORD:
            return "[MQTT] Bad username or password";
        case MQTT_NOT_AUTHORIZED:
            return "[MQTT] Not authorized";
        default:
            return "Unknown error";
    }
}

/*
 * ====================================================
 *  Cron tasks, to be repeated at fixed time intervals
 * ====================================================
 */

/*
 * Publish statistics periodic task, it will be called once every N config
 * defined seconds, it publishes some informations on predefined topics
 */
static void publish_stats(struct ev_ctx *ctx, void *data) {
    (void)data;

    char cclients[21];
    snprintf(cclients, 21, "%lu", info.active_connections);

    char bsent[21];
    snprintf(bsent, 21, "%lu", info.bytes_sent);

    char msent[21];
    snprintf(msent, 21, "%lu", info.messages_sent);

    char mrecv[21];
    snprintf(mrecv, 21, "%lu", info.messages_recv);

    long long uptime = time(NULL) - info.start_time;
    char utime[21];
    snprintf(utime, 21, "%llu", uptime);

    double sol_uptime = (double)(time(NULL) - info.start_time) / SOL_SECONDS;
    char sutime[16];
    snprintf(sutime, 16, "%.4f", sol_uptime);

    long long memory = memory_used();
    char mem[21];
    snprintf(mem, 21, "%lld", memory);

    // $SOL/uptime
    struct mqtt_packet p = {
        .header = (union mqtt_header) { .byte = PUBLISH_B },
        .publish = (struct mqtt_publish) {
            .pkt_id = 0,
            .topiclen = sys_topics[2].len,
            .topic = (unsigned char *) sys_topics[2].name,
            .payloadlen = strlen(utime),
            .payload = (unsigned char *) &utime
        }
    };

    publish_message(&p, topic_store_get(server.store, sys_topics[2].name));

    // $SOL/broker/uptime/sol
    p.publish.topiclen = sys_topics[3].len;
    p.publish.topic = (unsigned char *) sys_topics[3].name;
    p.publish.payloadlen = strlen(sutime);
    p.publish.payload = (unsigned char *) &sutime;

    publish_message(&p, topic_store_get(server.store, sys_topics[3].name));

    // $SOL/broker/clients/connected
    p.publish.topiclen = sys_topics[4].len;
    p.publish.topic = (unsigned char *) sys_topics[4].name;
    p.publish.payloadlen = strlen(cclients);
    p.publish.payload = (unsigned char *) &cclients;

    publish_message(&p, topic_store_get(server.store, sys_topics[4].name));

    // $SOL/broker/bytes/sent
    p.publish.topiclen = sys_topics[6].len;
    p.publish.topic = (unsigned char *) sys_topics[6].name;
    p.publish.payloadlen = strlen(bsent);
    p.publish.payload = (unsigned char *) &bsent;

    publish_message(&p, topic_store_get(server.store, sys_topics[6].name));

    // $SOL/broker/messages/sent
    p.publish.topiclen = sys_topics[8].len;
    p.publish.topic = (unsigned char *) sys_topics[8].name;
    p.publish.payloadlen = strlen(msent);
    p.publish.payload = (unsigned char *) &msent;

    publish_message(&p, topic_store_get(server.store, sys_topics[8].name));

    // $SOL/broker/messages/received
    p.publish.topiclen = sys_topics[9].len;
    p.publish.topic = (unsigned char *) sys_topics[9].name;
    p.publish.payloadlen = strlen(mrecv);
    p.publish.payload = (unsigned char *) &mrecv;

    publish_message(&p, topic_store_get(server.store, sys_topics[9].name));

    // $SOL/broker/memory/used
    p.publish.topiclen = sys_topics[10].len;
    p.publish.topic = (unsigned char *) sys_topics[10].name;
    p.publish.payloadlen = strlen(mem);
    p.publish.payload = (unsigned char *) &mem;

    publish_message(&p, topic_store_get(server.store, sys_topics[10].name));
}

/*
 * Check for inflight messages in the ingoing and outgoing maps (actually
 * arrays), each position between 0-65535 contains either NULL or a pointer
 * to an inflight stucture with a timestamp of the sending action, the
 * target file descriptor (e.g. the client) and the payload to be sent
 * unserialized, this way it's possible to set the DUP flag easily at the cost
 * of additional packing before re-sending it out
 */
static void inflight_msg_check(struct ev_ctx *ctx, void *data) {
    (void) data;
    (void) ctx;
    size_t size = 0;
    time_t now = time(NULL);
    struct mqtt_packet *p = NULL;
    struct client *c, *tmp;
#if THREADSNR > 0
    pthread_mutex_lock(&mutex);
#endif
    HASH_ITER(hh, server.clients_map, c, tmp) {
        if (!c || !c->connected || !c->session || !has_inflight(c->session))
            continue;
#if THREADSNR > 0
        pthread_mutex_lock(&c->mutex);
#endif
        for (int i = 1; i < MAX_INFLIGHT_MSGS; ++i) {
            // TODO remove 20 hardcoded value
            // Messages
            if (c->session->i_msgs[i].packet
                && (now - c->session->i_msgs[i].seen) > 20) {
                log_debug("Re-sending message to %s", c->client_id);
                p = c->session->i_msgs[i].packet;
                p->header.bits.qos = c->session->i_msgs[i].qos;
                // Set DUP flag to 1
                mqtt_set_dup(p);
                size = mqtt_size(c->session->i_msgs[i].packet, NULL);
                // Serialize the packet and send it out again
                mqtt_pack(p, c->wbuf + c->towrite);
                c->towrite += size;
                enqueue_event_write(c);
                // Update information stats
                info.messages_sent++;
            }
            // ACKs
            if (c->session->i_acks[i] > 0
                && (now - c->session->i_acks[i]) > 20) {
                log_debug("Re-sending ack to %s", c->client_id);
                struct mqtt_packet ack;
                mqtt_ack(&ack, i);
                // Set DUP flag to 1
                mqtt_set_dup(&ack);
                size = mqtt_size(&ack, NULL);
                // Serialize the packet and send it out again
                mqtt_pack(p, c->wbuf + c->towrite);
                c->towrite += size;
                enqueue_event_write(c);
                // Update information stats
                info.messages_sent++;
            }
        }
#if THREADSNR > 0
        pthread_mutex_unlock(&c->mutex);
#endif
    }
#if THREADSNR > 0
    pthread_mutex_unlock(&mutex);
#endif
}

/*
 * ======================================================
 *  Private functions and callbacks for server behaviour
 * ======================================================
 */

/*
 * All clients are pre-allocated at the start of the server, but their buffers
 * (read and write) are not, they're lazily allocated with this function, meant
 * to be called on the accept callback
 */
static void client_init(struct client *client) {
    client->online = true;
    client->connected = false;
    client->clean_session = true;
    client->client_id[0] = '\0';
    client->status = WAITING_HEADER;
    client->rc = 0;
    client->rpos = ATOMIC_VAR_INIT(0);
    client->read = ATOMIC_VAR_INIT(0);
    client->toread = ATOMIC_VAR_INIT(0);
    if (!client->rbuf)
        client->rbuf = try_calloc(conf->max_request_size, sizeof(unsigned char));
    client->wrote = ATOMIC_VAR_INIT(0);
    client->towrite = ATOMIC_VAR_INIT(0);
    if (!client->wbuf)
        client->wbuf = try_calloc(conf->max_request_size, sizeof(unsigned char));
    client->last_seen = time(NULL);
    client->has_lwt = false;
    client->session = NULL;
    pthread_mutex_init(&client->mutex, NULL);
}

/*
 * As we really don't want to completely de-allocate a client in favor of
 * making it reusable by another connection we simply deactivate it according
 * to its state (e.g. if it's a clean_session connected client or not) and we
 * allow the clients memory pool to reclaim it
 */
static void client_deactivate(struct client *client) {

#if THREADSNR > 0
    pthread_mutex_lock(&client->mutex);
#endif
    if (client->online == false) return;

    client->rpos = client->toread = client->read = 0;
    client->wrote = client->towrite = 0;
    close_connection(&client->conn);

    client->online = false;

#if THREADSNR > 0
    pthread_mutex_lock(&mutex);
#endif
    if (client->clean_session == true) {
        if (client->session) {
            topic_store_remove_wildcard(server.store, client->client_id);
            list_foreach(item, client->session->subscriptions) {
                topic_del_subscriber(item->data, client);
            }
            HASH_DEL(server.sessions, client->session);
            DECREF(client->session, struct client_session);
        }
        if (client->connected == true)
            HASH_DEL(server.clients_map, client);
        memorypool_free(server.pool, client);
    }
#if THREADSNR > 0
    pthread_mutex_unlock(&mutex);
#endif
    client->connected = false;
    client->client_id[0] = '\0';
#if THREADSNR > 0
    pthread_mutex_unlock(&client->mutex);
    pthread_mutex_destroy(&client->mutex);
#endif
}

/*
 * Parse packet header, it is required at least the Fixed Header of each
 * packed, which is contained in the first 2 bytes in order to read packet
 * type and total length that we need to recv to complete the packet.
 *
 * This function accept a socket fd, a buffer to read incoming streams of
 * bytes and a pointer to the decoded fixed header that will be set in the
 * final parsed packet.
 *
 * - c: A struct client pointer, contains the FD of the requesting client
 *      as well as his SSL context in case of TLS communication. Also it store
 *      the reading buffer to be used for incoming byte-streams, tracking
 *      read, to be read and reading position taking into account the bytes
 *      required to encode the packet length.
 */
static ssize_t recv_packet(struct client *c) {

    ssize_t nread = 0;
    unsigned opcode = 0, pos = 0;
    unsigned int pktlen = 0LL;

    // Base status, we have read 0 to 2 bytes
    if (c->status == WAITING_HEADER) {

        /*
         * Read the first two bytes, the first should contain the message type
         * code
         */
        nread = recv_data(&c->conn, c->rbuf + c->read, 2 - c->read);

        if (errno != EAGAIN && errno != EWOULDBLOCK && nread <= 0)
            return nread == -1 ? -ERRSOCKETERR : -ERRCLIENTDC;

        c->read += nread;

        if (errno == EAGAIN && c->read < 2)
            return -ERREAGAIN;

        c->status = WAITING_LENGTH;
    }

    /*
     * We have already read the packet HEADER, thus we know what packet we're
     * dealing with, we're between bytes 2-4, as after the 1st byte, the
     * remaining 3 can be all used to store the packet length, or, in case of
     * ACK type packet or PINGREQ/PINGRESP and DISCONNECT, the entire packet
     */
    if (c->status == WAITING_LENGTH) {

        if (c->read == 2) {
            opcode = *c->rbuf >> 4;

            /*
             * Check for OPCODE, if an unknown OPCODE is received return an
             * error
             */
            if (DISCONNECT < opcode || CONNECT > opcode)
                return -ERRPACKETERR;

            /*
             * We have a PINGRESP/PINGREQ or a DISCONNECT packet, we're done
             * here
             */
            if (opcode > UNSUBSCRIBE) {
                c->rpos = 2;
                c->toread = c->read;
                goto exit;
            }
        }

        /*
         * Read 2 extra bytes, because the first 4 bytes could countain the
         * total size in bytes of the entire packet
         */
        nread = recv_data(&c->conn, c->rbuf + c->read, 4 - c->read);

        if (errno != EAGAIN && errno != EWOULDBLOCK && nread <= 0)
            return nread == -1 ? -ERRSOCKETERR : -ERRCLIENTDC;

        c->read += nread;

        if (errno == EAGAIN && c->read < 4)
            return -ERREAGAIN;

        /*
         * Read remaining length bytes which starts at byte 2 and can be long to
         * 4 bytes based on the size stored, so byte 2-5 is dedicated to the
         * packet length.
         */
        pktlen = mqtt_decode_length(c->rbuf + 1, &pos);

        /*
         * Set return code to -ERRMAXREQSIZE in case the total packet len
         * exceeds the configuration limit `max_request_size`
         */
        if (pktlen > conf->max_request_size)
            return -ERRMAXREQSIZE;

        /*
         * Update the toread field for the client with the entire length of
         * the current packet, which is comprehensive of packet length,
         * bytes used to encode it and 1 byte for the header
         * We've already tracked the bytes we read so far, we just need to
         * read toread-read bytes.
         */
        c->rpos = pos + 1;
        c->toread = pktlen + pos + 1;  // pos = bytes used to store length

        /* Looks like we got an ACK packet, we're done reading */
        if (pktlen <= 4)
            goto exit;

        c->status = WAITING_DATA;
    }

    /*
     * Last status, we have access to the length of the packet and we know for
     * sure that it's not a PINGREQ/PINGRESP/DISCONNECT packet.
     */
    nread = recv_data(&c->conn, c->rbuf + c->read, c->toread - c->read);

    if (errno != EAGAIN && errno != EWOULDBLOCK && nread <= 0)
        return nread == -1 ? -ERRSOCKETERR : -ERRCLIENTDC;

    c->read += nread;
    if (errno == EAGAIN && c->read < c->toread)
        return -ERREAGAIN;

exit:

    return SOL_OK;
}

/*
 * Handle incoming requests, after being accepted or after a reply, under the
 * hood it calls recv_packet and return an error code according to the outcome
 * of the operation
 */
static inline int read_data(struct client *c) {

    /*
     * We must read all incoming bytes till an entire packet is received. This
     * is achieved by following the MQTT protocol specifications, which
     * send the size of the remaining packet as the second byte. By knowing it
     * we know if the packet is ready to be deserialized and used.
     */
    int err = recv_packet(c);

    /*
     * Looks like we got a client disconnection or If a not correct packet
     * received, we must free the buffer and reset the handler to the request
     * again, setting EPOLL to EPOLLIN
     *
     * TODO: Set a error_handler for ERRMAXREQSIZE instead of dropping client
     *       connection, explicitly returning an informative error code to the
     *       client connected.
     */
    if (err < 0)
        goto err;

    if (c->read < c->toread)
        return -ERREAGAIN;

    info.bytes_recv += c->read;

    return SOL_OK;

    // Disconnect packet received

err:

    return err;
}

/*
 * Write stream of bytes to a client represented by a connection object, till
 * all bytes to be written is exhausted, tracked by towrite field or if an
 * EAGAIN (socket descriptor must be in non-blocking mode) error is raised,
 * meaning we cannot write anymore for the current cycle.
 */
static inline int write_data(struct client *c) {
#if THREADSNR > 0
    pthread_mutex_lock(&c->mutex);
#endif
    ssize_t wrote = send_data(&c->conn, c->wbuf+c->wrote, c->towrite-c->wrote);
    if (errno != EAGAIN && errno != EWOULDBLOCK && wrote < 0)
        goto clientdc;
    c->wrote += wrote > 0 ? wrote : 0;
    if (c->wrote < c->towrite && errno == EAGAIN)
        goto eagain;
    // Update information stats
    info.bytes_sent += c->towrite;
    // Reset client written bytes track fields
    c->towrite = c->wrote = 0;
#if THREADSNR > 0
    pthread_mutex_unlock(&c->mutex);
#endif
    return SOL_OK;

clientdc:
#if THREADSNR > 0
    pthread_mutex_unlock(&c->mutex);
#endif
    return -ERRSOCKETERR;

eagain:
#if THREADSNR > 0
    pthread_mutex_unlock(&c->mutex);
#endif
    return -ERREAGAIN;
}

/*
 * ===========
 *  Callbacks
 * ===========
 */

/*
 * Callback dedicated to client replies, try to send as much data as possible
 * epmtying the client buffer and rearming the socket descriptor for reading
 * after
 */
static void write_callback(struct ev_ctx *ctx, void *arg) {
    struct client *client = arg;
    int err = write_data(client);
    switch (err) {
        case SOL_OK:
            /*
             * Rearm descriptor making it ready to receive input,
             * read_callback will be the callback to be used; also reset the
             * read buffer status for the client.
             */
            client->status = WAITING_HEADER;
            ev_fire_event(ctx, client->conn.fd, EV_READ, read_callback, client);
            break;
        case -ERREAGAIN:
            /*
             * We have an EAGAIN error, which is really just signaling that
             * for some reasons the kernel is not ready to write more bytes at
             * the moment and it would block, so we just want to re-try some
             * time later, re-enqueuing a new write event
             */
            enqueue_event_write(client);
            break;
        default:
            log_info("Closing connection with %s (%s): %s %i",
                     client->client_id, client->conn.ip,
                     solerr(client->rc), err);
            ev_del_fd(ctx, client->conn.fd);
            client_deactivate(client);
            // Update stats
            info.active_connections--;
            info.total_connections--;
            break;
    }
}

/*
 * Handle incoming connections, create a a fresh new struct client structure
 * and link it to the fd, ready to be set in EV_READ event, then schedule a
 * call to the read_callback to handle incoming streams of bytes
 */
static void accept_callback(struct ev_ctx *ctx, void *data) {
    int serverfd = *((int *) data);
    while (1) {

        /*
         * Accept a new incoming connection assigning ip address
         * and socket descriptor to the connection structure
         * pointer passed as argument
         */
        struct connection conn;
        connection_init(&conn, conf->tls ? server.ssl_ctx : NULL);
        int fd = accept_connection(&conn, serverfd);
        if (fd == 0)
            continue;
        if (fd < 0) {
            close_connection(&conn);
            break;
        }

        /*
         * Create a client structure to handle his context
         * connection
         */
#if THREADSNR > 0
        pthread_mutex_lock(&mutex);
#endif
        struct client *c = memorypool_alloc(server.pool);
#if THREADSNR > 0
        pthread_mutex_unlock(&mutex);
#endif
        c->conn = conn;
        client_init(c);
        c->ctx = ctx;

        /* Add it to the epoll loop */
        ev_register_event(ctx, fd, EV_READ, read_callback, c);

        /* Record the new client connected */
        info.active_connections++;
        info.total_connections++;

        log_info("[%p] Connection from %s", (void *) pthread_self(), conn.ip);
    }
}

/*
 * Reading packet callback, it's the main function that will be called every
 * time a connected client has some data to be read, notified by the eventloop
 * context.
 */
static void read_callback(struct ev_ctx *ctx, void *data) {
    struct client *c = data;
    if (c->status == SENDING_DATA)
        return;
    /*
     * Received a bunch of data from a client, after the creation
     * of an IO event we need to read the bytes and encoding the
     * content according to the protocol
     */
    int rc = read_data(c);
    switch (rc) {
        case SOL_OK:
            /*
             * All is ok, raise an event to the worker poll EPOLL and
             * link it with the IO event containing the decode payload
             * ready to be processed
             */
            /* Record last action as of now */
            c->last_seen = time(NULL);
            c->status = SENDING_DATA;
            process_message(ctx, c);
            break;
        case -ERRCLIENTDC:
        case -ERRSOCKETERR:
        case -ERRPACKETERR:
        case -ERRMAXREQSIZE:
            // TODO move to default branch
            /*
             * We got an unexpected error or a disconnection from the
             * client side, remove client from the global map and
             * free resources allocated such as io_event structure and
             * paired payload
             */
            log_error("Closing connection with %s (%s): %s",
                      c->client_id, c->conn.ip, solerr(rc));
#if THREADSNR > 0
            pthread_mutex_lock(&mutex);
#endif
            // Publish, if present, LWT message
            if (c->has_lwt == true) {
                char *tname = (char *) c->session->lwt_msg.publish.topic;
                struct topic *t = topic_store_get(server.store, tname);
                if (t)
                    publish_message(&c->session->lwt_msg, t);
            }
            // Clean resources
            ev_del_fd(ctx, c->conn.fd);
            // Remove from subscriptions for now
            if (c->session && list_size(c->session->subscriptions) > 0) {
                list_foreach(item, c->session->subscriptions) {
                    log_debug("Deleting %s from topic %s",
                              c->client_id, ((struct topic *) item->data)->name);
                    topic_del_subscriber(item->data, c);
                }
            }
#if THREADSNR > 0
            pthread_mutex_unlock(&mutex);
#endif
            client_deactivate(c);
            info.active_connections--;
            info.total_connections--;
            break;
        case -ERREAGAIN:
            /*
             * We have an EAGAIN error, which is really just signaling that
             * for some reasons the kernel is not ready to read more bytes at
             * the moment and it would block, so we just want to re-try some
             * time later, re-enqueuing a new read event
             */
            ev_fire_event(ctx, c->conn.fd, EV_READ, read_callback, c);
            break;
    }
}

/*
 * This function is called only if the client has sent a full stream of bytes
 * consisting of a complete packet as expected by the MQTT protocol and by the
 * declared length of the packet.
 * It uses eventloop APIs to react accordingly to the packet type received,
 * validating it before proceed to call handlers. Depending on the handler
 * called and its outcome, it'll enqueue an event to write a reply or just
 * reset the client state to allow reading some more packets.
 */
static void process_message(struct ev_ctx *ctx, struct client *c) {
    struct io_event io = { .client = c };
    /*
     * Unpack received bytes into a mqtt_packet structure and execute the
     * correct handler based on the type of the operation.
     */
    mqtt_unpack(c->rbuf + c->rpos, &io.data, *c->rbuf, c->read - c->rpos);
    c->toread = c->read = c->rpos = 0;
    c->rc = handle_command(io.data.header.bits.type, &io);
    switch (c->rc) {
        case REPLY:
        case MQTT_NOT_AUTHORIZED:
        case MQTT_BAD_USERNAME_OR_PASSWORD:
            /*
             * Write out to client, after a request has been processed in
             * worker thread routine. Just send out all bytes stored in the
             * reply buffer to the reply file descriptor.
             */
            enqueue_event_write(c);
            /* Free resource, ACKs will be free'd closing the server */
            if (io.data.header.bits.type != PUBLISH)
                mqtt_packet_destroy(&io.data);
            break;
        case -ERRCLIENTDC:
            ev_del_fd(ctx, c->conn.fd);
            client_deactivate(io.client);
            // Update stats
            info.active_connections--;
            info.total_connections--;
            break;
        case -ERRNOMEM:
            log_error(solerr(c->rc));
            break;
        default:
            c->status = WAITING_HEADER;
            if (io.data.header.bits.type != PUBLISH)
                mqtt_packet_destroy(&io.data);
            break;
    }
}

/*
 * Eventloop stop callback, will be triggered by an EV_CLOSEFD event and stop
 * the running loop, unblocking the call.
 */
static void stop_handler(struct ev_ctx *ctx, void *arg) {
    (void) arg;
    ev_stop(ctx);
}

/*
 * IO worker function, wait for events on a dedicated epoll descriptor which
 * is shared among multiple threads for input and output only, following the
 * normal EPOLL semantic, EPOLLIN for incoming bytes to be unpacked and
 * processed by a worker thread, EPOLLOUT for bytes incoming from a worker
 * thread, ready to be delivered out.
 */
static void eventloop_start(void *args) {
    struct listen_payload *loop_data = args;
    struct ev_ctx ctx;
    int sfd = loop_data->fd;
    ev_init(&ctx, EVENTLOOP_MAX_EVENTS);
    // Register stop event
#ifdef __linux__
    ev_register_event(&ctx, conf->run, EV_CLOSEFD|EV_READ, stop_handler, NULL);
#else
    ev_register_event(&ctx, conf->run[1], EV_CLOSEFD|EV_READ, stop_handler, NULL);
#endif
    // Register listening FD with accept callback
    ev_register_event(&ctx, sfd, EV_READ, accept_callback, &sfd);
    // Register periodic tasks
    if (loop_data->cronjobs == true) {
        ev_register_cron(&ctx, publish_stats, NULL, conf->stats_pub_interval, 0);
        ev_register_cron(&ctx, inflight_msg_check, NULL, 1, 0);
    }
    // Start the loop, blocking call
    ev_run(&ctx);
    ev_destroy(&ctx);
}

/*
 * ===================
 *  Main APIs exposed
 * ===================
 */

/*
 * Fire a write callback to reply after a client request, under the hood it
 * schedules an EV_WRITE event with a client pointer set to write carried
 * contents out on the socket descriptor.
 */
void enqueue_event_write(const struct client *c) {
    ev_fire_event(c->ctx, c->conn.fd, EV_WRITE, write_callback, (void *) c);
}

/*
 * Main entry point for the server, to be called with an address and a port
 * to start listening. The function may fail only in the case of Out of memory
 * error occurs or listen call fails, on the other cases it should just log
 * unexpected errors.
 */
int start_server(const char *addr, const char *port) {

    INIT_INFO;

    /* Initialize global Sol instance */
    server.store = topic_store_new();
    server.auths = NULL;
    server.pool = memorypool_new(BASE_CLIENTS_NUM, sizeof(struct client));
    if (!server.pool)
        log_fatal("Failed to allocate %d sized memory pool for clients",
                  BASE_CLIENTS_NUM);
    server.clients_map = NULL;
    server.sessions = NULL;
    pthread_mutex_init(&mutex, NULL);

    if (conf->allow_anonymous == false)
        if (!config_read_passwd_file(conf->password_file, &server.auths))
            log_error("Failed to read password file");

    /* Generate stats topics */
    for (int i = 0; i < SYS_TOPICS; i++) {
        struct topic *t = topic_new(try_strdup(sys_topics[i].name));
        if (!t)
            log_fatal("start_server failed: Out of memory");
        topic_store_put(server.store, t);
    }

    /* Start listening for new connections */
    int sfd = make_listen(addr, port, conf->socket_family);

    /* Setup SSL in case of flag true */
    if (conf->tls == true) {
        openssl_init();
        server.ssl_ctx = create_ssl_context();
        load_certificates(server.ssl_ctx, conf->cafile,
                          conf->certfile, conf->keyfile);
    }

    log_info("Server start");
    info.start_time = time(NULL);

    struct listen_payload loop_start = { sfd, ATOMIC_VAR_INIT(false) };

#if THREADSNR > 0
    pthread_t thrs[THREADSNR];
    for (int i = 0; i < THREADSNR; ++i) {
        pthread_create(&thrs[i], NULL, (void * (*) (void *)) &eventloop_start, &loop_start);
    }
#endif
    loop_start.cronjobs = true;
    // start eventloop, could be spread on multiple threads
    eventloop_start(&loop_start);

#if THREADSNR > 0
    for (int i = 0; i < THREADSNR; ++i)
        pthread_join(thrs[i], NULL);
#endif

    close(sfd);
    AUTH_DESTROY(server.auths);
    topic_store_destroy(server.store);

    /* Destroy SSL context, if any present */
    if (conf->tls == true) {
        SSL_CTX_free(server.ssl_ctx);
        openssl_cleanup();
    }
    pthread_mutex_destroy(&mutex);

    log_info("Sol v%s exiting", VERSION);

    return SOL_OK;
}

/*
 * Make the entire process a daemon running in background
 */
void daemonize(void) {

    int fd;

    if (fork() != 0)
        exit(0);

    setsid();

    if ((fd = open("/dev/null", O_RDWR, 0)) != -1) {
        dup2(fd, STDIN_FILENO);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        if (fd > STDERR_FILENO) close(fd);
    }
}
