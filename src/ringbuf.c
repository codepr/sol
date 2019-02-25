/* BSD 2-Clause License
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

#include <assert.h>
#include <stdlib.h>
#include "pack.h"
#include "ringbuf.h"
#include "util.h"


struct ringbuf {
    unsigned char *buffer;
    size_t head, tail, max;
    unsigned full : 1;
};


Ringbuffer *ringbuf_create(unsigned char *buffer, size_t size) {

    assert(buffer && size);

    Ringbuffer *rbuf = sol_malloc(sizeof(Ringbuffer));
    assert(rbuf);

    rbuf->buffer = buffer;
    rbuf->max = size;
    ringbuf_reset(rbuf);

    assert(ringbuf_empty(rbuf));

    return rbuf;
}


void ringbuf_init(Ringbuffer *rbuf, unsigned char *buffer, size_t size) {
    rbuf->buffer = buffer;
    rbuf->max = size;
    ringbuf_reset(rbuf);
}


void ringbuf_reset(Ringbuffer *rbuf) {

    assert(rbuf);

    rbuf->head = 0;
    rbuf->tail = 0;
    rbuf->full = 0;
}


void ringbuf_release(Ringbuffer *rbuf) {
    assert(rbuf);
    sol_free(rbuf);
    rbuf = NULL;
}


int ringbuf_full(const Ringbuffer *rbuf) {
    assert(rbuf);
    return rbuf->full;
}


int ringbuf_empty(const Ringbuffer *rbuf) {
    assert(rbuf);
    return (!rbuf->full && (rbuf->head == rbuf->tail));
}


size_t ringbuf_capacity(const Ringbuffer *rbuf) {
    assert(rbuf);
    return rbuf->max;
}


size_t ringbuf_size(const Ringbuffer *rbuf) {

    assert(rbuf);

    size_t size = rbuf->max;

    if (!rbuf->full) {
        if (rbuf->head >= rbuf->tail) {
            size = (rbuf->head - rbuf->tail);
        } else {
            size = (rbuf->max + rbuf->head - rbuf->tail);
        }
    }

    return size;
}


static void advance_pointer(Ringbuffer *rbuf) {

    assert(rbuf);

    if (rbuf->full)
        rbuf->tail = (rbuf->tail + 1) % rbuf->max;

    rbuf->head = (rbuf->head + 1) % rbuf->max;
    rbuf->full = (rbuf->head == rbuf->tail);
}


static void retreat_pointer(Ringbuffer *rbuf) {

    assert(rbuf);

    rbuf->full = 0;
    rbuf->tail = (rbuf->tail + 1) % rbuf->max;
}


int ringbuf_push(Ringbuffer *rbuf, unsigned char dest) {

    int r = -1;

    assert(rbuf && rbuf->buffer);

    if (!ringbuf_full(rbuf)) {
        rbuf->buffer[rbuf->head] = dest;
        advance_pointer(rbuf);
        r = 0;
    }

    return r;
}


int ringbuf_bulk_push(Ringbuffer *rbuf, unsigned char *dest, size_t size) {

    assert(rbuf && dest && rbuf->buffer);

    int r = 0;

    for (unsigned long long i = 0; i < size; ++i) {
        r = ringbuf_push(rbuf, dest[i]);
        if (r == -1) break;
    }

    return r;
}


int ringbuf_pop(Ringbuffer *rbuf, unsigned char *dest) {

    assert(rbuf && dest && rbuf->buffer);

    int r = -1;

    if (!ringbuf_empty(rbuf)) {
        *dest = rbuf->buffer[rbuf->tail];
        retreat_pointer(rbuf);

        r = 0;
    }

    return r;
}


int ringbuf_bulk_pop(Ringbuffer *rbuf, unsigned char *dest, size_t size) {

    assert(rbuf && dest && rbuf->buffer);

    int r = 0;

    while (ringbuf_size(rbuf) > 0 && size > 0) {
        r = ringbuf_pop(rbuf, dest++);
        size--;
        if (r == -1) break;
    }

    return r;
}


int ringbuf_peek(const Ringbuffer *rbuf, unsigned char *dest) {

    assert(rbuf && dest);

    if (ringbuf_empty(rbuf))
        return -1;

    *dest = rbuf->buffer[rbuf->tail];

    return 0;
}


int ringbuf_peek_head(const Ringbuffer *rbuf, unsigned char *dest) {

    assert(rbuf && dest);

    if (ringbuf_empty(rbuf))
        return -1;

    *dest = rbuf->buffer[rbuf->head-1];

    return 0;
}


unsigned char *ringbuf_buffer(const Ringbuffer *rbuf) {

    assert(rbuf);

    return rbuf->buffer;
}


int ringbuf_dump(Ringbuffer *rbuf, unsigned char *buf) {

    assert(rbuf && buf);

    ringbuf_bulk_pop(rbuf, buf, ringbuf_size(rbuf));

    return 0;
}
