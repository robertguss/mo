/* Allocation failures while checking a program: every arena limit that stops a
 * source's check must report no-memory diagnostics inside that source, and the
 * larger limits must still check cleanly. A sanitized run of all the sources
 * takes minutes, so each source is its own test program,
 * test_program_allocation_<n>.c, and make -j runs them side by side. */
#ifndef MO_TEST_PROGRAM_ALLOCATION_H
#define MO_TEST_PROGRAM_ALLOCATION_H

#include "base/codes.h"
#include "base/stack.h"
#include "program_fixture.h"
#include "test.h"

static const char *const allocation_sources[] = {
    "module Main\nfn main()\nend\n",
    "module Main\nstruct Box(T)\nvalue: T\nend\n"
    "fn to_text(box: Box(T)) : Text where T: Text\n\"#{box.value}\"\nend\n"
    "fn main() : Int\nvar total = 0\nfor i in 0..3\ntotal += i\nend\n"
    "reader = fn(value) value.value end\ntotal = reader(Box(value: total))\n"
    "box = Some(Box(value: total))\n_rendered = \"#{box}\"\n"
    "case box\nSome(Box(value: n)): n\nNone: 0\nend\nend\n",
    "module Main\nenum Outer(T)\nWrapped(Set(T))\nend\n"
    "fn lift(value: Result(Int, Set(T))) : Result(Text, Outer(T)) where T: Hash\n"
    "n = try value\nOk(n.to_text)\nend\n"
    "fn main() : Int\nreader = fn(values) values.get(0) or 17 end\nreader([3])\nend\n",
    "module Main\nfn twice(value: T) : T where T: Number\nexamples\n"
    "twice(3) == 6\nend\nvalue + value\nend\n"
    "fn main() : Int\ntwice(4)\nend\n",
    "module Main\nfn twice(value: T) : T where T: Number, T: Equal\n"
    "promise \"doubles\"\nresult == value + value\n"
    "allows result: 6, value: 3\nforbids value: 2, result: 5\nend\n"
    "value + value\nend\ntest \"private \\u{3bb}\"\nassert twice(4) == 8\nend\n"
    "fn main()\nend\n",
    "module Main\nanswer = twice(3)\nlabel = \"#{Label(value: answer)}\"\n"
    "prefix = \"value\"\nstruct Label\nvalue: Int\nend\n"
    "fn to_text(label: Label) : Text\n\"#{prefix}#{label.value}\"\nend\n"
    "fn twice(value: T) : T where T: Number\nvalue + value\nend\n"
    "fn main() : Int\nanswer\nend\n"};

static void allocation_failures(size_t s) {
    struct fixture f;
    init(&f, allocation_sources[s]);
    MO_ASSERT(f.program != NULL && !mo_diags_any(&f.diags));
    size_t used = f.arena.used;
    uint32_t failures = 0, successes = 0;
    for (size_t limit = 0; limit <= used + 128; limit += 128) {
        mo_arena_free(&f.arena);
        mo_arena_init(&f.arena, limit);
        mo_diags_free(&f.diags);
        MO_ASSERT(mo_diags_init(&f.diags));
        f.program = mo_check(&f.arena, &f.set, &f.diags);
        if (f.program == NULL) {
            failures++;
            CHECK(mo_diags_any(&f.diags));
            for (uint32_t i = 0; i < f.diags.count; i++) {
                CHECK(f.diags.items[i].code == MO_CODE_CHECK_NO_MEMORY);
                struct mo_span span = f.diags.items[i].primary.span;
                CHECK(span.file == f.module->span.file || span.file == UINT32_MAX);
                CHECK(span.start <= span.end && span.end <= strlen(allocation_sources[s]));
            }
        } else {
            successes++;
            CHECK(!mo_diags_any(&f.diags));
        }
    }
    CHECK(failures != 0 && successes != 0);
    free_fixture(&f);
}

static void allocation_run(void *argument) {
    allocation_failures(*(const size_t *)argument);
}

#define ALLOCATION_SOURCE_COUNT (sizeof(allocation_sources) / sizeof(allocation_sources[0]))

/* One shard file per source: a source added to the table without a shard of its own would
 * never run, so the counts must agree. */
#define ALLOCATION_SHARDS 6
_Static_assert(ALLOCATION_SOURCE_COUNT == ALLOCATION_SHARDS,
               "one test_program_allocation_<n>.c per source");

/* The body of each shard's main: runs one source on the large source stack. */
static int allocation_main(size_t source) {
    MO_ASSERT(source < ALLOCATION_SOURCE_COUNT);
    CHECK(mo_with_stack(MO_SOURCE_STACK_SIZE, allocation_run, &source) == 0);
    return mo_test_report("program allocation");
}

#endif
