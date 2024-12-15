/* BSD 2-Clause License
 *
 * Copyright (c) 2023, Andrea Giacomo Baldan All rights reserved.
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

#include "mqtt.h"
#include "memory.h"
#include "pack.h"
#include "util.h"
#include <string.h>

typedef int mqtt_unpack_handler(u8 *, struct mqtt_packet *, usize);

typedef usize mqtt_pack_handler(const struct mqtt_packet *, u8 *);

static int unpack_mqtt_connect(u8 *, struct mqtt_packet *, usize);

static int unpack_mqtt_publish(u8 *, struct mqtt_packet *, usize);

static int unpack_mqtt_subscribe(u8 *, struct mqtt_packet *, usize);

static int unpack_mqtt_unsubscribe(u8 *, struct mqtt_packet *, usize);

static int unpack_mqtt_ack(u8 *, struct mqtt_packet *, usize);

static usize pack_mqtt_header(const union mqtt_header *, u8 *);

static usize pack_mqtt_ack(const struct mqtt_packet *, u8 *);

static usize pack_mqtt_connack(const struct mqtt_packet *, u8 *);

static usize pack_mqtt_suback(const struct mqtt_packet *, u8 *);

static usize pack_mqtt_publish(const struct mqtt_packet *, u8 *);

/* MQTT v3.1.1 standard */
static const int MAX_LEN_BYTES                  = 4;

/*
 * MQTT v3.1.1 starts every connect packet with 7 bytes for storing the
 * protocol name 'M' 'Q' 'T' 'T' and mqtt properties, for now we just wanna
 * ignore those fields and skip them during packet decoding
 */
static const int SKIP_PROTOCOL_NAME             = 7;

/*
 * Unpack functions mapping unpacking_handlers positioned in the array based
 * on message type
 */
static mqtt_unpack_handler *unpack_handlers[11] = {NULL,
                                                   unpack_mqtt_connect,
                                                   NULL,
                                                   unpack_mqtt_publish,
                                                   unpack_mqtt_ack,
                                                   unpack_mqtt_ack,
                                                   unpack_mqtt_ack,
                                                   unpack_mqtt_ack,
                                                   unpack_mqtt_subscribe,
                                                   NULL,
                                                   unpack_mqtt_unsubscribe};

static mqtt_pack_handler *pack_handlers[13]     = {NULL,
                                                   NULL,
                                                   pack_mqtt_connack,
                                                   pack_mqtt_publish,
                                                   pack_mqtt_ack,
                                                   pack_mqtt_ack,
                                                   pack_mqtt_ack,
                                                   pack_mqtt_ack,
                                                   NULL,
                                                   pack_mqtt_suback,
                                                   NULL,
                                                   pack_mqtt_ack,
                                                   NULL};

/*
 * Encode Remaining Length on a MQTT packet header, comprised of Variable
 * Header and Payload if present. It does not take into account the bytes
 * required to store itself. Refer to MQTT v3.1.1 algorithm for the
 * implementation.
 */
int mqtt_write_length(u8 *buf, usize len)
{

    int bytes   = 0;
    u16 encoded = 0;

    do {

        if (bytes + 1 > MAX_LEN_BYTES)
            return bytes;

        encoded = len % 128;
        len /= 128;

        /* if there are more digits to encode, set the top bit of this digit */
        if (len > 0)
            encoded |= 128;

        buf[bytes++] = encoded;

    } while (len > 0);

    return bytes;
}

/*
 * Decode Remaining Length comprised of Variable Header and Payload if
 * present. It does not take into account the bytes for storing length. Refer
 * to MQTT v3.1.1 algorithm for the implementation suggestion.
 *
 * TODO Handle case where multiplier > 128 * 128 * 128
 */
usize mqtt_read_length(u8 *buf, unsigned *pos)
{

    u8 c;
    u64 multiplier = 1LL;
    u64 value      = 0LL;
    *pos           = 0;

    do {
        c = *buf;
        value += (c & 127) * multiplier;
        multiplier *= 128;
        ++buf;
        ++(*pos);
    } while ((c & 128) != 0);

    return value;
}

/*
 * MQTT unpacking functions
 *
 * Meant to be called through a dispatch table, with command opcode as index
 */

