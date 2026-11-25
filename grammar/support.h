// Minimal self-contained support utilities for the Bison executable grammar
// (mo-grammar): assertions, bump arena, and growable byte stack.
#ifndef MO_GRAMMAR_SUPPORT_H
#define MO_GRAMMAR_SUPPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MO_MUST_USE __attribute__((warn_unused_result))

_Noreturn void mo_grammar_assert_failed(const char *expression, const char *file, int line);

#define MO_ASSERT(expression)                                                                      \
    ((expression) ? (void)0 : mo_grammar_assert_failed(#expression, __FILE__, __LINE__))

#define MO_UNREACHABLE()                                                                           \
    mo_grammar_assert_failed("unreachable code was reached", __FILE__, __LINE__)

struct mo_arena_chunk;

struct mo_arena {
    struct mo_arena_chunk *chunk;
    size_t used;
    size_t limit;
    size_t next_chunk_size;
};

void mo_arena_init(struct mo_arena *arena, size_t limit);
void mo_arena_free(struct mo_arena *arena);
MO_MUST_USE void *mo_arena_allocate(struct mo_arena *arena, size_t size, size_t alignment);

struct mo_buffer {
    uint8_t *bytes;
    size_t length;
    size_t capacity;
    size_t limit;
};

void mo_buffer_init(struct mo_buffer *buffer, size_t limit);
void mo_buffer_free(struct mo_buffer *buffer);
MO_MUST_USE bool mo_buffer_append(struct mo_buffer *buffer, const void *bytes, size_t length);

#endif
