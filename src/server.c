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


typedef void callback(struct callback_obj *, union mqtt_packet *);


static void on_read(struct evloop *, void *);

static void on_connect(struct callback_obj *, union mqtt_packet *);

static void on_disconnect(struct callback_obj *, union mqtt_packet *);

static void on_subscribe(struct callback_obj *, union mqtt_packet *);

static void on_publish(struct callback_obj *, union mqtt_packet *);


static callback *callbacks[15] = {
    NULL,
    on_connect,
    NULL,
    on_publish,
    NULL,
    NULL,
    NULL,
    NULL,
    on_subscribe,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    on_disconnect
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


static void on_connect(struct callback_obj *cb, union mqtt_packet *pkt) {
    printf("Command %u retain: %i qos: %u dup: %i type: %u\n",
           pkt->connect.header.byte,
           pkt->connect.header.bits.retain,
           pkt->connect.header.bits.qos,
           pkt->connect.header.bits.dup,
           pkt->connect.header.bits.type);

    printf("Flags: %u %u %u %u %u %u %u\n",
           pkt->connect.byte,
           pkt->connect.bits.clean_session,
           pkt->connect.bits.password,
           pkt->connect.bits.username,
           pkt->connect.bits.will,
           pkt->connect.bits.will_qos,
           pkt->connect.bits.will_retain);

    printf("Keepalive: %u\n", pkt->connect.payload.keepalive);

    printf("Payload: %s %s\n",
           pkt->connect.payload.username,
           pkt->connect.payload.password);

    /*
     * Add the new connected client to the global map, if it is already
     * connected, kick him out accordingly to the MQTT v3.1.1 specs.
     */
    struct sol_client *new_client = sol_malloc(sizeof(*new_client));
    new_client->fd = cb->fd;
    const char *cid = (const char *) pkt->connect.payload.client_id;
    new_client->client_id = sol_strdup(cid);
    hashtable_put(sol.clients, cid, new_client);

    /* Substitute fd on callback with callback obj */
    cb->obj = new_client;

    //TODO kick already connected clients

    /* Respond with a connack */
    union mqtt_packet *response = sol_malloc(sizeof(*response));
    char data[MQTT_HEADER_LEN];
    unsigned char *pdata = (unsigned char *) &data[0];
    pack_u8(&pdata, 0);
    pack_u8(&pdata, 0);
    response->connack = *mqtt_packet_connack(0, data);

    cb->payload = bytestring_create(MQTT_ACK_LEN);
    unsigned char *p = pack_mqtt_packet(response, 2);
    memcpy(cb->payload->data, p, MQTT_ACK_LEN);
}


static void on_disconnect(struct callback_obj *cb, union mqtt_packet *pkt) {
  /* Handle disconnection request from client */
    struct sol_client *c = cb->obj;
    close(c->fd);
    hashtable_del(sol.clients, c->client_id);
    // TODO remove from all topic where it subscribed
}


static void on_subscribe(struct callback_obj *cb, union mqtt_packet *pkt) {
    printf("Command %u retain: %i qos: %u dup: %i type: %u\n",
           pkt->subscribe.header.byte,
           pkt->subscribe.header.bits.retain,
           pkt->subscribe.header.bits.qos,
           pkt->subscribe.header.bits.dup,
           pkt->subscribe.header.bits.type);

    printf("Packet ID: %u\n", pkt->subscribe.pkt_id);

    for (unsigned i = 0; i < pkt->subscribe.tuples_len; i++) {
        printf("Topic: %s qos %u\n",
               pkt->subscribe.tuples[i].topic,
               pkt->subscribe.tuples[i].qos);

        /*
         * Check if the topic exists already or in case create it and store in
         * the global map
         */
        const char *topic_name = (const char *) pkt->subscribe.tuples[i].topic;
        struct topic *t = sol_topic_get(&sol, topic_name);

        // TODO check for callback correctly set to obj

        if (!t) {
            struct topic *newt = topic_create(topic_name);
            sol_topic_put(&sol, newt);
            topic_add_subscriber(newt, cb->obj);
        } else {
            topic_add_subscriber(t, cb->obj);
        }
    }

}


static void on_publish(struct callback_obj *cb, union mqtt_packet *pkt) {
    printf("Command %u retain: %i qos: %u dup: %i type: %u\n",
           pkt->publish.header.byte,
           pkt->publish.header.bits.retain,
           pkt->publish.header.bits.qos,
           pkt->publish.header.bits.dup,
           pkt->publish.header.bits.type);

    printf("Packet ID: %u\n", pkt->publish.pkt_id);

    printf("Topic %s Payload %s\n",
           pkt->publish.topic,
           pkt->publish.payload);
}


static void on_write(struct evloop *loop, void *arg) {

    struct callback_obj *callback = arg;

    ssize_t sent;
    if ((sent = send_bytes(callback->fd, callback->payload->data,
                          callback->payload->size)) < 0)
        sol_error("server::write_handler %s", strerror(errno));

    // Update information stats
    info.noutputbytes += sent;

    /*
     * Re-arm callback by setting EPOLL event on EPOLLIN to read fds and
     * re-assigning the callback `on_read` for the next event
     */
    callback->callback = on_read;
    evloop_rearm_callback_read(loop, callback);

    printf("Sent %ldb\n", sent);
}

/* Handle incoming requests, after being accepted or after a reply */
static void on_read(struct evloop *loop, void *arg) {

    struct callback_obj *callback = arg;

    /* Raw bytes buffer to handle input from client */
    unsigned char *buffer = sol_malloc(conf->max_request_size);

    ssize_t bytes = 0;
    unsigned flags;

    /*
     * We must read all incoming bytes till an entire packet is received. This
     * is achieved by using a custom protocol, which send the size of the
     * complete packet as the first 4 bytes. By knowing it we know if the
     * packet is ready to be deserialized and used.
     */
    bytes = recv_packet(callback->fd, buffer, &flags);

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
    callbacks[hdr.bits.type](callback, &packet);

    callback->callback = on_write;

    /*
     * Reset handler to read_handler in order to read new incoming data and
     * EPOLL event for read fds
     */
    evloop_rearm_callback_write(loop, callback);

exit:

    sol_free(buffer);

    return;

errdc:

    sol_free(buffer);

    sol_error("Dropping client");
    shutdown(callback->fd, 0);
    close(callback->fd);

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
    struct callback_obj *server = arg;
    struct connection conn;

    accept_new_client(server->fd, &conn);

    /* Create a client structure to handle his context connection */
    struct callback_obj *client_cb = sol_malloc(sizeof(*client_cb));
    if (!client_cb)
        return;

    /* Populate client structure */
    client_cb->fd = conn.fd;
    client_cb->obj = NULL;
    client_cb->payload = NULL;
    client_cb->args = client_cb;
    client_cb->callback = on_read;

    /* Add it to the epoll loop */
    evloop_add_callback(loop, client_cb);

    /* Rearm server fd to accept new connections */
    evloop_rearm_callback_read(loop, server);

    /* Record the new client connected */
    info.nclients++;
    info.nconnections++;

    sol_debug("Client connection from %s", conn.ip);
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

    struct callback_obj server_cb;

    /* Initialize the sockets, first the server one */
    server_cb.fd = make_listen(addr, port, conf->socket_family);
    server_cb.payload = NULL;
    server_cb.args = &server_cb;
    server_cb.callback = on_accept;

    struct evloop *event_loop = evloop_create(EPOLL_MAX_EVENTS, EPOLL_TIMEOUT);

    /* Set socket in EPOLLIN flag mode, ready to read data */
    evloop_add_callback(event_loop, &server_cb);

    sol_info("Server start");

    run(event_loop);

    hashtable_release(sol.clients);

    return 0;
}
