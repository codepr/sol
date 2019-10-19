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

#include <string.h>
#include <arpa/inet.h>
#include "util.h"
#include "pack.h"
#include "mqtt.h"

typedef size_t mqtt_unpack_handler(unsigned char *,
                                   union mqtt_header *,
                                   union mqtt_packet *,
                                   size_t);

typedef unsigned char *mqtt_pack_handler(const union mqtt_packet *);

static size_t unpack_mqtt_connect(unsigned char *,
                                  union mqtt_header *,
                                  union mqtt_packet *,
                                  size_t);

static size_t unpack_mqtt_publish(unsigned char *,
                                  union mqtt_header *,
                                  union mqtt_packet *,
                                  size_t);

static size_t unpack_mqtt_subscribe(unsigned char *,
                                    union mqtt_header *,
                                    union mqtt_packet *,
                                    size_t);

static size_t unpack_mqtt_unsubscribe(unsigned char *,
                                      union mqtt_header *,
                                      union mqtt_packet *,
                                      size_t);

static size_t unpack_mqtt_ack(unsigned char *,
                              union mqtt_header *,
                              union mqtt_packet *,
                              size_t);

static unsigned char *pack_mqtt_header(const union mqtt_header *);

static unsigned char *pack_mqtt_ack(const union mqtt_packet *);

static unsigned char *pack_mqtt_connack(const union mqtt_packet *);

static unsigned char *pack_mqtt_suback(const union mqtt_packet *);

static unsigned char *pack_mqtt_publish(const union mqtt_packet *);

/* MQTT v3.1.1 standard */
static const int MAX_LEN_BYTES = 4;

/*
 * Unpack functions mapping unpacking_handlers positioned in the array based
 * on message type
 */
static mqtt_unpack_handler *unpack_handlers[11] = {
    NULL,
    unpack_mqtt_connect,
    NULL,
    unpack_mqtt_publish,
    unpack_mqtt_ack,
    unpack_mqtt_ack,
    unpack_mqtt_ack,
    unpack_mqtt_ack,
    unpack_mqtt_subscribe,
    NULL,
    unpack_mqtt_unsubscribe
};

static mqtt_pack_handler *pack_handlers[13] = {
    NULL,
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
    NULL
};

/*
 * Encode Remaining Length on a MQTT packet header, comprised of Variable
 * Header and Payload if present. It does not take into account the bytes
 * required to store itself. Refer to MQTT v3.1.1 algorithm for the
 * implementation.
 */
