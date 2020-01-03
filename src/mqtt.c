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
                                   struct mqtt_packet *, size_t);

typedef size_t mqtt_pack_handler(const struct mqtt_packet *, unsigned char *);

static size_t unpack_mqtt_connect(unsigned char *,
                                  struct mqtt_packet *, size_t);

static size_t unpack_mqtt_publish(unsigned char *,
                                  struct mqtt_packet *, size_t);

static size_t unpack_mqtt_subscribe(unsigned char *,
                                    struct mqtt_packet *, size_t);

static size_t unpack_mqtt_unsubscribe(unsigned char *,
                                      struct mqtt_packet *, size_t);

static size_t unpack_mqtt_ack(unsigned char *, struct mqtt_packet *, size_t);

static size_t pack_mqtt_header(const union mqtt_header *, unsigned char *);

static size_t pack_mqtt_ack(const struct mqtt_packet *, unsigned char *);

static size_t pack_mqtt_connack(const struct mqtt_packet *, unsigned char *);

static size_t pack_mqtt_suback(const struct mqtt_packet *, unsigned char *);

static size_t pack_mqtt_publish(const struct mqtt_packet *, unsigned char *);

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
    short encoded = 0;

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
                                  struct mqtt_packet *pkt, size_t len) {

    /*
     * For now we ignore checks on protocol name and reserved bits, just skip
     * to the 8th byte
     */
    raw += 7;

    unsigned int cid_len = 0;

    /*
     * Read variable header byte flags, followed by keepalive MSB and LSB
     * (2 bytes word) and the client ID length (2 bytes here again)
     */
    unpack(raw, "BHH", &pkt->connect.byte,
           &pkt->connect.payload.keepalive, &cid_len);

    raw += 5;

    /* Read the client id */
    if (cid_len > 0) {
        memcpy(pkt->connect.payload.client_id, raw, cid_len);
        pkt->connect.payload.client_id[cid_len] = '\0';
        raw += cid_len;
    }

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
                                  struct mqtt_packet *pkt, size_t len) {
    /* Read topic length and topic of the soon-to-be-published message */
    uint16_t topic_len = unpack_integer(&raw, 'H');
    pkt->publish.topiclen = topic_len;
    pkt->publish.topic = unpack_bytes(&raw, topic_len);

    uint64_t message_len = len;

    /* Read packet id */
    if (pkt->header.bits.qos > AT_MOST_ONCE) {
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
                                    struct mqtt_packet *pkt, size_t len) {

    struct mqtt_subscribe subscribe;
    subscribe.tuples = NULL;

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
        subscribe.tuples = xrealloc(subscribe.tuples,
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
                                      struct mqtt_packet *pkt, size_t len) {

    struct mqtt_unsubscribe unsubscribe;

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
        unsubscribe.tuples = xrealloc(unsubscribe.tuples,
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
                              struct mqtt_packet *pkt, size_t len) {
    pkt->ack = (struct mqtt_ack) { .pkt_id = unpacku16(raw) };
    return len;
}

int mqtt_unpack(unsigned char *raw, struct mqtt_packet *pkt,
                unsigned char byte, size_t len) {

    int rc = MQTT_OK;
    unsigned type = byte >> 4;

    pkt->header = (union mqtt_header) { .byte = byte };

    /* Call the appropriate unpack handler based on the message type */
    if (type >= PINGREQ && type <= DISCONNECT)
        return rc;

    if (unpack_handlers[type](raw, pkt, len) != len)
        rc = -MQTT_ERR;

    return rc;
}

/*
 * MQTT packets packing functions
 */

static size_t pack_mqtt_header(const union mqtt_header *hdr,
                               unsigned char *buf) {
    pack(buf++, "B", hdr->byte);

    /* Encode 0 length bytes, message like this have only a fixed header */
    mqtt_encode_length(buf, 0);

    return MQTT_HEADER_LEN;
}

static size_t pack_mqtt_ack(const struct mqtt_packet *pkt, unsigned char *buf) {

    pack(buf++, "B", pkt->header.byte);
    buf += mqtt_encode_length(buf, MQTT_HEADER_LEN);

    pack(buf, "H", pkt->ack.pkt_id);

    return MQTT_ACK_LEN;
}

static size_t pack_mqtt_connack(const struct mqtt_packet *pkt,
                                unsigned char *buf) {

    pack(buf++, "B", pkt->header.byte);
    buf += mqtt_encode_length(buf, MQTT_HEADER_LEN);

    pack(buf, "BB", pkt->connack.byte, pkt->connack.rc);

    return MQTT_ACK_LEN;
}

static size_t pack_mqtt_suback(const struct mqtt_packet *pkt,
                               unsigned char * buf) {

    size_t len = 0;
    size_t pktlen = mqtt_size(pkt, &len);

    pack(buf++, "B", pkt->header.byte);
    buf += mqtt_encode_length(buf, len);

    buf += pack(buf, "H", pkt->suback.pkt_id);
    for (int i = 0; i < pkt->suback.rcslen; i++)
        pack(buf++, "B", pkt->suback.rcs[i]);

    return pktlen;
}

static size_t pack_mqtt_publish(const struct mqtt_packet *pkt,
                                unsigned char *buf) {

    /*
     * We must calculate the total length of the packet including header and
     * length field of the fixed header part
     */

    // Total len of the packet excluding fixed header len
    size_t len = 0L;
    size_t pktlen = mqtt_size(pkt, &len);

    pack(buf++, "B", pkt->header.byte);

    /*
     * TODO handle case where step is > 1, e.g. when a message longer than 128
     * bytes is published
     */
    buf += mqtt_encode_length(buf, len);

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

size_t mqtt_pack(const struct mqtt_packet *pkt, unsigned char *buf) {
    unsigned type = pkt->header.bits.type;
    if (type == PINGREQ || type == PINGRESP)
        return pack_mqtt_header(&pkt->header, buf);
    return pack_handlers[type](pkt, buf);
}

/*
 * MQTT packets building functions
 */

void mqtt_ack(struct mqtt_packet *pkt, unsigned short pkt_id) {
    pkt->ack = (struct mqtt_ack) { .pkt_id = pkt_id };
}

void mqtt_connack(struct mqtt_packet *pkt,
                  unsigned char cflags, unsigned char rc) {
    pkt->connack = (struct mqtt_connack) {
        .byte = cflags,
        .rc = rc
    };
}

void mqtt_suback(struct mqtt_packet *pkt, unsigned short pkt_id,
                 unsigned char *rcs, unsigned short rcslen) {
    pkt->suback = (struct mqtt_suback) {
        .pkt_id = pkt_id,
        .rcslen = rcslen,
        .rcs = xmalloc(rcslen)
    };
    memcpy(pkt->suback.rcs, rcs, rcslen);
}

void mqtt_packet_publish(struct mqtt_packet *pkt, unsigned short pkt_id,
                         size_t topiclen, unsigned char *topic,
                         size_t payloadlen, unsigned char *payload) {
    pkt->publish = (struct mqtt_publish) {
        .pkt_id = pkt_id,
        .topiclen = topiclen,
        .topic = topic,
        .payloadlen = payloadlen,
        .payload = payload
    };
}

void mqtt_packet_release(struct mqtt_packet *pkt, unsigned type) {

    switch (type) {
        case CONNECT:
            if (pkt->connect.bits.username == 1)
                xfree(pkt->connect.payload.username);
            if (pkt->connect.bits.password == 1)
                xfree(pkt->connect.payload.password);
            if (pkt->connect.bits.will == 1) {
                xfree(pkt->connect.payload.will_message);
                xfree(pkt->connect.payload.will_topic);
            }
            break;
        case SUBSCRIBE:
        case UNSUBSCRIBE:
            for (unsigned i = 0; i < pkt->subscribe.tuples_len; i++)
                xfree(pkt->subscribe.tuples[i].topic);
            xfree(pkt->subscribe.tuples);
            break;
        case SUBACK:
            xfree(pkt->suback.rcs);
            break;
        case PUBLISH:
            xfree(pkt->publish.topic);
            xfree(pkt->publish.payload);
            break;
        default:
            break;
    }
}

void mqtt_set_dup(struct mqtt_packet *pkt) {
    pkt->header.bits.dup = 1;
}

int mqtt_pack_mono(unsigned char *buf, unsigned char op, unsigned short id) {
    unsigned byte = 0;
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
    buf += mqtt_encode_length(buf, MQTT_HEADER_LEN);
    pack(buf, "H", id);
    return 4;
}

size_t mqtt_size(const struct mqtt_packet *pkt, size_t *len) {
    size_t size = 0LL;
    switch (pkt->header.bits.type) {
        case PUBLISH:
            size = MQTT_HEADER_LEN + sizeof(uint16_t) +
                pkt->publish.topiclen + pkt->publish.payloadlen;
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
    int remaninglen_offset = 0;
    if ((size - 1) > 0x200000)
        remaninglen_offset = 3;
    else if ((size - 1) > 0x4000)
        remaninglen_offset = 2;
    else if ((size - 1) > 0x80)
        remaninglen_offset = 1;
    size += remaninglen_offset;
    if (len)
        *len = size - MQTT_HEADER_LEN - remaninglen_offset;
    return size;
}
