/* The tests of mistakes that must stop the program run them in a child process, with POSIX. */
#define _POSIX_C_SOURCE 200809L

#include "base/codes.h"
#include "base/diag.h"
#include "base/limits.h"
#include "test.h"

#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static struct mo_span span(uint32_t file, uint32_t start, uint32_t end) {
    return (struct mo_span){.file = file, .start = start, .end = end};
}

static bool same_span(struct mo_span a, struct mo_span b) {
    return a.file == b.file && a.start == b.start && a.end == b.end;
}

/* A diagnostic with only a code, a primary span, a headline and a reason. */
static struct mo_diag plain(uint16_t code, struct mo_span primary, const char *message) {
    return (struct mo_diag){.code = code,
                            .primary = {.span = primary},
                            .message = message,
                            .reason = "Mo never drops a failure silently (reference 8.3)."};
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
    char reason[] = "Mo never drops a failure silently (reference 8.3).";
    char primary_text[] = "returns Result(Unit, FileError), and the result is dropped";
    char secondary_text[] = "declared here";
    char title[] = "pass the failure upward with try";
    char replacement[] = "try ";
    char comment[] = "  # the report is written";
    struct mo_label secondary[] = {{.span = span(1, 5, 9), .text = secondary_text},
                                   {.span = span(0, 2, 2), .text = NULL}};
    /* Two fixes: one of two edits, an insertion and a replacement, and one of a single edit. */
    struct mo_edit upward[] = {{.span = span(0, 100, 100), .replacement = replacement},
                               {.span = span(0, 138, 140), .replacement = comment}};
    struct mo_edit handled[] = {{.span = span(0, 100, 138), .replacement = ""}};
    struct mo_fix fixes[] = {{.title = title, .edits = upward, .edit_count = 2},
                             {.title = NULL, .edits = handled, .edit_count = 1}};
    struct mo_diag diag = {.code = MO_CODE_FAILURE_IGNORED,
                           .primary = {.span = span(0, 100, 138), .text = primary_text},
                           .secondary = secondary,
                           .secondary_count = 2,
                           .message = message,
                           .reason = reason,
                           .fixes = fixes,
                           .fix_count = 2};
    CHECK(mo_diags_add(&diags, &diag));
    memset(message, '?', sizeof message - 1);
    memset(reason, '?', sizeof reason - 1);
    memset(primary_text, '?', sizeof primary_text - 1);
    memset(secondary_text, '?', sizeof secondary_text - 1);
    memset(title, '?', sizeof title - 1);
    memset(replacement, '?', sizeof replacement - 1);
    memset(comment, '?', sizeof comment - 1);
    memset(secondary, 0, sizeof secondary);
    memset(upward, 0, sizeof upward);
    memset(handled, 0, sizeof handled);
    memset(fixes, 0, sizeof fixes);
    memset(&diag, 0, sizeof diag);

    CHECK(diags.count == 1 && mo_diags_any(&diags));
    const struct mo_diag *kept = &diags.items[0];
    CHECK(kept->code == 412);
    CHECK(strcmp(kept->message, "a failure is ignored") == 0 && kept->message != message);
    CHECK(strcmp(kept->reason, "Mo never drops a failure silently (reference 8.3).") == 0);
    CHECK(same_span(kept->primary.span, span(0, 100, 138)));
    CHECK(strcmp(kept->primary.text,
                 "returns Result(Unit, FileError), and the result is dropped") == 0);
    CHECK(kept->secondary_count == 2 && kept->secondary != secondary);
    CHECK(same_span(kept->secondary[0].span, span(1, 5, 9)));
    CHECK(strcmp(kept->secondary[0].text, "declared here") == 0);
    CHECK(same_span(kept->secondary[1].span, span(0, 2, 2)) && kept->secondary[1].text == NULL);
    CHECK(kept->fix_count == 2 && kept->fixes != fixes);
    const struct mo_fix *first = &kept->fixes[0];
    CHECK(strcmp(first->title, "pass the failure upward with try") == 0);
    CHECK(first->edit_count == 2 && first->edits != upward);
    CHECK(same_span(first->edits[0].span, span(0, 100, 100)));
    CHECK(strcmp(first->edits[0].replacement, "try ") == 0);
    CHECK(same_span(first->edits[1].span, span(0, 138, 140)));
    CHECK(strcmp(first->edits[1].replacement, "  # the report is written") == 0);
    const struct mo_fix *second = &kept->fixes[1];
    CHECK(second->title == NULL && second->edit_count == 1);
    CHECK(same_span(second->edits[0].span, span(0, 100, 138)));
    CHECK(strcmp(second->edits[0].replacement, "") == 0);
    /* The section is the code's. */
    CHECK(kept->section != NULL && strcmp(kept->section, "8.3") == 0);
    mo_diags_free(&diags);
}

