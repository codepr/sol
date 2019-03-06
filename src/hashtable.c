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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include "util.h"
#include "hashtable.h"

/* Hashtable definition */
struct hashtable {
    size_t table_size;
    size_t size;
    int (*destructor)(struct hashtable_entry *);
    struct hashtable_entry *entries;
};


const int INITIAL_SIZE = 4;

const int MAX_CHAIN_LENGTH = 8;

const unsigned long KNUTH_PRIME = 2654435761;

static unsigned long crc32(const uint8_t *, unsigned int);

/*
 * Hashing function for a string
 */
static uint64_t hashtable_hash_int(HashTable *m, const uint8_t *keystr) {

    assert(m && keystr);

    uint64_t key = crc32(keystr, strlen((const char *) keystr));

    /* Robert Jenkins' 32 bit Mix Function */
    key += (key << 12);
    key ^= (key >> 22);
    key += (key << 4);
    key ^= (key >> 9);
    key += (key << 10);
    key ^= (key >> 2);
    key += (key << 7);
    key ^= (key >> 12);

    /* Knuth's Multiplicative Method */
    key = (key >> 3) * KNUTH_PRIME;

    return key % m->table_size;
}

/*
 * Return the integer of the location in entries to store the point to the item
 * or -HASHTABLE_FULL.
 */
static int hashtable_hash(HashTable *table, const uint8_t *key) {

    assert(table && key);

    /* If full, return immediately */
    if (table->size >= (table->table_size / 2))
        return -HASHTABLE_FULL;

    /* Find the best index */
    uint64_t curr = hashtable_hash_int(table, key);
    char *k, *currk;

    /* Linear probing */
    for (int i = 0; i < MAX_CHAIN_LENGTH; i++) {

        if (table->entries[curr].taken == false)
            return curr;

        k = (char *) table->entries[curr].key;
        currk = (char *) key;

        if (table->entries[curr].taken == true &&
            STREQ(k, currk, strlen(k)) == true)
            return curr;

        curr = (curr + 1) % table->table_size;
    }

    return -HASHTABLE_FULL;
}

/*
 * Doubles the size of the hashtable, and rehashes all the elements
 */
static int hashtable_rehash(HashTable *table) {

    assert(table);

    size_t old_size;
    struct hashtable_entry *curr;

    /* Setup the new elements */
    struct hashtable_entry *temp =
        sol_calloc(2 * table->table_size, sizeof(*temp));

    if (!temp)
        return -HASHTABLE_ERR;

    /* Update the array */
    curr = table->entries;
    table->entries = temp;

    /* Update the size */
    old_size = table->table_size;
    table->table_size = 2 * table->table_size;
    table->size = 0;

    int status;

    /* Rehash the elements */
    for (size_t i = 0; i < old_size; i++) {

        if (curr[i].taken == false)
            continue;

        if ((status = hashtable_put(table, curr[i].key,
                                    curr[i].val)) != HASHTABLE_OK)
            return status;
    }

    sol_free(curr);

    return HASHTABLE_OK;
}

/* callback function used with iterate to clean up the hashtable */
static int destroy_entry(struct hashtable_entry *entry) {

    if (!entry)
        return -HASHTABLE_ERR;

    // free key field
    if (entry->key)
        sol_free((void *) entry->key);

    // free value field
    if (entry->val)
        sol_free(entry->val);

    return HASHTABLE_OK;
}


/*
 * Return an empty hashtable, or NULL on failure. The newly create HashTable is
 * dynamically allocated on the heap memory, so it must be released manually.
 */
HashTable *hashtable_create(int (*destructor)(struct hashtable_entry *)) {

    HashTable *table = sol_malloc(sizeof(HashTable));
    if(!table)
        return NULL;

    table->entries = sol_calloc(INITIAL_SIZE, sizeof(struct hashtable_entry));
    if(!table->entries) {
        hashtable_release(table);
        return NULL;
    }

    table->destructor = destructor ? destructor : destroy_entry;

    table->table_size = INITIAL_SIZE;
    table->size = 0;

    return table;
}


size_t hashtable_size(const HashTable *table) {
    return table->size;
}


int hashtable_exists(HashTable *table, const char *key) {
    void *ret = hashtable_get(table, key);
    return !ret ? 0 : 1;
}

/* Add a new key-value pair into the hashtable entries array, use chaining in
   case of collision. */
int hashtable_put(HashTable *table, const char *key, void *val) {

    assert(table && key);

    /* Find a place to put our value */
    int index = hashtable_hash(table, (const uint8_t *) key);

    while (index == -HASHTABLE_FULL){

        if (hashtable_rehash(table) == -HASHTABLE_ERR)
            return -HASHTABLE_ERR;

        index = hashtable_hash(table, (const uint8_t *) key);
    }

    /* Set the entries */
    table->entries[index].val = val;
    table->entries[index].key = key;

    /* Update taken flag, if it was false, update the size also */
    if (table->entries[index].taken == false) {
        table->entries[index].taken = true;
        table->size++;
    }

    return HASHTABLE_OK;
}


