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
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/epoll.h>
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

/*
 * General informations of the broker, all fields will be published
 * periodically to internal topics
 */
static struct sol_info info;

/* Broker global instance, contains the topic trie and the clients hashtable */
static struct sol sol;

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


/* Prototype for a command handler */
typedef int handler(struct closure *, union mqtt_packet *);

/* I/O closures, for the 3 main operation of the server
 * - Accept a new connecting client
 * - Read incoming bytes from connected clients
 * - Write output bytes to connected clients
 */
static void on_read(struct evloop *, void *);

static void on_write(struct evloop *, void *);

static void on_accept(struct evloop *, void *);

/* Command handler, each one have responsibility over a defined command packet */
static int connect_handler(struct closure *, union mqtt_packet *);

static int disconnect_handler(struct closure *, union mqtt_packet *);

static int subscribe_handler(struct closure *, union mqtt_packet *);

static int unsubscribe_handler(struct closure *, union mqtt_packet *);

static int publish_handler(struct closure *, union mqtt_packet *);

static int puback_handler(struct closure *, union mqtt_packet *);

static int pubrec_handler(struct closure *, union mqtt_packet *);

static int pubrel_handler(struct closure *, union mqtt_packet *);

static int pubcomp_handler(struct closure *, union mqtt_packet *);

static int pingreq_handler(struct closure *, union mqtt_packet *);

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
 * Connection structure for private use of the module, mainly for accepting
 * new connections
 */
struct connection {
    char ip[INET_ADDRSTRLEN + 1];
    int fd;
};

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
static ssize_t recv_packet(int clientfd, unsigned char *buf, char *command) {

    ssize_t nbytes = 0;

    /* Read the first byte, it should contain the message type code */
    if ((nbytes = recv_bytes(clientfd, buf, 1)) <= 0)
        return -ERRCLIENTDC;

    unsigned char byte = *buf;
    buf++;

    if (DISCONNECT < (byte >> 4) || CONNECT > (byte >> 4))
        return -ERRPACKETERR;

    /*
     * Read remaning length bytes which starts at byte 2 and can be long to 4
     * bytes based on the size stored, so byte 2-5 is dedicated to the packet
     * length.
     */
    unsigned char buff[4];
    int count = 0;
    int n = 0;
    do {
        if ((n = recv_bytes(clientfd, buf+count, 1)) <= 0)
            return -ERRCLIENTDC;
        buff[count] = buf[count];
        nbytes += n;
    } while (buff[count++] & (1 << 7));

    // Reset temporary buffer
    const unsigned char *pbuf = &buff[0];
    unsigned long long tlen = mqtt_decode_length(&pbuf);

    /*
     * Set return code to -ERRMAXREQSIZE in case the total packet len exceeds
     * the configuration limit `max_request_size`
     */
    if (tlen > conf->max_request_size) {
        nbytes = -ERRMAXREQSIZE;
        goto exit;
    }

    /* Read remaining bytes to complete the packet */
    if ((n = recv_bytes(clientfd, buf + 1, tlen)) < 0)
        goto err;

    nbytes += n;

    *command = byte;

exit:

    return nbytes;

err:

    shutdown(clientfd, 0);
    close(clientfd);

    return nbytes;

}

/*
 * Command handlers
 */

