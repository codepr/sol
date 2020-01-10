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
#include <openssl/err.h>
#include "network.h"
#include "mqtt.h"
#include "util.h"
#include "pack.h"
#include "core.h"
#include "config.h"
#include "server.h"
#include "handlers.h"
#include "hashtable.h"
#include "ev.h"

/* Seconds in a Sol, easter egg */
static const double SOL_SECONDS = 88775.24;

/*
 * General informations of the broker, all fields will be published
 * periodically to internal topics
 */
struct sol_info info;

/* Broker global instance, contains the topic trie and the clients hashtable */
struct sol sol;

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

/*
 * Shared eventloop object, contains the IO and Worker contexts, as well as the
 * server descriptor.
 * Each thread will receive a copy of a pointer to this structure, to have
 * access to all file descriptor running the application
 */

struct eventloop {
    int serverfd;
    struct ev_ctx *io_ctx;
    struct ev_ctx *w_ctx;
};

static int client_destructor(struct client *);

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

static const char *sys_topics[SYS_TOPICS] = {
    "$SOL/broker/clients/",
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
 * Parse packet header, it is required at least the Fixed Header of each
 * packed, which is contained in the first 2 bytes in order to read packet
 * type and total length that we need to recv to complete the packet.
 *
 * This function accept a socket fd, a buffer to read incoming streams of
 * bytes and a pointer to the decoded fixed header that will be set in the
 * final parsed packet.
 *
 * - c: A struct connection pointer, contains the FD of the requesting client
 *      as well as his SSL context in case of TLS communication.
 * - buf: A byte buffer, it will be malloc'ed in the function and it will
 *        contain the serialized bytes of the incoming packet
 * - header: Single byte pointer, copy the opcode and flags of the incoming
 *           packet, again for simplicity and convenience of the caller.
 */
static ssize_t recv_packet(struct client *c) {

    ssize_t nread = 0;
    unsigned opcode = 0, pos = 0;
    unsigned long long pktlen = 0LL;

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

    if (c->status == WAITING_LENGTH) {

        if (c->read == 2) {
            opcode = *c->rbuf >> 4;

            /* Check for OPCODE, if an unknown OPCODE is received return an
             * error
             */
            if (DISCONNECT < opcode || CONNECT > opcode)
                return -ERRPACKETERR;

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

        c->status = WAITING_DATA;
    }

    if (c->status == WAITING_DATA) {

        if (c->toread == 0) {
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

            c->rpos = pos + 1;
            c->toread = pktlen + pos + 1;

            if (pktlen <= 4)
                goto exit;
        }

        nread = recv_data(&c->conn, c->rbuf + c->read, c->toread - c->read);

        if (errno != EAGAIN && errno != EWOULDBLOCK && nread <= 0)
            return -ERRCLIENTDC;

        c->read += nread;
        if (errno == EAGAIN && c->read < c->toread)
            return -ERREAGAIN;
    }

exit:

    return 0;
}

/* Handle incoming requests, after being accepted or after a reply */
static int read_data(struct client *c) {

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

static inline int write_data(struct client *c) {
    ssize_t wrote = send_data(&c->conn, c->wbuf+c->wrote, c->towrite-c->wrote);
    if (errno != EAGAIN && errno != EWOULDBLOCK && wrote < 0)
        return -ERRCLIENTDC;
    c->wrote += wrote > 0 ? wrote : 0;
    if (c->wrote < c->towrite && errno == EAGAIN)
        return -ERREAGAIN;
    // Update information stats
    info.bytes_sent += c->towrite;
    c->towrite = c->wrote = 0;
    return 0;
}

static void write_callback(struct ev_ctx *ctx, void *arg) {
    struct client *client = arg;
    int err = write_data(client);
    switch (err) {
        case 0: // OK
            /*
             * Rearm descriptor making it ready to receive input,
             * read_callback will be the callback to be used.
             */
            client->status = WAITING_HEADER;
            ev_fire_event(ctx, client->conn.fd, EV_READ, read_callback, client);
            break;
        case -ERREAGAIN:
            break;
        default:
            log_info("Closing connection with %s (%s): %s %i",
                     client->client_id, client->conn.ip,
                     solerr(client->rc), err);
            ev_del_fd(ctx, client->conn.fd);
            client_destructor(client);
            // Update stats
            info.nclients--;
            info.nconnections--;
            break;
    }
}

/*
 * Publish statistics periodic task, it will be called once every N config
 * defined seconds, it publish some informations on predefined topics
 */
static void publish_stats(struct ev_ctx *ctx, void *data) {
    (void)data;

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

    // $SOL/uptime
    struct mqtt_packet p = {
        .header = (union mqtt_header) { .byte = PUBLISH_B },
        .publish = (struct mqtt_publish) {
            .pkt_id = 0,
            .topiclen = strlen(sys_topics[2]),
            .topic = (unsigned char *) sys_topics[2],
            .payloadlen = strlen(utime),
            .payload = (unsigned char *) &utime
        }
    };

    publish_message(&p, sol_topic_get(&sol, (const char *) p.publish.topic), ctx);

    // $SOL/broker/uptime/sol
    p.publish.topiclen = strlen(sys_topics[3]);
    p.publish.topic = (unsigned char *) sys_topics[3];
    p.publish.payloadlen = strlen(sutime);
    p.publish.payload = (unsigned char *) &sutime;

    publish_message(&p, sol_topic_get(&sol, (const char *) p.publish.topic), ctx);

    // $SOL/broker/clients/connected
    p.publish.topiclen = strlen(sys_topics[4]);
    p.publish.topic = (unsigned char *) sys_topics[4];
    p.publish.payloadlen = strlen(cclients);
    p.publish.payload = (unsigned char *) &cclients;

    publish_message(&p, sol_topic_get(&sol, (const char *) p.publish.topic), ctx);

    // $SOL/broker/bytes/sent
    p.publish.topiclen = strlen(sys_topics[6]);
    p.publish.topic = (unsigned char *) sys_topics[6];
    p.publish.payloadlen = strlen(bsent);
    p.publish.payload = (unsigned char *) &bsent;

    publish_message(&p, sol_topic_get(&sol, (const char *) p.publish.topic), ctx);

    // $SOL/broker/messages/sent
    p.publish.topiclen = strlen(sys_topics[8]);
    p.publish.topic = (unsigned char *) sys_topics[8];
    p.publish.payloadlen = strlen(msent);
    p.publish.payload = (unsigned char *) &msent;

    publish_message(&p, sol_topic_get(&sol, (const char *) p.publish.topic), ctx);

    // $SOL/broker/messages/received
    p.publish.topiclen = strlen(sys_topics[9]);
    p.publish.topic = (unsigned char *) sys_topics[9];
    p.publish.payloadlen = strlen(mrecv);
    p.publish.payload = (unsigned char *) &mrecv;

    publish_message(&p, sol_topic_get(&sol, (const char *) p.publish.topic), ctx);

    // $SOL/broker/memory/used
    p.publish.topiclen = strlen(sys_topics[10]);
    p.publish.topic = (unsigned char *) sys_topics[10];
    p.publish.payloadlen = strlen(mem);
    p.publish.payload = (unsigned char *) &mem;

    publish_message(&p, sol_topic_get(&sol, (const char *) p.publish.topic), ctx);

}

/*
 * Check for infligh messages in the ingoing and outgoing maps (actually
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
    ssize_t sent;
    for (int i = 0; i < sol.maxfd; ++i) {
        struct client *c = &sol.clients[i];
        if (c->online == false)
            continue;
        for (int i = 1; i < MAX_INFLIGHT_MSGS; ++i) {
            // TODO remove 20 hardcoded value
            // Messages
            if (c->i_msgs[i].in_use && (now - c->i_msgs[i].sent_timestamp) > 20) {
                log_debug("Re-sending %s", c->i_msgs[i].client->client_id);
                // Set DUP flag to 1
                mqtt_set_dup(c->i_msgs[i].packet);
                // Serialize the packet and send it out again
                unsigned char pub[c->i_msgs[i].size];
                mqtt_pack(c->i_msgs[i].packet, pub);
                if ((sent = send_data(&c->i_msgs[i].client->conn,
                                      pub, c->i_msgs[i].size)) < 0)
                    log_error("Error re-sending %s", strerror(errno));

                // Update information stats
                info.messages_sent++;
                info.bytes_sent += sent;
            }
            // ACKs
            if (c->i_acks[i].in_use && (now - c->i_acks[i].sent_timestamp) > 20) {
                // Set DUP flag to 1
                mqtt_set_dup(c->i_acks[i].packet);
                // Serialize the packet and send it out again
                unsigned char pub[c->i_acks[i].size];
                mqtt_pack(c->i_acks[i].packet, pub);
                if ((sent = send_data(&c->i_acks[i].client->conn,
                                      pub, c->i_acks[i].size)) < 0)
                    log_error("Error re-sending %s", strerror(errno));

                // Update information stats
                info.messages_sent++;
                info.bytes_sent += sent;
            }
        }
    }
}

/*
 * Cleanup function to be passed in as destructor to the Hashtable for
 * connecting clients
 */
static int client_destructor(struct client *client) {

    if (!client || client->online == false)
        return -1;

    client->rpos = client->toread = client->read = 0;
    client->wrote = client->towrite = 0;
    close_connection(&client->conn);

    if (client->session) {
        list_destroy(client->session->subscriptions, 0);
        xfree(client->session->msg_queue);
        xfree(client->session);
    }

    xfree(client->i_acks);
    xfree(client->i_msgs);
    xfree(client->in_i_acks);

    if (client->lwt_msg)
        xfree(client->lwt_msg);

    client->online = false;
    client->client_id[0] = '\0';
    xfree(client->rbuf);
    xfree(client->wbuf);

    return 0;
}

/*
 * Cleanup function to be passed in as destructor to the Hashtable for client
 * sessions storing
 */
static int session_destructor(struct hashtable_entry *entry) {
    if (!entry)
        return -1;
    xfree((void *) entry->key);
    struct session *s = entry->val;
    if (s->subscriptions)
        list_destroy(s->subscriptions, 1);
    if (s->msg_queue)
        xfree(s->msg_queue);
    xfree(s);
    return 0;
}

/*
 * Cleanup function to be passed in as destructor to the Hashtable for
 * authentication entries
 */
static int auth_destructor(struct hashtable_entry *entry) {

    if (!entry)
        return -1;
    xfree((void *) entry->key);
    xfree(entry->val);

    return 0;
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
        connection_init(&conn, conf->use_ssl ? sol.ssl_ctx : NULL);
        int fd = accept_connection(&conn, serverfd);
        if (fd == 0)
            continue;
        if (fd < 0) {
            close_connection(&conn);
            break;
        }

        // Resize client pool if needed
        if (fd > sol.maxfd) {
            sol.maxfd = fd;
            sol.clients = xrealloc(sol.clients, fd * sizeof(struct client));
        }
        /*
         * Create a client structure to handle his context
         * connection
         */
        client_init(&sol.clients[fd]);
        sol.clients[fd].conn = conn;

        /* Add it to the epoll loop */
        ev_register_event(ctx, fd, EV_READ, read_callback, &sol.clients[fd]);

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
    c->status = SENDING_DATA;
    switch (rc) {
        case 0:
            /*
             * All is ok, raise an event to the worker poll EPOLL and
             * link it with the IO event containing the decode payload
             * ready to be processed
             */
            /* Record last action as of now */
            c->last_action_time = time(NULL);
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
            if (c->lwt_msg) {
                char *tname = (char *) c->lwt_msg->topic;
                struct topic *t = sol_topic_get(&sol, tname);
                struct mqtt_packet lwt = {
                    .header = (union mqtt_header) { .byte = PUBLISH_B },
                    .publish = *c->lwt_msg
                };
                publish_message(&lwt, t, ctx);
            }
            // Clean resources
            ev_del_fd(ctx, c->conn.fd);
            // Remove from subscriptions for now
            if (c->session) {
                struct list *subs = c->session->subscriptions;
                struct iterator *it = iter_new(subs, list_iter_next);
                FOREACH (it) {
                    log_debug("Deleting %s from topic %s",
                              c->client_id, ((struct topic *) it->ptr)->name);
                    topic_del_subscriber(it->ptr, c, false);
                }
                iter_destroy(it);
            }
            client_destructor(c);
            info.nclients--;
            info.nconnections--;
            break;
        case -ERREAGAIN:
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
            mqtt_packet_destroy(&io.data, io.data.header.bits.type);
            break;
        case -ERRCLIENTDC:
            ev_del_fd(ctx, c->conn.fd);
            client_destructor(io.client);
            // Update stats
            info.nclients--;
            info.nconnections--;
            break;
        default:
            c->status = WAITING_HEADER;
            break;
    }
}

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
    ev_register_cron(&ctx, publish_stats, conf->stats_pub_interval, 0);
    ev_register_cron(&ctx, inflight_msg_check, 0, 9e8);
    // Start the loop, blocking call
    ev_run(&ctx);
    ev_destroy(&ctx);
}

void enqueue_event_write(struct ev_ctx *ctx, struct client *c) {
    ev_fire_event(ctx, c->conn.fd, EV_WRITE, write_callback, c);
}

int start_server(const char *addr, const char *port) {

    /* Initialize global Sol instance */
    trie_init(&sol.topics, NULL);
    sol.maxfd = BASE_CLIENTS_NUM - 1;
    sol.clients = xcalloc(BASE_CLIENTS_NUM, sizeof(struct client));
    sol.sessions = hashtable_new(session_destructor);
    sol.authentications = hashtable_new(auth_destructor);

    if (conf->allow_anonymous == false)
        config_read_passwd_file(conf->password_file, sol.authentications);

    /* Generate stats topics */
    for (int i = 0; i < SYS_TOPICS; i++)
        sol_topic_put(&sol, topic_new(xstrdup(sys_topics[i])));

    /* Start listening for new connections */
    int sfd = make_listen(addr, port, conf->socket_family);

    /* Setup SSL in case of flag true */
    if (conf->use_ssl == true) {
        openssl_init();
        sol.ssl_ctx = create_ssl_context();
        load_certificates(sol.ssl_ctx, conf->cafile,
                          conf->certfile, conf->keyfile);
    }

    log_info("Server start");
    info.start_time = time(NULL);

    // start eventloop, could be spread on multiple threads
    eventloop_start(&sfd);

    hashtable_destroy(sol.sessions);
    hashtable_destroy(sol.authentications);
    // free client resources
    for (int i = 0; i < sol.maxfd; ++i)
        client_destructor(&sol.clients[i]);
    xfree(sol.clients);

    /* Destroy SSL context, if any present */
    if (conf->use_ssl == true) {
        SSL_CTX_free(sol.ssl_ctx);
        openssl_cleanup();
    }

    log_info("Sol v%s exiting", VERSION);

    return 0;
}
