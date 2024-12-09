/* BSD 2-Clause License
 *
 * Copyright (c) 2023, Andrea Giacomo Baldan All rights reserved.
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

#include "memory.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

static atomic_size_t memory = ATOMIC_VAR_INIT(0);

/*
 * Custom malloc function, allocate a defined size of bytes plus
 * sizeof(size_t), the size of an unsigned long long, and append the length
 * choosen at the beginning of the memory chunk as an unsigned long long,
 * returning the memory chunk allocated just sizeof(size_t) bytes after the
 * start; this way it is possible to track the memory usage at every
 * allocation.
 *
 * This function can fail if not memory is available, interrupting the
 * execution of the program and exiting, hence the prefix "try".
 */
void *try_alloc(size_t size)
{
    void *ptr = malloc(size + sizeof(size_t));
    if (!ptr && size != 0) {
        fprintf(stderr, "[%s:%ul] Out of memory (%lu bytes)\n", __FILE__,
                __LINE__, size);
        exit(EXIT_FAILURE);
    }
    memory += size + sizeof(size_t);
    *((size_t *)ptr) = size;
    return (char *)ptr + sizeof(size_t);
}

/*
 * Same as xmalloc, but with calloc, creating chunk o zero'ed memory.
 * TODO: still a suboptimal solution
 *
 * This function can fail if not memory is available, interrupting the
 * execution of the program and exiting, hence the prefix "try".
 */
void *try_calloc(size_t len, size_t size)
{
    void *ptr = try_alloc(len * size);
    memset(ptr, 0x00, len * size);
    return ptr;
}

/*
 * Same of xmalloc but with realloc, resize a chunk of memory pointed by a
 * given pointer, again appends the new size in front of the byte array
 *
 * This function can fail if not memory is available, interrupting the
 * execution of the program and exiting, hence the prefix "try".
 */
void *try_realloc(void *ptr, size_t size)
{
    if (!ptr)
        return try_alloc(size);

    void *realptr    = (char *)ptr - sizeof(size_t);
    size_t curr_size = *((size_t *)realptr);
    if (size == curr_size)
        return ptr;

    void *newptr = realloc(realptr, size + sizeof(size_t));
    if (!newptr && size != 0) {
        fprintf(stderr, "[%s:%ul] Out of memory (%lu bytes)\n", __FILE__,
                __LINE__, size);
        exit(EXIT_FAILURE);
    }

    *((size_t *)newptr) = size;
    memory += (-curr_size) + size + sizeof(size_t);
    return (char *)newptr + sizeof(size_t);
}

/*
 * Custom free function, must be used on memory chunks allocated with t*
 * functions, it move the pointer 8 position backward by the starting address
 * of memory pointed by `ptr`, this way it knows how many bytes will be
 * free'ed by the call
 */
void free_memory(void *ptr)
{
    if (!ptr)
        return;

    void *realptr = (char *)ptr - sizeof(size_t);
    if (!realptr)
        return;

    size_t ptr_size = *((size_t *)realptr);
    memory -= ptr_size + sizeof(size_t);
    free(realptr);
}

/*
 * Retrieve the bytes allocated by t* functions by backwarding the pointer of
 * 8 positions, the size of an unsigned long long in order to read the number
 * of allcated bytes
 */
size_t alloc_size(void *ptr)
{
    if (!ptr)
        return 0L;

    void *realptr = (char *)ptr - sizeof(size_t);
    if (!realptr)
        return 0L;

    size_t ptr_size = *((size_t *)realptr);
    return ptr_size;
}

/*
 * As strdup but using xmalloc instead of malloc, to track the number of bytes
 * allocated and to enable use of xfree on duplicated strings without having
 * to care when to use a normal free or a xfree
 *
 * This function can fail if not memory is available, interrupting the
 * execution of the program and exiting, hence the prefix "try".
 */
char *try_strdup(const char *s)
{
    size_t len = strlen(s);
    char *ds   = try_alloc(len + 1);
    snprintf(ds, len + 1, "%s", s);
    return ds;
}

size_t memory_used(void) { return memory; }
