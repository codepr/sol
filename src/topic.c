/* BSD 2-Clause License
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

#include "memory.h"
#include "sol_internal.h"

void topic_init(struct topic *t, const char *name) {
    t->name = name;
    t->subscribers = NULL;
    t->retained_msg = NULL;
}

struct topic *topic_new(const char *name) {
    if (!name)
        return NULL;
    struct topic *t = try_alloc(sizeof(*t));
    topic_init(t, name);
    return t;
}

struct subscriber *topic_add_subscriber(struct topic *t,
                                        struct client_session *s,
                                        unsigned char qos) {
    struct subscriber *sub = subscriber_new(t, s, qos), *tmp;
    HASH_FIND_STR(t->subscribers, sub->id, tmp);
    if (!tmp)
        HASH_ADD_STR(t->subscribers, id, sub);
    return sub;
}

void topic_del_subscriber(struct topic *t, struct client *c) {
    struct subscriber *sub = NULL;
    HASH_FIND_STR(t->subscribers, c->client_id, sub);
    if (sub) {
        HASH_DEL(t->subscribers, sub);
        DECREF(sub, struct subscriber);
    }
}
