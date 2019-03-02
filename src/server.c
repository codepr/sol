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
#include "config.h"
#include "server.h"
#include "solcore.h"
#include "hashtable.h"


static struct sol_info info;


static struct sol sol;


typedef int handler(struct closure *, union mqtt_packet *);


static void on_read(struct evloop *, void *);

static void on_write(struct evloop *, void *);

static void on_accept(struct evloop *, void *);

static int connect_handler(struct closure *, union mqtt_packet *);

static int disconnect_handler(struct closure *, union mqtt_packet *);

static int subscribe_handler(struct closure *, union mqtt_packet *);

static int unsubscribe_handler(struct closure *, union mqtt_packet *);

static int publish_handler(struct closure *, union mqtt_packet *);

static int pubrel_handler(struct closure *, union mqtt_packet *);


static handler *handlers[15] = {
    NULL,
    connect_handler,
    NULL,
    publish_handler,
    NULL,
    NULL,
    pubrel_handler,
    NULL,
    subscribe_handler,
    NULL,
    unsubscribe_handler,
    NULL,
    NULL,
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
 * Parse packet header, it is required at least the first 5 bytes in order to
 * read packet type and total length that we need to recv to complete the
 * packet.
 *
 * This function accept a socket fd, a buffer to read incoming streams of
 * bytes and a structure formed by 3 fields:
 *
 * - opcode -> to set the OPCODE set in the incoming header, for simplicity
 *             and convenience of the caller
 * - buf -> a byte buffer, it will be malloc'ed in the function and it will
 *          contain the serialized bytes of the incoming packet
 * - flags -> flags pointer, copy the flag setting of the incoming packet,
 *            again for simplicity and convenience of the caller.
 */
static ssize_t recv_packet(int clientfd, unsigned char *buf, unsigned *flags) {

    ssize_t nbytes = 0;

    /* Read the first byte, it should contain the message type code */
    if ((nbytes = recv_bytes(clientfd, buf, 1)) <= 0)
        return -ERRCLIENTDC;

    unsigned char byte = *buf;
    buf++;

    if (DISCONNECT < byte || CONNECT > byte)
        return -ERRPACKETERR;

    char continuation;

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
        buff[count] = buf[1];
        continuation = buff[count] & (1 << 7);
        nbytes += n;
        count++;
    } while (continuation == 1);

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
    if ((n = recv_bytes(clientfd, buf+1, tlen)) < 0)
        goto err;
    nbytes += n;

    *flags = byte;

exit:

    return nbytes;

err:

    shutdown(clientfd, 0);
    close(clientfd);

    return nbytes;

}


static int connect_handler(struct closure *cb, union mqtt_packet *pkt) {

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
    hashtable_put(sol.clients, cid, new_client);

    /* Substitute fd on callback with closure */
    cb->obj = new_client;

    //TODO kick already connected clients

    /* Respond with a connack */
    union mqtt_packet *response = sol_malloc(sizeof(*response));
    unsigned char byte = CONNACK;
    // TODO check for session already present
    unsigned char session_present = 0;
    unsigned char connect_flags = 0 | (session_present & 0x1) << 0;
    unsigned char rc = 0;  // 0 means connection accepted
    response->connack = *mqtt_packet_connack(byte, connect_flags, rc);

    cb->payload = bytestring_create(MQTT_ACK_LEN);
    unsigned char *p = pack_mqtt_packet(response, 2);
    memcpy(cb->payload->data, p, MQTT_ACK_LEN);

    sol_debug("Sending CONNACK to %s (%u, %u)",
              pkt->connect.payload.client_id,
              session_present, rc);

    return REARM_W;
}


static int disconnect_handler(struct closure *cb, union mqtt_packet *pkt) {
  /* Handle disconnection request from client */
    struct sol_client *c = cb->obj;
    close(c->fd);
    hashtable_del(sol.clients, c->client_id);

    // TODO remove from all topic where it subscribed
    return REARM_R;
}


