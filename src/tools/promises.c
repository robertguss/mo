#include "tools/promises.h"

#include "base/codes.h"
#include "tools/deadline.h"
#include "tools/run.h"

#include <string.h>

static void inability(struct mo_diags *diags, const struct mo_observation *observation,
                      const char *message, const char *detail) {
    struct mo_diag diag = {.code = MO_CODE_TOOL_PIPELINE,
                           .message = message,
                           .primary = {observation->source->syntax->span, detail}};
    bool added = mo_diags_add(diags, &diag);
    (void)added;
}

bool mo_promises_supported(const struct mo_program *program,
                           const struct mo_test_selection *selection, struct mo_diags *diags) {
    static const char *const scalars[] = {"Unit", "Bool",  "Byte",     "Int",     "Float",
                                          "Char", "Time",  "Duration", "Decimal", "Range",
                                          "Text", "Bytes", "BigInt"};
    for (uint32_t i = 0; i < selection->count; i++) {
        const struct mo_observation *observation = &selection->items[i];
        const struct mo_checked_spec *spec = observation->source;
        if (spec->kind != MO_SPEC_PROMISE)
            continue;
        const struct mo_checked_function *subject = &program->functions[spec->subject];
        bool supported = subject->signature->type->parameters == 0;
        for (uint32_t p = 0; supported && p < subject->parameter_count; p++) {
            const struct mo_source_type *type = subject->parameters[p]->type;
            supported = false;
            if (type->kind == MO_SOURCE_NAMED && type->module == UINT32_MAX && type->count == 0)
                for (size_t s = 0; s < sizeof scalars / sizeof scalars[0]; s++)
                    supported |= strcmp(type->name, scalars[s]) == 0;
        }
        if (!supported) {
            struct mo_diag diag = {.code = MO_CODE_TOOL_UNAVAILABLE,
                                   .message = "this build cannot generate inputs for this promise",
                                   .primary = {spec->syntax->span, observation->name}};
            bool added = mo_diags_add(diags, &diag);
            (void)added;
            return false;
        }
    }
    return true;
}

struct trial {
    struct mo_process *process;
    struct mo_cell input[MO_LIMIT_BYTECODE_PARAMETERS];
    struct mo_cell value;
    struct mo_defect defect;
    enum mo_outcome outcome;
    uint32_t condition;
    bool has_input, has_value;
};

static void discard(struct trial *trial, const struct mo_bytecode_function *subject) {
    if (trial->process == NULL)
        return;
    struct mo_heap *heap = mo_process_heap(trial->process);
    if (trial->has_input)
        for (uint32_t i = 0; i < subject->parameter_count; i++)
            mo_value_drop(heap, subject->parameters[i].type, trial->input[i]);
    if (trial->has_value)
        mo_value_drop(heap, subject->result_type, trial->value);
    mo_process_free(trial->process);
    *trial = (struct trial){0};
}

/* Keep C's independent ownership even if VM parameter ownership changes.
 * Borrowed parameters refer to the retained tuple/result until teardown. */
static void arguments(struct mo_heap *heap, const struct mo_bytecode_function *function,
                      const struct trial *trial, bool condition, struct mo_cell *out) {
    for (uint32_t i = 0; i < function->parameter_count; i++) {
        struct mo_cell value =
            condition && i + 1 == function->parameter_count ? trial->value : trial->input[i];
        out[i] = function->parameters[i].ownership == MO_PARAMETER_OWNED
                     ? mo_value_dup(heap, function->parameters[i].type, value)
                     : value;
    }
}

/* False is only control cancellation. STOPPED/FAILED are genuine VM outcomes,
 * never a deadline surrogate. The caller retains this entire trial for reports. */
