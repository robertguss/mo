// Minimal self-contained support utilities for the Bison executable grammar.
#include "support.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    MO_ARENA_ALIGNMENT = 16,
    MO_ARENA_MAX_ALIGNMENT = 4096,
    FIRST_CHUNK_SIZE = 4096,
    LARGEST_CHUNK_SIZE = 1 << 20,
    FIRST_BUFFER_CAPACITY = 256,
};

struct mo_arena_chunk {
    struct mo_arena_chunk *previous;
    size_t capacity;
    size_t used;
    _Alignas(MO_ARENA_ALIGNMENT) max_align_t data[];
};

_Noreturn void mo_grammar_assert_failed(const char *expression, const char *file, int line) {
    (void)fprintf(stderr, "mo-grammar: assertion failed at %s:%d: %s\n", file, line, expression);
    abort();
}

static bool is_power_of_two(size_t value) {
    return value != 0 && (value & (value - 1)) == 0;
}

static unsigned char *chunk_data(struct mo_arena_chunk *chunk) {
    return (unsigned char *)chunk->data;
}

static size_t padding_for(struct mo_arena_chunk *chunk, size_t alignment) {
    uintptr_t next = (uintptr_t)(chunk_data(chunk) + chunk->used);
    return (size_t)(((uintptr_t)0 - next) & (uintptr_t)(alignment - 1));
}

static bool fits(struct mo_arena_chunk *chunk, size_t size, size_t alignment) {
    size_t room = chunk->capacity - chunk->used;
    size_t padding = padding_for(chunk, alignment);
    return padding <= room && size <= room - padding;
}

static bool add_chunk(struct mo_arena *arena, size_t size, size_t alignment) {
    MO_ASSERT(arena->used <= arena->limit);
    size_t header = sizeof(struct mo_arena_chunk);
    size_t base_alignment = _Alignof(struct mo_arena_chunk);
    size_t padding = alignment > base_alignment ? alignment - base_alignment : 0;
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
    return block;
}

void mo_buffer_init(struct mo_buffer *buffer, size_t limit) {
    MO_ASSERT(buffer != NULL);
    *buffer = (struct mo_buffer){.bytes = NULL, .length = 0, .capacity = 0, .limit = limit};
}

void mo_buffer_free(struct mo_buffer *buffer) {
    MO_ASSERT(buffer != NULL);
    free(buffer->bytes);
    buffer->bytes = NULL;
    buffer->length = 0;
    buffer->capacity = 0;
}

bool mo_buffer_append(struct mo_buffer *buffer, const void *bytes, size_t length) {
    MO_ASSERT(buffer != NULL && (bytes != NULL || length == 0));
    if (length > buffer->limit - buffer->length) {
        return false;
    }
    size_t needed = buffer->length + length;
    if (needed > buffer->capacity) {
        size_t capacity =
            buffer->capacity < FIRST_BUFFER_CAPACITY ? FIRST_BUFFER_CAPACITY : buffer->capacity;
        while (capacity < needed && capacity <= buffer->limit / 2) {
            capacity *= 2;
        }
        if (capacity < needed || capacity > buffer->limit) {
            capacity = buffer->limit;
        }
        uint8_t *grown = realloc(buffer->bytes, capacity);
        if (grown == NULL) {
            return false;
        }
        buffer->bytes = grown;
        buffer->capacity = capacity;
    }
    if (length > 0) {
        memcpy(buffer->bytes + buffer->length, bytes, length);
        buffer->length += length;
    }
    return true;
}
