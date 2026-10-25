#include "base/codes.h"
#include "base/stack.h"
#include "format-contract.h"
#include "program_fixture.h"
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

    /* Diagnostics are taken in position order, not the order they were reported in. */
    start(&p, "abcdef");
    offer(&p, &shared[1], 1);
    offer(&p, &shared[0], 1);
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

static void service_alternatives(const char *source, const char *preserved, const char *label) {
    struct fixture original;
    init(&original, source);
    CHECK(original.program == NULL);
    CHECK(original.diags.count == 1);
    if (original.diags.count != 1) {
        free_fixture(&original);
        return;
    }
    const struct mo_diag *diagnostic = &original.diags.items[0];
    CHECK(diagnostic->code == MO_CODE_SERVICE_STATE_RESULT);
    if (diagnostic->code != MO_CODE_SERVICE_STATE_RESULT) {
        (void)fprintf(stderr, "service original: E%04u %s\n", diagnostic->code,
                      diagnostic->message);
        free_fixture(&original);
        return;
    }
    CHECK(strcmp(diagnostic->message, "a service tool returns only its state") == 0);
    CHECK(strcmp(diagnostic->primary.text, label) == 0);
    CHECK(strcmp(diagnostic->reason,
                 "A tool changes the state only by returning (state, reply). Returning the state "
                 "alone is how a timer changes it, so a tool's result would never be saved "
                 "(reference 11.5).") == 0);
    CHECK(strcmp(diagnostic->section, "11.5") == 0);
    CHECK(diagnostic->fix_count == 2);
    if (diagnostic->fix_count != 2) {
        free_fixture(&original);
        return;
    }
    CHECK(strcmp(diagnostic->fixes[0].title, "return (state, Unit) to save the new state") == 0);
    CHECK(strcmp(diagnostic->fixes[1].title,
                 "return the state inside a record, as read-only data") == 0);
    struct mo_buffer unchanged;
    mo_buffer_init(&unchanged, MO_LIMIT_FILE_SIZE);
    uint32_t applied;
    CHECK(mo_fix_source(&original.sources, 0, &original.diags, &unchanged, &applied));
    CHECK(applied == 0 && unchanged.length == strlen(source) &&
          memcmp(unchanged.bytes, source, unchanged.length) == 0);
    mo_buffer_free(&unchanged);

    for (uint32_t i = 0; i < 2; i++) {
        struct mo_diags selection;
        MO_ASSERT(mo_diags_init(&selection));
        struct mo_diag chosen = *diagnostic;
        chosen.fixes = &diagnostic->fixes[i];
        chosen.fix_count = 1;
        MO_ASSERT(mo_diags_add(&selection, &chosen));
        struct mo_buffer out;
        mo_buffer_init(&out, MO_LIMIT_FILE_SIZE);
        MO_ASSERT(mo_fix_source(&original.sources, 0, &selection, &out, &applied));
        CHECK(applied == 1);
        MO_ASSERT(mo_buffer_append_byte(&out, 0));
        CHECK(strstr((const char *)out.bytes, preserved) != NULL);
        struct fixture revised;
        init(&revised, (const char *)out.bytes);
        if (revised.program == NULL) {
            for (uint32_t j = 0; j < revised.diags.count; j++)
                (void)fprintf(stderr, "service alternative %u: E%04u %s\n", i,
                              revised.diags.items[j].code, revised.diags.items[j].message);
            (void)fprintf(stderr, "%s\n", out.bytes);
        }
        CHECK(revised.program != NULL && !mo_diags_any(&revised.diags));
        if (revised.program != NULL) {
            CHECK(revised.program->service_count == 1);
            const struct mo_checked_service *service = &revised.program->services[0];
            CHECK(service->tool_count == 1);
            const struct mo_symbol *adapter =
                revised.program->functions[service->tools[0].function].signature->function;
            size_t name_length = diagnostic->primary.span.end - diagnostic->primary.span.start;
            CHECK(strlen(adapter->name) == name_length &&
                  memcmp(adapter->name, source + diagnostic->primary.span.start, name_length) == 0);
            if (strstr(source, "## Public tool description.") != NULL) {
                const struct mo_syntax_comment *doc = adapter->declaration->leading;
                CHECK(doc != NULL && doc->documentation);
                if (doc != NULL)
                    CHECK(doc->span.end - doc->span.start ==
                              strlen("## Public tool description.") &&
                          memcmp(doc->text, "## Public tool description.",
                                 strlen("## Public tool description.")) == 0);
                const char *description =
                    strstr((const char *)out.bytes, "## Public tool description.");
                CHECK(description != NULL &&
                      strstr(description + 1, "## Public tool description.") == NULL);
            }
            CHECK(service->tools[0].reply == (i == 0 ? MO_SERVICE_UPDATE : MO_SERVICE_READ_ONLY));
            const struct mo_source_type *signature =
                revised.program->functions[service->tools[0].function].signature->type;
            const struct mo_source_type *result = signature->arguments[signature->count - 1];
            if (i == 0) {
                CHECK(result->kind == MO_SOURCE_TUPLE && result->count == 2);
                CHECK(result->arguments[0] == signature->arguments[0]);
                CHECK(strcmp(result->arguments[1]->name, "Unit") == 0);
            } else {
                CHECK(result->kind == MO_SOURCE_NAMED && result->declaration != NULL);
                bool field_found = false;
                for (uint32_t j = 0; j < revised.program->signatures->data_count; j++) {
                    const struct mo_data_signature *data = &revised.program->signatures->data[j];
                    if (data->symbol == result->declaration) {
                        CHECK(data->count == 1 &&
                              data->by_index[0]->type == signature->arguments[0]);
                        field_found = true;
                    }
                }
                CHECK(field_found);
            }
            struct mo_buffer formatted, twice;
            mo_buffer_init(&formatted, MO_LIMIT_FILE_SIZE);
            mo_buffer_init(&twice, MO_LIMIT_FILE_SIZE);
            CHECK(mo_format(revised.module, (struct mo_format_options){100, format_display_width},
                            &formatted));
            MO_ASSERT(mo_buffer_append_byte(&formatted, 0));
            struct fixture stable;
            init(&stable, (const char *)formatted.bytes);
            CHECK(stable.program != NULL && !mo_diags_any(&stable.diags));
            CHECK(mo_format(stable.module, (struct mo_format_options){100, format_display_width},
                            &twice));
            CHECK(formatted.length == twice.length + 1 &&
                  memcmp(formatted.bytes, twice.bytes, twice.length) == 0);
            free_fixture(&stable);
            mo_buffer_free(&formatted);
            mo_buffer_free(&twice);
        }
        free_fixture(&revised);
        mo_buffer_free(&out);
        mo_diags_free(&selection);
    }
    free_fixture(&original);
}

