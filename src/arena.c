#include "arena.h"
#include "memory.h"
#include <assert.h>
#include <string.h>

#define DEFAULT_ALIGNMENT  sizeof(void *)
#define is_power_of_two(x) ((x != 0) && ((x & (x - 1)) == 0))

//
// MEMORY POOL BACKEND
//

/*
 * Simple memory object-pool, the purpose is to allow for fixed size objects to
 * be pre-allocated and re-use of memory blocks, so no size have to be
 * specified like in a normal malloc but only alloc and free of a pointer is
 * possible.
 */
struct memorypool {
    void *memory;
    void *free;
    struct memorypool *next;
    int block_used;
    size_t blocks_nr;
    size_t blocksize;
};

// static void memorypool_resize(struct memorypool *);

static void memorypool_add_pool(struct memorypool **pool);

struct memorypool *memorypool_new(size_t blocks_nr, size_t blocksize)
{
    struct memorypool *pool = try_alloc(sizeof(*pool));
    blocksize    = blocksize >= sizeof(intptr_t) ? blocksize : sizeof(intptr_t);
    pool->memory = try_calloc(blocks_nr, blocksize);
    pool->free   = pool->memory;
    pool->blocks_nr = blocks_nr;
    pool->blocksize = blocksize;
    if (!pool->free) {
        free_memory(pool);
        return NULL;
    }
    /*
     * We pre-assign the position of each free block in the free pointer, this
     * way we know every block position before allocating new memory, we'll
     * call it the header of each block, where we store the offset in memory
     * to reach the next free slot:
     *
     *       ____________
     *     _| 0x1ad45f02 | (0+blocksize)
     *    | |------------|
     *    | |     .      |
     *    | |     .      |
     *    |_|------------|
     *     _| 0x2ff43da1 | (1+blocksize)
     *    | |------------|
     *    | |     .      |
     *    | |     .      |
     *    |_|------------|
     *      | 0x98fff34a | (n+blocksize)
     *      |------------|
     *      |     .      |
     *
     * Just before assigning a free block of memory, we update the free pointer,
     * pointing it to the memory address previously stored as r-value in it.
     * This way everytime we allocate a new block we can refresh the next free
     * block in the list.
     */
    intptr_t *ptr = pool->free;
    for (size_t i = 1; i != blocks_nr; ++i) {
        *ptr = (intptr_t)i;
        ptr  = (intptr_t *)((char *)ptr + blocksize);
    }
    pool->block_used = 0;
    return pool;
}

void memorypool_destroy(struct memorypool *pool)
{
    free_memory(pool->memory);
    free_memory(pool);
}

void *memorypool_alloc(struct memorypool *pool)
{
    if (pool->block_used == pool->blocks_nr - 1)
        memorypool_add_pool(&pool);
    void *ptr  = pool->free;
    /*
     * After pointing the return pointer to the next free block, we need to
     * update the next free block address on the free pointer. The address is
     * already stored in the "header" of the block.
     */
    pool->free = (intptr_t *)((char *)pool->memory +
                              (*((intptr_t *)pool->free)) * pool->blocksize);
    pool->block_used++;
    return memset(ptr, 0x00, pool->blocksize);
}

void memorypool_free(struct memorypool *pool, void *ptr)
{
    /*
     * Here we just need to point the header of the pointer to the next free
     * location and udpate the current free location by pointing it to the
     * free'd pointer
     */
    *((intptr_t *)ptr) = *((intptr_t *)pool->free);
    if (pool->block_used == pool->blocks_nr - 1)
        memorypool_add_pool(&pool);
    pool->free = ptr;
    pool->block_used--;
}

// When pool is full, allocate a new pool and link it
static void memorypool_add_pool(struct memorypool **pool)
{
    struct memorypool *new_pool =
        memorypool_new((*pool)->blocks_nr, (*pool)->blocksize);
    new_pool->next = *pool;
    *pool          = new_pool;
}

void *arena_pool_alloc(void *context) { return memorypool_alloc(context); }

void arena_pool_free(void *context, void *ptr)
{
    memorypool_free(context, ptr);
}

//
// Free-List implementation Odin's like
//

static size_t calc_padding_with_header(uintptr_t ptr, uintptr_t alignment,
                                       size_t header_size);

static void free_list_free_all(Free_List *fl)
{
    fl->used                   = 0;
    Free_List_Node *first_node = (Free_List_Node *)fl->data;
    first_node->block_size     = fl->size;
    first_node->next           = NULL;
    fl->head                   = first_node;
}

static void free_list_init(Free_List *fl, void *data, size_t size)
{
    fl->data = data;
    fl->size = size;
    free_list_free_all(fl);
}

Free_List *free_list_new(size_t size)
{
    Free_List *fl = try_calloc(1, sizeof(*fl));
    void *chunk   = try_calloc(1, size);

    // uintptr_t raw     = (uintptr_t)malloc(size + 16 - 1);
    // uintptr_t aligned = (raw + 16 - 1) & ~(16 - 1);
    free_list_init(fl, chunk, size);
    return fl;
}

static void free_list_node_insert(Free_List_Node **phead,
                                  Free_List_Node *prev_node,
                                  Free_List_Node *new_node)
{
    if (prev_node == NULL) {
        if (*phead != NULL) {
            new_node->next = *phead;
        } else {
            *phead = new_node;
        }
    } else {
        if (prev_node->next == NULL) {
            prev_node->next = new_node;
            new_node->next  = NULL;
        } else {
            new_node->next  = prev_node->next;
            prev_node->next = new_node;
        }
    }
}

