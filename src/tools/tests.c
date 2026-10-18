#define _POSIX_C_SOURCE 200809L

#include "tools/tests.h"

#include "base/codes.h"
#include "tools/promises.h"
#include "tools/run.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

const char *mo_observation_kind(enum mo_checked_spec_kind kind) {
    switch (kind) {
    case MO_SPEC_TEST:
        return "test";
    case MO_SPEC_EXAMPLE:
        return "example";
    case MO_SPEC_PROMISE:
        return "promise";
    case MO_SPEC_ANCHOR:
        break;
    }
    MO_UNREACHABLE();
}

static char *name(struct mo_arena *arena, const struct mo_program *program,
                  const struct mo_checked_spec *spec, size_t *work) {
    struct mo_buffer buffer;
    mo_buffer_init(&buffer, *work < MO_CLI_DIAGNOSTIC_SIZE ? *work : MO_CLI_DIAGNOSTIC_SIZE);
    bool ok = mo_buffer_append_text(&buffer, program->set->names[spec->module]);
    if (spec->subject != UINT32_MAX) {
        const struct mo_symbol *symbol = program->functions[spec->subject].signature->function;
        ok = ok && mo_buffer_append_text(&buffer, ".") &&
             mo_buffer_append_text(&buffer, symbol->name);
        const struct mo_module_scope *scope = &program->interfaces->scopes[symbol->module];
        const struct mo_symbol *group =
            mo_find_symbol(scope, (const uint8_t *)symbol->name, symbol->name_length);
        MO_ASSERT(group != NULL);
        if (symbol->receiver != UINT32_MAX && group->overload_count > 1)
            ok = ok && mo_buffer_append_text(&buffer, "@") &&
                 mo_buffer_append_text(&buffer, scope->symbols[symbol->receiver].name);
    }
    ok = ok && mo_buffer_append_text(&buffer, ".") &&
         mo_buffer_append_text(&buffer, mo_observation_kind(spec->kind)) &&
         mo_buffer_append_text(&buffer, ".") && mo_buffer_append_uint(&buffer, spec->number) &&
         mo_buffer_append_byte(&buffer, 0);
    *work -= buffer.length;
    char *result = ok ? mo_arena_copy(arena, buffer.bytes, buffer.length, 1) : NULL;
    mo_buffer_free(&buffer);
    return result;
}

struct mo_test_selection *mo_select_tests(struct mo_arena *arena, const struct mo_program *program,
                                          const struct mo_cli *cli, struct mo_diags *diags) {
    if (program->spec_count > MO_LIMIT_CORE_NODES)
        goto exhausted;
    struct mo_test_selection *selection =
        mo_arena_allocate(arena, sizeof(*selection), _Alignof(struct mo_test_selection));
    if (selection == NULL)
        goto exhausted;
    *selection = (struct mo_test_selection){0};
    selection->items = mo_arena_allocate_array(
        arena, program->spec_count, sizeof(*selection->items), _Alignof(struct mo_observation));
    if (selection->items == NULL)
        goto exhausted;
    bool found[MO_CLI_ARGUMENTS] = {false};
    size_t lengths[MO_CLI_ARGUMENTS];
    for (uint32_t i = 0; i < cli->only_count; i++)
        lengths[i] = strlen(cli->only[i]);
    size_t work = MO_CLI_TEST_WORK;
    for (uint32_t i = 0; i < program->spec_count; i++) {
        const struct mo_checked_spec *spec = &program->specs[i];
        if (spec->kind == MO_SPEC_ANCHOR)
            continue;
        /* A program's tests don't include the toolchain library's; `mo test lib/list.mo` does. */
        if (spec->module != 0 && program->set->library[spec->module])
            continue;
        char *full = name(arena, program, spec, &work);
        if (full == NULL)
            goto exhausted;
        bool selected = cli->only_count == 0;
        size_t length = strlen(full);
        for (uint32_t j = 0; j < cli->only_count; j++) {
            size_t cost = length == lengths[j] ? length + 1 : 1;
            if (cost > work)
                goto exhausted;
            work -= cost;
            if (length == lengths[j] && memcmp(full, cli->only[j], length) == 0)
                selected = found[j] = true;
        }
        selected = selected && (cli->module == NULL ||
                                strcmp(cli->module, program->set->names[spec->module]) == 0);
        if (selected)
            selection->items[selection->count++] = (struct mo_observation){full, spec};
    }
    for (uint32_t i = 0; i < cli->only_count; i++)
        if (!found[i])
            selection->mistake = "--only does not name an example, promise or test";
    if (selection->mistake == NULL && selection->count == 0)
        selection->mistake = "test selection matches no examples, promises or tests";
    return selection;
exhausted:;
    struct mo_diag diag = {.code = MO_CODE_TOOL_PIPELINE,
                           .message = "test inventory exceeds tool resources",
                           .primary.span.file = UINT32_MAX};
    bool added = mo_diags_add(diags, &diag);
    (void)added;
    return NULL;
}

