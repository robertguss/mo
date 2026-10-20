/* CBMC proofs of the arena, src/base/arena.c (V5, MO-55).
 *
 * Every arena the rest of Mo uses is built by mo_arena_init and changed only by the functions
 * below, so each proof starts from any arena those functions can leave behind (ARENA, any_arena)
 * and shows that the function:
 *
 * - reads and writes only inside its chunks, never overflows or wraps an integer, and never fails
 *   an MO_ASSERT, for every size, alignment and limit;
 * - leaves an arena that still meets ARENA, within its limit;
 * - hands out a block of at least the size asked, aligned as asked, that overlaps no block handed
 *   out before it, and ends where the newest chunk's free space starts (what resize relies on);
 * - or, when it fails, changes nothing.
 *
 * The bytecode reader's proofs (cbmc/reader.c) assume exactly this of the arena.
 *
 * arena.c is included rather than linked, so that its static functions and its chunk layout can
 * be used here. Its malloc and realloc are renamed to the system models below. */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

void *system_malloc(size_t size);
void *system_realloc(void *block, size_t size);
#define malloc system_malloc
#define realloc system_realloc
#include "base/arena.c"
#undef malloc
#undef realloc

size_t nondet_size(void);
bool nondet_bool(void);
unsigned char nondet_byte(void);

/* The largest limit an arena is proved for: far above any in src/base/limits.h, and small enough
 * that CBMC can represent a chunk of that size as one object. */
#define MOST_LIMIT ((size_t)1 << 40)

#define HEADER sizeof(struct mo_arena_chunk)

/* ---- The system ---------------------------------------------------------------------------- */

static bool aligned(const void *p, size_t alignment) {
    return ((uintptr_t)p & (uintptr_t)(alignment - 1)) == 0;
}

/* malloc and realloc may fail, and on our targets align what they return to 16 bytes, as
 * arena.c's chunk layout requires. A failed realloc leaves the block as it was. */
void *system_malloc(size_t size) {
    if (nondet_bool()) {
        return NULL;
    }
    void *p = malloc(size);
    __CPROVER_assume(p != NULL && aligned(p, 16));
    return p;
}

void *system_realloc(void *block, size_t size) {
    if (nondet_bool()) {
        return NULL;
    }
    void *p = realloc(block, size);
    __CPROVER_assume(p != NULL && aligned(p, 16));
    return p;
}

/* An MO_ASSERT that fails is a failed proof, not the end of a path. */
_Noreturn void mo_assert_failed(const char *expression, const char *file, int line) {
    (void)expression;
    (void)file;
    (void)line;
    __CPROVER_assert(0, "MO_ASSERT holds");
    __CPROVER_assume(0);
    abort();
}

/* ---- Specification ------------------------------------------------------------------------ */

/* The sizes the next chunk can have: the first size, doubled up to the largest. */
static bool chunk_size_ok(size_t size) {
    return is_power_of_two(size) && size >= FIRST_CHUNK_SIZE && size <= LARGEST_CHUNK_SIZE;
}

/* A chunk the arena took from the system: its own object of its header and capacity bytes,
 * aligned to 16, with used no more than capacity. */
static bool chunk_ok(const struct mo_arena_chunk *c) {
    return __CPROVER_POINTER_OFFSET(c) == 0 && __CPROVER_OBJECT_SIZE(c) == HEADER + c->capacity &&
           aligned(c, 16) && c->used <= c->capacity;
}

/* An arena: within its limit, and its newest chunk, when it has one, counted in what it used. */
static bool arena_ok(const struct mo_arena *a) {
    if (a->limit > MOST_LIMIT || a->used > a->limit || !chunk_size_ok(a->next_chunk_size)) {
        return false;
    }
    if (a->chunk == NULL) {
        return a->used == 0;
    }
    return chunk_ok(a->chunk) && HEADER + a->chunk->capacity <= a->used;
}

/* Any arena the arena's functions can leave behind. Only the newest chunk is ever read; the
 * ones before it are only freed, which harness_free covers. */
static void any_arena(struct mo_arena *a) {
    a->limit = nondet_size();
    a->used = nondet_size();
    a->next_chunk_size = nondet_size();
    a->chunk = NULL;
    __CPROVER_assume(a->limit <= MOST_LIMIT && a->used <= a->limit &&
                     chunk_size_ok(a->next_chunk_size));
    if (a->used > 0 && nondet_bool()) {
        size_t capacity = nondet_size();
        __CPROVER_assume(HEADER <= a->used && capacity <= a->used - HEADER);
        struct mo_arena_chunk *c = malloc(HEADER + capacity);
        __CPROVER_assume(c != NULL && aligned(c, 16));
        c->previous = NULL;
        c->capacity = capacity;
        c->used = nondet_size();
        __CPROVER_assume(c->used <= capacity);
        a->chunk = c;
    }
    __CPROVER_assume(a->chunk != NULL || a->used == 0);
    __CPROVER_assert(arena_ok(a), "any_arena makes an arena");
}

