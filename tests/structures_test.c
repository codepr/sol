/* BSD 2-Clause License
 *
 * Copyright (c) 2025, Andrea Giacomo Baldan
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

#include "structures_test.h"
#include "../src/iterator.h"
#include "../src/list.h"
#include "../src/memory.h"
#include "../src/trie.h"
#include "unit.h"
#include <stdlib.h>
#include <string.h>

/*
 * Tests the init feature of the list
 */
static char *test_list_new(void)
{
    List *l = list_new(NULL);
    ASSERT("list::list_new...FAIL", l != NULL);
    list_free(l, 0);
    printf("list::list_new...OK\n");
    return 0;
}

/*
 * Tests the free feature of the list
 */
static char *test_list_destroy(void)
{
    List *l = list_new(NULL);
    ASSERT("list::list_destroy...FAIL", l != NULL);
    list_free(l, 0);
    printf("list::list_destroy...OK\n");
    return 0;
}

/*
 * Tests the push feature of the list
 */
static char *test_list_push(void)
{
    List *l = list_new(NULL);
    char *x = "abc";
    list_push(l, x);
    ASSERT("list::list_push...FAIL", l->len == 1);
    list_free(l, 0);
    printf("list::list_push...OK\n");
    return 0;
}

/*
 * Tests the push_back feature of the list
 */
static char *test_list_push_back(void)
{
    List *l = list_new(NULL);
    char *x = "abc";
    list_push_back(l, x);
    ASSERT("list::list_push_back...FAIL", l->len == 1);
    list_free(l, 0);
    printf("list::list_push_back...OK\n");
    return 0;
}

static int compare_str(const void *arg1, const void *arg2)
{

    const char *tn1 = ((struct list_node *)arg1)->data;
    const char *tn2 = arg2;

    if (strcmp(tn1, tn2) == 0)
        return 0;

    return -1;
}

static char *test_list_remove_node(void)
{
    List *l                = list_new(NULL);
    char *x                = "abc";
    l                      = list_push(l, x);
    struct list_node *node = list_remove_node(l, x, compare_str);
    ASSERT("list::list_remove_node...FAIL", strcmp(node->data, x) == 0);
    free_memory(node);
    list_free(l, 0);
    printf("list::list_remove_node...OK\n");
    return 0;
}

/*
 * Tests the list iterator
 */
static char *test_list_iterator(void)
{
    List *l             = list_new(NULL);
    char *x             = "abc";
    l                   = list_push(l, x);
    struct iterator *it = iter_new(l, list_iter_next);
    ASSERT("list::list_iterator::list_iter_next...FAIL",
           strcmp(it->ptr, x) == 0);
    list_free(l, 0);
    iter_free(it);
    printf("list::list_iterator...OK\n");
    return 0;
}

/*
 * Tests the creation of a ringbuffer
 */
static char *test_trie_new(void)
{
    struct Trie *trie = trie_new(NULL);
    ASSERT("trie::trie_new...FAIL", trie != NULL);
    trie_free(trie);
    printf("trie::trie_new...OK\n");
    return 0;
}

/*
 * Tests the creation of a new node
 */
static char *test_trie_create_node(void)
{
    struct trie_node *node = trie_create_node('a');
    size_t size            = 0;
    ASSERT("trie::trie_create_node...FAIL", node != NULL);
    trie_node_free(node, &size, NULL);
    printf("trie::trie_create_node...OK\n");
    return 0;
}

/*
 * Tests the insertion on the trie
 */
static char *test_trie_insert(void)
{
    struct Trie *root = trie_new(NULL);
    const char *key   = "hello";
    char *val         = "world";
    trie_insert(root, key, try_strdup(val));
    void *payload = NULL;
    bool found    = trie_find(root, key, &payload);
    ASSERT("trie::trie_insert...FAIL", (found == true && payload != NULL));
    trie_free(root);
    printf("trie::trie_insert...OK\n");
    return 0;
}

/*
 * Tests the search on the trie
 */
