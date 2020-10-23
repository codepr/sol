/*
 * BSD 2-Clause License
 *
 * Copyright (c) 2020, Andrea Giacomo Baldan All rights reserved.
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

#include "trie.h"
#include "list.h"
#include "memory.h"
#include "sol_internal.h"


static int wildcard_destructor(struct list_node *);
static int subscription_cmp(const void *, const void *);

struct topic_store *topic_store_new(void) {
    struct topic_store *store = try_alloc(sizeof(*store));
    trie_init(&store->topics, NULL);
    store->wildcards = list_new(wildcard_destructor);
    return store;
}

void topic_store_destroy(struct topic_store *store) {
    list_destroy(store->wildcards, 1);
}

void topic_store_put(struct topic_store *store, struct topic *t) {
    trie_insert(&store->topics, t->name, t);
}

void topic_store_del(struct topic_store *store, const char *name) {
    trie_delete(&store->topics, name);
}

bool topic_store_contains(const struct topic_store *store, const char *name) {
    struct topic *t = topic_store_get(store, name);
    return t != NULL;
}

struct topic *topic_store_get(const struct topic_store *store,
                              const char *name) {
    struct topic *ret_topic;
    trie_find(&store->topics, name, (void *) &ret_topic);
    return ret_topic;
}

struct topic *topic_store_get_or_put(struct topic_store *store,
                                     const char *name) {
    struct topic *t = topic_store_get(store, name);
    if (t != NULL)
        return t;
    t = topic_new(try_strdup(name));
    if (!t)
        return NULL;
    topic_store_put(store, t);
    return t;
}

void topic_store_add_wildcard(struct topic_store *store, struct subscription *s) {
    store->wildcards = list_push(store->wildcards, s);
}

void topic_store_remove_wildcard(struct topic_store *store, char *id) {
    list_remove(store->wildcards, id, subscription_cmp);
}

void topic_store_map(struct topic_store *store, const char *prefix, void
                     (*fn)(struct trie_node *, void *), void *arg) {

    trie_prefix_map(store->topics.root, prefix, fn, arg);
}

bool topic_store_wildcards_empty(const struct topic_store *store) {
    return list_size(store->wildcards) == 0;
}

/*
 * Auxiliary function, destructor to be passed in to init a list structure,
 * this one is used to correctly destroy struct subscription items
 */
static int wildcard_destructor(struct list_node *node) {
    if (!node)
        return -SOL_ERR;
    struct subscription *s = node->data;
    DECREF(s->subscriber, struct subscriber);
    free_memory((char *) s->topic);
    free_memory(s);
    free_memory(node);
    return SOL_OK;
}

/*
 * Auxiliary compare function to be passed in as comparator to a list_remove
 * call
 */
static int subscription_cmp(const void *ptr_s1, const void *ptr_s2) {
    struct subscription *s1 = ((struct list_node *) ptr_s1)->data;
    const char *id = ptr_s2;
    return STREQ(s1->subscriber->id, id, MQTT_CLIENT_ID_LEN);
}
