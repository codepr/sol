/*
 * BSD 2-Clause License
 *
 * Copyright (c) 2019, Andrea Giacomo Baldan All rights reserved.
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

#ifndef ITERATOR_H
#define ITERATOR_H

/*
 * Generic iterator strucuture, have the following fields
 * - unsigned long index: The index of the last visited item in the iterable
 * - void *ptr: The pointer to the current visited item
 * - void *iterable: The iterable data structure to iterate on
 * - void (*next)(struct iterator *): Function pointer used to update the ptr
 *   value, it has to be defined for each different data structure
 */
struct iterator {
    unsigned long index;
    void *ptr;
    void *iterable;
    void (*next)(struct iterator *);
};


struct iterator *iter_get(void *, void (*next)(struct iterator *));
struct iterator *iter_next(struct iterator *);
void iter_destroy(struct iterator *);


#endif
