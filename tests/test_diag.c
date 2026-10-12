#include "base/codes.h"
#include "base/diag.h"
#include "base/limits.h"
#include "test.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static struct mo_span span(uint32_t file, uint32_t start, uint32_t end) {
    return (struct mo_span){.file = file, .start = start, .end = end};
}

static bool same_span(struct mo_span a, struct mo_span b) {
    return a.file == b.file && a.start == b.start && a.end == b.end;
}

/* A diagnostic with only a code, a primary span and a message. */
static struct mo_diag plain(uint16_t code, struct mo_span primary, const char *message) {
    return (struct mo_diag){.code = code, .primary = {.span = primary}, .message = message};
}

static void test_it_starts_empty(void) {
    struct mo_diags diags;
    CHECK(mo_diags_init(&diags));
    CHECK(diags.count == 0 && diags.dropped == 0);
    CHECK(!mo_diags_any(&diags));
    mo_diags_sort(&diags);
    mo_diags_free(&diags);
}

static void test_it_owns_its_strings(void) {
    struct mo_diags diags;
    CHECK(mo_diags_init(&diags));
    /* Everything the stage passes lives on its stack, and is overwritten after the call. */
    char message[] = "a failure is ignored";
    char primary_text[] = "returns Result(Unit, FileError), and the result is dropped";
    char secondary_text[] = "declared here";
    char replacement[] = "try files.write_text(\"orders.txt\", report)";
    struct mo_label secondary[] = {{.span = span(1, 5, 9), .text = secondary_text},
                                   {.span = span(0, 2, 2), .text = NULL}};
    struct mo_fix fixes[] = {{.span = span(0, 100, 138), .replacement = replacement},
                             {.span = span(0, 100, 100), .replacement = ""}};
    struct mo_diag diag = {.code = MO_CODE_FAILURE_IGNORED,
                           .primary = {.span = span(0, 100, 138), .text = primary_text},
                           .secondary = secondary,
                           .secondary_count = 2,
                           .message = message,
                           .fixes = fixes,
                           .fix_count = 2};
    CHECK(mo_diags_add(&diags, &diag));
    memset(message, '?', sizeof message - 1);
    memset(primary_text, '?', sizeof primary_text - 1);
    memset(secondary_text, '?', sizeof secondary_text - 1);
    memset(replacement, '?', sizeof replacement - 1);
    memset(secondary, 0, sizeof secondary);
    memset(fixes, 0, sizeof fixes);
    memset(&diag, 0, sizeof diag);

    CHECK(diags.count == 1 && mo_diags_any(&diags));
    const struct mo_diag *kept = &diags.items[0];
    CHECK(kept->code == 412);
    CHECK(strcmp(kept->message, "a failure is ignored") == 0 && kept->message != message);
    CHECK(same_span(kept->primary.span, span(0, 100, 138)));
    CHECK(strcmp(kept->primary.text,
                 "returns Result(Unit, FileError), and the result is dropped") == 0);
    CHECK(kept->secondary_count == 2 && kept->secondary != secondary);
    CHECK(same_span(kept->secondary[0].span, span(1, 5, 9)));
    CHECK(strcmp(kept->secondary[0].text, "declared here") == 0);
    CHECK(same_span(kept->secondary[1].span, span(0, 2, 2)) && kept->secondary[1].text == NULL);
    CHECK(kept->fix_count == 2 && kept->fixes != fixes);
    CHECK(same_span(kept->fixes[0].span, span(0, 100, 138)));
    CHECK(strcmp(kept->fixes[0].replacement, "try files.write_text(\"orders.txt\", report)") == 0);
    CHECK(strcmp(kept->fixes[1].replacement, "") == 0);
    /* The section is the code's. */
    CHECK(kept->section != NULL && strcmp(kept->section, "8.3") == 0);
    mo_diags_free(&diags);
}

static void test_the_section_is_the_codes(void) {
    struct mo_diags diags;
    CHECK(mo_diags_init(&diags));
    struct mo_diag cited = plain(MO_CODE_FAILURE_IGNORED, span(0, 0, 1), "cited");
    cited.section = "8.3";
    CHECK(mo_diags_add(&diags, &cited));
    struct mo_diag limit = plain(MO_CODE_LINE_TOO_LONG, span(0, 0, 5000), "a line too long");
    CHECK(mo_diags_add(&diags, &limit));
    CHECK(diags.count == 2);
    CHECK(diags.items[0].section != NULL && strcmp(diags.items[0].section, "8.3") == 0);
    CHECK(diags.items[1].section == NULL);
    CHECK(diags.items[1].secondary == NULL && diags.items[1].fixes == NULL);
    mo_diags_free(&diags);
}

