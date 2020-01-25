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

#include <errno.h>
#include <string.h>
#include <sys/epoll.h>
#include "mqtt.h"
#include "util.h"
#include "config.h"
#include "server.h"
#include "handlers.h"
#include "hashtable.h"

/* Prototype for a command handler */
typedef int handler(struct io_event *);

/* Command handler, each one have responsibility over a defined command packet */
static int connect_handler(struct io_event *);
static int disconnect_handler(struct io_event *);
static int subscribe_handler(struct io_event *);
static int unsubscribe_handler(struct io_event *);
static int publish_handler(struct io_event *);
static int puback_handler(struct io_event *);
static int pubrec_handler(struct io_event *);
static int pubrel_handler(struct io_event *);
static int pubcomp_handler(struct io_event *);
static int pingreq_handler(struct io_event *);

/* Command handler mapped usign their position paired with their type */
static handler *handlers[15] = {
    NULL,
    connect_handler,
    NULL,
    publish_handler,
    puback_handler,
    pubrec_handler,
    pubrel_handler,
    pubcomp_handler,
    subscribe_handler,
    NULL,
    unsubscribe_handler,
    NULL,
    pingreq_handler,
    NULL,
    disconnect_handler
};

int publish_message(struct mqtt_packet *pkt,
                    const struct topic *t, struct ev_ctx *ctx) {

    bool all_at_most_once = true;
    size_t len = 0;
    unsigned short mid = 0;
    unsigned char qos = pkt->header.bits.qos;
    int count = hashtable_size(t->subscribers);

    if (count == 0)
        goto exit;

    struct iterator *it = iter_new(t->subscribers, hashtable_iter_next);
    unsigned char type;

    // first run check
    FOREACH (it) {
        struct subscriber *sub = it->ptr;
        struct client *sc = sub->client;
        /*
         * If offline, we must enqueue messages in the inflight queue
         * of the client, they will be sent out only in case of a
         * clean_session == false connection
         */
        // TODO check for side-effects
        if (sc->online == false) {
            if (sc->clean_session == false) {
                INCREF(pkt, struct mqtt_packet);
                list_push(sc->session->outgoing_msgs, pkt);
            }
            continue;
        }
        /*
         * Update QoS according to subscriber's one, following MQTT
         * rules: The min between the original QoS and the subscriber
         * QoS
         */
        pkt->header.bits.qos = qos >= sub->qos ? sub->qos : qos;
        len = mqtt_size(pkt, NULL); // override len, no ID set in QoS 0

        /*
         * if QoS 0
         *
         * Set the correct size of the output packet and set the
         * correct QoS value (0) and packet identifier to (0) as
         * specified by MQTT specs
         */
        pkt->publish.pkt_id = 0;

        /*
         * if QoS > 0 we set packet identifier and track the inflight
         * message, proceed with the publish towards online subscriber.
         */
        if (pkt->header.bits.qos > AT_MOST_ONCE) {
            mid = next_free_mid(sc);
            pkt->publish.pkt_id = mid;
            INCREF(pkt, struct mqtt_packet);
            inflight_msg_init(&sc->session->i_msgs[mid], sc, pkt, len);
            type = sub->qos == AT_LEAST_ONCE ? PUBACK : PUBREC;
            struct mqtt_packet *ack = mqtt_packet_alloc(type);
            INCREF(ack, struct mqtt_packet);
            mqtt_ack(ack, mid);
            inflight_msg_init(&sc->session->i_acks[mid], sc, ack, len);
            sc->session->has_inflight = true;
            all_at_most_once = false;
        }

        mqtt_pack(pkt, sc->wbuf + sc->towrite);
        sc->towrite += len;

        // Schedule a write for the current subscriber on the next event cycle
        enqueue_event_write(ctx, sc);

        info.messages_sent++;

        log_debug("Sending PUBLISH to %s (d%i, q%u, r%i, m%u, %s, ... (%i bytes))",
                  sc->client_id,
                  pkt->header.bits.dup,
                  pkt->header.bits.qos,
                  pkt->header.bits.retain,
                  pkt->publish.pkt_id,
                  pkt->publish.topic,
                  pkt->publish.payloadlen);
    }
    iter_destroy(it);

    // add return code
    if (all_at_most_once == true)
        count = 0;

exit:

    return count;
}

