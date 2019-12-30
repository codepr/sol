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
#include "eventloop.h"

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
 * Guards the access to the main database structure, the trie underlying the
 * topic DB and all the hashtable/lists involved
 */
pthread_spinlock_t w_spinlock;

/* Guards the EPOLL event changing between different threads */
pthread_spinlock_t io_spinlock;

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
 *    [EV_CTX]        [IO EV_CTX]         [WORK EV_CTX]
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

/* Periodic routine to publish general stats about the broker on $SOL topics */
static void publish_stats(struct ev_ctx *);

/*
 * Periodic routine to check for incomplete transactions on QoS > 0 to be
 * concluded
 */
static void inflight_msg_check(struct ev_ctx *);

/* Simple error_code to string function, to be refined */
static const char *solerr(int rc) {
    switch (rc) {
        case -ERRCLIENTDC:
            return "Client disconnected";
        case -ERRPACKETERR:
            return "Error reading packet";
        case -ERRMAXREQSIZE:
            return "Packet sent exceeds max size accepted";
        case RC_UNACCEPTABLE_PROTOCOL_VERSION:
            return "[MQTT] Unknown protocol version";
        case RC_IDENTIFIER_REJECTED:
            return "[MQTT] Wrong identifier";
        case RC_SERVER_UNAVAILABLE:
            return "[MQTT] Server unavailable";
        case RC_BAD_USERNAME_OR_PASSWORD:
            return "[MQTT] Bad username or password";
        case RC_NOT_AUTHORIZED:
            return "[MQTT] Not authorized";
        default:
            return "Unknown error";
    }
}

/*
 * Handle incoming connections, create a a fresh new struct client structure
 * and link it to the fd, ready to be set in EPOLLIN event, then pass the
 * connection to the IO EPOLL loop, waited by the IO thread pool.
 */