static int connect_handler(struct closure *cb, union mqtt_packet *pkt) {

    // TODO just return error_code and handle it on `on_read`
    if (hashtable_exists(sol.clients,
                         (const char *) pkt->connect.payload.client_id)) {

        // Already connected client, 2 CONNECT packet should be interpreted as
        // a violation of the protocol, causing disconnection of the client

        sol_info("Received double CONNECT from %s, disconnecting client",
                 pkt->connect.payload.client_id);

        close(cb->fd);
        hashtable_del(sol.clients, (const char *) pkt->connect.payload.client_id);
        hashtable_del(sol.closures, cb->closure_id);

        // Update stats
        info.nclients--;
        info.nconnections--;

        return -REARM_W;
    }

    sol_info("New client connected as %s (c%i, k%u)",
             pkt->connect.payload.client_id,
             pkt->connect.bits.clean_session,
             pkt->connect.payload.keepalive);

    /*
     * Add the new connected client to the global map, if it is already
     * connected, kick him out accordingly to the MQTT v3.1.1 specs.
     */
    struct sol_client *new_client = sol_malloc(sizeof(*new_client));
    new_client->fd = cb->fd;
    const char *cid = (const char *) pkt->connect.payload.client_id;
    new_client->client_id = sol_strdup(cid);
    buffer_create(&new_client->buf);
    hashtable_put(sol.clients, cid, new_client);

    /* Substitute fd on callback with closure */
    cb->obj = new_client;

    /* Respond with a connack */
    union mqtt_packet *response = sol_malloc(sizeof(*response));
    unsigned char byte = CONNACK_BYTE;

    // TODO check for session already present

    if (pkt->connect.bits.clean_session == false)
        new_client->session.subscriptions = list_create(NULL);

    unsigned char session_present = 0;
    unsigned char connect_flags = 0 | (session_present & 0x1) << 0;
    unsigned char rc = 0;  // 0 means connection accepted

    response->connack = *mqtt_packet_connack(byte, connect_flags, rc);

    cb->payload = bytestring_create(MQTT_ACK_LEN);
    unsigned char *p = pack_mqtt_packet(response, CONNACK);
    memcpy(cb->payload->data, p, MQTT_ACK_LEN);
    sol_free(p);

    sol_debug("Sending CONNACK to %s (%u, %u)",
              pkt->connect.payload.client_id,
              session_present, rc);

    sol_free(response);

    return REARM_W;
}


static int disconnect_handler(struct closure *cb, union mqtt_packet *pkt) {

    // TODO just return error_code and handle it on `on_read`

    /* Handle disconnection request from client */
    struct sol_client *c = cb->obj;

    sol_debug("Received DISCONNECT from %s", c->client_id);

    close(c->fd);
    hashtable_del(sol.clients, c->client_id);
    hashtable_del(sol.closures, cb->closure_id);

    // Update stats
    info.nclients--;
    info.nconnections--;

    // TODO remove from all topic where it subscribed
    return -REARM_W;
}


static void recursive_subscription(struct trie_node *node, void *arg) {

    if (!node || !node->data)
        return;

    struct list_node *child = node->children->head;
    for (; child; child = child->next)
        recursive_subscription(child->data, arg);

    struct topic *t = node->data;

    struct subscriber *s = arg;

    t->subscribers = list_push(t->subscribers, s);
}


static int subscribe_handler(struct closure *cb, union mqtt_packet *pkt) {

    struct sol_client *c = cb->obj;
    bool wildcard = false;
    bool alloced = false;

    /*
     * We respond to the subscription request with SUBACK and a list of QoS in
     * the same exact order of reception
     */
    unsigned char rcs[pkt->subscribe.tuples_len];

    /* Subscribe packets contains a list of topics and QoS tuples */
    for (unsigned i = 0; i < pkt->subscribe.tuples_len; i++) {

        sol_debug("Received SUBSCRIBE from %s", c->client_id);

        /*
         * Check if the topic exists already or in case create it and store in
         * the global map
         */
        char *topic = (char *) pkt->subscribe.tuples[i].topic;

        sol_debug("\t%s (QoS %i)", topic, pkt->subscribe.tuples[i].qos);

        /* Recursive subscribe to all children topics if the topic ends with "/#" */
        if (topic[pkt->subscribe.tuples[i].topic_len - 1] == '#' &&
            topic[pkt->subscribe.tuples[i].topic_len - 2] == '/') {
            topic = remove_occur(topic, '#');
            wildcard = true;
        } else if (topic[pkt->subscribe.tuples[i].topic_len - 1] != '/') {
            topic = append_string((char *) pkt->subscribe.tuples[i].topic, "/", 1);
            alloced = true;
        }

        struct topic *t = sol_topic_get(&sol, topic);

        // TODO check for callback correctly set to obj

        if (!t) {
            t = topic_create(sol_strdup(topic));
            sol_topic_put(&sol, t);
        } else if (wildcard == true) {
            struct subscriber *sub = sol_malloc(sizeof(*sub));
            sub->client = cb->obj;
            sub->qos = pkt->subscribe.tuples[i].qos;
            trie_prefix_map_tuple(&sol.topics, topic,
                                  recursive_subscription, sub);
        }

        // Clean session true for now
        topic_add_subscriber(t, cb->obj, pkt->subscribe.tuples[i].qos, true);

        if (alloced)
            sol_free(topic);

        rcs[i] = pkt->subscribe.tuples[i].qos;
    }

    struct mqtt_suback *suback = mqtt_packet_suback(SUBACK_BYTE,
                                                    pkt->subscribe.pkt_id,
                                                    rcs,
                                                    pkt->subscribe.tuples_len);

    mqtt_packet_release(pkt, SUBSCRIBE);
    pkt->suback = *suback;
    unsigned char *packed = pack_mqtt_packet(pkt, SUBACK);
    size_t len = MQTT_HEADER_LEN + sizeof(uint16_t) + pkt->subscribe.tuples_len;
    cb->payload = bytestring_create(len);
    memcpy(cb->payload->data, packed, len);
    sol_free(packed);

    mqtt_packet_release(pkt, SUBACK);
    sol_free(suback);

    sol_debug("Sending SUBACK to %s", c->client_id);

    return REARM_W;
}


