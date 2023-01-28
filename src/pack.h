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

#ifndef PACK_H
#define PACK_H

#include "types.h"

void htonll(u8 *, u64);

u64 ntohll(const u8 *);

/* Reading data on const u8 pointer */

// bytes -> int16_t
i16 unpacki16(u8 *);

// bytes -> uint16_t
u16 unpacku16(u8 *);

// bytes -> int32_t
i32 unpacki32(u8 *);

// bytes -> uint32_t
u32 unpacku32(u8 *);

// bytes -> int64_t
i64 unpacki64(u8 *);

// bytes -> uint64_t
u64 unpacku64(u8 *);

/* Write data on const u8 pointer */
// append a u8 -> bytes into the bytestring
void pack_u8(u8 **, u8);

// append a uint16_t -> bytes into the bytestring
void packi16(u8 *, u16);

// append a int32_t -> bytes into the bytestring
void packi32(u8 *, u32);

// append a uint64_t -> bytes into the bytestring
void packi64(u8 *, u64);

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
usize pack(u8 *, char *, ...);

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
usize unpack(u8 *, char *, ...);

i64 unpack_integer(u8 **, i8);

u8 *unpack_bytes(u8 **, usize);

u16 unpack_string16(u8 **, u8 **);

#endif
