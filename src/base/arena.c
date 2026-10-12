#include "base/arena.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* A chunk: its header, then capacity bytes, of which the first used have been handed out. The
 * data starts aligned for any type: malloc's memory is (C17 7.22.3), and the header's size is a
 * multiple of max_align_t's alignment, the alignment of data. */
struct mo_arena_chunk {
    struct mo_arena_chunk *previous;
    size_t capacity;
    size_t used;
    max_align_t data[];
};

_Static_assert(sizeof(struct mo_arena_chunk) <= MO_ARENA_CHUNK_HEADER,
               "MO_ARENA_CHUNK_HEADER covers a chunk's header");
_Static_assert(sizeof(struct mo_arena_chunk) % _Alignof(max_align_t) == 0,
               "a chunk's data is aligned for any type");

/* Chunks start small, so a small arena takes little, and double up to the largest size, so a large
 * one makes few calls to malloc. A request that needs more gets a chunk of its own size. */
enum { FIRST_CHUNK_SIZE = 4096, LARGEST_CHUNK_SIZE = 1 << 20 };

static bool is_power_of_two(size_t value) {
    return value != 0 && (value & (value - 1)) == 0;
}

static unsigned char *chunk_data(struct mo_arena_chunk *chunk) {
    return (unsigned char *)chunk->data;
}

/* Bytes of padding before the chunk's next free byte is aligned to alignment. */
static size_t padding_for(struct mo_arena_chunk *chunk, size_t alignment) {
    uintptr_t next = (uintptr_t)(chunk_data(chunk) + chunk->used);
    return (size_t)(((uintptr_t)0 - next) & (uintptr_t)(alignment - 1));
}

/* Whether the chunk has room for size bytes aligned to alignment. */
static bool fits(struct mo_arena_chunk *chunk, size_t size, size_t alignment) {
    size_t room = chunk->capacity - chunk->used;
    size_t padding = padding_for(chunk, alignment);
    return padding <= room && size <= room - padding;
}

/* Takes a new chunk from the system, with room for size bytes aligned to alignment, and makes it
 * the newest. False, changing nothing, past the limit or when the system has no memory. */
static bool add_chunk(struct mo_arena *arena, size_t size, size_t alignment) {
    MO_ASSERT(arena->used <= arena->limit);
    size_t header = sizeof(struct mo_arena_chunk);
    /* The data is aligned for max_align_t, so only a larger alignment can need padding. */
    size_t padding = alignment > _Alignof(max_align_t) ? alignment - _Alignof(max_align_t) : 0;
    size_t remaining = arena->limit - arena->used;
    if (size > remaining || header + padding > remaining - size) {
        return false;
    }
    size_t needed = header + padding + size;
    size_t total = needed > arena->next_chunk_size ? needed : arena->next_chunk_size;
    if (total > remaining) {
        total = remaining;
    }
    struct mo_arena_chunk *chunk = malloc(total);
    if (chunk == NULL) {
        return false;
    }
    chunk->previous = arena->chunk;
    chunk->capacity = total - header;
    chunk->used = 0;
    arena->chunk = chunk;
    arena->used += total;
    if (arena->next_chunk_size < LARGEST_CHUNK_SIZE) {
        arena->next_chunk_size *= 2;
    }
    MO_ASSERT(fits(chunk, size, alignment));
    return true;
}

void mo_arena_init(struct mo_arena *arena, size_t limit) {
    MO_ASSERT(arena != NULL);
    *arena = (struct mo_arena){
        .chunk = NULL, .used = 0, .limit = limit, .next_chunk_size = FIRST_CHUNK_SIZE};
}

void mo_arena_free(struct mo_arena *arena) {
    MO_ASSERT(arena != NULL);
    struct mo_arena_chunk *chunk = arena->chunk;
    while (chunk != NULL) {
        struct mo_arena_chunk *previous = chunk->previous;
        free(chunk);
        chunk = previous;
    }
    mo_arena_init(arena, arena->limit);
}

