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

#ifndef MQTT_H
#define MQTT_H

#include "types.h"

// Packing/unpacking error codes
#define MQTT_OK                            0
#define MQTT_ERR                           1

#define MQTT_HEADER_LEN                    2
#define MQTT_ACK_LEN                       4
#define MQTT_CLIENT_ID_LEN                 24 // including nul char

// Return codes for connect packet
#define MQTT_CONNECTION_ACCEPTED           0x00
#define MQTT_UNACCEPTABLE_PROTOCOL_VERSION 0x01
#define MQTT_IDENTIFIER_REJECTED           0x02
#define MQTT_SERVER_UNAVAILABLE            0x03
#define MQTT_BAD_CREDENTIALS               0x04
#define MQTT_NOT_AUTHORIZED                0x05

/*
 * Stub bytes, useful for generic replies, these represent the first byte in
 * the fixed header
 */
#define CONNACK_B                          0x20
#define PUBLISH_B                          0x30
#define PUBACK_B                           0x40
#define PUBREC_B                           0x50
#define PUBREL_B                           0x62
#define PUBCOMP_B                          0x70
#define SUBACK_B                           0x90
#define UNSUBACK_B                         0xB0
#define PINGRESP_B                         0xD0

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

/*
 * MQTT Fixed header, according to official docs it's comprised of a single
 * byte carrying:
 * - opcode (packet type)
 * - dup flag
 * - QoS
 * - retain flag
 * It's followed by the remaining_len of the packet, encoded onto 1 to 4
 * bytes starting at bytes 2.
 *
 * |   Bit      |  7  |  6  |  5  |  4  |  3  |  2  |  1  |   0    |
 * |------------|-----------------------|--------------------------|
 * | Byte 1     |      MQTT type 3      | dup |    QoS    | retain |
 * |------------|--------------------------------------------------|
 * | Byte 2     |                                                  |
 * |   .        |               Remaining Length                   |
 * |   .        |                                                  |
 * | Byte 5     |                                                  |
 * |------------|--------------------------------------------------|
 */
typedef union mqtt_header {
    u8 byte;
    struct {
        u8 retain : 1;
        u8 qos : 2;
        u8 dup : 1;
        u8 type : 4;
    } bits;
} MQTT_Header;

/*
 * MQTT Connect packet, contains a variable header with some connect related
 * flags:
 * - clean session flag
 * - will flag
 * - will QoS (if will flag set to true)
 * - will topic (if will flag set to true)
 * - will retain flag (if will flag set to true)
 * - password flag
 * - username flag
 *
 * It's followed by all required fields according the flags set to true.
 *
 * |------------|--------------------------------------------------|
 * | Byte 6     |             Protocol name len MSB                |
 * | Byte 7     |             Protocol name len LSB                |  [UINT16]
 * |------------|--------------------------------------------------|
 * | Byte 8     |                                                  |
 * |   .        |                'M' 'Q' 'T' 'T'                   |
 * | Byte 12    |                                                  |
 * |------------|--------------------------------------------------|
 * | Byte 13    |                 Protocol level                   |
 * |------------|--------------------------------------------------|
 * |            |                 Connect flags                    |
 * | Byte 14    |--------------------------------------------------|
 * |            |  U  |  P  |  WR |     WQ    |  WF |  CS |    R   |
 * |------------|--------------------------------------------------|
 * | Byte 15    |                 Keepalive MSB                    |  [UINT16]
 * | Byte 17    |                 Keepalive LSB                    |
 * |------------|--------------------------------------------------|<-- Payload
 * | Byte 18    |             Client ID length MSB                 |  [UINT16]
 * | Byte 19    |             Client ID length LSB                 |
 * |------------|--------------------------------------------------|
 * | Byte 20    |                                                  |
 * |   .        |                  Client ID                       |
 * | Byte N     |                                                  |
 * |------------|--------------------------------------------------|
 * | Byte N+1   |              Username length MSB                 |
 * | Byte N+2   |              Username length LSB                 |
 * |------------|--------------------------------------------------|
 * | Byte N+3   |                                                  |
 * |   .        |                  Username                        |
 * | Byte N+M   |                                                  |
 * |------------|--------------------------------------------------|
 * | Byte N+M+1 |              Password length MSB                 |
 * | Byte N+M+2 |              Password length LSB                 |
 * |------------|--------------------------------------------------|
 * | Byte N+M+3 |                                                  |
 * |   .        |                  Password                        |
 * | Byte N+M+K |                                                  |
 * |------------|--------------------------------------------------|
 */
