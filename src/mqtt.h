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

#ifndef MQTT_H
#define MQTT_H

#include <stdio.h>
#include "ref.h"
#include "pack.h"

// Packing/unpacking error codes
#define MQTT_OK    0
#define MQTT_ERR   1

#define MQTT_HEADER_LEN     2
#define MQTT_ACK_LEN        4
#define MQTT_CLIENT_ID_LEN  24  // including nul char

// Return codes for connect packet
#define MQTT_CONNECTION_ACCEPTED           0x00
#define MQTT_UNACCEPTABLE_PROTOCOL_VERSION 0x01
#define MQTT_IDENTIFIER_REJECTED           0x02
#define MQTT_SERVER_UNAVAILABLE            0x03
#define MQTT_BAD_USERNAME_OR_PASSWORD      0x04
#define MQTT_NOT_AUTHORIZED                0x05

/*
 * Stub bytes, useful for generic replies, these represent the first byte in
 * the fixed header
 */
#define CONNACK_B  0x20
#define PUBLISH_B  0x30
#define PUBACK_B   0x40
#define PUBREC_B   0x50
#define PUBREL_B   0x62
#define PUBCOMP_B  0x70
#define SUBACK_B   0x90
#define UNSUBACK_B 0xB0
#define PINGRESP_B 0xD0

/* Message types */
enum packet_type {
    CONNECT     = 1,
    CONNACK     = 2,
    PUBLISH     = 3,
    PUBACK      = 4,
    PUBREC      = 5,
    PUBREL      = 6,
    PUBCOMP     = 7,
    SUBSCRIBE   = 8,
    SUBACK      = 9,
    UNSUBSCRIBE = 10,
    UNSUBACK    = 11,
    PINGREQ     = 12,
    PINGRESP    = 13,
    DISCONNECT  = 14
};

enum qos_level { AT_MOST_ONCE, AT_LEAST_ONCE, EXACTLY_ONCE };

union mqtt_header {
    unsigned char byte;
    struct {
        unsigned retain : 1;
        unsigned qos : 2;
        unsigned dup : 1;
        unsigned type : 4;
    } bits;
};

struct mqtt_connect {
    union {
        unsigned char byte;
        struct {
            unsigned reserved : 1;
            unsigned clean_session : 1;
            unsigned will : 1;
            unsigned will_qos : 2;
            unsigned will_retain : 1;
            unsigned password : 1;
            unsigned username : 1;
        } bits;
    };
    struct {
        unsigned short keepalive;
        unsigned char client_id[MQTT_CLIENT_ID_LEN];
        unsigned char *username;
        unsigned char *password;
        unsigned char *will_topic;
        unsigned char *will_message;
    } payload;
};

struct mqtt_connack {
    union {
        unsigned char byte;
        struct {
            unsigned session_present : 1;
            unsigned reserved : 7;
        } bits;
    };
    unsigned char rc;
};

struct mqtt_subscribe {
    unsigned short pkt_id;
    unsigned short tuples_len;
    struct {
        unsigned qos;
        unsigned short topic_len;
        unsigned char *topic;
    } *tuples;
};

struct mqtt_unsubscribe {
    unsigned short pkt_id;
    unsigned short tuples_len;
    struct {
        unsigned short topic_len;
        unsigned char *topic;
    } *tuples;
};

struct mqtt_suback {
    unsigned short pkt_id;
    unsigned short rcslen;
    unsigned char *rcs;
};

struct mqtt_publish {
    unsigned short pkt_id;
    unsigned short topiclen;
    unsigned char *topic;
    unsigned long long payloadlen;
    unsigned char *payload;
};

struct mqtt_ack {
    unsigned short pkt_id;
};

typedef struct mqtt_ack mqtt_puback;
typedef struct mqtt_ack mqtt_pubrec;
typedef struct mqtt_ack mqtt_pubrel;
typedef struct mqtt_ack mqtt_pubcomp;
typedef struct mqtt_ack mqtt_unsuback;
typedef union mqtt_header mqtt_pingreq;
typedef union mqtt_header mqtt_pingresp;
typedef union mqtt_header mqtt_disconnect;

struct mqtt_packet {
    union mqtt_header header;
    union {
        // This will cover PUBACK, PUBREC, PUBREL, PUBCOMP and UNSUBACK
        struct mqtt_ack ack;
        // This will cover PINGREQ, PINGRESP and DISCONNECT
        mqtt_pingreq pingreq;
        struct mqtt_connect connect;
        struct mqtt_connack connack;
        struct mqtt_suback suback;
        struct mqtt_publish publish;
        struct mqtt_subscribe subscribe;
        struct mqtt_unsubscribe unsubscribe;
    };
    struct ref refcount;
};

/*
 * Encoding packet length function, follows the OASIS specs, encode the total
 * length of the packet excluding header and the space for the encoding itself
 * into a 1-4 bytes using the continuation bit technique to allow a dynamic
 * storing size:
 * Using the first 7 bits of a byte we can store values till 127 and use the
 * last bit as a switch to notify if the subsequent byte is used to store
 * remaining length or not.
 * Returns the number of bytes used to store the value passed.
 */
int mqtt_encode_length(unsigned char *, size_t);

/*
 * The reverse of the encoding function, returns the value of the size decoded
 */
size_t mqtt_decode_length(unsigned char *, unsigned *);

/*
 * Pack to binary an MQTT packet, internally it uses a dispatch table to call
 * the right pack function based on the packet opcode.
 */
int mqtt_unpack(unsigned char *, struct mqtt_packet *, unsigned char, size_t);

/*
 * Unpack from binary to an mqtt_packet structure. Internally it uses a
 * dispatch table to call the right unpack function based on the opcode
 * expected to read.
 */
size_t mqtt_pack(const struct mqtt_packet *, unsigned char *);

/*
 * MQTT Build helpers
 *
 * They receive a pointer to a struct mqtt_packet and additional informations
 * to be stored inside. Just plain builder functions.
 */
void mqtt_ack(struct mqtt_packet *, unsigned short);

void mqtt_connack(struct mqtt_packet *, unsigned char , unsigned char);

void mqtt_suback(struct mqtt_packet *, unsigned short,
                 unsigned char *, unsigned short);

void mqtt_publish(struct mqtt_packet *, unsigned short, size_t,
                  unsigned char *, size_t, unsigned char *);

/*
 * Release the memory allocated through helpers function calls based on the
 * opcode of the MQTT packet passed
 */
void mqtt_packet_destroy(struct mqtt_packet *, unsigned);

void mqtt_set_dup(struct mqtt_packet *);

/*
 * Helper function used to pack ACK packets, mono as the single field `packet
 * identifier` that ACKs packet carries
 */
int mqtt_pack_mono(unsigned char *, unsigned char, unsigned short);

/*
 * Returns the size of a packet. Useful to pack functions to know the expected
 * buffer size of the packet based on the opcode. Accept an optional pointer
 * to get the len reserved for storing the remaining length of the full packet
 */
size_t mqtt_size(const struct mqtt_packet *, size_t *);

/*
 * Allocate struct mqtt_packet on the heap. This should be used in place of
 * malloc/calloc in order to leverage the refcounter
 */
struct mqtt_packet *mqtt_packet_alloc(unsigned char);

#endif
