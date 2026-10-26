#ifndef MO_STORE_MEMORY_H
#define MO_STORE_MEMORY_H

#include "base/base.h"

/* SQLite allocator callbacks. Each block is charged its exact size plus a
 * small header before it is allocated; all blocks together stay within
 * MO_LIMIT_STORE_MEMORY. Reallocation preserves the old block on refusal.
 * The callbacks are safe across SQLite connections and threads. */
MO_MUST_USE void *mo_store_malloc(int size);
void mo_store_free(void *pointer);
MO_MUST_USE void *mo_store_realloc(void *pointer, int size);
MO_MUST_USE int mo_store_size(void *pointer);
MO_MUST_USE int mo_store_roundup(int size);

#endif