static void report_error(struct mo_diags *diags, const char *message, struct mo_span span,
                         const char *detail) {
    struct mo_diag diag = {
        .code = MO_CODE_TOOL_PIPELINE, .message = message, .primary = {span, detail}};
    bool added = mo_diags_add(diags, &diag);
    (void)added;
}

struct observation_index {
    struct mo_span span;
    enum mo_function_kind kind;
    uint32_t selected;
};

/* Numeric declaration keys avoid repeated full scans and long-prefix name
 * comparisons. Names are still checked byte-for-byte before using a match. */
static int compare_observations(const void *a, const void *b) {
    const struct observation_index *left = a, *right = b;
    uint32_t l[] = {left->span.file, left->span.start, left->span.end, (uint32_t)left->kind};
    uint32_t r[] = {right->span.file, right->span.start, right->span.end, (uint32_t)right->kind};
    for (unsigned i = 0; i < 4; i++)
        if (l[i] != r[i])
            return l[i] < r[i] ? -1 : 1;
    return 0;
}

static uint32_t *bind_observations(struct mo_arena *arena, const struct mo_bytecode *file,
                                   const struct mo_sources *sources,
                                   const struct mo_test_selection *selection) {
    struct observation_index *index = mo_arena_allocate_array(
        arena, selection->count, sizeof(*index), _Alignof(struct observation_index));
    uint32_t *functions =
        mo_arena_allocate_array(arena, selection->count, sizeof(*functions), _Alignof(uint32_t));
    if (index == NULL || functions == NULL)
        return NULL;
    for (uint32_t i = 0; i < selection->count; i++) {
        const struct mo_checked_spec *source = selection->items[i].source;
        index[i] =
            (struct observation_index){source->syntax->span,
                                       source->kind == MO_SPEC_TEST      ? MO_FUNCTION_TEST
                                       : source->kind == MO_SPEC_PROMISE ? MO_FUNCTION_PROMISE
                                                                         : MO_FUNCTION_EXAMPLE,
                                       i};
        functions[i] = MO_NO_INDEX;
    }
    qsort(index, selection->count, sizeof(*index), compare_observations);
    for (uint32_t i = 0; i < file->function_count; i++) {
        const struct mo_bytecode_function *function = &file->functions[i];
        if (function->kind != MO_FUNCTION_TEST && function->kind != MO_FUNCTION_EXAMPLE &&
            function->kind != MO_FUNCTION_PROMISE)
            continue;
        const struct mo_bytecode_debug *debug = &file->debug[i];
        struct observation_index key = {
            .span = mo_runtime_span(file, sources, debug->file, debug->start, debug->end),
            .kind = function->kind};
        struct observation_index *match =
            bsearch(&key, index, selection->count, sizeof(*index), compare_observations);
        if (match == NULL)
            continue;
        const struct mo_bytecode_string *full = &file->strings[function->name];
        const struct mo_observation *observation = &selection->items[match->selected];
        const char *selected = observation->name;
        bool promise = function->kind == MO_FUNCTION_PROMISE;
        if (function->parameter_count != (promise ? observation->source->parameter_count : 0) ||
            file->types[function->result_type].kind != (promise ? MO_TYPE_INT : MO_TYPE_UNIT) ||
            full->length != strlen(selected) || memcmp(full->bytes, selected, full->length) != 0 ||
            functions[match->selected] != MO_NO_INDEX)
            return NULL;
        functions[match->selected] = i;
    }
    for (uint32_t i = 0; i < selection->count; i++)
        if (functions[i] == MO_NO_INDEX)
            return NULL;
    return functions;
}

static struct mo_span assertion_span(const struct mo_checked_spec *spec, struct mo_span failed) {
    struct mo_span result = {UINT32_MAX, 0, 0};
    for (uint32_t i = 0; i < spec->body.count; i++) {
        const struct mo_syntax *syntax = spec->body.items[i].syntax;
        struct mo_span span = syntax->span;
        if (syntax->kind == MO_S_ASSERT && span.file == failed.file && span.start <= failed.start &&
            span.end >= failed.end &&
            (result.file == UINT32_MAX || span.end - span.start < result.end - result.start))
            result = span;
    }
    return result;
}

static uint64_t microseconds(struct timespec start, struct timespec end) {
    MO_ASSERT(end.tv_sec >= start.tv_sec);
    uint64_t seconds = (uint64_t)end.tv_sec - (uint64_t)start.tv_sec;
    long nanos = end.tv_nsec - start.tv_nsec;
    if (nanos < 0) {
        MO_ASSERT(seconds != 0);
        seconds--;
        nanos += 1000000000;
    }
    MO_ASSERT(seconds <= (UINT64_MAX - 999999) / 1000000);
    return seconds * 1000000 + (uint64_t)nanos / 1000;
}