/* Adds a diagnostic with one fix of the edits given, and returns whether it was kept. */
static bool add_fix(const struct mo_edit *edits, uint32_t count) {
    struct mo_diags diags;
    if (!mo_diags_init(&diags)) {
        return false;
    }
    struct mo_fix fix = {.title = "a fix", .edits = edits, .edit_count = count};
    struct mo_diag diag = plain(MO_CODE_FAILURE_IGNORED, span(0, 0, 0), "with a fix");
    diag.fixes = &fix;
    diag.fix_count = 1;
    bool added = mo_diags_add(&diags, &diag);
    /* The edits are kept in the order given. */
    for (uint32_t i = 0; added && i < count; i++) {
        added = same_span(diags.items[0].fixes[0].edits[i].span, edits[i].span);
    }
    mo_diags_free(&diags);
    return added;
}

static void test_edits_that_are_apart(void) {
    /* Edits that touch, given in any order. */
    const struct mo_edit touching[] = {{.span = span(0, 20, 30), .replacement = "b"},
                                       {.span = span(0, 10, 20), .replacement = "a"}};
    CHECK(add_fix(touching, 2));
    /* Insertions at either end of a replacement, which come before or after it. */
    const struct mo_edit ends[] = {{.span = span(0, 10, 10), .replacement = "("},
                                   {.span = span(0, 10, 20), .replacement = "x"},
                                   {.span = span(0, 20, 20), .replacement = ")"}};
    CHECK(add_fix(ends, 3));
    /* The same places in two files. */
    const struct mo_edit files[] = {{.span = span(0, 5, 9), .replacement = "use Shop.Orders"},
                                    {.span = span(1, 5, 9), .replacement = "expose total"},
                                    {.span = span(1, 0, 0), .replacement = "## "},
                                    {.span = span(0, 0, 0), .replacement = "## "}};
    CHECK(add_fix(files, 4));
    /* As many edits as a fix may have: insertions at as many places. */
    static struct mo_edit most[MO_LIMIT_FIX_EDITS];
    for (uint32_t i = 0; i < MO_LIMIT_FIX_EDITS; i++) {
        most[i] = (struct mo_edit){.span = span(0, i, i), .replacement = "x"};
    }
    CHECK(add_fix(most, MO_LIMIT_FIX_EDITS));
}

/* Whether adding diag stops the program on a failed assertion, as a mistake in the stage that made
 * it must. It runs in a child process, whose message is thrown away. */
static bool add_fails_an_assertion(const struct mo_diag *diag) {
    if (fflush(NULL) != 0) {
        return false;
    }
    pid_t child = fork();
    if (child < 0) {
        return false;
    }
    if (child == 0) {
        if (freopen("/dev/null", "w", stderr) == NULL) {
            _exit(2);
        }
        struct mo_diags diags;
        if (!mo_diags_init(&diags)) {
            _exit(3);
        }
        bool added = mo_diags_add(&diags, diag);
        _exit(added ? 0 : 1);
    }
    int status = 0;
    if (waitpid(child, &status, 0) != child) {
        return false;
    }
    return WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
}

/* Whether a diagnostic whose one fix has these edits stops the program. */
static bool fix_fails_an_assertion(const struct mo_edit *edits, uint32_t count) {
    struct mo_fix fix = {.title = NULL, .edits = edits, .edit_count = count};
    struct mo_diag diag = plain(MO_CODE_FAILURE_IGNORED, span(0, 0, 0), "with a fix");
    diag.fixes = &fix;
    diag.fix_count = 1;
    return add_fails_an_assertion(&diag);
}

