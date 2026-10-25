#include "tools/promises.h"

#include "base/codes.h"
#include "tools/builders.h"
#include "tools/deadline.h"
#include "tools/fakes.h"
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

/* Check the canonical source graph, not the spelling of imported types or
 * runtime layouts (which deliberately contain no source visibility policy).
 * All actual arguments are checked, so a data template's scoped parameters
 * stand for eligible types without synthesizing substitutions. Visiting each
 * canonical node once also closes regular recursive fields and shared DAGs. */
struct eligibility {
    struct mo_arena arena;
    const struct mo_source_type **types;
    size_t count, capacity;
    size_t *work;
    bool limited;
    bool plain; /* a builder's input: no capabilities, and hidden types only of lower levels */
    /* A hidden type met here must start within level - 1 (MO_CLI_BUILDER_LEVELS); 0 allows none. */
    uint32_t level;
    bool copied; /* a hidden type's insides: no capabilities, and hidden types are structural */
};

static bool input_work(struct eligibility *walk) {
    if (*walk->work == 0) {
        walk->limited = true;
        return false;
    }
    (*walk->work)--;
    return true;
}

static bool input_type(struct eligibility *walk, const struct mo_source_type *type) {
    if (!input_work(walk))
        return false;
    for (size_t i = 0; i < walk->count; i++) {
        if (!input_work(walk))
            return false;
        if (walk->types[i] == type)
            return true;
    }
    if (walk->count == walk->capacity) {
        size_t capacity = walk->capacity == 0 ? 32 : walk->capacity * 2;
        void *grown =
            mo_arena_resize(&walk->arena, walk->types, walk->capacity * sizeof(*walk->types),
                            capacity * sizeof(*walk->types), _Alignof(void *));
        if (grown == NULL) {
            walk->limited = true;
            return false;
        }
        walk->types = grown;
        walk->capacity = capacity;
    }
    walk->types[walk->count++] = type;
    return true;
}

static bool input_fields(struct eligibility *walk, const struct mo_signatures *signatures,
                         const struct mo_symbol *symbol) {
    uint32_t lo = 0, hi = signatures->data_count;
    while (lo < hi) {
        if (!input_work(walk))
            return false;
        uint32_t mid = lo + (hi - lo) / 2;
        const struct mo_symbol *at = signatures->data[mid].symbol;
        if (at->module < symbol->module ||
            (at->module == symbol->module && (uintptr_t)at < (uintptr_t)symbol))
            lo = mid + 1;
        else
            hi = mid;
    }
    MO_ASSERT(lo < signatures->data_count && signatures->data[lo].symbol == symbol);
    const struct mo_data_signature *data = &signatures->data[lo];
    for (uint32_t i = 0; i < data->count; i++)
        if (!input_type(walk, data->by_index[i]->type))
            return false;
    return true;
}

static bool input_supported(struct eligibility *walk, const struct mo_program *program,
                            const struct mo_source_type *type, uint32_t module);

/* Every type reachable from the walk's roots can be generated. */
static bool walk_supported(struct eligibility *walk, const struct mo_program *program,
                           uint32_t module) {
    for (size_t p = 0; p < walk->count; p++)
        if (!input_supported(walk, program, walk->types[p], module))
            return false;
    return true;
}

static bool hidden_type(const struct mo_source_type *type, const struct mo_symbol *owner) {
    return type->kind == MO_SOURCE_NAMED && type->declaration == owner && type->count == 0;
}

/* Whether a result holds the hidden type: itself, a tuple's field, Some or Ok (the driver's
 * builders follow the same path in the image, tools/builders.c). */
static bool yields(const struct mo_source_type *type, const struct mo_symbol *owner,
                   uint32_t depth) {
    if (hidden_type(type, owner))
        return true;
    if (depth == MO_CLI_BUILDER_PATH)
        return false;
    if (type->kind == MO_SOURCE_TUPLE) {
        for (uint32_t i = 0; i < type->count; i++)
            if (yields(type->arguments[i], owner, depth + 1))
                return true;
        return false;
    }
    return type->kind == MO_SOURCE_NAMED && type->module == UINT32_MAX && type->count != 0 &&
           (strcmp(type->name, "Option") == 0 || strcmp(type->name, "Result") == 0) &&
           yields(type->arguments[0], owner, depth + 1);
}

/* A hidden type can be generated when its module can start one at level or below (reference
 * 12.2): an exposed constant of it, or an exposed nongeneric function returning it from inputs
 * that hold no capability or function, and only hidden types that start at a lower level. This
 * matches the levels the image's builders take (tools/builders.c); the image chooses steps. */
