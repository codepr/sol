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

#include <arpa/inet.h>
#include "util.h"
#include "mqtt.h"


typedef size_t mqtt_unpack_handler(const unsigned char *,
                                   union mqtt_header *,
                                   union mqtt_packet *);

typedef unsigned char *mqtt_pack_handler(const union mqtt_packet *);


static size_t unpack_mqtt_connect(const unsigned char *,
                                  union mqtt_header *,
                                  union mqtt_packet *);

static size_t unpack_mqtt_publish(const unsigned char *,
                                  union mqtt_header *,
                                  union mqtt_packet *);

static size_t unpack_mqtt_subscribe(const unsigned char *,
                                    union mqtt_header *,
                                    union mqtt_packet *);

static size_t unpack_mqtt_unsubscribe(const unsigned char *,
                                      union mqtt_header *,
                                      union mqtt_packet *);


static unsigned char *pack_mqtt_connack(const union mqtt_packet *);


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
    NULL,
    NULL,
    NULL,
    NULL,
    unpack_mqtt_subscribe,
    NULL,
    unpack_mqtt_unsubscribe
};


static mqtt_pack_handler *pack_handlers[3] = {
    NULL,
    NULL,
    pack_mqtt_connack
};


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


unsigned long long mqtt_decode_length(const unsigned char *buf) {

    char c;
	int multiplier = 1;
	unsigned long long value = 0LL;

	do {
        c = *buf;
		value += (c & 127) * multiplier;
		multiplier *= 128;
        buf++;
    } while ((c & 128) != 0);

    return value;
}


static size_t unpack_mqtt_connect(const unsigned char *raw,
                                  union mqtt_header *hdr,
                                  union mqtt_packet *pkt) {

    struct mqtt_connect connect = { .header = *hdr };
    pkt->connect = connect;


    /*
     * Second byte of the fixed header, contains the length of remaining bytes
     * of the connect packet
     */
    size_t len = mqtt_decode_length(raw);

    /*
     * For now we ignore checks on protocol name and reserverd bits, just skip
     * to the 8th byte
     */
    raw += 8;

    /* Read variable header byte flags */
    pkt->connect.byte = unpack_u8((const uint8_t **) &raw);

    /* Read keepalive MSB and LSB (2 bytes word) */
    pkt->connect.payload.keepalive = unpack_u16((const uint8_t **) &raw);

    /* Read CID length (2 bytes word) */
    uint16_t cid_len = unpack_u16((const uint8_t **) &raw);

    /* Read the client id */
    if (cid_len > 0) {
        pkt->connect.payload.client_id = sol_malloc(cid_len + 1);
        unpack_bytes((const uint8_t **) &raw, cid_len,
                     pkt->connect.payload.client_id);
    }

    /* Read the will topic and message if will is set on flags */
    if (pkt->connect.bits.will == 1) {

        uint16_t will_topic_len = unpack_u16((const uint8_t **) &raw);
        pkt->connect.payload.will_topic = sol_malloc(will_topic_len + 1);
        unpack_bytes((const uint8_t **) &raw, will_topic_len,
                     pkt->connect.payload.will_topic);

        uint16_t will_message_len = unpack_u16((const uint8_t **) &raw);
        pkt->connect.payload.will_message = sol_malloc(will_message_len + 1);
        unpack_bytes((const uint8_t **) &raw, will_message_len,
                     pkt->connect.payload.will_message);
    }

    /* Read the username if username flag is set */
    if (pkt->connect.bits.username == 1) {
        uint16_t username_len = unpack_u16((const uint8_t **) &raw);
        pkt->connect.payload.username = sol_malloc(username_len + 1);
        unpack_bytes((const uint8_t **) &raw, username_len,
                     pkt->connect.payload.username);
    }

    /* Read the password if username flag is set */
    if (pkt->connect.bits.password == 1) {
        uint16_t password_len = unpack_u16((const uint8_t **) &raw);
        pkt->connect.payload.password = sol_malloc(password_len + 1);
        unpack_bytes((const uint8_t **) &raw, password_len,
                     pkt->connect.payload.password);
    }

    return len;
}


static size_t unpack_mqtt_publish(const unsigned char *raw,
                                  union mqtt_header *hdr,
                                  union mqtt_packet *pkt) {

    struct mqtt_publish publish = { .header = *hdr };
    pkt->publish = publish;

    /*
     * Second byte of the fixed header, contains the length of remaining bytes
     * of the connect packet
     */
    size_t len = mqtt_decode_length(raw);
    raw++;

    /* Read topic length and topic of the soon-to-be-published message */
    uint16_t topic_len = unpack_u16((const uint8_t **) &raw);
    pkt->publish.topic = sol_malloc(topic_len + 1);
    unpack_bytes((const uint8_t **) &raw, topic_len, pkt->publish.topic);

    uint16_t message_len = len;

    /* Read packet id */
    if (publish.header.bits.qos > 0) {
        pkt->publish.pkt_id = unpack_u16((const uint8_t **) &raw);
        message_len -= sizeof(uint16_t);
    }

    /*
     * Message len is calculated subtracting the length of the variable header
     * from the Remaining Length field that is in the Fixed Header
     */
    message_len -= (sizeof(uint16_t) + topic_len);
    pkt->publish.payload = sol_malloc(message_len + 1);
    unpack_bytes((const uint8_t **) &raw, message_len, pkt->publish.payload);

    return len;
}