static void accept_loop(struct eventloop *loop) {

    int event = 0, events = 0;
    struct ev_ctx ctx;
    ev_init(&ctx, EPOLL_MAX_EVENTS);

    /*
     * We want to watch for events incoming on the server descriptor (e.g. new
     * connections)
     */
    ev_watch_fd(&ctx, loop->serverfd, EV_READ);

    /*
     * And also to the global event fd, this one is useful to gracefully
     * interrupt polling and thread execution
     */
    ev_watch_fd(&ctx, conf->run, EV_READ | EV_CLOSEFD);

    while (1) {
        events = ev_poll(&ctx, -1);
        if (events < 0) {
            /* Signals to all threads. Ignore it for now */
            if (errno == EINTR)
                continue;
            /* Error occured, break the loop */
            break;
        }
        for (int i = 0; i < events; ++i) {
            event = ev_get_event_type(&ctx, i);
            if (event & EV_CLOSEFD) {
                /* And quit event after that */
                log_debug("Stopping accept loop. Thread %p exiting.",
                          (void *) pthread_self());
                ev_read_event(&ctx, i, EV_CLOSEFD, NULL);
                goto exit;

            } else if ((event & EV_READ) &&
                       ev_get_fd(&ctx, i) == loop->serverfd) {

                while (1) {

                    /*
                     * Accept a new incoming connection assigning ip address
                     * and socket descriptor to the connection structure
                     * pointer passed as argument
                     */
                    struct connection *conn = conf->use_ssl ? \
                                              connection_new(sol.ssl_ctx) : \
                                              connection_new(NULL);
                    int fd = accept_connection(conn, loop->serverfd);
                    if (fd == 0)
                        continue;
                    if (fd < 0) {
                        close_connection(conn);
                        xfree(conn);
                        break;
                    }
                    /*
                     * Create a client structure to handle his context
                     * connection
                     */
                    struct client *client = sol_client_new(conn);
                    if (!conn || !client)
                        return;

                    /* Add it to the epoll loop */
                    ev_register_event(loop->io_ctx, fd, EV_READ, client);

                    /* Rearm server fd to accept new connections */
                    ev_fire_event(&ctx, loop->serverfd, EV_READ, NULL);

                    /* Record the new client connected */
                    info.nclients++;
                    info.nconnections++;

                    log_info("Connection from %s", conn->ip);
                }
            }
        }
    }

exit:

    ev_destroy(&ctx);
    return;
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
static ssize_t recv_packet(struct connection *c, unsigned char **buf,
                           unsigned char *header) {

    ssize_t ret = 0;
    unsigned char *bufptr = *buf;

    /*
     * Read the first two bytes, the first should contain the message type
     * code
     */
    ret = recv_data(c, *buf, 2);

    if (ret <= 0)
        return -ERRCLIENTDC;

    *header = *bufptr;
    bufptr++;
    unsigned opcode = *header >> 4;
    unsigned pos = 0;

    /* Check for OPCODE, if an unknown OPCODE is received return an error */
    if (DISCONNECT < opcode || CONNECT > opcode)
        return -ERRPACKETERR;

    if (opcode > UNSUBSCRIBE)
        goto exit;

    /*
     * Read 2 extra bytes, because the first 4 bytes could countain the total
     * size in bytes of the entire packet
     */
    ret += recv_data(c, *buf + 2, 2);

    /*
     * Read remaning length bytes which starts at byte 2 and can be long to 4
     * bytes based on the size stored, so byte 2-5 is dedicated to the packet
     * length.
     */
    ssize_t n = 0;
    unsigned long long tlen = mqtt_decode_length(&bufptr, &pos);

    /*
     * Set return code to -ERRMAXREQSIZE in case the total packet len exceeds
     * the configuration limit `max_request_size`
     */
    if (tlen > conf->max_request_size) {
        ret = -ERRMAXREQSIZE;
        goto exit;
    }

    if (tlen <= 4)
        goto exit;

    ssize_t offset = 4 - pos -1;

    unsigned long long remaining_bytes = tlen - offset;

    /* Read remaining bytes to complete the packet */
    while (remaining_bytes > 0) {
        n = recv_data(c, bufptr + offset, remaining_bytes);
        if (n < 0)
            goto err;
        remaining_bytes -= n;
        ret += n;
        offset += n;
    }

    ret -= (pos + 1);

exit:

    *buf += pos + 1;

    return ret;

err:

    // TODO move this out of the function (LWT handling)
    close_connection(c);

    return ret;

}

/* Handle incoming requests, after being accepted or after a reply */
static int read_data(struct connection *c, unsigned char *buf,
                     union mqtt_packet *pkt) {

    ssize_t bytes = 0;
    unsigned char header = 0;

    /*
     * We must read all incoming bytes till an entire packet is received. This
     * is achieved by following the MQTT protocol specifications, which
     * send the size of the remaining packet as the second byte. By knowing it
     * we know if the packet is ready to be deserialized and used.
     */
    bytes = recv_packet(c, &buf, &header);

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
     * Unpack received bytes into a mqtt_packet structure and execute the
     * correct handler based on the type of the operation.
     */
    unpack_mqtt_packet(buf, pkt, header, bytes);

    return 0;

    // Disconnect packet received

exit:

    return -ERRPACKETERR;

errdc:

    return -ERRCLIENTDC;
}

/*
 * IO worker function, wait for events on a dedicated epoll descriptor which
 * is shared among multiple threads for input and output only, following the
 * normal EPOLL semantic, EPOLLIN for incoming bytes to be unpacked and
 * processed by a worker thread, EPOLLOUT for bytes incoming from a worker
 * thread, ready to be delivered out.
 */
static void *io_thread(void *arg) {

    struct eventloop *loop = arg;
    int event = 0, events = 0;
    ssize_t sent = 0;

    struct ev_ctx ctx;
    ev_clone_ctx(&ctx, loop->io_ctx);
    /*
     * And also to the global event fd, this one is useful to gracefully
     * interrupt polling and thread execution
     */
    ev_watch_fd(&ctx, conf->run, EV_READ | EV_CLOSEFD);

    ev_register_cron(&ctx, publish_stats, conf->stats_pub_interval, 0);

    /* Raw bytes buffer to handle input from client */
    unsigned char *buffer = xmalloc(conf->max_request_size);

    while (1) {
        events = ev_poll(&ctx, -1);
        if (events < 0) {
            /* Signals to all threads. Ignore it for now */
            if (errno == EINTR)
                continue;
            /* Error occured, break the loop */
            break;
        }
        for (int i = 0; i < events; ++i) {
            event = ev_get_event_type(&ctx, i);
            if (event & EV_CLOSEFD) {
                /* And quit event after that */
                log_debug("Stopping IO epoll loop. Thread %p exiting.",
                          (void *) pthread_self());
                ev_read_event(&ctx, i, EV_CLOSEFD, NULL);
                goto exit;
            } else if (event & EV_TIMERFD) {
                ev_read_event(&ctx, i, EV_TIMERFD, NULL);
            } else if (event & EV_READ) {

                /* pthread_spin_lock(&io_spinlock); */
                struct io_event *io = xmalloc(sizeof(*io));
                io->ctx = &ctx;
                io->rc = 0;
                ev_read_event(&ctx, i, EV_NONE, (void **) &io->client);
                /*
                 * Received a bunch of data from a client, after the creation
                 * of an IO event we need to read the bytes and encoding the
                 * content according to the protocol
                 */
                struct connection *c = io->client->conn;
                int rc = read_data(c, buffer, &io->data);
                if (rc == 0) {
                    /*
                     * All is ok, raise an event to the worker poll EPOLL and
                     * link it with the IO event containing the decode payload
                     * ready to be processed
                     */
                    /* Record last action as of now */
                    io->client->last_action_time = time(NULL);
                    /* Fire an event toward the worker thread pool */
                    eventfd_t ev = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
                    ev_fire_event(loop->w_ctx, ev, EV_READ | EV_EVENTFD, io);

                } else if (rc == -ERRCLIENTDC || rc == -ERRPACKETERR) {

                    /*
                     * We got an unexpected error or a disconnection from the
                     * client side, remove client from the global map and
                     * free resources allocated such as io_event structure and
                     * paired payload
                     */
                    log_error("Closing connection with %s: %s",
                              io->client->conn->ip, solerr(rc));
                    // Publish, if present, LWT message
                    if (io->client->lwt_msg) {
                        char *tname = (char *) io->client->lwt_msg->topic;
                        struct topic *t = sol_topic_get(&sol, tname);
                        publish_message(io->client->lwt_msg, t, io->ctx);
                    }
                    io->client->online = false;
                    // Clean resources
                    ev_del_fd(&ctx, c->fd);
                    close_connection(io->client->conn);
                    // Remove from subscriptions for now
                    if (io->client->session) {
                        struct list *subs = io->client->session->subscriptions;
                        struct iterator *it = iter_new(subs, list_iter_next);
                        FOREACH (it) {
                            log_debug("Deleting %s from topic %s",
                                      io->client->client_id,
                                      ((struct topic *) it->ptr)->name);
                            topic_del_subscriber(it->ptr, io->client, false);
                        }
                        iter_destroy(it);
                    }
                    info.nclients--;
                    info.nconnections--;
                    xfree(io);
                }
                /* pthread_spin_unlock(&io_spinlock); */
            } else if (event & EV_WRITE) {
                /* pthread_spin_lock(&io_spinlock); */
                void *ptr = NULL;
                ev_read_event(&ctx, i, EV_NONE, &ptr);
                struct io_event *io = ptr;

                /*
                 * Write out to client, after a request has been processed in
                 * worker thread routine. Just send out all bytes stored in the
                 * reply buffer to the reply file descriptor.
                 */
                struct connection *c = io->client->conn;
                sent = send_data(c, io->reply, bstring_len(io->reply));
                if (sent <= 0 || io->rc == RC_NOT_AUTHORIZED
                    || io->rc == RC_BAD_USERNAME_OR_PASSWORD) {
                    log_info("Closing connection with %s: %s %lu",
                             c->ip, solerr(io->rc), sent);
                    hashtable_del(sol.clients, io->client->client_id);
                    // Update stats
                    info.nclients--;
                    info.nconnections--;
                } else {
                    /*
                     * Rearm descriptor, we're using EPOLLONESHOT feature to
                     * avoid race condition and thundering herd issues on
                     * multithreaded EPOLL
                     */
                    ev_fire_event(&ctx, c->fd, EV_READ, io->client);
                }

                // Update information stats
                info.bytes_sent += sent < 0 ? 0 : sent;

                /* Free resource, ACKs will be free'd closing the server */
                bstring_destroy(io->reply);
                mqtt_packet_release(&io->data, io->data.header.bits.type);
                xfree(io);
                /* pthread_spin_unlock(&io_spinlock); */
            }
        }
    }

exit:

    xfree(buffer);
    ev_destroy(&ctx);

    return NULL;
}

/*
 * Worker function, like io_worker, wait for events on a dedicated epoll
 * descriptor, shared among multiple threads, awaits for events incoming from
 * the IO workers to be processed and schedule them back to the IO side for
 * the answer.
 */
static void *worker_thread(void *arg) {

    struct eventloop *loop = arg;
    int event = 0, events = 0;
    struct ev_ctx ctx;
    ev_clone_ctx(&ctx, loop->w_ctx);
    /*
     * And also to the global event fd, this one is useful to gracefully
     * interrupt polling and thread execution
     */
    ev_watch_fd(&ctx, conf->run, EV_READ | EV_CLOSEFD);

    ev_register_cron(&ctx, inflight_msg_check, 0, 2e8);

    while (1) {
        events = ev_poll(&ctx, -1);
        if (events < 0) {
            /* Signals to all threads. Ignore it for now */
            if (errno == EINTR)
                continue;
            /* Error occured, break the loop */
            break;
        }
        for (int i = 0; i < events; ++i) {
            event = ev_get_event_type(&ctx, i);
            if (event & EV_CLOSEFD) {
                /* And quit event after that */
                log_debug("Stopping worker epoll loop. Thread %p exiting.",
                          (void *) pthread_self());
                ev_read_event(&ctx, i, EV_CLOSEFD, NULL);
                goto exit;
            } else if (event & EV_TIMERFD) {
                ev_read_event(&ctx, i, EV_TIMERFD, NULL);
            } else if (event & EV_READ) {
                void *ptr = NULL;
                ev_read_event(&ctx, i, EV_EVENTFD, &ptr);
                struct io_event *io = ptr;
                struct connection *c = io->client->conn;
                io->rc = handle_command(io->data.header.bits.type, io);
                /* pthread_spin_lock(&io_spinlock); */
                switch (io->rc) {
                    case REPLY:
                    case RC_NOT_AUTHORIZED:
                    case RC_BAD_USERNAME_OR_PASSWORD:
                        ev_fire_event(io->ctx, c->fd, EV_WRITE, io);
                        break;
                    case CLIENTDC:
                        /* pthread_spin_lock(&io_spinlock); */
                        io->client->online = false;
                        ev_del_fd(loop->io_ctx, c->fd);
                        close_connection(io->client->conn);
                        hashtable_del(sol.clients, io->client->client_id);
                        xfree(io);
                        // Update stats
                        info.nclients--;
                        info.nconnections--;
                        /* pthread_spin_unlock(&io_spinlock); */
                        break;
                    default:
                        ev_fire_event(io->ctx, c->fd, EV_READ, io->client);
                        xfree(io);
                        break;
                }
                /* pthread_spin_unlock(&io_spinlock); */
            }
        }
    }

exit:

    ev_destroy(&ctx);
    return NULL;
}

/*
 * Publish statistics periodic task, it will be called once every N config
 * defined seconds, it publish some informations on predefined topics
 */
static void publish_stats(struct ev_ctx *ctx) {

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
    struct mqtt_publish p = {
        .header = (union mqtt_header) { .byte = PUBLISH_B },
        .pkt_id = 0,
        .topiclen = strlen(sys_topics[2]),
        .topic = (unsigned char *) sys_topics[2],
        .payloadlen = strlen(utime),
        .payload = (unsigned char *) &utime
    };

    publish_message(&p, sol_topic_get(&sol, (const char *) p.topic), ctx);

    // $SOL/broker/uptime/sol
    p.topiclen = strlen(sys_topics[3]);
    p.topic = (unsigned char *) sys_topics[3];
    p.payloadlen = strlen(sutime);
    p.payload = (unsigned char *) &sutime;

    publish_message(&p, sol_topic_get(&sol, (const char *) p.topic), ctx);

    // $SOL/broker/clients/connected
    p.topiclen = strlen(sys_topics[4]);
    p.topic = (unsigned char *) sys_topics[4];
    p.payloadlen = strlen(cclients);
    p.payload = (unsigned char *) &cclients;

    publish_message(&p, sol_topic_get(&sol, (const char *) p.topic), ctx);

    // $SOL/broker/bytes/sent
    p.topiclen = strlen(sys_topics[6]);
    p.topic = (unsigned char *) sys_topics[6];
    p.payloadlen = strlen(bsent);
    p.payload = (unsigned char *) &bsent;

    publish_message(&p, sol_topic_get(&sol, (const char *) p.topic), ctx);

    // $SOL/broker/messages/sent
    p.topiclen = strlen(sys_topics[8]);
    p.topic = (unsigned char *) sys_topics[8];
    p.payloadlen = strlen(msent);
    p.payload = (unsigned char *) &msent;

    publish_message(&p, sol_topic_get(&sol, (const char *) p.topic), ctx);

    // $SOL/broker/messages/received
    p.topiclen = strlen(sys_topics[9]);
    p.topic = (unsigned char *) sys_topics[9];
    p.payloadlen = strlen(mrecv);
    p.payload = (unsigned char *) &mrecv;

    publish_message(&p, sol_topic_get(&sol, (const char *) p.topic), ctx);

    // $SOL/broker/memory/used
    p.topiclen = strlen(sys_topics[10]);
    p.topic = (unsigned char *) sys_topics[10];
    p.payloadlen = strlen(mem);
    p.payload = (unsigned char *) &mem;

    publish_message(&p, sol_topic_get(&sol, (const char *) p.topic), ctx);

}

/*
 * Check for infligh messages in the ingoing and outgoing maps (actually
 * arrays), each position between 0-65535 contains either NULL or a pointer
 * to an inflight stucture with a timestamp of the sending action, the
 * target file descriptor (e.g. the client) and the payload to be sent
 * unserialized, this way it's possible to set the DUP flag easily at the cost
 * of additional packing before re-sending it out
 */
static void inflight_msg_check(struct ev_ctx *ctx) {
    (void)ctx;
    time_t now = time(NULL);
    unsigned char *pub = NULL;
    ssize_t sent;
    pthread_spin_lock(&w_spinlock);
    struct iterator *it = iter_new(sol.clients, hashtable_iter_next);
    FOREACH (it) {
        struct client *c = it->ptr;
        for (int i = 1; i < MAX_INFLIGHT_MSGS; ++i) {
            // TODO remove 20 hardcoded value
            // Messages
            if (c->i_msgs[i] && (now - c->i_msgs[i]->sent_timestamp) > 20) {
                // Set DUP flag to 1
                mqtt_set_dup(c->i_msgs[i]->packet, c->i_msgs[i]->type);
                // Serialize the packet and send it out again
                pub = pack_mqtt_packet(c->i_msgs[i]->packet, c->i_msgs[i]->type);
                bstring payload = bstring_copy(pub, c->i_msgs[i]->size);
                if ((sent = send_data(c->i_msgs[i]->client->conn,
                                      payload, bstring_len(payload))) < 0)
                    log_error("Error re-sending %s", strerror(errno));

                // Update information stats
                info.messages_sent++;
                info.bytes_sent += sent;
            }
            // ACKs
            if (c->i_acks[i] && (now - c->i_acks[i]->sent_timestamp) > 20) {
                // Set DUP flag to 1
                mqtt_set_dup(c->i_acks[i]->packet, c->i_acks[i]->type);
                // Serialize the packet and send it out again
                pub = pack_mqtt_packet(c->i_acks[i]->packet, c->i_acks[i]->type);
                bstring payload = bstring_copy(pub, c->i_acks[i]->size);
                if ((sent = send_data(c->i_acks[i]->client->conn,
                                      payload, bstring_len(payload))) < 0)
                    log_error("Error re-sending %s", strerror(errno));

                // Update information stats
                info.messages_sent++;
                info.bytes_sent += sent;
            }
        }
    }
    iter_destroy(it);
    pthread_spin_unlock(&w_spinlock);
}

/*
 * Cleanup function to be passed in as destructor to the Hashtable for
 * connecting clients
 */
static int client_destructor(struct hashtable_entry *entry) {

    if (!entry)
        return -1;

    struct client *client = entry->val;

    if (client->conn) {
        if (client->online == true)
            close_connection(client->conn);
        xfree(client->conn);
    }

    if (client->session) {
        list_destroy(client->session->subscriptions, 0);
        xfree(client->session->msg_queue);
        xfree(client->session);
    }

    for (int i = 0; i < MAX_INFLIGHT_MSGS; ++i) {
        if (client->i_acks[i])
            xfree(client->i_acks[i]);
        if (client->i_msgs[i])
            xfree(client->i_msgs[i]);
        if (client->in_i_acks[i])
            xfree(client->in_i_acks[i]);
    }

    if (client->lwt_msg)
        xfree(client->lwt_msg);

    xfree(client);

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

int start_server(const char *addr, const char *port) {

    /* Initialize global Sol instance */
    trie_init(&sol.topics, NULL);
    sol.clients = hashtable_new(client_destructor);
    sol.sessions = hashtable_new(session_destructor);
    sol.authentications = hashtable_new(auth_destructor);

    if (conf->allow_anonymous == false)
        config_read_passwd_file(conf->password_file, sol.authentications);

    pthread_spin_init(&w_spinlock, PTHREAD_PROCESS_SHARED);
    pthread_spin_init(&io_spinlock, PTHREAD_PROCESS_SHARED);

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

    struct ev_ctx io_ctx;
    struct ev_ctx w_ctx;

    ev_init(&io_ctx, EPOLL_MAX_EVENTS);
    ev_init(&w_ctx, EPOLL_MAX_EVENTS);

    struct eventloop loop = {
        .io_ctx = &io_ctx,
        .w_ctx = &w_ctx,
        .serverfd = sfd
    };

    pthread_t iothreads[IOPOOLSIZE];
    pthread_t workers[WORKERPOOLSIZE];

    /* Start I/O thread pool */

    for (int i = 0; i < IOPOOLSIZE; ++i)
        pthread_create(&iothreads[i], NULL, &io_thread, &loop);

    /* Start Worker thread pool */

    for (int i = 0; i < WORKERPOOLSIZE; ++i)
        pthread_create(&workers[i], NULL, &worker_thread, &loop);

    log_info("Server start");
    info.start_time = time(NULL);

    // Main thread for accept new connections
    accept_loop(&loop);

    /* Join started thread pools */
    for (int i = 0; i < IOPOOLSIZE; ++i)
        pthread_join(iothreads[i], NULL);

    for (int i = 0; i < WORKERPOOLSIZE; ++i)
        pthread_join(workers[i], NULL);

    ev_destroy(&w_ctx);
    ev_destroy(&io_ctx);
    hashtable_destroy(sol.clients);
    hashtable_destroy(sol.sessions);
    hashtable_destroy(sol.authentications);

    /* Destroy SSL context, if any present */
    if (conf->use_ssl == true) {
        SSL_CTX_free(sol.ssl_ctx);
        openssl_cleanup();
    }

    pthread_spin_destroy(&w_spinlock);
    pthread_spin_destroy(&io_spinlock);

    log_info("Sol v%s exiting", VERSION);

    return 0;
}
