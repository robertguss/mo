#include "test.h"
#include "tools/fix.h"

#include <string.h>

/* Fixes are planned against one file's original text: each test adds diagnostics with fixes and
 * reads back the text mo_fix_source writes. */
struct plan {
    struct mo_sources sources;
    struct mo_diags diags;
};

static void start(struct plan *p, const char *text) {
    MO_ASSERT(mo_sources_init(&p->sources) && mo_diags_init(&p->diags));
    uint32_t file;
    struct mo_source_error error;
    MO_ASSERT(mo_sources_add_bytes(&p->sources, "main.mo", (const uint8_t *)text, strlen(text),
                                   &file, &error) &&
              file == 0);
    MO_ASSERT(mo_sources_add_bytes(&p->sources, "other.mo", (const uint8_t *)text, strlen(text),
                                   &file, &error) &&
              file == 1);
}

/* A diagnostic at the first edit's start with the given fixes. */
static void offer(struct plan *p, const struct mo_fix *fixes, uint32_t fix_count) {
    struct mo_diag d = {.code = 52,
                        .message = "a name or parameter is unused",
                        .reason = "A name starting with _ is unused on purpose (reference 6.1).",
                        .primary = {fixes[0].edits[0].span, NULL},
                        .fixes = fixes,
                        .fix_count = fix_count};
    MO_ASSERT(mo_diags_add(&p->diags, &d));
}

/* The text after planning, and the number of fixes taken. */
static bool fixed(struct plan *p, const char *expected, uint32_t taken) {
    struct mo_buffer out;
    mo_buffer_init(&out, MO_LIMIT_FILE_SIZE);
    uint32_t applied = UINT32_MAX;
    bool ok = mo_fix_source(&p->sources, 0, &p->diags, &out, &applied);
    size_t length = strlen(expected);
    ok = ok && applied == taken && out.length == length &&
         (length == 0 || memcmp(out.bytes, expected, length) == 0);
    if (!ok)
        (void)fprintf(stderr, "got %u fixes: %.*s\n", applied, (int)out.length,
                      out.bytes == NULL ? "" : (const char *)out.bytes);
    mo_buffer_free(&out);
    mo_sources_free(&p->sources);
    mo_diags_free(&p->diags);
    return ok;
}

static struct mo_edit edit(uint32_t file, uint32_t start, uint32_t end, const char *text) {
    return (struct mo_edit){{file, start, end}, text};
}

static void single_fixes(void) {
    struct plan p;
    start(&p, "abcdef");
    CHECK(fixed(&p, "abcdef", 0));

    /* Two fixes apply together, whatever order they were reported in. */
    start(&p, "abcdef");
    struct mo_edit late = edit(0, 4, 5, "E"), early = edit(0, 1, 1, "_");
    struct mo_fix fixes[] = {{"late", &late, 1}, {"early", &early, 1}};
    offer(&p, &fixes[0], 1);
    offer(&p, &fixes[1], 1);
    CHECK(fixed(&p, "a_bcdEf", 2));

    /* A fix of several edits applies all of them. */
    start(&p, "abcdef");
    struct mo_edit both[] = {edit(0, 0, 1, ""), edit(0, 6, 6, "!")};
    struct mo_fix pair = {"both", both, 2};
    offer(&p, &pair, 1);
    CHECK(fixed(&p, "bcdef!", 1));
}

static void skipped_fixes(void) {
    struct plan p;
    /* Alternatives are the reader's choice, so neither applies. */
    start(&p, "abcdef");
    struct mo_edit one = edit(0, 0, 1, "x"), two = edit(0, 0, 1, "y");
    struct mo_fix choices[] = {{"x", &one, 1}, {"y", &two, 1}};
    offer(&p, choices, 2);
    CHECK(fixed(&p, "abcdef", 0));

    /* A fix that also edits another file is left whole: fix rewrites only its own file. */
    start(&p, "abcdef");
    struct mo_edit spread[] = {edit(0, 0, 0, "<"), edit(1, 0, 0, "<")};
    struct mo_fix across = {"spread", spread, 2};
    offer(&p, &across, 1);
    CHECK(fixed(&p, "abcdef", 0));
}

static void overlapping_fixes(void) {
    struct plan p;
    /* The earlier fix wins a shared byte; the later waits for the next round. */
    start(&p, "abcdef");
    struct mo_edit first = edit(0, 1, 3, "X"), second = edit(0, 2, 4, "Y");
    struct mo_fix shared[] = {{"first", &first, 1}, {"second", &second, 1}};
    offer(&p, &shared[0], 1);
    offer(&p, &shared[1], 1);
    CHECK(fixed(&p, "aXdef", 1));

    /* Two insertions at one place would have no clear order. */
    start(&p, "abcdef");
    struct mo_edit left = edit(0, 2, 2, "1"), right = edit(0, 2, 2, "2");
    struct mo_fix same[] = {{"1", &left, 1}, {"2", &right, 1}};
    offer(&p, &same[0], 1);
    offer(&p, &same[1], 1);
    CHECK(fixed(&p, "ab1cdef", 1));

    /* An insertion inside a replaced span overlaps it; at either end it doesn't. */
    start(&p, "abcdef");
    struct mo_edit replaced = edit(0, 1, 4, "R"), inside = edit(0, 3, 3, "i"),
                   before = edit(0, 1, 1, "<"), after = edit(0, 4, 4, ">");
    struct mo_fix ends[] = {
        {"R", &replaced, 1}, {"i", &inside, 1}, {"<", &before, 1}, {">", &after, 1}};
    for (uint32_t i = 0; i < 4; i++)
        offer(&p, &ends[i], 1);
    CHECK(fixed(&p, "a<R>ef", 3));
}

int main(void) {
    single_fixes();
    skipped_fixes();
    overlapping_fixes();
    return mo_test_report("test_fix");
}
