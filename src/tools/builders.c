#include "tools/builders.h"

#include <string.h>

#define BOUNDARIES 3
#define STEPS (MO_CLI_BUILDER_CALLS - 1)

/* An exposed constant of the hidden type, or an exposed function whose result holds one. */
struct builder {
    uint32_t function; /* MO_NO_INDEX for a constant */
    uint32_t global;
    /* (case tag, field) pairs from the result to the value; the tag is MO_NO_INDEX for a tuple. */
    uint32_t path[2 * MO_CLI_BUILDER_PATH];
    uint32_t path_length;
    bool chained; /* takes the hidden type: a step, not a start */
    bool generated;
    struct mo_inputs arguments;
};

struct mo_hidden {
    struct mo_builders *owner;
    uint32_t type;
    struct mo_inputs copier;
    bool copier_ready;
    struct builder *builders;
    uint32_t count;
    uint32_t *starts, *steps;
    uint32_t start_count, step_count;
    /* A step's generator takes the value built so far for each parameter of the hidden type. */
    struct mo_input_factory chain;
    struct mo_heap *heap;
    struct mo_cell current;
};

static uint64_t next_random(uint64_t *state) {
    uint64_t z = (*state += UINT64_C(0x9e3779b97f4a7c15));
    z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
    return z ^ (z >> 31);
}

static bool option_or_result(const struct mo_type *type, const char *base) {
    size_t length = strlen(base);
    return type->kind == MO_TYPE_CHOICE && type->name != NULL &&
           strncmp(type->name, base, length) == 0 && type->name[length] == '(';
}

/* Where the hidden type sits in a result: the result itself, a tuple's field, Some or Ok. The
 * first match, fields in order, wins. */
static bool find_path(const struct mo_types *types, uint32_t type, uint32_t hidden, uint32_t depth,
                      struct builder *out) {
    if (type == hidden)
        return true;
    if (depth == MO_CLI_BUILDER_PATH)
        return false;
    const struct mo_type *item = &types->items[type];
    uint32_t *step = &out->path[2 * out->path_length];
    out->path_length++;
    if (item->kind == MO_TYPE_TUPLE) {
        for (uint32_t i = 0; i < item->parameter_count; i++) {
            step[0] = MO_NO_INDEX;
            step[1] = i;
            if (find_path(types, item->parameters[i], hidden, depth + 1, out))
                return true;
        }
    } else if (option_or_result(item, "Option") || option_or_result(item, "Result")) {
        uint32_t tag = option_or_result(item, "Option") ? 1 : 0;
        MO_ASSERT(tag < item->case_count && item->cases[tag].field_count == 1);
        step[0] = tag;
        step[1] = 0;
        if (find_path(types, item->cases[tag].fields[0].type, hidden, depth + 1, out))
            return true;
    }
    out->path_length--;
    return false;
}

/* The borrowed value at a builder's path, or false when a case on the way doesn't match. */
static bool extract(const struct mo_types *types, const struct builder *builder, uint32_t type,
                    struct mo_cell value, struct mo_cell *out) {
    for (uint32_t i = 0; i < builder->path_length; i++) {
        uint32_t tag = builder->path[2 * i], field = builder->path[2 * i + 1];
        const struct mo_type *item = &types->items[type];
        if (tag == MO_NO_INDEX) {
            value = mo_value_field(value, field);
            type = item->parameters[field];
            continue;
        }
        if (mo_cell_get_tag(value) != tag)
            return false;
        struct mo_cell payload = mo_cell_get_payload(value);
        value = mo_case_shape(types, type, tag) == MO_CASE_BOXED ? mo_value_field(payload, field)
                                                                 : payload;
        type = item->cases[tag].fields[field].type;
    }
    *out = value;
    return true;
}

/* Whether a generator can make a value of type for a builder: no capability, function or hidden
 * type inside it. visited is scratch of one byte per type. */
