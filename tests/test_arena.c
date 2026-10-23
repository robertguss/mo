#include "base/arena.h"
#include "test.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static bool aligned(const void *pointer, size_t alignment) {
    return ((uintptr_t)pointer & (uintptr_t)(alignment - 1)) == 0;
}

static void test_blocks_are_aligned_and_apart(void) {
    struct mo_arena arena;
    mo_arena_init(&arena, 1 << 24);
    const size_t alignments[] = {1, 2, 4, 8, 16, 32, 64, 256, MO_ARENA_MAX_ALIGNMENT};
    const size_t sizes[] = {1, 3, 8, 17, 100, 5000, 70000};
    enum { BLOCKS = sizeof alignments / sizeof alignments[0] * (sizeof sizes / sizeof sizes[0]) };
    unsigned char *blocks[BLOCKS];
    size_t lengths[BLOCKS];
    size_t count = 0;
    for (size_t round = 0; round < 2; round++) {
        count = 0;
        for (size_t a = 0; a < sizeof alignments / sizeof alignments[0]; a++) {
            for (size_t s = 0; s < sizeof sizes / sizeof sizes[0]; s++) {
                unsigned char *block = mo_arena_allocate(&arena, sizes[s], alignments[a]);
                CHECK(block != NULL);
                if (block == NULL) {
                    continue;
                }
                CHECK(aligned(block, alignments[a]));
                /* Fill each block with its own byte; a block that overlapped another would
                 * change it. */
                memset(block, (int)(count % 251), sizes[s]);
                blocks[count] = block;
                lengths[count] = sizes[s];
                count += 1;
            }
        }
        for (size_t i = 0; i < count; i++) {
            bool intact = true;
            for (size_t j = 0; j < lengths[i]; j++) {
                intact = intact && blocks[i][j] == (unsigned char)(i % 251);
            }
            CHECK(intact);
        }
        CHECK(arena.used <= arena.limit);
        /* The second round runs in an arena freed and used again. */
        mo_arena_free(&arena);
        CHECK(arena.used == 0 && arena.chunk == NULL && arena.limit == 1 << 24);
    }
}

static void test_a_size_of_zero_gives_distinct_blocks(void) {
    struct mo_arena arena;
    mo_arena_init(&arena, 1 << 16);
    void *first = mo_arena_allocate(&arena, 0, 1);
    void *second = mo_arena_allocate(&arena, 0, 1);
    CHECK(first != NULL && second != NULL && first != second);
    mo_arena_free(&arena);
}

static void test_the_limit_holds(void) {
    /* Small blocks until the arena refuses one: it never takes more than its limit. */
    struct mo_arena arena;
    mo_arena_init(&arena, 10000);
    size_t handed_out = 0;
    for (;;) {
        void *block = mo_arena_allocate(&arena, 100, 4);
        CHECK(arena.used <= 10000);
        if (block == NULL) {
            break;
        }
        handed_out += 100;
        CHECK(handed_out <= 10000);
    }
    /* It took two chunks, and left less than a block unused at the end of each. */
    CHECK(handed_out >= 10000 - 2 * MO_ARENA_CHUNK_HEADER - 2 * 100);
    size_t used = arena.used;
    CHECK(mo_arena_allocate(&arena, 100, 4) == NULL);
    CHECK(arena.used == used);
    mo_arena_free(&arena);

    /* A block bigger than the limit is refused before anything is taken. */
    mo_arena_init(&arena, 4096);
    CHECK(mo_arena_allocate(&arena, 4097, 1) == NULL);
    CHECK(arena.used == 0 && arena.chunk == NULL);
    /* An arena with no room hands out nothing. */
    mo_arena_init(&arena, 0);
    CHECK(mo_arena_allocate(&arena, 1, 1) == NULL);
    CHECK(arena.used == 0);
}

