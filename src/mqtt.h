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

/* Message types */
enum message_type {
    CONNECT     = 0x01,
    CONNACK     = 0x02,
    PUBLISH     = 0x03,
    PUBACK      = 0x04,
    PUBREC      = 0x05,
    PUBREL      = 0x06,
    PUBCOMP     = 0x07,
    SUBSCRIBE   = 0x08,
    SUBACK      = 0x09,
    UNSUBSCRIBE = 0x0A,
    UNSUBACK    = 0x0B,
    PINGREQ     = 0x0C,
    PINGRESP    = 0x0D,
    DISCONNECT  = 0x0E
};


enum qos_level { AT_MOST_ONCE, AT_LEAST_ONCE, EXACTLY_ONCE };


union mqtt_header {

    unsigned char byte;

    struct {
        int retain : 1;
        unsigned qos : 2;
        int dup : 1;
        unsigned type : 4;
    } bits;

};


struct mqtt_connect {

    union mqtt_header header;

    union {

        unsigned char byte;

        struct {
            int reserverd : 1;
            int clean_session : 1;
            int will : 1;
            unsigned will_qos : 2;
            int will_retain : 1;
            int password : 1;
            int username : 1;
        } bits;
    };
};


struct mqtt_connack {

    union mqtt_header header;

    union {

        unsigned char byte;

        struct {
            int session_present : 1;
            unsigned reserverd : 7;
        } bits;
    };

    unsigned char rc;
};


struct mqtt_subscribe {

    union mqtt_header header;

    int message_id;

    struct {
        int topic_len;
        char *topic;
        unsigned qos;
    } *tuples;
};


struct mqtt_suback {

    union mqtt_header header;

    int message_id;

    int *qos_list;
};


struct mqtt_publish {

    union mqtt_header header;

    int message_id;

    int topic_len;
    char *topic;
    int payload_len;
    char *payload;
};


struct mqtt_puback {

    union mqtt_header header;

    int message_id;

    unsigned char rc;
};


#endif