typedef struct mqtt_connect {
    union {
        u8 byte;
        struct {
            u8 reserved : 1;
            u8 clean_session : 1;
            u8 will : 1;
            u8 will_qos : 2;
            u8 will_retain : 1;
            u8 password : 1;
            u8 username : 1;
        } bits;
    };
    struct {
        u16 keepalive;
        u8 client_id[MQTT_CLIENT_ID_LEN];
        u8 *username;
        u8 *password;
        u8 *will_topic;
        u8 *will_message;
    } payload;
} MQTT_Connect;

typedef struct mqtt_connack {
    union {
        u8 byte;
        struct {
            u8 session_present : 1;
            u8 reserved : 7;
        } bits;
    };
    u8 rc;
} MQTT_Connack;

typedef struct mqtt_subscribe {
    u16 id;
    u16 tuples_len;
    struct {
        u8 qos;
        u16 topic_len;
        u8 *topic;
    } *tuples;
} MQTT_Subscribe;

typedef struct mqtt_unsubscribe {
    u16 id;
    u16 tuples_len;
    struct {
        u16 topic_len;
        u8 *topic;
    } *tuples;
} MQTT_Unsubscribe;

typedef struct mqtt_suback {
    u16 id;
    u16 rcslen;
    u8 *rcs;
} MQTT_Suback;

typedef struct mqtt_publish {
    u16 id;
    u16 topiclen;
    u8 *topic;
    u32 payloadlen;
    u8 *payload;
} MQTT_Publish;

typedef struct mqtt_ack {
    u16 id;
} MQTT_Ack;

typedef struct mqtt_ack mqtt_puback;
typedef struct mqtt_ack mqtt_pubrec;
typedef struct mqtt_ack mqtt_pubrel;
typedef struct mqtt_ack mqtt_pubcomp;
typedef struct mqtt_ack mqtt_unsuback;
typedef MQTT_Header mqtt_pingreq;
typedef MQTT_Header mqtt_pingresp;
typedef MQTT_Header mqtt_disconnect;

typedef struct mqtt_packet {
    MQTT_Header header;
    union {
        // This will cover PUBACK, PUBREC, PUBREL, PUBCOMP and UNSUBACK
        MQTT_Ack ack;
        // This will cover PINGREQ, PINGRESP and DISCONNECT
        mqtt_pingreq pingreq;
        MQTT_Connect connect;
        MQTT_Connack connack;
        MQTT_Suback suback;
        MQTT_Publish publish;
        MQTT_Subscribe subscribe;
        MQTT_Unsubscribe unsubscribe;
    };
} MQTT_Packet;

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
int mqtt_write_length(u8 *, usize);

/*
 * The reverse of the encoding function, returns the value of the size decoded
 */
usize mqtt_read_length(u8 *, unsigned *);

/*
 * Pack to binary an MQTT packet, internally it uses a dispatch table to call
 * the right pack function based on the packet opcode.
 */
typedef struct arena_allocator Arena_Allocator;
int mqtt_read(u8 *, MQTT_Packet *, u8, usize, Arena_Allocator *allocator);

/*
 * Unpack from binary to an mqtt_packet structure. Internally it uses a
 * dispatch table to call the right unpack function based on the opcode
 * expected to read.
 */
usize mqtt_write(const MQTT_Packet *, u8 *);

/*
 * MQTT Build helpers
 *
 * They receive a pointer to a struct mqtt_packet and additional informations
 * to be stored inside. Just plain builder functions.
 */
void mqtt_ack(MQTT_Packet *, u16);

void mqtt_connack(MQTT_Packet *, u8, u8);

void mqtt_suback(MQTT_Packet *, u16, u8 *, u16);

void mqtt_publish(MQTT_Packet *, u16, usize, u8 *, usize, u8 *);

/*
 * Release the memory allocated through helpers function calls based on the
 * opcode of the MQTT packet passed
 */
void mqtt_packet_free(MQTT_Packet *);

void mqtt_set_dup(MQTT_Packet *);

/*
 * Helper function used to pack ACK packets, mono as the single field `packet
 * identifier` that ACKs packet carries
 */
int mqtt_write_ack(u8 *, u8, u16);

/*
 * Returns the size of a packet. Useful to pack functions to know the expected
 * buffer size of the packet based on the opcode. Accept an optional pointer
 * to get the len reserved for storing the remaining length of the full packet
 */
usize mqtt_size(const MQTT_Packet *, usize *);

typedef struct pool_allocator Pool_Allocator;

/*
 * Allocate struct mqtt_packet on the heap. This should be used in place of
 * malloc/calloc in order to leverage the refcounter
 */
MQTT_Packet *mqtt_packet_alloc(u8, Pool_Allocator *);

#endif