static void test_one_header_of_room_is_enough(void) {
    /* An arena with a limit of n + MO_ARENA_CHUNK_HEADER holds n bytes in one piece, at any
     * alignment up to MO_ARENA_ALIGNMENT, and with the difference more at a larger one. */
    for (size_t alignment = 1; alignment <= 256; alignment *= 2) {
        size_t extra = alignment > MO_ARENA_ALIGNMENT ? alignment - MO_ARENA_ALIGNMENT : 0;
        for (size_t n = 1; n < 120000; n = n * 3 + 1) {
            struct mo_arena arena;
            mo_arena_init(&arena, n + MO_ARENA_CHUNK_HEADER + extra);
            unsigned char *block = mo_arena_allocate(&arena, n, alignment);
            CHECK(block != NULL && aligned(block, alignment));
            if (block != NULL) {
                memset(block, 0xAB, n);
            }
            CHECK(arena.used <= n + MO_ARENA_CHUNK_HEADER + extra);
            mo_arena_free(&arena);
        }
    }
}

static void test_arrays_that_overflow_are_refused(void) {
    struct mo_arena arena;
    mo_arena_init(&arena, 1 << 20);
    CHECK(mo_arena_allocate_array(&arena, SIZE_MAX / 2, 4, 4) == NULL);
    CHECK(mo_arena_allocate_array(&arena, 2, SIZE_MAX, 1) == NULL);
    CHECK(arena.used == 0);
    uint32_t *numbers = mo_arena_allocate_array(&arena, 100, sizeof(uint32_t), _Alignof(uint32_t));
    CHECK(numbers != NULL && aligned(numbers, _Alignof(uint32_t)));
    if (numbers != NULL) {
        numbers[99] = 7;
    }
    CHECK(mo_arena_allocate_array(&arena, 0, 8, 8) != NULL);
    mo_arena_free(&arena);
}

static bool holds_pattern(const unsigned char *block, size_t size) {
    for (size_t i = 0; i < size; i++) {
        if (block[i] != (unsigned char)(i * 7 + 1)) {
            return false;
        }
    }
    return true;
}

static void fill_pattern(unsigned char *block, size_t size) {
    for (size_t i = 0; i < size; i++) {
        block[i] = (unsigned char)(i * 7 + 1);
    }
}

static void test_the_newest_block_grows_in_place(void) {
    struct mo_arena arena;
    mo_arena_init(&arena, 1 << 20);
    unsigned char *block = mo_arena_allocate(&arena, 100, 1);
    CHECK(block != NULL);
    if (block == NULL) {
        return;
    }
    fill_pattern(block, 100);
    unsigned char *grown = mo_arena_resize(&arena, block, 100, 1000, 1);
    CHECK(grown == block);
    CHECK(holds_pattern(block, 100));
    fill_pattern(block, 1000);
    /* Shrinking keeps the block, and gives the rest back to the next block. */
    unsigned char *shrunk = mo_arena_resize(&arena, block, 1000, 10, 1);
    CHECK(shrunk == block && holds_pattern(block, 10));
    unsigned char *next = mo_arena_allocate(&arena, 1, 1);
    CHECK(next == block + 10);
    mo_arena_free(&arena);
}

static void test_a_block_alone_in_its_chunk_grows_with_it(void) {
    /* The arena's only block doubles past any chunk's size, keeping its bytes, and the arena
     * takes no more than the block and a header. */
    struct mo_arena arena;
    size_t most = (size_t)1 << 23;
    mo_arena_init(&arena, most + MO_ARENA_CHUNK_HEADER);
    size_t size = 64;
    unsigned char *block = mo_arena_allocate(&arena, size, 1);
    CHECK(block != NULL);
    if (block == NULL) {
        return;
    }
    fill_pattern(block, size);
    while (size < most) {
        unsigned char *grown = mo_arena_resize(&arena, block, size, size * 2, 1);
        CHECK(grown != NULL);
        if (grown == NULL) {
            break;
        }
        CHECK(holds_pattern(grown, size));
        block = grown;
        size *= 2;
        fill_pattern(block, size);
        CHECK(arena.used <= size + MO_ARENA_CHUNK_HEADER || arena.used <= 4096);
    }
    CHECK(size == most);
    /* One byte more than the limit allows is refused, and the block stays as it was. */
    CHECK(mo_arena_resize(&arena, block, size, size + 1, 1) == NULL);
    CHECK(holds_pattern(block, size));
    mo_arena_free(&arena);
}