static void test_mistakes_stop_the_program(void) {
    /* A diagnostic made right passes, so the check below can tell. */
    struct mo_diag right = plain(MO_CODE_FAILURE_IGNORED, span(0, 1, 2), "made right");
    CHECK(!add_fails_an_assertion(&right));

    /* Edits that overlap: sharing a byte, an insertion inside a replacement, and two insertions
     * at one place, whose order would be unclear. */
    const struct mo_edit sharing[] = {{.span = span(0, 10, 20), .replacement = "a"},
                                      {.span = span(0, 15, 25), .replacement = "b"}};
    CHECK(fix_fails_an_assertion(sharing, 2));
    const struct mo_edit inside[] = {{.span = span(0, 10, 20), .replacement = "a"},
                                     {.span = span(0, 15, 15), .replacement = "b"}};
    CHECK(fix_fails_an_assertion(inside, 2));
    const struct mo_edit same_place[] = {{.span = span(2, 7, 7), .replacement = "a"},
                                         {.span = span(0, 1, 3), .replacement = "b"},
                                         {.span = span(2, 7, 7), .replacement = "c"}};
    CHECK(fix_fails_an_assertion(same_place, 3));
    const struct mo_edit same_span[] = {{.span = span(0, 4, 6), .replacement = "a"},
                                        {.span = span(0, 4, 6), .replacement = "a"}};
    CHECK(fix_fails_an_assertion(same_span, 2));

    /* A fix with no edits, or with more than a fix may have, and an edit with no replacement. */
    CHECK(fix_fails_an_assertion(NULL, 0));
    static struct mo_edit too_many[MO_LIMIT_FIX_EDITS + 1];
    for (uint32_t i = 0; i <= MO_LIMIT_FIX_EDITS; i++) {
        too_many[i] = (struct mo_edit){.span = span(0, i, i), .replacement = "x"};
    }
    CHECK(fix_fails_an_assertion(too_many, MO_LIMIT_FIX_EDITS + 1));
    const struct mo_edit no_replacement[] = {{.span = span(0, 1, 2), .replacement = NULL}};
    CHECK(fix_fails_an_assertion(no_replacement, 1));

    /* A reason missing where the code cites a section, a section that isn't the code's, a code
     * that isn't registered, and a span that ends before it starts. */
    struct mo_diag no_reason = plain(MO_CODE_FAILURE_IGNORED, span(0, 1, 2), "no reason");
    no_reason.reason = NULL;
    CHECK(add_fails_an_assertion(&no_reason));
    struct mo_diag wrong_section = plain(MO_CODE_FAILURE_IGNORED, span(0, 1, 2), "wrong");
    wrong_section.section = "8.2";
    CHECK(add_fails_an_assertion(&wrong_section));
    struct mo_diag unregistered = plain(413, span(0, 1, 2), "unregistered");
    CHECK(add_fails_an_assertion(&unregistered));
    struct mo_diag backward = plain(MO_CODE_FAILURE_IGNORED, span(0, 5, 4), "backward");
    CHECK(add_fails_an_assertion(&backward));
}

static void test_the_section_is_the_codes(void) {
    struct mo_diags diags;
    CHECK(mo_diags_init(&diags));
    struct mo_diag cited = plain(MO_CODE_FAILURE_IGNORED, span(0, 0, 1), "cited");
    cited.section = "8.3";
    CHECK(mo_diags_add(&diags, &cited));
    /* A limit's code cites no section, and needs no reason. */
    struct mo_diag limit = plain(MO_CODE_LINE_TOO_LONG, span(0, 0, 5000), "a line too long");
    limit.reason = NULL;
    CHECK(mo_diags_add(&diags, &limit));
    CHECK(diags.count == 2);
    CHECK(diags.items[0].section != NULL && strcmp(diags.items[0].section, "8.3") == 0);
    CHECK(diags.items[1].section == NULL && diags.items[1].reason == NULL);
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
    struct mo_edit edit = {.span = span(0, 0, 0), .replacement = replacement};
    struct mo_fix fix = {.title = NULL, .edits = &edit, .edit_count = 1};
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
    test_edits_that_are_apart();
    test_mistakes_stop_the_program();
    test_the_section_is_the_codes();
    test_sorting_goes_by_position();
    test_sorting_the_most_diagnostics();
    test_the_number_of_diagnostics_is_limited();
    test_the_memory_of_diagnostics_is_limited();
    test_a_dropped_diagnostic_takes_no_memory();
    test_a_dropped_diagnostic_still_counts();
    return mo_test_report("test_diag");
}
