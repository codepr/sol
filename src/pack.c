/* BSD 2-Clause License
 *
 * Copyright (c) 2018, 2019 Andrea Giacomo Baldan All rights reserved.
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
#include <arpa/inet.h>
#include "pack.h"
#include "util.h"

/*
 * Return the length of the string without having to call strlen, thus this
 * works also with non-nul terminated string. The length of the string is in
 * fact stored in memory in an unsigned long just before the position of the
 * string itself.
 */
size_t bstring_len(const bstring s) {
    return *((size_t *) (s - sizeof(size_t)));
}

bstring bstring_new(const unsigned char *init) {
    if (!init)
        return NULL;
    size_t len = strlen((const char *) init);
    return bstring_copy(init, len);
}

bstring bstring_copy(const unsigned char *init, size_t len) {
    /*
     * The strategy would be to piggyback the real string to its stored length
     * in memory, having already implemented this logic before to actually
     * track memory usage of the system, we just need to malloc it with the
     * custom malloc in utils
     */
    unsigned char *str = xmalloc(len);
    memcpy(str, init, len);
    return str;
}

bstring bstring_dup(const bstring src) {
    size_t len = bstring_len(src);
    return bstring_copy(src, len);
}

/* Same as bstring_copy but setting the entire content of the string to 0 */
bstring bstring_empty(size_t len) {
    unsigned char *str = xcalloc(len, sizeof(unsigned char));
    return str;
}

void bstring_destroy(bstring s) {
    /*
     * Being allocated with utils custom functions just free it with the
     * corrispective free function
     */
    xfree(s);
}