static void service_fixes(void) {
    service_alternatives(
        "module Main\nstruct Box(T)\nvalue: T\nend\n"
        "service\nstate Box(Int) = Box(value: 0)\ntools change\n"
        "token from env \"TOKEN\"\nend\n"
        "fn change(state: Box(Int), _now: Time, amount: Int) : Box(Int)\n"
        "return state if amount == 0\n"
        "nested = fn(x)\nfn(y)\nreturn x + y\nend\nend\n"
        "Box(value: nested(state.value)(amount))\nend\n"
        "fn direct(state: Box(Int), now: Time) : Int\nchange(state, now, 2).value\nend\n"
        "fn service_tool_0_0()\nend\nstruct ServiceReply0Case0\nvalue: Int\nend\n",
        "fn service_tool_0_1(state: Box(Int), _now: Time, amount: Int) : Box(Int)\n"
        "return state if amount == 0\n"
        "nested = fn(x)\nfn(y)\nreturn x + y\nend\nend\n"
        "Box(value: nested(state.value)(amount))\nend\n"
        "fn direct(state: Box(Int), now: Time) : Int\n"
        "service_tool_0_1(state, now, 2).value\nend\n",
        "a timer returns this; a tool returns (Box(Int), reply)");
    service_alternatives(
        "module Main\nservice\nstate (Int, Text) = (0, \"\")\ntools service_argument0\n"
        "token from env \"TOKEN\"\nend\n"
        "fn service_argument0(state: (Int, Text), _now: Time) : (Int, Text)\nstate\nend\n"
        "fn direct(state: (Int, Text), now: Time) : (Int, Text)\n"
        "service_argument0(state, now)\nend\n",
        "fn direct(state: (Int, Text), now: Time) : (Int, Text)\n"
        "service_tool_0_0(state, now)\nend\n",
        "a timer returns this; a tool returns ((Int, Text), reply)");
    service_alternatives("module Main\nservice\nstate Unit = Unit\ntools idle\n"
                         "token from env \"TOKEN\"\nend\n"
                         "fn idle(_state: Unit, _now: Time)\nend\n",
                         "fn service_tool_0_0(_state: Unit, _now: Time)\nend\n",
                         "a timer returns this; a tool returns (Unit, reply)");
    service_alternatives(
        "module Main\nstruct State\nvalue: Int\nend\n"
        "struct Alternate\nvalue: Int\nend\n"
        "service\nstate State = change(State(value: 0), instant, 0)\ntools change\n"
        "every 1.second tick\n"
        "token from env \"TOKEN\"\nend\n"
        "## Public tool description.\n"
        "fn change(state: State, now: Time, amount: Int) : State\n"
        "examples\nchange(State(value: 0), instant, 0).value == 0\nend\n"
        "promise \"keeps zero\"\nchange(state, now, 0).value == result.value\n"
        "allows state: State(value: 0), now: instant, amount: 0, result: State(value: 0)\n"
        "forbids state: State(value: 0), now: instant, amount: 0, result: State(value: 1)\nend\n"
        "return state if amount == 0\nchange(state, now, amount - 1)\nend\n"
        "fn change(state: Alternate, _now: Time, _amount: Int) : Int\nstate.value\nend\n"
        "instant = Time.parse(\"2026-10-25T00:00:00Z\") or stop \"valid time\"\n"
        "saved = change(State(value: 0), instant, 0)\n"
        "fn tick(_state: State, _now: Time) : State\nsaved\nend\n"
        "fn direct(state: State, now: Time) : State\n"
        "service_tool_0_0 = 2\n"
        "callback = change\ncallback(state, now, service_tool_0_0)\nend\n"
        "fn method(state: State, now: Time) : State\nstate.change(now, 0)\nend\n"
        "fn shadow(state: State, now: Time) : State\n"
        "change = fn(s, _t, _n) s end\nchange(state, now, 0)\nend\n"
        "fn other(state: Alternate, now: Time) : Int\nchange(state, now, 1)\nend\n",
        "fn other(state: Alternate, now: Time) : Int\nchange(state, now, 1)\nend\n",
        "a timer returns this; a tool returns (State, reply)");
    struct fixture exposed;
    init(&exposed, "module Main\nexpose change\n"
                   "service\nstate Int = 0\ntools change\ntoken from env \"TOKEN\"\nend\n"
                   "fn change(state: Int, _now: Time) : Int\nstate\nend\n");
    CHECK(exposed.program == NULL && exposed.diags.count == 1);
    if (exposed.diags.count == 1) {
        CHECK(exposed.diags.items[0].code == MO_CODE_SERVICE_STATE_RESULT);
        CHECK(exposed.diags.items[0].fix_count == 0);
    }
    free_fixture(&exposed);

    const char *predicate =
        "module Main\nservice\nstate Bool = false\ntools active?\n"
        "token from env \"TOKEN\"\nend\n"
        "## Toggle.\nfn active?(state: Bool, _now: Time) : Bool\nnot state\nend\n";
    struct fixture named;
    init(&named, predicate);
    CHECK(named.program == NULL && named.diags.count == 1);
    if (named.diags.count == 1) {
        const struct mo_diag *diagnostic = &named.diags.items[0];
        CHECK(diagnostic->code == MO_CODE_SERVICE_STATE_RESULT && diagnostic->fix_count == 0);
        struct mo_buffer unchanged;
        mo_buffer_init(&unchanged, MO_LIMIT_FILE_SIZE);
        uint32_t applied;
        CHECK(mo_fix_source(&named.sources, 0, &named.diags, &unchanged, &applied));
        CHECK(applied == 0 && unchanged.length == strlen(predicate) &&
              memcmp(unchanged.bytes, predicate, unchanged.length) == 0);
        mo_buffer_free(&unchanged);
    }
    free_fixture(&named);
}

