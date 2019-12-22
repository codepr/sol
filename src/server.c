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
static void publish_stats(int);

/*
 * Periodic routine to check for incomplete transactions on QoS > 0 to be
 * concluded
 */
static void inflight_msg_check(void);

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

/* Utility macro to handle base case on each EPOLL loop */
#define EPOLL_ERR(e) if ((e.events & EPOLLERR) || (e.events & EPOLLHUP) || \
                         (!(e.events & EPOLLIN) && !(e.events & EPOLLOUT)))

/*
 * Handle incoming connections, create a a fresh new struct client structure
 * and link it to the fd, ready to be set in EPOLLIN event, then pass the
 * connection to the IO EPOLL loop, waited by the IO thread pool.
 */
static void accept_loop(struct epoll *epoll) {
    int events = 0;
    struct epoll_event *e_events =
        xmalloc(sizeof(struct epoll_event) * EPOLL_MAX_EVENTS);
    int epollfd = epoll_create1(0);

    /*
     * We want to watch for events incoming on the server descriptor (e.g. new
     * connections)
     */
    epoll_add(epollfd, epoll->serverfd, EPOLLIN, NULL);

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
                log_debug("Stopping epoll loop. Thread %p exiting.",
                          (void *) pthread_self());
                epoll_mod(epollfd, conf->run, EPOLLIN, NULL);
                goto exit;

            } else if (e_events[i].data.fd == epoll->serverfd) {

                while (1) {

                    /*
                     * Accept a new incoming connection assigning ip address
                     * and socket descriptor to the connection structure
                     * pointer passed as argument
                     */
                    struct connection *conn =
                        conf->use_ssl ? conn_new(sol.ssl_ctx) : conn_new(NULL);
                    int fd = accept_conn(conn, epoll->serverfd);
                    if (fd == 0)
                        continue;
                    if (fd < 0) {
                        close_conn(conn);
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
                    epoll_add(epoll->io_epollfd, fd, EPOLLIN, client);

                    /* Rearm server fd to accept new connections */
                    epoll_mod(epollfd, epoll->serverfd, EPOLLIN, NULL);

                    /* Record the new client connected */
                    info.nclients++;
                    info.nconnections++;

                    log_info("Connection from %s", conn->ip);
                }
            }
        }
    }

exit:

    xfree(e_events);
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
    close_conn(c);

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
static void *io_worker(void *arg) {

    struct epoll *epoll = arg;
    int events = 0;
    ssize_t sent = 0;

    struct epoll_event *e_events =
        xmalloc(sizeof(struct epoll_event) * EPOLL_MAX_EVENTS);

    /* Raw bytes buffer to handle input from client */
    unsigned char *buffer = xmalloc(conf->max_request_size);

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

                log_debug("Stopping epoll loop. Thread %p exiting.",
                          (void *) pthread_self());

                epoll_mod(epoll->io_epollfd, conf->run, EPOLLIN, NULL);
                goto exit;

            } else if (e_events[i].events & EPOLLIN) {

                struct io_event *event = xmalloc(sizeof(*event));
                event->epollfd = epoll->io_epollfd;
                event->rc = 0;
                event->client = e_events[i].data.ptr;
                /*
                 * Received a bunch of data from a client, after the creation
                 * of an IO event we need to read the bytes and encoding the
                 * content according to the protocol
                 */
                struct connection *c = event->client->conn;
                int rc = read_data(c, buffer, &event->data);
                if (rc == 0) {
                    /*
                     * All is ok, raise an event to the worker poll EPOLL and
                     * link it with the IO event containing the decode payload
                     * ready to be processed
                     */
                    eventfd_t ev = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
                    event->eventfd = ev;
                    epoll_add(epoll->w_epollfd, ev, EPOLLIN, event);

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
                    pthread_spin_lock(&io_spinlock);
                    log_error("Closing connection with %s: %s",
                              event->client->conn->ip, solerr(rc));
                    // Publish, if present, LWT message
                    if (event->client->lwt_msg) {
                        char *tname = (char *) event->client->lwt_msg->topic;
                        struct topic *t = sol_topic_get(&sol, tname);
                        publish_message(event->client->lwt_msg, t, event->epollfd);
                    }
                    event->client->online = false;
                    // Clean resources
                    close_conn(event->client->conn);
                    // Remove from subscriptions for now
                    if (event->client->session) {
                        struct list *subs = event->client->session->subscriptions;
                        struct iterator *it = iter_new(subs, list_iter_next);
                        FOREACH (it) {
                            log_debug("Deleting %s from topic %s",
                                      event->client->client_id,
                                      ((struct topic *) it->ptr)->name);
                            topic_del_subscriber(it->ptr, event->client, false);
                        }
                        iter_destroy(it);
                    }
                    info.nclients--;
                    info.nconnections--;
                    xfree(event);
                    pthread_spin_unlock(&io_spinlock);
                }
            } else if (e_events[i].events & EPOLLOUT) {

                pthread_spin_lock(&io_spinlock);
                struct io_event *event = e_events[i].data.ptr;

                /*
                 * Write out to client, after a request has been processed in
                 * worker thread routine. Just send out all bytes stored in the
                 * reply buffer to the reply file descriptor.
                 */
                struct connection *c = event->client->conn;
                sent = send_data(c, event->reply, bstring_len(event->reply));
                if (sent <= 0 || event->rc == RC_NOT_AUTHORIZED
                    || event->rc == RC_BAD_USERNAME_OR_PASSWORD) {
                    log_info("Closing connection with %s: %s %lu",
                             c->ip, solerr(event->rc), sent);
                    close_conn(c);
                    xfree(event->client->conn);
                    xfree(event->client);
                    // Update stats
                    info.nclients--;
                    info.nconnections--;
                } else {
                    /*
                     * Rearm descriptor, we're using EPOLLONESHOT feature to
                     * avoid race condition and thundering herd issues on
                     * multithreaded EPOLL
                     */
                    epoll_mod(epoll->io_epollfd,
                              c->fd, EPOLLIN, event->client);
                }

                // Update information stats
                info.bytes_sent += sent < 0 ? 0 : sent;

                /* Free resource, ACKs will be free'd closing the server */
                bstring_destroy(event->reply);
                mqtt_packet_release(&event->data, event->data.header.bits.type);
                xfree(event);
                pthread_spin_unlock(&io_spinlock);
            }
        }
    }