void *mo_arena_allocate(struct mo_arena *arena, size_t size, size_t alignment) {
    MO_ASSERT(arena != NULL);
    MO_ASSERT(is_power_of_two(alignment) && alignment <= MO_ARENA_MAX_ALIGNMENT);
    if (size == 0) {
        size = 1;
    }
    if (arena->chunk == NULL || !fits(arena->chunk, size, alignment)) {
        if (!add_chunk(arena, size, alignment)) {
            return NULL;
        }
    }
    struct mo_arena_chunk *chunk = arena->chunk;
    size_t padding = padding_for(chunk, alignment);
    unsigned char *block = chunk_data(chunk) + chunk->used + padding;
    chunk->used += padding + size;
    MO_ASSERT(chunk->used <= chunk->capacity);
    MO_ASSERT(((uintptr_t)block & (uintptr_t)(alignment - 1)) == 0);
    return block;
}

void *mo_arena_allocate_array(struct mo_arena *arena, size_t count, size_t element_size,
                              size_t alignment) {
    if (element_size != 0 && count > SIZE_MAX / element_size) {
        return NULL;
    }
    return mo_arena_allocate(arena, count * element_size, alignment);
}

/* Grows the newest chunk, which holds only the newest block, to hold size bytes. */
static bool grow_alone(struct mo_arena *arena, size_t size) {
    struct mo_arena_chunk *chunk = arena->chunk;
    size_t header = sizeof(struct mo_arena_chunk);
    MO_ASSERT(size > chunk->capacity);
    size_t growth = size - chunk->capacity;
    if (growth > arena->limit - arena->used) {
        return false;
    }
    struct mo_arena_chunk *grown = realloc(chunk, header + size);
    if (grown == NULL) {
        return false;
    }
    grown->capacity = size;
    grown->used = size;
    arena->chunk = grown;
    arena->used += growth;
    return true;
}

void *mo_arena_resize(struct mo_arena *arena, void *block, size_t old_size, size_t new_size,
                      size_t alignment) {
    MO_ASSERT(arena != NULL);
    MO_ASSERT(is_power_of_two(alignment) && alignment <= MO_ARENA_MAX_ALIGNMENT);
    if (block == NULL) {
        MO_ASSERT(old_size == 0);
        return mo_arena_allocate(arena, new_size, alignment);
    }
    MO_ASSERT(((uintptr_t)block & (uintptr_t)(alignment - 1)) == 0);
    unsigned char *start = block;
    struct mo_arena_chunk *chunk = arena->chunk;
    if (chunk != NULL && start + old_size == chunk_data(chunk) + chunk->used) {
        /* The newest block: it ends where the chunk's free space starts. */
        MO_ASSERT(old_size <= chunk->used);
        size_t offset = chunk->used - old_size;
        if (new_size <= chunk->capacity - offset) {
            chunk->used = offset + new_size;
            return block;
        }
        if (offset == 0 && alignment <= _Alignof(max_align_t)) {
            if (!grow_alone(arena, new_size)) {
                return NULL;
            }
            unsigned char *moved = chunk_data(arena->chunk);
            MO_ASSERT(((uintptr_t)moved & (uintptr_t)(alignment - 1)) == 0);
            return moved;
        }
    }
    if (new_size <= old_size) {
        return block;
    }
    void *moved = mo_arena_allocate(arena, new_size, alignment);
    if (moved == NULL) {
        return NULL;
    }
    memcpy(moved, block, old_size);
    return moved;
}

void *mo_arena_copy(struct mo_arena *arena, const void *bytes, size_t size, size_t alignment) {
    MO_ASSERT(bytes != NULL || size == 0);
    void *copy = mo_arena_allocate(arena, size, alignment);
    if (copy != NULL && size > 0) {
        memcpy(copy, bytes, size);
    }
    return copy;
}

char *mo_arena_copy_text(struct mo_arena *arena, const char *text) {
    MO_ASSERT(text != NULL);
    return mo_arena_copy(arena, text, strlen(text) + 1, 1);
}
