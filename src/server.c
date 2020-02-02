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

#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include "network.h"
#include "util.h"
#include "config.h"
#include "sol_internal.h"
#include "server.h"
#include "handlers.h"
#include "memorypool.h"
#include "ev.h"

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
 * Right now we're using a single thread, but the whole method could be easily
 * distributed across a threadpool, by paying attention to the shared critical
 * parts on handler module.
 * The access to shared data strucures on the worker thread pool could be
 * guarded by a spinlock, and being generally fast operations it shouldn't
 * suffer high contentions by the threads and thus being really fast.
 */

static void client_deactivate(struct client *);

static int wildcard_destructor(struct list_node *);

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

static void client_init(struct client *);

static struct topic *topic_new(const char *);

static void topic_init(struct topic *, const char *);

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
 * Publish statistics periodic task, it will be called once every N config
 * defined seconds, it publishes some informations on predefined topics
 */
static void publish_stats(struct ev_ctx *ctx, void *data) {
    (void)data;

    char cclients[21];
    snprintf(cclients, 21, "%d", info.nclients);

    char bsent[21];
    snprintf(bsent, 21, "%lld", info.bytes_sent);

    char msent[21];
    snprintf(msent, 21, "%lld", info.messages_sent);

    char mrecv[21];
    snprintf(mrecv, 21, "%lld", info.messages_recv);

    long long uptime = time(NULL) - info.start_time;
    char utime[21];
    snprintf(utime, 21, "%lld", uptime);

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

    publish_message(&p, topic_get(&server, (char *) p.publish.topic), ctx);

    // $SOL/broker/uptime/sol
    p.publish.topiclen = sys_topics[3].len;
    p.publish.topic = (unsigned char *) sys_topics[3].name;
    p.publish.payloadlen = strlen(sutime);
    p.publish.payload = (unsigned char *) &sutime;

    publish_message(&p, topic_get(&server, (char *) p.publish.topic), ctx);

    // $SOL/broker/clients/connected
    p.publish.topiclen = sys_topics[4].len;
    p.publish.topic = (unsigned char *) sys_topics[4].name;
    p.publish.payloadlen = strlen(cclients);
    p.publish.payload = (unsigned char *) &cclients;

    publish_message(&p, topic_get(&server, (char *) p.publish.topic), ctx);

    // $SOL/broker/bytes/sent
    p.publish.topiclen = sys_topics[6].len;
    p.publish.topic = (unsigned char *) sys_topics[6].name;
    p.publish.payloadlen = strlen(bsent);
    p.publish.payload = (unsigned char *) &bsent;

    publish_message(&p, topic_get(&server, (char *) p.publish.topic), ctx);

    // $SOL/broker/messages/sent
    p.publish.topiclen = sys_topics[8].len;
    p.publish.topic = (unsigned char *) sys_topics[8].name;
    p.publish.payloadlen = strlen(msent);
    p.publish.payload = (unsigned char *) &msent;

    publish_message(&p, topic_get(&server, (char *) p.publish.topic), ctx);

    // $SOL/broker/messages/received
    p.publish.topiclen = sys_topics[9].len;
    p.publish.topic = (unsigned char *) sys_topics[9].name;
    p.publish.payloadlen = strlen(mrecv);
    p.publish.payload = (unsigned char *) &mrecv;

    publish_message(&p, topic_get(&server, (char *) p.publish.topic), ctx);

    // $SOL/broker/memory/used
    p.publish.topiclen = sys_topics[10].len;
    p.publish.topic = (unsigned char *) sys_topics[10].name;
    p.publish.payloadlen = strlen(mem);
    p.publish.payload = (unsigned char *) &mem;

    publish_message(&p, topic_get(&server, (char *) p.publish.topic), ctx);
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
    time_t now = time(NULL);
    struct mqtt_packet *p = NULL;
    struct client *c, *tmp;
    HASH_ITER(hh, server.clients_map, c, tmp) {
        if (!c || !c->connected || !c->session || !c->session->has_inflight)
            continue;
        for (int i = 1; i < MAX_INFLIGHT_MSGS; ++i) {
            // TODO remove 20 hardcoded value
            // Messages
            if (c->session->i_msgs[i].in_use
                && (now - c->session->i_msgs[i].seen) > 20) {
                log_debug("Re-sending message to %s", c->client_id);
                p = c->session->i_msgs[i].packet;
                p->header.bits.qos = c->session->i_msgs[i].qos;
                // Set DUP flag to 1
                mqtt_set_dup(p);
                // Serialize the packet and send it out again
                mqtt_pack(p, c->wbuf + c->towrite);
                c->towrite += c->session->i_msgs[i].size;
                enqueue_event_write(ctx, c);
                // Update information stats
                info.messages_sent++;
            }
            // ACKs
            if (c->session->i_acks[i].in_use
                && (now - c->session->i_acks[i].seen) > 20) {
                log_debug("Re-sending ack to %s", c->client_id);
                p = c->session->i_acks[i].packet;
                p->header.bits.qos = c->session->i_acks[i].qos;
                // Set DUP flag to 1
                mqtt_set_dup(p);
                // Serialize the packet and send it out again
                mqtt_pack(p, c->wbuf + c->towrite);
                c->towrite += c->session->i_acks[i].size;
                enqueue_event_write(ctx, c);
                // Update information stats
                info.messages_sent++;
            }
        }
    }
}

