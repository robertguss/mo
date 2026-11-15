#define _POSIX_C_SOURCE 200809L

#include "base/intent-list.h"
#include "base/limits.h"
#include "base/stack.h"
#include "check/check.h"
#include "test.h"
#include "tools/intent.h"
#include "tools/upgrade.h"

#include <string.h>

#define EVEN "n == 0 or odd(n - 1)"
#define HELPERS(limit, even)                                                                       \
    "module Main\n\nlimit = " limit "\n\nfn even(n: Int) : Bool\n  " even "\nend\n\n"              \
    "fn odd(n: Int) : Bool\n  n != 0 and even(n - 1)\nend\n\n"
#define BASE HELPERS("3", EVEN)

#define ABOVE                                                                                      \
    "  promise \"never above the limit\"\n    result <= limit\n"                                   \
    "    allows x: 2, floor: 0, result: 2\n    forbids x: 5, floor: 0, result: 5\n  end\n\n"
#define EVENS                                                                                      \
    "  promise \"an even input stays even\"\n    even(x) implies even(result) or result == "       \
    "limit\n"                                                                                      \
    "    allows x: 2, floor: 0, result: 2\n    forbids x: 2, floor: 0, result: 1\n  end\n\n"
#define CLAMP_PROMISES ABOVE EVENS
#define CLAMP_SWAPPED_PROMISES EVENS ABOVE

#define CLAMP_EXAMPLES "  examples\n    clamp(5, 0) == 3\n  end\n\n"

#define CLAMP(header, promises, body) header "\n" promises CLAMP_EXAMPLES body "end\n"

#define HEADER "fn clamp(x: Int, floor: Int) : Int"
#define BODY "  return floor if x < floor\n  return limit if x > limit\n  x\n"

struct digests {
    uint32_t count;
    uint8_t items[8][32];
    bool reached[8];
};

static struct digests items_of(const char *source, struct mo_buffer *list) {
    struct digests out = {0};
    struct mo_sources sources;
    struct mo_diags diags;
    struct mo_arena arena;
    MO_ASSERT(mo_sources_init(&sources));
    MO_ASSERT(mo_diags_init(&diags));
    mo_arena_init(&arena, 64 << 20);
    uint32_t file;
    struct mo_source_error error;
    MO_ASSERT(mo_sources_add_bytes(&sources, "tests/intent/main.mo", (const uint8_t *)source,
                                   strlen(source), &file, &error));
    struct mo_module_set *set = mo_load_bundle_sources(&arena, &sources, "lib", &diags);
    struct mo_program *program = set != NULL ? mo_check(&arena, set, &diags) : NULL;
    CHECK(program != NULL && diags.count == 0);
    struct mo_intent intent;
    if (program != NULL && mo_intent_items(&arena, program, &intent)) {
        MO_ASSERT(intent.count <= 8);
        out.count = intent.count;
        for (uint32_t i = 0; i < intent.count; i++) {
            memcpy(out.items[i], intent.items[i].digest, 32);
            out.reached[i] = intent.items[i].reached;
        }
        if (list != NULL)
            CHECK(mo_intent_list_write(&intent, list));
    } else
        CHECK(false);
    mo_arena_free(&arena);
    mo_diags_free(&diags);
    mo_sources_free(&sources);
    return out;
}

static struct digests digests_of(const char *source) {
    return items_of(source, NULL);
}

static bool same(const uint8_t *a, const uint8_t *b) {
    return memcmp(a, b, 32) == 0;
}