void mo_test_program(const struct mo_cli *cli, const struct mo_program *program,
                     const struct mo_test_selection *selection, const struct mo_buffer *wire,
                     const struct mo_sources *sources, FILE *output, struct mo_diags *diags,
                     struct mo_test_totals *totals) {
    struct mo_load_error error;
    struct mo_image *image = mo_load(wire->bytes, wire->length, &error);
    if (image == NULL) {
        report_error(diags, "compiled bytecode could not be loaded",
                     (struct mo_span){UINT32_MAX, 0, 0}, error.reason);
        return;
    }
    struct mo_arena scratch;
    mo_arena_init(&scratch, MO_CLI_TEST_SIZE);
    const struct mo_bytecode *file = mo_image_bytecode(image);
    uint32_t *functions = bind_observations(&scratch, file, sources, selection);
    if (functions == NULL) {
        report_error(diags, "selected observations could not be bound to compiled functions",
                     (struct mo_span){UINT32_MAX, 0, 0}, NULL);
        goto done;
    }
    bool anchors = false;
    for (uint32_t i = 0; i < file->function_count; i++)
        anchors |= file->functions[i].kind == MO_FUNCTION_ANCHOR;
    struct mo_defect defect;
    enum mo_outcome initialized =
        anchors ? mo_image_init_bounded(image, &cli->limits, MO_CLI_ANCHOR_INSTRUCTIONS, &defect)
                : mo_image_init(image, &cli->limits, &defect);
    if (initialized != MO_RETURNED) {
        struct mo_buffer report;
        mo_buffer_init(&report, MO_LIMIT_DEFECT_TEXT_SIZE + 1);
        bool complete = mo_render_defect(NULL, &defect, &report);
        const char *detail = defect.reason;
        if (complete && mo_buffer_append_byte(&report, 0))
            detail = (const char *)report.bytes;
        struct mo_diag diag = {.code = MO_CODE_TOOL_TEST_INITIALIZATION,
                               .message = "program initialization stopped before tests could run",
                               .primary = {mo_runtime_span(file, sources, defect.span.file,
                                                           defect.span.start, defect.span.end),
                                           detail}};
        bool added = mo_diags_add(diags, &diag);
        (void)added;
        mo_buffer_free(&report);
        goto done;
    }
    if (!mo_check_anchors(image, cli, sources, diags))
        goto done;
    for (uint32_t i = 0; i < selection->count; i++) {
        const struct mo_observation *observation = &selection->items[i];
        uint32_t function = functions[i];
        if (observation->source->kind == MO_SPEC_PROMISE) {
            if (!mo_test_promise(cli, program, image, sources, observation, function, output, diags,
                                 totals))
                break;
            continue;
        }
        struct timespec start, end;
        if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
            report_error(diags, "could not measure test execution",
                         observation->source->syntax->span, strerror(errno));
            break;
        }
        errno = 0;
        struct mo_process *process = mo_process_new(image, &cli->limits);
        struct mo_cell value;
        struct mo_test_result result = {.observation = observation,
                                        .file = file,
                                        .process = process,
                                        .defect = &defect,
                                        .line.file = UINT32_MAX};
        if (process == NULL) {
            defect = (struct mo_defect){.reason = "running out of memory",
                                        .limit_name = errno == ENOSPC ? "--max-heap" : NULL,
                                        .limit = cli->limits.heap_size,
                                        .span.file = MO_NO_INDEX};
            result.outcome = MO_STOPPED;
        } else
            result.outcome = mo_call(process, function, NULL, 0, &value, &defect);
        switch (result.outcome) {
        case MO_RETURNED:
            totals->passed++;
            mo_value_drop(mo_process_heap(process), file->functions[function].result_type, value);
            break;
        case MO_FAILED:
            totals->failed++;
            if (observation->source->kind == MO_SPEC_TEST)
                result.line = assertion_span(observation->source,
                                             mo_runtime_span(file, sources, defect.span.file,
                                                             defect.span.start, defect.span.end));
            break;
        case MO_STOPPED:
            totals->stopped++;
            break;
        }
        bool shown = false;
        if (clock_gettime(CLOCK_MONOTONIC, &end) == 0) {
            result.microseconds = microseconds(start, end);
            shown = mo_write_test_result(output, cli->json, sources, &result);
        }
        mo_process_free(process);
        if (!shown) {
            report_error(diags, "test result could not be rendered or written completely",
                         observation->source->syntax->span, observation->name);
            break;
        }
    }
done:
    mo_arena_free(&scratch);
    mo_image_free(image);
}
