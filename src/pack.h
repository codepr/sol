/* BSD 2-Clause License
 *
 * Copyright (c) 2018, Andrea Giacomo Baldan All rights reserved.
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

#ifndef PACK_H
#define PACK_H

#include <stdio.h>
#include <stdint.h>

/*
 * Bytestring type, provides a convenient way of handling byte string data.
 * It is essentially an unsigned char pointer that track the position of the
 * last written byte and the total size of the bystestring
 */
typedef unsigned char *bstring;

/* Return the length of a bytestring */
size_t bstring_len(const bstring);

/*
 * Bytestring constructor, it creates a new bytestring from an existing and
 * nul terminated string (array of char).
 */
bstring bstring_new(const unsigned char *);

/*
 * Copy the content of a bstring returning another one with the copied
 * content till a given nr of bytes
 */
bstring bstring_copy(const unsigned char *, size_t);

/* Duplicate a bstring */
bstring bstring_dup(const bstring);

/* Bytestring constructor, it creates a new empty bytstring of a given size */
bstring bstring_empty(size_t);

/* Release memory of a bytestring effectively deleting it */
void bstring_destroy(bstring);

void htonll(uint8_t *, uint_least64_t );

uint_least64_t ntohll(const uint8_t *);

/* Reading data on const uint8_t pointer */

// bytes -> int16_t
int unpacki16(unsigned char *);

// bytes -> uint16_t
unsigned int unpacku16(unsigned char *);

// bytes -> int32_t
long int unpacki32(unsigned char *);

// bytes -> uint32_t
unsigned long int unpacku32(unsigned char *);

// bytes -> int64_t
long long int unpacki64(unsigned char *);

// bytes -> uint64_t
unsigned long long int unpacku64(unsigned char *);

/* Write data on const uint8_t pointer */
// append a uint8_t -> bytes into the bytestring
void pack_u8(uint8_t **, uint8_t);

// append a uint16_t -> bytes into the bytestring
void packi16(unsigned char *, unsigned short);

// append a int32_t -> bytes into the bytestring
void packi32(unsigned char *, unsigned int);

// append a uint64_t -> bytes into the bytestring
void packi64(unsigned char *, unsigned long long int);

/*
 * pack() -- store data dictated by the format string in the buffer
 *
 *   bits |signed   unsigned   float   string
 *   -----+----------------------------------
 *      8 |   b        B
 *     16 |   h        H         f
 *     32 |   i        I         d
 *     64 |   q        Q         g
 *      - |                               s
 *
 *  (16-bit unsigned length is automatically prepended to strings)
 */
unsigned int pack(unsigned char *, char *, ...);

/*
 * unpack() -- unpack data dictated by the format string into the buffer
 *
 *   bits |signed   unsigned   float   string
 *   -----+----------------------------------
 *      8 |   b        B
 *     16 |   h        H         f
 *     32 |   i        I         d
 *     64 |   q        Q         g
 *      - |                               s
 *
 *  (string is extracted based on its stored length, but 's' can be
 *  prepended with a max length)
 */
void unpack(unsigned char *, char *, ...);

long long unpack_integer(unsigned char **, char);

unsigned char *unpack_bytes(unsigned char **, size_t);

#endif