static bool evaluate(struct trial *trial, const struct mo_bytecode *file, uint32_t promise,
                     struct mo_deadline *deadline) {
    uint32_t subject = file->debug[promise].subject;
    struct mo_heap *heap = mo_process_heap(trial->process);
    struct mo_cell args[MO_LIMIT_BYTECODE_PARAMETERS];
    arguments(heap, &file->functions[subject], trial, false, args);
    enum mo_controlled_outcome outcome =
        mo_call_controlled(trial->process, subject, args, file->functions[subject].parameter_count,
                           mo_deadline_running, deadline, &trial->value, &trial->defect);
    if (outcome == MO_CONTROL_RETURNED) {
        trial->has_value = true;
        arguments(heap, &file->functions[promise], trial, true, args);
        struct mo_cell answer;
        outcome = mo_call_controlled(trial->process, promise, args,
                                     file->functions[promise].parameter_count, mo_deadline_running,
                                     deadline, &answer, &trial->defect);
        if (outcome == MO_CONTROL_RETURNED) {
            int64_t condition = mo_cell_get_int(answer);
            MO_ASSERT(condition >= 0 && (uint64_t)condition <= file->debug[promise].line_count);
            trial->condition = (uint32_t)condition;
            mo_value_drop(heap, file->functions[promise].result_type, answer);
            trial->outcome = condition == 0 ? MO_RETURNED : MO_FAILED;
            return true;
        }
    }
    switch (outcome) {
    case MO_CONTROL_STOPPED:
        trial->outcome = MO_STOPPED;
        return true;
    case MO_CONTROL_FAILED:
        trial->outcome = MO_FAILED;
        return true;
    case MO_CONTROL_CANCELLED:
        return false;
    case MO_CONTROL_RETURNED:
        break;
    }
    MO_UNREACHABLE();
}

static void interrupted(struct mo_diags *diags, const struct mo_observation *observation,
                        const struct mo_deadline *deadline) {
    if (deadline->state == MO_DEADLINE_ERROR)
        inability(diags, observation, "promise clock could not be read", strerror(deadline->error));
    else
        inability(diags, observation, "promise trial interrupted before its outcome was known",
                  "the promise's monotonic deadline expired");
}

static bool write_result(const struct mo_cli *cli, const struct mo_sources *sources,
                         const struct mo_observation *observation, const struct mo_bytecode *file,
                         uint32_t promise, const struct mo_inputs *inputs, const struct trial *best,
                         uint64_t calls, const struct mo_deadline *deadline, FILE *output,
                         struct mo_diags *diags, struct mo_test_totals *totals) {
    struct mo_promise_result details = {.subject = file->debug[promise].subject, .calls = calls};
    struct mo_test_result result = {.observation = observation,
                                    .file = file,
                                    .outcome = MO_RETURNED,
                                    .microseconds = mo_deadline_microseconds(deadline),
                                    .line.file = UINT32_MAX,
                                    .promise = &details};
    struct mo_input_extreme extreme;
    if (best->process != NULL) {
        result.process = best->process;
        result.defect = &best->defect;
        result.outcome = best->outcome;
        details.input = best->input;
        details.value = best->value;
        details.has_value = best->has_value;
        if (best->condition != 0) {
            const struct mo_bytecode_spec_line *line =
                &file->debug[promise].lines[best->condition - 1];
            result.line = mo_runtime_span(file, sources, line->file, line->start, line->end);
        } else if (result.outcome == MO_FAILED)
            result.line = mo_runtime_span(file, sources, best->defect.span.file,
                                          best->defect.span.start, best->defect.span.end);
        if (result.outcome == MO_STOPPED)
            for (uint32_t p = 0; p < file->functions[details.subject].parameter_count; p++) {
                enum mo_input_result found = mo_inputs_extreme(inputs, best->input, p, &extreme);
                if (found == MO_INPUT_OK) {
                    details.extreme = &extreme;
                    break;
                }
                if (found != MO_INPUT_DONE) {
                    inability(diags, observation, "promise input extreme could not be inspected",
                              NULL);
                    return true;
                }
            }
    }
    switch (result.outcome) {
    case MO_RETURNED:
        totals->passed++;
        break;
    case MO_FAILED:
        totals->failed++;
        break;
    case MO_STOPPED:
        totals->stopped++;
        break;
    }
    if (mo_write_test_result(output, cli->json, sources, &result))
        return true;
    inability(diags, observation, "promise result could not be rendered or written completely",
              NULL);
    return false;
}