/* Check if a topic match a wildcard subscription. for now just # works */
static inline int match_subscription(const char *topic,
                                     const char *wtopic, bool end_wildcard) {
    size_t len = strlen(wtopic);
    int i = 0, j = 0;
    bool found = false;
    char *ptopic = (char *) topic;
    /*
     * Cycle through the wildcard topic, char by char, seeking for '+' char and
     * at the same time assuring that every char is equal in the topic as well,
     * we don't want to accept different topics
     */
    while (wtopic[i]) {
        j = 0;
        for (; i < len; ++i) {
            if (wtopic[i] == '+') {
                found = true;
                break;
            } else if (wtopic[i] != ptopic[j]) {
                return -1;
            }
            j++;
        }
        /*
         * Get a pointer to the next '/', called two times because we want to
         * skip the first occurence, like foo/bar/baz, cause at this point we'
         * re already at /bar/baz and we don't need a pointer to /bar/baz
         * again
         */
        ptopic = index(ptopic, '/');
        ptopic = index(ptopic + 1, '/');
        i++;
    }
    if (!found && end_wildcard == true)
        return 0;
    if (ptopic[1] != '\0' && end_wildcard == false)
        return -1;
    return 0;
}

/*
 * Command handlers
 */

static void set_payload_connack(struct client *c, unsigned char rc) {
    unsigned char session_present = 0;
    unsigned char connect_flags = 0 | (session_present & 0x1) << 0;

    struct mqtt_packet response = {
        .header = { .byte = CONNACK_B },
        .connack = (struct mqtt_connack) {
            .byte = connect_flags,
            .rc = rc
        }
    };
    mqtt_pack(&response, c->wbuf + c->towrite);
    c->towrite += MQTT_ACK_LEN;
}

static int connect_handler(struct io_event *e) {

    struct mqtt_connect *c = &e->data.connect;
    struct client *cc = e->client;

    /*
     * If allow_anonymous is false we need to check for an existing
     * username:password pair match in the authentications table
     */
    if (conf->allow_anonymous == false) {
        if (c->bits.username == 0 || c->bits.password == 0)
            goto bad_auth;
        else {
            void *salt =
                hashtable_get(server.authentications, c->payload.username);
            if (!salt)
                goto bad_auth;

            if (check_passwd((char *) c->payload.password, salt) == false)
                goto bad_auth;
        }
    }

    /*
     * No client ID and clean_session == false? you're not authorized, we don't
     * know who you are
     */
    if (!c->payload.client_id[0] && c->bits.clean_session == false)
        goto not_authorized;

    /*
     * Check for client ID, if not present generate a random ID, otherwise add
     * the client to the sessions map if not already present
     */
    if (!c->payload.client_id[0]) {
        generate_random_id((char *) c->payload.client_id);
    } else {
        // First we check if a session is present
        if (c->bits.clean_session == false) {
            cc->session = hashtable_get(server.sessions, c->payload.client_id);
            if (cc->session) {
                /*
                 * If there's already some subscriptions and pending messages,
                 * empty the queue
                 */
                if (list_size(cc->session->outgoing_msgs) > 0) {
                    size_t len = 0;
                    struct iterator *it =
                        iter_new(cc->session->outgoing_msgs, list_iter_next);
                    FOREACH (it) {
                        len = mqtt_size(it->ptr, NULL);
                        mqtt_pack(it->ptr, cc->wbuf + cc->towrite);
                        cc->towrite += len;
                    }
                    enqueue_event_write(e->ctx, cc);
                }
            }
        } else {
            // Clean session true, we have to clean old session, if any
            hashtable_del(server.sessions, c->payload.client_id);
        }
    }

    if (server.clients[e->client->conn.fd].online == true &&
        strncmp(server.clients[e->client->conn.fd].client_id,
                (const char *) c->payload.client_id, MQTT_CLIENT_ID_LEN) == 1) {

        // Already connected client, 2 CONNECT packet should be interpreted as
        // a violation of the protocol, causing disconnection of the client

        log_info("Received double CONNECT from %s, disconnecting client",
                 c->payload.client_id);
        goto clientdc;
    }

    log_info("New client connected as %s (c%i, k%u)",
             c->payload.client_id,
             c->bits.clean_session,
             c->payload.keepalive);

    /*
     * Add the new connected client to the global map, if it is already
     * connected, kick him out accordingly to the MQTT v3.1.1 specs.
     */
    strncpy(e->client->client_id, (char *) c->payload.client_id, MQTT_CLIENT_ID_LEN);

    /*
     * If no session was found or the client is a new connecting client or an
     * anonymous one, we create a session here
     */
    if (c->bits.clean_session == true || !cc->session) {
        cc->session = client_session_alloc();
        INCREF(cc->session, struct client_session);
        hashtable_put(server.sessions, cc->client_id, cc->session);
    }

    // Add LWT topic and message if present
    if (c->bits.will) {
        e->client->has_lwt = true;
        const char *will_topic = (const char *) c->payload.will_topic;
        const char *will_message = (const char *) c->payload.will_message;
        // TODO check for will_topic != NULL
        struct topic *t = topic_get_or_create(&server, will_topic);
        if (!topic_exists(&server, t->name))
            topic_put(&server, t);
        // I'm sure that the string will be NUL terminated by unpack function
        size_t msg_len = strlen(will_message);
        size_t tpc_len = strlen(will_topic);

        e->client->session->lwt_msg = (struct mqtt_packet) {
            .header = (union mqtt_header) { .byte = PUBLISH_B },
            .publish = (struct mqtt_publish) {
                .pkt_id = 0,  // placeholder
                .topiclen = tpc_len,
                .topic = (unsigned char *) xstrdup(will_topic),
                .payloadlen = msg_len,
                .payload = (unsigned char *) xstrdup(will_message)
            }
        };

        e->client->session->lwt_msg.header.bits.qos = c->bits.will_qos;
        // We must store the retained message in the topic
        if (c->bits.will_retain == 1) {
            size_t publen = mqtt_size(&e->client->session->lwt_msg, NULL);
            bstring payload = bstring_empty(publen);
            mqtt_pack(&e->client->session->lwt_msg, payload);
            // We got a ready-to-be sent bytestring in the retained message
            // field
            t->retained_msg = payload;
        }
        log_info("Will message specified (%lu bytes)",
                 e->client->session->lwt_msg.publish.payloadlen);
        log_info("\t%s", e->client->session->lwt_msg.publish.payload);
    }

    // TODO check for session already present

    e->client->clean_session = c->bits.clean_session;

    set_payload_connack(cc, MQTT_CONNECTION_ACCEPTED);

    log_debug("Sending CONNACK to %s r=%u",
              c->payload.client_id, MQTT_CONNECTION_ACCEPTED);

    return REPLY;

clientdc:

    return -ERRCLIENTDC;

bad_auth:
    log_debug("Sending CONNACK to %s rc=%u",
              c->payload.client_id, MQTT_BAD_USERNAME_OR_PASSWORD);  // TODO check for session
    set_payload_connack(cc, MQTT_BAD_USERNAME_OR_PASSWORD);

    return MQTT_BAD_USERNAME_OR_PASSWORD;

not_authorized:
    log_debug("Sending CONNACK to %s rc=%u",
              c->payload.client_id, MQTT_NOT_AUTHORIZED); // TODO check for session
    set_payload_connack(cc, MQTT_NOT_AUTHORIZED);

    return MQTT_NOT_AUTHORIZED;
}