/*
 * Get the value void pointer out of the hashtable associated to a key
 */
void *hashtable_get(HashTable *table, const char *key) {

    assert(table && key);

    /* Find data location */
    uint64_t curr = hashtable_hash_int(table, (const uint8_t *) key);

    /* Linear probing, if necessary */
    for (int i = 0; i < MAX_CHAIN_LENGTH; i++){
        if (table->entries[curr].taken == true) {
            if (STREQ(table->entries[curr].key, key, strlen(key)) == true)
                return table->entries[curr].val;
        }
        curr = (curr + 1) % table->table_size;
    }

    /* Not found */
    return NULL;
}


/*
 * Return the key-value pair represented by a key in the hashtable
 */
struct hashtable_entry *hashtable_get_entry(HashTable *table,
                                            const char *key) {

    assert(table && key);

    /* Find data location */
    uint64_t curr = hashtable_hash_int(table, (const uint8_t *) key);

    /* Linear probing, if necessary */
    for (int i = 0; i < MAX_CHAIN_LENGTH; i++) {
        if (table->entries[curr].taken == true) {
            if (STREQ(table->entries[curr].key, key, strlen(key)) == true)
                return &table->entries[curr];
        }

        curr = (curr + 1) % table->table_size;
    }

    /* Not found */
    return NULL;
}


/*
 * Remove an element with that key from the hashtable
 */
int hashtable_del(HashTable *table, const char *key) {

    assert(table && key);

    /* Find key */
    uint64_t curr = hashtable_hash_int(table, (const uint8_t *) key);

    /* Linear probing, if necessary */
    for (int i = 0; i < MAX_CHAIN_LENGTH; i++) {

        // check wether the position in array is in use
        if (table->entries[curr].taken == true) {
            if (STREQ(table->entries[curr].key, key, strlen(key)) == true) {

                /* Blank out the fields */
                table->entries[curr].taken = false;

                /* Reduce the size */
                table->size--;

                /* Destroy the entry */
                table->destructor(&table->entries[curr]);

                return HASHTABLE_OK;
            }
        }

        curr = (curr + 1) % table->table_size;
    }

    /* Data not found */
    return -HASHTABLE_ERR;
}

/*
 * Iterate the function parameter over each element in the hashmap. The unique
 * void * argument is passed to the function as its first argument,
 * representing the key-value pair structure.
 */
int hashtable_map(HashTable *table, int (*func)(struct hashtable_entry *)) {

    assert(func);

    /* On empty hashmap, return immediately */
    if (!table || table->size <= 0)
        return -HASHTABLE_ERR;

    /* Linear probing */
    for (size_t i = 0; i < table->table_size; i++) {

        if (table->entries[i].taken == true) {

            /* Apply function to the key-value entry */
            struct hashtable_entry data = table->entries[i];
            int status = func(&data);

            if (status != HASHTABLE_OK)
                return status;

        }
    }

    return HASHTABLE_OK;
}

/*
 * Iterate through all key-value pairs in the hashtable, accept a functor as
 * parameter to apply function to each pair with an additional parameter
 */
int hashtable_map2(HashTable *table,
                   int (*func)(struct hashtable_entry *, void *),
                   void *param) {

    assert(func);

    /* On empty hashmap, return immediately */
    if (!table || table->size <= 0)
        return -HASHTABLE_ERR;

    /* Linear probing */
    for (size_t i = 0; i < table->table_size; i++) {

        if (table->entries[i].taken == true) {

            /* Apply function to the key-value entry */
            struct hashtable_entry data = table->entries[i];
            int status = func(&data, param);

            if (status != HASHTABLE_OK)
                return status;

        }
    }

    return HASHTABLE_OK;
}

/*
 * Deallocate the hashtable using the defined destructor, if the destructor is
 * NULL it call normal free on key-value pairs.
 */
void hashtable_release(HashTable *table){

    if (!table)
        return;

    hashtable_map(table, table->destructor);

    if (!table || !table->entries)
        return;

    sol_free(table->entries);
    sol_free(table);
}

/* The implementation here was originally done by Gary S. Brown. Slighltly
 * modified by Pete Warden, without any imposition on the reuse of the code.
 */