static bool plain(const struct mo_types *types, const bool *hidden, uint32_t type, uint8_t *visited,
                  uint32_t *stack) {
    memset(visited, 0, types->count);
    uint32_t depth = 0;
    stack[depth++] = type;
    visited[type] = 1;
    while (depth != 0) {
        const struct mo_type *item = &types->items[stack[--depth]];
        if (hidden[stack[depth]] || item->kind == MO_TYPE_FUNCTION || item->kind == MO_TYPE_ENV ||
            item->kind == MO_TYPE_CAPABILITY)
            return false;
        const struct mo_type_field *fields = NULL;
        uint32_t field_count = 0;
        if (item->kind == MO_TYPE_RECORD) {
            fields = item->fields;
            field_count = item->field_count;
        }
        for (uint32_t i = 0; i < field_count; i++)
            if (!visited[fields[i].type]) {
                visited[fields[i].type] = 1;
                stack[depth++] = fields[i].type;
            }
        for (uint32_t c = 0; item->kind == MO_TYPE_CHOICE && c < item->case_count; c++)
            for (uint32_t i = 0; i < item->cases[c].field_count; i++) {
                uint32_t child = item->cases[c].fields[i].type;
                if (!visited[child]) {
                    visited[child] = 1;
                    stack[depth++] = child;
                }
            }
        bool parameters = item->kind == MO_TYPE_TUPLE || item->kind == MO_TYPE_LIST ||
                          item->kind == MO_TYPE_SET || item->kind == MO_TYPE_MAP;
        for (uint32_t i = 0; parameters && i < item->parameter_count; i++)
            if (!visited[item->parameters[i]]) {
                visited[item->parameters[i]] = 1;
                stack[depth++] = item->parameters[i];
            }
    }
    return true;
}

/* The chain factory hands a step the value built so far. */
static enum mo_input_result chain_build(void *context, struct mo_heap *destination,
                                        const struct mo_input_request *request,
                                        struct mo_input_limits *remaining, struct mo_cell *out) {
    (void)request;
    (void)remaining;
    struct mo_hidden *hidden = context;
    MO_ASSERT(destination == hidden->heap);
    *out = mo_value_dup(destination, hidden->type, hidden->current);
    return MO_INPUT_OK;
}

static bool exposed_constant(const struct mo_program *program, const struct mo_bytecode *file,
                             uint32_t global) {
    const struct mo_bytecode_string *name = &file->strings[file->globals[global].name];
    for (uint32_t i = 0; i < program->constant_count; i++) {
        const struct mo_symbol *symbol = program->constants[i].symbol;
        if (!symbol->exported)
            continue;
        const char *module = program->interfaces->scopes[symbol->module].name;
        size_t prefix = module == NULL ? 0 : strlen(module);
        size_t length = strlen(symbol->name);
        if (module == NULL || name->length != prefix + 1 + length ||
            memcmp(name->bytes, module, prefix) != 0 || name->bytes[prefix] != '.' ||
            memcmp(name->bytes + prefix + 1, symbol->name, length) != 0)
            continue;
        return true;
    }
    return false;
}

static enum mo_input_result
prepare_builder(struct mo_hidden *hidden, const struct mo_bytecode *file, struct builder *builder) {
    if (builder->function == MO_NO_INDEX)
        return MO_INPUT_OK;
    const struct mo_bytecode_function *function = &file->functions[builder->function];
    uint32_t parameters[MO_LIMIT_BYTECODE_PARAMETERS];
    for (uint32_t i = 0; i < function->parameter_count; i++)
        parameters[i] = function->parameters[i].type;
    if (function->parameter_count == 0)
        return MO_INPUT_OK;
    const struct mo_bytecode_string *name = &file->strings[function->name];
    struct mo_input_limits limits = mo_input_limits_default();
    enum mo_input_result result =
        mo_inputs_init(&builder->arguments, mo_image_types(hidden->owner->image), parameters,
                       function->parameter_count, (const char *)name->bytes, name->length, &limits,
                       &hidden->chain, builder->chained ? 1 : 0);
    builder->generated = true;
    return result;
}

static enum mo_input_result build(void *context, struct mo_heap *destination,
                                  const struct mo_input_request *request,
                                  struct mo_input_limits *remaining, struct mo_cell *out);
static enum mo_input_result copy(void *context, struct mo_heap *destination, struct mo_cell source,
                                 struct mo_input_limits *remaining, struct mo_cell *out);