/*
 * MQTT Connect packet unpack function, it's the first mandatory packet that
 * a new client must send after the socket connection went ok. As described in
 * the MQTT v3.1.1 specs the packet has the following form:
 *
 * |   Bit      |  7  |  6  |  5  |  4  |  3  |  2  |  1  |   0    |
 * |------------|-----------------------|--------------------------|<-- Fixed
 * Header | Byte 1     |      MQTT type 3      | dup |    QoS    | retain |
 * |------------|--------------------------------------------------|
 * | Byte 2     |                                                  |
 * |   .        |               Remaining Length                   |
 * |   .        |                                                  |
 * | Byte 5     |                                                  |
 * |------------|--------------------------------------------------|<-- Variable
 * Header | Byte 6     |             Protocol name len MSB                | |
 * Byte 7     |             Protocol name len LSB                |  [UINT16]
 * |------------|--------------------------------------------------|
 * | Byte 8     |                                                  |
 * |   .        |                'M' 'Q' 'T' 'T'                   |  4 bytes on
 * v3.1.1 | Byte 12    |                                                  |
 * |------------|--------------------------------------------------|
 * | Byte 13    |                 Protocol level                   |  In v3.1.1
 * is '4'
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
 *
 * All that the function do is just read bytes sequentially according to their
 * storing type, strings are treated as arrays of unsigned char.
 * The example assume that the remaining length uses all the dedicated bytes
 * but in reality it can be even just one byte length; the function starts to
 * unpack from the Variable Header position to the end of the packet as stated
 * by the total length expected.
 */
static int unpack_mqtt_connect(u8 *buf, struct mqtt_packet *pkt, usize len)
{

    /*
     * For now we ignore checks on protocol name and reserved bits, just skip
     * to the 8th byte
     */
    buf += SKIP_PROTOCOL_NAME;

    u16 cid_len = 0;

    /*
     * Read variable header byte flags, followed by keepalive MSB and LSB
     * (2 bytes word) and the client ID length (2 bytes here again)
     */
    buf += unpack(buf, "BHH", &pkt->connect.byte,
                  &pkt->connect.payload.keepalive, &cid_len);

    /* Read the client id */
    if (cid_len > 0) {
        memcpy(pkt->connect.payload.client_id, buf, cid_len);
        pkt->connect.payload.client_id[cid_len] = '\0';
        buf += cid_len;
    }

    /* Read the will topic and message if will is set on flags */
    if (pkt->connect.bits.will == 1) {
        unpack_string16(&buf, &pkt->connect.payload.will_topic);
        unpack_string16(&buf, &pkt->connect.payload.will_message);
    }

    /* Read the username if username flag is set */
    if (pkt->connect.bits.username == 1)
        unpack_string16(&buf, &pkt->connect.payload.username);

    /* Read the password if password flag is set */
    if (pkt->connect.bits.password == 1)
        unpack_string16(&buf, &pkt->connect.payload.password);

    return MQTT_OK;
}

/*
 * MQTT Publish packet unpack function, as described in the MQTT v3.1.1 specs
 * the packet has the following form:
 *
 * |   Bit    |  7  |  6  |  5  |  4  |  3  |  2  |  1  |   0    |
 * |----------|-----------------------|--------------------------|<-- Fixed
 * Header | Byte 1   |      MQTT type 3      | dup |    QoS    | retain |
 * |----------|--------------------------------------------------|
 * | Byte 2   |                                                  |
 * |   .      |               Remaining Length                   |
 * |   .      |                                                  |
 * | Byte 5   |                                                  |
 * |----------|--------------------------------------------------|<-- Variable
 * Header | Byte 6   |                Topic len MSB                     | | Byte
 * 7   |                Topic len LSB                     |  [UINT16]
 * |----------|--------------------------------------------------|
 * | Byte 8   |                                                  |
 * |   .      |                Topic name                        |
 * | Byte N   |                                                  |
 * |----------|--------------------------------------------------|
 * | Byte N+1 |            Packet Identifier MSB                 |  [UINT16]
 * | Byte N+2 |            Packet Identifier LSB                 |
 * |----------|--------------------------------------------------|<-- Payload
 * | Byte N+3 |                                                  |
 * |   .      |                   Payload                        |
 * | Byte N+M |                                                  |
 *
 * All that the function do is just read bytes sequentially according to their
 * storing type, strings are treated as arrays of unsigned char.
 * The example assume that the remaining length uses all the dedicated bytes
 * but in reality it can be even just one byte length; the function starts to
 * unpack from the Variable Header position to the end of the packet as stated
 * by the total length expected.
 */
