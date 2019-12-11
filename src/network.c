/*
 * BSD 2-Clause License
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

#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <sys/un.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/eventfd.h>
#include <openssl/err.h>
#include "util.h"
#include "config.h"
#include "network.h"

/* Set non-blocking socket */
int set_nonblocking(int fd) {
    int flags, result;
    flags = fcntl(fd, F_GETFL, 0);

    if (flags == -1)
        goto err;

    result = fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    if (result == -1)
        goto err;

    return 0;

err:

    perror("set_nonblocking");
    return -1;
}

/* Disable Nagle's algorithm by setting TCP_NODELAY */
int set_tcp_nodelay(int fd) {
    return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &(int) {1}, sizeof(int));
}

static int create_and_bind_unix(const char *sockpath) {

    struct sockaddr_un addr;
    int fd;

    if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
        perror("socket error");
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;

    strncpy(addr.sun_path, sockpath, sizeof(addr.sun_path) - 1);
    unlink(sockpath);

    if (bind(fd, (struct sockaddr*) &addr, sizeof(addr)) == -1) {
        perror("bind error");
        return -1;
    }

    return fd;
}

static int create_and_bind_tcp(const char *host, const char *port) {

    struct addrinfo hints = {
        .ai_family = AF_UNSPEC,
        .ai_socktype = SOCK_STREAM,
        .ai_flags = AI_PASSIVE
    };

    struct addrinfo *result, *rp;
    int sfd;

    if (getaddrinfo(host, port, &hints, &result) != 0)
        goto err;

    for (rp = result; rp != NULL; rp = rp->ai_next) {
        sfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);

        if (sfd == -1) continue;

        /* set SO_REUSEADDR so the socket will be reusable after process kill */
        if (setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR,
                       &(int) { 1 }, sizeof(int)) < 0)
            perror("SO_REUSEADDR");

        if ((bind(sfd, rp->ai_addr, rp->ai_addrlen)) == 0) {
            /* Succesful bind */
            break;
        }
        close(sfd);
    }

    freeaddrinfo(result);

    if (rp == NULL)
        goto err;

    return sfd;

err:

    perror("Unable to bind socket");
    return -1;
}

int create_and_bind(const char *host, const char *port, int socket_family) {

    int fd;

    if (socket_family == UNIX)
        fd = create_and_bind_unix(host);
    else
        fd = create_and_bind_tcp(host, port);

    return fd;
}

/*
 * Create a non-blocking socket and make it listen on the specfied address and
 * port
 */
int make_listen(const char *host, const char *port, int socket_family) {

    int sfd;

    if ((sfd = create_and_bind(host, port, socket_family)) == -1)
        abort();

    if ((set_nonblocking(sfd)) == -1)
        abort();

    // Set TCP_NODELAY only for TCP sockets
    if (socket_family == INET)
        set_tcp_nodelay(sfd);

    if ((listen(sfd, conf->tcp_backlog)) == -1) {
        perror("listen");
        abort();
    }

    return sfd;
}

int accept_connection(int serversock, char *ip) {

    int clientsock;
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);

    if ((clientsock = accept(serversock,
                             (struct sockaddr *) &addr, &addrlen)) < 0)
        return -1;

    set_nonblocking(clientsock);

    // Set TCP_NODELAY only for TCP sockets
    if (conf->socket_family == INET)
        set_tcp_nodelay(clientsock);

    char ip_buff[INET_ADDRSTRLEN + 1];
    if (inet_ntop(AF_INET, &addr.sin_addr,
                  ip_buff, sizeof(ip_buff)) == NULL) {
        close(clientsock);
        return -1;
    }

    if (ip)
        strncpy(ip, ip_buff, INET_ADDRSTRLEN + 1);

    return clientsock;
}

/* Send all bytes contained in buf, updating sent bytes counter */
ssize_t send_bytes(int fd, const unsigned char *buf, size_t len) {

    size_t total = 0;
    size_t bytesleft = len;
    ssize_t n = 0;

    while (total < len) {
        n = send(fd, buf + total, bytesleft, MSG_NOSIGNAL);
        if (n == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            else
                goto err;
        }
        total += n;
        bytesleft -= n;
    }

    return total;

err:

    fprintf(stderr, "send(2) - error sending data: %s\n", strerror(errno));
    return -1;
}

/*
 * Receive a given number of bytes on the descriptor fd, storing the stream of
 * data into a 2 Mb capped buffer
 */
ssize_t recv_bytes(int fd, unsigned char *buf, size_t bufsize) {

    ssize_t n = 0;
    ssize_t total = 0;

    while (total < (ssize_t) bufsize) {

        if ((n = recv(fd, buf, bufsize - total, 0)) < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            } else
                goto err;
        }

        if (n == 0)
            return 0;

        buf += n;
        total += n;
    }

    return total;

