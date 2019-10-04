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


#define MQTT_HEADER_LEN 2
#define MQTT_ACK_LEN    4

/*
 * Stub bytes, useful for generic replies, these represent the first byte in
 * the fixed header
 */
#define CONNACK_BYTE  0x20
#define PUBLISH_BYTE  0x30
#define PUBACK_BYTE   0x40
#define PUBREC_BYTE   0x50
#define PUBREL_BYTE   0x60
#define PUBCOMP_BYTE  0x70
#define SUBACK_BYTE   0x90
#define UNSUBACK_BYTE 0xB0
#define PINGRESP_BYTE 0xD0

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

    union mqtt_header header;

    union {

        unsigned char byte;

        struct {
            int reserverd : 1;
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
        unsigned char *client_id;
        unsigned char *username;
        unsigned char *password;
        unsigned char *will_topic;
        unsigned char *will_message;
    } payload;

};


struct mqtt_connack {

    union mqtt_header header;

    union {

        unsigned char byte;

        struct {
            unsigned session_present : 1;
            unsigned reserverd : 7;
        } bits;
    };

    unsigned char rc;
};


struct mqtt_subscribe {

    union mqtt_header header;

    unsigned short pkt_id;

    unsigned short tuples_len;

    struct {
        unsigned short topic_len;
        unsigned char *topic;
        unsigned qos;
    } *tuples;
};


struct mqtt_unsubscribe {

    union mqtt_header header;

    unsigned short pkt_id;

    unsigned short tuples_len;

    struct {
        unsigned short topic_len;
        unsigned char *topic;
    } *tuples;
};


struct mqtt_suback {

    union mqtt_header header;

    unsigned short pkt_id;

    unsigned short rcslen;

    unsigned char *rcs;
};


struct mqtt_publish {

    union mqtt_header header;

    unsigned short pkt_id;

    unsigned short topiclen;
    unsigned char *topic;
    unsigned short payloadlen;
    unsigned char *payload;
};


struct mqtt_ack {

    union mqtt_header header;

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


union mqtt_packet {

    // This will cover PUBACK, PUBREC, PUBREL, PUBCOMP and UNSUBACK
    struct mqtt_ack ack;

    // This will cover PINGREQ, PINGRESP and DISCONNECT
    union mqtt_header header;

    struct mqtt_connect connect;
    struct mqtt_connack connack;
    struct mqtt_suback suback;
    struct mqtt_publish publish;
    struct mqtt_subscribe subscribe;
    struct mqtt_unsubscribe unsubscribe;

};


int mqtt_encode_length(unsigned char *, size_t);

unsigned long long mqtt_decode_length(const unsigned char **);

int unpack_mqtt_packet(const unsigned char *, union mqtt_packet *);

unsigned char *pack_mqtt_packet(const union mqtt_packet *, unsigned);

union mqtt_header *mqtt_packet_header(unsigned char);

struct mqtt_ack *mqtt_packet_ack(unsigned char , unsigned short);

struct mqtt_connack *mqtt_packet_connack(unsigned char ,
                                         unsigned char ,
                                         unsigned char);

struct mqtt_suback *mqtt_packet_suback(unsigned char, unsigned short,
                                       unsigned char *, unsigned short);

struct mqtt_publish *mqtt_packet_publish(unsigned char, unsigned short, size_t,
                                         unsigned char *,
                                         size_t, unsigned char *);

void mqtt_packet_release(union mqtt_packet *, unsigned);


#endif