static void free_list_node_remove(Free_List_Node **phead,
                                  Free_List_Node *prev_node,
                                  Free_List_Node *del_node)
{
    if (prev_node == NULL) {
        *phead = del_node->next;
    } else {
        prev_node->next = del_node->next;
    }
}

static size_t calc_padding_with_header(uintptr_t ptr, uintptr_t alignment,
                                       size_t header_size)
{
    uintptr_t p, a, modulo, padding, needed_space;

    assert(is_power_of_two(alignment));

    p       = ptr;
    a       = alignment;
    modulo  = p & (a - 1); // (p % a) as it assumes alignment is a power of two

    padding = 0;
    needed_space = 0;

    if (modulo != 0) { // Same logic as 'align_forward'
        padding = a - modulo;
    }

    needed_space = (uintptr_t)header_size;

    if (padding < needed_space) {
        needed_space -= padding;

        if ((needed_space & (a - 1)) != 0) {
            padding += a * (1 + (needed_space / a));
        } else {
            padding += a * (needed_space / a);
        }
    }

    return (size_t)padding;
}

static Free_List_Node *free_list_find_first(Free_List *fl, size_t size,
                                            size_t alignment, size_t *padding_,
                                            Free_List_Node **prev_node_)
{
    // Iterates the list and finds the first free block with enough space
    Free_List_Node *node      = fl->head;
    Free_List_Node *prev_node = NULL;

    size_t padding            = 0;

    while (node) {
        padding = calc_padding_with_header(
            (uintptr_t)node, (uintptr_t)alignment, sizeof(Free_List_Header));
        size_t required_space = size + padding;
        if (node->block_size >= required_space) {
            break;
        }
        prev_node = node;
        node      = node->next;
    }
    if (padding_)
        *padding_ = padding;
    if (prev_node_)
        *prev_node_ = prev_node;
    return node;
}

static void *free_list_alloc_aligned(Free_List *fl, size_t size,
                                     size_t alignment)
{

    size_t padding            = 0;
    Free_List_Node *prev_node = NULL;
    Free_List_Node *node      = NULL;
    size_t alignment_padding, required_space, remaining;
    Free_List_Header *header_ptr;

    if (size < sizeof(Free_List_Node)) {
        size = sizeof(Free_List_Node);
    }

    if (alignment < 8) {
        alignment = 8;
    }

    size_t aligned_size = (size + alignment - 1) & ~(alignment - 1);

    node =
        free_list_find_first(fl, aligned_size, alignment, &padding, &prev_node);
    if (!node) {
        assert(0 && "Free list has no free memory");
        return NULL;
    }

    alignment_padding = padding - sizeof(Free_List_Header);
    required_space    = aligned_size + padding;
    remaining         = node->block_size - required_space;

    if (remaining > 0) {
        Free_List_Node *new_node =
            (Free_List_Node *)((char *)node + required_space);
        new_node->block_size = remaining;
        free_list_node_insert(&fl->head, node, new_node);
    }

    free_list_node_remove(&fl->head, prev_node, node);

    header_ptr = (Free_List_Header *)((char *)node + alignment_padding);
    header_ptr->block_size = required_space;
    header_ptr->padding    = alignment_padding;

    fl->used += required_space;

    void *ptr = (void *)((char *)header_ptr + sizeof(Free_List_Header));

    return memset(ptr, 0x00, aligned_size);
}

static void free_list_coalescence(Free_List *fl, Free_List_Node *prev_node,
                                  Free_List_Node *free_node);

static void free_list_free_aligned(Free_List *fl, void *ptr)
{
    Free_List_Header *header;
    Free_List_Node *free_node;
    Free_List_Node *node;
    Free_List_Node *prev_node = NULL;

    if (!ptr)
        return;

    header    = (Free_List_Header *)((char *)ptr - sizeof(Free_List_Header));
    free_node = (Free_List_Node *)header;
    free_node->block_size = header->block_size + header->padding;
    free_node->next       = NULL;

    node                  = fl->head;
    while (node) {
        if (ptr < (void *)node) {
            free_list_node_insert(&fl->head, prev_node, free_node);
            break;
        }
        prev_node = node;
        node      = node->next;
    }

    fl->used -= free_node->block_size;

    free_list_coalescence(fl, prev_node, free_node);
}

static void free_list_coalescence(Free_List *fl, Free_List_Node *prev_node,
                                  Free_List_Node *free_node)
{
    if (free_node->next != NULL &&
        (void *)((char *)free_node + free_node->block_size) ==
            free_node->next) {
        free_node->block_size += free_node->next->block_size;
        free_list_node_remove(&fl->head, free_node, free_node->next);
    }

    if (prev_node && prev_node->next != NULL &&
        (void *)((char *)prev_node + prev_node->block_size) == free_node) {
        prev_node->block_size += free_node->next->block_size;
        free_list_node_remove(&fl->head, prev_node, free_node);
    }
}

void *free_list_alloc(void *context, size_t size)
{
    return free_list_alloc_aligned(context, size, DEFAULT_ALIGNMENT);
}

void free_list_free(void *context, void *ptr)
{
    free_list_free_aligned(context, ptr);
}