static size_t any_alignment(void) {
    size_t alignment = nondet_size();
    __CPROVER_assume(is_power_of_two(alignment) && alignment <= MO_ARENA_MAX_ALIGNMENT);
    return alignment;
}

/* Where the newest chunk's free space starts. */
static unsigned char *free_space(const struct mo_arena *a) {
    return chunk_data(a->chunk) + a->chunk->used;
}

static bool same_arena(const struct mo_arena *a, const struct mo_arena *b) {
    return a->chunk == b->chunk && a->used == b->used && a->limit == b->limit &&
           a->next_chunk_size == b->next_chunk_size;
}

/* A block of size bytes the arena just handed out from before: aligned, writable, the newest in
 * the newest chunk, and after every block handed out before it. */
static bool new_block(const struct mo_arena *a, const struct mo_arena *before, size_t old_used,
                      const unsigned char *block, size_t size, size_t alignment) {
    size_t n = size == 0 ? 1 : size;
    if (!aligned(block, alignment) || !__CPROVER_w_ok(block, n) ||
        !__CPROVER_same_object(block, a->chunk) || block + n != free_space(a)) {
        return false;
    }
    if (a->chunk == before->chunk) {
        return block >= chunk_data(a->chunk) + old_used && a->used == before->used;
    }
    /* A new chunk, linked to the one before it and counted in what the arena used. */
    return a->chunk->previous == before->chunk && a->used > before->used &&
           a->used - before->used == HEADER + a->chunk->capacity &&
           (a->next_chunk_size == before->next_chunk_size * 2 ||
            a->next_chunk_size == LARGEST_CHUNK_SIZE);
}

/* ---- Harnesses ---------------------------------------------------------------------------- */

/* Every harness ends here, so prove.sh --reach can check that some input gets to the end of it. */
void reached_end(void) {
}

void harness_arena_allocate(void) {
    struct mo_arena a;
    any_arena(&a);
    struct mo_arena before = a;
    size_t old_used = a.chunk != NULL ? a.chunk->used : 0;
    size_t size = nondet_size();
    size_t alignment = any_alignment();
    unsigned char *block = mo_arena_allocate(&a, size, alignment);
    __CPROVER_assert(arena_ok(&a) && a.limit == before.limit, "still an arena, same limit");
    if (block == NULL) {
        __CPROVER_assert(same_arena(&a, &before) &&
                             (a.chunk == NULL || a.chunk->used == old_used),
                         "a failed allocation changes nothing");
    } else {
        __CPROVER_assert(new_block(&a, &before, old_used, block, size, alignment),
                         "a new block of the size asked");
    }
    reached_end();
}

void harness_arena_allocate_array(void) {
    struct mo_arena a;
    any_arena(&a);
    struct mo_arena before = a;
    size_t old_used = a.chunk != NULL ? a.chunk->used : 0;
    size_t count = nondet_size();
    size_t element_size = nondet_size();
    size_t alignment = any_alignment();
    unsigned char *block = mo_arena_allocate_array(&a, count, element_size, alignment);
    __CPROVER_assert(arena_ok(&a), "still an arena");
    if (block == NULL) {
        __CPROVER_assert(same_arena(&a, &before) &&
                             (a.chunk == NULL || a.chunk->used == old_used),
                         "a failed allocation changes nothing");
    } else {
        __CPROVER_assert(element_size == 0 || count <= SIZE_MAX / element_size,
                         "a table whose size wraps is refused");
        __CPROVER_assert(new_block(&a, &before, old_used, block, count * element_size, alignment),
                         "a new block for the whole table");
    }
    reached_end();
}