static size_t unpack_mqtt_subscribe(const unsigned char *raw,
                                    union mqtt_header *hdr,
                                    union mqtt_packet *pkt) {

    struct mqtt_subscribe subscribe = { .header = *hdr };

    /*
     * Second byte of the fixed header, contains the length of remaining bytes
     * of the connect packet
     */
    size_t len = mqtt_decode_length(raw);
    size_t remaining_bytes = len;
    raw++;

    /* Read packet id */
    subscribe.pkt_id = unpack_u16((const uint8_t **) &raw);
    remaining_bytes -= sizeof(uint16_t);

    /*
     * Read in a loop all remaning bytes specified by len of the Fixed Header.
     * From now on the payload consists of 3-tuples formed by:
     *  - topic length
     *  - topic filter (string)
     *  - qos
     */
    int i = 0;
    while (remaining_bytes > 0) {

        /* Read length bytes of the first topic filter */
        uint16_t topic_len = unpack_u16((const uint8_t **) &raw);
        remaining_bytes -= sizeof(uint16_t);

        /* We have to make room for additional incoming tuples */
        subscribe.tuples = sol_realloc(subscribe.tuples,
                                       (i+1) * sizeof(*subscribe.tuples));
        subscribe.tuples[i].topic_len = topic_len;
        subscribe.tuples[i].topic = sol_malloc(topic_len + 1);
        unpack_bytes((const uint8_t **) &raw, topic_len,
                     subscribe.tuples[i].topic);
        remaining_bytes -= topic_len;
        subscribe.tuples[i].qos = unpack_u8((const uint8_t **) &raw);
        remaining_bytes -= sizeof(uint8_t);
        i++;
    }

    subscribe.tuples_len = i;

    pkt->subscribe = subscribe;

    return len;
}


static size_t unpack_mqtt_unsubscribe(const unsigned char *raw,
                                      union mqtt_header *hdr,
                                      union mqtt_packet *pkt) {

    struct mqtt_unsubscribe unsubscribe = { .header = *hdr };

    /*
     * Second byte of the fixed header, contains the length of remaining bytes
     * of the connect packet
     */
    size_t len = mqtt_decode_length(raw);
    size_t remaining_bytes = len;
    raw++;

    /* Read packet id */
    unsubscribe.pkt_id = unpack_u16((const uint8_t **) &raw);
    remaining_bytes -= sizeof(uint16_t);

    /*
     * Read in a loop all remaning bytes specified by len of the Fixed Header.
     * From now on the payload consists of 2-tuples formed by:
     *  - topic length
     *  - topic filter (string)
     */
    int i = 0;
    while (remaining_bytes > 0) {

        /* Read length bytes of the first topic filter */
        uint16_t topic_len = unpack_u16((const uint8_t **) &raw);
        remaining_bytes -= sizeof(uint16_t);

        /* We have to make room for additional incoming tuples */
        unsubscribe.tuples = sol_realloc(unsubscribe.tuples,
                                         (i+1) * sizeof(*unsubscribe.tuples));
        unsubscribe.tuples[i].topic_len = topic_len;
        unsubscribe.tuples[i].topic = sol_malloc(topic_len + 1);
        unpack_bytes((const uint8_t **) &raw, topic_len,
                     unsubscribe.tuples[i].topic);
        remaining_bytes -= topic_len;

        i++;
    }

    unsubscribe.tuples_len = i;

    pkt->unsubscribe = unsubscribe;

    return len;
}


int unpack_mqtt_packet(const unsigned char *raw, union mqtt_packet *pkt) {

    int rc = 0;

    /* Read first byte of the fixed header */
    unsigned char type = *raw;

    union mqtt_header header = {
        .byte = type
    };

    rc = unpack_handlers[header.bits.type](++raw, &header, pkt);

    return rc;
}


static unsigned char *pack_mqtt_connack(const union mqtt_packet *pkt) {

    unsigned char *packed = sol_malloc(4);
    unsigned char *ptr = packed;

    pack_u8(&ptr, pkt->connack.header.byte);
    mqtt_encode_length(ptr++, 2);

    pack_u8(&ptr, pkt->connack.byte);
    pack_u8(&ptr, pkt->connack.rc);

    return packed;
}


unsigned char *pack_mqtt_packet(const union mqtt_packet *pkt, unsigned type) {

    return pack_handlers[type](pkt);
}


struct mqtt_connack *mqtt_packet_connack(unsigned char byte,
                                         char *data, size_t len) {

    struct mqtt_connack *connack = sol_malloc(sizeof(*connack));

	connack->header.byte = byte;
	connack->byte = unpack_u8((const uint8_t **) &data); /* connect flags */
	connack->rc = unpack_u8((const uint8_t **) &data); /* reason code */

	return connack;
}