static int disconnect_handler(struct io_event *e) {

    log_debug("Received DISCONNECT from %s", e->client->client_id);

    // Remove from subscriptions if clean_session == true
    if (e->client->clean_session == true
        && list_size(e->client->session->subscriptions)) {
        struct iterator *it =
            iter_new(e->client->session->subscriptions, list_iter_next);
        FOREACH (it) {
            log_debug("Removing %s from topic %s",
                      e->client->client_id, ((struct topic *) it->ptr)->name);
            topic_del_subscriber(it->ptr, e->client);
        }
        iter_destroy(it);
    }
    return -ERRCLIENTDC;
}

static inline void add_wildcard(const char *topic, struct subscriber *s,
                                bool wildcard) {
    struct subscription *subscription = xmalloc(sizeof(*subscription));
    subscription->subscriber = s;
    subscription->topic = xstrdup(topic);
    subscription->end_wildcard = wildcard;
    INCREF(s, struct subscriber);
    server.wildcards = list_push(server.wildcards, subscription);
}

static void recursive_sub(struct trie_node *node, void *arg) {
    if (!node || !node->data)
        return;
    struct topic *t = node->data;
    struct subscriber *s = arg;
    INCREF(s, struct subscriber);
    log_debug("Adding subscriber %s to topic %s",
              s->client->client_id, t->name);
    hashtable_put(t->subscribers, s->client->client_id, s);
    list_push(s->client->session->subscriptions, t);
}