void harness_arena_resize(void) {
    struct mo_arena a;
    any_arena(&a);
    size_t alignment = any_alignment();
    size_t old_size = nondet_size();
    size_t new_size = nondet_size();
    unsigned char *block = NULL;
    if (nondet_bool()) {
        /* No block yet: resize allocates. */
        old_size = 0;
    } else if (a.chunk != NULL && nondet_bool()) {
        /* A block in the newest chunk, perhaps the newest block. */
        size_t offset = nondet_size();
        __CPROVER_assume(offset <= a.chunk->used && old_size <= a.chunk->used - offset);
        block = chunk_data(a.chunk) + offset;
        __CPROVER_assume(aligned(block, alignment));
    } else {
        /* A block in an older chunk, which is another object. */
        __CPROVER_assume(old_size <= a.limit);
        block = malloc(old_size);
        __CPROVER_assume(block != NULL && aligned(block, alignment));
    }
    struct mo_arena before = a;
    size_t old_used = a.chunk != NULL ? a.chunk->used : 0;
    /* Any byte the block keeps, and its value. */
    size_t kept = old_size < new_size ? old_size : new_size;
    bool keeps = kept > 0;
    size_t i = keeps ? nondet_size() : 0;
    __CPROVER_assume(!keeps || i < kept);
    unsigned char value = keeps ? block[i] : 0;

    unsigned char *resized = mo_arena_resize(&a, block, old_size, new_size, alignment);
    __CPROVER_assert(arena_ok(&a) && a.limit == before.limit, "still an arena, same limit");
    if (resized == NULL) {
        __CPROVER_assert(same_arena(&a, &before) &&
                             (a.chunk == NULL || a.chunk->used == old_used),
                         "a failed resize changes nothing");
        __CPROVER_assert(!keeps || block[i] == value, "a failed resize keeps the block");
    } else {
        __CPROVER_assert(aligned(resized, alignment) && __CPROVER_w_ok(resized, new_size),
                         "a block of the new size");
        __CPROVER_assert(!keeps || resized[i] == value, "the block keeps its bytes");
        if (resized != block || new_size > old_size) {
            /* A grown or moved block is the newest, so it overlaps no other. */
            __CPROVER_assert(__CPROVER_same_object(resized, a.chunk) &&
                                 resized + (new_size == 0 ? 1 : new_size) <= free_space(&a),
                             "a grown block is in the newest chunk");
        }
    }
    reached_end();
}

void harness_arena_copy(void) {
    struct mo_arena a;
    any_arena(&a);
    size_t size = nondet_size();
    __CPROVER_assume(size <= a.limit);
    const unsigned char *bytes = size == 0 && nondet_bool() ? NULL : malloc(size);
    __CPROVER_assume(bytes != NULL || size == 0);
    size_t alignment = any_alignment();
    unsigned char *copy = mo_arena_copy(&a, bytes, size, alignment);
    __CPROVER_assert(arena_ok(&a), "still an arena");
    if (copy != NULL) {
        __CPROVER_assert(aligned(copy, alignment) && __CPROVER_w_ok(copy, size == 0 ? 1 : size),
                         "a block of the size copied");
        if (size > 0) {
            size_t i = nondet_size();
            __CPROVER_assume(i < size);
            __CPROVER_assert(copy[i] == bytes[i], "the same bytes");
        }
    }
    reached_end();
}

/* Texts of up to TEXT bytes, terminator included. */
#define TEXT 8

void harness_arena_copy_text(void) {
    struct mo_arena a;
    any_arena(&a);
    size_t size = nondet_size();
    __CPROVER_assume(size >= 1 && size <= TEXT);
    char *text = malloc(size);
    __CPROVER_assume(text != NULL);
    text[size - 1] = '\0';
    char *copy = mo_arena_copy_text(&a, text);
    __CPROVER_assert(arena_ok(&a), "still an arena");
    if (copy != NULL) {
        size_t length = strlen(text);
        size_t i = nondet_size();
        __CPROVER_assume(i <= length);
        __CPROVER_assert(__CPROVER_w_ok(copy, length + 1) && copy[i] == text[i],
                         "the same text, terminated");
    }
    reached_end();
}

/* Chunks of the arena harness_free frees: enough to run the loop's body more than once. */
#define CHUNKS 3

/* Freeing returns every chunk to the system (prove.sh runs it with --memory-leak-check), and
 * leaves an empty arena with the same limit. */
void harness_arena_free(void) {
    struct mo_arena a;
    any_arena(&a);
    if (a.chunk != NULL) {
        free(a.chunk);
    }
    a.chunk = NULL;
    size_t chunks = nondet_size();
    __CPROVER_assume(chunks <= CHUNKS);
    for (size_t k = 0; k < chunks; k++) {
        struct mo_arena_chunk *c = malloc(HEADER);
        __CPROVER_assume(c != NULL);
        c->previous = a.chunk;
        c->capacity = 0;
        c->used = 0;
        a.chunk = c;
    }
    size_t limit = a.limit;
    mo_arena_free(&a);
    __CPROVER_assert(a.chunk == NULL && a.used == 0 && a.limit == limit &&
                         a.next_chunk_size == FIRST_CHUNK_SIZE,
                     "an empty arena with the same limit");
    reached_end();
}