static int subscription_cmp(const void *ptr_s1, const void *ptr_s2) {
    struct subscription *s1 = ((struct list_node *) ptr_s1)->data;
    const char *id = ptr_s2;
    return STREQ(s1->subscriber->id, id, MQTT_CLIENT_ID_LEN);
}

static void client_deactivate(struct client *client) {

    if (client->online == false) return;

    client->rpos = client->toread = client->read = 0;
    client->wrote = client->towrite = 0;
    close_connection(&client->conn);

    client->online = false;

    if (client->clean_session == true) {
        if (client->session) {
            list_remove(server.wildcards, client->client_id, subscription_cmp);
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
    client->connected = false;
    client->client_id[0] = '\0';
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
    unsigned long long pktlen = 0LL;

    // Base status, we have read 0 to 2 bytes
    if (c->status == WAITING_HEADER) {

        /*
         * Read the first two bytes, the first should contain the message type
         * code
         */
        nread = recv_data(&c->conn, c->rbuf + c->read, 2 - c->read);

        if (errno != EAGAIN && errno != EWOULDBLOCK && nread <= 0)
            return -ERRCLIENTDC;

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
            return -ERRCLIENTDC;

        c->read += nread;

        if (errno == EAGAIN && c->read < 4)
            return -ERREAGAIN;

        /*
         * Read remaning length bytes which starts at byte 2 and can be long to
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
        return -ERRCLIENTDC;

    c->read += nread;
    if (errno == EAGAIN && c->read < c->toread)
        return -ERREAGAIN;

exit:

    return 0;
}

/* Handle incoming requests, after being accepted or after a reply */
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

    return 0;

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
    ssize_t wrote = send_data(&c->conn, c->wbuf+c->wrote, c->towrite-c->wrote);
    if (errno != EAGAIN && errno != EWOULDBLOCK && wrote < 0)
        return -ERRCLIENTDC;
    c->wrote += wrote > 0 ? wrote : 0;
    if (c->wrote < c->towrite && errno == EAGAIN)
        return -ERREAGAIN;
    // Update information stats
    info.bytes_sent += c->towrite;
    // Reset client written bytes track fields
    c->towrite = c->wrote = 0;
    return 0;
}

/*
 * Callback dedicated to client replies, try to send as much data as possible
 * epmtying the client buffer and rearming the socket descriptor for reading
 * after
 */
static void write_callback(struct ev_ctx *ctx, void *arg) {
    struct client *client = arg;
    int err = write_data(client);
    switch (err) {
        case 0: // OK
            /*
             * Rearm descriptor making it ready to receive input,
             * read_callback will be the callback to be used; also reset the
             * read buffer status for the client.
             */
            client->status = WAITING_HEADER;
            ev_fire_event(ctx, client->conn.fd, EV_READ, read_callback, client);
            break;
        case -ERREAGAIN:
            enqueue_event_write(ctx, client);
            break;
        default:
            log_info("Closing connection with %s (%s): %s %i",
                     client->client_id, client->conn.ip,
                     solerr(client->rc), err);
            ev_del_fd(ctx, client->conn.fd);
            client_deactivate(client);
            // Update stats
            info.nclients--;
            info.nconnections--;
            break;
    }
}

/*
 * Handle incoming connections, create a a fresh new struct client structure
 * and link it to the fd, ready to be set in EPOLLIN event, then pass the
 * connection to the IO EPOLL loop, waited by the IO thread pool.
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
        struct client *c = memorypool_alloc(server.pool);
        c->conn = conn;
        client_init(c);

        /* Add it to the epoll loop */
        ev_register_event(ctx, fd, EV_READ, read_callback, c);

        /* Record the new client connected */
        info.nclients++;
        info.nconnections++;

        log_info("[%p] Connection from %s", (void *) pthread_self(), conn.ip);
    }
}

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
        case 0:
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
        case -ERRPACKETERR:
        case -ERRMAXREQSIZE:
            /*
             * We got an unexpected error or a disconnection from the
             * client side, remove client from the global map and
             * free resources allocated such as io_event structure and
             * paired payload
             */
            log_error("Closing connection with %s (%s): %s",
                      c->client_id, c->conn.ip, solerr(rc));
            // Publish, if present, LWT message
            if (c->has_lwt == true) {
                char *tname = (char *) c->session->lwt_msg.publish.topic;
                struct topic *t = topic_get(&server, tname);
                publish_message(&c->session->lwt_msg, t, ctx);
            }
            // Clean resources
            ev_del_fd(ctx, c->conn.fd);
            // Remove from subscriptions for now
            if (c->session && list_size(c->session->subscriptions) > 0) {
                struct list *subs = c->session->subscriptions;
                struct iterator *it = iter_new(subs, list_iter_next);
                FOREACH (it) {
                    log_debug("Deleting %s from topic %s",
                              c->client_id, ((struct topic *) it->ptr)->name);
                    topic_del_subscriber(it->ptr, c);
                }
                iter_destroy(it);
            }
            client_deactivate(c);
            info.nclients--;
            info.nconnections--;
            break;
        case -ERREAGAIN:
            ev_fire_event(ctx, c->conn.fd, EV_READ, read_callback, c);
            break;
    }
}