static int subscribe_handler(struct closure *cb, union mqtt_packet *pkt) {

    struct sol_client *c = cb->obj;

    for (unsigned i = 0; i < pkt->subscribe.tuples_len; i++) {

        sol_debug("Received SUBSCRIBE from %s", c->client_id);

        /*
         * Check if the topic exists already or in case create it and store in
         * the global map
         */
        const char *topic_name = (const char *) pkt->subscribe.tuples[i].topic;
        struct topic *t = sol_topic_get(&sol, topic_name);

        sol_debug("\t%s (QoS %i)", topic_name, pkt->subscribe.tuples[i].qos);

        // TODO check for callback correctly set to obj

        if (!t) {
            t = topic_create(topic_name);
            sol_topic_put(&sol, t);
        }

        topic_add_subscriber(t, cb->obj);
    }

    unsigned char rcs[pkt->subscribe.tuples_len];

    // For now we just make all 0s as return codes
    for (unsigned i = 0; i < pkt->subscribe.tuples_len; i++)
        rcs[i] = pkt->subscribe.tuples[i].qos;

    struct mqtt_suback *suback = mqtt_packet_suback(SUBACK,
                                                    pkt->subscribe.pkt_id,
                                                    rcs,
                                                    pkt->subscribe.tuples_len);

    pkt->suback = *suback;
    unsigned char *packed = pack_mqtt_packet(pkt, SUBACK_TYPE);
    size_t len = MQTT_HEADER_LEN + sizeof(uint16_t) + pkt->subscribe.tuples_len;
    cb->payload = bytestring_create(len);
    memcpy(cb->payload->data, packed, len);

    sol_debug("Sending SUBACK to %s", c->client_id);

    return REARM_W;
}


static int unsubscribe_handler(struct closure *cb, union mqtt_packet *pkt) {

    struct sol_client *c = cb->obj;

    sol_debug("Received UNSUBSCRIBE from %s", c->client_id);

    pkt->ack = *mqtt_packet_ack(UNSUBACK, pkt->unsubscribe.pkt_id);

    unsigned char *packed = pack_mqtt_packet(pkt, UNSUBACK_TYPE);
    cb->payload = bytestring_create(MQTT_ACK_LEN);
    memcpy(cb->payload->data, packed, MQTT_ACK_LEN);

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

    struct topic *t = sol_topic_get(&sol, (const char *) pkt->publish.topic);

    if (!t) {
        t = topic_create((const char *) pkt->publish.topic);
        sol_topic_put(&sol, t);
    }

    unsigned char *pub = pack_mqtt_packet(pkt, PUBLISH_TYPE);

    size_t publen = MQTT_HEADER_LEN + sizeof(uint16_t) +
        pkt->publish.topiclen + pkt->publish.payloadlen;

    if (pkt->publish.header.bits.qos > AT_MOST_ONCE)
        publen += sizeof(uint16_t);

    struct bytestring *payload = bytestring_create(publen);
    memcpy(payload->data, pub, publen);

    struct list_node *cur = t->subscribers->head;
    for (; cur; cur = cur->next) {
        struct sol_client *sc = cur->data;
        ssize_t sent;
        if ((sent = send_bytes(sc->fd, payload->data, payload->size)) < 0)
            sol_error("server::publish_handler %s", strerror(errno));

        // Update information stats
        info.noutputbytes += sent;

        sol_debug("Sending PUBLISH to %s (d%i, q%u, r%i, m%u, %s, ... (%i bytes))",
                  sc->client_id,
                  pkt->publish.header.bits.dup,
                  pkt->publish.header.bits.qos,
                  pkt->publish.header.bits.retain,
                  pkt->publish.pkt_id,
                  pkt->publish.topic,
                  pkt->publish.payloadlen);
    }

    // TODO free publish

    if (pkt->publish.header.bits.qos == AT_LEAST_ONCE) {

        mqtt_puback *puback = mqtt_packet_ack(PUBACK, pkt->publish.pkt_id);

        pkt->ack = *puback;

        unsigned char *packed = pack_mqtt_packet(pkt, PUBACK_TYPE);
        cb->payload = bytestring_create(MQTT_ACK_LEN);
        memcpy(cb->payload->data, packed, MQTT_ACK_LEN);

        sol_debug("Sending PUBACK to %s", c->client_id);

        return REARM_W;

    } else if (pkt->publish.header.bits.qos == EXACTLY_ONCE) {

        // TODO add to a hashtable to track PUBREC clients last

        mqtt_pubrec *pubrec = mqtt_packet_ack(PUBREC, pkt->publish.pkt_id);

        pkt->ack = *pubrec;

        unsigned char *packed = pack_mqtt_packet(pkt, PUBREC_TYPE);
        cb->payload = bytestring_create(MQTT_ACK_LEN);
        memcpy(cb->payload->data, packed, MQTT_ACK_LEN);

        sol_debug("Sending PUBREC to %s", c->client_id);

        return REARM_W;

    }

    /*
     * We're in the case of AT_MOST_ONCE QoS level, we don't need to sent out
     * any byte, it's a fire-and-forget.
     */
    return REARM_R;
}


