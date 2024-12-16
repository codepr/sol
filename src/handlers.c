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

#include "handlers.h"
#include "config.h"
#include "logging.h"
#include "memory.h"
#include "mqtt.h"
#include "server.h"
#include "sol_internal.h"
#include "util.h"
#include <stdio.h>

/* Prototype for a command handler */
typedef int handler(Connection_Context *);

/* Command handler, each one have responsibility over a defined command packet
 */
static int connect_handler(Connection_Context *);
static int disconnect_handler(Connection_Context *);
static int subscribe_handler(Connection_Context *);
static int unsubscribe_handler(Connection_Context *);
static int publish_handler(Connection_Context *);
static int puback_handler(Connection_Context *);
static int pubrec_handler(Connection_Context *);
static int pubrel_handler(Connection_Context *);
static int pubcomp_handler(Connection_Context *);
static int pingreq_handler(Connection_Context *);

static void session_init(struct client_session *, const char *);

static struct client_session *client_session_alloc(const char *);

static unsigned next_free_mid(struct client_session *);

static void inflight_msg_init(struct inflight_msg *, struct mqtt_packet *);

/* Command handler mapped usign their position paired with their type */
static handler *handlers[15] = {NULL,
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
                                disconnect_handler};

/*
 * =========================
 *  Internal module helpers
 * =========================
 */

static void session_free(const struct ref *refcount)
{
    struct client_session *session =
        container_of(refcount, struct client_session, refcount);
    list_destroy(session->subscriptions, 0);
    list_destroy(session->outgoing_msgs, 0);
    if (has_inflight(session)) {
        for (int i = 0; i < MAX_INFLIGHT_MSGS; ++i) {
            if (session->i_msgs[i].packet)
                DECREF(session->i_msgs[i].packet, struct mqtt_packet);
        }
    }
    free_memory(session->i_acks);
    free_memory(session->i_msgs);
    free_memory(session);
}

static void session_init(struct client_session *session, const char *session_id)
{
    session->inflights     = 0;
    session->next_free_mid = 1;
    session->subscriptions = list_new(NULL);
    session->outgoing_msgs = list_new(NULL);
    snprintf(session->session_id, MQTT_CLIENT_ID_LEN, "%s", session_id);
    session->i_acks = try_calloc(MAX_INFLIGHT_MSGS, sizeof(time_t));
    session->i_msgs =
        try_calloc(MAX_INFLIGHT_MSGS, sizeof(struct inflight_msg));
    session->refcount = (struct ref){session_free, 0};
}

static struct client_session *client_session_alloc(const char *session_id)
{
    struct client_session *session = try_alloc(sizeof(*session));
    session_init(session, session_id);
    return session;
}

static inline unsigned next_free_mid(struct client_session *session)
{
    if (session->next_free_mid == MAX_INFLIGHT_MSGS)
        session->next_free_mid = 1;
    return session->next_free_mid++;
}

static inline void inflight_msg_init(struct inflight_msg *imsg,
                                     struct mqtt_packet *p)
{
    imsg->seen   = time(NULL);
    imsg->packet = p;
    imsg->qos    = p->header.bits.qos;
}

/*
 * One of the two exposed functions of the module, it's also needed on server
 * module to publish periodic messages (e.g. $SOL stats). It's responsible
 * of the normal publish but also taking care of disconnected clients, enqueuing
 * packets and setting up inflight messages for QoS > 0.
 * Returns the number of publish done or an error code in case of conditions
 * that requires de-allocation of the pkt argument occurs.
 */