/* ============================================================= */
/*  COPYRIGHT (C) 1986 Gary S. Brown.  You may use this program, or       */
/*  code or tables extracted from it, as desired without restriction.     */
/*                                                                        */
/*  First, the polynomial itself and its table of feedback terms.  The    */
/*  polynomial is                                                         */
/*  X^32+X^26+X^23+X^22+X^16+X^12+X^11+X^10+X^8+X^7+X^5+X^4+X^2+X^1+X^0   */
/*                                                                        */
/*  Note that we take it "backwards" and put the highest-order term in    */
/*  the lowest-order bit.  The X^32 term is "implied"; the LSB is the     */
/*  X^31 term, etc.  The X^0 term (usually shown as "+1") results in      */
/*  the MSB being 1.                                                      */
/*                                                                        */
/*  Note that the usual hardware shift register implementation, which     */
/*  is what we're using (we're merely optimizing it by doing eight-bit    */
/*  chunks at a time) shifts bits into the lowest-order term.  In our     */
/*  implementation, that means shifting towards the right.  Why do we     */
/*  do it this way?  Because the calculated CRC must be transmitted in    */
/*  order from highest-order term to lowest-order term.  UARTs transmit   */
/*  characters in order from LSB to MSB.  By storing the CRC this way,    */
/*  we hand it to the UART in the order low-byte to high-byte; the UART   */
/*  sends each low-bit to hight-bit; and the result is transmission bit   */
/*  by bit from highest- to lowest-order term without requiring any bit   */
/*  shuffling on our part.  Reception works similarly.                    */
/*                                                                        */
/*  The feedback terms table consists of 256, 32-bit entries.  Notes:     */
/*                                                                        */
/*      The table can be generated at runtime if desired; code to do so   */
/*      is shown later.  It might not be obvious, but the feedback        */
/*      terms simply represent the results of eight shift/xor opera-      */
/*      tions for all combinations of data and CRC register values.       */
/*                                                                        */
/*      The values must be right-shifted by eight bits by the "updcrc"    */
/*      logic; the shift must be unsigned (bring in zeroes).  On some     */
/*      hardware you could probably optimize the shift in assembler by    */
/*      using byte-swap instructions.                                     */
/*      polynomial $edb88320                                              */
/*                                                                        */
/*  --------------------------------------------------------------------  */