static void test_sorting_goes_by_position(void) {
    struct mo_diags diags;
    CHECK(mo_diags_init(&diags));
    /* Added out of order, with two pairs at the same place. */
    const struct {
        uint32_t file;
        uint32_t start;
        const char *message;
    } added[] = {
        {1, 50, "f1 50"},        {0, 90, "f0 90"}, {0, 10, "f0 10 first"}, {2, 0, "f2 0"},
        {0, 10, "f0 10 second"}, {1, 5, "f1 5"},   {0, 90, "f0 90 again"}, {0, 0, "f0 0"},
    };
    for (size_t i = 0; i < sizeof added / sizeof added[0]; i++) {
        struct mo_diag diag =
            plain(MO_CODE_FAILURE_IGNORED, span(added[i].file, added[i].start, added[i].start + 3),
                  added[i].message);
        CHECK(mo_diags_add(&diags, &diag));
    }
    mo_diags_sort(&diags);
    const char *sorted[] = {"f0 0",        "f0 10 first", "f0 10 second", "f0 90",
                            "f0 90 again", "f1 5",        "f1 50",        "f2 0"};
    CHECK(diags.count == sizeof sorted / sizeof sorted[0]);
    for (uint32_t i = 0; i < diags.count && i < sizeof sorted / sizeof sorted[0]; i++) {
        CHECK(strcmp(diags.items[i].message, sorted[i]) == 0);
    }
    /* Sorting sorted diagnostics changes nothing. */
    mo_diags_sort(&diags);
    for (uint32_t i = 0; i < diags.count && i < sizeof sorted / sizeof sorted[0]; i++) {
        CHECK(strcmp(diags.items[i].message, sorted[i]) == 0);
    }
    mo_diags_free(&diags);
}

static void test_sorting_the_most_diagnostics(void) {
    /* MO_LIMIT_DIAGNOSTICS diagnostics in reverse order, up to three at each place. Each
     * message is a letter that says which came first at its place: a, then b, then c. */
    struct mo_diags diags;
    CHECK(mo_diags_init(&diags));
    char message[2];
    for (uint32_t i = 0; i < MO_LIMIT_DIAGNOSTICS; i++) {
        uint32_t place = (MO_LIMIT_DIAGNOSTICS - 1 - i) / 3;
        message[0] = (char)('a' + 2 - (MO_LIMIT_DIAGNOSTICS - 1 - i) % 3);
        message[1] = '\0';
        struct mo_diag diag =
            plain(MO_CODE_FAILURE_IGNORED, span(place % 4, place, place + 1), message);
        CHECK(mo_diags_add(&diags, &diag));
    }
    mo_diags_sort(&diags);
    bool ordered = diags.count == MO_LIMIT_DIAGNOSTICS;
    for (uint32_t i = 1; ordered && i < diags.count; i++) {
        struct mo_span before = diags.items[i - 1].primary.span;
        struct mo_span after = diags.items[i].primary.span;
        ordered =
            before.file < after.file || (before.file == after.file && before.start <= after.start);
        /* At one place, they keep the order they were added in. */
        if (ordered && same_span(before, after)) {
            ordered = diags.items[i - 1].message[0] < diags.items[i].message[0];
        }
    }
    CHECK(ordered);
    mo_diags_free(&diags);
}

static void test_the_number_of_diagnostics_is_limited(void) {
    struct mo_diags diags;
    CHECK(mo_diags_init(&diags));
    for (uint32_t i = 0; i < MO_LIMIT_DIAGNOSTICS; i++) {
        struct mo_diag diag = plain(MO_CODE_FAILURE_IGNORED, span(0, i, i), "kept");
        CHECK(mo_diags_add(&diags, &diag));
    }
    CHECK(diags.count == MO_LIMIT_DIAGNOSTICS && diags.dropped == 0);
    struct mo_diag one_more = plain(MO_CODE_FAILURE_IGNORED, span(0, 0, 0), "dropped");
    CHECK(!mo_diags_add(&diags, &one_more));
    CHECK(!mo_diags_add(&diags, &one_more));
    CHECK(diags.count == MO_LIMIT_DIAGNOSTICS && diags.dropped == 2);
    CHECK(strcmp(diags.items[MO_LIMIT_DIAGNOSTICS - 1].message, "kept") == 0);
    mo_diags_free(&diags);
}