bool mo_test_promise(const struct mo_cli *cli, struct mo_image *image,
                     const struct mo_sources *sources, const struct mo_observation *observation,
                     uint32_t function, FILE *output, struct mo_diags *diags,
                     struct mo_test_totals *totals) {
    const struct mo_bytecode *file = mo_image_bytecode(image);
    uint32_t subject = file->debug[function].subject;
    const struct mo_bytecode_function *ordinary = &file->functions[subject];
    struct mo_deadline deadline;
    if (!mo_deadline_start(&deadline, cli->seconds)) {
        interrupted(diags, observation, &deadline);
        return true;
    }
    uint32_t parameters[MO_LIMIT_BYTECODE_PARAMETERS];
    for (uint32_t i = 0; i < ordinary->parameter_count; i++)
        parameters[i] = ordinary->parameters[i].type;
    struct mo_inputs inputs;
    struct mo_input_limits limits = mo_input_limits_default();
    enum mo_input_result prepared =
        mo_inputs_init(&inputs, mo_image_types(image), parameters, ordinary->parameter_count,
                       observation->name, strlen(observation->name), &limits, NULL, 0);
    struct trial best = {0};
    uint64_t calls = 0;
    bool complete = true, known = true;
    if (prepared != MO_INPUT_OK) {
        inability(diags, observation, "promise input generator could not be prepared", NULL);
        goto done;
    }
    while (calls < cli->calls && mo_deadline_running(&deadline)) {
        struct trial candidate = {.process = mo_process_new(image, &cli->limits)};
        enum mo_input_kind kind;
        enum mo_input_result generated =
            candidate.process == NULL ? MO_INPUT_NO_MEMORY
                                      : mo_inputs_next(&inputs, mo_process_heap(candidate.process),
                                                       candidate.input, &kind);
        candidate.has_input = generated == MO_INPUT_OK;
        if (generated == MO_INPUT_DONE) {
            discard(&candidate, ordinary);
            break;
        }
        if (generated != MO_INPUT_OK) {
            inability(diags, observation, "promise input could not be generated", NULL);
            discard(&candidate, ordinary);
            known = false;
            break;
        }
        calls++;
        bool resolved = evaluate(&candidate, file, function, &deadline);
        if (!resolved) {
            interrupted(diags, observation, &deadline);
            known = false;
        } else if (candidate.outcome != MO_RETURNED) {
            best = candidate;
            break;
        }
        discard(&candidate, ordinary);
        if (!resolved)
            break;
    }
    if (known && best.process != NULL && best.condition != 0) {
        struct mo_input_shrinker shrinker;
        mo_input_shrink_init(&shrinker, &inputs, MO_CLI_PROMISE_SHRINK_CALLS);
        for (uint32_t shrinking = 0;
             shrinking < MO_CLI_PROMISE_SHRINK_CALLS && mo_deadline_running(&deadline);
             shrinking++) {
            struct trial candidate = {.process = mo_process_new(image, &cli->limits)};
            enum mo_input_result generated =
                candidate.process == NULL
                    ? MO_INPUT_NO_MEMORY
                    : mo_input_shrink_next(&shrinker, best.input,
                                           mo_process_heap(candidate.process), candidate.input);
            candidate.has_input = generated == MO_INPUT_OK;
            if (generated != MO_INPUT_OK) {
                discard(&candidate, ordinary);
                if (generated != MO_INPUT_DONE)
                    inability(diags, observation, "promise shrink input could not be generated",
                              NULL);
                break;
            }
            calls++;
            if (!evaluate(&candidate, file, function, &deadline)) {
                discard(&candidate, ordinary);
                /* Expiry keeps best. Clock failure is diagnosed below as well. */
                break;
            }
            bool fails = candidate.outcome != MO_RETURNED;
            mo_input_shrink_feedback(&shrinker, fails);
            if (fails) {
                discard(&best, ordinary);
                best = candidate;
                if (best.condition == 0)
                    break; /* A genuine stop/fail is not a false Bool condition. */
            } else
                discard(&candidate, ordinary);
        }
    }
    if (deadline.state == MO_DEADLINE_ERROR && known) {
        interrupted(diags, observation, &deadline);
        known = best.process != NULL;
    }
    if (known && calls == 0 && deadline.state == MO_DEADLINE_EXPIRED) {
        interrupted(diags, observation, &deadline);
        known = false;
    }
    if (known)
        complete = write_result(cli, sources, observation, file, function, &inputs, &best, calls,
                                &deadline, output, diags, totals);
done:
    discard(&best, ordinary);
    mo_inputs_free(&inputs);
    return complete;
}