static void service_fix_line_limit(void) {
    const char *call = "change(state, now)";
    size_t growth = strlen("service_tool_0_0") - strlen("change");
    for (uint32_t over = 0; over < 2; over++) {
        struct mo_buffer source;
        mo_buffer_init(&source, MO_LIMIT_FILE_SIZE);
        MO_ASSERT(mo_buffer_append_text(&source,
                                        "module Main\nservice\nstate Int = 0\ntools change\n"
                                        "token from env \"TOKEN\"\nend\n"
                                        "fn change(state: Int, _now: Time) : Int\nstate\nend\n"
                                        "fn direct(state: Int, now: Time) : Int\n"));
        size_t spaces = MO_LIMIT_LINE_LENGTH - strlen(call) - growth + over;
        for (size_t i = 0; i < spaces; i++)
            MO_ASSERT(mo_buffer_append_byte(&source, ' '));
        MO_ASSERT(mo_buffer_append_text(&source, call) &&
                  mo_buffer_append_text(&source, "\nend\n") && mo_buffer_append_byte(&source, 0));
        if (over == 0) {
            service_alternatives((const char *)source.bytes, "tools change\n",
                                 "a timer returns this; a tool returns (Int, reply)");
        } else {
            struct fixture refused;
            init(&refused, (const char *)source.bytes);
            CHECK(refused.program == NULL && refused.diags.count == 1);
            if (refused.diags.count == 1)
                CHECK(refused.diags.items[0].code == MO_CODE_LINE_TOO_LONG &&
                      refused.diags.items[0].fix_count == 0);
            free_fixture(&refused);
        }
        mo_buffer_free(&source);
    }
}