static void identity(void) {
    struct digests base = digests_of(BASE CLAMP(HEADER, CLAMP_PROMISES, BODY));
    CHECK(base.count == 3 && !base.reached[0] && !base.reached[1] && !base.reached[2]);
    CHECK(!same(base.items[0], base.items[1]) && !same(base.items[1], base.items[2]));

    struct digests again = digests_of(BASE CLAMP(HEADER, CLAMP_PROMISES, BODY));
    CHECK(memcmp(&base, &again, sizeof base) == 0);

    struct digests swapped = digests_of(BASE CLAMP(HEADER, CLAMP_SWAPPED_PROMISES, BODY));
    CHECK(swapped.count == 3 && same(swapped.items[0], base.items[1]) &&
          same(swapped.items[1], base.items[0]) && same(swapped.items[2], base.items[2]));

    struct digests limit = digests_of(HELPERS("4", EVEN) CLAMP(HEADER, CLAMP_PROMISES, BODY));
    CHECK(!same(limit.items[0], base.items[0]) && !same(limit.items[1], base.items[1]) &&
          same(limit.items[2], base.items[2]));

    struct digests body =
        digests_of(BASE CLAMP(HEADER, CLAMP_PROMISES,
                              "  return floor if x <= floor\n  return limit if x >= limit\n  x\n"));
    CHECK(memcmp(&base, &body, sizeof base) == 0);

    struct digests signature =
        digests_of(BASE CLAMP("fn clamp(floor: Int, x: Int) : Int", CLAMP_PROMISES, BODY));
    CHECK(same(signature.items[0], base.items[0]) && same(signature.items[1], base.items[1]));

    struct digests helper = digests_of(HELPERS("3", "n == 0 or odd(n - 1) == true")
                                           CLAMP(HEADER, CLAMP_PROMISES, BODY));
    CHECK(same(helper.items[0], base.items[0]) && !same(helper.items[1], base.items[1]) &&
          same(helper.items[2], base.items[2]));
}

#define SERVICE(kept, reply)                                                                       \
    "module Main\n\nservice\n  state List(Int) = []\n  tools add\n"                                \
    "  token from env \"NUMBERS_TOKEN\"\n\n  promise \"no number is ever lost\"\n"                 \
    "    kept(before, after)\n    allows before: [1], after: [1, 2], now: t0\n"                    \
    "    forbids before: [1], after: [], now: t0\n  end\nend\n\n"                                  \
    "fn kept(before: List(Int), after: List(Int)) : Bool\n  " kept "\nend\n\n"                     \
    "fn add(numbers: List(Int), _now: Time, n: Int) : (List(Int), Int)\n"                          \
    "  (numbers.append(n), " reply ")\nend\n\n"                                                    \
    "t0 = Time.parse(\"2026-01-01T00:00:00Z\") or stop \"time\"\n"

static void service(void) {
    struct digests base =
        digests_of(SERVICE("before.all?(fn(n) after.contains?(n) end)", "numbers.size + 1"));
    struct digests handler =
        digests_of(SERVICE("before.all?(fn(n) after.contains?(n) end)", "numbers.size + 2"));
    struct digests helper =
        digests_of(SERVICE("before.all?(fn(m) after.contains?(m) end)", "numbers.size + 1"));
    CHECK(base.count == 1 && handler.count == 1 && helper.count == 1);
    CHECK(same(handler.items[0], base.items[0]) && !same(helper.items[0], base.items[0]));
    CHECK(base.reached[0]);
}

#define REACH(initial, handler)                                                                    \
    "module Main\n\nservice\n  state Int = " initial "\n  tools add\n"                             \
    "  token from env \"TOKEN\"\nend\n\n"                                                          \
    "fn bump(n: Int) : Int\n  examples\n    bump(1) == 2\n  end\n\n  n + 1\nend\n\n"               \
    "fn twice(n: Int) : Int\n  bump(bump(n))\nend\n\n"                                             \
    "fn add(state: Int, _now: Time) : (Int, Int)\n  (" handler ", state)\nend\n"

static void reach(void) {
    struct digests through = digests_of(REACH("0", "twice(state)"));
    struct digests dropped = digests_of(REACH("0", "state + 1"));
    struct digests initial = digests_of(REACH("bump(0)", "state + 1"));
    CHECK(through.count == 1 && dropped.count == 1 && initial.count == 1);
    CHECK(through.reached[0] && !dropped.reached[0] && initial.reached[0]);
    CHECK(same(through.items[0], dropped.items[0]));
}

#define SMALL                                                                                      \
    "  promise \"stays small\"\n    result <= n + 1\n"                                             \
    "    allows n: 1, result: 2\n    forbids n: 1, result: 5\n  end\n\n"
