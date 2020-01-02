#include <errno.h>
#include <string.h>
#include <sys/epoll.h>
#include "handlers.h"
#include "core.h"
#include "mqtt.h"
#include "util.h"
#include "config.h"
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

void publish_message(struct mqtt_publish *p,
                     const struct topic *t, int epollfd) {

    unsigned char qos = p->header.bits.qos;
    size_t publen = 0;
    bstring payload = NULL;
    unsigned char *pub = NULL;
    union mqtt_packet pkt = { .publish = *p };

    if (hashtable_size(t->subscribers) == 0)
        return;

    struct iterator *it = iter_new(t->subscribers, hashtable_iter_next);
    unsigned char type, opcode;

    // first run check
    FOREACH (it) {
        struct subscriber *sub = it->ptr;
        struct client *sc = sub->client;

        /*
         * Update QoS according to subscriber's one, following MQTT
         * rules: The min between the original QoS and the subscriber
         * QoS
         */
        p->header.bits.qos = qos >= sub->qos ? sub->qos : qos;

        /*
         * If offline, we must enqueue messages in the inflight queue
         * of the client, they will be sent out only in case of a
         * clean_session == false connection
         */
        pthread_spin_lock(&w_spinlock);
        if (sc->online == false && sc->clean_session == false) {
            pkt.publish.header.bits.qos = p->header.bits.qos;
            struct inflight_msg *im =
                inflight_msg_new(sc, &pkt, PUBLISH, publen);
            sol_session_append_imsg(sc->session, im);
            continue;
        }

        /*
         * Proceed with the publish towards online subscriber.
         * TODO move the IO part into the dedicated workers. Coded
         * here as first simpler working version
         */
        if (p->header.bits.qos > AT_MOST_ONCE) {
            // QoS > 0 (1|2)
            publen = MQTT_HEADER_LEN + sizeof(uint16_t) +
                p->topiclen + p->payloadlen;
            // Add 2 bytes to make space for packet identifier
            publen += sizeof(uint16_t);

            int remaninglen_offset = 0;
            if ((publen - 1) > 0x200000)
                remaninglen_offset = 3;
            else if ((publen - 1) > 0x4000)
                remaninglen_offset = 2;
            else if ((publen - 1) > 0x80)
                remaninglen_offset = 1;

            publen += remaninglen_offset;

            pkt.publish.pkt_id = next_free_mid(sc->i_msgs);
            pkt.publish.header.bits.qos = p->header.bits.qos;
            pub = pack_mqtt_packet(&pkt, PUBLISH);

            if (!sc->i_msgs[pkt.publish.pkt_id])
                sc->i_msgs[pkt.publish.pkt_id] =
                    inflight_msg_new(sc, &pkt, PUBLISH, publen);

            unsigned short mid = next_free_mid(sc->i_acks);
            if (!sc->i_acks[mid]) {
                type = sub->qos == AT_LEAST_ONCE ? PUBACK : PUBREC;
                opcode = type == PUBACK ? PUBACK_B : PUBREC_B;
                union mqtt_packet ack = {
                    .ack = *mqtt_packet_ack(opcode, mid)
                };
                sc->i_acks[mid] =
                    inflight_msg_new(sc, &ack, type, publen);
            }
        } else {
            /*
             * QoS 0
             *
             * Set the correct size of the output packet and set the
             * correct QoS value (0) and packet identifier to 0 as
             * specified by MQTT specs
             */
            publen = MQTT_HEADER_LEN + sizeof(uint16_t) +
                p->topiclen + p->payloadlen;

            int remaninglen_offset = 0;
            if ((publen - 1) > 0x200000)
                remaninglen_offset = 3;
            else if ((publen - 1) > 0x4000)
                remaninglen_offset = 2;
            else if ((publen - 1) > 0x80)
                remaninglen_offset = 1;

            publen += remaninglen_offset;
            pkt.publish.header.bits.qos = 0;
            pkt.publish.pkt_id = 0;
            pub = pack_mqtt_packet(&pkt, PUBLISH);
        }
        pthread_spin_unlock(&w_spinlock);

        pthread_spin_lock(&io_spinlock);
        payload = bstring_copy(pub, publen);
        xfree(pub);

        // Trigger write on IO threadpool
        struct io_event *event = xmalloc(sizeof(*event));
        event->epollfd = epollfd;
        event->rc = REPLY;
        event->client = sc;
        event->reply = payload;

        epoll_mod(event->epollfd, sc->conn->fd, EPOLLOUT, event);

        info.messages_sent++;

        pthread_spin_unlock(&io_spinlock);
        log_debug("Sending PUBLISH to %s (d%i, q%u, r%i, m%u, %s, ... (%i bytes))",
                  sc->client_id,
                  p->header.bits.dup,
                  p->header.bits.qos,
                  p->header.bits.retain,
                  pkt.publish.pkt_id,
                  p->topic,
                  p->payloadlen);
    }
    iter_destroy(it);
}

