#define _POSIX_C_SOURCE 200809L
#include "codegen/codegen.h"
#include "prim_oracle.h"
#include "test.h"
#include "vm/vm.h"

#include <fenv.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void shared_text_history(void) {
    struct prim_fixture f;
    prim_fixture_init(&f, 1 << 20);
    uint32_t type = f.builtin[MO_TYPE_TEXT];
    struct mo_cell value, piece, history[48];
    MO_ASSERT(mo_value_text(f.heap, "a", 1, &value) && mo_value_text(f.heap, "a", 1, &piece));
    uint32_t count = 0;
    while (count < 48) {
        history[count++] = mo_value_dup(f.heap, type, value);
        bool ok = mo_text_concat(f.heap, &value, piece);
        CHECK(ok);
        if (!ok)
            break;
    }
    CHECK(count == 48 && mo_heap_used(f.heap) < (128 << 10));
    for (uint32_t i = 0; i < count; i++) {
        size_t length;
        const uint8_t *bytes = mo_value_data(history[i], &length);
        CHECK(length == (size_t)i + 1);
        for (size_t j = 0; j < length; j++)
            CHECK(bytes[j] == 'a');
        mo_value_drop(f.heap, type, history[i]);
    }
    mo_value_drop(f.heap, type, value);
    mo_value_drop(f.heap, type, piece);
    CHECK(mo_heap_objects(f.heap) == 0);
    prim_fixture_free(&f);
}

static void shared_copy_charge(void) {
    struct prim_fixture f;
    prim_fixture_init_type(&f, 1 << 20, MO_TYPE_TEXT);
    uint32_t text = f.builtin[MO_TYPE_TEXT];
    uint32_t list = f.types.items[f.signature[MO_PRIM_LIST_CONCAT]].result;
    struct mo_cell value, piece, held, exact;
    MO_ASSERT(mo_value_text(f.heap, "β🙂", 6, &piece));
    MO_ASSERT(mo_value_text(f.heap, "β🙂", 6, &value));
    MO_ASSERT(mo_text_concat(f.heap, &value, piece));
    held = mo_value_dup(f.heap, text, value);
    size_t before = mo_heap_used(f.heap);
    MO_ASSERT(mo_text_concat(f.heap, &value, piece));
    size_t copy_charge = mo_heap_used(f.heap) - before;
    before = mo_heap_used(f.heap);
    MO_ASSERT(mo_value_text(f.heap, "β🙂β🙂β🙂", 18, &exact));
    CHECK(copy_charge == mo_heap_used(f.heap) - before);
    size_t length;
    const uint8_t *bytes = mo_value_data(value, &length);
    CHECK(length == 18 && memcmp(bytes, "β🙂β🙂β🙂", 18) == 0);
    bytes = mo_value_data(held, &length);
    CHECK(length == 12 && memcmp(bytes, "β🙂β🙂", 12) == 0);
    mo_value_drop(f.heap, text, held);
    mo_value_drop(f.heap, text, value);
    mo_value_drop(f.heap, text, exact);

    struct mo_cell elements[10];
    for (unsigned i = 0; i < 10; i++)
        elements[i] = piece;
    MO_ASSERT(mo_value_list(f.heap, list, elements, 8, &value));
    MO_ASSERT(mo_list_append(f.heap, list, &value, piece));
    held = mo_value_dup(f.heap, list, value);
    before = mo_heap_used(f.heap);
    MO_ASSERT(mo_list_append(f.heap, list, &value, piece));
    copy_charge = mo_heap_used(f.heap) - before;
    before = mo_heap_used(f.heap);
    MO_ASSERT(mo_value_list(f.heap, list, elements, 10, &exact));
    CHECK(copy_charge == mo_heap_used(f.heap) - before);
    CHECK(mo_list_size(held) == 9 && mo_list_size(value) == 10);
    mo_value_drop(f.heap, list, exact);
    mo_value_drop(f.heap, list, value);
    mo_value_drop(f.heap, list, held);
    CHECK(mo_cell_get_ref(piece)->count == 1);
    mo_value_drop(f.heap, text, piece);
    CHECK(mo_heap_objects(f.heap) == 0);
    prim_fixture_free(&f);
}

static void unique_contraction(void) {
    for (unsigned full = 0; full < 2; full++) {
        for (unsigned kind = 0; kind < 3; kind++) {
            struct prim_fixture f;
            prim_fixture_init_type(&f, 1 << 20, MO_TYPE_TEXT);
            uint32_t text = f.builtin[MO_TYPE_TEXT];
            uint32_t list = f.types.items[f.signature[MO_PRIM_LIST_CONCAT]].result;
            struct mo_cell value, piece;
            MO_ASSERT(mo_value_text(f.heap, "β", 2, &piece));
            if (kind == 0) {
                char bytes[4096];
                memset(bytes, 'a', sizeof bytes);
                memcpy(bytes + 32, "β", 2);
                MO_ASSERT(mo_value_text(f.heap, bytes, sizeof bytes, &value));
            } else {
                struct mo_cell elements[256];
                for (unsigned i = 0; i < 256; i++)
                    elements[i] = piece;
                MO_ASSERT(mo_value_list(f.heap, list, elements, 256, &value));
            }
            size_t before = mo_heap_used(f.heap);
            struct mo_scratch scratch;
            mo_scratch_init(&scratch, f.heap);
            if (full != 0)
                MO_ASSERT(mo_scratch_resize(&scratch, NULL, 0,
                                            scratch.arena.limit - MO_ARENA_CHUNK_HEADER) != NULL);
            size_t filled = mo_heap_used(f.heap);
            if (kind == 0) {
                MO_ASSERT(mo_text_slice(f.heap, &value, 32, 2));
                size_t length;
                const uint8_t *bytes = mo_value_data(value, &length);
                CHECK(length == 2 && memcmp(bytes, "β", 2) == 0);
            } else {
                if (kind == 1)
                    MO_ASSERT(mo_list_slice(f.heap, list, &value, 17, 1));
                else
                    while (mo_list_size(value) > 1)
                        MO_ASSERT(mo_list_remove_at(f.heap, list, &value, 0));
                struct mo_cell item;
                MO_ASSERT(mo_list_get(value, 0, &item));
                CHECK(mo_cell_get_ref(item) == mo_cell_get_ref(piece) &&
                      mo_cell_get_ref(piece)->count == 2);
            }
            if (full != 0)
                CHECK(mo_heap_used(f.heap) == filled);
            else
                CHECK(mo_heap_used(f.heap) + 3000 < before);
            mo_scratch_free(&scratch);
            mo_value_drop(f.heap, kind == 0 ? text : list, value);
            CHECK(mo_cell_get_ref(piece)->count == 1);
            mo_value_drop(f.heap, text, piece);
            CHECK(mo_heap_objects(f.heap) == 0);
            prim_fixture_free(&f);
        }
    }
}

static void growth_near_limit(void) {
    struct prim_fixture f;
    prim_fixture_init(&f, 4 << 20);
    char *bytes = malloc(3 << 19);
    MO_ASSERT(bytes != NULL);
    memset(bytes, 'p', 3 << 19);
    struct mo_cell filler, value, piece;
    MO_ASSERT(mo_value_bytes(f.heap, (const uint8_t *)bytes, 3 << 19, &filler));
    MO_ASSERT(mo_value_text(f.heap, bytes, 1 << 20, &value));
    MO_ASSERT(mo_value_text(f.heap, "a", 1, &piece));
    free(bytes);
    MO_ASSERT(mo_text_concat(f.heap, &value, piece));
    size_t grown = mo_heap_used(f.heap);
    for (unsigned i = 1; i < 2000; i++)
        MO_ASSERT(mo_text_concat(f.heap, &value, piece));
    CHECK(mo_heap_used(f.heap) == grown);
    size_t length;
    const uint8_t *result = mo_value_data(value, &length);
    CHECK(length == (1 << 20) + 2000);
    for (size_t i = 0; i < length; i++)
        CHECK(result[i] == (i < (1 << 20) ? 'p' : 'a'));
    mo_value_drop(f.heap, f.builtin[MO_TYPE_BYTES], filler);
    mo_value_drop(f.heap, f.builtin[MO_TYPE_TEXT], value);
    mo_value_drop(f.heap, f.builtin[MO_TYPE_TEXT], piece);
    CHECK(mo_heap_objects(f.heap) == 0);
    prim_fixture_free(&f);
}