static char *test_trie_find(void)
{
    struct Trie *root = trie_new(NULL);
    const char *key   = "hello";
    char *val         = "world";
    trie_insert(root, key, try_strdup(val));
    void *payload = NULL;
    bool found    = trie_find(root, key, &payload);
    ASSERT("trie::trie_find...FAIL", (found == true && payload != NULL));
    trie_free(root);
    printf("trie::trie_find...OK\n");
    return 0;
}

/*
 * Tests the delete on the trie
 */
static char *test_trie_delete(void)
{
    struct Trie *root = trie_new(NULL);
    const char *key1  = "hello";
    const char *key2  = "hel";
    const char *key3  = "del";
    char *val1        = "world";
    char *val2        = "world";
    char *val3        = "world";
    trie_insert(root, key1, try_strdup(val1));
    trie_insert(root, key2, try_strdup(val2));
    trie_insert(root, key3, try_strdup(val3));
    trie_delete(root, key1);
    trie_delete(root, key2);
    trie_delete(root, key3);
    void *payload = NULL;
    bool found    = trie_find(root, key1, &payload);
    ASSERT("trie::trie_delete...FAIL", (found == false || payload == NULL));
    found = trie_find(root, key2, &payload);
    ASSERT("trie::trie_delete...FAIL", (found == false || payload == NULL));
    found = trie_find(root, key3, &payload);
    ASSERT("trie::trie_delete...FAIL", (found == false || payload == NULL));
    trie_free(root);
    printf("trie::trie_delete...OK\n");
    return 0;
}

/*
 * Tests the prefix delete on the trie
 */
static char *test_trie_prefix_delete(void)
{
    struct Trie *root = trie_new(NULL);
    const char *key1  = "hello";
    const char *key2  = "helloworld";
    const char *key3  = "hellot";
    const char *key4  = "hel";
    char *val1        = "world";
    char *val2        = "world";
    char *val3        = "world";
    char *val4        = "world";
    trie_insert(root, key1, try_strdup(val1));
    trie_insert(root, key2, try_strdup(val2));
    trie_insert(root, key3, try_strdup(val3));
    trie_insert(root, key4, try_strdup(val4));
    trie_prefix_delete(root, key1);
    void *payload = NULL;
    bool found    = trie_find(root, key1, &payload);
    ASSERT("trie::trie_prefix_delete...FAIL",
           (found == false || payload == NULL));
    found = trie_find(root, key2, &payload);
    ASSERT("trie::trie_prefix_delete...FAIL",
           (found == false || payload == NULL));
    found = trie_find(root, key3, &payload);
    ASSERT("trie::trie_prefix_delete...FAIL",
           (found == false || payload == NULL));
    found = trie_find(root, key4, &payload);
    ASSERT("trie::trie_prefix_delete...FAIL",
           (found == true || payload != NULL));
    trie_free(root);
    printf("trie::trie_prefix_delete...OK\n");
    return 0;
}

/*
 * Tests the prefix count on the trie
 */
static char *test_trie_prefix_count(void)
{
    struct Trie *root = trie_new(NULL);
    const char *key1  = "hello";
    const char *key2  = "helloworld";
    const char *key3  = "hellot";
    const char *key4  = "hel";
    char *val1        = "world";
    char *val2        = "world";
    char *val3        = "world";
    char *val4        = "world";
    trie_insert(root, key1, try_strdup(val1));
    trie_insert(root, key2, try_strdup(val2));
    trie_insert(root, key3, try_strdup(val3));
    trie_insert(root, key4, try_strdup(val4));
    int count = trie_prefix_count(root, "hel");
    ASSERT("trie::trie_prefix_count...FAIL", count == 4);
    count = trie_prefix_count(root, "helloworld!");
    ASSERT("trie::trie_prefix_count...FAIL", count == 0);
    trie_free(root);
    printf("trie::trie_prefix_count...OK\n");
    return 0;
}

/*
 * All datastructure tests
 */
char *structures_test(void)
{

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

    return 0;
}
