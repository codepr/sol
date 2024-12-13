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
#include "util.h"

static void subscriber_destroy(const struct ref *);

/*
 * Allocate memory on the heap to create and return a pointer to a struct
 * subscriber, assigining the passed in QoS, session pointer, and
 * instantiating a reference counter to 0.
 * It may fail as it needs to allocate some bytes on the heap.
 */
struct subscriber *subscriber_new(struct client_session *s, unsigned char qos)
{
    struct subscriber *sub = try_alloc(sizeof(*sub));
    sub->session           = s;
    sub->granted_qos       = qos;
    sub->refcount = (struct ref){.count = 0, .free = subscriber_destroy};
    memcpy(sub->id, s->session_id, MQTT_CLIENT_ID_LEN);
    return sub;
}

/*
 * Allocate memory on the heap to clone a subscriber pointer, deep copies all
 * fields into the newly allocated pointer except for the reference counter,
 * the new pointer will have its own refcount set to 0. Finally the newly
 * allocated pointer is returned.
 * It may fail as it needs to allocate some bytes on the heap.
 */
struct subscriber *subscriber_clone(const struct subscriber *s)
{
    struct subscriber *sub = try_alloc(sizeof(*sub));
    sub->session           = s->session;
    sub->granted_qos       = s->granted_qos;
    sub->refcount = (struct ref){.count = 0, .free = subscriber_destroy};
    memcpy(sub->id, s->id, MQTT_CLIENT_ID_LEN);
    return sub;
}

/*
 * Checks if a client is subscribed to a topic by trying to fetch the
 * client_session by its ID on the subscribers inner hashmap of the topic.
 */
bool is_subscribed(const struct topic *t, const struct client_session *s)
{
    struct subscriber *dummy = NULL;
    HASH_FIND_STR(t->subscribers, s->session_id, dummy);
    return dummy != NULL;
}

/*
 * Auxiliary function, defines the destructor behavior for subscriber, just
 * decreasing the reference counter till 0, then free the memory.
 */
static void subscriber_destroy(const struct ref *r)
{
    struct subscriber *sub = container_of(r, struct subscriber, refcount);
    free_memory(sub);
}
