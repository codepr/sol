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

#include <string.h>
#include "util.h"
#include "memorypool.h"

static void memorypool_resize(struct memorypool *);

struct memorypool *memorypool_new(size_t blocks_nr, size_t blocksize) {
    struct memorypool *pool = xmalloc(sizeof(*pool));
    if (!pool)
        return NULL;
    blocksize = blocksize >= sizeof(uintptr_t) ? blocksize : sizeof(uintptr_t);
    pool->memory = xcalloc(blocks_nr, blocksize);
    pool->free = pool->memory;
    pool->blocks_nr = blocks_nr;
    pool->blocksize = blocksize;
    if (!pool->free) {
        xfree(pool);
        return NULL;
    }
    /*
     * We pre-assign the position of each free block in the free pointer, this
     * way we know every block position before allocating new memory, we'll
     * call it the header of each block:
     *
     *       ____________
     *     _| 0x1ad45f02 |
     *    | |------------|
     *    | |     .      |
     *    | |     .      |
     *    |_|------------|
     *     _| 0x2ff43da1 |
     *    | |------------|
     *    | |     .      |
     *    | |     .      |
     *    |_|------------|
     *      | 0x98fff34a |
     *      |------------|
     *      |     .      |
     *
     * Just before assigning a free block of memory, we update the free pointer,
     * pointing it to the memory address previously stored as r-value in it.
     * This way everytime we allocate a new block we can refresh the next free
     * block in the list.
     */
    intptr_t *ptr = pool->free;
    for (size_t i = 1; i < blocks_nr; ++i) {
        *ptr = (intptr_t)((char *) pool->free + blocksize * i);
        ptr = (intptr_t *)((char *) pool->free + blocksize * i);
    }
    pool->block_used = 0;
    return pool;
}

void memorypool_destroy(struct memorypool *pool) {
    xfree(pool->memory);
    xfree(pool);
}

void *memorypool_alloc(struct memorypool *pool) {
    void *ptr = pool->free;
    /*
     * After pointing the return pointer to the next free block, we need to
     * update the next free block address on the free pointer. The address is
     * already stored in the "header" of the block.
     */
    if ((intptr_t *)(*((intptr_t *) pool->free)) == NULL)
        memorypool_resize(pool);
    pool->free = (intptr_t *)(*((intptr_t *) pool->free));
    pool->block_used++;
    printf("Alloc %d\n", pool->block_used);
    return ptr;
}

void memorypool_free(struct memorypool *pool, void *ptr) {
    /*
     * Here we just need to point the header of the pointer to the next free
     * location and udpate the current free location by pointing it to the
     * free'd pointer
     */
    /* if ((intptr_t *)(*((intptr_t *) pool->free)) == NULL) */
    if (((intptr_t *) pool->free) == NULL)
        memorypool_resize(pool);
    *((intptr_t *) ptr) = *((intptr_t *) pool->free);
    pool->free = ptr;
    pool->block_used--;
    printf("Free %d\n", pool->block_used);
}

/* static void memorypool_resize(struct memorypool *pool) { */
/*     size_t newsize = (pool->blocks_nr * pool->blocksize) * 2; */
/*     size_t newblocks_nr = pool->blocks_nr * 2; */
/*     pool->memory = xrealloc(pool->memory, newsize); */
/*     #<{(| memset((char *) pool->memory + (pool->blocks_nr * pool->blocksize), 0x00, |)}># */
/*     #<{(|        pool->blocks_nr * pool->blocksize); |)}># */
/*     #<{(| */
/*      * Apply the same logic of the init, but starting from the updated offset, */
/*      * the ald size of the pool */
/*      |)}># */
/*     intptr_t *ptr = (intptr_t *) pool->free; */
/*     for (size_t i = 1; i < pool->blocks_nr; ++i) { */
/*         printf("Resizin\n"); */
/*         *ptr = (intptr_t)((char *) ptr + pool->blocksize); */
/*         ptr = (intptr_t *)((char *) ptr + pool->blocksize); */
/*     } */
/*     pool->blocks_nr = newblocks_nr; */
/* } */