/*
 * Command handlers
 */

static void set_payload_connack(struct io_event *e, unsigned char rc) {
    union mqtt_packet response;
    unsigned char session_present = 0;
    unsigned char connect_flags = 0 | (session_present & 0x1) << 0;

    response.connack = (struct mqtt_connack) {
        .header = { .byte = CONNACK_B },
            .byte = connect_flags,
            .rc = rc
    };
    unsigned char *packed = pack_mqtt_packet(&response, CONNACK);
    e->reply = bstring_copy(packed, MQTT_ACK_LEN);
    xfree(packed);
    if (rc != RC_CONNECTION_ACCEPTED) {
        if (e->client->session) {
            list_destroy(e->client->session->subscriptions, 0);
            xfree(e->client->session);
        }
    }
}

static int connect_handler(struct io_event *e) {

    pthread_spin_lock(&w_spinlock);

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
            void *salt = hashtable_get(sol.authentications, c->payload.username);
            if (!salt)
                goto bad_auth;

            bool authenticated =
                check_passwd((const char *) c->payload.password, salt);
            if (authenticated == false)
                goto bad_auth;
        }
    }

    if (!c->payload.client_id[0] && c->bits.clean_session == false)
        goto not_authorized;

    /*
     * Check for client ID, if not present generate a random ID, otherwise add
     * the client to the sessions map if not already present
     */
    if (!c->payload.client_id[0]) {
        generate_random_id((char *) c->payload.client_id);
    } else {
        struct session *s = hashtable_get(sol.sessions, c->payload.client_id);
        if (s == NULL) {
            struct session *new_s = sol_session_new();
            hashtable_put(sol.sessions,
                          xstrdup((char *) c->payload.client_id), new_s);
        } else {
            if (c->bits.clean_session == false) {
                // A session is present and we want to re-establish that
                struct topic *t;
                char *tname = NULL;
                // Send the messages in queue
                for (size_t i = 0; i < cc->session->msg_queue_next; ++i) {
                    tname = (char *) cc->session->msg_queue[i]->packet->publish.topic;
                    t = sol_topic_get_or_create(&sol, tname);
                    publish_message(&cc->session->msg_queue[i]->packet->publish, t, e->epollfd);
                    xfree(cc->session->msg_queue[i]);
                }
                cc->session->msg_queue_next = 0;
            } else {
                /*
                 * Requested a clean session, delete the older one and create
                 * a fresh one
                 */
                hashtable_del(sol.sessions, c->payload.client_id);
                struct session *new_s = sol_session_new();
                hashtable_put(sol.sessions,
                              xstrdup((char *) c->payload.client_id), new_s);
            }
        }
    }

    // TODO just return error_code and handle it on `on_read`
    if (hashtable_exists(sol.clients, c->payload.client_id)) {

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
    size_t cid_len = strlen((const char *) c->payload.client_id);
    memcpy(e->client->client_id, c->payload.client_id, cid_len + 1);
    hashtable_put(sol.clients, e->client->client_id, e->client);

    // Add LWT topic and message if present
    if (c->bits.will) {
        // TODO check for will_topic != NULL
        struct topic *t =
            sol_topic_get_or_create(&sol, (char *) c->payload.will_topic);
        if (!sol_topic_exists(&sol, t->name))
            sol_topic_put(&sol, t);
        // I'm sure that the string will be NUL terminated by unpack function
        size_t msg_len = strlen((const char *) c->payload.will_message);
        size_t tpc_len = strlen((const char *) c->payload.will_topic);

        struct mqtt_publish *lwt = xmalloc(sizeof(*lwt));
        lwt->header = (union mqtt_header) { .byte = PUBLISH_B };
        lwt->pkt_id = 0;  // placeholder
        lwt->topiclen = tpc_len;
        lwt->topic = c->payload.will_topic;
        lwt->payloadlen = msg_len;
        lwt->payload = c->payload.will_message;
        e->client->lwt_msg = lwt;
        // We must store the retained message in the topic
        if (c->bits.will_retain == 1) {
            union mqtt_packet up = { .publish = *lwt };
            // Update the QOS of the retained message according to the desired
            // one by the connected client
            up.publish.header.bits.qos = c->bits.will_qos;
            size_t publen = MQTT_HEADER_LEN + sizeof(uint16_t) +
                tpc_len + msg_len;
            if (c->bits.will_qos > AT_MOST_ONCE)
                publen += sizeof(uint16_t);
            unsigned char *pub = pack_mqtt_packet(&up, PUBLISH);
            bstring payload = bstring_copy(pub, publen);
            // We got a ready-to-be sent bytestring in the retained message
            // field
            t->retained_msg = payload;
            xfree(pub);
        }
        log_info("Will message specified (%lu bytes)", lwt->payloadlen);
        log_info("\t%s", lwt->payload);
    }

    // TODO check for session already present

    if (c->bits.clean_session == false) {
        e->client->clean_session = false;
        e->client->session->subscriptions = list_new(NULL);
    }

    set_payload_connack(e, RC_CONNECTION_ACCEPTED);

    pthread_spin_unlock(&w_spinlock);

    log_debug("Sending CONNACK to %s r=%u",
              c->payload.client_id, RC_CONNECTION_ACCEPTED);

    return REPLY;

clientdc:

    pthread_spin_unlock(&w_spinlock);

    return CLIENTDC;

bad_auth:
    log_debug("Sending CONNACK to %s rc=%u",
              c->payload.client_id, RC_BAD_USERNAME_OR_PASSWORD);  // TODO check for session
    set_payload_connack(e, RC_BAD_USERNAME_OR_PASSWORD);

    pthread_spin_unlock(&w_spinlock);

    return RC_BAD_USERNAME_OR_PASSWORD;

not_authorized:
    log_debug("Sending CONNACK to %s rc=%u",
              c->payload.client_id, RC_NOT_AUTHORIZED); // TODO check for session
    set_payload_connack(e, RC_NOT_AUTHORIZED);

    pthread_spin_unlock(&w_spinlock);

    return RC_NOT_AUTHORIZED;
}