static bool startable(struct eligibility *outer, const struct mo_program *program,
                      const struct mo_symbol *owner, uint32_t level) {
    for (uint32_t i = 0; i < program->constant_count; i++) {
        const struct mo_checked_constant *constant = &program->constants[i];
        if (!input_work(outer))
            return false;
        if (constant->symbol->exported && constant->symbol->module == owner->module &&
            hidden_type(constant->type, owner))
            return true;
    }
    for (uint32_t i = 0; i < program->function_count; i++) {
        const struct mo_function_signature *signature = program->functions[i].signature;
        const struct mo_source_type *type = signature->type;
        if (!input_work(outer))
            return false;
        if (!signature->function->exported || signature->function->module != owner->module ||
            type->parameters != 0 || !yields(type->arguments[type->count - 1], owner, 0))
            continue;
        struct eligibility walk = {.work = outer->work, .plain = true, .level = level};
        mo_arena_init(&walk.arena, MO_CLI_TEST_SIZE);
        bool supported = true;
        for (uint32_t p = 0; supported && p + 1 < type->count; p++)
            supported = input_type(&walk, type->arguments[p]);
        supported = supported && walk_supported(&walk, program, owner->module);
        mo_arena_free(&walk.arena);
        outer->limited |= walk.limited;
        if (supported)
            return true;
        if (walk.limited)
            return false;
    }
    return false;
}

/* Built values are copied into each trial field by field, from their own module's view. */
static bool insides_supported(struct eligibility *outer, const struct mo_program *program,
                              const struct mo_symbol *owner) {
    struct eligibility walk = {.work = outer->work, .copied = true};
    mo_arena_init(&walk.arena, MO_CLI_TEST_SIZE);
    bool choice = owner->declaration->kind == MO_S_ENUM;
    uint32_t count = choice ? owner->case_count : 1;
    bool supported = true;
    for (uint32_t i = 0; supported && i < count; i++) {
        const struct mo_symbol *symbol =
            choice ? &program->interfaces->scopes[owner->module].symbols[owner->cases[i]] : owner;
        supported = input_fields(&walk, program->signatures, symbol);
    }
    supported = supported && walk_supported(&walk, program, owner->module);
    mo_arena_free(&walk.arena);
    outer->limited |= walk.limited;
    return supported;
}

static bool input_supported(struct eligibility *walk, const struct mo_program *program,
                            const struct mo_source_type *type, uint32_t module) {
    if (!input_work(walk))
        return false;
    if (type->kind == MO_SOURCE_FUNCTION)
        return false;
    if (type->kind == MO_SOURCE_PARAMETER) {
        /* Only data templates can introduce parameters in a closed subject. */
        MO_ASSERT(type->declaration != NULL && type->declaration->declaration != NULL);
        enum mo_syntax_kind kind = type->declaration->declaration->kind;
        MO_ASSERT(kind == MO_S_STRUCT || kind == MO_S_ENUM);
        return true;
    }
    if (type->kind == MO_SOURCE_NAMED) {
        if (type->module == UINT32_MAX) {
            static const char *const scalars[] = {
                "Unit", "Bool", "Byte", "Int", "Float", "Char", "Time", "Duration", "Decimal",
                "Range", "Text", "Bytes", "BigInt",
                /* Capabilities, as generated fakes. */
                "Console", "Files", "ReadFiles", "Clock", "Random", "Args", "Env"};
            bool supported = (type->count == 1 &&
                              (strcmp(type->name, "List") == 0 || strcmp(type->name, "Set") == 0 ||
                               strcmp(type->name, "Option") == 0)) ||
                             (type->count == 2 && (strcmp(type->name, "Map") == 0 ||
                                                   strcmp(type->name, "Result") == 0));
            size_t scalar_count =
                sizeof scalars / sizeof scalars[0] - (walk->plain || walk->copied ? 7 : 0);
            for (size_t i = 0; type->count == 0 && i < scalar_count; i++)
                supported |= strcmp(type->name, scalars[i]) == 0;
            if (!supported)
                return false;
        } else {
            const struct mo_symbol *owner = type->declaration;
            MO_ASSERT(owner != NULL && owner->module == type->module);
            /* Exposed-opaque types require builders even in their own module, and a builder's
             * own inputs hold only those of lower levels. Unexported local data, unlike those
             * types, is structural; a hidden type's insides must be too, since its built values
             * are copied. */
            if (!owner->insides && (owner->exported || owner->module != module) && !walk->copied)
                return walk->level != 0 && type->count == 0 &&
                       startable(walk, program, owner, walk->level - 1) &&
                       insides_supported(walk, program, owner);
            bool choice = owner->declaration->kind == MO_S_ENUM;
            uint32_t count = choice ? owner->case_count : 1;
            for (uint32_t i = 0; i < count; i++) {
                const struct mo_symbol *symbol =
                    choice ? &program->interfaces->scopes[owner->module].symbols[owner->cases[i]]
                           : owner;
                if (!input_fields(walk, program->signatures, symbol))
                    return false;
            }
        }
    }
    for (uint32_t i = 0; i < type->count; i++)
        if (!input_type(walk, type->arguments[i]))
            return false;
    return true;
}