static void process_message(struct ev_ctx *ctx, struct client *c) {
    struct io_event io = { .client = c, .ctx = ctx };
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
            enqueue_event_write(ctx, c);
            /* Free resource, ACKs will be free'd closing the server */
            if (io.data.header.bits.type != PUBLISH)
                mqtt_packet_destroy(&io.data);
            break;
        case -ERRCLIENTDC:
            ev_del_fd(ctx, c->conn.fd);
            client_deactivate(io.client);
            // Update stats
            info.nclients--;
            info.nconnections--;
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
    int sfd = *((int *) args);
    struct ev_ctx ctx;
    ev_init(&ctx, EVENTLOOP_MAX_EVENTS);
    // Register stop event
    ev_register_event(&ctx, conf->run, EV_CLOSEFD|EV_READ, stop_handler, NULL);
    // Register listening FD with accept callback
    ev_register_event(&ctx, sfd, EV_READ, accept_callback, &sfd);
    // Register periodic tasks
    ev_register_cron(&ctx, publish_stats, NULL, conf->stats_pub_interval, 0);
    ev_register_cron(&ctx, inflight_msg_check, NULL, 0, 9e8);
    // Start the loop, blocking call
    ev_run(&ctx);
    ev_destroy(&ctx);
}

/* Fire a write callback to reply after a client request */
void enqueue_event_write(struct ev_ctx *ctx, struct client *c) {
    ev_fire_event(ctx, c->conn.fd, EV_WRITE, write_callback, c);
}