int publish_message(struct mqtt_packet *pkt, const struct topic *t)
{

    bool all_at_most_once = true;
    size_t len            = 0;
    unsigned short mid    = 0;
    unsigned char qos     = pkt->header.bits.qos;
    int count             = HASH_COUNT(t->subscribers);

    if (count == 0) {
        INCREF(pkt, struct mqtt_packet);
        goto exit;
    }

    // first run check
    struct subscriber *sub, *dummy;
    HASH_ITER(hh, t->subscribers, sub, dummy)
    {
        struct client_session *s = sub->session;
        Connection_Context *sc   = NULL;
        HASH_FIND_STR(server.context_map, s->session_id, sc);
        /*
         * Update QoS according to subscriber's one, following MQTT
         * rules: The min between the original QoS and the subscriber
         * QoS
         */
        pkt->header.bits.qos = qos >= sub->granted_qos ? sub->granted_qos : qos;
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
            mid                 = next_free_mid(s);
            pkt->publish.pkt_id = mid;
            INCREF(pkt, struct mqtt_packet);
            /*
             * If offline, we must enqueue messages in the inflight queue
             * of the client, they will be sent out only in case of a
             * clean_session == false connection
             */
            if (!sc || sc->online == false) {
                if (s->clean_session == false) {
                    list_push(s->outgoing_msgs, pkt);
                    all_at_most_once = false;
                    INCREF(pkt, struct mqtt_packet);
                    inflight_msg_init(&s->i_msgs[mid], pkt);
                    s->i_acks[mid] = time(NULL);
                    ++s->inflights;
                }
                continue;
            }
            /*
             * The subscriber client is marked as online, so we proceed to
             * set the inflight messages according to the QoS level required
             * and write back the payload
             */
            inflight_msg_init(&sc->session->i_msgs[mid], pkt);
            sc->session->i_acks[mid] = time(NULL);
            ++sc->session->inflights;
            all_at_most_once = false;
        }
        mqtt_write(pkt, sc->send_buf + sc->write_total);
        sc->write_total += len;

        // Schedule a write for the current subscriber on the next event cycle
        enqueue_event_write(sc);

        info.messages_sent++;

        log_debug(
            "Sending PUBLISH to %s (d%i, q%u, r%i, m%u, %s, ... (%i bytes))",
            sc->cid, pkt->header.bits.dup, pkt->header.bits.qos,
            pkt->header.bits.retain, pkt->publish.pkt_id, pkt->publish.topic,
            pkt->publish.payloadlen);
    }

    // add return code
    if (all_at_most_once == true)
        count = 0;

exit:

    return count;
}

/*
 * Check if a topic matches a wildcard subscription. It works with + and # as
 * well
 */
static int match_subscription(const char *topic, const char *wtopic,
                              bool multilevel)
{
    size_t len = strlen(wtopic);
    int i = 0, j = 0;
    bool found   = false;
    char *ptopic = (char *)topic;

    if (!ptopic)
        return -SOL_ERR;

    /*
     * Cycle through the wildcard topic, char by char, seeking for '+' char and
     * at the same time assuring that every char is equal in the topic as well,
     * we don't want to accept different topics
     */
    while (i < len && wtopic[i]) {
        j = 0;
        for (; i < len; ++i, ++j) {
            if (wtopic[i] == '+') {
                found = true;
                break;
            } else if (wtopic[i] != ptopic[j]) {
                return -SOL_ERR;
            }
        }
        /*
         * Get a pointer to the next '/', called two times because we want to
         * skip the first occurence, like foo/bar/baz, cause at this point we'
         * re already at /bar/baz and we don't need a pointer to /bar/baz
         * again
         */
        if (ptopic[0] == '/')
            ptopic++;
        ptopic = index(ptopic, '/');
        if (ptopic[0] == '/')
            ptopic = index(ptopic + 1, '/');
        i++;
    }
    if (!found && ptopic && multilevel == true)
        return SOL_OK;
    if (ptopic && (ptopic[0] == '/' || ptopic[1] != '\0') &&
        multilevel == false)
        return -SOL_ERR;
    return SOL_OK;
}

/*
 * Command handlers
 */

static void set_connack(Connection_Context *c, unsigned char rc, unsigned sp)
{
    unsigned char connect_flags = 0 | (sp & 0x1) << 0;

    struct mqtt_packet response = {
        .header  = {.byte = CONNACK_B},
        .connack = (struct mqtt_connack){.byte = connect_flags, .rc = rc}};
    mqtt_write(&response, c->send_buf + c->write_total);
    c->write_total += MQTT_ACK_LEN;

    /*
     * If a session was present and the connected client have disabled the
     * clean session flag, we have to take care of the outgoing messages
     * pending, strictly after the CONNACK encoding
     */
    if (c->clean_session == false && sp == 1) {
        log_info("Resuming session for %s", c->cid);
        /*
         * If there's already some subscriptions and pending messages,
         * empty the queue
         */
        // TODO check for write buffer size exceed
        if (list_size(c->session->outgoing_msgs) > 0) {
            size_t len = 0;
            list_foreach(item, c->session->outgoing_msgs)
            {
                len = mqtt_size(item->data, NULL);
                mqtt_write(item->data, c->send_buf + c->write_total);
                c->write_total += len;
            }
            // We want to clean up the queue after the payload set
            list_clear(c->session->outgoing_msgs, 0);
        }
    }
}