static void service_fix_type_limit(void) {
    for (uint32_t depth = MO_LIMIT_SOURCE_TYPE_DEPTH - 3; depth <= MO_LIMIT_SOURCE_TYPE_DEPTH - 2;
         depth++) {
        struct mo_buffer type, source, label;
        mo_buffer_init(&type, MO_LIMIT_FILE_SIZE);
        mo_buffer_init(&source, MO_LIMIT_FILE_SIZE);
        mo_buffer_init(&label, MO_LIMIT_FILE_SIZE);
        for (uint32_t i = 0; i < depth; i++)
            MO_ASSERT(mo_buffer_append_text(&type, "List("));
        MO_ASSERT(mo_buffer_append_text(&type, "Int"));
        for (uint32_t i = 0; i < depth; i++)
            MO_ASSERT(mo_buffer_append_byte(&type, ')'));
        MO_ASSERT(mo_buffer_append_byte(&type, 0));
        const char *spelling = (const char *)type.bytes;
        MO_ASSERT(mo_buffer_append_text(&source, "module Main\nservice\nstate ") &&
                  mo_buffer_append_text(&source, spelling) &&
                  mo_buffer_append_text(&source, " = stop \"start\"\ntools change\n"
                                                 "token from env \"TOKEN\"\nend\n"
                                                 "fn change(state: ") &&
                  mo_buffer_append_text(&source, spelling) &&
                  mo_buffer_append_text(&source, ", _now: Time) : ") &&
                  mo_buffer_append_text(&source, spelling) &&
                  mo_buffer_append_text(&source, "\nstate\nend\n") &&
                  mo_buffer_append_byte(&source, 0));
        if (depth == MO_LIMIT_SOURCE_TYPE_DEPTH - 3) {
            MO_ASSERT(mo_buffer_append_text(&label, "a timer returns this; a tool returns (") &&
                      mo_buffer_append_text(&label, spelling) &&
                      mo_buffer_append_text(&label, ", reply)") &&
                      mo_buffer_append_byte(&label, 0));
            service_alternatives((const char *)source.bytes, "tools change\n",
                                 (const char *)label.bytes);
        } else {
            struct fixture refused;
            init(&refused, (const char *)source.bytes);
            CHECK(refused.program == NULL && refused.diags.count == 1);
            if (refused.diags.count == 1)
                CHECK(refused.diags.items[0].code == MO_CODE_CHECK_TYPE_DEPTH &&
                      refused.diags.items[0].fix_count == 0);
            free_fixture(&refused);
        }
        mo_buffer_free(&label);
        mo_buffer_free(&source);
        mo_buffer_free(&type);
    }
}

static void run(void *unused) {
    (void)unused;
    single_fixes();
    skipped_fixes();
    overlapping_fixes();
    service_fixes();
    service_fix_line_limit();
    service_fix_type_limit();
}

int main(void) {
    CHECK(mo_with_stack(MO_SOURCE_STACK_SIZE, run, NULL) == 0);
    return mo_test_report("test_fix");
}