static int unpack_mqtt_publish(u8 *buf, struct mqtt_packet *pkt, usize len)
{
    /* Read topic length and topic of the soon-to-be-published message */
    pkt->publish.topiclen = unpack_string16(&buf, &pkt->publish.topic);

    if (!pkt->publish.topic)
        return -MQTT_ERR;

    /* Read packet id */
    if (pkt->header.bits.qos > AT_MOST_ONCE) {
        pkt->publish.pkt_id = unpack_integer(&buf, 'H');
        len -= sizeof(u16);
    }

    /*
     * Message len is calculated subtracting the length of the variable header
     * from the Remaining Length field that is in the Fixed Header
     */
    len -= (sizeof(u16) + pkt->publish.topiclen);
    pkt->publish.payloadlen = len;
    pkt->publish.payload    = unpack_bytes(&buf, len);

    if (!pkt->publish.payload)
        return -MQTT_ERR;

    return MQTT_OK;
}

static int unpack_mqtt_subscribe(u8 *buf, struct mqtt_packet *pkt, usize len)
{

    struct mqtt_subscribe subscribe;
    subscribe.tuples = NULL;

    /* Read packet id */
    subscribe.pkt_id = unpack_integer(&buf, 'H');
    len -= sizeof(u16);

    /*
     * Read in a loop all remaining bytes specified by len of the Fixed Header.
     * From now on the payload consists of 3-tuples formed by:
     *  - topic length
     *  - topic filter (string)
     *  - qos
     */
    usize i = 0;
    for (; len > 0; ++i) {
        /* Read length bytes of the first topic filter */
        len -= sizeof(u16);

        /* We have to make room for additional incoming tuples */
        subscribe.tuples =
            try_realloc(subscribe.tuples, (i + 1) * sizeof(*subscribe.tuples));
        subscribe.tuples[i].topic_len =
            unpack_string16(&buf, &subscribe.tuples[i].topic);

        if (!subscribe.tuples[i].topic)
            goto err;

        len -= subscribe.tuples[i].topic_len;
        subscribe.tuples[i].qos = unpack_integer(&buf, 'B');
        len -= sizeof(u8);
    }

    subscribe.tuples_len = i;
    pkt->subscribe       = subscribe;

    return MQTT_OK;

err:
    return -MQTT_ERR;
}

static int unpack_mqtt_unsubscribe(u8 *buf, struct mqtt_packet *pkt, usize len)
{

    struct mqtt_unsubscribe unsubscribe;
    unsubscribe.tuples = NULL;

    /* Read packet id */
    unsubscribe.pkt_id = unpack_integer(&buf, 'H');
    len -= sizeof(u16);

    /*
     * Read in a loop all remaining bytes specified by len of the Fixed Header.
     * From now on the payload consists of 2-tuples formed by:
     *  - topic length
     *  - topic filter (string)
     */
    usize i = 0;
    for (; len > 0; ++i) {

        /* Read length bytes of the first topic filter */
        len -= sizeof(u16);

        /* We have to make room for additional incoming tuples */
        unsubscribe.tuples = try_realloc(unsubscribe.tuples,
                                         (i + 1) * sizeof(*unsubscribe.tuples));
        unsubscribe.tuples[i].topic_len =
            unpack_string16(&buf, &unsubscribe.tuples[i].topic);

        if (!unsubscribe.tuples[i].topic)
            goto err;

        len -= unsubscribe.tuples[i].topic_len;
    }

    unsubscribe.tuples_len = i;
    pkt->unsubscribe       = unsubscribe;

    return MQTT_OK;

err:
    return -MQTT_ERR;
}

static int unpack_mqtt_ack(u8 *buf, struct mqtt_packet *pkt, usize len)
{
    pkt->ack = (struct mqtt_ack){.pkt_id = unpacku16(buf)};
    return MQTT_OK;
}

/*
 * Main unpacking function entry point. Call the correct unpacking function
 * through a dispatch table
 */
int mqtt_read(u8 *buf, struct mqtt_packet *pkt, u8 byte, usize len)
{

    int rc  = MQTT_OK;
    u8 type = byte >> 4;

    /*
     * Check for OPCODE, if an unknown OPCODE is received return an
     * error
     */
    if (DISCONNECT < type || CONNECT > type)
        return -MQTT_ERR;

    pkt->header = (union mqtt_header){.byte = byte};

    /* Call the appropriate unpack handler based on the message type */
    if (type >= PINGREQ && type <= DISCONNECT)
        return rc;

    rc = unpack_handlers[type](buf, pkt, len);

    return rc;
}