static enum mo_input_result prepare_hidden(struct mo_builders *builders,
                                           const struct mo_program *program,
                                           const bool *hidden_types, uint32_t module,
                                           struct mo_hidden *hidden, uint8_t *visited,
                                           uint32_t *stack) {
    const struct mo_bytecode *file = mo_image_bytecode(builders->image);
    const struct mo_types *types = mo_image_types(builders->image);
    const struct mo_bytecode_module *owner = &file->modules[module];
    hidden->chain = (struct mo_input_factory){
        .type = hidden->type, .boundaries = 1, .context = hidden, .build = chain_build};
    uint32_t most = owner->function_count + owner->global_count;
    hidden->builders =
        mo_arena_allocate(&builders->arena, (most == 0 ? 1 : most) * sizeof *hidden->builders,
                          _Alignof(struct builder));
    hidden->starts = mo_arena_allocate(&builders->arena, (most == 0 ? 1 : most) * sizeof(uint32_t),
                                       _Alignof(uint32_t));
    hidden->steps = mo_arena_allocate(&builders->arena, (most == 0 ? 1 : most) * sizeof(uint32_t),
                                      _Alignof(uint32_t));
    if (hidden->builders == NULL || hidden->starts == NULL || hidden->steps == NULL)
        return MO_INPUT_NO_MEMORY;
    struct mo_input_limits limits = mo_input_limits_default();
    enum mo_input_result result =
        mo_inputs_init(&hidden->copier, types, &hidden->type, 1, "", 0, &limits, NULL, 0);
    hidden->copier_ready = true;
    if (result == MO_INPUT_NO_MEMORY)
        return result;
    if (result != MO_INPUT_OK)
        return MO_INPUT_OK; /* No way to copy one, so none is built: the factory refuses. */
    hidden->chain.hashable = mo_inputs_hashable(&hidden->copier, hidden->type);
    for (uint32_t i = 0; i < owner->global_count; i++) {
        uint32_t global = owner->globals[i];
        if (file->globals[global].type != hidden->type || !exposed_constant(program, file, global))
            continue;
        hidden->builders[hidden->count] =
            (struct builder){.function = MO_NO_INDEX, .global = global};
        hidden->starts[hidden->start_count++] = hidden->count++;
    }
    for (uint32_t i = 0; i < owner->function_count; i++) {
        uint32_t index = owner->functions[i];
        const struct mo_bytecode_function *function = &file->functions[index];
        struct builder candidate = {.function = index, .global = MO_NO_INDEX};
        if (function->kind != MO_FUNCTION_FUNCTION || function->spec_code ||
            !find_path(types, function->result_type, hidden->type, 0, &candidate))
            continue;
        bool usable = true;
        for (uint32_t p = 0; usable && p < function->parameter_count; p++) {
            uint32_t type = function->parameters[p].type;
            if (type == hidden->type)
                candidate.chained = true;
            else
                usable = plain(types, hidden_types, type, visited, stack);
        }
        if (!usable)
            continue;
        struct builder *builder = &hidden->builders[hidden->count];
        *builder = candidate;
        result = prepare_builder(hidden, file, builder);
        if (result == MO_INPUT_NO_MEMORY)
            return result;
        if (result != MO_INPUT_OK) {
            mo_inputs_free(&builder->arguments);
            continue;
        }
        if (builder->chained)
            hidden->steps[hidden->step_count++] = hidden->count;
        else
            hidden->starts[hidden->start_count++] = hidden->count;
        hidden->count++;
    }
    return MO_INPUT_OK;
}

enum mo_input_result mo_builders_init(struct mo_builders *builders,
                                      const struct mo_program *program,
                                      const struct mo_image *image, const struct mo_limits *limits,
                                      struct mo_deadline *deadline) {
    *builders = (struct mo_builders){.image = image, .limits = limits, .deadline = deadline};
    mo_arena_init(&builders->arena, MO_CLI_TEST_SIZE);
    const struct mo_bytecode *file = mo_image_bytecode(image);
    const struct mo_types *types = mo_image_types(image);
    bool *hidden_types = mo_arena_allocate(&builders->arena, types->count + 1, 1);
    uint8_t *visited = mo_arena_allocate(&builders->arena, types->count + 1, 1);
    uint32_t *stack = mo_arena_allocate(&builders->arena, (types->count + 1) * sizeof(uint32_t),
                                        _Alignof(uint32_t));
    uint32_t *modules = mo_arena_allocate(&builders->arena, (types->count + 1) * sizeof(uint32_t),
                                          _Alignof(uint32_t));
    if (hidden_types == NULL || visited == NULL || stack == NULL || modules == NULL)
        return MO_INPUT_NO_MEMORY;
    memset(hidden_types, 0, types->count + 1);
    for (uint32_t m = 0; m < file->module_count; m++)
        for (uint32_t i = 0; i < file->modules[m].type_count; i++) {
            const struct mo_bytecode_type_export *exported = &file->modules[m].types[i];
            if (exported->contents_exposed || hidden_types[exported->type])
                continue;
            hidden_types[exported->type] = true;
            modules[builders->count++] = m;
        }
    builders->hidden =
        mo_arena_allocate(&builders->arena, (builders->count + 1) * sizeof *builders->hidden,
                          _Alignof(struct mo_hidden));
    builders->factories =
        mo_arena_allocate(&builders->arena, (builders->count + 1) * sizeof *builders->factories,
                          _Alignof(struct mo_input_factory));
    if (builders->hidden == NULL || builders->factories == NULL) {
        builders->count = 0;
        return MO_INPUT_NO_MEMORY;
    }
    uint32_t count = builders->count;
    builders->count = 0;
    for (uint32_t t = 0; t < types->count && builders->count < count; t++) {
        if (!hidden_types[t])
            continue;
        uint32_t slot = builders->count++;
        struct mo_hidden *hidden = &builders->hidden[slot];
        *hidden = (struct mo_hidden){.owner = builders, .type = t};
        enum mo_input_result result =
            prepare_hidden(builders, program, hidden_types, modules[slot], hidden, visited, stack);
        builders->factories[slot] = (struct mo_input_factory){.type = t,
                                                              .boundaries = BOUNDARIES,
                                                              .hashable = hidden->chain.hashable,
                                                              .context = hidden,
                                                              .build = build,
                                                              .copy = copy};
        if (result != MO_INPUT_OK)
            return result;
    }
    return MO_INPUT_OK;
}