static int connect_handler(Connection_Context *cc)
{

    unsigned session_present = 0;
    struct mqtt_connect *c   = &cc->data.connect;

    if (cc->connected == true) {
        /*
         * Already connected client, 2 CONNECT packet should be interpreted as
         * a violation of the protocol, causing disconnection of the client
         */
        log_info("Received double CONNECT from %s, disconnecting client",
                 c->payload.client_id);
        goto e_client_dc;
    }

    /*
     * If allow_anonymous is false we need to check for an existing
     * username:password pair match in the authentications table
     */
    if (conf->allow_anonymous == false) {
        if (c->bits.username == 0 || c->bits.password == 0)
            goto e_bad_auth;
        else {
            struct authentication *auth = NULL;
            HASH_FIND_STR(server.auths, (char *)c->payload.username, auth);
            if (!auth || !check_passwd((char *)c->payload.password, auth->salt))
                goto e_bad_auth;
        }
    }

    /*
     * No client ID and clean_session == false? you're not authorized, we don't
     * know who you are
     */
    if (!c->payload.client_id[0] && c->bits.clean_session == false)
        goto e_not_authorized;

    /*
     * Check for client ID, if not present generate a random ID, otherwise add
     * the client to the sessions map if not already present
     */
    if (!c->payload.client_id[0])
        generate_random_id((char *)c->payload.client_id);
    /*
     * Add the new connected client to the global map, if it is already
     * connected, kick him out accordingly to the MQTT v3.1.1 specs.
     */
    snprintf(cc->cid, MQTT_CLIENT_ID_LEN, "%s", c->payload.client_id);

    // First we check if a session is present
    HASH_FIND_STR(server.sessions, cc->cid, cc->session);
    if (cc->session && c->bits.clean_session == true)
        // Clean session true, we have to clean old session, if any
        HASH_DEL(server.sessions, cc->session);
    else if (cc->session)
        session_present = 1;

    cc->connected = true;

    log_info("New client connected as %s (c%i, k%u)", c->payload.client_id,
             c->bits.clean_session, c->payload.keepalive);

    /*
     * If no session was found or the client is a new connecting client or an
     * anonymous one, we create a session here
     */
    if (c->bits.clean_session == true || !cc->session) {
        cc->session = client_session_alloc(cc->cid);
        INCREF(cc->session, struct client_session);
        HASH_ADD_STR(server.sessions, session_id, cc->session);
    }

    cc->session->clean_session = c->bits.clean_session;

    // Let's track client on the global map to be used on publish
    HASH_ADD_STR(server.context_map, cid, cc);

    // Add LWT topic and message if present
    if (c->bits.will) {
        cc->has_lwt              = true;
        const char *will_topic   = (const char *)c->payload.will_topic;
        const char *will_message = (const char *)c->payload.will_message;
        // TODO check for will_topic != NULL
        struct topic *t = topic_repo_fetch_default(server.repo, will_topic);
        if (!topic_repo_contains(server.repo, t->name))
            topic_repo_put(server.repo, t);
        // I'm sure that the string will be NUL terminated by unpack function
        size_t msg_len       = strlen(will_message);
        size_t tpc_len       = strlen(will_topic);

        cc->session->lwt_msg = (struct mqtt_packet){
            .header  = (union mqtt_header){.byte = PUBLISH_B},
            .publish = (struct mqtt_publish){
                .pkt_id     = 0, // placeholder
                .topiclen   = tpc_len,
                .topic      = (unsigned char *)try_strdup(will_topic),
                .payloadlen = msg_len,
                .payload    = (unsigned char *)try_strdup(will_message)}};

        cc->session->lwt_msg.header.bits.qos = c->bits.will_qos;
        // We must store the retained message in the topic
        if (c->bits.will_retain == 1) {
            size_t publen          = mqtt_size(&cc->session->lwt_msg, NULL);
            unsigned char *payload = try_alloc(publen);
            mqtt_write(&cc->session->lwt_msg, payload);
            // We got a ready-to-be-sent bytestring in the retained message
            // field
            t->retained_msg = payload;
        }
        log_info("Will message specified (%lu bytes)",
                 cc->session->lwt_msg.publish.payloadlen);
        log_info("\t%s", cc->session->lwt_msg.publish.payload);
    }

    // TODO check for session already present

    cc->clean_session = c->bits.clean_session;

    set_connack(cc, MQTT_CONNECTION_ACCEPTED, session_present);

    log_debug("Sending CONNACK to %s (%u, %u)", cc->cid, session_present,
              MQTT_CONNECTION_ACCEPTED);

    return REPLY;

e_client_dc:

    return -ERRCLIENTDC;

e_bad_auth:
    log_debug("Sending CONNACK to %s (%u, %u)", cc->cid, session_present,
              MQTT_BAD_USERNAME_OR_PASSWORD);
    set_connack(cc, MQTT_BAD_USERNAME_OR_PASSWORD, session_present);

    return MQTT_BAD_USERNAME_OR_PASSWORD;

e_not_authorized:
    log_debug("Sending CONNACK to %s (%u, %u)", cc->cid, session_present,
              MQTT_NOT_AUTHORIZED);
    set_connack(cc, MQTT_NOT_AUTHORIZED, session_present);

    return MQTT_NOT_AUTHORIZED;
}

