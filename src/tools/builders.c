#include "tools/builders.h"

#include "base/hash.h"

#include <string.h>

#define BOUNDARIES 3
#define STEPS (MO_CLI_BUILDER_CALLS - 1)
#define UNLEVELLED UINT32_MAX

/* An exposed constant of the hidden type, or an exposed function whose result holds one. */
struct builder {
    uint32_t function; /* MO_NO_INDEX for a constant */
    uint32_t global;
    /* (case tag, field) pairs from the result to the value; the tag is MO_NO_INDEX for a tuple. */
    uint32_t path[2 * MO_CLI_BUILDER_PATH];
    uint32_t path_length;
    bool chained; /* takes the hidden type: a step, not a start */
    /* The first other field of a tuple result holding a hidden type, kept as the value's
     * companion (D284); MO_NO_INDEX for none. */
    uint32_t companion_field, companion_slot;
    bool generated;
    /* The other hidden types among its inputs, as slots in mo_builders, each once. */
    uint32_t *needs;
    uint32_t need_count;
    struct mo_inputs arguments;
};

struct mo_build_call {
    struct builder *builder;
    uint64_t ordinal;
    struct mo_build_recipe *children;
    uint32_t slot;
    struct mo_cell *arguments;
    struct mo_build_call *next_frozen;
};

struct mo_build_recipe {
    struct mo_build_recipe *next;
    struct mo_hidden *hidden;
    uint32_t id;
    uint32_t nodes;
    size_t bytes;
    struct mo_build_call calls[MO_CLI_BUILDER_CALLS];
    uint32_t count;
    bool locked;
};

struct mo_builder_replay {
    const struct mo_build_trace *source;
    const struct mo_build_recipe *scope;
    uint32_t node, mask;
    uint32_t active, slot;
    const struct mo_cell *replacement;
    struct mo_process *capture;
    struct mo_cell *captured;
    bool captured_arguments;
    uint64_t work;
    bool invalid;
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
    /* 0 when a start needs no other hidden type, else one more than the highest level among a
     * start's needs; UNLEVELLED when no start qualifies within MO_CLI_BUILDER_LEVELS. A builder
     * takes only hidden types of lower levels, so building one never comes back to its type. */
    uint32_t level;
    /* A step's generator takes the value built so far for each parameter of the hidden type. */
    struct mo_input_factory chain;
    struct mo_heap *heap;
    struct mo_cell current;
    bool building; /* levels keep a build from reaching its own type again */
    /* The companion the call that made the value so far returned, in its scratch process. */
    struct mo_cell companion;
    uint32_t companion_slot; /* MO_NO_INDEX for none */
};

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

/* Whether a generator can make a value of type for a builder: no capability or function inside
 * it. A hidden type inside it comes from that type's own factory, so the walk stops there and
 * adds its slot to needs, once per builder. visited is scratch of one byte per type, cleared for
 * each builder; slots maps a type to its hidden slot or MO_NO_INDEX. */