void mo_builders_free(struct mo_builders *builders) {
    for (uint32_t h = 0; builders->hidden != NULL && h < builders->count; h++) {
        struct mo_hidden *hidden = &builders->hidden[h];
        if (hidden->copier_ready)
            mo_inputs_free(&hidden->copier);
        for (uint32_t i = 0; i < hidden->count; i++)
            if (hidden->builders[i].generated)
                mo_inputs_free(&hidden->builders[i].arguments);
    }
    mo_arena_free(&builders->arena);
    *builders = (struct mo_builders){0};
}

enum step { STEP_BUILT, STEP_SKIPPED, STEP_ENDED, STEP_CANCELLED, STEP_REFUSED };

/* One call, in process. BUILT replaces *value (owned) with the new one; SKIPPED leaves it, since
 * the call returned without a value or its arguments couldn't be made; ENDED means the call
 * stopped or failed and the process can run nothing more, though *value is still readable. */
static enum step call(struct mo_hidden *hidden, struct mo_process *process, struct builder *builder,
                      uint64_t ordinal, bool *has_value, struct mo_cell *value) {
    struct mo_builders *builders = hidden->owner;
    const struct mo_types *types = mo_image_types(builders->image);
    struct mo_heap *heap = mo_process_heap(process);
    struct mo_cell made;
    if (builder->function == MO_NO_INDEX) {
        struct mo_cell global;
        if (!mo_image_global(builders->image, builder->global, &global))
            return STEP_SKIPPED;
        made = mo_value_dup(heap, hidden->type, global);
    } else {
        const struct mo_bytecode *file = mo_image_bytecode(builders->image);
        const struct mo_bytecode_function *function = &file->functions[builder->function];
        struct mo_cell arguments[MO_LIMIT_BYTECODE_PARAMETERS];
        if (builder->generated) {
            struct mo_inputs *generator = &builder->arguments;
            hidden->heap = heap;
            hidden->current = *value;
            MO_ASSERT(!builder->chained || *has_value);
            enum mo_input_kind kind;
            mo_inputs_seek(generator, ordinal);
            enum mo_input_result generated = mo_inputs_next(generator, heap, arguments, &kind);
            if (generated == MO_INPUT_NO_MEMORY)
                return STEP_REFUSED;
            if (generated != MO_INPUT_OK)
                return STEP_SKIPPED;
        }
        struct mo_cell result;
        struct mo_defect defect;
        enum mo_controlled_outcome outcome =
            mo_call_controlled(process, builder->function, arguments, function->parameter_count,
                               mo_deadline_running, builders->deadline, &result, &defect);
        for (uint32_t i = 0; i < function->parameter_count; i++)
            if (function->parameters[i].ownership == MO_PARAMETER_BORROWED)
                mo_value_drop(heap, function->parameters[i].type, arguments[i]);
        if (outcome == MO_CONTROL_CANCELLED)
            return STEP_CANCELLED;
        if (outcome != MO_CONTROL_RETURNED)
            return STEP_ENDED;
        struct mo_cell found;
        bool counts = extract(types, builder, function->result_type, result, &found);
        if (counts)
            made = mo_value_dup(heap, hidden->type, found);
        mo_value_drop(heap, function->result_type, result);
        if (!counts)
            return STEP_SKIPPED;
    }
    if (*has_value)
        mo_value_drop(heap, hidden->type, *value);
    *value = made;
    *has_value = true;
    return STEP_BUILT;
}