static void test_the_memory_of_diagnostics_is_limited(void) {
    struct mo_diags diags;
    CHECK(mo_diags_init(&diags));
    /* A message of a mebibyte: the 64th can't fit beside the 63 before it. */
    enum { SIZE = 1 << 20 };
    static char message[SIZE + 1];
    memset(message, 'm', SIZE);
    uint32_t added = 0;
    while (added <= MO_LIMIT_DIAGNOSTICS_SIZE / SIZE) {
        struct mo_diag diag = plain(MO_CODE_FAILURE_IGNORED, span(0, added, added), message);
        if (!mo_diags_add(&diags, &diag)) {
            break;
        }
        added += 1;
    }
    CHECK(added > 0 && added < MO_LIMIT_DIAGNOSTICS_SIZE / SIZE);
    CHECK(diags.count == added && diags.dropped == 1);
    CHECK(diags.arena.used <= MO_LIMIT_DIAGNOSTICS_SIZE);
    /* A small one still fits, and what was kept is whole. */
    struct mo_diag small = plain(MO_CODE_FAILURE_IGNORED, span(0, 0, 0), "small");
    CHECK(mo_diags_add(&diags, &small));
    CHECK(strlen(diags.items[0].message) == SIZE);
    mo_diags_free(&diags);
}

static void test_a_dropped_diagnostic_takes_no_memory(void) {
    /* A diagnostic too big to keep, with a 40 MiB label and a 30 MiB fix, leaves room for one of
     * 60 MiB after it. */
    struct mo_diags diags;
    CHECK(mo_diags_init(&diags));
    static char label[40 << 20];
    static char replacement[30 << 20];
    static char message[60 << 20];
    memset(label, 'l', sizeof label - 1);
    memset(replacement, 'r', sizeof replacement - 1);
    memset(message, 'm', sizeof message - 1);
    struct mo_fix fix = {.span = span(0, 0, 0), .replacement = replacement};
    struct mo_diag too_big = plain(MO_CODE_FAILURE_IGNORED, span(0, 0, 0), "too big");
    too_big.primary.text = label;
    too_big.fixes = &fix;
    too_big.fix_count = 1;
    size_t used = diags.arena.used;
    CHECK(!mo_diags_add(&diags, &too_big));
    CHECK(diags.count == 0 && diags.dropped == 1 && diags.arena.used == used);
    struct mo_diag big = plain(MO_CODE_FAILURE_IGNORED, span(0, 0, 0), message);
    CHECK(mo_diags_add(&diags, &big));
    CHECK(diags.count == 1 && strlen(diags.items[0].message) == sizeof message - 1);
    mo_diags_free(&diags);
}

static void test_a_dropped_diagnostic_still_counts(void) {
    /* Even when nothing could be kept, the run knows something was wrong. */
    struct mo_diags diags;
    CHECK(mo_diags_init(&diags));
    static char message[MO_LIMIT_DIAGNOSTICS_SIZE];
    memset(message, 'm', sizeof message - 1);
    struct mo_diag diag = plain(MO_CODE_FAILURE_IGNORED, span(0, 0, 0), message);
    CHECK(!mo_diags_add(&diags, &diag));
    CHECK(diags.count == 0 && diags.dropped == 1);
    CHECK(mo_diags_any(&diags));
    mo_diags_free(&diags);
    CHECK(mo_diags_init(&diags));
    CHECK(!mo_diags_any(&diags));
    mo_diags_free(&diags);
}

int main(void) {
    test_it_starts_empty();
    test_it_owns_its_strings();
    test_the_section_is_the_codes();
    test_sorting_goes_by_position();
    test_sorting_the_most_diagnostics();
    test_the_number_of_diagnostics_is_limited();
    test_the_memory_of_diagnostics_is_limited();
    test_a_dropped_diagnostic_takes_no_memory();
    test_a_dropped_diagnostic_still_counts();
    return mo_test_report("test_diag");
}