static void rec_sub(struct trie_node *node, void *arg) {
    if (!node || !node->data)
        return;
    struct topic *t = node->data;
    struct subscriber *s = arg;
    s->refs++;
    log_debug("Adding subscriber %s to topic %s",
              s->client->client_id, t->name);
    hashtable_put(t->subscribers, s->client->client_id, s);
    if (s->client->session)
        list_push(s->client->session->subscriptions, t);
}

static int disconnect_handler(struct io_event *e) {

    log_debug("Received DISCONNECT from %s", e->client->client_id);

    pthread_spin_lock(&w_spinlock);

    // Remove from subscriptions for now
    if (e->client->session) {
        struct list *subs = e->client->session->subscriptions;
        struct iterator *it = iter_new(subs, list_iter_next);
        FOREACH (it) {
            log_debug("Removing %s from topic %s",
                      e->client->client_id, ((struct topic *) it->ptr)->name);
            topic_del_subscriber(it->ptr, e->client, false);
        }
        iter_destroy(it);
    }
    pthread_spin_unlock(&w_spinlock);

    // TODO remove from all topic where it subscribed
    return CLIENTDC;
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

        pthread_spin_lock(&w_spinlock);

        /* Recursive subscribe to all children topics if the topic ends with "/#" */
        if (topic[s->tuples[i].topic_len - 1] == '#' &&
            topic[s->tuples[i].topic_len - 2] == '/') {
            topic[s->tuples[i].topic_len - 1] = '\0';
            wildcard = true;
        } else if (topic[s->tuples[i].topic_len - 1] != '/') {
            topic[s->tuples[i].topic_len] = '/';
            topic[s->tuples[i].topic_len + 1] = '\0';
        }

        struct topic *t = sol_topic_get_or_create(&sol, topic);

        if (wildcard == true) {
            struct subscriber *sub = xmalloc(sizeof(*sub));
            sub->client = e->client;
            sub->qos = s->tuples[i].qos;
            sub->refs = 0;
            trie_prefix_map(sol.topics.root, topic, rec_sub, sub);
        }

        // Clean session true for now
        topic_add_subscriber(t, e->client, s->tuples[i].qos, false);

        // Retained message? Publish it
        // TODO move to IO threadpool
        if (t->retained_msg) {
            ssize_t sent = send_data(e->client->conn, t->retained_msg,
                                     bstring_len(t->retained_msg));
            if (sent < 0)
                log_error("Error publishing to %s: %s",
                          e->client->client_id, strerror(errno));

            info.messages_sent++;
            info.bytes_sent += sent;
        }

        pthread_spin_unlock(&w_spinlock);

        rcs[i] = s->tuples[i].qos;
    }

    struct mqtt_suback *suback =
        mqtt_packet_suback(SUBACK_B, s->pkt_id, rcs, s->tuples_len);

    union mqtt_packet pkt = { .suback = *suback };
    unsigned char *packed = pack_mqtt_packet(&pkt, SUBACK);
    size_t len = MQTT_HEADER_LEN + sizeof(uint16_t) + s->tuples_len;

    log_debug("Sending SUBACK to %s", c->client_id);

    e->reply = bstring_copy(packed, len);
    xfree(packed);
    mqtt_packet_release(&pkt, SUBACK);
    xfree(suback);

    return REPLY;
}

