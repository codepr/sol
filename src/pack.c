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

#include <string.h>
#include <arpa/inet.h>
#include "pack.h"
#include "util.h"


struct bytestring *bytestring_create(size_t len) {
    struct bytestring *bstring = sol_malloc(sizeof(*bstring));
    bytestring_init(bstring, len);
    return bstring;
}


void bytestring_init(struct bytestring *bstring, size_t size) {
    if (!bstring)
        return;
    bstring->size = size;
    bstring->data = sol_malloc(sizeof(unsigned char) * size);
    bytestring_reset(bstring);
}


void bytestring_release(struct bytestring *bstring) {
    if (!bstring)
        return;
    sol_free(bstring->data);
    sol_free(bstring);
}


void bytestring_reset(struct bytestring *bstring) {
    if (!bstring)
        return;
    bstring->last = 0;
    memset(bstring->data, 0, bstring->size);
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

// Reading data
uint8_t unpack_u8(const uint8_t **buf) {
    uint8_t val = **buf;
    (*buf)++;
    return val;
}


uint16_t unpack_u16(const uint8_t **buf) {
    uint16_t val;
    memcpy(&val, *buf, sizeof(uint16_t));
    (*buf) += sizeof(uint16_t);
    return ntohs(val);
}


uint32_t unpack_u32(const uint8_t **buf) {
    uint32_t val;
    memcpy(&val, *buf, sizeof(uint32_t));
    (*buf) += sizeof(uint32_t);
    return ntohl(val);
}


uint64_t unpack_u64(const uint8_t **buf) {
    uint64_t val = ntohll(*buf);
    (*buf) += sizeof(uint64_t);
    return val;
}


uint8_t *unpack_bytes(const uint8_t **buf, size_t len, uint8_t *str) {

    memcpy(str, *buf, len);
    str[len] = '\0';
    (*buf) += len;

    return str;
}

// Write data
void pack_u8(uint8_t **buf, uint8_t val) {
    **buf = val;
    (*buf) += sizeof(uint8_t);
}


void pack_u16(uint8_t **buf, uint16_t val) {
    uint16_t htonsval = htons(val);
    memcpy(*buf, &htonsval, sizeof(uint16_t));
    (*buf) += sizeof(uint16_t);
}


void pack_u32(uint8_t **buf, uint32_t val) {
    uint32_t htonlval = htonl(val);
    memcpy(*buf, &htonlval, sizeof(uint32_t));
    (*buf) += sizeof(uint32_t);
}


void pack_u64(uint8_t **buf, uint64_t val) {
    htonll(*buf, val);
    (*buf) += sizeof(uint64_t);
}


void pack_bytes(uint8_t **buf, uint8_t *str) {

    size_t len = strlen((char *) str);

    memcpy(*buf, str, len);
    (*buf) += len;
}