static int unsubscribe_handler(struct closure *cb, union mqtt_packet *pkt) {

    struct sol_client *c = cb->obj;

    sol_debug("Received UNSUBSCRIBE from %s", c->client_id);

    pkt->ack = *mqtt_packet_ack(UNSUBACK_BYTE, pkt->unsubscribe.pkt_id);

    unsigned char *packed = pack_mqtt_packet(pkt, UNSUBACK);
    cb->payload = bytestring_create(MQTT_ACK_LEN);
    memcpy(cb->payload->data, packed, MQTT_ACK_LEN);
    sol_free(packed);

    sol_debug("Sending UNSUBACK to %s", c->client_id);

    return REARM_W;
}


static int publish_handler(struct closure *cb, union mqtt_packet *pkt) {

    struct sol_client *c = cb->obj;

    sol_debug("Received PUBLISH from %s (d%i, q%u, r%i, m%u, %s, ... (%i bytes))",
              c->client_id,
              pkt->publish.header.bits.dup,
              pkt->publish.header.bits.qos,
              pkt->publish.header.bits.retain,
              pkt->publish.pkt_id,
              pkt->publish.topic,
              pkt->publish.payloadlen);

    info.messages_recv++;

    char *topic = (char *) pkt->publish.topic;
    bool alloced = false;
    unsigned char qos = pkt->publish.header.bits.qos;

    /*
     * For convenience we assure that all topics ends with a '/', indicating a
     * hierarchical level
     */
    if (topic[pkt->publish.topiclen - 1] != '/') {
        topic = append_string((char *) pkt->publish.topic, "/", 1);
        alloced = true;
    }

    /*
     * Retrieve the topic from the global map, if it wasn't created before,
     * create a new one with the name selected
     */
    struct topic *t = sol_topic_get(&sol, topic);

    if (!t) {
        t = topic_create(sol_strdup(topic));
        sol_topic_put(&sol, t);
    }

    // Not the best way to handle this
    if (alloced == true)
        sol_free(topic);

    size_t publen;
    unsigned char *pub;

    struct list_node *cur = t->subscribers->head;
    for (; cur; cur = cur->next) {

        publen = MQTT_HEADER_LEN + sizeof(uint16_t) +
            pkt->publish.topiclen + pkt->publish.payloadlen;

        struct subscriber *sub = cur->data;
        struct sol_client *sc = sub->client;

        /* Update QoS according to subscriber's one */
        pkt->publish.header.bits.qos = sub->qos;

        if (pkt->publish.header.bits.qos > AT_MOST_ONCE)
            publen += sizeof(uint16_t);

        pub = pack_mqtt_packet(pkt, PUBLISH);

        ssize_t sent;
        if ((sent = send_bytes(sc->fd, pub, publen)) < 0)
            sol_error("Error publishing to %s: %s",
                      sc->client_id, strerror(errno));

        // Update information stats
        info.bytes_sent += sent;

        sol_debug("Sending PUBLISH to %s (d%i, q%u, r%i, m%u, %s, ... (%i bytes))",
                  sc->client_id,
                  pkt->publish.header.bits.dup,
                  pkt->publish.header.bits.qos,
                  pkt->publish.header.bits.retain,
                  pkt->publish.pkt_id,
                  pkt->publish.topic,
                  pkt->publish.payloadlen);

        info.messages_sent++;

        sol_free(pub);
    }

    // TODO free publish

    if (qos == AT_LEAST_ONCE) {

        unsigned char pkt_byte = 0x40;
        mqtt_puback *puback = mqtt_packet_ack(pkt_byte, pkt->publish.pkt_id);

        mqtt_packet_release(pkt, PUBLISH);

        pkt->ack = *puback;

        unsigned char *packed = pack_mqtt_packet(pkt, PUBACK);
        cb->payload = bytestring_create(MQTT_ACK_LEN);
        memcpy(cb->payload->data, packed, MQTT_ACK_LEN);
        sol_free(packed);

        sol_debug("Sending PUBACK to %s", c->client_id);

        return REARM_W;

    } else if (qos == EXACTLY_ONCE) {

        // TODO add to a hashtable to track PUBREC clients last

        mqtt_pubrec *pubrec = mqtt_packet_ack(PUBREC_BYTE, pkt->publish.pkt_id);

        mqtt_packet_release(pkt, PUBLISH);

        pkt->ack = *pubrec;

        unsigned char *packed = pack_mqtt_packet(pkt, PUBREC);
        cb->payload = bytestring_create(MQTT_ACK_LEN);
        memcpy(cb->payload->data, packed, MQTT_ACK_LEN);
        sol_free(packed);

        sol_debug("Sending PUBREC to %s", c->client_id);

        return REARM_W;

    }

    mqtt_packet_release(pkt, PUBLISH);

    /*
     * We're in the case of AT_MOST_ONCE QoS level, we don't need to sent out
     * any byte, it's a fire-and-forget.
     */
    return REARM_R;
}