static int unsubscribe_handler(struct io_event *e) {

    struct client *c = e->client;

    log_debug("Received UNSUBSCRIBE from %s", c->client_id);

    struct topic *t = NULL;
    for (int i = 0; i < e->data.unsubscribe.tuples_len; ++i) {
        t = sol_topic_get(&sol,
                          (const char *) e->data.unsubscribe.tuples[i].topic);
        if (t)
            topic_del_subscriber(t, c, false);
    }

    unsigned char packed[MQTT_ACK_LEN];
    mqtt_pack_mono(packed, UNSUBACK, e->data.unsubscribe.pkt_id);

    log_debug("Sending UNSUBACK to %s", c->client_id);

    e->reply = bstring_copy(packed, MQTT_ACK_LEN);
    mqtt_packet_release(&e->data, UNSUBACK);

    return REPLY;
}

static int publish_handler(struct io_event *e) {

    struct client *c = e->client;
    struct mqtt_publish *p = &e->data.publish;
    unsigned short orig_mid = p->pkt_id;

    log_debug("Received PUBLISH from %s (d%i, q%u, r%i, m%u, %s, ... (%llu bytes))",
              c->client_id,
              p->header.bits.dup,
              p->header.bits.qos,
              p->header.bits.retain,
              p->pkt_id,
              p->topic,
              p->payloadlen);

    info.messages_recv++;

    char topic[p->topiclen + 2];
    strncpy(topic, (const char *) p->topic, p->topiclen);
    unsigned char qos = p->header.bits.qos;

    /*
     * For convenience we assure that all topics ends with a '/', indicating a
     * hierarchical level
     */
    if (topic[p->topiclen] != '/') {
        topic[p->topiclen] = '/';
        topic[p->topiclen + 1] = '\0';
    }

    /*
     * Retrieve the topic from the global map, if it wasn't created before,
     * create a new one with the name selected
     */
    struct topic *t = sol_topic_get_or_create(&sol, topic);
    union mqtt_packet pkt = e->data;

    unsigned char *pub = pack_mqtt_packet(&pkt, PUBLISH);
    size_t publen = MQTT_HEADER_LEN + sizeof(uint16_t) +
        p->topiclen + p->payloadlen;

    if (p->header.bits.qos > AT_MOST_ONCE)
        publen += sizeof(uint16_t);

    if (p->header.bits.retain == 1)
        t->retained_msg = bstring_copy(pub, publen);

    xfree(pub);

    publish_message(p, t, e->epollfd);

    mqtt_packet_release(&e->data, PUBLISH);

    // We have to answer to the publisher

    pthread_spin_lock(&w_spinlock);

    if (qos == AT_MOST_ONCE)
        goto exit;

    int ptype = PUBACK;
    unsigned char packed[MQTT_ACK_LEN];

    // TODO check for unwanted values
    if (qos == AT_LEAST_ONCE) {
        log_debug("Sending PUBACK to %s (m%u)", c->client_id, orig_mid);
    } else if (qos == EXACTLY_ONCE) {
        // TODO add to a hashtable to track PUBREC clients last
        log_debug("Sending PUBREC to %s (m%u)", c->client_id, orig_mid);
        ptype = PUBREC;
        union mqtt_packet ack = {
            .ack = *mqtt_packet_ack(PUBREC_B, orig_mid)
        };
        c->in_i_acks[orig_mid] =
            inflight_msg_new(c, &ack, ptype, publen);
    }

    e->data.ack = *mqtt_packet_ack(ptype == PUBACK ? PUBACK_B : PUBREC_B,
                                   orig_mid);
    mqtt_pack_mono(packed, ptype, orig_mid);
    e->reply = bstring_copy(packed, MQTT_ACK_LEN);

    pthread_spin_unlock(&w_spinlock);

    return REPLY;

exit:

    pthread_spin_unlock(&w_spinlock);

    /*
     * We're in the case of AT_MOST_ONCE QoS level, we don't need to sent out
     * any byte, it's a fire-and-forget.
     */
    return NOREPLY;
}