int mqtt_encode_length(unsigned char *buf, size_t len) {

    int bytes = 0;

	do {

        if (bytes + 1 > MAX_LEN_BYTES)
            return bytes;

		char d = len % 128;
		len /= 128;

		/* if there are more digits to encode, set the top bit of this digit */
		if (len > 0)
			d |= 0x80;

		buf[bytes++] = d;

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
size_t mqtt_decode_length(unsigned char **buf, unsigned *pos) {

    char c;
	unsigned long long multiplier = 1LL;
	unsigned long long value = 0LL;
    *pos = 0;

	do {
        c = **buf;
		value += (c & 127) * multiplier;
		multiplier *= 128;
        (*buf)++;
        (*pos)++;
    } while ((c & 128) != 0);

    return value;
}

/*
 * MQTT unpacking functions
 */

static size_t unpack_mqtt_connect(unsigned char *raw,
                                  union mqtt_header *hdr,
                                  union mqtt_packet *pkt,
                                  size_t len) {

    struct mqtt_connect connect = { .header = *hdr };
    pkt->connect = connect;

    /*
     * For now we ignore checks on protocol name and reserverd bits, just skip
     * to the 8th byte
     */
    raw += 7;

    unsigned int cid_len = 0;

    /*
     * Read variable header byte flags, followed by keepalive MSB and LSB
     * (2 bytes word) and the client ID length (2 bytes here again)
     */
    unpack((unsigned char *) raw, "BHH",
           &pkt->connect.byte,
           &pkt->connect.payload.keepalive,
           &cid_len);

    raw += 5;

    /* Read the client id */
    if (cid_len > 0)
        pkt->connect.payload.client_id = unpack_bytes(&raw, cid_len);

    /* Read the will topic and message if will is set on flags */
    if (pkt->connect.bits.will == 1) {

        uint16_t will_topic_len = unpack_integer(&raw, 'H');
        pkt->connect.payload.will_topic = unpack_bytes(&raw, will_topic_len);

        uint16_t will_message_len = unpack_integer(&raw, 'H');
        pkt->connect.payload.will_message = unpack_bytes(&raw, will_message_len);
    }

    /* Read the username if username flag is set */
    if (pkt->connect.bits.username == 1) {
        uint16_t username_len = unpack_integer(&raw, 'H');
        pkt->connect.payload.username = unpack_bytes(&raw, username_len);
    }

    /* Read the password if password flag is set */
    if (pkt->connect.bits.password == 1) {
        uint16_t password_len = unpack_integer(&raw, 'H');
        pkt->connect.payload.password = unpack_bytes(&raw, password_len);
    }

    return len;
}

static size_t unpack_mqtt_publish(unsigned char *raw,
                                  union mqtt_header *hdr,
                                  union mqtt_packet *pkt,
                                  size_t len) {
    struct mqtt_publish publish = { .header = *hdr };
    pkt->publish = publish;

    /* Read topic length and topic of the soon-to-be-published message */
    uint16_t topic_len = unpack_integer(&raw, 'H');
    pkt->publish.topiclen = topic_len;
    pkt->publish.topic = unpack_bytes(&raw, topic_len);

    uint64_t message_len = len;

    /* Read packet id */
    if (publish.header.bits.qos > AT_MOST_ONCE) {
        pkt->publish.pkt_id = unpack_integer(&raw, 'H');
        message_len -= sizeof(uint16_t);
    }

    /*
     * Message len is calculated subtracting the length of the variable header
     * from the Remaining Length field that is in the Fixed Header
     */
    message_len -= (sizeof(uint16_t) + topic_len);
    pkt->publish.payloadlen = message_len;
    pkt->publish.payload = unpack_bytes(&raw, message_len);

    return len;
}

static size_t unpack_mqtt_subscribe(unsigned char *raw,
                                    union mqtt_header *hdr,
                                    union mqtt_packet *pkt,
                                    size_t len) {

    struct mqtt_subscribe subscribe = { .header = *hdr };

    /* Read packet id */
    subscribe.pkt_id = unpack_integer(&raw, 'H');
    len -= sizeof(uint16_t);

    /*
     * Read in a loop all remaning bytes specified by len of the Fixed Header.
     * From now on the payload consists of 3-tuples formed by:
     *  - topic length
     *  - topic filter (string)
     *  - qos
     */
    int i = 0;
    while (len > 0) {

        /* Read length bytes of the first topic filter */
        unsigned int topic_len = unpack_integer(&raw, 'H');
        len -= sizeof(uint16_t);

        /* We have to make room for additional incoming tuples */
        subscribe.tuples = sol_realloc(subscribe.tuples,
                                       (i+1) * sizeof(*subscribe.tuples));
        subscribe.tuples[i].topic_len = topic_len;
        subscribe.tuples[i].topic = unpack_bytes(&raw, topic_len);
        len -= topic_len;
        subscribe.tuples[i].qos = unpack_integer(&raw, 'B');
        len -= sizeof(uint8_t);
        i++;
    }

    subscribe.tuples_len = i;

    pkt->subscribe = subscribe;

    return len;
}

static size_t unpack_mqtt_unsubscribe(unsigned char *raw,
                                      union mqtt_header *hdr,
                                      union mqtt_packet *pkt,
                                      size_t len) {

    struct mqtt_unsubscribe unsubscribe = { .header = *hdr };

    /* Read packet id */
    unsubscribe.pkt_id = unpack_integer(&raw, 'H');
    len -= sizeof(uint16_t);

    /*
     * Read in a loop all remaning bytes specified by len of the Fixed Header.
     * From now on the payload consists of 2-tuples formed by:
     *  - topic length
     *  - topic filter (string)
     */
    int i = 0;
    while (len > 0) {

        /* Read length bytes of the first topic filter */
        uint16_t topic_len = unpack_integer(&raw, 'H');
        len -= sizeof(uint16_t);

        /* We have to make room for additional incoming tuples */
        unsubscribe.tuples = sol_realloc(unsubscribe.tuples,
                                         (i+1) * sizeof(*unsubscribe.tuples));
        unsubscribe.tuples[i].topic_len = topic_len;
        unsubscribe.tuples[i].topic = unpack_bytes(&raw, topic_len);
        len -= topic_len;

        i++;
    }

    unsubscribe.tuples_len = i;

    pkt->unsubscribe = unsubscribe;

    return len;
}

static size_t unpack_mqtt_ack(unsigned char *raw,
                              union mqtt_header *hdr,
                              union mqtt_packet *pkt,
                              size_t len) {

    struct mqtt_ack ack = { .header = *hdr };

    ack.pkt_id = unpacku16((unsigned char *) raw);
    pkt->ack = ack;

    return len;
}

int unpack_mqtt_packet(unsigned char *raw,
                       union mqtt_packet *pkt,
                       unsigned char type,
                       size_t len) {

    int rc = 0;

    union mqtt_header header = {
        .byte = type
    };

    if (header.bits.type == DISCONNECT
        || header.bits.type == PINGREQ
        || header.bits.type == PINGRESP)
        pkt->header = header;
    else
        /* Call the appropriate unpack handler based on the message type */
        rc = unpack_handlers[header.bits.type](raw, &header, pkt, len);

    return rc;
}

/*
 * MQTT packets packing functions
 */

static unsigned char *pack_mqtt_header(const union mqtt_header *hdr) {

    unsigned char *packed = sol_malloc(MQTT_HEADER_LEN);
    unsigned char *ptr = packed;

    pack(ptr++, "B", hdr->byte);

    /* Encode 0 length bytes, message like this have only a fixed header */
    mqtt_encode_length(ptr, 0);

    return packed;
}

static unsigned char *pack_mqtt_ack(const union mqtt_packet *pkt) {

    unsigned char *packed = sol_malloc(MQTT_ACK_LEN);
    unsigned char *ptr = packed;

    pack(ptr++, "B", pkt->ack.header.byte);
    int step = mqtt_encode_length(ptr, MQTT_HEADER_LEN);
    ptr += step;

    pack(ptr, "H", pkt->ack.pkt_id);

    return packed;
}

static unsigned char *pack_mqtt_connack(const union mqtt_packet *pkt) {

    unsigned char *packed = sol_malloc(MQTT_ACK_LEN);
    unsigned char *ptr = packed;

    pack(ptr++, "B", pkt->connack.header.byte);
    int step = mqtt_encode_length(ptr, MQTT_HEADER_LEN);
    ptr += step;

    pack(ptr, "BB", pkt->connack.byte, pkt->connack.rc);

    return packed;
}

static unsigned char *pack_mqtt_suback(const union mqtt_packet *pkt) {

    size_t pktlen = MQTT_HEADER_LEN + sizeof(uint16_t) + pkt->suback.rcslen;
    unsigned char *packed = sol_malloc(pktlen + 0);
    unsigned char *ptr = packed;

    pack(ptr++, "B", pkt->suback.header.byte);
    size_t len = sizeof(uint16_t) + pkt->suback.rcslen;
    int step = mqtt_encode_length(ptr, len);
    ptr += step;

    pack(ptr, "H", pkt->suback.pkt_id);
    ptr += 2;
    for (int i = 0; i < pkt->suback.rcslen; i++)
        pack(ptr++, "B", pkt->suback.rcs[i]);

    return packed;
}

static unsigned char *pack_mqtt_publish(const union mqtt_packet *pkt) {

    /*
     * We must calculate the total length of the packet including header and
     * length field of the fixed header part
     */
    size_t pktlen = MQTT_HEADER_LEN + sizeof(uint16_t) +
        pkt->publish.topiclen + pkt->publish.payloadlen;

    // Total len of the packet excluding fixed header len
    size_t len = 0L;

    if (pkt->publish.header.bits.qos > AT_MOST_ONCE)
        pktlen += sizeof(uint16_t);

    unsigned char *packed = sol_malloc(pktlen);
    unsigned char *ptr = packed;

    pack(ptr++, "B", pkt->publish.header.byte);

    // Total len of the packet excluding fixed header len
    len += (pktlen - MQTT_HEADER_LEN);

    /*
     * TODO handle case where step is > 1, e.g. when a message longer than 128
     * bytes is published
     */
    int step = mqtt_encode_length(ptr, len);
    ptr += step;

    // Topic len followed by topic name in bytes
    pack(ptr, "H", pkt->publish.topiclen);
    ptr += sizeof(uint16_t);
    memcpy(ptr, pkt->publish.topic, pkt->publish.topiclen);
    ptr += pkt->publish.topiclen;

    // Packet id
    if (pkt->publish.header.bits.qos > AT_MOST_ONCE) {
        pack(ptr, "H", pkt->publish.pkt_id);
        ptr += sizeof(uint16_t);
    }

    // Finally the payload, same way of topic, payload len -> payload
    memcpy(ptr, pkt->publish.payload, pkt->publish.payloadlen);

    return packed;
}

unsigned char *pack_mqtt_packet(const union mqtt_packet *pkt, unsigned type) {
    if (type == PINGREQ || type == PINGRESP)
        return pack_mqtt_header(&pkt->header);
    return pack_handlers[type](pkt);
}

/*
 * MQTT packets building functions
 */

union mqtt_header *mqtt_packet_header(unsigned char byte) {
    static union mqtt_header header;
    header.byte = byte;
    return &header;
}

struct mqtt_ack *mqtt_packet_ack(unsigned char byte, unsigned short pkt_id) {

    static struct mqtt_ack ack;

    ack.header.byte = byte;
    ack.pkt_id = pkt_id;

    return &ack;
}

struct mqtt_connack *mqtt_packet_connack(unsigned char byte,
                                         unsigned char cflags,
                                         unsigned char rc) {

    static struct mqtt_connack connack;

	connack.header.byte = byte;
    connack.byte = cflags;
    connack.rc = rc;

	return &connack;
}

struct mqtt_suback *mqtt_packet_suback(unsigned char byte,
                                       unsigned short pkt_id,
                                       unsigned char *rcs,
                                       unsigned short rcslen) {

    struct mqtt_suback *suback = sol_malloc(sizeof(*suback));

    suback->header.byte = byte;
    suback->pkt_id = pkt_id;
    suback->rcslen = rcslen;
    suback->rcs = sol_malloc(rcslen);
    memcpy(suback->rcs, rcs, rcslen);

    return suback;
}

struct mqtt_publish *mqtt_packet_publish(unsigned char byte,
                                         unsigned short pkt_id,
                                         size_t topiclen,
                                         unsigned char *topic,
                                         size_t payloadlen,
                                         unsigned char *payload) {

    struct mqtt_publish *publish = sol_malloc(sizeof(*publish));

    publish->header.byte = byte;
    publish->pkt_id = pkt_id;
    publish->topiclen = topiclen;
    publish->topic = topic;
    publish->payloadlen = payloadlen;
    publish->payload = payload;

    return publish;
}

void mqtt_packet_release(union mqtt_packet *pkt, unsigned type) {

    switch (type) {
        case CONNECT:
            sol_free(pkt->connect.payload.client_id);
            if (pkt->connect.bits.username == 1)
                sol_free(pkt->connect.payload.username);
            if (pkt->connect.bits.password == 1)
                sol_free(pkt->connect.payload.password);
            if (pkt->connect.bits.will == 1) {
                sol_free(pkt->connect.payload.will_message);
                sol_free(pkt->connect.payload.will_topic);
            }
            break;
        case SUBSCRIBE:
        case UNSUBSCRIBE:
            for (unsigned i = 0; i < pkt->subscribe.tuples_len; i++)
                sol_free(pkt->subscribe.tuples[i].topic);
            sol_free(pkt->subscribe.tuples);
            break;
        case SUBACK:
            sol_free(pkt->suback.rcs);
            break;
        case PUBLISH:
            sol_free(pkt->publish.topic);
            sol_free(pkt->publish.payload);
            break;
        default:
            break;
    }
}

void mqtt_set_dup(union mqtt_packet *pkt, int type) {
    switch (type) {
        case SUBACK:
            pkt->suback.header.bits.dup = 1;
            break;
        case PUBLISH:
            pkt->publish.header.bits.dup = 1;
            break;
        case PUBACK:
        case PUBREC:
        case PUBREL:
            pkt->ack.header.bits.dup = 1;
            break;
        default:
            break;
    }
}