static int puback_handler(struct closure *cb, union mqtt_packet *pkt) {

    sol_debug("Received PUBACK from %s",
              ((struct sol_client *) cb->obj)->client_id);

    // TODO Remove from pending PUBACK clients map

    return REARM_R;
}


static int pubrec_handler(struct closure *cb, union mqtt_packet *pkt) {

    struct sol_client *c = cb->obj;

    sol_debug("Received PUBREC from %s", c->client_id);

    mqtt_pubrel *pubrel = mqtt_packet_ack(PUBREL_BYTE, pkt->publish.pkt_id);

    pkt->ack = *pubrel;

    unsigned char *packed = pack_mqtt_packet(pkt, PUBREC);
    cb->payload = bytestring_create(MQTT_ACK_LEN);
    memcpy(cb->payload->data, packed, MQTT_ACK_LEN);
    sol_free(packed);

    sol_debug("Sending PUBREL to %s", c->client_id);

    return REARM_W;
}


static int pubrel_handler(struct closure *cb, union mqtt_packet *pkt) {

    sol_debug("Received PUBREL from %s",
              ((struct sol_client *) cb->obj)->client_id);

    mqtt_pubcomp *pubcomp = mqtt_packet_ack(PUBCOMP_BYTE, pkt->publish.pkt_id);

    pkt->ack = *pubcomp;

    unsigned char *packed = pack_mqtt_packet(pkt, PUBCOMP);
    cb->payload = bytestring_create(MQTT_ACK_LEN);
    memcpy(cb->payload->data, packed, MQTT_ACK_LEN);
    sol_free(packed);

    sol_debug("Sending PUBCOMP to %s",
              ((struct sol_client *) cb->obj)->client_id);

    return REARM_W;
}


static int pubcomp_handler(struct closure *cb, union mqtt_packet *pkt) {

    sol_debug("Received PUBCOMP from %s",
              ((struct sol_client *) cb->obj)->client_id);

    // TODO Remove from pending PUBACK clients map

    return REARM_R;
}


static int pingreq_handler(struct closure *cb, union mqtt_packet *pkt) {

    sol_debug("Received PINGREQ from %s",
              ((struct sol_client *) cb->obj)->client_id);

    pkt->header = *mqtt_packet_header(PINGRESP_BYTE);
    unsigned char *packed = pack_mqtt_packet(pkt, PINGRESP);
    cb->payload = bytestring_create(MQTT_HEADER_LEN);
    memcpy(cb->payload->data, packed, MQTT_HEADER_LEN);
    sol_free(packed);

    sol_debug("Sending PINGRESP to %s",
              ((struct sol_client *) cb->obj)->client_id);

    return REARM_W;
}

/*
 *  I/O Closures
 */

