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


static struct sol_info info;


/*
 * Connection structure for private use of the module, mainly for accepting
 * new connections
 */
struct connection {
    char ip[INET_ADDRSTRLEN + 1];
    int fd;
};

/*
 * Fixed size of the header of each packet, consists of essentially the first 2
 * bytes containing respectively the type of packet the total length in bytes
 * of the packet and the is_bulk flag which tell if the packet contains a
 * stream of commands or a single one
 */
static const int HEADLEN = 2;

/*
 * Parse packet header, it is required at least the first 5 bytes in order to
 * read packet type and total length that we need to recv to complete the
 * packet.
 *
 * This function accept a socket fd, a ringbuffer to read incoming streams of
 * bytes and a structure formed by 3 fields:
 *
 * - opcode -> to set the OPCODE set in the incoming header, for simplicity
 *             and convenience of the caller
 * - buf -> a byte buffer, it will be malloc'ed in the function and it will
 *          contain the serialized bytes of the incoming packet
 * - flags -> flags pointer, copy the flag setting of the incoming packet,
 *            again for simplicity and convenience of the caller.
 */
static int recv_packet(int clientfd, Ringbuffer *rbuf) {

    ssize_t n = 0;
    int rc = 0;

    while (n < HEADLEN) {
        /* Read first 5 bytes to get the total len of the packet */
        n += recvbytes(clientfd, rbuf, HEADLEN);
        if (n <= 0)
            return -ERRCLIENTDC;
    }

    unsigned char *tmp = sol_malloc(ringbuf_size(rbuf));
    unsigned char *bytearray = tmp;

    /* Try to read at least length of the packet */
    for (int i = 0; i < HEADLEN; i++)
        ringbuf_pop(rbuf, bytearray++);

    /* Read message flags, the first byte of every packet */
    unsigned char byte = *tmp;

    /* If opcode is not known close the connection */
    if (DISCONNECT < byte || CONNECT > byte ) {
        rc = -ERRPACKETERR;
        goto errrecv;
    }

    /* Read the total length of the packet */
    int tlen = ntohl(*((int *) (tmp + sizeof(unsigned char))));

    /*
     * Set return code to -ERRMAXREQSIZE in case the total packet len exceeds
     * the configuration limit `max_request_size`
     */
    if (tlen > (int) conf->max_request_size) {
        rc = -ERRMAXREQSIZE;
        goto err;
    }

    /* Read remaining bytes to complete the packet */
    while ((int) ringbuf_size(rbuf) < tlen - HEADLEN)
        if ((n = recvbytes(clientfd, rbuf, tlen - HEADLEN)) < 0)
            goto errrecv;

    /* Allocate a buffer to fit the entire packet */
    struct bytestring *bstring = bytestring_create(tlen);

    /* Copy previous read part of the header (first 6 bytes) */
    memcpy(bstring->data, tmp, HEADLEN);

    /* Move forward pointer after HEADLEN bytes */
    bytearray = bstring->data + HEADLEN;

    /* Empty the rest of the ring buffer */
    while ((tlen - HEADLEN) > 0) {
        ringbuf_pop(rbuf, bytearray++);
        --tlen;
    }

    sol_free(tmp);

    return 0;

errrecv:

    shutdown(clientfd, 0);
    close(clientfd);

err:

    sol_free(tmp);

    return rc;
}

/* Handle incoming requests, after being accepted or after a reply */
void on_read(struct evloop *loop, void *arg) {

    struct cb *callback = arg;

    /*
     * Raw bytes buffer to initialize the ring buffer, used to handle input
     * from client
     */
    unsigned char *buffer = sol_malloc(conf->max_request_size);

    /*
     * Ringbuffer pointer struct, helpful to handle different and unknown
     * size of chunks of data which can result in partially formed packets or
     * overlapping as well
     */
    Ringbuffer *rbuf = ringbuf_create(buffer, conf->max_request_size);

    int rc = 0;

    /*
     * We must read all incoming bytes till an entire packet is received. This
     * is achieved by using a custom protocol, which send the size of the
     * complete packet as the first 4 bytes. By knowing it we know if the
     * packet is ready to be deserialized and used.
     */
    rc = recv_packet(callback->fd, rbuf);

    /*
     * Looks like we got a client disconnection.
     * TODO: Set a error_handler for ERRMAXREQSIZE instead of dropping client
     *       connection, explicitly returning an informative error code to the
     *       client connected.
     */
    if (rc == -ERRCLIENTDC || rc == -ERRMAXREQSIZE) {
        ringbuf_release(rbuf);
        sol_free(buffer);
        goto errclient;
    }

    /*
     * If not correct packet received, we must free ringbuffer and reset the
     * handler to the request again, setting EPOLL to EPOLLIN
     */
    if (rc == -ERRPACKETERR)
        goto freebuf;

    /*
     * Currently we have a stream of bytes, we want to unpack them into a
     * request structure or a response structure
     */

    /*
     * Reset handler to read_handler in order to read new incoming data and
     * EPOLL event for read fds
     */
    mod_epoll(loop->epollfd, callback->fd, EPOLLOUT, callback);

    /* Free ring buffer as we alredy have all needed informations in memory */
    ringbuf_release(rbuf);

    sol_free(buffer);

exit:

    return;

freebuf:

    ringbuf_release(rbuf);
    sol_free(buffer);

reset:

    /*
     * Free ring buffer as we alredy have all needed informations
     * in memory
     */
    ringbuf_release(rbuf);

    sol_free(buffer);

    callback->callback = on_read;
    mod_epoll(loop->epollfd, callback->fd, EPOLLIN, callback);
    return;

errclient:

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
void on_accept(struct evloop *loop, void *arg) {

    struct connection *server_conn = arg;
    struct connection conn;

    accept_new_client(server_conn->fd, &conn);

    /* Create a client structure to handle his context connection */
    struct cb *client_cb = sol_malloc(sizeof(*client_cb));
    if (!client_cb)
        return;

    /* Populate client structure */
    client_cb->fd = conn.fd;
    client_cb->payload = NULL;
    client_cb->args = client_cb;
    client_cb->callback = on_read;

    /* Add it to the epoll loop */
    add_epoll(loop->epollfd, conn.fd, EPOLLIN, client_cb);

    /* Rearm server fd to accept new connections */
    mod_epoll(loop->epollfd, server_conn->fd, EPOLLIN, server_conn);

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


int start_server(const char *addr, const char *port) {

    struct cb server_cb;

    /* Initialize the sockets, first the server one */
    server_cb.fd = make_listen(addr, port, conf->socket_family);
    server_cb.payload = NULL;
    server_cb.args = &(struct connection) { .fd = server_cb.fd };
    server_cb.callback = on_accept;

    struct evloop *event_loop = evloop_create(EPOLL_MAX_EVENTS, EPOLL_TIMEOUT);

    /* Set socket in EPOLLIN flag mode, ready to read data */
    add_epoll(event_loop->epollfd, server_cb.fd, EPOLLIN, &server_cb);

    sol_info("Server start");

    run(event_loop);

    return 0;
}
