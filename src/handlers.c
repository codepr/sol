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

#include "handlers.h"
#include "arena.h"
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

static void session_init(Session *, const char *);

static unsigned next_free_mid(Session *);

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

static void session_init(Session *session, const char *cid)
{
    session->inflights     = 0;
    session->next_mid      = 1;
    session->subscriptions = list_new(NULL);
    snprintf(session->cid, sizeof(session->cid), "%s", cid);
}

static inline unsigned next_free_mid(Session *session)
{
    if (session->next_mid == MAX_INFLIGHT_MSGS)
        session->next_mid = 1;
    return session->next_mid++;
}

/*
 * One of the two exposed functions of the module, it's also needed on server
 * module to publish periodic messages (e.g. $SOL stats). It's responsible
 * of the normal publish but also taking care of disconnected clients, enqueuing
 * packets and setting up inflight messages for QoS > 0.
 * Returns the number of publish done or an error code in case of conditions
 * that requires de-allocation of the pkt argument occurs.
 */
void publish_message(MQTT_Packet *packet, const Topic *topic,
                     Arena_Allocator *allocator)
{
    unsigned short mid = 0;
    unsigned char qos  = packet->header.bits.qos;

    if (HASH_COUNT(topic->subscribers) == 0)
        return;

    // first run check
    Subscriber *subscriber, *dummy;
    HASH_ITER(hh, topic->subscribers, subscriber, dummy)
    {
        Session *subscriber_session        = subscriber->session;
        Connection_Context *subscriber_ctx = NULL;
        HASH_FIND_STR(server.contexts, subscriber_session->cid, subscriber_ctx);

        /*
         * Update QoS according to subscriber's one, following MQTT
         * rules: The min between the original QoS and the subscriber
         * QoS
         */
        packet->header.bits.qos =
            qos >= subscriber->granted_qos ? subscriber->granted_qos : qos;

        // QoS 0 disconnected
        if (!subscriber_ctx && subscriber->granted_qos == AT_MOST_ONCE)
            continue;

        /*
         * if QoS 0
         *
         * Set the correct size of the output packet and set the
         * correct QoS value (0) and packet identifier to (0) as
         * specified by MQTT specs
         */
        packet->publish.id = 0;

        /*
         * if QoS > 0 we set packet identifier and track the inflight
         * message, proceed with the publish towards online subscriber.
         */
        if (packet->header.bits.qos > AT_MOST_ONCE) {

            mid                = next_free_mid(subscriber_session);
            packet->publish.id = mid;
            /*
             * If offline, we must enqueue messages in the inflight queue
             * of the client, they will be sent out only in case of a
             * clean_session == false connection
             */
            if (!subscriber_ctx || !subscriber_ctx->online) {
                if (!subscriber_session->clean_session) {
                    subscriber_session->i_msgs[mid].lastack_at = time(NULL);
                    subscriber_session->i_msgs[mid].packet     = packet;
                    ++subscriber_session->inflights;
                }
                continue;
            }
            /*
             * The subscriber client is marked as online, so we proceed to
             * set the inflight messages according to the QoS level required
             * and write back the payload
             */
            subscriber_ctx->session->i_msgs[mid].lastack_at = time(NULL);
            subscriber_ctx->session->i_msgs[mid].packet     = packet;
            ++subscriber_ctx->session->inflights;
        }

        subscriber_ctx->write_total += mqtt_write(
            packet, subscriber_ctx->send_buf + subscriber_ctx->write_total);

        // Schedule a write for the current subscriber on the next event cycle
        enqueue_event_write(subscriber_ctx);

        info.messages_sent++;

        log_debug(
            "Sending PUBLISH to %s (d%i, q%u, r%i, m%u, %s, ... (%i bytes))",
            subscriber_ctx->cid, packet->header.bits.dup,
            packet->header.bits.qos, packet->header.bits.retain,
            packet->publish.id, packet->publish.topic,
            packet->publish.payloadlen);
    }
}

/*
 * Check if a topic matches a wildcard subscription. It works with + and # as
 * well
 */