static void on_write(struct evloop *loop, void *arg) {

    struct closure *cb = arg;
    struct sol_client *c = cb->obj;

    ssize_t sent;

    if (buffer_empty(&c->buf) == 0) {

        /* Check if there's still some remaning bytes to send out */
        if ((sent = send_bytes(cb->fd, c->buf.bytes + c->buf.start, c->buf.end)) < 0)
            sol_error("Error writing on socket to client %s: %s",
                      ((struct sol_client *) cb->obj)->client_id, strerror(errno));

        /* Update buffer pointers */
        c->buf.start += sent;

    }

    if ((sent = send_bytes(cb->fd, cb->payload->data, cb->payload->size)) < 0)
        sol_error("Error writing on socket to client %s: %s",
                  ((struct sol_client *) cb->obj)->client_id, strerror(errno));

    /* Update client buffer for remaining bytes to send */
    if (sent < cb->payload->size)
        buffer_push_back(&c->buf, cb->payload->data + sent,
                         cb->payload->size - sent);

    // Update information stats
    info.bytes_sent += sent;
    bytestring_release(cb->payload);
    cb->payload = NULL;

    /*
     * Re-arm callback by setting EPOLL event on EPOLLIN to read fds and
     * re-assigning the callback `on_read` for the next event
     */
    cb->call = on_read;
    evloop_rearm_callback_read(loop, cb);
}

/* Handle incoming requests, after being accepted or after a reply */
static void on_read(struct evloop *loop, void *arg) {

    struct closure *cb = arg;

    /* Raw bytes buffer to handle input from client */
    unsigned char *buffer = sol_malloc(conf->max_request_size);

    ssize_t bytes = 0;
    char command = 0;

    /*
     * We must read all incoming bytes till an entire packet is received. This
     * is achieved by following the MQTT v3.1.1 protocol specifications, which
     * send the size of the remaining packet as the second byte. By knowing it
     * we know if the packet is ready to be deserialized and used.
     */
    bytes = recv_packet(cb->fd, buffer, &command);

    /*
     * Looks like we got a client disconnection.
     *
     * TODO: Set a error_handler for ERRMAXREQSIZE instead of dropping client
     *       connection, explicitly returning an informative error code to the
     *       client connected.
     */
    if (bytes == -ERRCLIENTDC || bytes == -ERRMAXREQSIZE)
        goto exit;

    /*
     * If a not correct packet received, we must free the buffer and reset the
     * handler to the request again, setting EPOLL to EPOLLIN
     */
    if (bytes == -ERRPACKETERR)
        goto errdc;

    info.bytes_recv++;

    /*
     * Unpack received bytes into a mqtt_packet structure and execute the
     * correct handler based on the type of the operation.
     */
    union mqtt_packet packet;
    unpack_mqtt_packet(buffer, &packet);

    union mqtt_header hdr = { .byte = command };

    /* Execute command callback */
    int rc = handlers[hdr.bits.type](cb, &packet);

    if (rc == REARM_W) {

        cb->call = on_write;

        /*
         * Reset handler to read_handler in order to read new incoming data and
         * EPOLL event for read fds
         */
        evloop_rearm_callback_write(loop, cb);
    } else if (rc == REARM_R) {
        cb->call = on_read;
        evloop_rearm_callback_read(loop, cb);
    }

    // Disconnect packet received

exit:

    sol_free(buffer);

    return;

errdc:

    sol_free(buffer);

    sol_error("Dropping client");
    shutdown(cb->fd, 0);
    close(cb->fd);

    hashtable_del(sol.clients, ((struct sol_client *) cb->obj)->client_id);
    hashtable_del(sol.closures, cb->closure_id);

    info.nclients--;

    info.nconnections--;

    return;
}

/*
 * Accept a new incoming connection assigning ip address and socket descriptor
 * to the connection structure pointer passed as argument
 */
static int accept_new_client(int fd, struct connection *conn) {

    if (!conn)
        return -1;

    /* Accept the connection */
    int clientsock = accept_connection(fd);

    /* Abort if not accepted */
    if (clientsock == -1)
        return -1;

    /* Just some informations retrieval of the new accepted client connection */
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);

    if (getpeername(clientsock, (struct sockaddr *) &addr, &addrlen) < 0)
        return -1;

    char ip_buff[INET_ADDRSTRLEN + 1];
    if (inet_ntop(AF_INET, &addr.sin_addr, ip_buff, sizeof(ip_buff)) == NULL)
        return -1;

    struct sockaddr_in sin;
    socklen_t sinlen = sizeof(sin);

    if (getsockname(fd, (struct sockaddr *) &sin, &sinlen) < 0)
        return -1;

    conn->fd = clientsock;
    strcpy(conn->ip, ip_buff);

    return 0;
}