static int disconnect_handler(Connection_Context *c)
{
    log_debug("Received DISCONNECT from %s", c->cid);
    return -ERRCLIENTDC;
}

static inline void add_wildcard(const char *topic, struct subscriber *s,
                                bool wildcard)
{
    struct subscription *subscription = try_alloc(sizeof(*subscription));
    subscription->subscriber          = s;
    subscription->topic               = try_strdup(topic);
    subscription->multilevel          = wildcard;
    INCREF(s, struct subscriber);
    topic_repo_add_wildcard(server.repo, subscription);
}

static void recursive_sub(struct trie_node *node, void *arg)
{
    if (!node || !node->data)
        return;
    struct topic *t      = node->data;
    /*
     * We need to make a copy of the subscriber cause UTHASH needs a proper
     * handle to work correctly, otherwise we'll end up freeing the same
     * refernce on disconnect and break the table
     */
    struct subscriber *s = subscriber_clone(arg), *tmp;
    HASH_FIND_STR(t->subscribers, s->id, tmp);
    if (!tmp) {
        INCREF(s, struct subscriber);
        HASH_ADD_STR(t->subscribers, id, s);
    }
    log_debug("Adding subscriber %s to topic %s", s->session->session_id,
              t->name);
    list_push(s->session->subscriptions, t);
}