static void unchanged_trim(void) {
    struct prim_fixture f;
    prim_fixture_init(&f, 1 << 20);
    struct mo_cell arg, out;
    MO_ASSERT(mo_value_text(f.heap, "β🙂", 6, &arg));
    struct mo_cell held = mo_value_dup(f.heap, f.builtin[MO_TYPE_TEXT], arg);
    const char *reason;
    MO_ASSERT(mo_prim_call(f.heap, &f.types, MO_PRIM_TEXT_TRIM, f.signature[MO_PRIM_TEXT_TRIM],
                           &arg, &out, &reason) == MO_PRIM_RETURNED);
    CHECK(mo_cell_get_ref(out) == mo_cell_get_ref(held));
    size_t length;
    const uint8_t *bytes = mo_value_data(out, &length);
    CHECK(length == 6 && memcmp(bytes, "β🙂", 6) == 0);
    mo_value_drop(f.heap, f.builtin[MO_TYPE_TEXT], held);
    mo_value_drop(f.heap, f.builtin[MO_TYPE_TEXT], out);
    prim_fixture_free(&f);
}

static void unique_reverse_without_space(void) {
    struct prim_fixture f;
    prim_fixture_init(&f, 1 << 20);
    uint32_t type = f.types.items[f.signature[MO_PRIM_LIST_REVERSE]].result;
    struct mo_cell elements[] = {mo_cell_int(3), mo_cell_int(-7), mo_cell_int(11)};
    struct mo_cell arg, out = mo_cell_int(41);
    MO_ASSERT(mo_value_list(f.heap, type, elements, 3, &arg));
    struct mo_scratch full;
    mo_scratch_init(&full, f.heap);
    MO_ASSERT(mo_scratch_resize(&full, NULL, 0, full.arena.limit - MO_ARENA_CHUNK_HEADER) != NULL);
    const char *reason;
    enum mo_prim_status status =
        mo_prim_call(f.heap, &f.types, MO_PRIM_LIST_REVERSE, f.signature[MO_PRIM_LIST_REVERSE],
                     &arg, &out, &reason);
    CHECK(status == MO_PRIM_RETURNED);
    if (status == MO_PRIM_RETURNED) {
        struct mo_cell first, last;
        MO_ASSERT(mo_list_get(out, 0, &first) && mo_list_get(out, 2, &last));
        CHECK(mo_cell_get_int(first) == 11 && mo_cell_get_int(last) == 3);
        mo_value_drop(f.heap, type, out);
    } else
        mo_value_drop(f.heap, type, arg);
    mo_scratch_free(&full);
    prim_fixture_free(&f);
}

static void unique_union_without_space(void) {
    struct prim_fixture f;
    prim_fixture_init(&f, 1 << 20);
    uint32_t type = f.types.items[f.signature[MO_PRIM_SET_UNION]].result;
    struct mo_cell keys[] = {mo_cell_int(11), mo_cell_int(-7), mo_cell_int(3)};
    struct mo_cell args[2], out = mo_cell_int(41);
    MO_ASSERT(mo_value_set(f.heap, type, keys, 2, &args[0]));
    MO_ASSERT(mo_value_set(f.heap, type, keys + 2, 1, &args[1]));
    struct mo_scratch full;
    mo_scratch_init(&full, f.heap);
    MO_ASSERT(mo_scratch_resize(&full, NULL, 0, full.arena.limit - MO_ARENA_CHUNK_HEADER) != NULL);
    const char *reason;
    enum mo_prim_status status = mo_prim_call(f.heap, &f.types, MO_PRIM_SET_UNION,
                                              f.signature[MO_PRIM_SET_UNION], args, &out, &reason);
    CHECK(status == MO_PRIM_RETURNED);
    if (status == MO_PRIM_RETURNED) {
        uint32_t cursor = 0;
        struct mo_cell key, value;
        CHECK(mo_table_size(out) == 3);
        for (unsigned i = 0; i < 3; i++) {
            MO_ASSERT(mo_table_next(out, &cursor, &key, &value));
            CHECK(mo_cell_get_int(key) == mo_cell_get_int(keys[i]));
        }
        mo_value_drop(f.heap, type, out);
    } else
        mo_value_drop(f.heap, type, args[0]);
    mo_value_drop(f.heap, type, args[1]);
    mo_scratch_free(&full);
    prim_fixture_free(&f);
}

/* Sweep available heap space across planning/copy boundaries. No duplicate is
 * kept in the unique mode: serialization retains observations, not ownership. */
