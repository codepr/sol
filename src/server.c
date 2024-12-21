/* BSD 2-Clause License
 *
 * Copyright (c) 2025, Andrea Giacomo Baldan All rights reserved.
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

#include "server.h"
#include "config.h"
#include "ev.h"
#include "handlers.h"
#include "logging.h"
#include "memory.h"
#include "memorypool.h"
#include "network.h"
#include "sol_internal.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>

#define BUFSIZE 512000

/*
 * Auxiliary structure to be used as init argument for eventloop, fd is the
 * listening socket we want to share between multiple instances, cronjobs is
 * just a flag to signal if we want to register cronjobs on that particular
 * instance or not (to not repeat useless cron jobs on multiple threads)
 */
struct listen_payload {
    int fd;
    bool cronjobs;
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

static void connection_ctx_init(Connection_Context *);

static void connection_ctx_deactivate(Connection_Context *);

// CALLBACKS for the eventloop
static void accept_callback(struct ev_ctx *, void *);

static void recv_callback(struct ev_ctx *, void *);

static void send_callback(struct ev_ctx *, void *);

/*
 * Processing message function, will be applied on fully formed mqtt packet
 * received on read_callback callback
 */
// static void process_message(struct ev_ctx *, struct client *);

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
    {"$SOL/broker/clients/", 20},
    {"$SOL/broker/messages/", 21},
    {"$SOL/broker/uptime/", 19},
    {"$SOL/broker/uptime/sol", 22},
    {"$SOL/broker/clients/connected/", 30},
    {"$SOL/broker/clients/disconnected/", 34},
    {"$SOL/broker/bytes/sent/", 23},
    {"$SOL/broker/bytes/received/", 27},
    {"$SOL/broker/messages/sent/", 26},
    {"$SOL/broker/messages/received/", 30},
    {"$SOL/broker/memory/used", 23}};