static int puback_handler(struct io_event *e) {
    pthread_spin_lock(&w_spinlock);
    struct client *c = e->client;
    log_debug("Received PUBACK from %s (m%u)",
              c->client_id, e->data.ack.pkt_id);
    if (c->i_msgs[e->data.ack.pkt_id]) {
        xfree(c->i_msgs[e->data.ack.pkt_id]);
        c->i_msgs[e->data.ack.pkt_id] = NULL;
    }
    if (c->i_acks[e->data.ack.pkt_id]) {
        xfree(c->i_acks[e->data.ack.pkt_id]);
        c->i_acks[e->data.ack.pkt_id] = NULL;
    }
    pthread_spin_unlock(&w_spinlock);
    return NOREPLY;
}

static int pubrec_handler(struct io_event *e) {
    pthread_spin_lock(&w_spinlock);
    struct client *c = e->client;
    log_debug("Received PUBREC from %s (m%u)",
              c->client_id, e->data.ack.pkt_id);
    unsigned char packed[MQTT_ACK_LEN];
    mqtt_pack_mono(packed, PUBREL, e->data.ack.pkt_id);
    e->data.ack.header.bits.type = PUBREL;
    e->reply = bstring_copy(packed, MQTT_ACK_LEN);
    // Update inflight acks table
    if (c->i_acks[e->data.ack.pkt_id]) {
        xfree(c->i_acks[e->data.ack.pkt_id]);
        c->i_acks[e->data.ack.pkt_id] =
            inflight_msg_new(c, &e->data, PUBREL, MQTT_ACK_LEN);
    }
    log_debug("Sending PUBREL to %s (m%u)",
              c->client_id, e->data.ack.pkt_id);
    pthread_spin_unlock(&w_spinlock);
    return REPLY;
}

static int pubrel_handler(struct io_event *e) {
    pthread_spin_lock(&w_spinlock);
    log_debug("Received PUBREL from %s (m%u)",
              e->client->client_id, e->data.ack.pkt_id);
    struct client *c = e->client;
    unsigned char packed[MQTT_ACK_LEN];
    mqtt_pack_mono(packed, PUBCOMP, e->data.ack.pkt_id);
    if (c->in_i_acks[e->data.ack.pkt_id]) {
        xfree(c->in_i_acks[e->data.ack.pkt_id]);
        c->in_i_acks[e->data.ack.pkt_id] = NULL;
    }
    log_debug("Sending PUBCOMP to %s (m%u)",
              e->client->client_id, e->data.ack.pkt_id);
    e->reply = bstring_copy(packed, MQTT_ACK_LEN);
    pthread_spin_unlock(&w_spinlock);
    return REPLY;
}

static int pubcomp_handler(struct io_event *e) {
    pthread_spin_lock(&w_spinlock);
    log_debug("Received PUBCOMP from %s (m%u)",
              e->client->client_id, e->data.ack.pkt_id);
    struct client *c = e->client;
    if (c->i_acks[e->data.ack.pkt_id]) {
        xfree(c->i_acks[e->data.ack.pkt_id]);
        c->i_acks[e->data.ack.pkt_id] = NULL;
    }
    if (c->i_msgs[e->data.ack.pkt_id]) {
        xfree(c->i_msgs[e->data.ack.pkt_id]);
        c->i_msgs[e->data.ack.pkt_id] = NULL;
    }
    pthread_spin_unlock(&w_spinlock);
    // TODO Remove from inflight PUBACK clients map
    return NOREPLY;
}

static int pingreq_handler(struct io_event *e) {
    log_debug("Received PINGREQ from %s", e->client->client_id);
    e->data.ack.header.byte = PINGRESP_B;
    unsigned char *packed = pack_mqtt_packet(&e->data, PINGRESP);
    e->reply = bstring_copy(packed, MQTT_HEADER_LEN);
    xfree(packed);
    log_debug("Sending PINGRESP to %s", e->client->client_id);
    return REPLY;
}

/* This is the only public API we expose from this module */
int handle_command(unsigned type, struct io_event *event) {
    return handlers[type](event);
}
