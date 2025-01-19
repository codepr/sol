#ifndef ARENA_H
#define ARENA_H

#include <stdint.h>
#include <stdlib.h>

typedef struct pool_allocator {
    void *(*alloc)(void *context);
    void (*free)(void *context, void *ptr);
    void *context;
} Pool_Allocator;

#define pool_alloc(a)   ((a)->alloc((a)->context))
#define pool_free(a, p) ((a)->free((a)->context, p))

void *arena_pool_alloc(void *context);
void arena_pool_free(void *context, void *ptr);

struct memorypool *memorypool_new(size_t, size_t);
void memorypool_destroy(struct memorypool *);
void *memorypool_alloc(struct memorypool *);
void memorypool_free(struct memorypool *, void *);

// Free list definition

typedef struct Free_List_Header {
    size_t block_size;
    size_t padding;
} Free_List_Header;

typedef struct Free_List_Node {
    struct Free_List_Node *next;
    size_t block_size;
} Free_List_Node;

typedef struct Free_List {
    void *data;
    size_t size;
    size_t used;
    Free_List_Node *head;
} Free_List;

typedef struct arena_allocator {
    void *(*alloc)(void *context, size_t size);
    void (*free)(void *context, void *ptr);
    void *context;
} Arena_Allocator;

#define arena_alloc(a, s) ((a)->alloc((a)->context, (s)))
#define arena_free(a, p)  ((a)->free((a)->context, (p)))

Free_List *free_list_new(size_t size);
void *free_list_alloc(void *, size_t);
void free_list_free(void *, void *);

#endif