/* Simple error_code to string function, to be refined */
static const char *solerr(int rc)
{
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
    case MQTT_BAD_CREDENTIALS:
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
 * defined seconds, it publishes some information on predefined topics
 */
static void publish_stats(struct ev_ctx *ctx, void *data)
{
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
    struct mqtt_packet p = {.header  = (union mqtt_header){.byte = PUBLISH_B},
                            .publish = (struct mqtt_publish){
                                .id       = 0,
                                .topiclen = sys_topics[2].len,
                                .topic    = (unsigned char *)sys_topics[2].name,
                                .payloadlen = strlen(utime),
                                .payload    = (unsigned char *)&utime}};

    publish_message(&p, topic_repo_fetch(server.repo, sys_topics[2].name));

    // $SOL/broker/uptime/sol
    p.publish.topiclen   = sys_topics[3].len;
    p.publish.topic      = (unsigned char *)sys_topics[3].name;
    p.publish.payloadlen = strlen(sutime);
    p.publish.payload    = (unsigned char *)&sutime;

    publish_message(&p, topic_repo_fetch(server.repo, sys_topics[3].name));

    // $SOL/broker/clients/connected
    p.publish.topiclen   = sys_topics[4].len;
    p.publish.topic      = (unsigned char *)sys_topics[4].name;
    p.publish.payloadlen = strlen(cclients);
    p.publish.payload    = (unsigned char *)&cclients;

    publish_message(&p, topic_repo_fetch(server.repo, sys_topics[4].name));

    // $SOL/broker/bytes/sent
    p.publish.topiclen   = sys_topics[6].len;
    p.publish.topic      = (unsigned char *)sys_topics[6].name;
    p.publish.payloadlen = strlen(bsent);
    p.publish.payload    = (unsigned char *)&bsent;

    publish_message(&p, topic_repo_fetch(server.repo, sys_topics[6].name));

    // $SOL/broker/messages/sent
    p.publish.topiclen   = sys_topics[8].len;
    p.publish.topic      = (unsigned char *)sys_topics[8].name;
    p.publish.payloadlen = strlen(msent);
    p.publish.payload    = (unsigned char *)&msent;

    publish_message(&p, topic_repo_fetch(server.repo, sys_topics[8].name));

    // $SOL/broker/messages/received
    p.publish.topiclen   = sys_topics[9].len;
    p.publish.topic      = (unsigned char *)sys_topics[9].name;
    p.publish.payloadlen = strlen(mrecv);
    p.publish.payload    = (unsigned char *)&mrecv;

    publish_message(&p, topic_repo_fetch(server.repo, sys_topics[9].name));

    // $SOL/broker/memory/used
    p.publish.topiclen   = sys_topics[10].len;
    p.publish.topic      = (unsigned char *)sys_topics[10].name;
    p.publish.payloadlen = strlen(mem);
    p.publish.payload    = (unsigned char *)&mem;

    publish_message(&p, topic_repo_fetch(server.repo, sys_topics[10].name));
}

/*
 * Check for inflight messages in the ingoing and outgoing maps (actually
 * arrays), each position between 0-65535 contains either NULL or a pointer
 * to an inflight stucture with a timestamp of the sending action, the
 * target file descriptor (e.g. the client) and the payload to be sent
 * unserialized, this way it's possible to set the DUP flag easily at the cost
 * of additional packing before re-sending it out
 */
static void inflight_msg_check(struct ev_ctx *ctx, void *data)
{
    (void)data;
    (void)ctx;
    size_t size           = 0;
    time_t now            = time(NULL);
    struct mqtt_packet *p = NULL;
    Connection_Context *c, *tmp;
    HASH_ITER(hh, server.context_map, c, tmp)
    {
        if (!c || !c->connected || !c->session || !has_inflight(c->session))
            continue;
        for (int i = 1; i < MAX_INFLIGHT_MSGS; ++i) {
            // TODO remove 20 hardcoded value
            // Messages
            if (c->session->i_msgs[i].packet &&
                (now - c->session->i_msgs[i].seen) > 20) {
                log_debug("Re-sending message to %s", c->cid);
                p                  = c->session->i_msgs[i].packet;
                p->header.bits.qos = c->session->i_msgs[i].qos;
                // Set DUP flag to 1
                mqtt_set_dup(p);
                size = mqtt_size(c->session->i_msgs[i].packet, NULL);
                // Serialize the packet and send it out again
                mqtt_write(p, c->send_buf + c->write_total);
                c->write_total += size;
                enqueue_event_write(c);
                // Update information stats
                info.messages_sent++;
            }
            // ACKs
            if (c->session->i_acks[i] > 0 &&
                (now - c->session->i_acks[i]) > 20) {
                log_debug("Re-sending ack to %s", c->cid);
                struct mqtt_packet ack;
                mqtt_ack(&ack, i);
                // Set DUP flag to 1
                mqtt_set_dup(&ack);
                size = mqtt_size(&ack, NULL);
                // Serialize the packet and send it out again
                mqtt_write(p, c->send_buf + c->write_total);
                c->write_total += size;
                enqueue_event_write(c);
                // Update information stats
                info.messages_sent++;
            }
        }
    }
}

/*
 * ======================================================
 *  Private functions and callbacks for server behaviour
 * ======================================================
 */

/*
 * All clients are pre-allocated at the start of the server, but their
 * buffers (read and write) are not, they're lazily allocated with this
 * function, meant to be called on the accept callback
 */
static void connection_ctx_init(Connection_Context *client)
{
    client->online        = true;
    client->connected     = false;
    client->clean_session = true;
    client->cid[0]        = '\0';
    client->state         = CS_OPEN;
    client->rc            = 0;
    client->recv_buf      = try_alloc(BUFSIZE);
    client->send_buf      = try_alloc(BUFSIZE);
    client->read          = 0;
    client->written       = 0;
    client->write_total   = 0;
    client->last_seen     = time(NULL);
    client->has_lwt       = false;
    client->session       = NULL;
}

/*
 * As we really don't want to completely de-allocate a client in favor of
 * making it reusable by another connection we simply deactivate it
 * according to its state (e.g. if it's a clean_session connected client or
 * not) and we allow the clients memory pool to reclaim it
 */
static void connection_ctx_deactivate(Connection_Context *context)
{

    if (context->online == false)
        return;

    context->read    = 0;
    context->written = context->write_total = 0;
    close_connection(&context->conn);

    context->online = false;

    if (context->clean_session == true) {
        if (context->session) {
            topic_repo_remove_wildcard(server.repo, context->cid);
            list_foreach(item, context->session->subscriptions)
            {
                topic_del_subscriber(item->data, context);
            }
            HASH_DEL(server.sessions, context->session);
            DECREF(context->session, struct client_session);
        }
        if (context->connected == true)
            HASH_DEL(server.context_map, context);
        memorypool_free(server.pool, context);
    }
    context->connected = false;
    context->cid[0]    = '\0';
    context->state     = CS_CLOSED;
}

/*
 * Write stream of bytes to a client represented by a connection object,
 * till all bytes to be written is exhausted, tracked by towrite field or if
 * an EAGAIN (socket descriptor must be in non-blocking mode) error is
 * raised, meaning we cannot write anymore for the current cycle.
 */
static inline int write_data(Connection_Context *c)
{
    ssize_t wrote = send_data(&c->conn, c->send_buf + c->written,
                              c->write_total - c->written);
    if (errno != EAGAIN && errno != EWOULDBLOCK && wrote < 0)
        goto e_client_dc;
    c->written += wrote > 0 ? wrote : 0;
    if (c->written < c->write_total && errno == EAGAIN)
        goto eagain;
    // Update information stats
    info.bytes_sent += c->write_total;
    memset(c->send_buf, 0x00, BUFSIZE);
    // Reset client written bytes track fields
    c->write_total = c->written = 0;
    // printf("resetting %ld (%ld)\n", c->written, c->write_total);
    return SOL_OK;

e_client_dc:
    return -ERRSOCKETERR;

eagain:
    return -ERREAGAIN;
}

/*
 * ===========
 *  Callbacks
 * ===========
 */

/*
 * Callback dedicated to client replies, try to send as much data as
 * possible epmtying the client buffer and rearming the socket descriptor
 * for reading after
 */
static void send_callback(struct ev_ctx *ctx, void *arg)
{
    Connection_Context *client = arg;
    if (client->write_total == 0)
        return;
    int err = write_data(client);
    switch (err) {
    case SOL_OK:
        /*
         * Rearm descriptor making it ready to receive input,
         * read_callback will be the callback to be used; also reset the
         * read buffer state for the client.
         */
        ev_add(ctx, client->conn.fd, EV_READ, recv_callback, client);
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
        log_info("Closing connection with %s (%s): %s %i", client->cid,
                 client->conn.ip, solerr(client->rc), err);
        ev_delete(ctx, client->conn.fd);
        connection_ctx_deactivate(client);
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
static void accept_callback(struct ev_ctx *ctx, void *data)
{
    int serverfd = *((int *)data);
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
        Connection_Context *c = memorypool_alloc(server.pool);
        c->conn               = conn;
        connection_ctx_init(c);
        c->ctx = ctx;

        /* Add it to the epoll loop */
        ev_add_event(ctx, fd, EV_READ, recv_callback, c);

        /* Record the new client connected */
        info.active_connections++;
        info.total_connections++;

        log_info("[%p] Connection from %s", (void *)pthread_self(), conn.ip);
    }
}

/*
 * This function is called only if the client has sent a full stream of
 * bytes consisting of a complete packet as expected by the MQTT protocol
 * and by the declared length of the packet. It uses eventloop APIs to react
 * accordingly to the packet type received, validating it before proceed to
 * call handlers. Depending on the handler called and its outcome, it'll
 * enqueue an event to write a reply or just reset the client state to allow
 * reading some more packets.
 */
static void process_mqtt_message(Connection_Context *c, off_t start,
                                 off_t offset, size_t total_len)
{
    u8 fixed_header  = *(c->recv_buf + start);
    usize total_size = total_len - offset;
    /*
     * Unpack received bytes into a mqtt_packet structure and execute the
     * correct handler based on the type of the operation.
     */
    if (mqtt_read(c->recv_buf + offset, &c->data, fixed_header, total_size) <
        0) {
        log_error("Unknown packet received");
        return;
    }
    c->rc = handle_command(c->data.header.bits.type, c);
    switch (c->rc) {
    case REPLY:
    case MQTT_NOT_AUTHORIZED:
    case MQTT_BAD_CREDENTIALS:
        /*
         * Write out to client, after a request has been processed in
         * worker thread routine. Just send out all bytes stored in the
         * reply buffer to the reply file descriptor.
         */
        // Free resource, ACKs will be free'd closing the server
        if (c->data.header.bits.type != PUBLISH)
            mqtt_packet_free(&c->data);
        break;
    case -ERRCLIENTDC:
        // Client disconnected, set resources to be cleaned up
        c->state = CS_CLOSING;
        shutdown(c->conn.fd, SHUT_RDWR);
        break;
    case -ERRNOMEM:
        log_error(solerr(c->rc));
        break;
    default:
        if (c->data.header.bits.type != PUBLISH)
            mqtt_packet_free(&c->data);
        break;
    }
}

/*
 * Reading packet callback, it's the main function that will be called every
 * time a connected client has some data to be read, notified by the
 * eventloop context.
 */
static void recv_callback(struct ev_ctx *ctx, void *data)
{
    Connection_Context *c = data;
    if (c->state == CS_CLOSING) {
        ev_delete(ctx, c->conn.fd);
        connection_ctx_deactivate(c);
        // Update stats
        info.active_connections--;
        info.total_connections--;

        return;
    }

    ssize_t nread =
        recv_data(&c->conn, c->recv_buf + c->read, BUFSIZE - c->read);

    if (nread <= 0) {
        // No data available right now
        if (nread == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            /*
             * We have an EAGAIN error, which is really just signaling that
             * for some reasons the kernel is not ready to read more bytes at
             * the moment and it would block, so we just want to re-try some
             * time later, re-enqueuing a new read event
             */
            ev_add(ctx, c->conn.fd, EV_READ, recv_callback, c);
            return;
        }

        /*
         * We got an unexpected error or a disconnection from the
         * client side, remove client from the global map and
         * free resources allocated such as io_event structure and
         * paired payload
         */
        log_error("Closing connection with %s (%s): %s", c->cid, c->conn.ip,
                  solerr(nread));
        // Publish, if present, LWT message
        if (c->has_lwt == true) {
            char *tname     = (char *)c->session->lwt_msg.publish.topic;
            struct topic *t = topic_repo_fetch(server.repo, tname);
            if (t)
                publish_message(&c->session->lwt_msg, t);
        }
        // Clean resources
        ev_delete(ctx, c->conn.fd);
        // Remove from subscriptions for now
        if (c->session && list_size(c->session->subscriptions) > 0) {
            list_foreach(item, c->session->subscriptions)
            {
                log_debug("Deleting %s from topic %s", c->cid,
                          ((struct topic *)item->data)->name);
                topic_del_subscriber(item->data, c);
            }
        }
        connection_ctx_deactivate(c);
        info.active_connections--;
        info.total_connections--;

        return;
    }

    c->read += nread; // Update the amount of data read
    info.bytes_recv += nread;
    c->last_seen     = time(NULL);
    size_t processed = 0;

    // Process all complete packets in the buffer
    while (processed < c->read) {
        if (c->read - processed < 1) // Not enough for a header
            break;

        size_t offset       = processed + sizeof(u8);
        unsigned pos        = 0;
        /*
         * Read remaining length bytes which starts at byte 2 and can be
         * long to 4 bytes based on the size stored, so byte 2-5 is
         * dedicated to the packet length.
         */
        size_t variable_len = mqtt_read_length(c->recv_buf + offset, &pos);
        // Total packet length
        size_t total_len    = variable_len + pos + sizeof(u8);

        if (c->read - processed < total_len) // Incomplete packet
            break;

        offset += pos;

        // Process the complete packet
        process_mqtt_message(c, processed, offset, total_len);
        processed += total_len; // Advance to the next packet
    }

    // Handle leftover data
    size_t leftover = c->read - processed;
    if (leftover > 0) {
        // Move leftover data to the start
        memmove(c->recv_buf, c->recv_buf + processed, leftover);
    }
    c->read = leftover;

    memset(c->recv_buf + c->read, 0x00, BUFSIZE - c->read);

    enqueue_event_write(c);
}

/*
 * Eventloop stop callback, will be triggered by an EV_CLOSEFD event and
 * stop the running loop, unblocking the call.
 */
static void stop_handler(struct ev_ctx *ctx, void *arg)
{
    (void)arg;
    ev_stop(ctx);
}

/*
 * IO worker function, wait for events on a dedicated epoll descriptor which
 * is shared among multiple threads for input and output only, following the
 * normal EPOLL semantic, EPOLLIN for incoming bytes to be unpacked and
 * processed by a worker thread, EPOLLOUT for bytes incoming from a worker
 * thread, ready to be delivered out.
 */
static void eventloop_start(void *args)
{
    struct listen_payload *loop_data = args;
    struct ev_ctx ctx;
    int sfd = loop_data->fd;
    ev_init(&ctx, EVENTLOOP_MAX_EVENTS);
    // Register stop event
#ifdef __linux__
    ev_register_event(&ctx, conf->run, EV_CLOSEFD | EV_READ, stop_handler,
                      NULL);
#else
    ev_add_event(&ctx, conf->run[1], EV_CLOSEFD | EV_READ, stop_handler, NULL);
#endif
    // Register listening FD with accept callback
    ev_add_event(&ctx, sfd, EV_READ, accept_callback, &sfd);
    // Register periodic tasks
    if (loop_data->cronjobs == true) {
        ev_add_cron(&ctx, publish_stats, NULL, conf->stats_pub_interval, 0);
        ev_add_cron(&ctx, inflight_msg_check, NULL, 1, 0);
    }
    // Start the loop, blocking call
    if (ev_run(&ctx) < 0)
        log_error("Event loop: %s", strerror(errno));
    ev_free(&ctx);
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
void enqueue_event_write(const Connection_Context *c)
{
    if (c->state == CS_CLOSING) {
        ev_delete(c->ctx, c->conn.fd);
        connection_ctx_deactivate((Connection_Context *)c);
        // Update stats
        info.active_connections--;
        info.total_connections--;
        return;
    }
    ev_add(c->ctx, c->conn.fd, EV_WRITE, send_callback, (void *)c);
}

/*
 * Main entry point for the server, to be called with an address and a port
 * to start listening. The function may fail only in the case of Out of
 * memory error occurs or listen call fails, on the other cases it should
 * just log unexpected errors.
 */
int start_server(const char *addr, const char *port)
{

    INIT_INFO;

    /* Initialize global Sol instance */
    server.repo  = topic_repo_new();
    server.auths = NULL;
    server.pool  = memorypool_new(BASE_CLIENTS_NUM, sizeof(Connection_Context));
    if (!server.pool)
        log_fatal("Failed to allocate %d sized memory pool for clients",
                  BASE_CLIENTS_NUM);
    server.context_map = NULL;
    server.sessions    = NULL;

    if (conf->allow_anonymous == false)
        if (!config_read_passwd_file(conf->password_file, &server.auths))
            log_error("Failed to read password file");

    /* Generate stats topics */
    for (int i = 0; i < SYS_TOPICS; i++) {
        struct topic *t = topic_new(try_strdup(sys_topics[i].name));
        if (!t)
            log_fatal("start_server failed: Out of memory");
        topic_repo_put(server.repo, t);
    }

    /* Start listening for new connections */
    int sfd = make_listen(addr, port, conf->socket_family);

    /* Setup SSL in case of flag true */
    if (conf->tls == true) {
        openssl_init();
        server.ssl_ctx = create_ssl_context();
        load_certificates(server.ssl_ctx, conf->cafile, conf->certfile,
                          conf->keyfile);
    }

    log_info("Server start");
    info.start_time                  = time(NULL);

    struct listen_payload loop_start = {sfd, false};

    loop_start.cronjobs              = true;
    // start eventloop, could be spread on multiple threads
    eventloop_start(&loop_start);

    close(sfd);
    AUTH_DESTROY(server.auths);
    topic_repo_free(server.repo);

    /* Destroy SSL context, if any present */
    if (conf->tls == true) {
        SSL_CTX_free(server.ssl_ctx);
        openssl_cleanup();
    }
    log_info("Sol v%s exiting", VERSION);

    return SOL_OK;
}

/*
 * Make the entire process a daemon running in background
 */
void daemonize(void)
{

    int fd;

    if (fork() != 0)
        exit(0);

    setsid();

    if ((fd = open("/dev/null", O_RDWR, 0)) != -1) {
        dup2(fd, STDIN_FILENO);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        if (fd > STDERR_FILENO)
            close(fd);
    }
}