/*
 * MQTT packets packing functions
 *
 * Meant to be called through a dispatch table, with command opcode as index
 */

static usize pack_mqtt_header(const union mqtt_header *hdr, u8 *buf)
{
    pack(buf++, "B", hdr->byte);

    /* Encode 0 length bytes, message like this have only a fixed header */
    mqtt_write_length(buf, 0);

    return MQTT_HEADER_LEN;
}

static usize pack_mqtt_ack(const struct mqtt_packet *pkt, u8 *buf)
{

    pack(buf, "BBH", pkt->header.byte, MQTT_HEADER_LEN, pkt->ack.pkt_id);

    return MQTT_ACK_LEN;
}

static usize pack_mqtt_connack(const struct mqtt_packet *pkt, u8 *buf)
{

    pack(buf++, "B", pkt->header.byte);
    buf += mqtt_write_length(buf, MQTT_HEADER_LEN);

    pack(buf, "BB", pkt->connack.byte, pkt->connack.rc);

    return MQTT_ACK_LEN;
}

static usize pack_mqtt_suback(const struct mqtt_packet *pkt, u8 *buf)
{

    usize len    = 0;
    usize pktlen = mqtt_size(pkt, &len);

    pack(buf++, "B", pkt->header.byte);
    buf += mqtt_write_length(buf, len);

    buf += pack(buf, "H", pkt->suback.pkt_id);
    for (int i = 0; i < pkt->suback.rcslen; i++)
        pack(buf++, "B", pkt->suback.rcs[i]);

    return pktlen;
}

static usize pack_mqtt_publish(const struct mqtt_packet *pkt, u8 *buf)
{

    /*
     * We must calculate the total length of the packet including header and
     * length field of the fixed header part
     */

    // Total len of the packet excluding fixed header len
    usize len    = 0L;
    usize pktlen = mqtt_size(pkt, &len);

    pack(buf++, "B", pkt->header.byte);

    /*
     * TODO handle case where step is > 1, e.g. when a message longer than 128
     * bytes is published
     */
    buf += mqtt_write_length(buf, len);

    // Topic len followed by topic name in bytes
    buf += pack(buf, "H", pkt->publish.topiclen);
    memcpy(buf, pkt->publish.topic, pkt->publish.topiclen);
    buf += pkt->publish.topiclen;

    // Packet id
    if (pkt->header.bits.qos > AT_MOST_ONCE)
        buf += pack(buf, "H", pkt->publish.pkt_id);

    // Finally the payload, same way of topic, payload len -> payload
    memcpy(buf, pkt->publish.payload, pkt->publish.payloadlen);

    return pktlen;
}

/*
 * Main packing function entry point. Call the correct packing function through
 * a dispatch table
 */
usize mqtt_write(const struct mqtt_packet *pkt, u8 *buf)
{
    u8 type = pkt->header.bits.type;
    if (type == PINGREQ || type == PINGRESP)
        return pack_mqtt_header(&pkt->header, buf);
    return pack_handlers[type](pkt, buf);
}

/*
 * MQTT packets building functions
 */

void mqtt_ack(struct mqtt_packet *pkt, u16 pkt_id)
{
    pkt->ack = (struct mqtt_ack){.pkt_id = pkt_id};
}

void mqtt_connack(struct mqtt_packet *pkt, u8 cflags, u8 rc)
{
    pkt->connack = (struct mqtt_connack){.byte = cflags, .rc = rc};
}

void mqtt_suback(struct mqtt_packet *pkt, u16 pkt_id, u8 *rcs, u16 rcslen)
{
    pkt->suback = (struct mqtt_suback){
        .pkt_id = pkt_id, .rcslen = rcslen, .rcs = try_alloc(rcslen)};
    memcpy(pkt->suback.rcs, rcs, rcslen);
}

void mqtt_packet_publish(struct mqtt_packet *pkt, u16 pkt_id, usize topiclen,
                         u8 *topic, usize payloadlen, u8 *payload)
{
    pkt->publish = (struct mqtt_publish){.pkt_id     = pkt_id,
                                         .topiclen   = topiclen,
                                         .topic      = topic,
                                         .payloadlen = payloadlen,
                                         .payload    = payload};
}