static int subscribe_handler(Connection_Context *c)
{

    bool wildcard            = false;
    struct mqtt_subscribe *s = &c->data.subscribe;

    /*
     * We respond to the subscription request with SUBACK and a list of QoS in
     * the same exact order of reception
     */
    unsigned char rcs[s->tuples_len];

    /* Subscribe packets contains a list of topics and QoS tuples */
    for (unsigned i = 0; i < s->tuples_len; i++) {

        log_debug("Received SUBSCRIBE from %s", c->cid);

        /*
         * Check if the topic exists already or in case create it and store in
         * the global map
         */
        char topic[s->tuples[i].topic_len + 2];
        snprintf(topic, s->tuples[i].topic_len + 1, "%s", s->tuples[i].topic);

        log_debug("\t%s (QoS %i)", topic, s->tuples[i].qos);
        /* Recursive subscribe to all children topics if the topic ends with
         * "/#" */
        if (topic[s->tuples[i].topic_len - 1] == '#' &&
            topic[s->tuples[i].topic_len - 2] == '/') {
            topic[s->tuples[i].topic_len - 1] = '\0';
            wildcard                          = true;
        } else if (topic[s->tuples[i].topic_len - 1] != '/') {
            topic[s->tuples[i].topic_len]     = '/';
            topic[s->tuples[i].topic_len + 1] = '\0';
        }

        struct topic *t = topic_repo_fetch_default(server.repo, topic);
        /*
         * Let's explore two possible scenarios:
         * 1. Normal topic (no single level wildcard '+') which can end with
         *    multilevel wildcard '#'
         * 2. A topic contaning one or more single level wildcard '+'
         */
        if (!index(topic, '+')) {
            struct subscriber *tmp;
            HASH_FIND_STR(t->subscribers, c->cid, tmp);
            if (c->clean_session == true || !tmp) {
                if (!tmp) {
                    tmp = topic_add_subscriber(t, c->session, s->tuples[i].qos);
                    // we increment reference for the subscriptions session
                    INCREF(tmp, struct subscriber);
                }
                list_push(c->session->subscriptions, t);
                if (wildcard == true) {
                    add_wildcard(topic, tmp, wildcard);
                    topic_repo_map(server.repo, topic, recursive_sub, tmp);
                }
            }
        } else {
            /*
             * Here we encountered at least 1 single level wildcard '+', we add
             * the topic to the wildcards list as we can't know at this point
             * which topic it will match
             */
            struct subscriber *sub =
                subscriber_new(c->session, s->tuples[i].qos);
            add_wildcard(topic, sub, wildcard);
        }

        // Retained message? Publish it
        // TODO move after SUBACK response
        if (t->retained_msg) {
            size_t len = alloc_size(t->retained_msg);
            memcpy(c->send_buf + c->write_total, t->retained_msg, len);
            c->write_total += len;
        }
        rcs[i] = s->tuples[i].qos;
    }

    struct mqtt_packet pkt = {.header = (union mqtt_header){.byte = SUBACK_B}};
    mqtt_suback(&pkt, s->pkt_id, rcs, s->tuples_len);

    size_t len = mqtt_size(&pkt, NULL);
    mqtt_write(&pkt, c->send_buf + c->write_total);
    c->write_total += len;

    log_debug("Sending SUBACK to %s", c->cid);

    mqtt_packet_free(&pkt);

    return REPLY;
}

static int unsubscribe_handler(Connection_Context *c)
{

    log_debug("Received UNSUBSCRIBE from %s", c->cid);

    struct topic *t = NULL;
    for (int i = 0; i < c->data.unsubscribe.tuples_len; ++i) {
        t = topic_repo_fetch(server.repo,
                             (const char *)c->data.unsubscribe.tuples[i].topic);
        if (t)
            topic_del_subscriber(t, c);
    }
    mqtt_write_ack(c->send_buf + c->write_total, UNSUBACK,
                   c->data.unsubscribe.pkt_id);
    c->write_total += MQTT_ACK_LEN;

    log_debug("Sending UNSUBACK to %s", c->cid);

    mqtt_packet_free(&c->data);

    return REPLY;
}