static unsigned long crc32_tab[] = {
    0x00000000L, 0x77073096L, 0xee0e612cL, 0x990951baL, 0x076dc419L,
    0x706af48fL, 0xe963a535L, 0x9e6495a3L, 0x0edb8832L, 0x79dcb8a4L,
    0xe0d5e91eL, 0x97d2d988L, 0x09b64c2bL, 0x7eb17cbdL, 0xe7b82d07L,
    0x90bf1d91L, 0x1db71064L, 0x6ab020f2L, 0xf3b97148L, 0x84be41deL,
    0x1adad47dL, 0x6ddde4ebL, 0xf4d4b551L, 0x83d385c7L, 0x136c9856L,
    0x646ba8c0L, 0xfd62f97aL, 0x8a65c9ecL, 0x14015c4fL, 0x63066cd9L,
    0xfa0f3d63L, 0x8d080df5L, 0x3b6e20c8L, 0x4c69105eL, 0xd56041e4L,
    0xa2677172L, 0x3c03e4d1L, 0x4b04d447L, 0xd20d85fdL, 0xa50ab56bL,
    0x35b5a8faL, 0x42b2986cL, 0xdbbbc9d6L, 0xacbcf940L, 0x32d86ce3L,
    0x45df5c75L, 0xdcd60dcfL, 0xabd13d59L, 0x26d930acL, 0x51de003aL,
    0xc8d75180L, 0xbfd06116L, 0x21b4f4b5L, 0x56b3c423L, 0xcfba9599L,
    0xb8bda50fL, 0x2802b89eL, 0x5f058808L, 0xc60cd9b2L, 0xb10be924L,
    0x2f6f7c87L, 0x58684c11L, 0xc1611dabL, 0xb6662d3dL, 0x76dc4190L,
    0x01db7106L, 0x98d220bcL, 0xefd5102aL, 0x71b18589L, 0x06b6b51fL,
    0x9fbfe4a5L, 0xe8b8d433L, 0x7807c9a2L, 0x0f00f934L, 0x9609a88eL,
    0xe10e9818L, 0x7f6a0dbbL, 0x086d3d2dL, 0x91646c97L, 0xe6635c01L,
    0x6b6b51f4L, 0x1c6c6162L, 0x856530d8L, 0xf262004eL, 0x6c0695edL,
    0x1b01a57bL, 0x8208f4c1L, 0xf50fc457L, 0x65b0d9c6L, 0x12b7e950L,
    0x8bbeb8eaL, 0xfcb9887cL, 0x62dd1ddfL, 0x15da2d49L, 0x8cd37cf3L,
    0xfbd44c65L, 0x4db26158L, 0x3ab551ceL, 0xa3bc0074L, 0xd4bb30e2L,
    0x4adfa541L, 0x3dd895d7L, 0xa4d1c46dL, 0xd3d6f4fbL, 0x4369e96aL,
    0x346ed9fcL, 0xad678846L, 0xda60b8d0L, 0x44042d73L, 0x33031de5L,
    0xaa0a4c5fL, 0xdd0d7cc9L, 0x5005713cL, 0x270241aaL, 0xbe0b1010L,
    0xc90c2086L, 0x5768b525L, 0x206f85b3L, 0xb966d409L, 0xce61e49fL,
    0x5edef90eL, 0x29d9c998L, 0xb0d09822L, 0xc7d7a8b4L, 0x59b33d17L,
    0x2eb40d81L, 0xb7bd5c3bL, 0xc0ba6cadL, 0xedb88320L, 0x9abfb3b6L,
    0x03b6e20cL, 0x74b1d29aL, 0xead54739L, 0x9dd277afL, 0x04db2615L,
    0x73dc1683L, 0xe3630b12L, 0x94643b84L, 0x0d6d6a3eL, 0x7a6a5aa8L,
    0xe40ecf0bL, 0x9309ff9dL, 0x0a00ae27L, 0x7d079eb1L, 0xf00f9344L,
    0x8708a3d2L, 0x1e01f268L, 0x6906c2feL, 0xf762575dL, 0x806567cbL,
    0x196c3671L, 0x6e6b06e7L, 0xfed41b76L, 0x89d32be0L, 0x10da7a5aL,
    0x67dd4accL, 0xf9b9df6fL, 0x8ebeeff9L, 0x17b7be43L, 0x60b08ed5L,
    0xd6d6a3e8L, 0xa1d1937eL, 0x38d8c2c4L, 0x4fdff252L, 0xd1bb67f1L,
    0xa6bc5767L, 0x3fb506ddL, 0x48b2364bL, 0xd80d2bdaL, 0xaf0a1b4cL,
    0x36034af6L, 0x41047a60L, 0xdf60efc3L, 0xa867df55L, 0x316e8eefL,
    0x4669be79L, 0xcb61b38cL, 0xbc66831aL, 0x256fd2a0L, 0x5268e236L,
    0xcc0c7795L, 0xbb0b4703L, 0x220216b9L, 0x5505262fL, 0xc5ba3bbeL,
    0xb2bd0b28L, 0x2bb45a92L, 0x5cb36a04L, 0xc2d7ffa7L, 0xb5d0cf31L,
    0x2cd99e8bL, 0x5bdeae1dL, 0x9b64c2b0L, 0xec63f226L, 0x756aa39cL,
    0x026d930aL, 0x9c0906a9L, 0xeb0e363fL, 0x72076785L, 0x05005713L,
    0x95bf4a82L, 0xe2b87a14L, 0x7bb12baeL, 0x0cb61b38L, 0x92d28e9bL,
    0xe5d5be0dL, 0x7cdcefb7L, 0x0bdbdf21L, 0x86d3d2d4L, 0xf1d4e242L,
    0x68ddb3f8L, 0x1fda836eL, 0x81be16cdL, 0xf6b9265bL, 0x6fb077e1L,
    0x18b74777L, 0x88085ae6L, 0xff0f6a70L, 0x66063bcaL, 0x11010b5cL,
    0x8f659effL, 0xf862ae69L, 0x616bffd3L, 0x166ccf45L, 0xa00ae278L,
    0xd70dd2eeL, 0x4e048354L, 0x3903b3c2L, 0xa7672661L, 0xd06016f7L,
    0x4969474dL, 0x3e6e77dbL, 0xaed16a4aL, 0xd9d65adcL, 0x40df0b66L,
    0x37d83bf0L, 0xa9bcae53L, 0xdebb9ec5L, 0x47b2cf7fL, 0x30b5ffe9L,
    0xbdbdf21cL, 0xcabac28aL, 0x53b39330L, 0x24b4a3a6L, 0xbad03605L,
    0xcdd70693L, 0x54de5729L, 0x23d967bfL, 0xb3667a2eL, 0xc4614ab8L,
    0x5d681b02L, 0x2a6f2b94L, 0xb40bbe37L, 0xc30c8ea1L, 0x5a05df1bL,
    0x2d02ef8dL
};

/* Return a 32-bit CRC of the contents of the buffer. */
static unsigned long crc32(const uint8_t *s, unsigned int len) {
    unsigned int i;
    uint64_t crc32val;

    crc32val = 0LL;
    for (i = 0;  i < len;  i ++) {
        crc32val = crc32_tab[(crc32val ^ s[i]) & 0xff] ^ (crc32val >> 8);
    }
    return crc32val;
}