static void reuse_refusals(void) {
    const enum mo_primitive_id ids[] = {
        MO_PRIM_TEXT_CONCAT,   MO_PRIM_TEXT_TRIM,        MO_PRIM_TEXT_TRIM_START,
        MO_PRIM_TEXT_TRIM_END, MO_PRIM_TEXT_DROP_PREFIX, MO_PRIM_TEXT_DROP_SUFFIX,
        MO_PRIM_LIST_PREPEND,  MO_PRIM_LIST_CONCAT,      MO_PRIM_LIST_REVERSE,
        MO_PRIM_LIST_TAKE,     MO_PRIM_LIST_DROP,        MO_PRIM_LIST_REMOVE_AT,
        MO_PRIM_SET_UNION};
    static const char *const ownership[] = {
#define PRIM(id, module, name, signature, params, bugs) params,
#include "base/prims.def"
    };
    unsigned refusals = 0, successes = 0;
    for (size_t operation = 0; operation < sizeof ids / sizeof ids[0]; operation++) {
        enum mo_primitive_id id = ids[operation];
        for (unsigned holders = 0; holders < 3; holders++) {
            for (size_t room = 0; room <= 8192; room += 128) {
                struct prim_fixture f;
                prim_fixture_init_type(&f, 1 << 20, MO_TYPE_TEXT);
                const struct mo_type *fn = &f.types.items[f.signature[id]];
                size_t count = fn->parameter_count;
                struct mo_cell args[3], held[3], out = mo_cell_int(41);
                struct mo_cell elements[7];
                const char *const words[] = {"β", "🙂", "x", "z", "u", "v", "w"};
                for (unsigned i = 0; i < 7; i++)
                    MO_ASSERT(mo_value_text(f.heap, words[i], strlen(words[i]), &elements[i]));
                for (size_t i = 0; i < count; i++) {
                    uint32_t type = fn->parameters[i];
                    enum mo_type_kind kind = f.types.items[type].kind;
                    if (kind == MO_TYPE_TEXT) {
                        const char *s = i == 0                           ? " \tβ🙂 \n"
                                        : id == MO_PRIM_TEXT_DROP_PREFIX ? " \t"
                                        : id == MO_PRIM_TEXT_DROP_SUFFIX ? " \n"
                                                                         : "β";
                        MO_ASSERT(mo_value_text(f.heap, s, strlen(s), &args[i]));
                    } else if (kind == MO_TYPE_INT)
                        args[i] = mo_cell_int(1);
                    else if (kind == MO_TYPE_LIST) {
                        uint32_t n = i == 0 ? 3U : 2U;
                        MO_ASSERT(mo_value_list(f.heap, type, elements + i, n, &args[i]));
                        /* Preserve contents with a nonzero live start. */
                        MO_ASSERT(mo_list_concat(f.heap, type, &args[i], args[i]) &&
                                  mo_list_slice(f.heap, type, &args[i], n, n));
                    } else {
                        MO_ASSERT(kind == MO_TYPE_SET);
                        /* Full left storage plus three new keys forces growth,
                         * and exercises refusals after the planning allocation. */
                        MO_ASSERT(mo_value_set(f.heap, type, elements + (i == 0 ? 0 : 4),
                                               i == 0 ? 4U : 3U, &args[i]));
                    }
                    if (holders == 1)
                        held[i] = mo_value_dup(f.heap, type, args[i]);
                    else if (holders == 2)
                        MO_ASSERT(mo_value_make_immortal(f.heap, type, args[i]));
                }
                struct mo_buffer before[3];
                uint32_t counts[3] = {0};
                for (size_t i = 0; i < count; i++) {
                    mo_buffer_init(&before[i], 4096);
                    MO_ASSERT(mo_value_print(f.heap, fn->parameters[i], args[i], &before[i]));
                    if (mo_type_is_reference(&f.types, fn->parameters[i]))
                        counts[i] = mo_cell_get_ref(args[i])->count;
                }
                struct mo_scratch full;
                mo_scratch_init(&full, f.heap);
                MO_ASSERT(mo_scratch_resize(&full, NULL, 0,
                                            full.arena.limit - MO_ARENA_CHUNK_HEADER - room) !=
                          NULL);
                size_t used = mo_heap_used(f.heap), objects = mo_heap_objects(f.heap);
                const char *reason;
                enum mo_prim_status status =
                    mo_prim_call(f.heap, &f.types, id, f.signature[id], args, &out, &reason);
                CHECK(status == MO_PRIM_RETURNED || status == MO_PRIM_OUT_OF_MEMORY);
                if (status == MO_PRIM_OUT_OF_MEMORY) {
                    refusals++;
                    CHECK(mo_cell_get_int(out) == 41 && reason != NULL);
                    CHECK(mo_heap_used(f.heap) == used && mo_heap_objects(f.heap) == objects);
                } else
                    successes++;
                mo_scratch_free(&full);
                for (size_t i = 0; i < count; i++) {
                    uint32_t type = fn->parameters[i];
                    bool unchanged = status != MO_PRIM_RETURNED || ownership[id][i] == 'b';
                    if (unchanged || holders != 0) {
                        struct mo_cell original = holders == 1 ? held[i] : args[i];
                        struct mo_buffer after;
                        mo_buffer_init(&after, 4096);
                        MO_ASSERT(mo_value_print(f.heap, type, original, &after));
                        CHECK(before[i].length == after.length &&
                              memcmp(before[i].bytes, after.bytes, after.length) == 0);
                        if (status != MO_PRIM_RETURNED && mo_type_is_reference(&f.types, type))
                            CHECK(mo_cell_get_ref(original)->count == counts[i]);
                        mo_buffer_free(&after);
                    }
                    if (unchanged)
                        mo_value_drop(f.heap, type, args[i]);
                    if (holders == 1)
                        mo_value_drop(f.heap, type, held[i]);
                    mo_buffer_free(&before[i]);
                }
                if (status == MO_PRIM_RETURNED)
                    mo_value_drop(f.heap, fn->result, out);
                for (unsigned i = 0; i < 7; i++)
                    mo_value_drop(f.heap, f.builtin[MO_TYPE_TEXT], elements[i]);
                CHECK(holders == 2 || mo_heap_objects(f.heap) == 0);
                prim_fixture_free(&f);
            }
        }
    }
    CHECK(refusals != 0 && successes != 0);
}

static void reuse_aliases(void) {
    for (unsigned kind = 0; kind < 3; kind++) {
        for (unsigned holders = 0; holders < 3; holders++) {
            struct prim_fixture f;
            prim_fixture_init_type(&f, 1 << 20, MO_TYPE_TEXT);
            uint32_t text = f.builtin[MO_TYPE_TEXT];
            uint32_t type =
                kind == 0
                    ? text
                    : f.types
                          .items[f.signature[kind == 1 ? MO_PRIM_LIST_CONCAT : MO_PRIM_SET_UNION]]
                          .result;
            struct mo_cell a, b, value, expected;
            MO_ASSERT(mo_value_text(f.heap, "β", 2, &a) && mo_value_text(f.heap, "🙂", 4, &b));
            struct mo_cell elements[] = {a, b, a, b};
            if (kind == 0) {
                value = mo_value_dup(f.heap, text, a);
                MO_ASSERT(mo_text_concat(f.heap, &value, b));
                MO_ASSERT(mo_value_text(f.heap, "β🙂β🙂", 12, &expected));
            } else if (kind == 1) {
                MO_ASSERT(mo_value_list(f.heap, type, elements, 2, &value));
                /* Leave enough capacity for an in-place self-concatenation. */
                MO_ASSERT(mo_list_append(f.heap, type, &value, a) &&
                          mo_list_slice(f.heap, type, &value, 0, 2));
                MO_ASSERT(mo_value_list(f.heap, type, elements, 4, &expected));
            } else {
                MO_ASSERT(mo_value_set(f.heap, type, elements, 2, &value) &&
                          mo_value_set(f.heap, type, elements, 2, &expected));
            }
            uint64_t hash;
            MO_ASSERT(mo_value_hash(f.heap, type, value, &hash));
            struct mo_cell held = mo_cell_unit();
            if (holders == 1)
                held = mo_value_dup(f.heap, type, value);
            else if (holders == 2)
                MO_ASSERT(mo_value_make_immortal(f.heap, type, value));
            struct mo_cell original = value;
            bool ok = kind == 0   ? mo_text_concat(f.heap, &value, value)
                      : kind == 1 ? mo_list_concat(f.heap, type, &value, value)
                                  : mo_set_union(f.heap, type, &value, value);
            MO_ASSERT(ok);
            bool equal;
            uint64_t expected_hash;
            MO_ASSERT(mo_value_equal(f.heap, type, value, expected, &equal) &&
                      mo_value_hash(f.heap, type, value, &hash) &&
                      mo_value_hash(f.heap, type, expected, &expected_hash));
            CHECK(equal && hash == expected_hash);
            if (holders != 0) {
                if (kind == 0) {
                    size_t length;
                    const uint8_t *bytes = mo_value_data(original, &length);
                    CHECK(length == 6 && memcmp(bytes, "β🙂", 6) == 0);
                } else
                    CHECK(kind == 1 ? mo_list_size(original) == 2 : mo_table_size(original) == 2);
            }
            if (holders == 1)
                mo_value_drop(f.heap, type, held);
            mo_value_drop(f.heap, type, value);
            mo_value_drop(f.heap, type, expected);
            mo_value_drop(f.heap, text, a);
            mo_value_drop(f.heap, text, b);
            CHECK(holders == 2 || mo_heap_objects(f.heap) == 0);
            prim_fixture_free(&f);
        }
    }
}

