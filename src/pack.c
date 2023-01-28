/* BSD 2-Clause License
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

#include <ctype.h>
#include <string.h>
#include <stdarg.h>
#include "pack.h"
#include "util.h"
#include "memory.h"

/* Host-to-network (native endian to big endian) */
void htonll(u8 *block, u64 num) {
    block[0] = num >> 56 & 0xFF;
    block[1] = num >> 48 & 0xFF;
    block[2] = num >> 40 & 0xFF;
    block[3] = num >> 32 & 0xFF;
    block[4] = num >> 24 & 0xFF;
    block[5] = num >> 16 & 0xFF;
    block[6] = num >> 8 & 0xFF;
    block[7] = num >> 0 & 0xFF;
}

/* Network-to-host (big endian to native endian) */
u64 ntohll(const u8 *block) {
    return (u64) block[0] << 56 | (u64) block[1] << 48
        | (u64) block[2] << 40 | (u64) block[3] << 32
        | (u64) block[4] << 24 | (u64) block[5] << 16
        | (u64) block[6] << 8 | (u64) block[7] << 0;
}

// Beej'us network guide functions

/*
** packi16() -- store a 16-bit int into a char buffer (like htons())
*/
void packi16(u8 *buf, u16 val) {
    *buf++ = val >> 8;
    *buf++ = val;
}

/*
** packi32() -- store a 32-bit int into a char buffer (like htonl())
*/
void packi32(u8 *buf, u32 val) {
    *buf++ = val >> 24;
    *buf++ = val >> 16;
    *buf++ = val >> 8;
    *buf++ = val;
}

/*
** packi64() -- store a 64-bit int into a char buffer (like htonl())
*/
void packi64(u8 *buf, u64 val) {
    *buf++ = val >> 56; *buf++ = val >> 48;
    *buf++ = val >> 40; *buf++ = val >> 32;
    *buf++ = val >> 24; *buf++ = val >> 16;
    *buf++ = val >> 8;  *buf++ = val;
}

/*
** unpacki16() -- unpack a 16-bit int from a char buffer (like ntohs())
*/
i16 unpacki16(u8 *buf) {
    u16 i2 = ((u16) buf[0] << 8) | buf[1];
    i16 val;

    // change unsigned numbers to signed
    if (i2 <= 0x7fffu)
        val = i2;
    else
        val = -1 - (u16) (0xffffu - i2);

    return val;
}

/*
** unpacku16() -- unpack a 16-bit unsigned from a char buffer (like ntohs())
*/
u16 unpacku16(u8 *buf) {
    return ((u16) buf[0] << 8) | buf[1];
}

/*
** unpacki32() -- unpack a 32-bit int from a char buffer (like ntohl())
*/
i32 unpacki32(u8 *buf) {
    u32 i2 = ((i32) buf[0] << 24) |
        ((i32) buf[1] << 16) |
        ((i32) buf[2] << 8)  |
        buf[3];
    i32 val;

    // change unsigned numbers to signed
    if (i2 <= 0x7fffffffu)
        val = i2;
    else
        val = -1 - (i32) (0xffffffffu - i2);

    return val;
}

/*
** unpacku32() -- unpack a 32-bit unsigned from a char buffer (like ntohl())
*/
u32 unpacku32(u8 *buf) {
    return ((u32) buf[0] << 24) |
        ((u32) buf[1] << 16) |
        ((u32) buf[2] << 8)  |
        buf[3];
}

/*
** unpacki64() -- unpack a 64-bit int from a char buffer (like ntohl())
*/
i64 unpacki64(u8 *buf) {
    u64 i2 = ((u64) buf[0] << 56) |
        ((u64) buf[1] << 48) |
        ((u64) buf[2] << 40) |
        ((u64) buf[3] << 32) |
        ((u64) buf[4] << 24) |
        ((u64) buf[5] << 16) |
        ((u64) buf[6] << 8)  |
        buf[7];
    i64 val;

    // change unsigned numbers to signed
    if (i2 <= 0x7fffffffffffffffu)
        val = i2;
    else
        val = -1 -(i64) (0xffffffffffffffffu - i2);

    return val;
}

