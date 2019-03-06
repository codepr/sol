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


static int compare_cid(void *c1, void *c2) {
    return strcmp(((struct subscriber *) c1)->client->client_id,
                  ((struct subscriber *) c2)->client->client_id);
}


struct topic *topic_create(const char *name) {
    struct topic *t = sol_malloc(sizeof(*t));
    topic_init(t, name);
    return t;
}


void topic_init(struct topic *t, const char *name) {
    t->name = name;
    t->subscribers = list_create(NULL);
}


void topic_add_subscriber(struct topic *t,
                          struct sol_client *client,
                          unsigned qos,
                          bool cleansession) {
    struct subscriber *sub = sol_malloc(sizeof(*sub));
    sub->client = client;
    sub->qos = qos;
    t->subscribers = list_push(t->subscribers, sub);

    // It must be added to the session if cleansession is false
    if (!cleansession)
        client->session.subscriptions =
            list_push(client->session.subscriptions, t);

}


void topic_del_subscriber(struct topic *t,
                          struct sol_client *client,
                          bool cleansession) {
    list_remove_node(t->subscribers, client, compare_cid);

    // TODO remomve in case of cleansession == false
}


void sol_topic_put(struct sol *sol, struct topic *t) {
    trie_insert(&sol->topics, t->name, t);
}


void sol_topic_del(struct sol *sol, const char *name) {
    trie_delete(&sol->topics, name);
}


struct topic *sol_topic_get(struct sol *sol, const char *name) {
    struct topic *ret_topic;
    trie_find(&sol->topics, name, (void *) &ret_topic);
    return ret_topic;
}