static void runtime_sizes(void) {
    struct prim_fixture f;
    prim_fixture_init(&f, MO_DEFAULT_HEAP_SIZE);
    struct mo_cell args[2], out = mo_cell_int(41);
    const char *reason;
    MO_ASSERT(mo_value_text(f.heap, "x", 1, &args[0]));
    args[1] = mo_cell_int(9000000);
    MO_ASSERT(mo_prim_call(f.heap, &f.types, MO_PRIM_TEXT_REPEAT, f.signature[MO_PRIM_TEXT_REPEAT],
                           args, &out, &reason) == MO_PRIM_RETURNED);
    size_t length;
    const uint8_t *bytes = mo_value_data(out, &length);
    CHECK(length == 9000000);
    for (size_t i = 0; i < length; i++)
        CHECK(bytes[i] == 'x');
    args[0] = out;
    args[1] = out;
    MO_ASSERT(mo_prim_call(f.heap, &f.types, MO_PRIM_TEXT_CONCAT, f.signature[MO_PRIM_TEXT_CONCAT],
                           args, &out, &reason) == MO_PRIM_RETURNED);
    bytes = mo_value_data(out, &length);
    CHECK(length == 18000000 && bytes[0] == 'x' && bytes[length - 1] == 'x');
    mo_value_drop(f.heap, f.builtin[MO_TYPE_TEXT], out);

    MO_ASSERT(mo_value_text(f.heap, "x", 1, &args[0]));
    args[1] = mo_cell_int((int64_t)MO_LIMIT_VALUE_BYTES + 1);
    out = mo_cell_int(41);
    CHECK(mo_prim_call(f.heap, &f.types, MO_PRIM_TEXT_REPEAT, f.signature[MO_PRIM_TEXT_REPEAT],
                       args, &out, &reason) == MO_PRIM_OUT_OF_MEMORY);
    CHECK(strstr(reason, "MO_LIMIT_VALUE_BYTES") != NULL && mo_cell_get_int(out) == 41);
    mo_value_drop(f.heap, f.builtin[MO_TYPE_TEXT], args[0]);
    prim_fixture_free(&f);
}

/* Independent caller can compare every emitted digit with its own big-number
 * model. The input is 256^n - 1, with no decimal parser in the measurement. */