bool mo_promises_supported(const struct mo_program *program,
                           const struct mo_test_selection *selection, struct mo_diags *diags) {
    size_t work = MO_CLI_TEST_WORK;
    for (uint32_t i = 0; i < selection->count; i++) {
        const struct mo_observation *observation = &selection->items[i];
        const struct mo_checked_spec *spec = observation->source;
        if (spec->kind != MO_SPEC_PROMISE)
            continue;
        const struct mo_checked_function *subject = &program->functions[spec->subject];
        const struct mo_lowered_promise *instance = observation->instance;
        if (subject->signature->type->parameters != 0 && instance == NULL)
            continue; /* Concrete inputs are checked after lowering has found all uses. */
        struct eligibility walk = {.work = &work, .level = MO_CLI_BUILDER_LEVELS};
        mo_arena_init(&walk.arena, MO_CLI_TEST_SIZE);
        bool supported = true;
        MO_ASSERT(instance == NULL || instance->parameter_count == subject->parameter_count);
        for (uint32_t p = 0; supported && p < subject->parameter_count; p++)
            supported = input_type(&walk, instance == NULL ? subject->parameters[p]->type
                                                           : instance->parameters[p]);
        supported =
            supported && walk_supported(&walk, program, subject->signature->function->module);
        mo_arena_free(&walk.arena);
        if (!supported) {
            if (walk.limited) {
                inability(diags, observation, "promise input preflight exceeds its resource budget",
                          observation->name);
                return false;
            }
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
    struct mo_build_trace trace;
    struct mo_cell input[MO_LIMIT_BYTECODE_PARAMETERS];
    struct mo_cell value;
    struct mo_defect defect;
    enum mo_outcome outcome;
    uint32_t condition;
    /* Both the subject and the promise returned, whatever the condition. */
    bool has_input, has_value, returned;
};

static void discard(struct trial *trial, const struct mo_bytecode_function *subject) {
    mo_build_trace_free(&trial->trace);
    if (trial->process == NULL)
        return;
    struct mo_heap *heap = mo_process_heap(trial->process);
    if (trial->has_input)
        for (uint32_t i = 0; i < subject->parameter_count; i++)
            mo_value_drop(heap, subject->parameters[i].type, trial->input[i]);
    if (trial->has_value)
        mo_value_drop(heap, subject->result_type, trial->value);
    /* Verified bytecode releases every reference it acquires (bytecode 4.2). */
    MO_ASSERT(!trial->returned || mo_heap_objects(heap) == 0);
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
            trial->returned = true;
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

bool mo_test_promise(const struct mo_cli *cli, const struct mo_program *program,
                     struct mo_image *image, const struct mo_sources *sources,
                     const struct mo_observation *observation, uint32_t function, FILE *output,
                     struct mo_diags *diags, struct mo_test_totals *totals) {
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
    struct mo_fakes fakes;
    mo_fakes_init(&fakes, mo_image_types(image));
    struct mo_builders builders;
    enum mo_input_result prepared =
        mo_builders_init(&builders, program, image, &cli->limits, &deadline);
    /* Capabilities and hidden types are disjoint kinds of type, so their factories never clash. */
    struct mo_input_factory *factories =
        prepared != MO_INPUT_OK
            ? NULL
            : mo_arena_allocate(&builders.arena,
                                (mo_fakes_count(&fakes) + mo_builders_count(&builders) + 1) *
                                    sizeof *factories,
                                _Alignof(struct mo_input_factory));
    if (prepared == MO_INPUT_OK && factories == NULL)
        prepared = MO_INPUT_NO_MEMORY;
    if (prepared == MO_INPUT_OK) {
        memcpy(factories, mo_fakes_factories(&fakes), mo_fakes_count(&fakes) * sizeof *factories);
        memcpy(factories + mo_fakes_count(&fakes), mo_builders_factories(&builders),
               mo_builders_count(&builders) * sizeof *factories);
        prepared =
            mo_inputs_init(&inputs, mo_image_types(image), parameters, ordinary->parameter_count,
                           observation->name, strlen(observation->name), &limits, factories,
                           mo_fakes_count(&fakes) + mo_builders_count(&builders));
    } else
        inputs = (struct mo_inputs){0};
    struct trial best = {0};
    uint64_t calls = 0, random_trials = 0;
    bool complete = true, known = true;
    if (prepared != MO_INPUT_OK) {
        inability(diags, observation, "promise input generator could not be prepared", NULL);
        goto done;
    }
    while (calls < cli->calls && mo_deadline_running(&deadline)) {
        struct trial candidate = {.process = mo_process_new(image, &cli->limits)};
        enum mo_input_kind kind;
        enum mo_input_result generated = MO_INPUT_NO_MEMORY;
        if (candidate.process != NULL) {
            mo_fakes_bind(&fakes, mo_process_host(candidate.process));
            mo_builders_begin(&builders, mo_process_heap(candidate.process), &candidate.trace,
                              inputs.ordinal);
            generated =
                mo_inputs_next(&inputs, mo_process_heap(candidate.process), candidate.input, &kind);
            /* Every second random trial takes a built value's companions as its other inputs
             * of their types, so they come from the same call (D284); boundaries never do. */
            bool ok = generated == MO_INPUT_OK;
            mo_builders_relate(&builders, candidate.input, parameters,
                               ok ? ordinary->parameter_count : 0,
                               ok && kind == MO_INPUT_RANDOM && random_trials++ % 2 == 1);
        }
        candidate.has_input = generated == MO_INPUT_OK;
        if (generated == MO_INPUT_DONE) {
            discard(&candidate, ordinary);
            break;
        }
        if (generated != MO_INPUT_OK) {
            discard(&candidate, ordinary);
            /* A builder's calls share the promise's deadline: running out ends the search. */
            if (!mo_deadline_running(&deadline))
                break;
            inability(diags, observation, "promise input could not be generated", NULL);
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
            mo_fakes_keep(&fakes);
            break;
        }
        discard(&candidate, ordinary);
        if (!resolved)
            break;
    }
    if (known && best.process != NULL && best.condition != 0) {
        struct mo_input_shrinker shrinker;
        mo_input_shrink_init(&shrinker, &inputs, MO_CLI_PROMISE_SHRINK_CALLS);
        struct mo_builder_shrinker builder_shrinker;
        mo_builder_shrink_init(&builder_shrinker, &builders);
        bool building = true;
        for (uint32_t shrinking = 0;
             shrinking < MO_CLI_PROMISE_SHRINK_CALLS && mo_deadline_running(&deadline);) {
            struct trial candidate = {.process = mo_process_new(image, &cli->limits)};
            enum mo_input_result generated = MO_INPUT_NO_MEMORY;
            if (candidate.process != NULL) {
                mo_fakes_bind(&fakes, mo_process_host(candidate.process));
                if (building) {
                    generated = mo_builder_shrink_next(&builder_shrinker, &inputs, &best.trace,
                                                       mo_process_heap(candidate.process),
                                                       &candidate.trace, candidate.input);
                    if (generated == MO_INPUT_DONE) {
                        building = false;
                        mo_build_trace_free(&best.trace);
                    }
                }
                if (!building)
                    generated = mo_input_shrink_next(
                        &shrinker, best.input, mo_process_heap(candidate.process), candidate.input);
            }
            candidate.has_input = generated == MO_INPUT_OK;
            if (building && generated == MO_INPUT_UNINHABITED) {
                discard(&candidate, ordinary);
                continue;
            }
            if (generated != MO_INPUT_OK) {
                discard(&candidate, ordinary);
                if (generated != MO_INPUT_DONE && mo_deadline_running(&deadline))
                    inability(
                        diags, observation,
                        building && !best.trace.complete
                            ? "promise shrinking is limited because builder recording is incomplete"
                            : "promise shrink input could not be generated",
                        NULL);
                break;
            }
            shrinking++;
            calls++;
            if (!evaluate(&candidate, file, function, &deadline)) {
                discard(&candidate, ordinary);
                /* Expiry keeps best. Clock failure is diagnosed below as well. */
                break;
            }
            bool fails = candidate.outcome != MO_RETURNED;
            if (building)
                mo_builder_shrink_feedback(&builder_shrinker, fails);
            else
                mo_input_shrink_feedback(&shrinker, fails);
            if (fails) {
                discard(&best, ordinary);
                best = candidate;
                mo_fakes_keep(&fakes);
                if (best.condition == 0)
                    break; /* A genuine stop/fail is not a false Bool condition. */
            } else
                discard(&candidate, ordinary);
        }
        mo_builder_shrink_free(&builder_shrinker);
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
    mo_builders_free(&builders);
    return complete;
}
