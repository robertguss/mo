#define _POSIX_C_SOURCE 200809L

#include "base/limits.h"
#include "base/stack.h"
#include "check/check.h"
#include "test.h"
#include "tools/intent.h"

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
};

static struct digests digests_of(const char *source) {
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
        for (uint32_t i = 0; i < intent.count; i++)
            memcpy(out.items[i], intent.items[i].digest, 32);
    } else
        CHECK(false);
    mo_arena_free(&arena);
    mo_diags_free(&diags);
    mo_sources_free(&sources);
    return out;
}

static bool same(const uint8_t *a, const uint8_t *b) {
    return memcmp(a, b, 32) == 0;
}

static void identity(void) {
    struct digests base = digests_of(BASE CLAMP(HEADER, CLAMP_PROMISES, BODY));
    CHECK(base.count == 3);
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
}

static void run_tests(void *unused) {
    (void)unused;
    identity();
    service();
}

int main(void) {
    CHECK(mo_with_stack(MO_SOURCE_STACK_SIZE, run_tests, NULL) == 0);
    return mo_test_report("intent");
}
