/* BSD 2-Clause License
 *
 * Copyright (c) 2019, Andrea Giacomo Baldan
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice, this
 *   list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <string.h>
#include "util.h"
#include "core.h"

struct sol_client *sol_client_new(struct connection *c) {
    struct sol_client *client = sol_malloc(sizeof(*client));
    client->conn = c;
    client->online = true;
    client->client_id = NULL;
    client->last_action_time = time(NULL);
    client->lwt_msg = NULL;
    return client;
}

struct topic *topic_new(const char *name) {
    struct topic *t = sol_malloc(sizeof(*t));
    topic_init(t, name);
    return t;
}

void topic_init(struct topic *t, const char *name) {
    t->name = name;
    t->subscribers = hashtable_new(NULL);
    t->retained_msg = NULL;
}

void topic_add_subscriber(struct topic *t,
                          struct sol_client *client,
                          unsigned qos,
                          bool cleansession) {
    struct subscriber *sub = sol_malloc(sizeof(*sub));
    sub->client = client;
    sub->qos = qos;
    hashtable_put(t->subscribers, sub->client->client_id, sub);

    // It must be added to the session if cleansession is false
    if (!cleansession)
        client->session.subscriptions =
            list_push(client->session.subscriptions, t);

}

void topic_del_subscriber(struct topic *t,
                          struct sol_client *client,
                          bool cleansession) {
    cleansession = (bool) cleansession; // temporary placeholder for compiler
    hashtable_del(t->subscribers, client->client_id);

    // TODO remomve in case of cleansession == false
}

void sol_topic_put(struct sol *sol, struct topic *t) {
    trie_insert(&sol->topics, t->name, t);
}

void sol_topic_del(struct sol *sol, const char *name) {
    trie_delete(&sol->topics, name);
}

bool sol_topic_exists(struct sol *sol, const char *name) {
    struct topic *t = sol_topic_get(sol, name);
    return t != NULL;
}

struct topic *sol_topic_get(struct sol *sol, const char *name) {
    struct topic *ret_topic;
    trie_find(&sol->topics, name, (void *) &ret_topic);
    return ret_topic;
}

struct topic *sol_topic_get_or_create(struct sol *sol, const char *name) {
    struct topic *t = sol_topic_get(sol, name);
    if (!t) {
        t = topic_new(sol_strdup(name));
        sol_topic_put(sol, t);
    }
    return t;
}

struct pending_message *pending_message_new(int fd, union mqtt_packet *p,
                                            int type, size_t size) {
    struct pending_message *pm = sol_malloc(sizeof(*pm));
    pm->fd = fd;
    pm->ssl = NULL;
    pm->sent_timestamp = time(NULL);
    pm->packet = p;
    pm->type = type;
    pm->size = size;
    return pm;
}

struct session *sol_session_new(void) {
    struct session *s = sol_malloc(sizeof(*s));
    // TODO add a subscription destroyer
    s->subscriptions = list_new(NULL);
    return s;
}
