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

#include <stdlib.h>
#include <string.h>
#include "unit.h"
#include "structures_test.h"
#include "../src/util.h"
#include "../src/trie.h"
#include "../src/list.h"
#include "../src/hashtable.h"
#include "../src/iterator.h"

/*
 * Tests the init feature of the list
 */
static char *test_list_new(void) {
    List *l = list_new(NULL);
    ASSERT("[! list_new]: list not created", l != NULL);
    list_destroy(l, 0);
    printf(" [list::list_new]: OK\n");
    return 0;
}

/*
 * Tests the free feature of the list
 */
static char *test_list_destroy(void) {
    List *l = list_new(NULL);
    ASSERT("[! list_destroy]: list not created", l != NULL);
    list_destroy(l, 0);
    printf(" [list::list_destroy]: OK\n");
    return 0;
}

/*
 * Tests the push feature of the list
 */
static char *test_list_push(void) {
    List *l = list_new(NULL);
    char *x = "abc";
    list_push(l, x);
    ASSERT("[! list_push]: item not pushed in", l->len == 1);
    list_destroy(l, 0);
    printf(" [list::list_push]: OK\n");
    return 0;
}

/*
 * Tests the push_back feature of the list
 */
static char *test_list_push_back(void) {
    List *l = list_new(NULL);
    char *x = "abc";
    list_push_back(l, x);
    ASSERT("[! list_push_back]: item not pushed in", l->len == 1);
    list_destroy(l, 0);
    printf(" [list::list_push_back]: OK\n");
    return 0;
}


static int compare_str(void *arg1, void *arg2) {

    const char *tn1 = ((struct list_node *) arg1)->data;
    const char *tn2 = arg2;

    if (strcmp(tn1, tn2) == 0)
        return 0;

    return -1;
}


static char *test_list_remove_node(void) {
    List *l = list_new(NULL);
    char *x = "abc";
    l = list_push(l, x);
    ASSERT("[! list_remove_node :: list_push]: item not pushed in", l->len == 1);
    struct list_node *node = list_remove_node(l, x, compare_str);
    ASSERT("[! list_remove_node]: item not removed", strcmp(node->data, x) == 0);
    sol_free(node);
    list_destroy(l, 0);
    printf(" [list::list_remove_node]: OK\n");
    return 0;
}

/*
 * Tests the list iterator
 */
static char *test_list_iterator(void) {
    List *l = list_new(NULL);
    char *x = "abc";
    l = list_push(l, x);
    struct iterator *it = iter_new(l, list_iter_next);
    struct list_node *n = it->ptr;
    ASSERT("[! list_iter_next]: next iterator didn't point to the right item",
           strcmp(n->data, x) == 0);
    list_destroy(l, 0);
    iter_destroy(it);
    printf(" [list::list_iterator]: OK\n");
    return 0;
}

/*
 * Tests the creation of a ringbuffer
 */
static char *test_trie_new(void) {
    struct Trie *trie = trie_new(NULL);
    ASSERT("[! trie_new]: Trie not created", trie != NULL);
    trie_destroy(trie);
    printf(" [trie::trie_new]: OK\n");
    return 0;
}

/*
 * Tests the creation of a new node
 */
static char *test_trie_create_node(void) {
    struct trie_node *node = trie_create_node('a');
    size_t size = 0;
    ASSERT("[! trie_create_node]: struct trie_node not created", node != NULL);
    trie_node_destroy(node, &size, NULL);
    printf(" [trie::trie_create_node]: OK\n");
    return 0;
}

/*
 * Tests the insertion on the trie
 */
static char *test_trie_insert(void) {
    struct Trie *root = trie_new(NULL);
    const char *key = "hello";
    char *val = "world";
    trie_insert(root, key, sol_strdup(val));
    void *payload = NULL;
    bool found = trie_find(root, key, &payload);
    ASSERT("[! trie_insert]: Trie insertion failed",
           (found == true && payload != NULL));
    trie_destroy(root);
    printf(" [trie::trie_insert]: OK\n");
    return 0;
}

/*
 * Tests the search on the trie
 */