exit:

    xfree(e_events);
    xfree(buffer);

    return NULL;
}

/*
 * Worker function, like io_worker, wait for events on a dedicated epoll
 * descriptor, shared among multiple threads, awaits for events incoming from
 * the IO workers to be processed and schedule them back to the IO side for
 * the answer.
 */
static void *worker(void *arg) {

    struct epoll *epoll = arg;
    int events = 0;
    long int timers = 0;
    eventfd_t val;

    struct epoll_event *e_events =
        xmalloc(sizeof(struct epoll_event) * EPOLL_MAX_EVENTS);

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

                log_debug("Stopping epoll loop. Thread %p exiting.",
                          (void *) pthread_self());

                epoll_mod(epoll->w_epollfd, conf->run, EPOLLIN, NULL);
                goto exit;

            } else if (e_events[i].data.fd == epoll->timerfd[0]) {
                (void) read(e_events[i].data.fd, &timers, sizeof(timers));
                // Check for keys about to expire out
                publish_stats(epoll->io_epollfd);
                epoll_mod(epoll->w_epollfd, e_events[i].data.fd, EPOLLIN, NULL);
            } else if (e_events[i].data.fd == epoll->timerfd[1]) {
                (void) read(e_events[i].data.fd, &timers, sizeof(timers));
                // Check for keys about to expire out
                inflight_msg_check();
                epoll_mod(epoll->w_epollfd, e_events[i].data.fd, EPOLLIN, NULL);
            } else if (e_events[i].events & EPOLLIN) {
                struct io_event *event = e_events[i].data.ptr;
                eventfd_read(event->eventfd, &val);
                // TODO free client and remove it from the global map in case
                // of QUIT command (check return code)
                struct connection *c = event->client->conn;
                int reply = handle_command(event->data.header.bits.type, event);
                if (reply == REPLY) {
                    if (close(event->eventfd) < 0)
                        perror("reply - eventfd close");
                    epoll_mod(event->epollfd, c->fd, EPOLLOUT, event);
                } else if (reply == RC_BAD_USERNAME_OR_PASSWORD
                           || reply == RC_NOT_AUTHORIZED) {
                    event->rc = reply;
                    epoll_mod(event->epollfd, c->fd, EPOLLOUT, event);
                } else if (reply == CLIENTDC) {
                    event->client->online = false;
                    close_conn(event->client->conn);
                    hashtable_del(sol.clients, event->client->client_id);
                    if (close(event->eventfd) < 0)
                        perror("DISCONNECT close");
                    xfree(event);
                    // Update stats
                    info.nclients--;
                    info.nconnections--;
                } else if (reply != CLIENTDC) {
                    epoll_mod(epoll->io_epollfd, c->fd, EPOLLIN, event->client);
                    if (close(event->eventfd) < 0)
                        perror("noreply - eventfd close");
                    xfree(event);
                }
            }
        }
    }

exit:

    xfree(e_events);

    return NULL;
}

/*
 * Publish statistics periodic task, it will be called once every N config
 * defined seconds, it publish some informations on predefined topics
 */