err:

    fprintf(stderr, "recv(2) - error reading data: %s\n", strerror(errno));
    return -1;
}

int epoll_add(int efd, int fd, int evs, void *data) {

    struct epoll_event ev;
    ev.data.fd = fd;

    // Being ev.data a union, in case of data != NULL, fd will be set to random
    if (data)
        ev.data.ptr = data;

    ev.events = evs | EPOLLET | EPOLLONESHOT;

    return epoll_ctl(efd, EPOLL_CTL_ADD, fd, &ev);
}

int epoll_mod(int efd, int fd, int evs, void *data) {

    struct epoll_event ev;
    ev.data.fd = fd;

    // Being ev.data a union, in case of data != NULL, fd will be set to random
    if (data)
        ev.data.ptr = data;

    ev.events = evs | EPOLLET | EPOLLONESHOT;

    return epoll_ctl(efd, EPOLL_CTL_MOD, fd, &ev);
}

int epoll_del(int efd, int fd) {
    return epoll_ctl(efd, EPOLL_CTL_DEL, fd, NULL);
}

int add_cron_task(int epollfd, const struct itimerspec *timervalue) {

    int timerfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);

    if (timerfd_settime(timerfd, 0, timervalue, NULL) < 0)
        goto err;

    // Add the timer to the event loop
    struct epoll_event ev;
    ev.data.fd = timerfd;
    ev.events = EPOLLIN;

    if (epoll_ctl(epollfd, EPOLL_CTL_ADD, timerfd, &ev) < 0)
        goto err;

    return timerfd;

err:

    perror("add_cron_task");
    return -1;
}

void openssl_init() {
    SSL_library_init();
    ERR_load_crypto_strings();
    SSL_load_error_strings();
    OpenSSL_add_ssl_algorithms();
}


void openssl_cleanup() {
    EVP_cleanup();
}


SSL_CTX *create_ssl_context() {

    SSL_CTX *ctx;

    ctx = SSL_CTX_new(TLS_method());
    if (!ctx) {
        perror("Unable to create SSL context");
        ERR_print_errors_fp(stderr);
        exit(EXIT_FAILURE);
    }

    SSL_CTX_set_options(ctx, SSL_OP_NO_SSLv2|SSL_OP_NO_SSLv3);

    return ctx;
}

static int client_certificate_verify(int preverify_ok, X509_STORE_CTX *ctx) {

    (void) ctx;

	/* Preverify should check expiry, revocation. */
	return preverify_ok;
}

void load_certificates(SSL_CTX *ctx, const char *ca,
                       const char *cert, const char *key) {

    if (SSL_CTX_load_verify_locations(ctx, ca, NULL) <= 0) {
        ERR_print_errors_fp(stderr);
        exit(EXIT_FAILURE);
    }

    SSL_CTX_set_mode(ctx, SSL_MODE_ENABLE_PARTIAL_WRITE|SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
	SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, client_certificate_verify);
    SSL_CTX_set_ecdh_auto(ctx, 1);

	if (SSL_CTX_use_certificate_chain_file(ctx, cert) <= 0) {
        ERR_print_errors_fp(stderr);
        exit(EXIT_FAILURE);
    }

    if (SSL_CTX_use_PrivateKey_file(ctx, key, SSL_FILETYPE_PEM) <= 0 ) {
        ERR_print_errors_fp(stderr);
        exit(EXIT_FAILURE);
    }

    /* verify private key */
    if (!SSL_CTX_check_private_key(ctx) ) {
        fprintf(stderr, "Private key does not match the public certificate\n");
        exit(EXIT_FAILURE);
    }
}

ssize_t ssl_send_bytes(SSL *ssl, const unsigned char *buf, size_t len) {

    size_t total = 0;
    size_t bytesleft = len;
    ssize_t n = 0;

    while (total < len) {
        n = SSL_write(ssl, buf + total, bytesleft);
        int err = SSL_get_error(ssl, n);
        if (err == SSL_ERROR_WANT_WRITE)
            continue;
        if (n == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            else
                goto err;
        }
        total += n;
        bytesleft -= n;
    }

    return total;

err:

    fprintf(stderr, "SSL_write(2) - error sending data: %s\n", strerror(errno));
    return -1;
}


ssize_t ssl_recv_bytes(SSL *ssl, unsigned char *buf, size_t bufsize) {

    ssize_t n = 0;
    ssize_t total = 0;

    ERR_clear_error();

    while (total < (ssize_t) bufsize) {

        if ((n = SSL_read(ssl, buf, bufsize - total)) < 0) {

            int err = SSL_get_error(ssl, n);
            if (err == SSL_ERROR_WANT_READ)
                continue;

            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            else
                goto err;
        }

        if (n == 0)
            return 0;

        buf += n;
        total += n;
    }

    return total;

err:

    fprintf(stderr, "SSL_read(2) - error reading data: %s\n", strerror(errno));
    return -1;
}
