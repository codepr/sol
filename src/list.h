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

#ifndef LIST_H
#define LIST_H


struct list_node {
    void *data;
    struct list_node *next;
};


typedef struct list {
    struct list_node *head;
    struct list_node *tail;
    unsigned long len;
    int (*destructor)(struct list_node *);
} List;


/*
 * Compare function, accept two void * arguments, generally referring a node
 * and his subsequent
 */
typedef int (*compare_func)(void *, void *);

/* Create an empty list */
List *list_create(int (*destructor)(struct list_node*));

/*
 * Release a list, accept a integer flag to control the depth of the free call
 * (e.g. going to free also data field of every node)
 */
void list_release(List *, int);

/* Return list size */
unsigned long list_size(const List *);

/* Clear out the list without de-allocating it */
void list_clear(List *, int);

/* Attach a list to another one on tail */
List *list_attach(List *, struct list_node *, unsigned long);

/* Insert data into a node and push it to the front of the list */
List *list_push(List *, void *);

/* Insert data into a node and push it to the back of the list */
List *list_push_back(List *, void *);

/*
 * Remove a node from the list based on a compare function that must be
 * previously defined and passed in as a function pointer, accept two void
 * *args, which generally means a node and his subsequent
 */
void list_remove(List *, struct list_node *, compare_func);

/*
 * Remove a single node from the list, the first one satisfy compare_func
 * criteria, without de-allocating it
 */
struct list_node *list_remove_node(List *, void *, compare_func);

/* Comapare function for merge_sort application */
typedef int cmp(void *, void *);

/* Merge sort customized on TTL of new values data with complexity of O(nlogn) */
struct list_node *list_merge_sort(struct list_node *, cmp);

/* Divide a list in 2 sublists at roughly the middle of the original list */
struct list_node *bisect_list(struct list_node *);

/* Insert a new node into a list while maintaining the order of the elements */
struct list_node *list_sort_insert(struct list_node **,
                                   struct list_node *, compare_func);


#endif