static int subscribe_handler(struct io_event *e) {

    bool wildcard = false;
    struct mqtt_subscribe *s = &e->data.subscribe;

    /*
     * We respond to the subscription request with SUBACK and a list of QoS in
     * the same exact order of reception
     */
    unsigned char rcs[s->tuples_len];
    struct client *c = e->client;

    /* Subscribe packets contains a list of topics and QoS tuples */
    for (unsigned i = 0; i < s->tuples_len; i++) {

        log_debug("Received SUBSCRIBE from %s", c->client_id);

        /*
         * Check if the topic exists already or in case create it and store in
         * the global map
         */
        char topic[s->tuples[i].topic_len + 2];
        strncpy(topic, (const char *) s->tuples[i].topic, s->tuples[i].topic_len + 1);

        log_debug("\t%s (QoS %i)", topic, s->tuples[i].qos);
        /* Recursive subscribe to all children topics if the topic ends with "/#" */
        if (topic[s->tuples[i].topic_len - 1] == '#' &&
            topic[s->tuples[i].topic_len - 2] == '/') {
            topic[s->tuples[i].topic_len - 1] = '\0';
            wildcard = true;
        } else if (topic[s->tuples[i].topic_len - 1] != '/') {
            topic[s->tuples[i].topic_len] = '/';
            topic[s->tuples[i].topic_len + 1] = '\0';
        }

        struct topic *t = topic_get_or_create(&server, topic);
        // Clean session true for now
        struct subscriber *sub =
            topic_add_subscriber(t, e->client, s->tuples[i].qos);
        // we increment reference for the subscriptions session
        INCREF(sub, struct subscriber);
        list_push(e->client->session->subscriptions, t);
        if (wildcard == true || index(topic, '+')) {
            trie_prefix_map(server.topics.root, topic, recursive_sub, sub);
            add_wildcard(topic, sub, wildcard);
        }

        // Retained message? Publish it
        // TODO move to IO threadpool
        if (t->retained_msg) {
            size_t len = bstring_len(t->retained_msg);
            memcpy(c->wbuf + c->towrite, t->retained_msg, len);
            c->towrite += len;
        }
        rcs[i] = s->tuples[i].qos;
    }

    struct mqtt_packet pkt = {
        .header = (union mqtt_header) { .byte = SUBACK_B }
    };
    mqtt_suback(&pkt, s->pkt_id, rcs, s->tuples_len);

    size_t len = mqtt_size(&pkt, NULL);
    mqtt_pack(&pkt, c->wbuf + c->towrite);
    c->towrite += len;

    log_debug("Sending SUBACK to %s", c->client_id);

    mqtt_packet_destroy(&pkt, SUBACK);

    return REPLY;
}

static int unsubscribe_handler(struct io_event *e) {

    struct client *c = e->client;

    log_debug("Received UNSUBSCRIBE from %s", c->client_id);

    struct topic *t = NULL;
    for (int i = 0; i < e->data.unsubscribe.tuples_len; ++i) {
        t = topic_get(&server,
                      (const char *) e->data.unsubscribe.tuples[i].topic);
        if (t)
            topic_del_subscriber(t, c);
    }

    mqtt_pack_mono(c->wbuf + c->towrite, UNSUBACK, e->data.unsubscribe.pkt_id);
    c->towrite += MQTT_ACK_LEN;

    log_debug("Sending UNSUBACK to %s", c->client_id);

    mqtt_packet_destroy(&e->data, UNSUBACK);

    return REPLY;
}

