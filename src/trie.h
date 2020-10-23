/* BSD 2-Clause License
 *
 * Copyright (c) 2020, Andrea Giacomo Baldan
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

#include <stddef.h>
#include <stdbool.h>
#include "bst.h"
#include "list.h"

typedef struct Trie Trie;

/*
 * Trie node, it contains a fixed size array (every node can have at max the
 * alphabet length size of children), a flag defining if the node represent
 * the end of a word and then if it contains a value defined by data.
 */
struct trie_node {
    char chr;
    struct bst_node *children;
    void *data;
};

typedef bool trie_destructor(struct trie_node *, bool);

/*
 * Trie ADT, it is formed by a root struct trie_node, and the total size of
 * the Trie
 */
struct Trie {
    trie_destructor *destructor;
    struct trie_node *root;
    size_t size;
};

/* Key val abstraction, useful for range queries like GET with prefix */
struct kv_obj {
    const char *key;
    const void *data;
};

// Returns new trie node (initialized to NULLs)
struct trie_node *trie_create_node(char);

// Returns a new Trie, which is formed by a root node and a size
struct Trie *trie_new(trie_destructor *);

void trie_init(Trie *, trie_destructor *);

// Return the size of the trie
size_t trie_size(const Trie *);

/*
 * The leaf represents the node with the associated data
 *           .
 *          / \
 *         h   s: s-value
 *        / \
 *       e   k: hk-value
 *      /
 *     l: hel-value
 *
 * Here we got 3 <key:value> pairs:
 * - s: s-value
 * - hk: hk-value
 * - hel: hel-value
 */
struct node_data *trie_insert(Trie *, const char *, const void *);

bool trie_delete(Trie *, const char *);

/*
 * Returns true if key presents in trie, else false, the last pointer to
 * pointer is used to store the value associated with the searched key, if
 * present
 */
bool trie_find(const Trie *, const char *, void **);

void trie_node_destroy(struct trie_node *, size_t *, trie_destructor *);

void trie_destroy(Trie *);

/*
 * Remove all keys matching a given prefix in a less than linear time
 * complexity
 */
void trie_prefix_delete(Trie *, const char *);

/*
 * Count all keys matching a given prefix in a less than linear time
 * complexity
 */
int trie_prefix_count(const Trie *, const char *);

/* Search for all keys matching a given prefix */
List *trie_prefix_find(const Trie *, const char *);

/* Apply a given function to all nodes which keys match a given prefix */
void trie_prefix_map(struct trie_node *, const char *, void (*fn)(struct trie_node *, void *), void *);

bool trie_is_free_node(const struct trie_node *);

struct trie_node *trie_node_find(const struct trie_node *, const char *);

#endif