static char *test_trie_find(void) {
    struct Trie *root = trie_new(NULL);
    const char *key = "hello";
    char *val = "world";
    trie_insert(root, key, sol_strdup(val));
    void *payload = NULL;
    bool found = trie_find(root, key, &payload);
    ASSERT("[! trie_find]: Trie search failed",
           (found == true && payload != NULL));
    trie_destroy(root);
    printf(" [trie::trie_find]: OK\n");
    return 0;
}

/*
 * Tests the delete on the trie
 */
static char *test_trie_delete(void) {
    struct Trie *root = trie_new(NULL);
    const char *key1 = "hello";
    const char *key2 = "hel";
    const char *key3 = "del";
    char *val1 = "world";
    char *val2 = "world";
    char *val3 = "world";
    trie_insert(root, key1, sol_strdup(val1));
    trie_insert(root, key2, sol_strdup(val2));
    trie_insert(root, key3, sol_strdup(val3));
    trie_delete(root, key1);
    trie_delete(root, key2);
    trie_delete(root, key3);
    void *payload = NULL;
    bool found = trie_find(root, key1, &payload);
    ASSERT("[! trie_delete]: Trie delete failed",
           (found == false || payload == NULL));
    found = trie_find(root, key2, &payload);
    ASSERT("[! trie_delete]: Trie delete failed",
           (found == false || payload == NULL));
    found = trie_find(root, key3, &payload);
    ASSERT("[! trie_delete]: Trie delete failed",
           (found == false || payload == NULL));
    trie_destroy(root);
    printf(" [trie::trie_delete]: OK\n");
    return 0;
}

/*
 * Tests the prefix delete on the trie
 */
static char *test_trie_prefix_delete(void) {
    struct Trie *root = trie_new(NULL);
    const char *key1 = "hello";
    const char *key2 = "helloworld";
    const char *key3 = "hellot";
    const char *key4 = "hel";
    char *val1 = "world";
    char *val2 = "world";
    char *val3 = "world";
    char *val4 = "world";
    trie_insert(root, key1, sol_strdup(val1));
    trie_insert(root, key2, sol_strdup(val2));
    trie_insert(root, key3, sol_strdup(val3));
    trie_insert(root, key4, sol_strdup(val4));
    trie_prefix_delete(root, key1);
    void *payload = NULL;
    bool found = trie_find(root, key1, &payload);
    ASSERT("[! trie_prefix_delete]: Trie prefix delete key1 failed",
            (found == false || payload == NULL));
    found = trie_find(root, key2, &payload);
    ASSERT("[! trie_prefix_delete]: Trie prefix delete key2 failed",
            (found == false || payload == NULL));
    found = trie_find(root, key3, &payload);
    ASSERT("[! trie_prefix_delete]: Trie prefix delete key3 failed",
            (found == false || payload == NULL));
    found = trie_find(root, key4, &payload);
    ASSERT("[! trie_prefix_delete]: Trie prefix delete key4 success",
            (found == true || payload != NULL));
    trie_destroy(root);
    printf(" [trie::trie_prefix_delete]: OK\n");
    return 0;
}

/*
 * Tests the prefix count on the trie
 */
static char *test_trie_prefix_count(void) {
    struct Trie *root = trie_new(NULL);
    const char *key1 = "hello";
    const char *key2 = "helloworld";
    const char *key3 = "hellot";
    const char *key4 = "hel";
    char *val1 = "world";
    char *val2 = "world";
    char *val3 = "world";
    char *val4 = "world";
    trie_insert(root, key1, sol_strdup(val1));
    trie_insert(root, key2, sol_strdup(val2));
    trie_insert(root, key3, sol_strdup(val3));
    trie_insert(root, key4, sol_strdup(val4));
    int count = trie_prefix_count(root, "hel");
    ASSERT("[! trie_prefix_count]: Trie prefix count on prefix \"hel\" failed",
            count == 4);
    count = trie_prefix_count(root, "helloworld!");
    ASSERT("[! trie_prefix_count]: Trie prefix count on prefix \"helloworld!\" failed",
            count == 0);
    trie_destroy(root);
    printf(" [trie::trie_prefix_count]: OK\n");
    return 0;
}

/*
 * Tests the creation of a hashtable
 */
static char *test_hashtable_new(void) {
    HashTable *m = hashtable_new(NULL);
    ASSERT("[! hashtable_new]: hashtable not created", m != NULL);
    hashtable_destroy(m);
    printf(" [hashtable::hashtable_new]: OK\n");
    return 0;
}

