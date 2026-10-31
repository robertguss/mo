/* Arenas: memory handed out in pieces and freed all at once (Tiger Style, architecture section 2).
 *
 * An arena takes memory from the system in chunks, and hands it out by moving a pointer through
 * the newest chunk. Nothing is freed on its own: mo_arena_free frees everything at once. Each
 * arena has a limit on the memory it takes from the system, chunk headers included, and every
 * allocation reports failure, past the limit or when the system has no more, by returning NULL.
 *
 * Apart from src/heap, which holds a running program's values, this is the only code that calls
 * malloc. */
#ifndef MO_ARENA_H
#define MO_ARENA_H

#include <stdbool.h>
#include <stddef.h>

#include "base/base.h"

/* The alignment for memory whose type the caller doesn't name: enough for max_align_t and the
 * 16-byte wide integers. It is 16 on every target, including Apple arm64 where max_align_t's
 * alignment is only 8, so a program takes the same arena memory everywhere and reaches a limit at
 * the same place. Chunks start aligned to it. */
enum { MO_ARENA_ALIGNMENT = 16 };

/* The most memory an arena takes from the system beyond what it hands out, per chunk: the chunk's
 * header. An arena that must hold n bytes in one piece needs a limit of at least
 * n + MO_ARENA_CHUNK_HEADER, with alignment - MO_ARENA_ALIGNMENT more for an alignment larger
 * than MO_ARENA_ALIGNMENT. */
enum { MO_ARENA_CHUNK_HEADER = 32 };

/* The largest alignment an arena hands out. */
enum { MO_ARENA_MAX_ALIGNMENT = 4096 };

struct mo_arena_chunk;

/* An arena. Its fields are private, except that used and limit may be read. */
struct mo_arena {
    struct mo_arena_chunk *chunk; /* the newest chunk, which links to the ones before it */
    size_t used;                  /* bytes taken from the system, chunk headers included */
    size_t limit;                 /* the most bytes it may take */
    size_t next_chunk_size;       /* the size of the next chunk, unless a request needs more */
    bool confidential;
};

/* Prepares an empty arena that may take up to limit bytes from the system. It takes nothing yet,
 * so it can't fail. */
void mo_arena_init(struct mo_arena *arena, size_t limit);
void mo_arena_init_confidential(struct mo_arena *arena, size_t limit);

/* Frees everything the arena handed out, at once. The arena is then empty, with the same limit,
 * and can be used again. */
void mo_arena_free(struct mo_arena *arena);

/* size bytes aligned to alignment, a power of two up to MO_ARENA_MAX_ALIGNMENT. A size of 0 hands
 * out one byte, so every result is distinct. NULL past the limit, or when the system has no
 * memory. The memory is not cleared. */
MO_MUST_USE void *mo_arena_allocate(struct mo_arena *arena, size_t size, size_t alignment);

/* count elements of element_size bytes each, as mo_arena_allocate does; NULL also when their size
 * doesn't fit in a size_t. */
MO_MUST_USE void *mo_arena_allocate_array(struct mo_arena *arena, size_t count, size_t element_size,
                                          size_t alignment);

/* Resizes block, of old_size bytes, which this arena handed out with alignment, to new_size bytes,
 * keeping its first bytes, as many as both sizes have. A block may be NULL with an old_size of 0,
 * which allocates.
 *
 * The newest block of an arena changes size in place while its chunk has room. When it is alone in
 * its chunk, with an alignment no larger than the chunk's (at least 16), the chunk itself grows,
 * and may move.
 * Other blocks move to a new block and leave the old one unused until mo_arena_free. So a buffer
 * that grows by doubling, and is the only thing its arena holds, wastes nothing.
 *
 * Once it succeeds, use only the block it returns: the old one may be gone. On failure it returns
 * NULL and leaves the block as it was. Shrinking never fails, and keeps the block where it is: the
 * newest block gives what it no longer needs to the blocks after it, all of it when shrunk to 0,
 * after which it must not be used. */
MO_MUST_USE void *mo_arena_resize(struct mo_arena *arena, void *block, size_t old_size,
                                  size_t new_size, size_t alignment);

/* A copy of size bytes, aligned to alignment, or NULL as mo_arena_allocate. */
MO_MUST_USE void *mo_arena_copy(struct mo_arena *arena, const void *bytes, size_t size,
                                size_t alignment);

/* A copy of the text, with its terminating NUL, or NULL as mo_arena_allocate. */
MO_MUST_USE char *mo_arena_copy_text(struct mo_arena *arena, const char *text);

#endif
