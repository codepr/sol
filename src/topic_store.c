/*
 * BSD 2-Clause License
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

#include "list.h"
#include "memory.h"
#include "sol_internal.h"
#include "trie.h"
#include "util.h"

static int wildcard_destructor(struct list_node *);

static bool topic_destructor(struct trie_node *, bool);

static int subscription_cmp(const void *, const void *);

/*
 * Allocate a new store structure on the heap and return it after its
 * initialization, also allocating a new list on the heap to keep track of
 * wildcard topics.
 * The function may gracefully crash as the memory allocation may fail.
 */
struct topic_store *topic_store_new(void)
{
    struct topic_store *store = try_alloc(sizeof(*store));
    store->topics             = trie_new(topic_destructor);
    store->wildcards          = list_new(wildcard_destructor);
    return store;
}

/*
 * Deallocate heap memory for the list and every wildcard item stored into,
 * also the store is deallocated
 */
void topic_store_destroy(struct topic_store *store)
{
    list_destroy(store->wildcards, 1);
    trie_destroy(store->topics);
    free_memory(store);
}

/*
 * Insert a topic into the store or update it if already present
 */
void topic_store_put(struct topic_store *store, struct topic *t)
{
    trie_insert(store->topics, t->name, t);
}

/*
 * Remove a topic into the store
 */
void topic_store_del(struct topic_store *store, const char *name)
{
    trie_delete(store->topics, name);
}

/*
 * Check if the store contains a topic by name key
 */
bool topic_store_contains(const struct topic_store *store, const char *name)
{
    struct topic *t = topic_store_get(store, name);
    return t != NULL;
}

/*
 * Return a topic associated to a topic name from the store, returns NULL if no
 * topic is found.
 */
struct topic *topic_store_get(const struct topic_store *store, const char *name)
{
    struct topic *ret_topic;
    trie_find(store->topics, name, (void *)&ret_topic);
    return ret_topic;
}

/*
 * Return a topic associated to a topic name from the store, if no topic is
 * insert it into the store before returning it. Like topic_store_get but
 * cannot return NULL.
 * The function may fail as in case of no topic found it tries to allocate
 * space on the heap for the new inserted topic.
 */
struct topic *topic_store_get_or_put(struct topic_store *store,
                                     const char *name)
{
    struct topic *t = topic_store_get(store, name);
    if (t != NULL)
        return t;
    t = topic_new(try_strdup(name));
    topic_store_put(store, t);
    return t;
}

/*
 * Add a wildcard topic to the topic_store struct, does not check if it already
 * exists
 */
void topic_store_add_wildcard(struct topic_store *store, struct subscription *s)
{
    store->wildcards = list_push(store->wildcards, s);
}

/*
 * Remove a wildcard by id key from the topic_store struct
 */
void topic_store_remove_wildcard(struct topic_store *store, char *id)
{
    list_remove(store->wildcards, id, subscription_cmp);
}

/*
 * Run a function to each node of the topic_store trie holding the topic
 * entries
 */
void topic_store_map(struct topic_store *store, const char *prefix,
                     void (*fn)(struct trie_node *, void *), void *arg)
{
    trie_prefix_map(store->topics->root, prefix, fn, arg);
}

/*
 * Check if the wildcards list of the topic_store is empty
 */
bool topic_store_wildcards_empty(const struct topic_store *store)
{
    return list_size(store->wildcards) == 0;
}

/*
 * Auxiliary function, destructor to be passed in to init a list structure,
 * this one is used to correctly destroy struct subscription items
 */
static int wildcard_destructor(struct list_node *node)
{
    if (!node)
        return -SOL_ERR;
    struct subscription *s = node->data;
    DECREF(s->subscriber, struct subscriber);
    free_memory((char *)s->topic);
    free_memory(s);
    free_memory(node);
    return SOL_OK;
}

/*
 * Auxiliary function, destructor to be passed in to init a trie structure,
 * used to release topics inside the main topic store
 */
static bool topic_destructor(struct trie_node *node, bool flag)
{
    if (!node || !node->data)
        return false;
    struct topic *t = node->data;
    topic_destroy(t);
    return true;
}

/*
 * Auxiliary compare function to be passed in as comparator to a list_remove
 * call
 */
static int subscription_cmp(const void *ptr_s1, const void *ptr_s2)
{
    struct subscription *s1 = ((struct list_node *)ptr_s1)->data;
    const char *id          = ptr_s2;
    return STREQ(s1->subscriber->id, id, MQTT_CLIENT_ID_LEN);
}