/* Host-to-network (native endian to big endian) */
void htonll(uint8_t *block, uint_least64_t num) {
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
uint_least64_t ntohll(const uint8_t *block) {
    return (uint_least64_t) block[0] << 56 | (uint_least64_t) block[1] << 48
        | (uint_least64_t) block[2] << 40 | (uint_least64_t) block[3] << 32
        | (uint_least64_t) block[4] << 24 | (uint_least64_t) block[5] << 16
        | (uint_least64_t) block[6] << 8 | (uint_least64_t) block[7] << 0;
}

// Beej'us network guide functions

/*
** packi16() -- store a 16-bit int into a char buffer (like htons())
*/
void packi16(unsigned char *buf, unsigned short val) {
    *buf++ = val >> 8;
    *buf++ = val;
}

/*
** packi32() -- store a 32-bit int into a char buffer (like htonl())
*/
void packi32(unsigned char *buf, unsigned int val) {
    *buf++ = val >> 24;
    *buf++ = val >> 16;
    *buf++ = val >> 8;
    *buf++ = val;
}

/*
** packi64() -- store a 64-bit int into a char buffer (like htonl())
*/
void packi64(unsigned char *buf, unsigned long long int val) {
    *buf++ = val >> 56; *buf++ = val >> 48;
    *buf++ = val >> 40; *buf++ = val >> 32;
    *buf++ = val >> 24; *buf++ = val >> 16;
    *buf++ = val >> 8;  *buf++ = val;
}

/*
** unpacki16() -- unpack a 16-bit int from a char buffer (like ntohs())
*/
int unpacki16(unsigned char *buf) {
    unsigned int i2 = ((unsigned int) buf[0] << 8) | buf[1];
    int val;

    // change unsigned numbers to signed
    if (i2 <= 0x7fffu)
        val = i2;
    else
        val = -1 - (unsigned int) (0xffffu - i2);

    return val;
}

/*
** unpacku16() -- unpack a 16-bit unsigned from a char buffer (like ntohs())
*/
unsigned int unpacku16(unsigned char *buf) {
    return ((unsigned int) buf[0] << 8) | buf[1];
}

/*
** unpacki32() -- unpack a 32-bit int from a char buffer (like ntohl())
*/
long int unpacki32(unsigned char *buf) {
    unsigned long int i2 = ((unsigned long int) buf[0] << 24) |
                           ((unsigned long int) buf[1] << 16) |
                           ((unsigned long int) buf[2] << 8)  |
                           buf[3];
    long int val;

    // change unsigned numbers to signed
    if (i2 <= 0x7fffffffu)
        val = i2;
    else
        val = -1 - (long int) (0xffffffffu - i2);

    return val;
}

/*
** unpacku32() -- unpack a 32-bit unsigned from a char buffer (like ntohl())
*/
unsigned long int unpacku32(unsigned char *buf) {
    return ((unsigned long int) buf[0] << 24) |
           ((unsigned long int) buf[1] << 16) |
           ((unsigned long int) buf[2] << 8)  |
           buf[3];
}

/*
** unpacki64() -- unpack a 64-bit int from a char buffer (like ntohl())
*/
long long int unpacki64(unsigned char *buf) {
    unsigned long long int i2 = ((unsigned long long int) buf[0] << 56) |
                                ((unsigned long long int) buf[1] << 48) |
                                ((unsigned long long int) buf[2] << 40) |
                                ((unsigned long long int) buf[3] << 32) |
                                ((unsigned long long int) buf[4] << 24) |
                                ((unsigned long long int) buf[5] << 16) |
                                ((unsigned long long int) buf[6] << 8)  |
                                buf[7];
    long long int val;

    // change unsigned numbers to signed
    if (i2 <= 0x7fffffffffffffffu)
        val = i2;
    else
        val = -1 -(long long int) (0xffffffffffffffffu - i2);

    return val;
}

/*
** unpacku64() -- unpack a 64-bit unsigned from a char buffer (like ntohl())
*/
unsigned long long int unpacku64(unsigned char *buf) {
    return ((unsigned long long int) buf[0] << 56) |
           ((unsigned long long int) buf[1] << 48) |
           ((unsigned long long int) buf[2] << 40) |
           ((unsigned long long int) buf[3] << 32) |
           ((unsigned long long int) buf[4] << 24) |
           ((unsigned long long int) buf[5] << 16) |
           ((unsigned long long int) buf[6] << 8)  |
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
unsigned int pack(unsigned char *buf, char *format, ...) {
    va_list ap;

    signed char b;              // 8-bit
    unsigned char B;

    int h;                      // 16-bit
    unsigned int H;

    long int i;                 // 32-bit
    unsigned long int I;

    long long int q;            // 64-bit
    unsigned long long int Q;

    char *s;                    // strings
    unsigned int len;

    unsigned int size = 0;

    va_start(ap, format);

    for(; *format != '\0'; format++) {
        switch(*format) {
            case 'b': // 8-bit
                size += 1;
                b = (signed char) va_arg(ap, int); // promoted
                *buf++ = b;
                break;

            case 'B': // 8-bit unsigned
                size += 1;
                B = (unsigned char) va_arg(ap, unsigned int); // promoted
                *buf++ = B;
                break;

            case 'h': // 16-bit
                size += 2;
                h = va_arg(ap, int);
                packi16(buf, h);
                buf += 2;
                break;

            case 'H': // 16-bit unsigned
                size += 2;
                H = va_arg(ap, unsigned int);
                packi16(buf, H);
                buf += 2;
                break;

            case 'i': // 32-bit
                size += 4;
                i = va_arg(ap, long int);
                packi32(buf, i);
                buf += 4;
                break;

            case 'I': // 32-bit unsigned
                size += 4;
                I = va_arg(ap, unsigned long int);
                packi32(buf, I);
                buf += 4;
                break;

            case 'q': // 64-bit
                size += 8;
                q = va_arg(ap, long long int);
                packi64(buf, q);
                buf += 8;
                break;

            case 'Q': // 64-bit unsigned
                size += 8;
                Q = va_arg(ap, unsigned long long int);
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
void unpack(unsigned char *buf, char *format, ...) {
    va_list ap;

    signed char *b;              // 8-bit
    unsigned char *B;

    int *h;                      // 16-bit
    unsigned int *H;

    long int *i;                 // 32-bit
    unsigned long int *I;

    long long int *q;            // 64-bit
    unsigned long long int *Q;

    char *s;
    unsigned int maxstrlen = 0;

    va_start(ap, format);

    for(; *format != '\0'; format++) {
        switch(*format) {
            case 'b': // 8-bit
                b = va_arg(ap, signed char*);
                if (*buf <= 0x7f)
                    *b = *buf; // re-sign
                else
                    *b = -1 - (unsigned char) (0xffu - *buf);
                buf++;
                break;

            case 'B': // 8-bit unsigned
                B = va_arg(ap, unsigned char*);
                *B = *buf++;
                break;

            case 'h': // 16-bit
                h = va_arg(ap, int*);
                *h = unpacki16(buf);
                buf += 2;
                break;

            case 'H': // 16-bit unsigned
                H = va_arg(ap, unsigned int*);
                *H = unpacku16(buf);
                buf += 2;
                break;

            case 'i': // 32-bit
                i = va_arg(ap, long int*);
                *i = unpacki32(buf);
                buf += 4;
                break;

            case 'I': // 32-bit unsigned
                I = va_arg(ap, unsigned long int*);
                *I = unpacku32(buf);
                buf += 4;
                break;

            case 'q': // 64-bit
                q = va_arg(ap, long long int*);
                *q = unpacki64(buf);
                buf += 8;
                break;

            case 'Q': // 64-bit unsigned
                Q = va_arg(ap, unsigned long long int*);
                *Q = unpacku64(buf);
                buf += 8;
                break;

            case 's': // string
                s = va_arg(ap, char*);
                memcpy(s, buf, maxstrlen);
                s[maxstrlen] = '\0';
                buf += maxstrlen;
                break;

            default:
                if (isdigit(*format))  // track max str len
                    maxstrlen = maxstrlen * 10 + (*format-'0');
        }

        if (!isdigit(*format))
            maxstrlen = 0;
    }

    va_end(ap);
}

/* Helper functions */
long long unpack_integer(unsigned char **buf, char size) {
    long long val = 0LL;
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

unsigned char *unpack_bytes(unsigned char **buf, size_t len) {
    unsigned char *dest = xmalloc(len + 1);
    memcpy(dest, *buf, len);
    dest[len] = '\0';
    *buf += len;
    return dest;
}