static int publish_handler(struct io_event *e) {

    struct client *c = e->client;
    union mqtt_header *hdr = &e->data.header;
    struct mqtt_publish *p = &e->data.publish;
    unsigned short orig_mid = p->pkt_id;

    log_debug("Received PUBLISH from %s (d%i, q%u, r%i, m%u, %s, ... (%llu bytes))",
              c->client_id,
              hdr->bits.dup,
              hdr->bits.qos,
              hdr->bits.retain,
              p->pkt_id,
              p->topic,
              p->payloadlen);

    info.messages_recv++;

    char topic[p->topiclen + 2];
    unsigned char qos = hdr->bits.qos;

    /*
     * For convenience we assure that all topics ends with a '/', indicating a
     * hierarchical level
     */
    if (p->topic[p->topiclen - 1] != '/')
        snprintf(topic, p->topiclen + 2, "%s/", (const char *) p->topic);
    else
        snprintf(topic, p->topiclen + 1, "%s", (const char *) p->topic);

    /*
     * Retrieve the topic from the global map, if it wasn't created before,
     * create a new one with the name selected
     */
    struct topic *t = topic_get_or_create(&server, topic);

    /* Check for # wildcards subscriptions */
    struct iterator it;
    iter_init(&it, server.wildcards, list_iter_next);
    struct iterator *pit = &it;
    FOREACH (pit) {
        struct subscription *s = pit->ptr;
        int matched = match_subscription(topic, s->topic, s->end_wildcard);
        if (matched == 0 && !subscribed_to_topic(t, s->subscriber->client))
            topic_add_subscriber(t, s->subscriber->client, s->subscriber->qos);
    }

    struct mqtt_packet *pkt = mqtt_packet_alloc(e->data.header.byte);
    // TODO must perform a deep copy here
    pkt->publish = e->data.publish;

    size_t publen = mqtt_size(&e->data, NULL);
    if (hdr->bits.retain == 1) {
        t->retained_msg = bstring_empty(publen);
        mqtt_pack(&e->data, t->retained_msg);
    }

    if (publish_message(pkt, t, e->ctx) == 0)
        xfree(pkt);

    // We have to answer to the publisher
    if (qos == AT_MOST_ONCE)
        goto exit;

    int ptype = PUBACK;

    // TODO check for unwanted values
    if (qos == EXACTLY_ONCE) {
        ptype = PUBREC;
        struct mqtt_packet *ack = mqtt_packet_alloc(ptype);
        INCREF(ack, struct mqtt_packet);
        mqtt_ack(ack, orig_mid);
        inflight_msg_init(&c->session->in_i_acks[orig_mid], c, ack, publen);
        c->session->has_inflight = true;
    }

    log_debug("Sending %s to %s (m%u)",
              ptype == PUBACK ? "PUBACK" : "PUBREC", c->client_id, orig_mid);

    mqtt_ack(&e->data, ptype == PUBACK ? PUBACK_B : PUBREC_B);
    mqtt_pack_mono(c->wbuf + c->towrite, ptype, orig_mid);
    c->towrite += MQTT_ACK_LEN;

    return REPLY;

exit:

    /*
     * We're in the case of AT_MOST_ONCE QoS level, we don't need to sent out
     * any byte, it's a fire-and-forget.
     */
    return NOREPLY;
}

static int puback_handler(struct io_event *e) {
    struct client *c = e->client;
    unsigned pkt_id = e->data.ack.pkt_id;
    log_debug("Received PUBACK from %s (m%u)", c->client_id, pkt_id);
    c->session->i_msgs[pkt_id].in_use = 0;
    inflight_msg_clear(&c->session->i_msgs[pkt_id]);
    c->session->i_acks[pkt_id].in_use = 0;
    inflight_msg_clear(&c->session->i_acks[pkt_id]);
    c->session->has_inflight = false;
    return NOREPLY;
}

static int pubrec_handler(struct io_event *e) {
    struct client *c = e->client;
    unsigned pkt_id = e->data.ack.pkt_id;
    log_debug("Received PUBREC from %s (m%u)", c->client_id, pkt_id);
    mqtt_pack_mono(c->wbuf + c->towrite, PUBREL, pkt_id);
    c->towrite += MQTT_ACK_LEN;
    // Update inflight acks table
    c->session->i_acks[pkt_id].seen = time(NULL);
    log_debug("Sending PUBREL to %s (m%u)", c->client_id, pkt_id);
    return REPLY;
}

static int pubrel_handler(struct io_event *e) {
    struct client *c = e->client;
    unsigned pkt_id = e->data.ack.pkt_id;
    log_debug("Received PUBREL from %s (m%u)", c->client_id, pkt_id);
    mqtt_pack_mono(c->wbuf + c->towrite, PUBCOMP, pkt_id);
    c->towrite += MQTT_ACK_LEN;
    c->session->in_i_acks[pkt_id].in_use = 0;
    c->session->has_inflight = false;
    inflight_msg_clear(&c->session->in_i_acks[pkt_id]);
    log_debug("Sending PUBCOMP to %s (m%u)", c->client_id, pkt_id);
    return REPLY;
}

static int pubcomp_handler(struct io_event *e) {
    struct client *c = e->client;
    unsigned pkt_id = e->data.ack.pkt_id;
    log_debug("Received PUBCOMP from %s (m%u)", c->client_id, pkt_id);
    c->session->i_acks[pkt_id].in_use = 0;
    c->session->i_msgs[pkt_id].in_use = 0;
    inflight_msg_clear(&c->session->i_msgs[pkt_id]);
    inflight_msg_clear(&c->session->i_acks[pkt_id]);
    c->session->has_inflight = false;
    return NOREPLY;
}

static int pingreq_handler(struct io_event *e) {
    log_debug("Received PINGREQ from %s", e->client->client_id);
    e->data.header.byte = PINGRESP_B;
    mqtt_pack(&e->data, e->client->wbuf + e->client->towrite);
    e->client->towrite += MQTT_HEADER_LEN;
    log_debug("Sending PINGRESP to %s", e->client->client_id);
    return REPLY;
}

/* This is the only public API we expose from this module */
int handle_command(unsigned type, struct io_event *event) {
    return handlers[type](event);
}