/*
 * Tests the release of a hashtable
 */
static char *test_hashtable_destroy(void) {
    HashTable *m = hashtable_new(NULL);
    hashtable_destroy(m);
    printf(" [hashtable::hashtable_destroy]: OK\n");
    return 0;
}

/*
 * Tests the insertion function of the hashtable
 */
static char *test_hashtable_put(void) {
    HashTable *m = hashtable_new(NULL);
    char *key = "hello";
    char *val = "world";
    int status = hashtable_put(m, key, val);
    ASSERT("[! hashtable_put]: hashtable size = 0", hashtable_size(m) == 1);
    ASSERT("[! hashtable_put]: hashtable_put didn't work as expected",
           status == HASHTABLE_OK);
    char *val1 = "WORLD";
    hashtable_put(m, sol_strdup(key), sol_strdup(val1));
    void *ret = hashtable_get(m, key);
    ASSERT("[! hashtable_put]: hashtable_put didn't update the value",
           strcmp(val1, ret) == 0);
    hashtable_destroy(m);
    printf(" [hashtable::hashtable_put]: OK\n");
    return 0;
}

/*
 * Tests lookup function of the hashtable
 */
static char *test_hashtable_get(void) {
    HashTable *m = hashtable_new(NULL);
    char *key = "hello";
    char *val = "world";
    hashtable_put(m, sol_strdup(key), sol_strdup(val));
    char *ret = (char *) hashtable_get(m, key);
    ASSERT("[! hashtable_get]: hashtable_get didn't work as expected",
           strcmp(ret, val) == 0);
    hashtable_destroy(m);
    printf(" [hashtable::hashtable_get]: OK\n");
    return 0;
}

/*
 * Tests the deletion function of the hashtable
 */
static char *test_hashtable_del(void) {
    HashTable *m = hashtable_new(NULL);
    char *key = "hello";
    char *val = "world";
    hashtable_put(m, sol_strdup(key), sol_strdup(val));
    int status = hashtable_del(m, key);
    ASSERT("[! hashtbale_del]: hashtable size = 1", hashtable_size(m) == 0);
    ASSERT("[! hashtbale_del]: hashtbale_del didn't work as expected",
           status == HASHTABLE_OK);
    hashtable_destroy(m);
    printf(" [hashtable::hashtable_del]: OK\n");
    return 0;
}

/*
 * Tests the hashtable iterator function of the hashtable
 */
static char *test_hashtable_iterator(void) {
    HashTable *m = hashtable_new(NULL);
    char *key = "hello";
    char *val = "world";
    hashtable_put(m, sol_strdup(key), sol_strdup(val));
    char *keytwo = "foobar";
    char *valtwo = "worldfoo";
    hashtable_put(m, sol_strdup(keytwo), sol_strdup(valtwo));
    struct iterator *it = iter_new(m, hashtable_iter_next);
    ASSERT("[! hashtable_iterator]: hashtable_iterator didn't work as expected",
           strcmp(it->ptr, val) == 0);
    it = iter_next(it);
    ASSERT("[! hashtable_iterator]: hashtable_iterator didn't work as expected",
           strcmp(it->ptr, valtwo) == 0);
    hashtable_destroy(m);
    iter_destroy(it);
    printf(" [hashtable::hashtable_iterator]: OK\n");
    return 0;
}

/*
 * All datastructure tests
 */
char *structures_test() {

    RUN_TEST(test_list_new);
    RUN_TEST(test_list_destroy);
    RUN_TEST(test_list_push);
    RUN_TEST(test_list_push_back);
    RUN_TEST(test_list_remove_node);
    RUN_TEST(test_list_iterator);
    RUN_TEST(test_trie_create_node);
    RUN_TEST(test_trie_new);
    RUN_TEST(test_trie_insert);
    RUN_TEST(test_trie_find);
    RUN_TEST(test_trie_delete);
    RUN_TEST(test_trie_prefix_delete);
    RUN_TEST(test_trie_prefix_count);
    RUN_TEST(test_hashtable_new);
    RUN_TEST(test_hashtable_destroy);
    RUN_TEST(test_hashtable_put);
    RUN_TEST(test_hashtable_get);
    RUN_TEST(test_hashtable_del);
    RUN_TEST(test_hashtable_iterator);

    return 0;
}