void mqtt_packet_free(struct mqtt_packet *pkt)
{

    switch (pkt->header.bits.type) {
    case CONNECT:
        if (pkt->connect.bits.username == 1)
            free_memory(pkt->connect.payload.username);
        if (pkt->connect.bits.password == 1)
            free_memory(pkt->connect.payload.password);
        if (pkt->connect.bits.will == 1) {
            free_memory(pkt->connect.payload.will_message);
            free_memory(pkt->connect.payload.will_topic);
        }
        break;
    case SUBSCRIBE:
    case UNSUBSCRIBE:
        for (unsigned i = 0; i < pkt->subscribe.tuples_len; i++)
            free_memory(pkt->subscribe.tuples[i].topic);
        free_memory(pkt->subscribe.tuples);
        break;
    case SUBACK:
        free_memory(pkt->suback.rcs);
        break;
    case PUBLISH:
        free_memory(pkt->publish.topic);
        free_memory(pkt->publish.payload);
        break;
    default:
        break;
    }
}

void mqtt_set_dup(struct mqtt_packet *pkt) { pkt->header.bits.dup = 1; }

/*
 * Helper function for ACKs with a packet identifier, just encode a bytearray
 * of length 4, 1 byte for the fixed header, 1 for the encoded length of the
 * packet and 2 for the packet identifier value, which is a 16 bit integer
 */
int mqtt_write_ack(u8 *buf, u8 op, u16 id)
{
    u8 byte = 0;
    switch (op) {
    case PUBACK:
        byte = PUBACK_B;
        break;
    case PUBREC:
        byte = PUBREC_B;
        break;
    case PUBREL:
        byte = PUBREL_B;
        break;
    case PUBCOMP:
        byte = PUBCOMP_B;
        break;
    case UNSUBACK:
        byte = UNSUBACK_B;
        break;
    }
    pack(buf++, "B", byte);
    buf += mqtt_write_length(buf, MQTT_HEADER_LEN);
    pack(buf, "H", id);
    return 4; // u8=1 + u16=2 + 1 byte for remaining bytes field
}

/*
 * Returns the size of a packet. Useful to pack functions to know the expected
 * buffer size of the packet based on the opcode. Accept an optional pointer
 * to get the len reserved for storing the remaining length of the full packet
 * excluding the fixed header (1 byte) and the bytes needed to store the
 * value itself (1 to 3 bytes).
 */
usize mqtt_size(const struct mqtt_packet *pkt, usize *len)
{
    usize size = 0LL;
    switch (pkt->header.bits.type) {
    case PUBLISH:
        size = MQTT_HEADER_LEN + sizeof(uint16_t) + pkt->publish.topiclen +
               pkt->publish.payloadlen;
        if (pkt->header.bits.qos > AT_MOST_ONCE)
            size += sizeof(uint16_t);
        break;
    case SUBACK:
        size = MQTT_HEADER_LEN + sizeof(uint16_t) + pkt->suback.rcslen;
        break;
    default:
        size = MQTT_ACK_LEN;
        break;
    }
    /*
     * Here we take into account the number of bytes needed to store the total
     * amount of bytes size of the packet, excluding the encoding space to
     * store the value itself and the fixed header, updating len pointer if
     * not NULL.
     */
    int remaininglen_offset = 0;
    if ((size - 1) > 0x200000) // 3 bytes <= 128 * 128 * 128
        remaininglen_offset = 3;
    else if ((size - 1) > 0x4000) // 2 bytes <= 128 * 128
        remaininglen_offset = 2;
    else if ((size - 1) > 0x80) // 1 byte  <= 128
        remaininglen_offset = 1;
    size += remaininglen_offset;
    if (len)
        *len = size - MQTT_HEADER_LEN - remaininglen_offset;
    return size;
}

static void mqtt_free(const struct ref *refcount)
{
    struct mqtt_packet *pkt =
        container_of(refcount, struct mqtt_packet, refcount);
    mqtt_packet_free(pkt);
    free_memory(pkt);
}

/* Just a packet allocing with the reference counter set */
struct mqtt_packet *mqtt_packet_alloc(u8 byte)
{
    struct mqtt_packet *packet = try_alloc(sizeof(*packet));
    packet->header             = (union mqtt_header){.byte = byte};
    packet->refcount           = (struct ref){mqtt_free, 0};
    packet->refcount.count     = 0;
    return packet;
}
