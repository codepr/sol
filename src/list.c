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

#include "list.h"
#include "util.h"

static struct list_node *list_node_remove(struct list_node *,
                                          struct list_node *,
                                          compare_func, int *);

/*
 * Create a list, initializing all fields
 */
List *list_new(int (*destructor)(struct list_node *)) {

    List *l = sol_malloc(sizeof(List));

    if (!l)
        return NULL;

    // set default values to the List structure fields
    l->head = l->tail = NULL;
    l->len = 0L;
    // TODO if NULL set default destructor
    l->destructor = destructor;

    return l;
}

/*
 * Destroy a list, releasing all allocated memory
 */
void list_destroy(List *l, int deep) {

    if (!l)
        return;

    struct list_node *h = l->head;
    struct list_node *tmp;

    // free all nodes
    while (l->len--) {

        tmp = h->next;

        if (l->destructor)
            l->destructor(h);
        else {
            if (h) {
                if (h->data && deep == 1)
                    sol_free(h->data);
                sol_free(h);
            }
        }

        h = tmp;
    }

    // free List structure pointer
    sol_free(l);
}

unsigned long list_size(const List *list) {
    return list->len;
}

/*
 * Destroy a list, releasing all allocated memory but the list itself
 */
void list_clear(List *l, int deep) {

    if (!l || !l->head)
        return;

    struct list_node *h = l->head;
    struct list_node *tmp;

    // free all nodes
    while (l->len--) {

        tmp = h->next;

        if (h) {
            if (h->data && deep == 1)
                sol_free(h->data);
            sol_free(h);
        }

        h = tmp;
    }

    l->head = l->tail = NULL;
    l->len = 0L;
}

/*
 * Attach a node to the head of a new list
 */
List *list_attach(List *l, struct list_node *head, unsigned long len) {
    // set default values to the List structure fields
    l->head = head;
    l->len = len;
    return l;
}

/*
 * Insert value at the front of the list
 * Complexity: O(1)
 */
List *list_push(List *l, void *val) {

    struct list_node *new_node = sol_malloc(sizeof(struct list_node));

    if (!new_node)
        return NULL;

    new_node->data = val;

    if (l->len == 0) {
        l->head = l->tail = new_node;
        new_node->next = NULL;
    } else {
        new_node->next = l->head;
        l->head = new_node;
    }

    l->len++;

    return l;
}

/*
 * Insert value at the back of the list
 * Complexity: O(1)
 */
List *list_push_back(List *l, void *val) {

    struct list_node *new_node = sol_malloc(sizeof(struct list_node));

    if (!new_node)
        return NULL;

    new_node->data = val;
    new_node->next = NULL;

    if (l->len == 0) {
        l->head = l->tail = new_node;
    } else {
        l->tail->next = new_node;
        l->tail = new_node;
    }

    l->len++;

    return l;
}

void list_remove(List *l, struct list_node *node, compare_func cmp) {

    if (!l || !node)
        return;

    int counter = 0;

    l->head = list_node_remove(l->head, node, cmp, &counter);

    l->len -= counter;

}

static struct list_node *list_node_remove(struct list_node *head,
                                          struct list_node *node,
                                          compare_func cmp, int *counter) {

    if (!head)
        return NULL;

    if (cmp(head, node) == 0) {

        struct list_node *tmp_next = head->next;
        sol_free(head);
        head = NULL;

        // Update remove counter
        (*counter)++;

        return tmp_next;
    }

    head->next = list_node_remove(head->next, node, cmp, counter);

    return head;
}

static struct list_node *list_remove_single_node(struct list_node *head,
                                                 void *data,
                                                 struct list_node **ret,
                                                 compare_func cmp) {

    if (!head)
        return NULL;

    // We want the first match
    if (cmp(head, data) == 0 && !*ret) {

        struct list_node *tmp_next = head->next;

        *ret = head;

        return tmp_next;

    }

    head->next = list_remove_single_node(head->next, data, ret, cmp);

    return head;

}

struct list_node *list_remove_node(List *list, void *data, compare_func cmp) {

    if (list->len == 0 || !list)
        return NULL;

    struct list_node *node = NULL;

    list_remove_single_node(list->head, data, &node, cmp);

    if (node) {
        list->len--;
        node->next = NULL;
    }

    return node;
}

/*
 * Iterator function to retrieve the next node, uses cur pointer in the
 * iterator to track the last visited node in the list
 */
void list_iter_next(struct iterator *it) {
    if (!it)
        return;
    void *data = NULL;
    if (!it->ptr)
        it->ptr = ((List *) it->iterable)->head->data;
    else {
        data = ((struct list_node *) it->ptr)->next;
        if (data)
            it->ptr = ((struct list_node *) data)->data;
        else
            it->ptr = NULL;
    }
    it->index++;
}
