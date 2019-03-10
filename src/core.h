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

#ifndef CORE_H
#define CORE_H

#include "trie.h"
#include "list.h"
#include "network.h"
#include "hashtable.h"


struct topic {
    const char *name;
    List *subscribers;
};

/*
 * Main structure, a global instance will be instantiated at start, tracking
 * topics, connected clients and registered closures.
 */
struct sol {
    HashTable *clients;
    HashTable *closures;
    Trie topics;
};


struct session {
    List *subscriptions;
    // TODO add pending confirmed messages
};

/*
 * Wrapper structure around a connected client, each client can be a publisher
 * or a subscriber, it can be used to track sessions too.
 */
struct sol_client {
    char *client_id;
    int fd;
    struct session session;
    struct buffer buf;
};


struct subscriber {
    unsigned qos;
    struct sol_client *client;
};


struct topic *topic_create(const char *);

void topic_init(struct topic *, const char *);

void topic_add_subscriber(struct topic *, struct sol_client *, unsigned, bool);

void topic_del_subscriber(struct topic *, struct sol_client *, bool);

void sol_topic_put(struct sol *, struct topic *);

void sol_topic_del(struct sol *, const char *);

/* Find a topic by name and return it */
struct topic *sol_topic_get(struct sol *, const char *);


#endif