/*
** unpacku64() -- unpack a 64-bit unsigned from a char buffer (like ntohl())
*/
u64 unpacku64(u8 *buf) {
    return ((u64) buf[0] << 56) |
        ((u64) buf[1] << 48) |
        ((u64) buf[2] << 40) |
        ((u64) buf[3] << 32) |
        ((u64) buf[4] << 24) |
        ((u64) buf[5] << 16) |
        ((u64) buf[6] << 8)  |
        buf[7];
}

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
usize pack(u8 *buf, char *format, ...) {
    va_list ap;

    i8 b;       // 8-bit
    u8 B;

    i16 h;      // 16-bit
    u16 H;

    i32 i;      // 32-bit
    u32 I;

    i64 q;      // 64-bit
    u64 Q;

    char *s;    // strings
    usize len, size = 0;

    va_start(ap, format);

    for(; *format != '\0'; format++) {
        switch(*format) {
            case 'b': // 8-bit
                size += 1;
                b = (i8) va_arg(ap, i32); // promoted
                *buf++ = b;
                break;

            case 'B': // 8-bit unsigned
                size += 1;
                B = (u8) va_arg(ap, u32); // promoted
                *buf++ = B;
                break;

            case 'h': // 16-bit
                size += 2;
                h = va_arg(ap, i32);
                packi16(buf, h);
                buf += 2;
                break;

            case 'H': // 16-bit unsigned
                size += 2;
                H = va_arg(ap, u32);
                packi16(buf, H);
                buf += 2;
                break;

            case 'i': // 32-bit
                size += 4;
                i = va_arg(ap, i32);
                packi32(buf, i);
                buf += 4;
                break;

            case 'I': // 32-bit unsigned
                size += 4;
                I = va_arg(ap, u32);
                packi32(buf, I);
                buf += 4;
                break;

            case 'q': // 64-bit
                size += 8;
                q = va_arg(ap, i64);
                packi64(buf, q);
                buf += 8;
                break;

            case 'Q': // 64-bit unsigned
                size += 8;
                Q = va_arg(ap, u64);
                packi64(buf, Q);
                buf += 8;
                break;

            case 's': // string
                s = va_arg(ap, char *);
                len = strlen(s);
                memcpy(buf, s, len);
                buf += len;
                break;
        }
    }

    va_end(ap);

    return size;
}

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
usize unpack(u8 *buf, char *format, ...) {
    va_list ap;

    i8 *b;      // 8-bit
    u8 *B;

    i16 *h;     // 16-bit
    u16 *H;

    i32 *i;     // 32-bit
    u32 *I;

    i64 *q;     // 64-bit
    u64 *Q;

    char *s;    // strings
    usize maxstrlen = 0, size = 0;

    va_start(ap, format);

    for(; *format != '\0'; format++) {
        switch(*format) {
            case 'b': // 8-bit
                b = va_arg(ap, i8*);
                if (*buf <= 0x7f)
                    *b = *buf; // re-sign
                else
                    *b = -1 - (u8) (0xffu - *buf);
                buf++;
                size += 1;
                break;

            case 'B': // 8-bit unsigned
                B = va_arg(ap, u8*);
                *B = *buf++;
                size += 1;
                break;

            case 'h': // 16-bit
                h = va_arg(ap, i16*);
                *h = unpacki16(buf);
                buf += 2;
                size += 2;
                break;

            case 'H': // 16-bit unsigned
                H = va_arg(ap, u16*);
                *H = unpacku16(buf);
                buf += 2;
                size += 2;
                break;

            case 'i': // 32-bit
                i = va_arg(ap, i32*);
                *i = unpacki32(buf);
                buf += 4;
                size += 4;
                break;

            case 'I': // 32-bit unsigned
                I = va_arg(ap, u32*);
                *I = unpacku32(buf);
                buf += 4;
                size += 4;
                break;

            case 'q': // 64-bit
                q = va_arg(ap, i64*);
                *q = unpacki64(buf);
                buf += 8;
                size += 8;
                break;

            case 'Q': // 64-bit unsigned
                Q = va_arg(ap, u64*);
                *Q = unpacku64(buf);
                buf += 8;
                size += 8;
                break;

            case 's': // string
                s = va_arg(ap, char*);
                memcpy(s, buf, maxstrlen);
                s[maxstrlen] = '\0';
                buf += maxstrlen;
                size += maxstrlen;
                break;

            default:
                if (isdigit(*format))  // track max str len
                    maxstrlen = maxstrlen * 10 + (*format-'0');
        }

        if (!isdigit(*format))
            maxstrlen = 0;
    }

    va_end(ap);

    return size;
}

/* Helper functions */
i64 unpack_integer(u8 **buf, i8 size) {
    i64 val = 0LL;
    switch (size) {
        case 'b':
            val = **buf;
            *buf += 1;
            break;
        case 'B':
            val = **buf;
            *buf += 1;
            break;
        case 'h':
            val = unpacki16(*buf);
            *buf += 2;
            break;
        case 'H':
            val = unpacku16(*buf);
            *buf += 2;
            break;
        case 'i':
            val = unpacki32(*buf);
            *buf += 4;
            break;
        case 'I':
            val = unpacku32(*buf);
            *buf += 4;
            break;
        case 'q':
            val = unpacki64(*buf);
            *buf += 8;
            break;
        case 'Q':
            val = unpacku64(*buf);
            *buf += 8;
            break;
    }
    return val;
}

u8 *unpack_bytes(u8 **buf, usize len) {
    u8 *dest = try_alloc(len + 1);
    memcpy(dest, *buf, len);
    dest[len] = '\0';
    *buf += len;
    return dest;
}

u16 unpack_string16(u8 **buf, u8 **dest) {
    u16 len = unpack_integer(buf, 'H');
    *dest = unpack_bytes(buf, len);
    return len;
}