static int pubrel_handler(struct closure *cb, union mqtt_packet *pkt) {

    sol_debug("Received PUBREL from %s",
              ((struct sol_client *) cb->obj)->client_id);

    mqtt_pubcomp *pubcomp = mqtt_packet_ack(PUBCOMP, pkt->publish.pkt_id);

    pkt->ack = *pubcomp;

    unsigned char *packed = pack_mqtt_packet(pkt, PUBCOMP_TYPE);
    cb->payload = bytestring_create(MQTT_ACK_LEN);
    memcpy(cb->payload->data, packed, MQTT_ACK_LEN);

    sol_debug("Sending PUBCOMP to %s",
              ((struct sol_client *) cb->obj)->client_id);

    return REARM_W;
}


static void on_write(struct evloop *loop, void *arg) {

    struct closure *cb = arg;

    ssize_t sent;
    if ((sent = send_bytes(cb->fd, cb->payload->data, cb->payload->size)) < 0)
        sol_error("server::write_handler %s", strerror(errno));

    // Update information stats
    info.noutputbytes += sent;

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
    unsigned flags;

    /*
     * We must read all incoming bytes till an entire packet is received. This
     * is achieved by following the MQTT v3.1.1 protocol specifications, which
     * send the size of the remaining packet as the second byte. By knowing it
     * we know if the packet is ready to be deserialized and used.
     */
    bytes = recv_packet(cb->fd, buffer, &flags);

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

    /*
     * Unpack received bytes into a mqtt_packet structure and execute the
     * correct handler based on the type of the operation.
     */
    union mqtt_packet packet;
    unpack_mqtt_packet(buffer, &packet);

    union mqtt_header hdr = { .byte = flags };

    /* Execute command callback */
    int rc = handlers[hdr.bits.type](cb, &packet);

    if (rc == REARM_W) {
        cb->call = on_write;

        /*
         * Reset handler to read_handler in order to read new incoming data and
         * EPOLL event for read fds
         */
        evloop_rearm_callback_write(loop, cb);
    } else {
        cb->call = on_read;
        evloop_rearm_callback_read(loop, cb);
    }

exit:

    sol_free(buffer);

    return;

errdc:

    sol_free(buffer);

    sol_error("Dropping client");
    shutdown(cb->fd, 0);
    close(cb->fd);

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
    struct closure *client_cb = sol_malloc(sizeof(*client_cb));
    if (!client_cb)
        return;

    /* Populate client structure */
    client_cb->fd = conn.fd;
    client_cb->obj = NULL;
    client_cb->payload = NULL;
    client_cb->args = client_cb;
    client_cb->call = on_read;

    /* Add it to the epoll loop */
    evloop_add_callback(loop, client_cb);

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

    trie_init(&sol.topics);
    sol.clients = hashtable_create(client_destructor);

    struct closure server_cb;

    /* Initialize the sockets, first the server one */
    server_cb.fd = make_listen(addr, port, conf->socket_family);
    server_cb.payload = NULL;
    server_cb.args = &server_cb;
    server_cb.call = on_accept;

    struct evloop *event_loop = evloop_create(EPOLL_MAX_EVENTS, EPOLL_TIMEOUT);

    /* Set socket in EPOLLIN flag mode, ready to read data */
    evloop_add_callback(event_loop, &server_cb);

    sol_info("Server start");

    run(event_loop);

    hashtable_release(sol.clients);

    sol_info("Sol v%s exiting", VERSION);

    return 0;
}