/* A random input's ordinal in a builder's generator, near the trial's size. */
static uint64_t random_ordinal(const struct builder *builder, uint64_t *random, uint32_t size) {
    if (!builder->generated)
        return 0;
    uint64_t grown = size > 64 ? 64 : size;
    return mo_inputs_boundaries(&builder->arguments) + 4 * (grown == 0 ? 0 : grown - 1) +
           next_random(random) % 4;
}

static enum mo_input_result build(void *context, struct mo_heap *destination,
                                  const struct mo_input_request *request,
                                  struct mo_input_limits *remaining, struct mo_cell *out) {
    struct mo_hidden *hidden = context;
    struct mo_builders *builders = hidden->owner;
    if (hidden->start_count == 0)
        return MO_INPUT_UNSUPPORTED;
    bool boundary = request->kind == MO_INPUT_BOUNDARY;
    uint64_t random = boundary ? UINT64_C(0x6275696c64657273) + request->boundary : request->seed;
    uint32_t steps = boundary ? (request->boundary == 0   ? 0
                                 : request->boundary == 1 ? 1
                                                          : STEPS)
                              : (uint32_t)(next_random(&random) %
                                           ((request->size < STEPS ? request->size : STEPS) + 1));
    if (hidden->step_count == 0)
        steps = 0;
    struct mo_process *process = NULL;
    struct mo_cell value = mo_cell_unit();
    bool has_value = false;
    enum mo_input_result result = MO_INPUT_LIMIT;
    /* A start counts when it returns a value; boundaries go through the starts and their boundary
     * inputs in order, and random inputs fall back to that order after half their attempts. */
    for (uint32_t attempt = 0; attempt < MO_CLI_BUILDER_ATTEMPTS && !has_value; attempt++) {
        if (process == NULL &&
            (process = mo_process_new(builders->image, builders->limits)) == NULL)
            return MO_INPUT_NO_MEMORY;
        bool ordered = boundary || attempt >= MO_CLI_BUILDER_ATTEMPTS / 2;
        uint32_t rank = boundary ? attempt : attempt - MO_CLI_BUILDER_ATTEMPTS / 2;
        struct builder *start =
            &hidden->builders[hidden->starts[ordered ? rank % hidden->start_count
                                                     : next_random(&random) % hidden->start_count]];
        uint64_t ordinal =
            ordered ? rank / hidden->start_count : random_ordinal(start, &random, request->size);
        enum step outcome = call(hidden, process, start, ordinal, &has_value, &value);
        if (outcome == STEP_CANCELLED || outcome == STEP_REFUSED) {
            result = outcome == STEP_REFUSED ? MO_INPUT_NO_MEMORY : MO_INPUT_LIMIT;
            goto done;
        }
        if (outcome == STEP_ENDED) {
            mo_process_free(process);
            process = NULL;
        }
    }
    if (!has_value)
        goto done;
    for (uint32_t i = 0; i < steps; i++) {
        struct builder *step =
            &hidden->builders[hidden->steps[boundary ? i % hidden->step_count
                                                     : next_random(&random) % hidden->step_count]];
        uint64_t ordinal = boundary ? 1 + i : random_ordinal(step, &random, request->size);
        enum step outcome = call(hidden, process, step, ordinal, &has_value, &value);
        if (outcome == STEP_CANCELLED || outcome == STEP_REFUSED) {
            result = outcome == STEP_REFUSED ? MO_INPUT_NO_MEMORY : MO_INPUT_LIMIT;
            goto done;
        }
        if (outcome == STEP_ENDED)
            break;
    }
    result = mo_inputs_copy(&hidden->copier, destination, &value, remaining, out);
done:
    if (process != NULL) {
        if (has_value)
            mo_value_drop(mo_process_heap(process), hidden->type, value);
        mo_process_free(process);
    }
    return result;
}

static enum mo_input_result copy(void *context, struct mo_heap *destination, struct mo_cell source,
                                 struct mo_input_limits *remaining, struct mo_cell *out) {
    struct mo_hidden *hidden = context;
    if (hidden->start_count == 0)
        return MO_INPUT_UNSUPPORTED;
    return mo_inputs_copy(&hidden->copier, destination, &source, remaining, out);
}