/*
 * Handle new connection, create a a fresh new struct client structure and link
 * it to the fd, ready to be set in EPOLLIN event
 */
static void on_accept(struct evloop *loop, void *arg) {

    /* struct connection *server_conn = arg; */
    struct closure *server = arg;
    struct connection conn;

    accept_new_client(server->fd, &conn);

    /* Create a client structure to handle his context connection */
    struct closure *client_closure = sol_malloc(sizeof(*client_closure));
    if (!client_closure)
        return;

    /* Populate client structure */
    client_closure->fd = conn.fd;
    client_closure->obj = NULL;
    client_closure->payload = NULL;
    client_closure->args = client_closure;
    client_closure->call = on_read;
    generate_uuid(client_closure->closure_id);

    hashtable_put(sol.closures, client_closure->closure_id, client_closure);

    /* Add it to the epoll loop */
    evloop_add_callback(loop, client_closure);

    /* Rearm server fd to accept new connections */
    evloop_rearm_callback_read(loop, server);

    /* Record the new client connected */
    info.nclients++;
    info.nconnections++;

    sol_info("New connection from %s on port %s", conn.ip, conf->port);
}


static void run(struct evloop *loop) {
    if (evloop_wait(loop) < 0) {
        sol_error("Event loop exited unexpectedly: %s", strerror(loop->status));
        evloop_free(loop);
    }
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
    struct mqtt_publish *p = mqtt_packet_publish(PUBLISH_BYTE,
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
    size_t sent = 0L;
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
static void publish_stats(struct evloop *loop, void *args) {

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
 * Cleanup function to be passed in as destructor to the Hashtable for
 * connecting clients
 */
static int client_destructor(struct hashtable_entry *entry) {

    if (!entry)
        return -1;

    struct sol_client *client = entry->val;

    if (client->client_id)
        sol_free(client->client_id);

    buffer_release(&client->buf);

    sol_free(client);

    return 0;
}

/*
 * Cleanup function to be passed in as destructor to the Hashtable for
 * registered closures.
 */
static int closure_destructor(struct hashtable_entry *entry) {

    if (!entry)
        return -1;

    struct closure *closure = entry->val;

    if (closure->payload)
        bytestring_release(closure->payload);

    sol_free(closure);

    return 0;
}


int start_server(const char *addr, const char *port) {

    /* Initialize global Sol instance */
    trie_init(&sol.topics);
    sol.clients = hashtable_create(client_destructor);
    sol.closures = hashtable_create(closure_destructor);

    struct closure server_closure;

    /* Initialize the sockets, first the server one */
    server_closure.fd = make_listen(addr, port, conf->socket_family);
    server_closure.payload = NULL;
    server_closure.args = &server_closure;
    server_closure.call = on_accept;
    generate_uuid(server_closure.closure_id);

    /* Generate stats topics */
    for (int i = 0; i < SYS_TOPICS; i++)
        sol_topic_put(&sol, topic_create(sol_strdup(sys_topics[i])));

    struct evloop *event_loop = evloop_create(EPOLL_MAX_EVENTS, EPOLL_TIMEOUT);

    /* Set socket in EPOLLIN flag mode, ready to read data */
    evloop_add_callback(event_loop, &server_closure);

    /* Add periodic task for publishing stats on SYS topics */
    // TODO Implement
    struct closure sys_closure = {
        .fd = 0,
        .payload = NULL,
        .args = &sys_closure,
        .call = publish_stats
    };

    generate_uuid(sys_closure.closure_id);

    /* Schedule as periodic task to be executed every 5 seconds */
    evloop_add_periodic_task(event_loop, conf->stats_pub_interval,
                             0, &sys_closure);

    sol_info("Server start");
    info.start_time = time(NULL);

    run(event_loop);

    hashtable_release(sol.clients);
    hashtable_release(sol.closures);

    sol_info("Sol v%s exiting", VERSION);

    return 0;
}
