/*
 * BSD 2-Clause License
 *
 * Copyright (c) 2023 Andrea Giacomo Baldan All rights reserved.
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

#include "bst.h"
#include "memory.h"

#define MAX(a, b)  a > b ? a : b
#define HEIGHT(n)  !n ? 0 : n->height
#define BALANCE(n) !n ? 0 : (HEIGHT(n->left)) - (HEIGHT(n->right))

struct bst_node *bst_new(unsigned char key, const void *data)
{
    struct bst_node *node = try_alloc(sizeof(*node));
    node->key             = key;
    node->height          = 1;
    node->left            = NULL;
    node->right           = NULL;
    node->data            = (void *)data;
    return node;
}

static struct bst_node *bst_rotate_right(struct bst_node *y)
{
    struct bst_node *x  = y->left;
    struct bst_node *t2 = x->right;

    x->right            = y;
    y->left             = t2;

    y->height           = MAX(HEIGHT(y->left), HEIGHT(y->right)) + 1;
    x->height           = MAX(HEIGHT(x->left), HEIGHT(x->right)) + 1;

    return x;
}

static struct bst_node *bst_rotate_left(struct bst_node *x)
{
    struct bst_node *y  = x->left;
    struct bst_node *t2 = y->right;

    y->right            = x;
    x->left             = t2;

    x->height           = MAX(HEIGHT(x->left), HEIGHT(x->right)) + 1;
    y->height           = MAX(HEIGHT(y->left), HEIGHT(y->right)) + 1;

    return y;
}

static struct bst_node *bst_min(const struct bst_node *node)
{
    const struct bst_node *curr = node;
    while (curr->left)
        curr = curr->left;
    return (struct bst_node *)curr;
}

/*
 * Insert a new node into the AVL tree, paying attention to maintain the tree
 * balanced in order to freeze the time-complexity to O(logN) for search
 */
struct bst_node *bst_insert(struct bst_node *node, unsigned char key,
                            const void *data)
{

    // Base case, an empty tree, just add the key to the root
    if (!node)
        return bst_new(key, data);

    /*
     * Recusrive call: key, being it smaller than the root (current node) has
     * to be inserted on the left subtree
     */
    if (key < node->key)
        node->left = bst_insert(node->left, key, data);

    /*
     * Recursive call: Same reasoning as the step before, but now the key is
     * greater than the root, so it has to be inserted to the right subtree
     */
    else if (key > node->key)
        node->right = bst_insert(node->right, key, data);

    // Corner case: the node is already in the tree, no need to do anything
    else
        return node;

    /*
     * Obtain the current height of the tree after the insertion of the new
     * node: Being it unbalanced on the left or the right subtree, the height
     * can be assumed to be the max between left subtree and right subtree + 1
     * which is the root node itself
     */
    node->height = 1 + MAX((HEIGHT(node->left)), (HEIGHT(node->right)));

    /*
     * Call for BALANCE macro, which return a positive or a negative value,
     * based on the unbalanced part of the node that needs to be rotated to
     * be re-balanecd with the rest of the tree.
     *
     * A positive value means that the tree is unbalanced on the left subtree
     * a negative value means the opposite, the tree is unbalanced on the right
     * subtree
     */
    int balance  = BALANCE(node);

    /*
     * Recursive calls done before assinged to nodes the result of the
     * subsequent calls
     */
    if (balance > 1 && key < node->left->key)
        return bst_rotate_right(node);

    if (balance < -1 && key > node->right->key)
        return bst_rotate_left(node);

    if (balance > 1 && key > node->left->key) {
        node->left = bst_rotate_left(node->left);
        return bst_rotate_right(node);
    }

    if (balance < -1 && key < node->right->key) {
        node->right = bst_rotate_right(node->right);
        return bst_rotate_left(node);
    }

    return node;
}

/* Return a node from the tree if present, otherwise return NULL. O(logN). */
struct bst_node *bst_search(const struct bst_node *node, unsigned char key)
{

    // Base: No nodes in the tree
    if (!node)
        return NULL;

    // Base: We found the node, just return it
    if (key == node->key)
        return (struct bst_node *)node;

    /*
     * Recursive call: The key is smaller than the current node, we have to
     * search on the left subtree
     */
    if (key < node->key)
        return bst_search(node->left, key);

    /*
     * Recursive call: Opposite of the previous branch, the key is greater
     * than the current node, we have to search on the right subtree
     */
    else
        return bst_search(node->right, key);
}

struct bst_node *bst_delete(struct bst_node *node, unsigned char key)
{
    if (!node)
        return node;
    if (key < node->key)
        node->left = bst_delete(node->left, key);
    if (key > node->key)
        node->right = bst_delete(node->right, key);
    else {
        if (!node->left || !node->right) {
            struct bst_node *tmp = node->left ? node->left : node->right;
            if (!tmp) {
                tmp  = node;
                node = NULL;
            } else
                *node = *tmp;
            free_memory(tmp);
        } else {
            struct bst_node *tmp = bst_min(node->right);
            node->key            = tmp->key;
            node->right          = bst_delete(node->right, tmp->key);
        }
    }

    // If the tree had only one node then return
    if (!node)
        return node;

    // STEP 2: UPDATE HEIGHT OF THE CURRENT NODE
    node->height = 1 + MAX((HEIGHT(node->left)), (HEIGHT(node->right)));

    // STEP 3: GET THE BALANCE FACTOR OF THIS NODE (to
    // check whether this node became unbalanced)
    int balance  = BALANCE(node);

    // If this node becomes unbalanced, then there are 4 cases

    // Left Left Case
    if (balance > 1 && BALANCE(node->left) >= 0)
        return bst_rotate_right(node);

    // Left Right Case
    if (balance > 1 && BALANCE(node->left) < 0) {
        node->left = bst_rotate_left(node->left);
        return bst_rotate_right(node);
    }

    // Right Right Case
    if (balance < -1 && BALANCE(node->right) <= 0)
        return bst_rotate_left(node);

    // Right Left Case
    if (balance < -1 && BALANCE(node->right) > 0) {
        node->right = bst_rotate_right(node->right);
        return bst_rotate_left(node);
    }

    return node;
}