#define TWICE(promises, handler)                                                                   \
    "module Main\n\nservice\n  state Int = 0\n  tools add\n"                                       \
    "  token from env \"TOKEN\"\nend\n\nfn bump(n: Int) : Int\n" promises "  n + 1\nend\n\n"       \
    "fn add(state: Int, _now: Time) : (Int, Int)\n  (" handler ", state)\nend\n"

static uint32_t marks(const struct mo_buffer *text, const char *mark) {
    uint32_t count = 0;
    for (size_t at = 0; at < text->length; at++)
        count += at == 0 || text->bytes[at - 1] == '\n'
                     ? strncmp((const char *)text->bytes + at, mark, strlen(mark)) == 0
                     : 0;
    return count;
}

static void duplicates(void) {
    struct mo_buffer active, lacking, unreached;
    mo_buffer_init(&active, MO_LIMIT_INTENT_LIST_BYTES);
    mo_buffer_init(&lacking, MO_LIMIT_INTENT_LIST_BYTES);
    mo_buffer_init(&unreached, MO_LIMIT_INTENT_LIST_BYTES);
    struct digests twice = items_of(TWICE(SMALL SMALL, "bump(state)"), &active);
    (void)items_of(TWICE("", "bump(state)"), &lacking);
    (void)items_of(TWICE(SMALL SMALL, "state + 1"), &unreached);
    CHECK(twice.count == 2 && same(twice.items[0], twice.items[1]));
    struct mo_store_review r = {.reviewed = true, .approved = &twice.items[0], .approved_count = 1};
    CHECK(mo_intent_list_read(active.bytes, active.length, &r.items));
    const struct mo_buffer *candidates[] = {&lacking, &unreached};
    for (unsigned i = 0; i < 2; i++) {
        struct mo_intent_list candidate;
        struct mo_arena arena;
        struct mo_door_view view;
        mo_arena_init(&arena, MO_DOOR_BYTES);
        mo_buffer_init(&view.text, MO_DOOR_DIFFERENCE_BYTES);
        CHECK(mo_intent_list_read(candidates[i]->bytes, candidates[i]->length, &candidate) &&
              mo_door_difference(&arena, &r, &candidate, &view));
        CHECK(!view.keeps && view.approve_count == 0);
        CHECK(marks(&view.text, i == 0 ? "lacks " : "not reached ") == 2);
        CHECK(view.retire_count == (i == 0 ? 0 : 2));
        mo_buffer_free(&view.text);
        mo_arena_free(&arena);
    }
    mo_buffer_free(&active);
    mo_buffer_free(&lacking);
    mo_buffer_free(&unreached);
}

static void earlier_list(void) {
    struct mo_buffer v1;
    mo_buffer_init(&v1, 256);
    uint8_t digest[32];
    memset(digest, 'q', 32);
    CHECK(mo_buffer_append(&v1, "MOIL\1\1", 6) && mo_binary_write(&v1, 1, 4) &&
          mo_buffer_append(&v1, digest, 32) && mo_buffer_append_byte(&v1, 0));
    for (unsigned j = 0; j < 3; j++)
        CHECK(mo_binary_write(&v1, 1, 4) && mo_buffer_append_byte(&v1, (uint8_t) "fnt"[j]));
    CHECK(mo_binary_write(&v1, 0, 4));
    struct mo_intent_list list;
    struct mo_intent_entry entry;
    CHECK(mo_intent_list_read(v1.bytes, v1.length, &list) && list.count == 1);
    struct mo_binary cursor = mo_intent_list_first(&list);
    CHECK(mo_intent_list_next(&list, &cursor, &entry) && entry.reached && entry.text.length == 1 &&
          entry.text.bytes[0] == 't');
    mo_buffer_free(&v1);
}

static void run_tests(void *unused) {
    (void)unused;
    identity();
    service();
    reach();
    duplicates();
    earlier_list();
}

int main(void) {
    CHECK(mo_with_stack(MO_SOURCE_STACK_SIZE, run_tests, NULL) == 0);
    return mo_test_report("intent");
}