static int wildcard_destructor(struct list_node *node) {
    if (!node)
        return -1;
    struct subscription *s = node->data;
    DECREF(s->subscriber, struct subscriber);
    xfree((char *) s->topic);
    xfree(s);
    xfree(node);
    return 0;
}

int start_server(const char *addr, const char *port) {

    /* Initialize global Sol instance */
    trie_init(&server.topics, NULL);
    server.maxfd = BASE_CLIENTS_NUM - 1;
    server.authentications = NULL;
    server.pool = memorypool_new(BASE_CLIENTS_NUM, sizeof(struct client));
    server.clients_map = NULL;
    server.sessions = NULL;
    server.wildcards = list_new(wildcard_destructor);

    if (conf->allow_anonymous == false)
        config_read_passwd_file(conf->password_file, &server.authentications);

    /* Generate stats topics */
    for (int i = 0; i < SYS_TOPICS; i++)
        topic_put(&server, topic_new(xstrdup(sys_topics[i].name)));

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

    // start eventloop, could be spread on multiple threads
    eventloop_start(&sfd);

    close(sfd);
    AUTH_DESTROY(server.authentications);
    list_destroy(server.wildcards, 1);

    /* Destroy SSL context, if any present */
    if (conf->tls == true) {
        SSL_CTX_free(server.ssl_ctx);
        openssl_cleanup();
    }

    log_info("Sol v%s exiting", VERSION);

    return 0;
}

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

static void client_init(struct client *client) {
    client->online = true;
    client->connected = false;
    client->clean_session = true;
    client->client_id[0] = '\0';
    client->status = WAITING_HEADER;
    client->rc = 0;
    client->rpos = 0;
    client->read = 0;
    client->toread = 0;
    if (!client->rbuf)
        client->rbuf = xcalloc(conf->max_request_size, sizeof(unsigned char));
    client->wrote = 0;
    client->towrite = 0;
    if (!client->wbuf)
        client->wbuf = xcalloc(conf->max_request_size, sizeof(unsigned char));
    client->last_seen = time(NULL);
    client->has_lwt = false;
    client->session = NULL;
}

static struct topic *topic_new(const char *name) {
    struct topic *t = xmalloc(sizeof(*t));
    if (!t) return NULL;
    topic_init(t, name);
    return t;
}

static void subscriber_free(const struct ref *r) {
    struct subscriber *sub = container_of(r, struct subscriber, refcount);
    xfree(sub);
}

static void topic_init(struct topic *t, const char *name) {
    t->name = name;
    t->subscribers = NULL;
    t->retained_msg = NULL;
}

static void session_free(const struct ref *refcount) {
    struct client_session *session =
        container_of(refcount, struct client_session, refcount);
    list_destroy(session->subscriptions, 0);
    list_destroy(session->outgoing_msgs, 0);
    for (int i = 0; i < MAX_INFLIGHT_MSGS; ++i) {
        if (session->i_acks[i].in_use)
            DECREF(session->i_acks[i].packet, struct mqtt_packet);
        if (session->in_i_acks[i].in_use)
            DECREF(session->in_i_acks[i].packet, struct mqtt_packet);
        if (session->i_msgs[i].in_use)
            DECREF(session->i_msgs[i].packet, struct mqtt_packet);
    }
    xfree(session->i_acks);
    xfree(session->i_msgs);
    xfree(session->in_i_acks);
    xfree(session);
}

void session_init(struct client_session *session, char *session_id) {
    session->has_inflight = false;
    session->next_free_mid = 1;
    session->subscriptions = list_new(NULL);
    session->outgoing_msgs = list_new(NULL);
    snprintf(session->session_id, MQTT_CLIENT_ID_LEN, "%s", session_id);
    session->i_acks = xcalloc(MAX_INFLIGHT_MSGS, sizeof(struct inflight_msg));
    session->i_msgs = xcalloc(MAX_INFLIGHT_MSGS, sizeof(struct inflight_msg));
    session->in_i_acks = xcalloc(MAX_INFLIGHT_MSGS, sizeof(struct inflight_msg));
    session->refcount = (struct ref) { session_free, 0 };
}