static void bigint_text_work(size_t count) {
    struct prim_fixture f;
    prim_fixture_init(&f, MO_DEFAULT_HEAP_SIZE);
    struct mo_arena arena;
    mo_arena_init(&arena, count + MO_ARENA_CHUNK_HEADER);
    uint8_t *magnitude = mo_arena_allocate(&arena, count, 1);
    MO_ASSERT(magnitude != NULL);
    memset(magnitude, 255, count);
    struct mo_cell arg, out;
    MO_ASSERT(mo_value_bigint(f.heap, MO_BIGINT_POSITIVE, magnitude, count, &arg));
    const char *reason;
    struct timespec start, end;
    MO_ASSERT(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
    MO_ASSERT(mo_prim_call(f.heap, &f.types, MO_PRIM_BIGINT_TO_TEXT,
                           f.signature[MO_PRIM_BIGINT_TO_TEXT], &arg, &out,
                           &reason) == MO_PRIM_RETURNED);
    MO_ASSERT(clock_gettime(CLOCK_MONOTONIC, &end) == 0);
    size_t length;
    const uint8_t *text = mo_value_data(out, &length);
    double seconds =
        (double)(end.tv_sec - start.tv_sec) + (double)(end.tv_nsec - start.tv_nsec) / 1e9;
    MO_ASSERT(fprintf(stderr, "%zu digits: %.6f s\n", length, seconds) > 0);
    MO_ASSERT(fwrite(text, 1, length, stdout) == length && fflush(stdout) == 0);
    mo_value_drop(f.heap, f.builtin[MO_TYPE_TEXT], out);
    mo_value_drop(f.heap, f.builtin[MO_TYPE_BIGINT], arg);
    mo_arena_free(&arena);
    prim_fixture_free(&f);
}

static void empty_constants_and_rounding(void) {
    struct prim_fixture f;
    prim_fixture_init(&f, 1 << 20);
    const enum mo_primitive_id ids[] = {MO_PRIM_LIST_EMPTY, MO_PRIM_MAP_EMPTY, MO_PRIM_SET_EMPTY};
    const char *reason;
    struct mo_cell out;
    for (size_t i = 0; i < sizeof ids / sizeof ids[0]; i++) {
        const struct mo_type *fn = &f.types.items[f.signature[ids[i]]];
        MO_ASSERT(fn->kind == MO_TYPE_FUNCTION && fn->parameter_count == 0);
        CHECK(mo_prim_call(f.heap, &f.types, ids[i], f.signature[ids[i]], NULL, &out, &reason) ==
              MO_PRIM_RETURNED);
        CHECK(reason == NULL);
        CHECK(ids[i] == MO_PRIM_LIST_EMPTY ? mo_list_size(out) == 0 : mo_table_size(out) == 0);
        mo_value_drop(f.heap, fn->result, out);
    }
    MO_ASSERT(fesetround(FE_UPWARD) == 0);
    struct mo_cell arg = mo_cell_float(2.0);
    MO_ASSERT(mo_prim_call(f.heap, &f.types, MO_PRIM_FLOAT_SQRT, f.signature[MO_PRIM_FLOAT_SQRT],
                           &arg, &out, &reason) == MO_PRIM_RETURNED);
    CHECK(fegetround() == FE_TONEAREST && mo_cell_get_float(out) == sqrt(2.0));
    prim_fixture_free(&f);
}

static void integer_power(void) {
    struct mo_arena arena;
    struct mo_types types;
    mo_arena_init(&arena, 1 << 20);
    mo_types_init(&types, &arena);
    uint32_t integer, signature;
    struct mo_type t = {.kind = MO_TYPE_INT};
    MO_ASSERT(mo_types_intern(&types, &t, &integer) == MO_TYPES_OK);
    uint32_t params[] = {integer, integer};
    t = (struct mo_type){
        .kind = MO_TYPE_FUNCTION, .parameters = params, .parameter_count = 2, .result = integer};
    MO_ASSERT(mo_types_intern(&types, &t, &signature) == MO_TYPES_OK);
    MO_ASSERT(mo_types_finish(&types) == MO_TYPES_OK);
    struct mo_limits limits = mo_limits_default();
    struct mo_heap_image *image = mo_heap_image_new(&types, &limits);
    MO_ASSERT(image != NULL);
    struct mo_heap *heap = mo_heap_new(image, &limits);
    MO_ASSERT(heap != NULL);
    struct mo_cell args[] = {mo_cell_int(-2), mo_cell_int(63)};
    struct mo_cell out = mo_cell_int(41);
    const char *reason = NULL;
    CHECK(mo_prim_call(heap, &types, MO_PRIM_INT_POW, signature, args, &out, &reason) ==
          MO_PRIM_RETURNED);
    CHECK(mo_cell_get_int(out) == INT64_MIN);
    args[0] = mo_cell_int(2);
    out = mo_cell_int(41);
    struct mo_cell before[2];
    memcpy(before, args, sizeof before);
    CHECK(mo_prim_call(heap, &types, MO_PRIM_INT_POW, signature, args, &out, &reason) ==
          MO_PRIM_OVERFLOW);
    CHECK(reason != NULL && strcmp(reason, "Int") == 0);
    CHECK(mo_cell_get_int(out) == 41 && memcmp(args, before, sizeof before) == 0);
    mo_heap_free(heap);
    mo_heap_image_free(image);
    mo_arena_free(&arena);
}

static void integer_floor(void) {
    struct prim_fixture f;
    prim_fixture_init(&f, 1 << 20);
    struct mo_cell args[] = {mo_cell_int(7), mo_cell_int(-3)}, out = mo_cell_int(41);
    const char *reason = NULL;
    CHECK(mo_prim_call(f.heap, &f.types, MO_PRIM_INT_FLOOR_DIV, f.signature[MO_PRIM_INT_FLOOR_DIV],
                       args, &out, &reason) == MO_PRIM_RETURNED);
    CHECK(mo_cell_get_int(out) == -3);
    CHECK(mo_prim_call(f.heap, &f.types, MO_PRIM_INT_FLOOR_MOD, f.signature[MO_PRIM_INT_FLOOR_MOD],
                       args, &out, &reason) == MO_PRIM_RETURNED);
    CHECK(mo_cell_get_int(out) == -2);
    args[0] = mo_cell_int(INT64_MIN);
    args[1] = mo_cell_int(-1);
    CHECK(mo_prim_call(f.heap, &f.types, MO_PRIM_INT_FLOOR_MOD, f.signature[MO_PRIM_INT_FLOOR_MOD],
                       args, &out, &reason) == MO_PRIM_RETURNED);
    CHECK(mo_cell_get_int(out) == 0);
    CHECK(mo_prim_call(f.heap, &f.types, MO_PRIM_INT_FLOOR_DIV, f.signature[MO_PRIM_INT_FLOOR_DIV],
                       args, &out, &reason) == MO_PRIM_OVERFLOW);
    CHECK(strcmp(reason, "Int") == 0 && mo_cell_get_int(out) == 0);
    args[1] = mo_cell_int(0);
    CHECK(mo_prim_call(f.heap, &f.types, MO_PRIM_INT_FLOOR_DIV, f.signature[MO_PRIM_INT_FLOOR_DIV],
                       args, &out, &reason) == MO_PRIM_DIV_BY_ZERO);
    CHECK(strcmp(reason, "dividing by zero") == 0 && mo_cell_get_int(out) == 0);
    prim_fixture_free(&f);
}

static void decimal_rounding(void) {
    struct prim_fixture f;
    prim_fixture_init(&f, 1 << 20);
    struct mo_cell args[] = {mo_cell_decimal(INT64_C(-50000000000000000)), mo_cell_int(1)};
    struct mo_cell out = mo_cell_int(41);
    const char *reason = NULL;
    CHECK(mo_prim_call(f.heap, &f.types, MO_PRIM_DECIMAL_ROUND, f.signature[MO_PRIM_DECIMAL_ROUND],
                       args, &out, &reason) == MO_PRIM_RETURNED);
    CHECK(mo_cell_get_decimal(out) == INT64_C(-100000000000000000));
    args[0] = mo_cell_decimal((mo_i128)(((mo_u128)1 << 127) - 1));
    args[1] = mo_cell_int(0);
    out = mo_cell_int(41);
    CHECK(mo_prim_call(f.heap, &f.types, MO_PRIM_DECIMAL_ROUND, f.signature[MO_PRIM_DECIMAL_ROUND],
                       args, &out, &reason) == MO_PRIM_OVERFLOW);
    CHECK(strcmp(reason, "Decimal") == 0 && mo_cell_get_int(out) == 41);
    CHECK(mo_prim_call(f.heap, &f.types, MO_PRIM_DECIMAL_TO_FIXED,
                       f.signature[MO_PRIM_DECIMAL_TO_FIXED], args, &out,
                       &reason) == MO_PRIM_RETURNED);
    size_t length;
    const uint8_t *bytes = mo_value_data(out, &length);
    CHECK(length == 21 && memcmp(bytes, "170141183460469231732", 21) == 0);
    mo_value_drop(f.heap, f.builtin[MO_TYPE_TEXT], out);
    prim_fixture_free(&f);
}

static void text_split(void) {
    struct prim_fixture f;
    prim_fixture_init(&f, 1 << 20);
    struct mo_cell args[2], out = mo_cell_int(41);
    CHECK(mo_value_text(f.heap, "ababa", 5, &args[0]));
    CHECK(mo_value_text(f.heap, "aba", 3, &args[1]));
    const char *reason;
    enum mo_prim_status status = mo_prim_call(f.heap, &f.types, MO_PRIM_TEXT_SPLIT,
                                              f.signature[MO_PRIM_TEXT_SPLIT], args, &out, &reason);
    CHECK(status == MO_PRIM_RETURNED);
    if (status == MO_PRIM_RETURNED) {
        struct mo_cell piece;
        size_t length;
        CHECK(mo_list_size(out) == 2);
        MO_ASSERT(mo_list_get(out, 0, &piece));
        const uint8_t *bytes = mo_value_data(piece, &length);
        CHECK(length == 0);
        CHECK(mo_list_get(out, 1, &piece));
        bytes = mo_value_data(piece, &length);
        CHECK(length == 2 && memcmp(bytes, "ba", 2) == 0);
        mo_value_drop(f.heap, f.types.items[f.signature[MO_PRIM_TEXT_SPLIT]].result, out);
    }
    mo_value_drop(f.heap, f.builtin[MO_TYPE_TEXT], args[0]);
    mo_value_drop(f.heap, f.builtin[MO_TYPE_TEXT], args[1]);
    prim_fixture_free(&f);
}

static void collection_ownership(void) {
    struct prim_fixture f;
    prim_fixture_init_type(&f, 1 << 20, MO_TYPE_TEXT);
    size_t baseline = mo_heap_used(f.heap);
    uint32_t type = f.types.items[f.signature[MO_PRIM_LIST_APPEND]].result;
    uint32_t text = f.builtin[MO_TYPE_TEXT];
    const char *reason;
    for (unsigned shared = 0; shared < 2; shared++) {
        struct mo_cell a, b, args[3], out;
        MO_ASSERT(mo_value_text(f.heap, "a", 1, &a));
        MO_ASSERT(mo_value_text(f.heap, "b", 1, &b));
        MO_ASSERT(mo_value_list(f.heap, type, &a, 1, &args[0]));
        mo_value_drop(f.heap, text, a);
        struct mo_cell retained = shared ? mo_value_dup(f.heap, type, args[0]) : mo_cell_unit();
        args[1] = b;
        MO_ASSERT(mo_prim_call(f.heap, &f.types, MO_PRIM_LIST_APPEND,
                               f.signature[MO_PRIM_LIST_APPEND], args, &out,
                               &reason) == MO_PRIM_RETURNED);
        CHECK(reason == NULL && mo_list_size(out) == 2);
        if (shared) {
            CHECK(mo_list_size(retained) == 1);
            mo_value_drop(f.heap, type, retained);
        }
        args[0] = out;
        args[1] = mo_cell_int(0);
        MO_ASSERT(mo_value_text(f.heap, "replacement", 11, &args[2]));
        MO_ASSERT(mo_prim_call(f.heap, &f.types, MO_PRIM_LIST_SET, f.signature[MO_PRIM_LIST_SET],
                               args, &out, &reason) == MO_PRIM_RETURNED);
        CHECK(mo_cell_get_tag(out) == 1);
        struct mo_cell value;
        MO_ASSERT(mo_list_get(mo_cell_get_payload(out), 0, &value));
        size_t length;
        const uint8_t *bytes = mo_value_data(value, &length);
        CHECK(length == 11 && memcmp(bytes, "replacement", 11) == 0);
        mo_value_drop(f.heap, f.types.items[f.signature[MO_PRIM_LIST_SET]].result, out);
        CHECK(mo_heap_objects(f.heap) == 0 && mo_heap_used(f.heap) == baseline);
    }
    prim_fixture_free(&f);
}

static void refusal_preserves_operands(void) {
    struct prim_fixture f;
    prim_fixture_init(&f, 16384);
    size_t baseline = mo_heap_used(f.heap);
    uint32_t map = f.types.items[f.signature[MO_PRIM_MAP_SET]].result;
    uint32_t text = f.builtin[MO_TYPE_TEXT];
    struct mo_cell args[3], out = mo_cell_int(41);
    const char *reason;
    MO_ASSERT(mo_value_map(f.heap, map, NULL, NULL, 0, &args[0]));
    args[1] = mo_cell_int(7);
    MO_ASSERT(mo_value_text(f.heap, "original", 8, &args[2]));
    MO_ASSERT(mo_prim_call(f.heap, &f.types, MO_PRIM_MAP_SET, f.signature[MO_PRIM_MAP_SET], args,
                           &out, &reason) == MO_PRIM_RETURNED);
    args[0] = out;
    MO_ASSERT(mo_value_text(f.heap, "duplicate", 9, &args[2]));
    out = mo_cell_int(41);
    CHECK(mo_prim_call(f.heap, &f.types, MO_PRIM_MAP_LITERAL_INSERT,
                       f.signature[MO_PRIM_MAP_LITERAL_INSERT], args, &out,
                       &reason) == MO_PRIM_REPEATED_KEY);
    CHECK(mo_cell_get_int(out) == 41 && reason != NULL && mo_table_size(args[0]) == 1);
    struct mo_cell value;
    bool found;
    MO_ASSERT(mo_table_get(f.heap, map, args[0], args[1], &found, &value) && found);
    size_t length;
    const uint8_t *bytes = mo_value_data(value, &length);
    CHECK(length == 8 && memcmp(bytes, "original", 8) == 0);
    bytes = mo_value_data(args[2], &length);
    CHECK(length == 9 && memcmp(bytes, "duplicate", 9) == 0);
    mo_value_drop(f.heap, map, args[0]);
    mo_value_drop(f.heap, text, args[2]);

    MO_ASSERT(mo_value_text(f.heap, "ab", 2, &args[0]));
    for (unsigned pass = 0; pass < 2; pass++) {
        args[1] = mo_cell_int(pass == 0 ? -1 : 20000);
        enum mo_prim_status status =
            mo_prim_call(f.heap, &f.types, MO_PRIM_TEXT_REPEAT, f.signature[MO_PRIM_TEXT_REPEAT],
                         args, &out, &reason);
        CHECK(status == (pass == 0 ? MO_PRIM_OUTSIDE : MO_PRIM_OUT_OF_MEMORY));
        CHECK(reason != NULL && mo_cell_get_int(out) == 41);
        if (pass == 1)
            CHECK(strstr(reason, "--max-heap") != NULL);
        bytes = mo_value_data(args[0], &length);
        CHECK(length == 2 && memcmp(bytes, "ab", 2) == 0);
    }
    mo_value_drop(f.heap, text, args[0]);
    CHECK(mo_heap_objects(f.heap) == 0 && mo_heap_used(f.heap) == baseline);
    prim_fixture_free(&f);
}

/* Exercise the compiler's actual ownership decisions, not a C-only unique
 * builder. Shared calls must also complete within the --work gate's budget. */
static void vm_builder_work(uint32_t iterations) {
    const char *source = "(core " MO_CORE_TEXT_VERSION " (Program () ((FunDecl \"go\" "
                         "((\"acc\" (text)) (\"n\" (int))) (text) "
                         "(if_ (prim (eq (int)) ((var \"n\") (lit (int 0)))) (var \"acc\") "
                         "(call \"go\" ((prim (text (concat)) ((var \"acc\") (lit (text \"ab\")))) "
                         "(prim (int (sub)) ((var \"n\") (lit (int 1))))))) "
                         "\"Build\" (function) 0)) () \"go\" (noSource)))";
    struct mo_arena arena;
    struct mo_diags diags;
    struct mo_buffer wire;
    mo_arena_init(&arena, MO_LIMIT_CORE_SCRATCH_SIZE);
    MO_ASSERT(mo_diags_init(&diags));
    mo_buffer_init(&wire, MO_LIMIT_BYTECODE_SIZE);
    struct mo_core *core = mo_core_parse(&arena, source, strlen(source), &diags);
    bool ok = core != NULL && mo_compile(core, &wire, &diags);
    CHECK(ok);
    if (!ok && diags.count != 0)
        CHECK(fprintf(stderr, "VM builder: %s\n", diags.items[0].message) > 0);
    struct mo_load_error error;
    struct mo_image *image = ok ? mo_load(wire.bytes, wire.length, &error) : NULL;
    CHECK(image != NULL);
    mo_buffer_free(&wire);
    mo_diags_free(&diags);
    mo_arena_free(&arena);
    if (image == NULL)
        return;
    struct mo_limits limits = mo_limits_default();
    struct mo_defect defect;
    MO_ASSERT(mo_image_init(image, &limits, &defect) == MO_RETURNED);
    struct mo_process *process = mo_process_new(image, &limits);
    MO_ASSERT(process != NULL);
    struct mo_heap *heap = mo_process_heap(process);
    struct mo_cell args[] = {mo_cell_unit(), mo_cell_int(iterations)}, out;
    MO_ASSERT(mo_value_text(heap, "", 0, &args[0]));
    struct timespec start, end;
    MO_ASSERT(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
    enum mo_outcome outcome = mo_call(process, 0, args, 2, &out, &defect);
    MO_ASSERT(clock_gettime(CLOCK_MONOTONIC, &end) == 0);
    CHECK(outcome == MO_RETURNED);
    if (outcome == MO_RETURNED) {
        size_t length;
        const uint8_t *bytes = mo_value_data(out, &length);
        CHECK(length == (size_t)iterations * 2);
        for (size_t i = 0; i < length; i++)
            CHECK(bytes[i] == (i % 2 == 0 ? 'a' : 'b'));
        const struct mo_bytecode *bytecode = mo_image_bytecode(image);
        mo_value_drop(heap, bytecode->functions[0].result_type, out);
    }
    const struct mo_bytecode_function *function = &mo_image_bytecode(image)->functions[0];
    if (function->parameters[0].ownership == MO_PARAMETER_BORROWED)
        mo_value_drop(heap, function->parameters[0].type, args[0]);
    double seconds =
        (double)(end.tv_sec - start.tv_sec) + (double)(end.tv_nsec - start.tv_nsec) / 1e9;
    MO_ASSERT(printf("Compiled VM Text builder (%u appends): %.6f s\n", iterations, seconds) > 0);
    mo_process_free(process);
    mo_image_free(image);
}

static void vm_list_window(void) {
    const char *source = "(core " MO_CORE_TEXT_VERSION " (Program () ((FunDecl \"first\" "
                         "((\"xs\" (list (int)))) (option (int)) "
                         "(prim (list (int) (first)) ((var \"xs\"))) "
                         "\"Window\" (function) 0)) () \"first\" (noSource)))";
    struct mo_arena arena;
    struct mo_diags diags;
    struct mo_buffer wire;
    mo_arena_init(&arena, MO_LIMIT_CORE_SCRATCH_SIZE);
    MO_ASSERT(mo_diags_init(&diags));
    mo_buffer_init(&wire, MO_LIMIT_BYTECODE_SIZE);
    struct mo_core *core = mo_core_parse(&arena, source, strlen(source), &diags);
    bool ok = core != NULL && mo_compile(core, &wire, &diags);
    if (!ok && diags.count != 0)
        MO_ASSERT(fprintf(stderr, "VM list window: %s\n", diags.items[0].message) > 0);
    MO_ASSERT(ok);
    struct mo_load_error error;
    struct mo_image *image = mo_load(wire.bytes, wire.length, &error);
    MO_ASSERT(image != NULL);
    mo_buffer_free(&wire);
    mo_diags_free(&diags);
    mo_arena_free(&arena);
    struct mo_limits limits = mo_limits_default();
    struct mo_defect defect;
    MO_ASSERT(mo_image_init(image, &limits, &defect) == MO_RETURNED);
    struct mo_process *process = mo_process_new(image, &limits);
    MO_ASSERT(process != NULL);
    struct mo_heap *heap = mo_process_heap(process);
    const struct mo_parameter *parameter = &mo_image_bytecode(image)->functions[0].parameters[0];
    struct mo_cell values[16], list, out;
    for (uint32_t i = 0; i < 16; i++)
        values[i] = mo_cell_int(i);
    MO_ASSERT(mo_value_list(heap, parameter->type, values, 16, &list));
    MO_ASSERT(mo_list_slice(heap, parameter->type, &list, 3, 12));
    MO_ASSERT(mo_call(process, 0, &list, 1, &out, &defect) == MO_RETURNED);
    CHECK(mo_cell_get_tag(out) == 1 && mo_cell_get_int(mo_cell_get_payload(out)) == 3);
    if (parameter->ownership == MO_PARAMETER_BORROWED)
        mo_value_drop(heap, parameter->type, list);
    CHECK(mo_heap_objects(heap) == 0);
    mo_process_free(process);
    mo_image_free(image);
}

/* A million single-holder uncons steps distinguish advancing a view from
 * moving the retained suffix on every drop. The CI work gate bounds time. */
static void list_drop_work(void) {
    struct prim_fixture f;
    prim_fixture_init(&f, MO_DEFAULT_HEAP_SIZE);
    uint32_t type = f.types.items[f.signature[MO_PRIM_LIST_DROP]].result;
    struct mo_cell list;
    MO_ASSERT(mo_value_list(f.heap, type, NULL, 0, &list));
    for (uint32_t i = 0; i < 1000000; i++)
        MO_ASSERT(mo_list_append(f.heap, type, &list, mo_cell_int(i)));
    struct timespec start, end;
    MO_ASSERT(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
    int64_t sum = 0;
    for (uint32_t i = 0; i < 1000000; i++) {
        struct mo_cell first;
        MO_ASSERT(mo_list_get(list, 0, &first));
        MO_ASSERT(mo_cell_get_int(first) == i);
        sum += mo_cell_get_int(first);
        struct mo_cell args[] = {list, mo_cell_int(1)}, out;
        const char *reason;
        MO_ASSERT(mo_prim_call(f.heap, &f.types, MO_PRIM_LIST_DROP, f.signature[MO_PRIM_LIST_DROP],
                               args, &out, &reason) == MO_PRIM_RETURNED);
        list = out;
    }
    MO_ASSERT(clock_gettime(CLOCK_MONOTONIC, &end) == 0);
    CHECK(sum == INT64_C(499999500000) && mo_list_size(list) == 0);
    double seconds =
        (double)(end.tv_sec - start.tv_sec) + (double)(end.tv_nsec - start.tv_nsec) / 1e9;
    MO_ASSERT(printf("Single-holder List uncons (1000000 steps): %.6f s\n", seconds) > 0);
    mo_value_drop(f.heap, type, list);
    CHECK(mo_heap_objects(f.heap) == 0);
    prim_fixture_free(&f);
}

/* These sizes distinguish constant-amortized updates from repeated copies.
 * ci/check_primitives.py times them together with the compiled VM builder. */
static void reuse_work(void) {
    const enum mo_primitive_id ids[] = {MO_PRIM_TEXT_CONCAT, MO_PRIM_LIST_CONCAT,
                                        MO_PRIM_SET_UNION};
    for (unsigned kind = 0; kind < 3; kind++) {
        struct prim_fixture f;
        prim_fixture_init(&f, MO_DEFAULT_HEAP_SIZE);
        enum mo_primitive_id id = ids[kind];
        uint32_t type = f.types.items[f.signature[id]].result;
        struct mo_cell args[2], out;
        const char *reason;
        if (kind == 0) {
            MO_ASSERT(mo_value_text(f.heap, "", 0, &args[0]));
            MO_ASSERT(mo_value_text(f.heap, "β🙂abcd", 10, &args[1]));
        } else if (kind == 1)
            MO_ASSERT(mo_value_list(f.heap, type, NULL, 0, &args[0]));
        else
            MO_ASSERT(mo_value_set(f.heap, type, NULL, 0, &args[0]));
        struct timespec start, end;
        MO_ASSERT(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
        for (uint32_t i = 0; i < 100000; i++) {
            struct mo_cell keys[] = {mo_cell_int(i), mo_cell_int((int64_t)i + 1)};
            if (kind == 1)
                MO_ASSERT(mo_value_list(f.heap, type, keys, 1, &args[1]));
            else if (kind == 2)
                /* Alternate singleton and batch union; batches overlap. */
                MO_ASSERT(mo_value_set(f.heap, type, keys, i % 2 == 0 ? 1U : 2U, &args[1]));
            MO_ASSERT(mo_prim_call(f.heap, &f.types, id, f.signature[id], args, &out, &reason) ==
                      MO_PRIM_RETURNED);
            args[0] = out;
            if (kind != 0)
                mo_value_drop(f.heap, type, args[1]);
        }
        MO_ASSERT(clock_gettime(CLOCK_MONOTONIC, &end) == 0);
        double seconds =
            (double)(end.tv_sec - start.tv_sec) + (double)(end.tv_nsec - start.tv_nsec) / 1e9;
        MO_ASSERT(printf("%s incremental build: %.6f s\n",
                         kind == 0   ? "Text"
                         : kind == 1 ? "List"
                                     : "Set",
                         seconds) > 0 &&
                  fflush(stdout) == 0);
        if (kind == 0) {
            size_t length;
            const uint8_t *bytes = mo_value_data(out, &length);
            CHECK(length == 1000000);
            for (size_t i = 0; i < length; i += 10)
                CHECK(memcmp(bytes + i, "β🙂abcd", 10) == 0);
            mo_value_drop(f.heap, type, args[1]);
        } else if (kind == 1) {
            CHECK(mo_list_size(out) == 100000);
            for (uint32_t i = 0; i < 100000; i++) {
                struct mo_cell value;
                MO_ASSERT(mo_list_get(out, i, &value));
                CHECK(mo_cell_get_int(value) == i);
            }
        } else {
            CHECK(mo_table_size(out) == 100001);
            uint32_t cursor = 0;
            struct mo_cell key, value;
            for (uint32_t i = 0; i < 100001; i++) {
                MO_ASSERT(mo_table_next(out, &cursor, &key, &value));
                CHECK(mo_cell_get_int(key) == i);
            }
        }
        mo_value_drop(f.heap, type, out);
        CHECK(mo_heap_objects(f.heap) == 0);
        prim_fixture_free(&f);
    }
}

static void primitive_work(void) {
    struct prim_fixture f;
    prim_fixture_init(&f, MO_DEFAULT_HEAP_SIZE);
    size_t baseline = mo_heap_used(f.heap);
    uint32_t list = f.types.items[f.signature[MO_PRIM_LIST_APPEND]].result;
    uint32_t map = f.types.items[f.signature[MO_PRIM_MAP_SET]].result;
    struct mo_cell args[3], out;
    const char *reason;
    MO_ASSERT(mo_value_list(f.heap, list, NULL, 0, &args[0]));
    for (uint32_t i = 0; i < 100000; i++) {
        args[1] = mo_cell_int(i);
        MO_ASSERT(mo_prim_call(f.heap, &f.types, MO_PRIM_LIST_APPEND,
                               f.signature[MO_PRIM_LIST_APPEND], args, &out,
                               &reason) == MO_PRIM_RETURNED);
        args[0] = out;
    }
    MO_ASSERT(mo_prim_call(f.heap, &f.types, MO_PRIM_LIST_UNIQUE, f.signature[MO_PRIM_LIST_UNIQUE],
                           args, &out, &reason) == MO_PRIM_RETURNED);
    CHECK(mo_list_size(out) == 100000);
    struct mo_cell last;
    MO_ASSERT(mo_list_get(out, 99999, &last));
    CHECK(mo_cell_get_int(last) == 99999);
    mo_value_drop(f.heap, list, out);

    MO_ASSERT(mo_value_map(f.heap, map, NULL, NULL, 0, &args[0]));
    for (uint32_t i = 0; i < 100000; i++) {
        args[1] = mo_cell_int(i);
        MO_ASSERT(mo_value_text(f.heap, "x", 1, &args[2]));
        MO_ASSERT(mo_prim_call(f.heap, &f.types, MO_PRIM_MAP_SET, f.signature[MO_PRIM_MAP_SET],
                               args, &out, &reason) == MO_PRIM_RETURNED);
        args[0] = out;
    }
    CHECK(mo_table_size(args[0]) == 100000);
    mo_value_drop(f.heap, map, args[0]);

    const enum mo_primitive_id adds[] = {MO_PRIM_MAP_SET, MO_PRIM_SET_ADD};
    const enum mo_primitive_id removes[] = {MO_PRIM_MAP_REMOVE, MO_PRIM_SET_REMOVE};
    for (unsigned kind = 0; kind < 2; kind++) {
        uint32_t type = f.types.items[f.signature[adds[kind]]].result;
        bool ok = kind == 0 ? mo_value_map(f.heap, type, NULL, NULL, 0, &args[0])
                            : mo_value_set(f.heap, type, NULL, 0, &args[0]);
        MO_ASSERT(ok);
        for (uint32_t i = 0; i < 65536; i++) {
            args[1] = mo_cell_int(i);
            if (kind == 0)
                MO_ASSERT(mo_value_text(f.heap, "β", 2, &args[2]));
            MO_ASSERT(mo_prim_call(f.heap, &f.types, adds[kind], f.signature[adds[kind]], args,
                                   &out, &reason) == MO_PRIM_RETURNED);
            args[0] = out;
        }
        for (uint32_t i = 0; i < 100000; i++) {
            args[1] = mo_cell_int(i % 65536);
            MO_ASSERT(mo_prim_call(f.heap, &f.types, removes[kind], f.signature[removes[kind]],
                                   args, &out, &reason) == MO_PRIM_RETURNED);
            args[0] = out;
            if (kind == 0)
                MO_ASSERT(mo_value_text(f.heap, "🙂", 4, &args[2]));
            MO_ASSERT(mo_prim_call(f.heap, &f.types, adds[kind], f.signature[adds[kind]], args,
                                   &out, &reason) == MO_PRIM_RETURNED);
            args[0] = out;
        }
        CHECK(mo_table_size(args[0]) == 65536);
        uint32_t cursor = 0;
        struct mo_cell key, value;
        for (uint32_t i = 0; i < 65536; i++) {
            MO_ASSERT(mo_table_next(args[0], &cursor, &key, &value));
            CHECK(mo_cell_get_int(key) == (int64_t)((100000 + i) % 65536));
        }
        mo_value_drop(f.heap, type, args[0]);
    }

    struct mo_arena arena;
    mo_arena_init(&arena, 8 << 20);
    size_t count = 4 << 20;
    uint8_t *bytes = mo_arena_allocate(&arena, count, 1);
    MO_ASSERT(bytes != NULL);
    memset(bytes, 'a', count);
    MO_ASSERT(mo_value_text(f.heap, (const char *)bytes, count, &args[0]));
    bytes[(1 << 20) - 1] = 'b';
    MO_ASSERT(mo_value_text(f.heap, (const char *)bytes, 1 << 20, &args[1]));
    MO_ASSERT(mo_prim_call(f.heap, &f.types, MO_PRIM_TEXT_CONTAINS,
                           f.signature[MO_PRIM_TEXT_CONTAINS], args, &out,
                           &reason) == MO_PRIM_RETURNED);
    CHECK(!mo_cell_get_bool(out));
    mo_value_drop(f.heap, f.builtin[MO_TYPE_TEXT], args[0]);
    mo_value_drop(f.heap, f.builtin[MO_TYPE_TEXT], args[1]);

    count = 16384;
    memset(bytes, 255, count);
    MO_ASSERT(mo_value_bigint(f.heap, MO_BIGINT_POSITIVE, bytes, count, &args[0]));
    args[1] = args[0];
    MO_ASSERT(mo_prim_call(f.heap, &f.types, MO_PRIM_BIGINT_MUL, f.signature[MO_PRIM_BIGINT_MUL],
                           args, &out, &reason) == MO_PRIM_RETURNED);
    size_t length;
    const uint8_t *result = mo_value_data(out, &length);
    CHECK(length == 2 * count && result[0] == 1 && result[count] == 254);
    for (size_t i = 1; i < count; i++)
        CHECK(result[i] == 0 && result[count + i] == 255);
    mo_value_drop(f.heap, f.builtin[MO_TYPE_BIGINT], args[0]);
    mo_value_drop(f.heap, f.builtin[MO_TYPE_BIGINT], out);
    count = 2 << 20;
    memset(bytes, 255, count);
    MO_ASSERT(mo_value_bigint(f.heap, MO_BIGINT_POSITIVE, bytes, count, &args[0]));
    uint8_t three = 3;
    MO_ASSERT(mo_value_bigint(f.heap, MO_BIGINT_POSITIVE, &three, 1, &args[1]));
    MO_ASSERT(mo_prim_call(f.heap, &f.types, MO_PRIM_BIGINT_MUL, f.signature[MO_PRIM_BIGINT_MUL],
                           args, &out, &reason) == MO_PRIM_RETURNED);
    result = mo_value_data(out, &length);
    CHECK(length == count + 1 && result[0] == 253 && result[count] == 2);
    for (size_t i = 1; i < count; i++)
        CHECK(result[i] == 255);
    mo_value_drop(f.heap, f.builtin[MO_TYPE_BIGINT], args[0]);
    mo_value_drop(f.heap, f.builtin[MO_TYPE_BIGINT], args[1]);
    mo_value_drop(f.heap, f.builtin[MO_TYPE_BIGINT], out);
    mo_arena_free(&arena);
    CHECK(mo_heap_objects(f.heap) == 0 && mo_heap_used(f.heap) == baseline);
    prim_fixture_free(&f);
}

static void time_boundary(void) {
    struct prim_fixture f;
    prim_fixture_init(&f, 1 << 20);
    struct mo_cell args[] = {mo_cell_time(INT64_C(253402300799999999)), mo_cell_duration(1)};
    struct mo_cell out = mo_cell_int(41);
    const char *reason;
    CHECK(mo_prim_call(f.heap, &f.types, MO_PRIM_TIME_CHECKED_ADD,
                       f.signature[MO_PRIM_TIME_CHECKED_ADD], args, &out,
                       &reason) == MO_PRIM_RETURNED);
    CHECK(mo_cell_get_tag(out) == 0);
    prim_fixture_free(&f);
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--big-text-1m") == 0) {
        bigint_text_work(415241);
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--big-text-10m") == 0) {
        bigint_text_work(4152410);
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--oracle") == 0) {
        primitive_oracle(true);
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--oracle-unique") == 0) {
        primitive_oracle(false);
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--work") == 0) {
        reuse_work();
        vm_builder_work(30000);
        list_drop_work();
        primitive_work();
        return mo_test_report("primitive work");
    }
    if (argc == 2 && strcmp(argv[1], "--reuse-work") == 0) {
        reuse_work();
        vm_builder_work(30000);
        return mo_test_report("owned reuse work");
    }
    if (argc == 2 && strcmp(argv[1], "--vm-builder-work") == 0) {
        vm_builder_work(100000);
        return mo_test_report("compiled VM builder work");
    }
    if (argc == 2 && strcmp(argv[1], "--list-drop-work") == 0) {
        list_drop_work();
        return mo_test_report("single-holder List uncons work");
    }
    MO_ASSERT(argc == 1);
    empty_constants_and_rounding();
    runtime_sizes();
    integer_power();
    integer_floor();
    decimal_rounding();
    text_split();
    collection_ownership();
    refusal_preserves_operands();
    shared_text_history();
    shared_copy_charge();
    unique_contraction();
    growth_near_limit();
    unchanged_trim();
    unique_reverse_without_space();
    unique_union_without_space();
    reuse_refusals();
    reuse_aliases();
    vm_list_window();
    time_boundary();
    return mo_test_report("primitives");
}