static int publish_handler(Connection_Context *c)
{

    union mqtt_header *hdr  = &c->data.header;
    struct mqtt_publish *p  = &c->data.publish;
    unsigned short orig_mid = p->pkt_id;

    log_debug(
        "Received PUBLISH from %s (d%i, q%u, r%i, m%u, %s, ... (%llu bytes))",
        c->cid, hdr->bits.dup, hdr->bits.qos, hdr->bits.retain, p->pkt_id,
        p->topic, p->payloadlen);

    info.messages_recv++;

    char topic[p->topiclen + 2];
    unsigned char qos = hdr->bits.qos;

    /*
     * For convenience we assure that all topics ends with a '/', indicating a
     * hierarchical level
     */
    if (p->topic[p->topiclen - 1] != '/')
        snprintf(topic, p->topiclen + 2, "%s/", (const char *)p->topic);
    else
        snprintf(topic, p->topiclen + 1, "%s", (const char *)p->topic);

    /*
     * Retrieve the topic from the global map, if it wasn't created before,
     * create a new one with the name selected
     */
    struct topic *t = topic_repo_fetch_default(server.repo, topic);

    /* Check for # wildcards subscriptions */
    if (topic_repo_wildcards_empty(server.repo)) {
        topic_repo_wildcards_foreach(item, server.repo)
        {
            struct subscription *s = item->data;
            int matched = match_subscription(topic, s->topic, s->multilevel);
            if (matched == SOL_OK &&
                !is_subscribed(t, s->subscriber->session)) {
                /*
                 * We need to make a copy of the subscriber cause UTHASH needs
                 * a proper handle to work correctly, otherwise we'll end up
                 * freeing the same refernce on disconnect and break the table
                 */
                struct subscriber *copy = subscriber_clone(s->subscriber);
                INCREF(copy, struct subscriber);
                HASH_ADD_STR(t->subscribers, id, copy);
                list_push(s->subscriber->session->subscriptions, t);
            }
        }
    }
    struct mqtt_packet *pkt = mqtt_packet_alloc(c->data.header.byte);
    // TODO must perform a deep copy here
    pkt->publish            = c->data.publish;

    if (hdr->bits.retain == 1) {
        t->retained_msg = try_alloc(mqtt_size(&c->data, NULL));
        mqtt_write(&c->data, t->retained_msg);
    }

    if (publish_message(pkt, t) == 0)
        DECREF(pkt, struct mqtt_packet);

    // We have to answer to the publisher
    if (qos == AT_MOST_ONCE)
        goto exit;

    int ptype = qos == EXACTLY_ONCE ? PUBREC : PUBACK;

    mqtt_ack(&c->data, ptype == PUBACK ? PUBACK_B : PUBREC_B);
    mqtt_write_ack(c->send_buf + c->write_total, ptype, orig_mid);
    c->write_total += MQTT_ACK_LEN;
    log_debug("Sending %s to %s (m%u)", ptype == PUBACK ? "PUBACK" : "PUBREC",
              c->cid, orig_mid);
    return REPLY;

exit:

    /*
     * We're in the case of AT_MOST_ONCE QoS level, we don't need to send out
     * any byte, it's a fire-and-forget.
     */
    return NOREPLY;
}

static int puback_handler(Connection_Context *c)
{
    unsigned pkt_id = c->data.ack.pkt_id;
    log_debug("Received PUBACK from %s (m%u)", c->cid, pkt_id);
    inflight_msg_clear(&c->session->i_msgs[pkt_id]);
    c->session->i_msgs[pkt_id].packet = NULL;
    c->session->i_acks[pkt_id]        = -1;
    --c->session->inflights;
    return NOREPLY;
}

static int pubrec_handler(Connection_Context *c)
{
    unsigned pkt_id = c->data.ack.pkt_id;
    log_debug("Received PUBREC from %s (m%u)", c->cid, pkt_id);
    mqtt_write_ack(c->send_buf + c->write_total, PUBREL, pkt_id);
    c->write_total += MQTT_ACK_LEN;
    // Update inflight acks table
    c->session->i_acks[pkt_id] = time(NULL);
    log_debug("Sending PUBREL to %s (m%u)", c->cid, pkt_id);
    return REPLY;
}

static int pubrel_handler(Connection_Context *c)
{
    unsigned pkt_id = c->data.ack.pkt_id;
    log_debug("Received PUBREL from %s (m%u)", c->cid, pkt_id);
    mqtt_write_ack(c->send_buf + c->write_total, PUBCOMP, pkt_id);
    c->write_total += MQTT_ACK_LEN;
    log_debug("Sending PUBCOMP to %s (m%u)", c->cid, pkt_id);
    return REPLY;
}

static int pubcomp_handler(Connection_Context *c)
{
    unsigned pkt_id = c->data.ack.pkt_id;
    log_debug("Received PUBCOMP from %s (m%u)", c->cid, pkt_id);
    c->session->i_acks[pkt_id] = -1;
    inflight_msg_clear(&c->session->i_msgs[pkt_id]);
    c->session->i_msgs[pkt_id].packet = NULL;
    --c->session->inflights;
    return NOREPLY;
}

static int pingreq_handler(Connection_Context *c)
{
    log_debug("Received PINGREQ from %s", c->cid);
    c->data.header.byte = PINGRESP_B;
    mqtt_write(&c->data, c->send_buf + c->write_total);
    c->write_total += MQTT_HEADER_LEN;
    log_debug("Sending PINGRESP to %s", c->cid);
    return REPLY;
}

/*
 * This is the only public API we expose from this module beside
 * publish_message. It just give access to handlers mapped by message type.
 */
int handle_command(unsigned type, Connection_Context *context)
{
    return handlers[type](context);
}