struct client_session *client_session_alloc(char *session_id) {
    struct client_session *session = xmalloc(sizeof(*session));
    if (!session) return NULL;
    session_init(session, session_id);
    snprintf(session->session_id, MQTT_CLIENT_ID_LEN, "%s", session_id);
    return session;
}

bool is_subscribed(const struct topic *t, const struct client_session *s) {
    struct subscriber *dummy = NULL;
    HASH_FIND_STR(t->subscribers, s->session_id, dummy);
    return dummy != NULL;
}

struct subscriber *subscriber_new(struct topic *t,
                                  struct client_session * s,
                                  unsigned char qos) {
    struct subscriber *sub = xmalloc(sizeof(*sub));
    if (!sub) return NULL;
    sub->session = s;
    sub->granted_qos = qos;
    sub->refcount = (struct ref) { .count = 0, .free = subscriber_free };
    memcpy(sub->id, s->session_id, MQTT_CLIENT_ID_LEN);
    return sub;
}

struct subscriber *subscriber_clone(const struct subscriber *s) {
    struct subscriber *sub = xmalloc(sizeof(*sub));
    if (!sub) return NULL;
    sub->session = s->session;
    sub->granted_qos = s->granted_qos;
    sub->refcount = (struct ref) { .count = 0, .free = subscriber_free };
    memcpy(sub->id, s->id, MQTT_CLIENT_ID_LEN);
    return sub;
}

struct subscriber *topic_add_subscriber(struct topic *t,
                                        struct client_session *s,
                                        unsigned char qos) {
    struct subscriber *sub = xmalloc(sizeof(*sub)), *tmp;
    if (!sub) return NULL;
    sub->session = s;
    sub->granted_qos = qos;
    sub->refcount = (struct ref) { .count = 0, .free = subscriber_free };
    memcpy(sub->id, s->session_id, MQTT_CLIENT_ID_LEN);
    HASH_FIND_STR(t->subscribers, sub->id, tmp);
    if (!tmp)
        HASH_ADD_STR(t->subscribers, id, sub);
    return sub;
}

void topic_del_subscriber(struct topic *t, struct client *c) {
    struct subscriber *sub = NULL;
    HASH_FIND_STR(t->subscribers, c->client_id, sub);
    if (sub) {
        HASH_DEL(t->subscribers, sub);
        DECREF(sub, struct subscriber);
    }
}

void topic_put(struct server *server, struct topic *t) {
    trie_insert(&server->topics, t->name, t);
}

void topic_del(struct server *server, const char *name) {
    trie_delete(&server->topics, name);
}

bool topic_exists(const struct server *server, const char *name) {
    struct topic *t = topic_get(server, name);
    return t != NULL;
}

struct topic *topic_get(const struct server *server, const char *name) {
    struct topic *ret_topic;
    trie_find(&server->topics, name, (void *) &ret_topic);
    return ret_topic;
}

struct topic *topic_get_or_create(struct server *server, const char *name) {
    struct topic *t = topic_get(server, name);
    if (!t) {
        t = topic_new(xstrdup(name));
        topic_put(server, t);
    }
    return t;
}

void inflight_msg_init(struct inflight_msg *imsg,
                       struct client *c,
                       struct mqtt_packet *p,
                       size_t size) {
    imsg->seen = time(NULL);
    imsg->size = size;
    imsg->packet = p;
    imsg->client = c;
    imsg->in_use = 1;
    imsg->qos = p->header.bits.qos;
}

void inflight_msg_clear(struct inflight_msg *imsg) {
    DECREF(imsg->packet, struct mqtt_packet);
}

unsigned next_free_mid(struct client_session *session) {
    if (session->next_free_mid == MAX_INFLIGHT_MSGS)
        session->next_free_mid = 1;
    return session->next_free_mid++;
}