static bool buildable(const struct mo_types *types, const uint32_t *slots, uint32_t type,
                      uint8_t *visited, uint32_t *stack, uint32_t *needs, uint32_t *need_count) {
    if (visited[type])
        return true;
    uint32_t depth = 0;
    stack[depth++] = type;
    visited[type] = 1;
    while (depth != 0) {
        uint32_t at = stack[--depth];
        const struct mo_type *item = &types->items[at];
        if (slots[at] != MO_NO_INDEX) {
            needs[(*need_count)++] = slots[at];
            continue;
        }
        if (item->kind == MO_TYPE_FUNCTION || item->kind == MO_TYPE_ENV ||
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

/* A builder's arguments come from the chain for the hidden type itself and the factories of the
 * lower-level hidden types it needs. */
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
    struct mo_builders *builders = hidden->owner;
    uint32_t count = (builder->chained ? 1 : 0) + builder->need_count;
    struct mo_input_factory *factories =
        mo_arena_allocate(&builders->arena, (count == 0 ? 1 : count) * sizeof *factories,
                          _Alignof(struct mo_input_factory));
    if (factories == NULL)
        return MO_INPUT_NO_MEMORY;
    uint32_t used = 0;
    if (builder->chained)
        factories[used++] = hidden->chain;
    for (uint32_t i = 0; i < builder->need_count; i++)
        factories[used++] = builders->factories[builder->needs[i]];
    const struct mo_bytecode_string *name = &file->strings[function->name];
    struct mo_input_limits limits = mo_input_limits_default();
    enum mo_input_result result = mo_inputs_init(
        &builder->arguments, mo_image_types(builders->image), parameters, function->parameter_count,
        (const char *)name->bytes, name->length, &limits, factories, count);
    builder->generated = true;
    return result;
}

static enum mo_input_result build(void *context, struct mo_heap *destination,
                                  const struct mo_input_request *request,
                                  struct mo_input_limits *remaining, struct mo_cell *out);
static enum mo_input_result copy(void *context, struct mo_heap *destination, struct mo_cell source,
                                 struct mo_input_limits *remaining, struct mo_cell *out);

/* The copier that moves a built value into a trial, which also says whether it is hashable. */
static enum mo_input_result prepare_copier(struct mo_builders *builders, struct mo_hidden *hidden,
                                           bool *copyable) {
    const struct mo_types *types = mo_image_types(builders->image);
    hidden->chain = (struct mo_input_factory){.type = hidden->type,
                                              .boundaries = 1,
                                              .context = hidden,
                                              .build = chain_build,
                                              .copy = copy};
    struct mo_input_limits limits = mo_input_limits_default();
    enum mo_input_result result =
        mo_inputs_init(&hidden->copier, types, &hidden->type, 1, "", 0, &limits, NULL, 0);
    hidden->copier_ready = true;
    *copyable = result == MO_INPUT_OK;
    if (result == MO_INPUT_NO_MEMORY)
        return result;
    if (*copyable)
        hidden->chain.hashable = mo_inputs_hashable(&hidden->copier, hidden->type);
    return MO_INPUT_OK;
}

/* Every exposed constant and function of the module that yields the hidden type from inputs a
 * generator can make, with the other hidden types each needs and its prepared generator. Which
 * of them start and step waits for the levels. */
static enum mo_input_result collect_builders(struct mo_builders *builders,
                                             const struct mo_program *program,
                                             const uint32_t *slots, uint32_t module,
                                             struct mo_hidden *hidden, uint8_t *visited,
                                             uint32_t *stack, uint32_t *needs) {
    const struct mo_bytecode *file = mo_image_bytecode(builders->image);
    const struct mo_types *types = mo_image_types(builders->image);
    const struct mo_bytecode_module *owner = &file->modules[module];
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
    for (uint32_t i = 0; i < owner->global_count; i++) {
        uint32_t global = owner->globals[i];
        if (file->globals[global].type != hidden->type || !exposed_constant(program, file, global))
            continue;
        hidden->builders[hidden->count++] = (struct builder){.function = MO_NO_INDEX,
                                                             .global = global,
                                                             .companion_field = MO_NO_INDEX,
                                                             .companion_slot = MO_NO_INDEX};
    }
    for (uint32_t i = 0; i < owner->function_count; i++) {
        uint32_t index = owner->functions[i];
        const struct mo_bytecode_function *function = &file->functions[index];
        struct builder candidate = {.function = index,
                                    .global = MO_NO_INDEX,
                                    .companion_field = MO_NO_INDEX,
                                    .companion_slot = MO_NO_INDEX};
        if (function->kind != MO_FUNCTION_FUNCTION || function->spec_code ||
            !find_path(types, function->result_type, hidden->type, 0, &candidate))
            continue;
        const struct mo_type *returned = &types->items[function->result_type];
        for (uint32_t f = 0;
             candidate.path_length != 0 && candidate.path[0] == MO_NO_INDEX &&
             candidate.companion_field == MO_NO_INDEX && f < returned->parameter_count;
             f++)
            if (f != candidate.path[1] && slots[returned->parameters[f]] != MO_NO_INDEX) {
                candidate.companion_field = f;
                candidate.companion_slot = slots[returned->parameters[f]];
            }
        memset(visited, 0, types->count);
        bool usable = true;
        for (uint32_t p = 0; usable && p < function->parameter_count; p++) {
            uint32_t type = function->parameters[p].type;
            if (type == hidden->type)
                candidate.chained = true;
            else
                usable =
                    buildable(types, slots, type, visited, stack, needs, &candidate.need_count);
        }
        if (!usable)
            continue;
        if (candidate.need_count != 0) {
            candidate.needs = mo_arena_allocate(
                &builders->arena, candidate.need_count * sizeof(uint32_t), _Alignof(uint32_t));
            if (candidate.needs == NULL)
                return MO_INPUT_NO_MEMORY;
            memcpy(candidate.needs, needs, candidate.need_count * sizeof(uint32_t));
        }
        struct builder *builder = &hidden->builders[hidden->count];
        *builder = candidate;
        enum mo_input_result result = prepare_builder(hidden, file, builder);
        if (result == MO_INPUT_OK) {
            hidden->count++;
            continue;
        }
        if (builder->generated)
            mo_inputs_free(&builder->arguments);
        if (result == MO_INPUT_NO_MEMORY)
            return result;
    }
    return MO_INPUT_OK;
}

/* Whether every hidden type a builder needs has a level below level. */
static bool below(const struct mo_builders *builders, const struct builder *builder,
                  uint32_t level) {
    for (uint32_t i = 0; i < builder->need_count; i++)
        if (builders->hidden[builder->needs[i]].level >= level)
            return false;
    return true;
}

enum mo_input_result mo_builders_init(struct mo_builders *builders,
                                      const struct mo_program *program,
                                      const struct mo_image *image, const struct mo_limits *limits,
                                      struct mo_deadline *deadline) {
    *builders = (struct mo_builders){.image = image, .limits = limits, .deadline = deadline};
    mo_arena_init(&builders->arena, MO_CLI_TEST_SIZE);
    const struct mo_bytecode *file = mo_image_bytecode(image);
    const struct mo_types *types = mo_image_types(image);
    uint32_t *slots = mo_arena_allocate(&builders->arena, (types->count + 1) * sizeof(uint32_t),
                                        _Alignof(uint32_t));
    uint8_t *visited = mo_arena_allocate(&builders->arena, types->count + 1, 1);
    uint32_t *stack = mo_arena_allocate(&builders->arena, (types->count + 1) * sizeof(uint32_t),
                                        _Alignof(uint32_t));
    uint32_t *modules = mo_arena_allocate(&builders->arena, (types->count + 1) * sizeof(uint32_t),
                                          _Alignof(uint32_t));
    uint32_t *needs = mo_arena_allocate(&builders->arena, (types->count + 1) * sizeof(uint32_t),
                                        _Alignof(uint32_t));
    if (slots == NULL || visited == NULL || stack == NULL || modules == NULL || needs == NULL)
        return MO_INPUT_NO_MEMORY;
    for (uint32_t t = 0; t < types->count; t++)
        slots[t] = MO_NO_INDEX;
    for (uint32_t m = 0; m < file->module_count; m++)
        for (uint32_t i = 0; i < file->modules[m].type_count; i++) {
            const struct mo_bytecode_type_export *exported = &file->modules[m].types[i];
            if (exported->contents_exposed || slots[exported->type] != MO_NO_INDEX)
                continue;
            slots[exported->type] = 0;
            modules[exported->type] = m;
            builders->count++;
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
    /* Every factory exists before any builder's generator takes it. */
    uint32_t count = builders->count;
    builders->count = 0;
    bool *copyable = mo_arena_allocate(&builders->arena, count + 1, 1);
    if (copyable == NULL)
        return MO_INPUT_NO_MEMORY;
    for (uint32_t t = 0; t < types->count && builders->count < count; t++) {
        if (slots[t] == MO_NO_INDEX)
            continue;
        uint32_t slot = builders->count++;
        slots[t] = slot;
        struct mo_hidden *hidden = &builders->hidden[slot];
        *hidden = (struct mo_hidden){
            .owner = builders, .type = t, .level = UNLEVELLED, .companion_slot = MO_NO_INDEX};
        enum mo_input_result result = prepare_copier(builders, hidden, &copyable[slot]);
        builders->factories[slot] = (struct mo_input_factory){.type = t,
                                                              .boundaries = BOUNDARIES,
                                                              .hashable = hidden->chain.hashable,
                                                              .context = hidden,
                                                              .build = build,
                                                              .copy = copy};
        if (result != MO_INPUT_OK)
            return result;
    }
    for (uint32_t slot = 0; slot < count; slot++) {
        struct mo_hidden *hidden = &builders->hidden[slot];
        if (!copyable[slot])
            continue; /* No way to copy one, so none is built: the factory refuses. */
        enum mo_input_result result = collect_builders(
            builders, program, slots, modules[hidden->type], hidden, visited, stack, needs);
        if (result != MO_INPUT_OK)
            return result;
    }
    /* Round r gives level r to each type with a start whose needs all got levels before it. */
    for (uint32_t round = 0; round < MO_CLI_BUILDER_LEVELS; round++)
        for (uint32_t slot = 0; slot < count; slot++) {
            struct mo_hidden *hidden = &builders->hidden[slot];
            for (uint32_t i = 0; hidden->level == UNLEVELLED && i < hidden->count; i++)
                if (!hidden->builders[i].chained && below(builders, &hidden->builders[i], round))
                    hidden->level = round;
        }
    /* A type with no level keeps no starts, so its factory refuses. */
    for (uint32_t slot = 0; slot < count; slot++) {
        struct mo_hidden *hidden = &builders->hidden[slot];
        for (uint32_t i = 0; hidden->level != UNLEVELLED && i < hidden->count; i++) {
            if (!below(builders, &hidden->builders[i], hidden->level))
                continue;
            if (hidden->builders[i].chained)
                hidden->steps[hidden->step_count++] = i;
            else
                hidden->starts[hidden->start_count++] = i;
        }
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

static void drop_companion(struct mo_hidden *hidden, struct mo_heap *heap) {
    if (hidden->companion_slot != MO_NO_INDEX)
        mo_value_drop(heap, hidden->owner->hidden[hidden->companion_slot].type, hidden->companion);
    hidden->companion_slot = MO_NO_INDEX;
}

enum step { STEP_BUILT, STEP_SKIPPED, STEP_ENDED, STEP_CANCELLED, STEP_REFUSED };

void mo_build_trace_free(struct mo_build_trace *trace) {
    for (struct mo_build_call *call = trace->frozen; call != NULL; call = call->next_frozen) {
        struct mo_heap *heap = mo_process_heap(trace->snapshots);
        const struct mo_inputs *inputs = &call->builder->arguments;
        for (uint32_t i = 0; i < inputs->parameter_count; i++)
            mo_value_drop(heap, inputs->parameters[i], call->arguments[i]);
    }
    if (trace->snapshots != NULL)
        mo_process_free(trace->snapshots);
    mo_arena_free(&trace->arena);
    *trace = (struct mo_build_trace){0};
}

static struct mo_build_recipe *record_build(struct mo_hidden *hidden) {
    struct mo_build_trace *trace = hidden->owner->recording;
    if (trace == NULL || !trace->complete)
        return NULL;
    struct mo_build_recipe *recipe = NULL;
    if (trace->next_id < MO_CLI_BUILDER_RECIPES)
        recipe = mo_arena_allocate(&trace->arena, sizeof *recipe, _Alignof(struct mo_build_recipe));
    if (recipe == NULL) {
        trace->complete = false;
        return NULL;
    }
    *recipe = (struct mo_build_recipe){.hidden = hidden, .id = trace->next_id++};
    return recipe;
}

static enum mo_input_result freeze_arguments(struct mo_builders *builders,
                                             struct mo_build_call *record,
                                             const struct mo_cell *arguments) {
    struct mo_build_trace *trace = builders->recording;
    if (trace == NULL)
        return MO_INPUT_OK;
    if (!trace->complete)
        return MO_INPUT_LIMIT;
    const struct mo_inputs *inputs = &record->builder->arguments;
    if (trace->snapshots == NULL)
        trace->snapshots = mo_process_new(builders->image, builders->limits);
    struct mo_build_call *frozen =
        mo_arena_allocate(&trace->arena, sizeof *frozen, _Alignof(struct mo_build_call));
    struct mo_cell *cells = mo_arena_allocate(
        &trace->arena, inputs->parameter_count * sizeof *cells, _Alignof(struct mo_cell));
    if (trace->snapshots == NULL || frozen == NULL || cells == NULL)
        return MO_INPUT_NO_MEMORY;
    enum mo_input_result result = mo_inputs_copy(inputs, mo_process_heap(trace->snapshots),
                                                 arguments, &trace->snapshot_limits, cells);
    if (result != MO_INPUT_OK)
        return result;
    *frozen = (struct mo_build_call){
        .builder = record->builder, .arguments = cells, .next_frozen = trace->frozen};
    trace->frozen = frozen;
    record->arguments = cells;
    return MO_INPUT_OK;
}

/* One call, in process. BUILT replaces *value (owned) with the new one; SKIPPED leaves it, since
 * the call returned without a value or its arguments couldn't be made; ENDED means the call
 * stopped or failed and the process can run nothing more, though *value is still readable. */
static enum step call(struct mo_hidden *hidden, struct mo_process *process, struct builder *builder,
                      uint64_t ordinal, bool *has_value, struct mo_cell *value,
                      const struct mo_build_call *source, struct mo_build_call *record) {
    struct mo_builders *builders = hidden->owner;
    const struct mo_types *types = mo_image_types(builders->image);
    struct mo_heap *heap = mo_process_heap(process);
    struct mo_cell made;
    if (builder->function == MO_NO_INDEX) {
        struct mo_cell global;
        if (!mo_image_global(builders->image, builder->global, &global))
            return STEP_SKIPPED;
        made = mo_value_dup(heap, hidden->type, global);
        drop_companion(hidden, heap);
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
            struct mo_builder_replay *replay = builders->replay;
            bool selected =
                source != NULL && replay->active == replay->node && source->slot == replay->slot;
            const struct mo_cell *frozen = selected && replay->replacement != NULL
                                               ? replay->replacement
                                           : source == NULL ? NULL
                                                            : source->arguments;
            enum mo_input_result generated;
            if (frozen != NULL) {
                struct mo_input_limits limits = generator->limits;
                generated = mo_inputs_copy(generator, heap, frozen, &limits, arguments);
                if (generated == MO_INPUT_OK && builder->chained)
                    for (uint32_t i = 0; i < generator->parameter_count; i++)
                        if (generator->parameters[i] == hidden->type) {
                            mo_value_drop(heap, hidden->type, arguments[i]);
                            arguments[i] = mo_value_dup(heap, hidden->type, *value);
                        }
                replay->scope = NULL;
            } else {
                mo_inputs_seek(generator, ordinal);
                generated = mo_inputs_next(generator, heap, arguments, &kind);
            }
            if (generated == MO_INPUT_NO_MEMORY)
                return STEP_REFUSED;
            if (generated != MO_INPUT_OK)
                return STEP_SKIPPED;
            enum mo_input_result saved = MO_INPUT_OK;
            if (selected && replay->capture != NULL) {
                struct mo_input_limits limits = generator->limits;
                saved = mo_inputs_copy(generator, mo_process_heap(replay->capture), arguments,
                                       &limits, replay->captured);
                replay->captured_arguments = saved == MO_INPUT_OK;
            }
            if (saved == MO_INPUT_OK && frozen != NULL)
                saved = freeze_arguments(builders, record, arguments);
            if (saved != MO_INPUT_OK) {
                for (uint32_t i = 0; i < generator->parameter_count; i++)
                    mo_value_drop(heap, generator->parameters[i], arguments[i]);
                return STEP_REFUSED;
            }
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
        if (counts) {
            made = mo_value_dup(heap, hidden->type, found);
            drop_companion(hidden, heap);
            if (builder->companion_slot != MO_NO_INDEX) {
                hidden->companion_slot = builder->companion_slot;
                hidden->companion =
                    mo_value_dup(heap, builders->hidden[builder->companion_slot].type,
                                 mo_value_field(result, builder->companion_field));
            }
        }
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

static enum step record_call(struct mo_hidden *hidden, struct mo_process *process,
                             struct builder *builder, uint64_t ordinal, bool *has_value,
                             struct mo_cell *value, struct mo_build_recipe *recipe,
                             const struct mo_build_call *source) {
    struct mo_builders *builders = hidden->owner;
    struct mo_build_call recorded = {.builder = builder, .ordinal = ordinal};
    struct mo_build_recipe **parent = builders->scope;
    builders->scope = &recorded.children;
    enum step result = call(hidden, process, builder, ordinal, has_value, value, source, &recorded);
    builders->scope = parent;
    if (result == STEP_BUILT && recipe != NULL) {
        MO_ASSERT(recipe->count < MO_CLI_BUILDER_CALLS);
        recorded.slot = recipe->count;
        recipe->calls[recipe->count++] = recorded;
    }
    return result;
}

/* A random input's ordinal in a builder's generator, near the trial's size. */
static uint64_t random_ordinal(const struct builder *builder, uint64_t *random, uint32_t size) {
    if (!builder->generated)
        return 0;
    uint64_t grown = size > 64 ? 64 : size;
    return mo_inputs_boundaries(&builder->arguments) + 4 * (grown == 0 ? 0 : grown - 1) +
           mo_splitmix64(random) % 4;
}

/* The generator already counted the value's root node, so the copy's own count of it is
 * given back. */
static enum mo_input_result copy_value(struct mo_hidden *hidden, struct mo_heap *destination,
                                       struct mo_cell value, struct mo_input_limits *remaining,
                                       struct mo_cell *out) {
    uint32_t nodes = remaining->nodes;
    enum mo_input_result result =
        mo_inputs_copy(&hidden->copier, destination, &value, remaining, out);
    if (remaining->nodes < nodes)
        remaining->nodes++;
    return result;
}

static void keep_companion(struct mo_hidden *hidden, struct mo_heap *destination, uint32_t origin) {
    struct mo_builders *builders = hidden->owner;
    if (hidden->companion_slot == MO_NO_INDEX || destination != builders->trial ||
        builders->companion_count == MO_CLI_BUILDER_COMPANIONS)
        return;
    if (builders->replay != NULL) {
        bool bound = false;
        const struct mo_build_trace *source = builders->replay->source;
        for (uint32_t i = 0; i < source->binding_count; i++)
            bound |= source->bindings[i].origin == origin;
        if (!bound)
            return;
    }
    const struct mo_input_factory *factory = &builders->factories[hidden->companion_slot];
    struct mo_companion *kept = &builders->companions[builders->companion_count];
    struct mo_input_limits limits = mo_input_limits_default();
    kept->type = factory->type;
    kept->owner = hidden->type;
    kept->origin = origin;
    if (factory->copy(factory->context, destination, hidden->companion, &limits, &kept->value) ==
        MO_INPUT_OK)
        builders->companion_count++;
}

static enum mo_input_result replay_build(struct mo_hidden *hidden,
                                         const struct mo_build_recipe *source,
                                         struct mo_heap *destination,
                                         struct mo_input_limits *remaining, struct mo_cell *out) {
    struct mo_builders *builders = hidden->owner;
    struct mo_builder_replay *replay = builders->replay;
    MO_ASSERT(!hidden->building && source->hidden == hidden && source->count != 0);
    if (replay->work == 0 || !mo_deadline_running(builders->deadline))
        return MO_INPUT_LIMIT;
    replay->work--;
    struct mo_process *process = mo_process_new(builders->image, builders->limits);
    if (process == NULL)
        return MO_INPUT_NO_MEMORY;
    hidden->building = true;
    hidden->companion_slot = MO_NO_INDEX;
    struct mo_cell value = mo_cell_unit();
    bool has_value = false;
    struct mo_build_recipe *recipe = record_build(hidden);
    if (recipe != NULL) {
        recipe->id = source->id;
        recipe->locked = source->locked;
        recipe->nodes = source->nodes;
        recipe->bytes = source->bytes;
    }
    const struct mo_build_recipe *parent = replay->scope;
    uint32_t parent_active = replay->active;
    replay->active = source->id;
    enum mo_input_result result = MO_INPUT_DONE;
    for (uint32_t i = 0; i < source->count; i++) {
        if (i != 0 && source->id == replay->node && (replay->mask & (1u << (i - 1))) == 0)
            continue;
        const struct mo_build_call *item = &source->calls[i];
        replay->scope = item->children;
        enum step step = record_call(hidden, process, item->builder, item->ordinal, &has_value,
                                     &value, recipe, item);
        if (step != STEP_BUILT) {
            if (step == STEP_CANCELLED)
                result = MO_INPUT_LIMIT;
            else if (step == STEP_REFUSED)
                result = MO_INPUT_NO_MEMORY;
            else
                replay->invalid = true;
            goto done;
        }
        MO_ASSERT(replay->scope == NULL);
        if (recipe != NULL)
            recipe->calls[recipe->count - 1].slot = item->slot;
    }
    if (source->nodes > remaining->nodes || source->bytes > remaining->bytes) {
        replay->invalid = true;
        goto done;
    }
    struct mo_input_limits reserved = *remaining;
    reserved.nodes = source->nodes + 1;
    reserved.bytes = source->bytes;
    result = copy_value(hidden, destination, value, &reserved, out);
    remaining->work = reserved.work;
    if (result == MO_INPUT_LIMIT) {
        replay->invalid = true;
        result = MO_INPUT_DONE;
    }
    if (result == MO_INPUT_OK) {
        remaining->nodes -= source->nodes;
        remaining->bytes -= source->bytes;
        if (recipe != NULL) {
            *builders->scope = recipe;
            builders->scope = &recipe->next;
        }
        keep_companion(hidden, destination, source->id);
    }
done:
    replay->scope = parent;
    replay->active = parent_active;
    drop_companion(hidden, mo_process_heap(process));
    if (has_value)
        mo_value_drop(mo_process_heap(process), hidden->type, value);
    mo_process_free(process);
    hidden->building = false;
    return result;
}

static enum mo_input_result build(void *context, struct mo_heap *destination,
                                  const struct mo_input_request *request,
                                  struct mo_input_limits *remaining, struct mo_cell *out) {
    struct mo_hidden *hidden = context;
    struct mo_builders *builders = hidden->owner;
    if (builders->replay != NULL) {
        const struct mo_build_recipe *source = builders->replay->scope;
        MO_ASSERT(source != NULL && source->hidden == hidden);
        builders->replay->scope = source->next;
        return replay_build(hidden, source, destination, remaining, out);
    }
    if (hidden->start_count == 0)
        return MO_INPUT_UNSUPPORTED;
    MO_ASSERT(!hidden->building);
    hidden->building = true;
    struct mo_build_recipe *recipe = record_build(hidden);
    uint32_t nodes_before = remaining->nodes;
    size_t bytes_before = remaining->bytes;
    bool boundary = request->kind == MO_INPUT_BOUNDARY;
    uint64_t random = boundary ? UINT64_C(0x6275696c64657273) + request->boundary : request->seed;
    uint32_t steps = boundary ? (request->boundary == 0   ? 0
                                 : request->boundary == 1 ? 1
                                                          : STEPS)
                              : (uint32_t)(mo_splitmix64(&random) %
                                           ((request->size < STEPS ? request->size : STEPS) + 1));
    if (hidden->step_count == 0)
        steps = 0;
    struct mo_process *process = NULL;
    struct mo_cell value = mo_cell_unit();
    bool has_value = false;
    hidden->companion_slot = MO_NO_INDEX;
    enum mo_input_result result = MO_INPUT_LIMIT;
    /* A start counts when it returns a value; boundaries go through the starts and their boundary
     * inputs in order. Random inputs first make half as many attempts of their own, then fall back
     * to that same order, so a random value can be built whenever boundary 0 can. */
    uint32_t random_attempts = boundary ? 0 : MO_CLI_BUILDER_ATTEMPTS / 2;
    for (uint32_t attempt = 0; attempt < random_attempts + MO_CLI_BUILDER_ATTEMPTS && !has_value;
         attempt++) {
        if (process == NULL &&
            (process = mo_process_new(builders->image, builders->limits)) == NULL) {
            result = MO_INPUT_NO_MEMORY;
            goto done;
        }
        bool ordered = attempt >= random_attempts;
        uint32_t rank = attempt - random_attempts;
        struct builder *start =
            &hidden->builders[hidden->starts[ordered ? rank % hidden->start_count
                                                     : mo_splitmix64(&random) % hidden->start_count]];
        uint64_t ordinal =
            ordered ? rank / hidden->start_count : random_ordinal(start, &random, request->size);
        enum step outcome =
            record_call(hidden, process, start, ordinal, &has_value, &value, recipe, NULL);
        if (outcome == STEP_CANCELLED || outcome == STEP_REFUSED) {
            result = outcome == STEP_REFUSED ? MO_INPUT_NO_MEMORY : MO_INPUT_LIMIT;
            goto done;
        }
        if (outcome == STEP_ENDED) {
            drop_companion(hidden, mo_process_heap(process));
            mo_process_free(process);
            process = NULL;
        }
    }
    if (!has_value)
        goto done;
    for (uint32_t i = 0; i < steps; i++) {
        struct builder *step =
            &hidden->builders[hidden->steps[boundary ? i % hidden->step_count
                                                     : mo_splitmix64(&random) % hidden->step_count]];
        uint64_t ordinal = boundary ? 1 + i : random_ordinal(step, &random, request->size);
        enum step outcome =
            record_call(hidden, process, step, ordinal, &has_value, &value, recipe, NULL);
        if (outcome == STEP_CANCELLED || outcome == STEP_REFUSED) {
            result = outcome == STEP_REFUSED ? MO_INPUT_NO_MEMORY : MO_INPUT_LIMIT;
            goto done;
        }
        if (outcome == STEP_ENDED)
            break;
    }
    result = copy_value(hidden, destination, value, remaining, out);
    if (result == MO_INPUT_OK && recipe != NULL) {
        recipe->nodes = nodes_before - remaining->nodes;
        recipe->bytes = bytes_before - remaining->bytes;
        MO_ASSERT(builders->scope != NULL);
        *builders->scope = recipe;
        builders->scope = &recipe->next;
    }
    if (result == MO_INPUT_OK)
        keep_companion(hidden, destination, recipe == NULL ? MO_NO_INDEX : recipe->id);
done:
    if (process != NULL) {
        drop_companion(hidden, mo_process_heap(process));
        if (has_value)
            mo_value_drop(mo_process_heap(process), hidden->type, value);
        mo_process_free(process);
    }
    hidden->building = false;
    return result;
}

static enum mo_input_result copy(void *context, struct mo_heap *destination, struct mo_cell source,
                                 struct mo_input_limits *remaining, struct mo_cell *out) {
    struct mo_hidden *hidden = context;
    if (hidden->start_count == 0)
        return MO_INPUT_UNSUPPORTED;
    return copy_value(hidden, destination, source, remaining, out);
}

void mo_builders_begin(struct mo_builders *builders, struct mo_heap *trial,
                       struct mo_build_trace *trace, uint64_t ordinal) {
    MO_ASSERT(builders->trial == NULL && builders->companion_count == 0);
    builders->trial = trial;
    *trace = (struct mo_build_trace){.ordinal = ordinal, .complete = true};
    trace->snapshot_limits = mo_input_limits_default();
    mo_arena_init(&trace->arena, MO_CLI_TEST_SIZE);
    builders->recording = trace;
    builders->scope = &trace->roots;
}

static void lock_companions(struct mo_build_recipe *recipes, const struct mo_build_trace *trace,
                            bool locked) {
    for (struct mo_build_recipe *recipe = recipes; recipe != NULL; recipe = recipe->next) {
        recipe->locked |= locked;
        for (uint32_t i = 0; i < trace->binding_count; i++)
            recipe->locked |= recipe->id == trace->bindings[i].origin ||
                              recipe->hidden->type == trace->bindings[i].type;
        for (uint32_t i = 0; i < recipe->count; i++)
            lock_companions(recipe->calls[i].children, trace, recipe->locked);
    }
}

void mo_builders_relate(struct mo_builders *builders, struct mo_cell *inputs, const uint32_t *types,
                        uint32_t count, bool swap) {
    struct mo_heap *heap = builders->trial;
    struct mo_build_trace *trace = builders->recording;
    if (builders->replay != NULL && count != 0) {
        const struct mo_build_trace *source = builders->replay->source;
        trace->binding_count = source->binding_count;
        memcpy(trace->bindings, source->bindings, sizeof trace->bindings);
        for (uint32_t i = 0; i < source->binding_count; i++) {
            uint32_t parameter = source->bindings[i].parameter;
            MO_ASSERT(parameter < count && types[parameter] == source->bindings[i].type);
            bool found = false;
            for (uint32_t c = 0; c < builders->companion_count; c++) {
                struct mo_companion *companion = &builders->companions[c];
                if (companion->origin != source->bindings[i].origin ||
                    companion->type != types[parameter])
                    continue;
                mo_value_drop(heap, types[parameter], inputs[parameter]);
                inputs[parameter] = companion->value;
                companion->type = UINT32_MAX;
                found = true;
                break;
            }
            builders->replay->invalid |= !found;
        }
    }
    /* The types whose inputs keep their own value, because a companion of theirs was taken. */
    uint32_t anchored[MO_LIMIT_BYTECODE_PARAMETERS];
    uint32_t anchored_count = 0;
    MO_ASSERT(count <= MO_LIMIT_BYTECODE_PARAMETERS);
    for (uint32_t i = 0; builders->replay == NULL && swap && i < count; i++) {
        bool anchor = false;
        for (uint32_t a = 0; a < anchored_count; a++)
            anchor |= anchored[a] == types[i];
        for (uint32_t c = 0; !anchor && c < builders->companion_count; c++) {
            struct mo_companion *companion = &builders->companions[c];
            if (companion->type != types[i])
                continue;
            mo_value_drop(heap, types[i], inputs[i]);
            inputs[i] = companion->value;
            anchored[anchored_count++] = companion->owner;
            if (trace->complete) {
                uint32_t binding = trace->binding_count++;
                MO_ASSERT(binding < MO_CLI_BUILDER_COMPANIONS);
                trace->bindings[binding].origin = companion->origin;
                trace->bindings[binding].parameter = i;
                trace->bindings[binding].type = types[i];
            }
            companion->type = UINT32_MAX; /* taken */
            break;
        }
    }
    if (trace->complete)
        lock_companions(trace->roots, trace, false);
    for (uint32_t c = 0; c < builders->companion_count; c++)
        if (builders->companions[c].type != UINT32_MAX)
            mo_value_drop(heap, builders->companions[c].type, builders->companions[c].value);
    builders->companion_count = 0;
    builders->trial = NULL;
    builders->recording = NULL;
    builders->scope = NULL;
}

static const struct mo_build_recipe *find_recipe(const struct mo_build_recipe *recipes,
                                                 uint32_t id) {
    for (const struct mo_build_recipe *recipe = recipes; recipe != NULL; recipe = recipe->next) {
        if (recipe->id == id)
            return recipe;
        for (uint32_t i = 0; i < recipe->count; i++) {
            const struct mo_build_recipe *found = find_recipe(recipe->calls[i].children, id);
            if (found != NULL)
                return found;
        }
    }
    return NULL;
}

void mo_builder_shrink_init(struct mo_builder_shrinker *shrinker, struct mo_builders *builders) {
    *shrinker = (struct mo_builder_shrinker){.builders = builders};
}

static void drop_arguments(const struct mo_inputs *inputs, struct mo_process **process,
                           struct mo_cell *arguments) {
    if (*process == NULL)
        return;
    for (uint32_t i = 0; i < inputs->parameter_count; i++)
        mo_value_drop(mo_process_heap(*process), inputs->parameters[i], arguments[i]);
    mo_process_free(*process);
    *process = NULL;
}

void mo_builder_shrink_free(struct mo_builder_shrinker *shrinker) {
    drop_arguments(shrinker->arguments, &shrinker->current, shrinker->current_args);
    drop_arguments(shrinker->arguments, &shrinker->proposed, shrinker->proposed_args);
    shrinker->arguments = NULL;
}

static enum mo_input_result capture_arguments(struct mo_builder_shrinker *shrinker,
                                              const struct mo_build_trace *best,
                                              const struct mo_build_recipe *source) {
    struct mo_builders *builders = shrinker->builders;
    const struct mo_build_call *call = &source->calls[shrinker->call];
    const struct mo_inputs *inputs = &call->builder->arguments;
    struct mo_process *process = mo_process_new(builders->image, builders->limits);
    if (process == NULL)
        return MO_INPUT_NO_MEMORY;
    struct mo_builder_replay replay = {.source = best,
                                       .node = source->id,
                                       .mask = UINT32_MAX,
                                       .slot = call->slot,
                                       .capture = process,
                                       .captured = shrinker->current_args,
                                       .work = inputs->limits.work};
    builders->replay = &replay;
    struct mo_input_limits limits = mo_input_limits_default();
    struct mo_cell value;
    enum mo_input_result result =
        replay_build(source->hidden, source, mo_process_heap(process), &limits, &value);
    builders->replay = NULL;
    if (result == MO_INPUT_OK)
        mo_value_drop(mo_process_heap(process), source->hidden->type, value);
    if (result == MO_INPUT_OK && replay.captured_arguments) {
        shrinker->current = process;
        shrinker->arguments = inputs;
        mo_input_shrink_init(&shrinker->argument_shrinker, inputs, MO_CLI_BUILDER_SHRINK_ATTEMPTS);
        return MO_INPUT_OK;
    }
    if (replay.captured_arguments)
        for (uint32_t i = 0; i < inputs->parameter_count; i++)
            mo_value_drop(mo_process_heap(process), inputs->parameters[i],
                          shrinker->current_args[i]);
    mo_process_free(process);
    return result == MO_INPUT_OK ? MO_INPUT_LIMIT : result;
}

static enum mo_input_result argument_candidate(struct mo_builder_shrinker *shrinker,
                                               const struct mo_build_trace *best,
                                               const struct mo_build_recipe **selected) {
    while (shrinker->node != 0) {
        const struct mo_build_recipe *source = find_recipe(best->roots, shrinker->node - 1);
        if (source == NULL || source->locked || shrinker->call == source->count) {
            shrinker->node--;
            shrinker->call = 0;
            continue;
        }
        if (!source->calls[shrinker->call].builder->generated) {
            shrinker->call++;
            continue;
        }
        if (shrinker->arguments == NULL) {
            enum mo_input_result result = capture_arguments(shrinker, best, source);
            if (result != MO_INPUT_OK)
                return result;
        }
        struct mo_process *process =
            mo_process_new(shrinker->builders->image, shrinker->builders->limits);
        if (process == NULL)
            return MO_INPUT_NO_MEMORY;
        enum mo_input_result result =
            mo_input_shrink_next(&shrinker->argument_shrinker, shrinker->current_args,
                                 mo_process_heap(process), shrinker->proposed_args);
        if (result == MO_INPUT_OK) {
            shrinker->proposed = process;
            *selected = source;
            return MO_INPUT_OK;
        }
        mo_process_free(process);
        if (result != MO_INPUT_DONE)
            return result;
        mo_builder_shrink_free(shrinker);
        shrinker->call++;
    }
    return MO_INPUT_DONE;
}

enum mo_input_result mo_builder_shrink_next(struct mo_builder_shrinker *shrinker,
                                            struct mo_inputs *inputs,
                                            const struct mo_build_trace *best,
                                            struct mo_heap *destination,
                                            struct mo_build_trace *pending, struct mo_cell *out) {
    MO_ASSERT(!shrinker->awaiting);
    if (!best->complete)
        return MO_INPUT_LIMIT;
    const struct mo_build_recipe *source = NULL;
    if (!shrinker->arguments_phase) {
        for (; shrinker->node < best->next_id; shrinker->node++, shrinker->mask = 0) {
            source = find_recipe(best->roots, shrinker->node);
            if (source != NULL && !source->locked &&
                shrinker->mask < (1u << (source->count - 1)) - 1)
                break;
        }
        if (shrinker->node == best->next_id) {
            shrinker->arguments_phase = true;
            shrinker->call = 0;
        }
    }
    if (shrinker->arguments_phase) {
        enum mo_input_result result = argument_candidate(shrinker, best, &source);
        if (result == MO_INPUT_DONE && shrinker->changed) {
            shrinker->arguments_phase = shrinker->changed = false;
            shrinker->node = shrinker->mask = 0;
            return MO_INPUT_UNINHABITED;
        }
        if (result != MO_INPUT_OK)
            return result;
    }
    if (shrinker->attempts++ == MO_CLI_BUILDER_SHRINK_ATTEMPTS)
        return MO_INPUT_LIMIT;
    struct mo_builders *builders = shrinker->builders;
    struct mo_builder_replay replay = {
        .source = best,
        .scope = best->roots,
        .node = source->id,
        .mask = shrinker->arguments_phase ? UINT32_MAX : shrinker->mask,
        .slot = shrinker->arguments_phase ? source->calls[shrinker->call].slot : 0,
        .replacement = shrinker->arguments_phase ? shrinker->proposed_args : NULL,
        .work = inputs->limits.work};
    mo_builders_begin(builders, destination, pending, best->ordinal);
    builders->replay = &replay;
    mo_inputs_seek(inputs, best->ordinal);
    struct mo_cell made[MO_LIMIT_BYTECODE_PARAMETERS];
    enum mo_input_kind kind;
    enum mo_input_result result = mo_inputs_next(inputs, destination, made, &kind);
    if (result == MO_INPUT_OK)
        MO_ASSERT(replay.scope == NULL);
    mo_builders_relate(builders, made, inputs->parameters,
                       result == MO_INPUT_OK ? inputs->parameter_count : 0, false);
    builders->replay = NULL;
    pending->next_id = best->next_id;
    if (result == MO_INPUT_OK && (replay.invalid || !pending->complete)) {
        for (uint32_t i = 0; i < inputs->parameter_count; i++)
            mo_value_drop(destination, inputs->parameters[i], made[i]);
        result = pending->complete ? MO_INPUT_UNINHABITED : MO_INPUT_LIMIT;
    } else if (result == MO_INPUT_DONE && replay.invalid)
        result = MO_INPUT_UNINHABITED;
    if (result == MO_INPUT_OK) {
        memcpy(out, made, inputs->parameter_count * sizeof *out);
        shrinker->awaiting = true;
    } else {
        mo_build_trace_free(pending);
        shrinker->awaiting = true;
        mo_builder_shrink_feedback(shrinker, false);
    }
    return result;
}

void mo_builder_shrink_feedback(struct mo_builder_shrinker *shrinker, bool still_fails) {
    MO_ASSERT(shrinker->awaiting);
    shrinker->awaiting = false;
    if (shrinker->arguments_phase) {
        mo_input_shrink_feedback(&shrinker->argument_shrinker, still_fails);
        if (still_fails) {
            drop_arguments(shrinker->arguments, &shrinker->current, shrinker->current_args);
            shrinker->current = shrinker->proposed;
            memcpy(shrinker->current_args, shrinker->proposed_args, sizeof shrinker->current_args);
            shrinker->proposed = NULL;
            shrinker->changed = true;
        } else
            drop_arguments(shrinker->arguments, &shrinker->proposed, shrinker->proposed_args);
        return;
    }
    if (still_fails)
        shrinker->node = shrinker->mask = 0;
    else
        shrinker->mask++;
}