static int match_subscription(const char *topic,
                              const Subscription *subscription)
{
    bool multilevel    = subscription->multilevel;
    const char *wtopic = subscription->topic;
    size_t len         = strlen(wtopic);
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

static void set_connack(Connection_Context *c, unsigned char rc,
                        unsigned session_present)
{
    unsigned char connect_flags = 0 | (session_present & 0x1) << 0;

    MQTT_Packet response        = {
               .header  = {.byte = CONNACK_B},
               .connack = (MQTT_Connack){.byte = connect_flags, .rc = rc}};
    c->write_total += mqtt_write(&response, c->send_buf + c->write_total);

    /*
     * If a session was present and the connected client have disabled the
     * clean session flag, we have to take care of the outgoing messages
     * pending, strictly after the CONNACK encoding
     */
    if (c->clean_session == false && session_present == 1) {
        log_info("Resuming session for %s", c->cid);
        /*
         * If there's already some subscriptions and pending messages,
         * empty the queue
         */
        // TODO check for write buffer size exceed
        if (has_inflight(c->session)) {
            size_t len                = 0;
            Inflight_Message *message = NULL;
            for (int i = 0; i < MAX_INFLIGHT_MSGS;
                 ++i, c->write_total += len, message = &c->session->i_msgs[i]) {
                if (!message->packet)
                    continue;
                len = mqtt_write(message->packet, c->send_buf + c->write_total);
            }
        }
    }
}

static int connect_handler(Connection_Context *c)
{
    unsigned session_present = 0;
    MQTT_Connect *packet     = &c->data.connect;

    if (c->connected == true) {
        /*
         * Already connected client, 2 CONNECT packet should be interpreted as
         * a violation of the protocol, causing disconnection of the client
         */
        log_info("Received double CONNECT from %s, disconnecting client",
                 packet->payload.client_id);
        goto e_client_dc;
    }

    /*
     * If allow_anonymous is false we need to check for an existing
     * username:password pair match in the authentications table
     */
    if (conf->allow_anonymous == false) {
        if (packet->bits.username == 0 || packet->bits.password == 0)
            goto e_bad_auth;
        else {
            struct authentication *auth = NULL;
            HASH_FIND_STR(server.auths, (char *)packet->payload.username, auth);
            if (!auth ||
                !check_passwd((char *)packet->payload.password, auth->salt))
                goto e_bad_auth;
        }
    }

    /*
     * No client ID and clean_session == false? you're not authorized, we don't
     * know who you are
     */
    if (!packet->payload.client_id[0] && packet->bits.clean_session == false)
        goto e_not_authorized;

    /*
     * Check for client ID, if not present generate a random ID, otherwise add
     * the client to the sessions map if not already present
     */
    if (!packet->payload.client_id[0])
        generate_random_id((char *)packet->payload.client_id);
    /*
     * Add the new connected client to the global map, if it is already
     * connected, kick him out accordingly to the MQTT v3.1.1 specs.
     */
    snprintf(c->cid, sizeof(c->cid), "%s", packet->payload.client_id);

    // First we check if a session is present
    HASH_FIND_STR(server.sessions, c->cid, c->session);
    if (c->session && packet->bits.clean_session == true)
        // Clean session true, we have to clean old session, if any
        HASH_DEL(server.sessions, c->session);
    else if (c->session)
        session_present = 1;

    c->connected = true;

    log_info("New client connected as %s (c%i, k%u)", packet->payload.client_id,
             packet->bits.clean_session, packet->payload.keepalive);

    /*
     * If no session was found or the client is a new connecting client or an
     * anonymous one, we create a session here
     */
    if (packet->bits.clean_session || !c->session) {
        c->session =
            arena_alloc(&server.session_allocator, sizeof(*c->session));
        session_init(c->session, c->cid);
        HASH_ADD_STR(server.sessions, cid, c->session);
    }

    c->session->clean_session = packet->bits.clean_session;

    // Let's track client on the global map to be used on publish
    HASH_ADD_STR(server.contexts, cid, c);

    // Add LWT topic and message if present
    if (packet->bits.will) {
        const char *will_topic   = (const char *)packet->payload.will_topic;
        const char *will_message = (const char *)packet->payload.will_message;
        // TODO check for will_topic != NULL
        Topic *t = topic_repo_fetch_default(server.repo, will_topic);
        if (!topic_repo_contains(server.repo, t->name))
            topic_repo_put(server.repo, t);
        // I'm sure that the string will be NUL terminated by unpack function
        size_t messagelen            = strlen(will_message);
        size_t topiclen              = strlen(will_topic);

        // TODO move to arena
        c->session->lwt              = pool_alloc(&server.packet_allocator);
        c->session->lwt->header.byte = PUBLISH_B;
        c->session->lwt->publish     = (MQTT_Publish){
                .id         = 0, // placeholder
                .topiclen   = topiclen,
                .topic      = (unsigned char *)try_strdup(will_topic),
                .payloadlen = messagelen,
                .payload    = (unsigned char *)try_strdup(will_message)};

        c->session->lwt->header.bits.qos = packet->bits.will_qos;
        // We must store the retained message in the topic
        if (packet->bits.will_retain == 1) {
            size_t publen          = mqtt_size(c->session->lwt, NULL);
            // unsigned char *payload = try_alloc(publen);
            unsigned char *payload = arena_alloc(&c->allocator, publen);
            mqtt_write(c->session->lwt, payload);
            // We got a ready-to-be-sent bytestring in the retained message
            // field
            t->retained_msg = payload;
        }
        log_info("Will message specified (%lu bytes)",
                 c->session->lwt->publish.payloadlen);
        log_info("\t%s", c->session->lwt->publish.payload);
    }

    // TODO check for session already present

    c->clean_session = packet->bits.clean_session;

    set_connack(c, MQTT_CONNECTION_ACCEPTED, session_present);

    log_debug("Sending CONNACK to %s (%u, %u)", c->cid, session_present,
              MQTT_CONNECTION_ACCEPTED);

    return REPLY;

e_client_dc:

    return -ERRCLIENTDC;

e_bad_auth:
    log_debug("Sending CONNACK to %s (%u, %u)", c->cid, session_present,
              MQTT_BAD_CREDENTIALS);
    set_connack(c, MQTT_BAD_CREDENTIALS, session_present);

    return MQTT_BAD_CREDENTIALS;

e_not_authorized:
    log_debug("Sending CONNACK to %s (%u, %u)", c->cid, session_present,
              MQTT_NOT_AUTHORIZED);
    set_connack(c, MQTT_NOT_AUTHORIZED, session_present);

    return MQTT_NOT_AUTHORIZED;
}

static int disconnect_handler(Connection_Context *c)
{
    log_debug("Received DISCONNECT from %s", c->cid);
    return -ERRCLIENTDC;
}

static inline void add_wildcard(const char *topic, Subscriber *s, bool wildcard)
{
    Subscription *subscription = try_alloc(sizeof(*subscription));
    subscription->subscriber   = s;
    subscription->topic        = try_strdup(topic);
    subscription->multilevel   = wildcard;
    topic_repo_add_wildcard(server.repo, subscription);
}

static void recursive_sub(struct trie_node *node, void *arg)
{
    if (!node || !node->data)
        return;
    Topic *topic           = node->data;
    /*
     * We need to make a copy of the subscriber cause UTHASH needs a proper
     * handle to work correctly, otherwise we'll end up freeing the same
     * refernce on disconnect and break the table
     */
    Subscriber *subscriber = subscriber_clone(arg), *tmp;
    HASH_FIND_STR(topic->subscribers, subscriber->cid, tmp);
    if (!tmp) {
        HASH_ADD_STR(topic->subscribers, cid, subscriber);
    }
    log_debug("Adding subscriber %s to topic %s", subscriber->cid, topic->name);
    list_push(subscriber->session->subscriptions, topic);
}

static int subscribe_handler(Connection_Context *c)
{
    bool wildcard             = false;
    MQTT_Subscribe *subscribe = &c->data.subscribe;

    /*
     * We respond to the subscription request with SUBACK and a list of QoS in
     * the same exact order of reception
     */
    unsigned char rcs[subscribe->tuples_len];

    /* Subscribe packets contains a list of topics and QoS tuples */
    for (unsigned i = 0; i < subscribe->tuples_len; i++) {

        log_debug("Received SUBSCRIBE from %s", c->cid);

        /*
         * Check if the topic exists already or in case create it and store in
         * the global map
         */
        char topic[subscribe->tuples[i].topic_len + 2];
        snprintf(topic, sizeof(topic), "%s", subscribe->tuples[i].topic);

        log_debug("\t%s (QoS %i)", topic, subscribe->tuples[i].qos);
        /* Recursive subscribe to all children topics if the topic ends with
         * "/#" */
        if (topic[subscribe->tuples[i].topic_len - 1] == '#' &&
            topic[subscribe->tuples[i].topic_len - 2] == '/') {
            topic[subscribe->tuples[i].topic_len - 1] = '\0';
            wildcard                                  = true;
        } else if (topic[subscribe->tuples[i].topic_len - 1] != '/') {
            topic[subscribe->tuples[i].topic_len]     = '/';
            topic[subscribe->tuples[i].topic_len + 1] = '\0';
        }

        Topic *t = topic_repo_fetch_default(server.repo, topic);
        /*
         * Let's explore two possible scenarios:
         * 1. Normal topic (no single level wildcard '+') which can end with
         *    multilevel wildcard '#'
         * 2. A topic contaning one or more single level wildcard '+'
         */
        if (!index(topic, '+')) {
            Subscriber *tmp;
            HASH_FIND_STR(t->subscribers, c->cid, tmp);
            if (c->clean_session == true || !tmp) {
                if (!tmp) {
                    tmp = topic_add_subscriber(t, c->session,
                                               subscribe->tuples[i].qos);
                    // we increment reference for the subscriptions session
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
            Subscriber *sub =
                subscriber_new(c->session, subscribe->tuples[i].qos);
            add_wildcard(topic, sub, wildcard);
        }

        // Retained message? Publish it
        // TODO move after SUBACK response
        if (t->retained_msg) {
            size_t len = alloc_size(t->retained_msg);
            memcpy(c->send_buf + c->write_total, t->retained_msg, len);
            c->write_total += len;
        }
        rcs[i] = subscribe->tuples[i].qos;
    }

    MQTT_Packet packet = {.header = (MQTT_Header){.byte = SUBACK_B}};
    mqtt_suback(&packet, subscribe->id, rcs, subscribe->tuples_len);

    size_t len = mqtt_size(&packet, NULL);
    mqtt_write(&packet, c->send_buf + c->write_total);
    c->write_total += len;

    log_debug("Sending SUBACK to %s", c->cid);

    return REPLY;
}

static int unsubscribe_handler(Connection_Context *c)
{
    log_debug("Received UNSUBSCRIBE from %s", c->cid);

    Topic *topic = NULL;
    for (int i = 0; i < c->data.unsubscribe.tuples_len; ++i) {
        topic = topic_repo_fetch(
            server.repo, (const char *)c->data.unsubscribe.tuples[i].topic);
        if (topic)
            topic_del_subscriber(topic, c);
    }
    mqtt_write_ack(c->send_buf + c->write_total, UNSUBACK,
                   c->data.unsubscribe.id);
    c->write_total += MQTT_ACK_LEN;

    log_debug("Sending UNSUBACK to %s", c->cid);

    return REPLY;
}

static int publish_handler(Connection_Context *c)
{
    MQTT_Header *header  = &c->data.header;
    MQTT_Publish *packet = &c->data.publish;
    unsigned original_id = packet->id;

    log_debug(
        "Received PUBLISH from %s (d%i, q%u, r%i, m%u, %s, ... (%llu bytes))",
        c->cid, header->bits.dup, header->bits.qos, header->bits.retain,
        packet->id, packet->topic, packet->payloadlen);

    info.messages_recv++;

    // TODO move to arena
    char topic_name[packet->topiclen + 2];
    unsigned char qos = header->bits.qos;

    /*
     * For convenience we assure that all topics ends with a '/', indicating a
     * hierarchical level
     */
    if (packet->topic[packet->topiclen - 1] != '/')
        snprintf(topic_name, sizeof(topic_name), "%s/",
                 (const char *)packet->topic);
    else
        snprintf(topic_name, sizeof(topic_name), "%s",
                 (const char *)packet->topic);

    /*
     * Retrieve the topic from the global map, if it wasn't created before,
     * create a new one with the name selected
     */
    Topic *topic = topic_repo_fetch_default(server.repo, topic_name);

    /* Check for # wildcards subscriptions */
    if (!topic_repo_wildcards_empty(server.repo)) {
        topic_repo_wildcards_foreach(item, server.repo)
        {
            Subscription *subscription = item->data;
            int matched = match_subscription(topic_name, subscription);
            if (matched == SOL_OK &&
                !is_subscribed(topic, subscription->subscriber->session)) {
                /*
                 * We need to make a copy of the subscriber cause UTHASH needs
                 * a proper handle to work correctly, otherwise we'll end up
                 * freeing the same refernce on disconnect and break the table
                 */
                Subscriber *copy = subscriber_clone(subscription->subscriber);
                HASH_ADD_STR(topic->subscribers, cid, copy);
                list_push(subscription->subscriber->session->subscriptions,
                          topic);
            }
        }
    }
    // MQTT_Packet *pkt = mqtt_packet_alloc(c->data.header.byte);
    // // TODO must perform a deep copy here
    // pkt->publish            = c->data.publish;

    if (header->bits.retain == 1) {
        topic->retained_msg =
            arena_alloc(&c->allocator, mqtt_size(&c->data, NULL));
        mqtt_write(&c->data, topic->retained_msg);
    }

    publish_message(&c->data, topic, &c->allocator);

    //     mqtt_packet_free(&c->data);

    // We have to answer to the publisher
    if (qos == AT_MOST_ONCE)
        goto exit;

    int ack_type = qos == EXACTLY_ONCE ? PUBREC : PUBACK;
    packet->id   = original_id;

    mqtt_ack(&c->data, packet->id);
    mqtt_write_ack(c->send_buf + c->write_total, ack_type, packet->id);
    c->write_total += MQTT_ACK_LEN;
    log_debug("Sending %s to %s (m%u)",
              ack_type == PUBACK ? "PUBACK" : "PUBREC", c->cid, packet->id);
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
    unsigned packet_id = c->data.ack.id;
    log_debug("Received PUBACK from %s (m%u)", c->cid, packet_id);
    arena_free(&server.mqtt_allocator,
               &c->session->i_msgs[packet_id].packet->publish.topic);
    arena_free(&server.mqtt_allocator,
               &c->session->i_msgs[packet_id].packet->publish.payload);
    c->session->i_msgs[packet_id].packet     = NULL;
    c->session->i_msgs[packet_id].lastack_at = -1;
    --c->session->inflights;
    return NOREPLY;
}

static int pubrec_handler(Connection_Context *c)
{
    unsigned packet_id = c->data.ack.id;
    log_debug("Received PUBREC from %s (m%u)", c->cid, packet_id);
    c->write_total +=
        mqtt_write_ack(c->send_buf + c->write_total, PUBREL, packet_id);
    // Update inflight acks table
    c->session->i_msgs[packet_id].lastack_at = time(NULL);
    log_debug("Sending PUBREL to %s (m%u)", c->cid, packet_id);
    return REPLY;
}

static int pubrel_handler(Connection_Context *c)
{
    unsigned packet_id = c->data.ack.id;
    log_debug("Received PUBREL from %s (m%u)", c->cid, packet_id);
    c->write_total +=
        mqtt_write_ack(c->send_buf + c->write_total, PUBCOMP, packet_id);
    log_debug("Sending PUBCOMP to %s (m%u)", c->cid, packet_id);
    return REPLY;
}

static int pubcomp_handler(Connection_Context *c)
{
    unsigned packet_id = c->data.ack.id;
    log_debug("Received PUBCOMP from %s (m%u)", c->cid, packet_id);
    c->session->i_msgs[packet_id].lastack_at = -1;
    arena_free(&server.mqtt_allocator,
               &c->session->i_msgs[packet_id].packet->publish.payload);
    arena_free(&server.mqtt_allocator,
               &c->session->i_msgs[packet_id].packet->publish.topic);
    c->session->i_msgs[packet_id].packet = NULL;
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
int handle_command(Connection_Context *context)
{
    return handlers[context->data.header.bits.type](context);
}
