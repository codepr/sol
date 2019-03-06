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

#ifndef TRIE_H
#define TRIE_H

#include <stdio.h>
#include <stdbool.h>
#include "list.h"


typedef struct trie Trie;

/*
 * Trie node, it contains a fixed size array (every node can have at max the
 * alphabet length size of children), a flag defining if the node represent
 * the end of a word and then if it contains a value defined by data.
 */
struct trie_node {
    char chr;
    List *children;
    void *data;
};

/*
 * Trie ADT, it is formed by a root struct trie_node, and the total size of the
 * Trie
 */
struct trie {
    struct trie_node *root;
    size_t size;
};

// Returns new trie node (initialized to NULLs)
struct trie_node *trie_create_node(char);

// Returns a new Trie, which is formed by a root node and a size
struct trie *trie_create(void);

void trie_init(Trie *);

// Return the size of the trie
size_t trie_size(const Trie *);

/*
 * The leaf represents the node with the associated data
 *           .
 *          / \
 *         h   s: s -> value
 *        / \
 *       e   k: hk -> value
 *      /
 *     l: hel -> value
 *
 * Here we got 3 <key:value> pairs:
 * - s   -> value
 * - hk  -> value
 * - hel -> value
 */
void *trie_insert(Trie *, const char *, const void *);

bool trie_delete(Trie *, const char *);

/* Returns true if key presents in trie, else false, the last pointer to
   pointer is used to store the value associated with the searched key, if
   present */
bool trie_find(const Trie *, const char *, void **);

void trie_node_free(struct trie_node *, size_t *);

void trie_release(Trie *);

/* Remove all keys matching a given prefix in a less than linear time
   complexity */
void trie_prefix_delete(Trie *, const char *);

/* Count all keys matching a given prefix in a less than linear time
   complexity */
int trie_prefix_count(const Trie *, const char *);

/* Search for all keys matching a given prefix */
List *trie_prefix_find(const Trie *, const char *);

/* Apply a given function to all nodes which keys match a given prefix */
void trie_prefix_map(Trie *, const char *, void (*mapfunc)(struct trie_node *));

/*
 * Apply a given function to all ndoes which keys match a given prefix. The
 * function accepts two arguments, a struct trie_node pointer which correspond
 * to each node on the trie after the prefix node and a void pointer, used for
 * additional data which can be useful to the execution of `mapfunc`.
 */
void trie_prefix_map_tuple(Trie *, const char *,
                           void (*mapfunc)(struct trie_node *, void *), void *);

#endif
