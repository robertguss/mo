#define _POSIX_C_SOURCE 200809L

#include "tools/tests.h"

#include "base/codes.h"
#include "tools/deadline.h"
#include "tools/promises.h"
#include "tools/recovery.h"
#include "tools/run.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

const char *mo_observation_kind(const struct mo_observation *observation) {
    if (observation->source == NULL)
        return "recovery";
    switch (observation->source->kind) {
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

struct mo_span mo_observation_span(const struct mo_observation *observation) {
    return observation->source != NULL ? observation->source->syntax->span
                                       : observation->service->syntax->span;
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
         mo_buffer_append_text(&buffer,
                               mo_observation_kind(&(struct mo_observation){.source = spec})) &&
         mo_buffer_append_text(&buffer, ".") && mo_buffer_append_uint(&buffer, spec->number) &&
         mo_buffer_append_byte(&buffer, 0);
    *work -= buffer.length;
    char *result = ok ? mo_arena_copy(arena, buffer.bytes, buffer.length, 1) : NULL;
    mo_buffer_free(&buffer);
    return result;
}

/* <Module>.service.recovery: service is reserved, so no spec has the name. */
static char *recovery_name(struct mo_arena *arena, const struct mo_program *program,
                           const struct mo_checked_service *service, size_t *work) {
    struct mo_buffer buffer;
    mo_buffer_init(&buffer, *work < MO_CLI_DIAGNOSTIC_SIZE ? *work : MO_CLI_DIAGNOSTIC_SIZE);
    bool ok = mo_buffer_append_text(&buffer, program->set->names[service->module]) &&
              mo_buffer_append_text(&buffer, ".service.recovery") &&
              mo_buffer_append_byte(&buffer, 0);
    *work -= buffer.length;
    char *result = ok ? mo_arena_copy(arena, buffer.bytes, buffer.length, 1) : NULL;
    mo_buffer_free(&buffer);
    return result;
}

/* Whether full, a candidate's full name in module, is selected, marking the --only names it
 * matches. false in exhausted when the selection's work runs out. */
static bool selected(const struct mo_program *program, const struct mo_cli *cli, uint32_t module,
                     const char *full, const size_t *lengths, bool *found, size_t *work,
                     bool *exhausted) {
    bool chosen = cli->only_count == 0;
    size_t length = strlen(full);
    for (uint32_t j = 0; j < cli->only_count; j++) {
        size_t cost = length == lengths[j] ? length + 1 : 1;
        if (cost > *work) {
            *exhausted = true;
            return false;
        }
        *work -= cost;
        if (length == lengths[j] && memcmp(full, cli->only[j], length) == 0)
            chosen = found[j] = true;
    }
    return chosen && (cli->module == NULL || strcmp(cli->module, program->set->names[module]) == 0);
}

struct mo_test_selection *mo_select_tests(struct mo_arena *arena, const struct mo_program *program,
                                          const struct mo_cli *cli, struct mo_diags *diags) {
    if ((uint64_t)program->spec_count + program->service_count > MO_LIMIT_CORE_NODES)
        goto exhausted;
    struct mo_test_selection *selection =
        mo_arena_allocate(arena, sizeof(*selection), _Alignof(struct mo_test_selection));
    if (selection == NULL)
        goto exhausted;
    *selection = (struct mo_test_selection){0};
    selection->items =
        mo_arena_allocate_array(arena, (size_t)program->spec_count + program->service_count,
                                sizeof(*selection->items), _Alignof(struct mo_observation));
    if (selection->items == NULL)
        goto exhausted;
    bool found[MO_CLI_ARGUMENTS] = {false};
    size_t lengths[MO_CLI_ARGUMENTS];
    for (uint32_t i = 0; i < cli->only_count; i++)
        lengths[i] = strlen(cli->only[i]);
    size_t work = MO_CLI_TEST_WORK;
    bool exhausted = false;
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
        if (selected(program, cli, spec->module, full, lengths, found, &work, &exhausted))
            selection->items[selection->count++] =
                (struct mo_observation){.name = full, .source = spec};
        if (exhausted)
            goto exhausted;
    }
    /* Each service's recovery check follows every other check of its module. */
    for (uint32_t i = 0; i < program->service_count; i++) {
        const struct mo_checked_service *service = &program->services[i];
        if (service->module != 0 && program->set->library != NULL &&
            program->set->library[service->module])
            continue;
        char *full = recovery_name(arena, program, service, &work);
        if (full == NULL)
            goto exhausted;
        if (selected(program, cli, service->module, full, lengths, found, &work, &exhausted))
            selection->items[selection->count++] =
                (struct mo_observation){.name = full, .service = service};
        if (exhausted)
            goto exhausted;
    }
    for (uint32_t i = 0; i < cli->only_count; i++)
        if (!found[i])
            selection->mistake = "--only does not name an example, promise, test or recovery check";
    if (selection->mistake == NULL && selection->count == 0)
        selection->mistake =
            "test selection matches no examples, promises, tests or recovery checks";
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

struct mo_test_selection *mo_instantiate_tests(struct mo_arena *arena,
                                               const struct mo_test_selection *selection,
                                               const struct mo_lowered_promises *promises,
                                               struct mo_diags *diags) {
    struct mo_test_selection *result =
        mo_arena_allocate(arena, sizeof(*result), _Alignof(struct mo_test_selection));
    if (result == NULL)
        goto exhausted;
    *result = (struct mo_test_selection){0};
    result->items =
        mo_arena_allocate_array(arena, (size_t)selection->count + promises->count,
                                sizeof(*result->items), _Alignof(struct mo_observation));
    if (result->items == NULL)
        goto exhausted;
    size_t work = MO_CLI_TEST_WORK;
    for (uint32_t i = 0; i < selection->count; i++) {
        const struct mo_observation *observation = &selection->items[i];
        if (result->count == MO_LIMIT_CORE_NODES)
            goto exhausted;
        if (observation->source == NULL || observation->source->kind != MO_SPEC_PROMISE) {
            result->items[result->count++] = *observation;
            continue;
        }
        uint32_t before = result->count;
        size_t length = strlen(observation->name);
        for (uint32_t j = 0; j < promises->count; j++) {
            if (work == 0)
                goto exhausted;
            work--;
            const struct mo_lowered_promise *promise = &promises->items[j];
            if (promise->source != observation->source)
                continue;
            if (length > work || result->count == MO_LIMIT_CORE_NODES)
                goto exhausted;
            work -= length;
            struct mo_observation *item = &result->items[result->count++];
            *item = *observation;
            if (promise->name.length != length ||
                memcmp(promise->name.bytes, observation->name, length) != 0)
                item->instance = promise;
        }
        if (result->count == before) {
            report_error(diags, "selected promise has no concrete instantiation",
                         observation->source->syntax->span, observation->name);
            return NULL;
        }
    }
    return result;
exhausted:
    report_error(diags, "test instances exceed tool resources", (struct mo_span){UINT32_MAX, 0, 0},
                 NULL);
    return NULL;
}

struct observation_index {
    struct mo_span span;
    enum mo_function_kind kind;
    uint32_t selected;
    mo_core_string name;
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
    size_t length = left->name.length < right->name.length ? left->name.length : right->name.length;
    int order = memcmp(left->name.bytes, right->name.bytes, length);
    return order != 0                               ? order
           : left->name.length < right->name.length ? -1
           : left->name.length > right->name.length ? 1
                                                    : 0;
}

/* The bytecode service a recovery check runs: the one its module declares. */
static uint32_t bind_service(const struct mo_bytecode *file, const struct mo_program *program,
                             const struct mo_checked_service *service) {
    const char *module = program->set->names[service->module];
    size_t length = strlen(module);
    uint32_t found = MO_NO_INDEX;
    for (uint32_t i = 0; i < file->service_count; i++) {
        const struct mo_bytecode_string *name =
            &file->strings[file->modules[file->services[i].module].name];
        if (name->length == length && memcmp(name->bytes, module, length) == 0) {
            if (found != MO_NO_INDEX)
                return MO_NO_INDEX;
            found = i;
        }
    }
    return found;
}

/* Each observation's compiled function, or a recovery check's service index. */
static uint32_t *bind_observations(struct mo_arena *arena, const struct mo_bytecode *file,
                                   const struct mo_program *program,
                                   const struct mo_sources *sources,
                                   const struct mo_test_selection *selection) {
    struct observation_index *index = mo_arena_allocate_array(
        arena, selection->count, sizeof(*index), _Alignof(struct observation_index));
    uint32_t *functions =
        mo_arena_allocate_array(arena, selection->count, sizeof(*functions), _Alignof(uint32_t));
    if (index == NULL || functions == NULL)
        return NULL;
    uint32_t indexed = 0;
    for (uint32_t i = 0; i < selection->count; i++) {
        const struct mo_observation *observation = &selection->items[i];
        const struct mo_checked_spec *source = observation->source;
        if (source == NULL) {
            functions[i] = bind_service(file, program, observation->service);
            if (functions[i] == MO_NO_INDEX)
                return NULL;
            continue;
        }
        index[indexed++] = (struct observation_index){
            source->syntax->span,
            source->kind == MO_SPEC_TEST      ? MO_FUNCTION_TEST
            : source->kind == MO_SPEC_PROMISE ? MO_FUNCTION_PROMISE
                                              : MO_FUNCTION_EXAMPLE,
            i,
            observation->instance != NULL
                ? observation->instance->name
                : (mo_core_string){observation->name, strlen(observation->name)}};
        functions[i] = MO_NO_INDEX;
    }
    if (indexed != 0)
        qsort(index, indexed, sizeof(*index), compare_observations);
    for (uint32_t i = 0; i < file->function_count; i++) {
        const struct mo_bytecode_function *function = &file->functions[i];
        if (function->kind != MO_FUNCTION_TEST && function->kind != MO_FUNCTION_EXAMPLE &&
            function->kind != MO_FUNCTION_PROMISE)
            continue;
        const struct mo_bytecode_debug *debug = &file->debug[i];
        const struct mo_bytecode_string *full = &file->strings[function->name];
        struct observation_index key = {
            .span = mo_runtime_span(file, sources, debug->file, debug->start, debug->end),
            .kind = function->kind,
            .name = {(const char *)full->bytes, full->length}};
        struct observation_index *match =
            indexed == 0 ? NULL
                         : bsearch(&key, index, indexed, sizeof(*index), compare_observations);
        if (match == NULL)
            continue;
        const struct mo_observation *observation = &selection->items[match->selected];
        bool promise = function->kind == MO_FUNCTION_PROMISE;
        if (function->parameter_count != (promise ? observation->source->parameter_count : 0) ||
            file->types[function->result_type].kind != (promise ? MO_TYPE_INT : MO_TYPE_UNIT) ||
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

/* What each step of a failing sequence's last run replied, copied into the checker's heap. */
struct replies {
    struct mo_recovery *recovery;
    struct mo_cell values[MO_CLI_RECOVERY_STEPS];
    uint32_t types[MO_CLI_RECOVERY_STEPS];
};

static void keep_reply(const struct mo_recovery_observation *observation, void *context) {
    struct replies *replies = context;
    if (observation->reply == NULL)
        return;
    const struct mo_recovery_step *step = observation->step;
    uint32_t type;
    struct mo_cell value =
        mo_recovery_reply(replies->recovery, step->tool, step->sent, observation->reply, &type);
    replies->values[observation->index] =
        mo_value_dup(mo_recovery_heap(replies->recovery), type, value);
    replies->types[observation->index] = type;
}

/* Runs one service's recovery check (reference 12.5) on the shared deadline, shrinks a failing
 * sequence and runs it once more, then writes the result. A check that cannot reach an outcome is
 * E1404. false when the result could not be written, which ends the run. */
static bool test_recovery(const struct mo_cli *cli, struct mo_image *image,
                          const struct mo_sources *sources,
                          const struct mo_observation *observation, uint32_t service, FILE *output,
                          struct mo_diags *diags, struct mo_test_totals *totals) {
    struct mo_span span = mo_observation_span(observation);
    struct mo_deadline deadline;
    if (!mo_deadline_start(&deadline, cli->seconds)) {
        report_error(diags, "recovery check clock could not be read", span,
                     strerror(deadline.error));
        return true;
    }
    struct mo_recovery *recovery = NULL;
    if (mo_recovery_new(image, service, &cli->limits, &recovery) != MO_INPUT_OK) {
        report_error(diags, "recovery check could not be prepared", span, observation->name);
        return true;
    }
    struct mo_recovery_result found, rerun;
    enum mo_recovery_outcome outcome =
        mo_recovery_search(recovery, cli->calls, &deadline, NULL, &found);
    struct replies replies = {.recovery = recovery};
    for (uint32_t i = 0; i < MO_CLI_RECOVERY_STEPS; i++)
        replies.types[i] = MO_NO_INDEX;
    struct mo_recovery_report report = {
        .recovery = recovery, .service = service, .counts = found.counts, .result = &found};
    struct mo_test_result result = {.observation = observation,
                                    .file = mo_image_bytecode(image),
                                    .line.file = UINT32_MAX,
                                    .recovery = &report};
    const char *problem = NULL, *detail = observation->name;
    bool ran_again = false;
    switch (outcome) {
    case MO_RECOVERY_PASSED:
        result.outcome = MO_RETURNED;
        break;
    case MO_RECOVERY_STOPPED:
        result.outcome = MO_STOPPED;
        break;
    case MO_RECOVERY_UNCOMPARED:
        problem = "the deadline ended before the check compared a restart";
        break;
    case MO_RECOVERY_UNAVAILABLE:
        problem = "recovery check could not run";
        detail = found.problem;
        break;
    case MO_RECOVERY_MISMATCH:
    case MO_RECOVERY_REFUSED: {
        report.counts.sequences++;
        uint32_t runs = mo_recovery_shrink(recovery, &found.sequence, outcome, &deadline);
        (void)runs;
        const struct mo_recovery_hooks hooks = {keep_reply, &replies};
        ran_again = true;
        if (mo_recovery_replay(recovery, &found.sequence, &hooks, &rerun) != outcome) {
            problem = "the shrunk recovery sequence did not fail the same way when it ran again";
            break;
        }
        result.outcome = MO_FAILED;
        report.sequence = &found.sequence;
        report.result = &rerun;
        report.replies = replies.values;
        report.reply_types = replies.types;
        break;
    }
    }
    result.microseconds = mo_deadline_microseconds(&deadline);
    bool written = true;
    if (problem != NULL)
        report_error(diags, problem, span, detail);
    else {
        totals->passed += result.outcome == MO_RETURNED;
        totals->failed += result.outcome == MO_FAILED;
        totals->stopped += result.outcome == MO_STOPPED;
        written = mo_write_test_result(output, cli->json, sources, &result);
        if (!written)
            report_error(diags, "test result could not be rendered or written completely", span,
                         observation->name);
    }
    for (uint32_t i = 0; i < MO_CLI_RECOVERY_STEPS; i++)
        if (replies.types[i] != MO_NO_INDEX)
            mo_value_drop(mo_recovery_heap(recovery), replies.types[i], replies.values[i]);
    if (ran_again)
        mo_recovery_result_free(recovery, &rerun);
    mo_recovery_result_free(recovery, &found);
    mo_recovery_free(recovery);
    return written;
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
    uint32_t *functions = bind_observations(&scratch, file, program, sources, selection);
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
        if (observation->source == NULL) {
            if (!test_recovery(cli, image, sources, observation, function, output, diags, totals))
                break;
            continue;
        }
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
            /* Verified bytecode releases every reference it acquires (bytecode 4.2). */
            MO_ASSERT(mo_heap_objects(mo_process_heap(process)) == 0);
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
