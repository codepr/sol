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

#include "memory.h"
#include "sol_internal.h"

/*
 * Initialize a struct topic pointer by setting its name, subscribers and
 * retained_msg are set to NULL.
 * The function expects a non-null pointer and can't fail, if a null topic
 * is passed, the function return prematurely.
 */
void topic_init(struct topic *t, const char *name)
{
    if (!t)
        return;
    t->name         = name;
    t->subscribers  = NULL;
    t->retained_msg = NULL;
}

/*
 * Allocate a new topic struct on the heap, initialize it then return a pointer
 * to it. The function can fail as a memory allocation is requested, if it
 * fails the program execution graceful crash.
 */
struct topic *topic_new(const char *name)
{
    struct topic *t = try_alloc(sizeof(*t));
    topic_init(t, name);
    return t;
}

/*
 * Deallocate the topic name, retained_msg and all its subscribers
 */
void topic_destroy(struct topic *t)
{
    if (!t)
        return;
    free_memory((void *)t->name);
    free_memory(t->retained_msg);
    if (!t->subscribers) {
        free_memory(t);
        return;
    }
    struct subscriber *sub, *dummy;
    HASH_ITER(hh, t->subscribers, sub, dummy)
    {
        if (!sub)
            continue;
        HASH_DEL(t->subscribers, sub);
        free_memory(sub);
    }
    free_memory(t);
}

/*
 * Allocate a new subscriber struct on the heap referring to the passed in
 * topic, client_session and QoS, then add it to the topic map.
 * The function can fail as a memory allocation is requested, if it fails the
 * program execution graceful crash.
 */
struct subscriber *topic_add_subscriber(struct topic *t,
                                        struct client_session *s,
                                        unsigned char qos)
{
    struct subscriber *sub = subscriber_new(s, qos), *tmp;
    HASH_FIND_STR(t->subscribers, sub->id, tmp);
    if (!tmp)
        HASH_ADD_STR(t->subscribers, id, sub);
    return sub;
}

/*
 * Remove a subscriber from the topic, the subscriber to be removed refers to
 * the client_id belonging to the client pointer passed in.
 * The subscriber deletion is really a reference count subtraction, DECREF
 * macro takes care of the counter, if it reaches 0 it de-allocates the memory
 * reserved to the struct subscriber.
 * The function can't fail.
 */
void topic_del_subscriber(struct topic *t, struct client *c)
{
    struct subscriber *sub = NULL;
    HASH_FIND_STR(t->subscribers, c->client_id, sub);
    if (sub) {
        HASH_DEL(t->subscribers, sub);
        DECREF(sub, struct subscriber);
    }
}