static void test_an_older_block_moves(void) {
    struct mo_arena arena;
    mo_arena_init(&arena, 1 << 20);
    unsigned char *older = mo_arena_allocate(&arena, 50, 1);
    unsigned char *newer = mo_arena_allocate(&arena, 50, 1);
    CHECK(older != NULL && newer != NULL);
    if (older == NULL || newer == NULL) {
        return;
    }
    fill_pattern(older, 50);
    memset(newer, 0x5A, 50);
    unsigned char *moved = mo_arena_resize(&arena, older, 50, 500, 1);
    CHECK(moved != NULL && moved != older);
    CHECK(moved != NULL && holds_pattern(moved, 50));
    bool newer_intact = true;
    for (size_t i = 0; i < 50; i++) {
        newer_intact = newer_intact && newer[i] == 0x5A;
    }
    CHECK(newer_intact);
    /* An older block shrinks where it is. */
    CHECK(mo_arena_resize(&arena, newer, 50, 20, 1) == newer);
    /* No block yet: resizing allocates. */
    unsigned char *fresh = mo_arena_resize(&arena, NULL, 0, 30, 8);
    CHECK(fresh != NULL && aligned(fresh, 8));
    mo_arena_free(&arena);
}

static void test_a_failed_resize_keeps_the_block(void) {
    struct mo_arena arena;
    mo_arena_init(&arena, 6000);
    unsigned char *first = mo_arena_allocate(&arena, 1000, 1);
    unsigned char *second = mo_arena_allocate(&arena, 1000, 1);
    CHECK(first != NULL && second != NULL);
    if (first == NULL || second == NULL) {
        return;
    }
    fill_pattern(first, 1000);
    size_t used = arena.used;
    CHECK(mo_arena_resize(&arena, first, 1000, 100000, 1) == NULL);
    CHECK(arena.used == used);
    CHECK(holds_pattern(first, 1000));
    mo_arena_free(&arena);
}

static void test_copies(void) {
    struct mo_arena arena;
    mo_arena_init(&arena, 1 << 16);
    char text[] = "a failure is ignored";
    char *copy = mo_arena_copy_text(&arena, text);
    CHECK(copy != NULL && copy != text);
    text[0] = 'X';
    CHECK(copy != NULL && strcmp(copy, "a failure is ignored") == 0);
    const char empty[] = "";
    char *empty_copy = mo_arena_copy_text(&arena, empty);
    CHECK(empty_copy != NULL && empty_copy[0] == '\0');
    const uint64_t numbers[] = {1, 2, 3};
    uint64_t *numbers_copy = mo_arena_copy(&arena, numbers, sizeof numbers, _Alignof(uint64_t));
    CHECK(numbers_copy != NULL && aligned(numbers_copy, _Alignof(uint64_t)));
    CHECK(numbers_copy != NULL && memcmp(numbers_copy, numbers, sizeof numbers) == 0);
    CHECK(mo_arena_copy(&arena, NULL, 0, 1) != NULL);
    mo_arena_free(&arena);
}

int main(void) {
    test_blocks_are_aligned_and_apart();
    test_a_size_of_zero_gives_distinct_blocks();
    test_the_limit_holds();
    test_one_header_of_room_is_enough();
    test_arrays_that_overflow_are_refused();
    test_the_newest_block_grows_in_place();
    test_a_block_alone_in_its_chunk_grows_with_it();
    test_an_older_block_moves();
    test_a_failed_resize_keeps_the_block();
    test_copies();
    return mo_test_report("test_arena");
}