static void publish_stats(int epollfd) {

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

    struct mqtt_publish p = {
        .header = (union mqtt_header) { .byte = PUBLISH_B },
        .pkt_id = 0,
        .topiclen = strlen(sys_topics[2]),
        .topic = (unsigned char *) sys_topics[2],
        .payloadlen = strlen(utime),
        .payload = (unsigned char *) &utime
    };

    publish_message(&p, sol_topic_get(&sol, (const char *) p.topic), epollfd);

    p.topiclen = strlen(sys_topics[3]);
    p.topic = (unsigned char *) sys_topics[3];
    p.payloadlen = strlen(sutime);
    p.payload = (unsigned char *) &sutime;

    publish_message(&p, sol_topic_get(&sol, (const char *) p.topic), epollfd);

    p.topiclen = strlen(sys_topics[4]);
    p.topic = (unsigned char *) sys_topics[4];
    p.payloadlen = strlen(cclients);
    p.payload = (unsigned char *) &cclients;

    publish_message(&p, sol_topic_get(&sol, (const char *) p.topic), epollfd);

    p.topiclen = strlen(sys_topics[6]);
    p.topic = (unsigned char *) sys_topics[6];
    p.payloadlen = strlen(bsent);
    p.payload = (unsigned char *) &bsent;

    publish_message(&p, sol_topic_get(&sol, (const char *) p.topic), epollfd);

    p.topiclen = strlen(sys_topics[8]);
    p.topic = (unsigned char *) sys_topics[8];
    p.payloadlen = strlen(msent);
    p.payload = (unsigned char *) &msent;

    publish_message(&p, sol_topic_get(&sol, (const char *) p.topic), epollfd);

    p.topiclen = strlen(sys_topics[9]);
    p.topic = (unsigned char *) sys_topics[9];
    p.payloadlen = strlen(mrecv);
    p.payload = (unsigned char *) &mrecv;

    publish_message(&p, sol_topic_get(&sol, (const char *) p.topic), epollfd);

    p.topiclen = strlen(sys_topics[10]);
    p.topic = (unsigned char *) sys_topics[10];
    p.payloadlen = strlen(mem);
    p.payload = (unsigned char *) &mem;

    publish_message(&p, sol_topic_get(&sol, (const char *) p.topic), epollfd);

}

/*
 * Check for infligh messages in the ingoing and outgoing maps (actually
 * arrays), each position between 0-65535 contains either NULL or a pointer
 * to an inflight stucture with a timestamp of the sending action, the
 * target file descriptor (e.g. the client) and the payload to be sent
 * unserialized, this way it's possible to set the DUP flag easily at the cost
 * of additional packing before re-sending it out
 */
static void inflight_msg_check(void) {
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
            close_conn(client->conn);
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

/*
 * Helper function, return an itimerspec structure for creating custom timer
 * events to be triggered after being registered in an EPOLL loop
 */
static struct itimerspec get_timer(int sec, unsigned long ns) {
    struct itimerspec timer;
    memset(&timer, 0x00, sizeof(timer));
    timer.it_value.tv_sec = sec;
    timer.it_value.tv_nsec = ns;
    timer.it_interval.tv_sec = sec;
    timer.it_interval.tv_nsec = ns;
    return timer;
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

    struct epoll epoll = {
        .io_epollfd = epoll_create1(0),
        .w_epollfd = epoll_create1(0),
        .serverfd = sfd
    };

    /* Start the expiration keys check routine */
    struct itimerspec exp_keys_timer = get_timer(conf->stats_pub_interval, 0);

    /* And one for the inflight ingoing and outgoing messages with QoS > 0 */
    struct itimerspec inflight_msgs_timer = get_timer(0, 2e8);

    // add expiration keys cron task and inflight messages cron task
    int exptimerfd = add_cron_task(epoll.w_epollfd, &exp_keys_timer);
    int inflightfd = add_cron_task(epoll.w_epollfd, &inflight_msgs_timer);

    epoll.timerfd[0] = exptimerfd;
    epoll.timerfd[1] = inflightfd;

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

    log_info("Server start");
    info.start_time = time(NULL);

    // Main thread for accept new connections
    accept_loop(&epoll);

    // Stop expire keys check routine
    epoll_del(epoll.w_epollfd, epoll.timerfd[0]);

    // Stop inflight messages timer fd
    epoll_del(epoll.w_epollfd, epoll.timerfd[1]);

    /* Join started thread pools */
    for (int i = 0; i < IOPOOLSIZE; ++i)
        pthread_join(iothreads[i], NULL);

    for (int i = 0; i < WORKERPOOLSIZE; ++i)
        pthread_join(workers[i], NULL);

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
