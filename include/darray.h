#ifndef DARRAY_H
#define DARRAY_H

#include <assert.h>

// Dynamic array helpers

#define da_extend(da)                                                          \
    do {                                                                       \
        (da).capacity *= 2;                                                    \
        (da).items = realloc((da).items, (da).capacity * sizeof(*(da).items)); \
        if (!(da).items) {                                                     \
            fprintf(stderr, "DA realloc failed");                              \
            exit(EXIT_FAILURE);                                                \
        }                                                                      \
    } while (0)

#define da_append(da, item)                                                    \
    do {                                                                       \
        assert((da));                                                          \
        if ((da).length + 1 == (da).capacity)                                  \
            da_extend((da));                                                   \
        (da).items[(da).length++] = (item);                                    \
    } while (0)

#endif
