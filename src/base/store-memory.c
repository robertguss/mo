#include "base/store-memory.h"

#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "base/limits.h"

/* One exact allocation per block, charged at its full size including this header. */
struct block {
    size_t charge;
    int size;
    _Alignas(16) unsigned char bytes[];
};
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static size_t used;

void *mo_store_malloc(int size) {
    if (size <= 0 || size > MO_LIMIT_STORE_MEMORY)
        return NULL;
    size_t charge = sizeof(struct block) + (size_t)size;
    MO_ASSERT(pthread_mutex_lock(&mutex) == 0);
    bool fits = charge <= MO_LIMIT_STORE_MEMORY - used;
    if (fits)
        used += charge;
    MO_ASSERT(pthread_mutex_unlock(&mutex) == 0);
    struct block *b = fits ? malloc(charge) : NULL;
    if (b == NULL) {
        if (fits) {
            MO_ASSERT(pthread_mutex_lock(&mutex) == 0);
            used -= charge;
            MO_ASSERT(pthread_mutex_unlock(&mutex) == 0);
        }
        return NULL;
    }
    b->charge = charge;
    b->size = size;
    return b->bytes;
}

static struct block *owner(void *pointer) {
    return (struct block *)((unsigned char *)pointer - offsetof(struct block, bytes));
}

void mo_store_free(void *pointer) {
    if (pointer != NULL) {
        struct block *b = owner(pointer);
        size_t charge = b->charge;
        free(b);
        MO_ASSERT(pthread_mutex_lock(&mutex) == 0);
        MO_ASSERT(used >= charge);
        used -= charge;
        MO_ASSERT(pthread_mutex_unlock(&mutex) == 0);
    }
}

void *mo_store_realloc(void *pointer, int size) {
    if (size <= 0) {
        mo_store_free(pointer);
        return NULL;
    }
    void *result = mo_store_malloc(size);
    if (result != NULL && pointer != NULL) {
        int old = owner(pointer)->size;
        memcpy(result, pointer, (size_t)(old < size ? old : size));
        mo_store_free(pointer);
    }
    return result;
}

int mo_store_size(void *pointer) {
    return owner(pointer)->size;
}

int mo_store_roundup(int size) {
    return size > 0 && size <= INT_MAX - 7 ? (size + 7) & ~7 : 0;
}
