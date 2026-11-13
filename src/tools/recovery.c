#include "tools/recovery.h"

#include "base/hash.h"
#include "tools/inputs-internal.h"

#include <string.h>

/* Every sequence's clock starts at 2026-01-01T00:00:00Z. */
static const int64_t recovery_epoch = INT64_C(1767225600000000);
/* This key is public and fixed, like the input generator's. */
static const uint64_t recovery_key[2] = {UINT64_C(0x6d6f2d7265636f76),
                                         UINT64_C(0x6572792d63686563)};
/* Past the boundary inputs, a tool's arguments are one of the generator's first random inputs, so
 * collections stay at most 64 entries (the generator's size grows every four inputs). */
enum { RANDOM_INPUTS = 256 };
/* Tries for one tool's arguments when an input exceeds the generator's limits. */
enum { ARGUMENT_TRIES = 16 };
enum { SECOND_US = 1000000 };

struct pool_entry {
    uint32_t type;
    uint32_t group; /* the reply it came from; a group's entries are adjacent, in order */
    struct mo_cell value;
};

struct mo_recovery {
    struct mo_arena arena;
    const struct mo_image *image;
    const struct mo_bytecode_service *service;
    const struct mo_types *types;
    uint32_t index;
    struct mo_process *process;
    struct mo_heap *heap;
    char *name;
    uint64_t seed;
    struct mo_inputs *inputs; /* one per tool */
    uint64_t *boundary;       /* per tool: the next boundary input */
    uint32_t *sendable;       /* tools whose arguments can be generated */
    uint32_t sendable_count;
    int64_t *intervals; /* the timers' intervals, in microseconds */
    uint32_t interval_count;
    /* Values from earlier replies in this sequence, oldest first. */
    struct pool_entry pool[MO_CLI_RECOVERY_POOL];
    uint32_t pool_count;
    uint32_t group;
    bool compare; /* restarts are compared */
};

void mo_recovery_sequence_init(struct mo_recovery_sequence *sequence) {
    sequence->count = 0;
    /* Every step's arguments at once, with room for the arena's growing chunks. */
    mo_arena_init(&sequence->arena, 2 * (size_t)MO_CLI_RECOVERY_STEPS *
                                        (MO_LIMIT_BYTECODE_PARAMETERS * sizeof(struct mo_cell) +
                                         MO_ARENA_CHUNK_HEADER + MO_ARENA_ALIGNMENT));
}

/* Drops the steps from count on. */
static void cut(struct mo_recovery *r, struct mo_recovery_sequence *sequence, uint32_t count) {
    for (uint32_t i = count; i < sequence->count; i++) {
        const struct mo_recovery_step *step = &sequence->steps[i];
        if (step->kind != MO_RECOVERY_SEND)
            continue;
        const struct mo_bytecode_service_tool *tool = &r->service->tools[step->tool];
        for (uint32_t a = 0; a < tool->argument_count; a++)
            mo_value_drop(r->heap, tool->argument_types[a], step->arguments[a]);
    }
    sequence->count = count;
}

void mo_recovery_sequence_clear(struct mo_recovery *r, struct mo_recovery_sequence *sequence) {
    cut(r, sequence, 0);
    mo_arena_free(&sequence->arena);
}

static void result_init(struct mo_recovery_result *out) {
    *out = (struct mo_recovery_result){
        .outcome = MO_RECOVERY_PASSED, .step = MO_NO_INDEX, .verdict.check = MO_NO_INDEX};
    mo_recovery_sequence_init(&out->sequence);
    mo_buffer_init(&out->before, MO_LIMIT_SAVED_BYTES);
    mo_buffer_init(&out->after, MO_LIMIT_SAVED_BYTES);
}

void mo_recovery_result_free(struct mo_recovery *r, struct mo_recovery_result *result) {
    mo_recovery_sequence_clear(r, &result->sequence);
    mo_buffer_free(&result->after);
    mo_buffer_free(&result->before);
}

static void pool_clear(struct mo_recovery *r) {
    for (uint32_t i = 0; i < r->pool_count; i++)
        mo_value_drop(r->heap, r->pool[i].type, r->pool[i].value);
    r->pool_count = 0;
}

static enum mo_input_result intervals(struct mo_recovery *r, const struct mo_limits *limits) {
    const struct mo_bytecode_service *service = r->service;
    r->intervals = mo_arena_allocate_array(&r->arena, service->timer_count, sizeof *r->intervals,
                                           _Alignof(int64_t));
    if (r->intervals == NULL)
        return MO_INPUT_NO_MEMORY;
    for (uint32_t i = 0; i < service->timer_count; i++) {
        /* A scratch process: an interval that fails ends it, and the service then fails to open. */
        struct mo_process *scratch = mo_process_new(r->image, limits);
        if (scratch == NULL)
            return MO_INPUT_NO_MEMORY;
        struct mo_cell every;
        struct mo_defect defect;
        if (mo_call(scratch, service->timers[i].interval, NULL, 0, &every, &defect) ==
                MO_RETURNED &&
            mo_cell_get_duration(every) > 0)
            r->intervals[r->interval_count++] = mo_cell_get_duration(every);
        mo_process_free(scratch);
    }
    return MO_INPUT_OK;
}

enum mo_input_result mo_recovery_new(const struct mo_image *image, uint32_t index,
                                     const struct mo_limits *limits, struct mo_recovery **out) {
    const struct mo_bytecode *file = mo_image_bytecode(image);
    MO_ASSERT(index < file->service_count && limits != NULL && out != NULL);
    const struct mo_bytecode_service *service = &file->services[index];
    struct mo_arena arena;
    mo_arena_init(&arena, MO_CLI_TEST_SIZE);
    struct mo_recovery *r = mo_arena_allocate(&arena, sizeof *r, _Alignof(struct mo_recovery));
    if (r == NULL) {
        mo_arena_free(&arena);
        return MO_INPUT_NO_MEMORY;
    }
    *r = (struct mo_recovery){.arena = arena,
                              .image = image,
                              .service = service,
                              .types = mo_image_types(image),
                              .index = index,
                              .compare = true};
    static const char suffix[] = MO_RECOVERY_SUFFIX;
    const struct mo_bytecode_string *module = &file->strings[file->modules[service->module].name];
    size_t length = module->length + sizeof suffix - 1;
    r->name = mo_arena_allocate(&r->arena, length + 1, 1);
    r->inputs = mo_arena_allocate_array(&r->arena, service->tool_count, sizeof *r->inputs,
                                        _Alignof(struct mo_inputs));
    r->boundary = mo_arena_allocate_array(&r->arena, service->tool_count, sizeof *r->boundary,
                                          _Alignof(uint64_t));
    r->sendable = mo_arena_allocate_array(&r->arena, service->tool_count, sizeof *r->sendable,
                                          _Alignof(uint32_t));
    r->process = mo_process_new(image, limits);
    if (r->name == NULL || r->inputs == NULL || r->boundary == NULL || r->sendable == NULL ||
        r->process == NULL) {
        mo_process_free(r->process);
        arena = r->arena;
        mo_arena_free(&arena);
        return MO_INPUT_NO_MEMORY;
    }
    r->heap = mo_process_heap(r->process);
    memcpy(r->name, module->bytes, module->length);
    memcpy(r->name + module->length, suffix, sizeof suffix);
    r->seed = mo_hash(recovery_key, r->name, length);
    struct mo_input_limits input_limits = mo_input_limits_default();
    /* Well inside the saved depth, however each counts a level, so copying in never refuses. */
    if (input_limits.depth > MO_LIMIT_SAVED_DEPTH / 2)
        input_limits.depth = MO_LIMIT_SAVED_DEPTH / 2;
    enum mo_input_result result = MO_INPUT_OK;
    uint32_t prepared = 0;
    for (; prepared < service->tool_count; prepared++) {
        const struct mo_bytecode_service_tool *tool = &service->tools[prepared];
        result = mo_inputs_init(&r->inputs[prepared], r->types, tool->argument_types,
                                tool->argument_count, r->name, length, &input_limits, NULL, 0);
        r->boundary[prepared] = 0;
        if (result == MO_INPUT_OK)
            r->sendable[r->sendable_count++] = prepared;
        else if (result != MO_INPUT_UNINHABITED) {
            prepared++;
            break;
        }
        result = MO_INPUT_OK;
    }
    if (result == MO_INPUT_OK)
        result = intervals(r, limits);
    if (result != MO_INPUT_OK) {
        for (uint32_t i = 0; i < prepared; i++)
            mo_inputs_free(&r->inputs[i]);
        mo_process_free(r->process);
        arena = r->arena;
        mo_arena_free(&arena);
        return result;
    }
    *out = r;
    return MO_INPUT_OK;
}

void mo_recovery_free(struct mo_recovery *r) {
    if (r == NULL)
        return;
    pool_clear(r);
    /* Every sequence and result was cleared, so nothing is left in the heap. */
    MO_ASSERT(mo_heap_objects(r->heap) == 0);
    for (uint32_t i = 0; i < r->service->tool_count; i++)
        mo_inputs_free(&r->inputs[i]);
    mo_process_free(r->process);
    struct mo_arena arena = r->arena;
    mo_arena_free(&arena);
}

const char *mo_recovery_name(const struct mo_recovery *r) {
    return r->name;
}

struct mo_heap *mo_recovery_heap(struct mo_recovery *r) {
    return r->heap;
}

void mo_recovery_compare_restarts(struct mo_recovery *r, bool compare) {
    r->compare = compare;
}

enum mo_recovery_compared mo_recovery_compare(struct mo_simulation *simulation,
                                              const struct mo_buffer *before,
                                              struct mo_buffer *after) {
    mo_buffer_clear(after);
    if (!mo_simulation_encode(simulation, after))
        return MO_RECOVERY_UNREADABLE;
    return after->length == before->length &&
                   (after->length == 0 || memcmp(after->bytes, before->bytes, after->length) == 0)
               ? MO_RECOVERY_EQUAL
               : MO_RECOVERY_DIFFERENT;
}

/* Reply values: the scalar and Text leaves of a reply, in order, without its new state. */

struct walk_frame {
    uint32_t type, index, count, position, tag;
    struct mo_cell value, pending;
};

static void walk_push(const struct mo_types *types, struct walk_frame *frame, uint32_t type,
                      struct mo_cell value) {
    const struct mo_type *t = &types->items[type];
    *frame = (struct walk_frame){.type = type, .value = value};
    switch (t->kind) {
    case MO_TYPE_TUPLE:
        frame->count = t->parameter_count;
        break;
    case MO_TYPE_RECORD:
        frame->count = t->field_count;
        break;
    case MO_TYPE_CHOICE:
        frame->tag = mo_cell_get_tag(value);
        MO_ASSERT(frame->tag < t->case_count);
        frame->count = t->cases[frame->tag].field_count;
        break;
    case MO_TYPE_LIST:
        frame->count = mo_list_size(value);
        break;
    case MO_TYPE_SET:
        frame->count = mo_table_size(value);
        break;
    case MO_TYPE_MAP:
        frame->count = 2 * mo_table_size(value);
        break;
    case MO_TYPE_UNIT:
    case MO_TYPE_BOOL:
    case MO_TYPE_BYTE:
    case MO_TYPE_INT:
    case MO_TYPE_FLOAT:
    case MO_TYPE_CHAR:
    case MO_TYPE_TIME:
    case MO_TYPE_DURATION:
    case MO_TYPE_DECIMAL:
    case MO_TYPE_RANGE:
    case MO_TYPE_TEXT:
    case MO_TYPE_BYTES:
    case MO_TYPE_BIGINT:
    case MO_TYPE_FUNCTION:
    case MO_TYPE_ENV:
    case MO_TYPE_CAPABILITY:
        MO_UNREACHABLE();
    }
}

static struct mo_cell walk_child(const struct mo_types *types, struct walk_frame *frame,
                                 uint32_t *type) {
    const struct mo_type *t = &types->items[frame->type];
    uint32_t index = frame->index++;
    struct mo_cell child = {0}, unused;
    switch (t->kind) {
    case MO_TYPE_TUPLE:
        *type = t->parameters[index];
        return mo_value_field(frame->value, index);
    case MO_TYPE_RECORD:
        *type = t->fields[index].type;
        return mo_value_field(frame->value, index);
    case MO_TYPE_CHOICE: {
        *type = t->cases[frame->tag].fields[index].type;
        struct mo_cell payload = mo_cell_get_payload(frame->value);
        return mo_case_shape(types, frame->type, frame->tag) == MO_CASE_BOXED
                   ? mo_value_field(payload, index)
                   : payload;
    }
    case MO_TYPE_LIST:
        *type = t->parameters[0];
        MO_ASSERT(mo_list_get(frame->value, index, &child));
        return child;
    case MO_TYPE_SET:
        *type = t->parameters[0];
        MO_ASSERT(mo_table_next(frame->value, &frame->position, &child, &unused));
        return child;
    case MO_TYPE_MAP:
        if (index % 2 != 0) {
            *type = t->parameters[1];
            return frame->pending;
        }
        *type = t->parameters[0];
        MO_ASSERT(mo_table_next(frame->value, &frame->position, &child, &frame->pending));
        return child;
    case MO_TYPE_UNIT:
    case MO_TYPE_BOOL:
    case MO_TYPE_BYTE:
    case MO_TYPE_INT:
    case MO_TYPE_FLOAT:
    case MO_TYPE_CHAR:
    case MO_TYPE_TIME:
    case MO_TYPE_DURATION:
    case MO_TYPE_DECIMAL:
    case MO_TYPE_RANGE:
    case MO_TYPE_TEXT:
    case MO_TYPE_BYTES:
    case MO_TYPE_BIGINT:
    case MO_TYPE_FUNCTION:
    case MO_TYPE_ENV:
    case MO_TYPE_CAPABILITY:
        break;
    }
    MO_UNREACHABLE();
}

static bool leaf_kind(enum mo_type_kind kind) {
    switch (kind) {
    case MO_TYPE_BOOL:
    case MO_TYPE_BYTE:
    case MO_TYPE_INT:
    case MO_TYPE_FLOAT:
    case MO_TYPE_CHAR:
    case MO_TYPE_TIME:
    case MO_TYPE_DURATION:
    case MO_TYPE_DECIMAL:
    case MO_TYPE_RANGE:
    case MO_TYPE_TEXT:
    case MO_TYPE_BYTES:
    case MO_TYPE_BIGINT:
        return true;
    case MO_TYPE_UNIT:
    case MO_TYPE_TUPLE:
    case MO_TYPE_LIST:
    case MO_TYPE_SET:
    case MO_TYPE_MAP:
    case MO_TYPE_RECORD:
    case MO_TYPE_CHOICE:
    case MO_TYPE_FUNCTION:
    case MO_TYPE_ENV:
    case MO_TYPE_CAPABILITY:
        return false;
    }
    MO_UNREACHABLE();
}

/* The first leaves of value, borrowed, at most room of them. The reply passed the saved codec, so
 * it is no deeper than MO_LIMIT_SAVED_DEPTH. */
static uint32_t leaves(const struct mo_types *types, uint32_t type, struct mo_cell value,
                       struct pool_entry *out, uint32_t room) {
    struct walk_frame stack[MO_LIMIT_SAVED_DEPTH + 1];
    uint32_t depth = 0, found = 0;
    for (;;) {
        enum mo_type_kind kind = types->items[type].kind;
        if (leaf_kind(kind))
            out[found++] = (struct pool_entry){.type = type, .value = value};
        else if (kind != MO_TYPE_UNIT) {
            MO_ASSERT(depth < sizeof stack / sizeof stack[0]);
            walk_push(types, &stack[depth++], type, value);
        }
        while (depth != 0 && stack[depth - 1].index == stack[depth - 1].count)
            depth--;
        if (depth == 0 || found == room)
            return found;
        value = walk_child(types, &stack[depth - 1], &type);
    }
}

struct mo_cell mo_recovery_reply(const struct mo_recovery *r, uint32_t tool,
                                 enum mo_simulation_sent sent,
                                 const struct mo_simulation_reply *reply, uint32_t *type) {
    MO_ASSERT(tool < r->service->tool_count &&
              (sent == MO_SENT_REPLIED || sent == MO_SENT_REJECTED));
    *type = reply->type;
    struct mo_cell value = reply->reply;
    if (sent == MO_SENT_REPLIED && !r->service->tools[tool].read_only) {
        /* An update's result holds the new state then the reply, maybe inside Ok. */
        const struct mo_type *t = &r->types->items[*type];
        if (t->kind == MO_TYPE_CHOICE) {
            MO_ASSERT(mo_cell_get_tag(value) == 0);
            value = mo_cell_get_payload(value);
            *type = t->cases[0].fields[0].type;
            t = &r->types->items[*type];
        }
        MO_ASSERT(t->kind == MO_TYPE_TUPLE && t->parameter_count == 2);
        value = mo_value_field(value, 1);
        *type = t->parameters[1];
    }
    return value;
}

/* Keeps a reply's values for later arguments, as one group, evicting the oldest groups' values. */
static void harvest(struct mo_recovery *r, uint32_t tool, enum mo_simulation_sent sent,
                    const struct mo_simulation_reply *reply) {
    uint32_t type;
    struct mo_cell value = mo_recovery_reply(r, tool, sent, reply, &type);
    struct pool_entry found[MO_CLI_RECOVERY_POOL];
    uint32_t count = leaves(r->types, type, value, found, MO_CLI_RECOVERY_POOL);
    if (count == 0)
        return;
    uint32_t evict = r->pool_count + count > MO_CLI_RECOVERY_POOL
                         ? r->pool_count + count - MO_CLI_RECOVERY_POOL
                         : 0;
    for (uint32_t i = 0; i < evict; i++)
        mo_value_drop(r->heap, r->pool[i].type, r->pool[i].value);
    memmove(r->pool, r->pool + evict, (r->pool_count - evict) * sizeof r->pool[0]);
    r->pool_count -= evict;
    for (uint32_t i = 0; i < count; i++)
        r->pool[r->pool_count++] = (struct pool_entry){
            found[i].type, r->group, mo_value_dup(r->heap, found[i].type, found[i].value)};
    r->group++;
}

/* How many of tool's arguments group can supply, taking its values of each type in order. With
 * apply, replaces those arguments with copies of the values. */
static uint32_t supply(struct mo_recovery *r, const struct mo_bytecode_service_tool *tool,
                       uint32_t group, struct mo_cell *arguments, bool apply) {
    uint32_t first = 0;
    while (first < r->pool_count && r->pool[first].group != group)
        first++;
    uint32_t end = first;
    while (end < r->pool_count && r->pool[end].group == group)
        end++;
    bool used[MO_CLI_RECOVERY_POOL] = {false};
    uint32_t supplied = 0;
    for (uint32_t a = 0; a < tool->argument_count; a++) {
        uint32_t type = tool->argument_types[a];
        for (uint32_t i = first; i < end; i++) {
            if (used[i] || r->pool[i].type != type)
                continue;
            used[i] = true;
            supplied++;
            if (apply) {
                mo_value_drop(r->heap, type, arguments[a]);
                arguments[a] = mo_value_dup(r->heap, type, r->pool[i].value);
            }
            break;
        }
    }
    return supplied;
}

/* Often takes arguments from one earlier reply: the newest of those that supply the most, or any
 * of them, so values that came together, such as a job's id and attempt, arrive together. */
static void reuse(struct mo_recovery *r, const struct mo_bytecode_service_tool *tool,
                  uint64_t *random, struct mo_cell *arguments) {
    if (r->pool_count == 0 || mo_input_random(random) % 4 == 0)
        return;
    uint32_t best = 0, candidates[MO_CLI_RECOVERY_POOL], count = 0;
    for (uint32_t end = r->pool_count; end != 0;) {
        uint32_t group = r->pool[end - 1].group;
        while (end != 0 && r->pool[end - 1].group == group)
            end--;
        uint32_t supplied = supply(r, tool, group, arguments, false);
        if (supplied > best) {
            best = supplied;
            count = 0;
        }
        if (supplied == best && supplied != 0)
            candidates[count++] = group;
    }
    if (count == 0)
        return;
    uint64_t pick = mo_input_random(random);
    uint32_t group = pick % 2 == 0 ? candidates[0] : candidates[(pick / 2) % count];
    (void)supply(r, tool, group, arguments, true);
}

static enum mo_input_result generate_arguments(struct mo_recovery *r, uint32_t tool,
                                               uint64_t *random, struct mo_cell *arguments) {
    struct mo_inputs *inputs = &r->inputs[tool];
    enum mo_input_result result = MO_INPUT_LIMIT;
    for (uint32_t attempt = 0; attempt < ARGUMENT_TRIES && result == MO_INPUT_LIMIT; attempt++) {
        if (r->boundary[tool] < mo_inputs_boundaries(inputs))
            mo_inputs_seek(inputs, r->boundary[tool]++);
        else
            mo_inputs_seek(inputs,
                           mo_inputs_boundaries(inputs) + mo_input_random(random) % RANDOM_INPUTS);
        enum mo_input_kind kind;
        result = mo_inputs_next(inputs, r->heap, arguments, &kind);
    }
    return result;
}

static int64_t times(int64_t every, uint64_t multiple) {
    return (uint64_t)every > (uint64_t)INT64_MAX / multiple ? INT64_MAX : every * (int64_t)multiple;
}

/* A timer's interval or a neighbour, a duration crossing whole multiples of it (some past the
 * next snapshot), or a random duration. Halved until the firings fit MO_LIMIT_SIMULATION_TICKS and
 * the clock stays inside year 9999. */
static int64_t advance_by(const struct mo_recovery *r, const struct mo_simulation *simulation,
                          uint64_t *random) {
    int64_t by;
    if (r->interval_count != 0 && mo_input_random(random) % 4 != 0) {
        int64_t every = r->intervals[mo_input_random(random) % r->interval_count];
        uint64_t choice = mo_input_random(random) % 6;
        if (choice == 0)
            by = every;
        else if (choice == 1)
            by = every - 1;
        else if (choice == 2)
            by = every == INT64_MAX ? every : every + 1;
        else if (choice == 3)
            by = times(every, 2 + mo_input_random(random) % 3);
        else if (choice == 4)
            by = times(every,
                       MO_LIMIT_SERVICE_SNAPSHOT_MESSAGES +
                           mo_input_random(random) % (MO_LIMIT_SERVICE_SNAPSHOT_MESSAGES + 1));
        else
            by = (int64_t)(mo_input_random(random) % ((uint64_t)times(every, 2) + 1));
    } else {
        static const int64_t spans[] = {SECOND_US, INT64_C(3600) * SECOND_US,
                                        INT64_C(86400) * SECOND_US};
        int64_t span = spans[mo_input_random(random) % (sizeof spans / sizeof spans[0])];
        by = 1 + (int64_t)(mo_input_random(random) % (uint64_t)span);
    }
    uint64_t firings;
    while (by > 0 && (!mo_simulation_firings(simulation, by, &firings) ||
                      firings > MO_LIMIT_SIMULATION_TICKS))
        by /= 2;
    return by;
}

/* One sequence on its own store. */
struct trial {
    struct mo_recovery *recovery;
    struct mo_simulation *simulation;
    struct mo_simulation_hooks hooks;
    const struct mo_recovery_hooks *observer;
    struct mo_recovery_result *out;
    uint64_t ordinal;
    bool harvest;
    bool failed; /* failure holds the newest failure the service handed back */
    struct mo_recovery_failure failure;
    uint32_t failed_firings;  /* timer handlers that failed during the current step */
    uint64_t committed, lost; /* this store's commits, and timer firings that committed nothing */
    bool ended; /* a refused restart ended the sequence, as restarts are not compared */
    /* For the check hook: the state before and after the current step, when encoded. */
    struct mo_buffer pre, post;
    bool have_pre;
};

static void copy_text(char *out, size_t size, const char *text, size_t length) {
    if (length >= size)
        length = size - 1;
    memcpy(out, text, length);
    out[length] = '\0';
}

static bool failed(const struct mo_simulation_failure *f, void *context) {
    struct trial *t = context;
    const struct mo_bytecode_service *service = t->recovery->service;
    t->failed = true;
    t->failure = (struct mo_recovery_failure){
        .status = f->status, .now = f->now, .function = f->function, .span.file = MO_NO_INDEX};
    if (f->defect != NULL)
        t->failure.span = f->defect->span;
    if (f->defect != NULL && f->defect->reason != NULL)
        copy_text(t->failure.reason, sizeof t->failure.reason, f->defect->reason,
                  strlen(f->defect->reason));
    if (f->defect != NULL && f->defect->has_detail) {
        size_t length = 0;
        const uint8_t *detail = mo_value_data(f->defect->detail, &length);
        if (length > sizeof t->failure.detail)
            length = sizeof t->failure.detail;
        memcpy(t->failure.detail, detail, length);
        t->failure.detail_length = length;
    }
    for (uint32_t i = 0; i < service->timer_count; i++)
        if (f->function == service->timers[i].function) {
            t->lost++;
            t->failed_firings++;
        }
    /* Every failure is an ordinary step of the sequence, or ends it as an outcome. */
    return true;
}

static uint64_t commits(const struct trial *t) {
    return t->committed > t->lost ? t->committed - t->lost : 0;
}

/* Opens the service on a fresh store and runs its startup ticks. */
static enum mo_recovery_outcome start(struct trial *t) {
    struct mo_recovery *r = t->recovery;
    mo_buffer_init(&t->pre, MO_LIMIT_SAVED_BYTES);
    mo_buffer_init(&t->post, MO_LIMIT_SAVED_BYTES);
    if (mo_simulation_new(r->image, r->index, recovery_epoch, &t->simulation) != MO_STORE_OK) {
        t->simulation = NULL;
        t->out->problem = "an in-memory store could not be opened";
        return MO_RECOVERY_UNAVAILABLE;
    }
    t->hooks = (struct mo_simulation_hooks){failed, t};
    t->failed = false;
    if (mo_simulation_open(t->simulation, &t->hooks) == MO_SIMULATION_DOWN) {
        MO_ASSERT(t->failed);
        t->out->failure = t->failure;
        return MO_RECOVERY_STOPPED;
    }
    MO_ASSERT(mo_simulation_drain(t->simulation, &t->hooks) == MO_SIMULATION_OK);
    t->committed += r->service->timer_count;
    return MO_RECOVERY_PASSED;
}

static void finish(struct trial *t) {
    mo_buffer_free(&t->post);
    mo_buffer_free(&t->pre);
    mo_simulation_free(t->simulation);
    t->simulation = NULL;
    pool_clear(t->recovery);
}

static void swap(struct mo_buffer *a, struct mo_buffer *b) {
    struct mo_buffer held = *a;
    *a = *b;
    *b = held;
}

/* Runs the check hook after step, which ran without failing. */
static enum mo_recovery_outcome check(struct trial *t, const struct mo_recovery_step *step,
                                      uint32_t index, const struct mo_simulation_reply *reply) {
    struct mo_recovery_result *out = t->out;
    bool restart = step->kind == MO_RECOVERY_RESTART;
    mo_buffer_clear(&t->post);
    bool after = mo_simulation_encode(t->simulation, &t->post);
    const struct mo_recovery_observation observation = {t->ordinal,
                                                        index,
                                                        step,
                                                        reply,
                                                        t->simulation,
                                                        restart       ? &out->before
                                                        : t->have_pre ? &t->pre
                                                                      : NULL,
                                                        after ? &t->post : NULL};
    struct mo_recovery_verdict verdict = t->observer->check(&observation, t->observer->context);
    if (verdict.check != MO_NO_INDEX) {
        out->verdict = verdict;
        if (!restart)
            swap(&out->before, &t->pre);
        swap(&out->after, &t->post);
        return MO_RECOVERY_CHECK;
    }
    swap(&t->pre, &t->post);
    t->have_pre = after;
    return MO_RECOVERY_PASSED;
}

/* Runs one step; PASSED means the sequence goes on. */
static enum mo_recovery_outcome run_step(struct trial *t, struct mo_recovery_step *step,
                                         uint32_t index) {
    struct mo_recovery *r = t->recovery;
    struct mo_recovery_result *out = t->out;
    struct mo_simulation_reply reply = {0};
    enum mo_recovery_outcome outcome = MO_RECOVERY_PASSED;
    bool replied = false, restarted = false;
    bool checking = t->observer != NULL && t->observer->check != NULL;
    if (checking && !t->have_pre && step->kind != MO_RECOVERY_CRASH &&
        step->kind != MO_RECOVERY_RESTART) {
        mo_buffer_clear(&t->pre);
        t->have_pre = mo_simulation_encode(t->simulation, &t->pre);
    }
    t->failed_firings = 0;
    switch (step->kind) {
    case MO_RECOVERY_SEND: {
        bool running = mo_simulation_running(t->simulation);
        uint64_t restarts = mo_simulation_restarts(t->simulation);
        step->sent = mo_simulation_send(t->simulation, &t->hooks, r->heap, step->tool,
                                        step->arguments, &reply);
        MO_ASSERT(step->sent != MO_SENT_STOPPED);
        step->status = reply.status;
        step->updated = reply.updated;
        step->supervisor = running && !mo_simulation_running(t->simulation) ? MO_SUPERVISOR_STOPPED
                           : mo_simulation_restarts(t->simulation) != restarts
                               ? MO_SUPERVISOR_RESTARTED
                               : MO_SUPERVISOR_NONE;
        t->committed += reply.updated;
        replied = step->sent == MO_SENT_REPLIED || step->sent == MO_SENT_REJECTED;
        break;
    }
    case MO_RECOVERY_ADVANCE:
        step->outcome =
            mo_simulation_advance(t->simulation, &t->hooks, step->duration, &step->firings);
        MO_ASSERT(step->outcome != MO_SIMULATION_STOPPED);
        if (step->outcome == MO_SIMULATION_OK)
            t->committed += step->firings;
        step->failed_firings = t->failed_firings;
        break;
    case MO_RECOVERY_CRASH:
        mo_buffer_clear(&out->before);
        if (!mo_simulation_encode(t->simulation, &out->before)) {
            out->problem = "the state before a crash could not be encoded";
            outcome = MO_RECOVERY_UNAVAILABLE;
        }
        mo_simulation_crash(t->simulation);
        t->have_pre = false;
        break;
    case MO_RECOVERY_RESTART:
        t->failed = false;
        step->outcome = mo_simulation_open(t->simulation, &t->hooks);
        MO_ASSERT(step->outcome == MO_SIMULATION_OK || step->outcome == MO_SIMULATION_DOWN);
        if (step->outcome == MO_SIMULATION_DOWN) {
            MO_ASSERT(t->failed);
            out->failure = t->failure;
            if (r->compare)
                outcome = MO_RECOVERY_REFUSED;
            else
                t->ended = true;
            break;
        }
        if (!r->compare) {
            restarted = true;
            break;
        }
        switch (mo_recovery_compare(t->simulation, &out->before, &out->after)) {
        case MO_RECOVERY_EQUAL:
            restarted = true;
            out->counts.restarts++;
            out->counts.replayed += commits(t) % MO_LIMIT_SERVICE_SNAPSHOT_MESSAGES;
            out->counts.snapshots += commits(t) >= MO_LIMIT_SERVICE_SNAPSHOT_MESSAGES;
            break;
        case MO_RECOVERY_DIFFERENT:
            outcome = MO_RECOVERY_MISMATCH;
            break;
        case MO_RECOVERY_UNREADABLE:
            out->problem = "the recovered state could not be encoded";
            outcome = MO_RECOVERY_UNAVAILABLE;
            break;
        }
        break;
    }
    step->ran = true;
    if (t->observer != NULL && t->observer->observe != NULL) {
        const struct mo_recovery_observation observation = {
            t->ordinal, index, step, replied ? &reply : NULL, t->simulation, NULL, NULL};
        t->observer->observe(&observation, t->observer->context);
    }
    /* The startup ticks run only after the recovered state was compared. */
    if (restarted) {
        MO_ASSERT(mo_simulation_drain(t->simulation, &t->hooks) == MO_SIMULATION_OK);
        t->committed += r->service->timer_count;
    }
    if (checking && outcome == MO_RECOVERY_PASSED && !t->ended && step->kind != MO_RECOVERY_CRASH)
        outcome = check(t, step, index, replied ? &reply : NULL);
    if (replied) {
        if (t->harvest)
            harvest(r, step->tool, step->sent, &reply);
        mo_value_drop(r->heap, reply.type, reply.reply);
    }
    if (outcome == MO_RECOVERY_MISMATCH || outcome == MO_RECOVERY_REFUSED ||
        outcome == MO_RECOVERY_CHECK)
        out->step = index;
    return outcome;
}

static struct mo_recovery_step *add_step(struct mo_recovery_sequence *sequence,
                                         enum mo_recovery_step_kind kind) {
    MO_ASSERT(sequence->count < MO_CLI_RECOVERY_STEPS);
    struct mo_recovery_step *step = &sequence->steps[sequence->count++];
    *step = (struct mo_recovery_step){.kind = kind};
    return step;
}

/* Generates and runs one sequence into out->sequence. */
static enum mo_recovery_outcome generate(struct trial *t) {
    struct mo_recovery *r = t->recovery;
    struct mo_recovery_sequence *sequence = &t->out->sequence;
    uint8_t bytes[16];
    for (unsigned i = 0; i < 8; i++) {
        bytes[i] = (uint8_t)(r->seed >> (8 * i));
        bytes[8 + i] = (uint8_t)(t->ordinal >> (8 * i));
    }
    uint64_t random = mo_hash(recovery_key, bytes, sizeof bytes);
    /* A quarter are short; the rest run up to the step limit. */
    uint32_t length = mo_input_random(&random) % 4 == 0
                          ? 2 + (uint32_t)(mo_input_random(&random) % 7)
                          : 9 + (uint32_t)(mo_input_random(&random) % (MO_CLI_RECOVERY_STEPS - 8));
    uint32_t restarts = 0;
    for (;;) {
        /* A crash and its restart end every sequence, and come next whenever the supervisor
         * stopped the service. */
        bool end = length - sequence->count <= 3;
        if (end || !mo_simulation_running(t->simulation) || mo_input_random(&random) % 10 == 0) {
            enum mo_recovery_outcome outcome =
                run_step(t, add_step(sequence, MO_RECOVERY_CRASH), sequence->count - 1);
            if (outcome == MO_RECOVERY_PASSED)
                outcome = run_step(t, add_step(sequence, MO_RECOVERY_RESTART), sequence->count - 1);
            if (outcome != MO_RECOVERY_PASSED)
                return outcome;
            if (end || t->ended || ++restarts == MO_CLI_RECOVERY_RESTARTS)
                return MO_RECOVERY_PASSED;
            continue;
        }
        struct mo_recovery_step *step;
        if (r->sendable_count == 0 || mo_input_random(&random) % 6 == 0) {
            step = add_step(sequence, MO_RECOVERY_ADVANCE);
            step->duration = advance_by(r, t->simulation, &random);
        } else {
            step = add_step(sequence, MO_RECOVERY_SEND);
            step->tool = r->sendable[mo_input_random(&random) % r->sendable_count];
            const struct mo_bytecode_service_tool *tool = &r->service->tools[step->tool];
            if (tool->argument_count != 0) {
                step->arguments =
                    mo_arena_allocate_array(&sequence->arena, tool->argument_count,
                                            sizeof *step->arguments, _Alignof(struct mo_cell));
                if (step->arguments == NULL ||
                    generate_arguments(r, step->tool, &random, step->arguments) != MO_INPUT_OK) {
                    sequence->count--;
                    t->out->problem = "tool arguments could not be generated";
                    return MO_RECOVERY_UNAVAILABLE;
                }
                reuse(r, tool, &random, step->arguments);
            }
        }
        enum mo_recovery_outcome outcome = run_step(t, step, sequence->count - 1);
        if (outcome != MO_RECOVERY_PASSED)
            return outcome;
    }
}

enum mo_recovery_outcome mo_recovery_search(struct mo_recovery *r, uint64_t first,
                                            uint64_t sequences,
                                            const struct mo_recovery_hooks *hooks,
                                            struct mo_recovery_result *out) {
    MO_ASSERT(r != NULL && out != NULL);
    result_init(out);
    enum mo_recovery_outcome outcome = MO_RECOVERY_PASSED;
    for (uint64_t ordinal = first; ordinal < sequences; ordinal++) {
        struct trial t = {
            .recovery = r, .observer = hooks, .out = out, .ordinal = ordinal, .harvest = true};
        out->resume = out->counts;
        out->ordinal = ordinal;
        outcome = start(&t);
        if (outcome == MO_RECOVERY_PASSED)
            outcome = generate(&t);
        out->counts.commits += commits(&t);
        finish(&t);
        if (outcome != MO_RECOVERY_PASSED)
            break;
        out->counts.sequences++;
        mo_recovery_sequence_clear(r, &out->sequence);
    }
    if (outcome != MO_RECOVERY_MISMATCH && outcome != MO_RECOVERY_REFUSED &&
        outcome != MO_RECOVERY_CHECK)
        mo_recovery_sequence_clear(r, &out->sequence);
    out->outcome = outcome;
    return outcome;
}

enum mo_recovery_outcome mo_recovery_replay(struct mo_recovery *r,
                                            struct mo_recovery_sequence *sequence,
                                            const struct mo_recovery_hooks *hooks,
                                            struct mo_recovery_result *out) {
    MO_ASSERT(r != NULL && sequence != NULL && out != NULL);
    MO_ASSERT(sequence->count <= MO_CLI_RECOVERY_STEPS);
    bool crashed = false;
    for (uint32_t i = 0; i < sequence->count; i++) {
        struct mo_recovery_step *step = &sequence->steps[i];
        MO_ASSERT(step->kind != MO_RECOVERY_RESTART || crashed);
        MO_ASSERT(!crashed || step->kind == MO_RECOVERY_RESTART);
        MO_ASSERT(step->kind != MO_RECOVERY_SEND || step->tool < r->service->tool_count);
        crashed = step->kind == MO_RECOVERY_CRASH;
        step->ran = false;
    }
    MO_ASSERT(!crashed);
    result_init(out);
    struct trial t = {.recovery = r, .observer = hooks, .out = out};
    enum mo_recovery_outcome outcome = start(&t);
    for (uint32_t i = 0; i < sequence->count && outcome == MO_RECOVERY_PASSED && !t.ended; i++)
        outcome = run_step(&t, &sequence->steps[i], i);
    out->counts.commits += commits(&t);
    finish(&t);
    if (outcome == MO_RECOVERY_PASSED)
        out->counts.sequences = 1;
    out->outcome = outcome;
    return outcome;
}

/* Shrinking. */

struct shrink {
    struct mo_recovery *recovery;
    struct mo_recovery_sequence *best;
    enum mo_recovery_outcome failure;
    const struct mo_recovery_hooks *hooks;
    struct mo_recovery_verdict verdict;
    uint32_t runs;
    bool ended; /* the budget ran out, or memory did */
};

/* Copies from's steps into to, which is empty, leaving out the steps in [skip, end). false when
 * an argument array cannot be allocated; to then holds the steps copied before it. */
static bool copy_steps(struct mo_recovery *r, const struct mo_recovery_sequence *from,
                       uint32_t skip, uint32_t end, struct mo_recovery_sequence *to) {
    MO_ASSERT(to->count == 0);
    for (uint32_t i = 0; i < from->count; i++) {
        if (i >= skip && i < end)
            continue;
        const struct mo_recovery_step *step = &from->steps[i];
        struct mo_recovery_step *copy = &to->steps[to->count];
        *copy = (struct mo_recovery_step){
            .kind = step->kind, .tool = step->tool, .duration = step->duration};
        if (step->kind == MO_RECOVERY_SEND) {
            const struct mo_bytecode_service_tool *tool = &r->service->tools[step->tool];
            if (tool->argument_count != 0) {
                copy->arguments =
                    mo_arena_allocate_array(&to->arena, tool->argument_count,
                                            sizeof *copy->arguments, _Alignof(struct mo_cell));
                if (copy->arguments == NULL)
                    return false;
                for (uint32_t a = 0; a < tool->argument_count; a++)
                    copy->arguments[a] =
                        mo_value_dup(r->heap, tool->argument_types[a], step->arguments[a]);
            }
        }
        to->count++;
    }
    return true;
}

/* A copy of the best sequence without [skip, end), or false with candidate cleared. */
static bool candidate_from(struct shrink *s, uint32_t skip, uint32_t end,
                           struct mo_recovery_sequence *candidate) {
    mo_recovery_sequence_init(candidate);
    if (copy_steps(s->recovery, s->best, skip, end, candidate))
        return true;
    mo_recovery_sequence_clear(s->recovery, candidate);
    s->ended = true;
    return false;
}

/* Replays candidate when the budget allows. When it fails the same way, it becomes the best
 * sequence, cut after its failing step. Clears whichever loses. */
static bool attempt(struct shrink *s, struct mo_recovery_sequence *candidate) {
    struct mo_recovery *r = s->recovery;
    bool fails = false;
    if (s->runs == MO_CLI_RECOVERY_SHRINK_RUNS)
        s->ended = true;
    else {
        s->runs++;
        struct mo_recovery_result result;
        fails = mo_recovery_replay(r, candidate, s->hooks, &result) == s->failure &&
                result.verdict.check == s->verdict.check && result.verdict.line == s->verdict.line;
        uint32_t step = result.step;
        mo_recovery_result_free(r, &result);
        if (fails) {
            MO_ASSERT(step < candidate->count &&
                      (s->failure == MO_RECOVERY_CHECK ||
                       candidate->steps[step].kind == MO_RECOVERY_RESTART));
            cut(r, candidate, step + 1);
            struct mo_recovery_sequence old = *s->best;
            *s->best = *candidate;
            *candidate = old;
        }
    }
    mo_recovery_sequence_clear(r, candidate);
    return fails;
}

/* Steps a deletion removes together: a crash with its restart, or one step. */
static uint32_t unit(const struct mo_recovery_sequence *sequence, uint32_t i) {
    return sequence->steps[i].kind == MO_RECOVERY_CRASH ? 2 : 1;
}

/* Deletes runs of steps, halving their length down to one step or pair. */
static bool delete_steps(struct shrink *s) {
    bool progress = false;
    for (uint32_t size = s->best->count / 2; size != 0 && !s->ended; size /= 2)
        for (uint32_t i = 0; i < s->best->count && !s->ended;) {
            uint32_t end = i;
            while (end < s->best->count && end - i < size)
                end += unit(s->best, end);
            struct mo_recovery_sequence candidate;
            if (!candidate_from(s, i, end, &candidate))
                break;
            if (attempt(s, &candidate))
                progress = true;
            else
                i += unit(s->best, i);
        }
    return progress;
}

/* Simplifies each tool call's arguments with the input shrinker. */
static bool shrink_arguments(struct shrink *s) {
    struct mo_recovery *r = s->recovery;
    bool progress = false;
    for (uint32_t i = 0; i < s->best->count && !s->ended; i++) {
        if (s->best->steps[i].kind != MO_RECOVERY_SEND)
            continue;
        uint32_t tool = s->best->steps[i].tool;
        const struct mo_bytecode_service_tool *t = &r->service->tools[tool];
        if (t->argument_count == 0)
            continue;
        struct mo_input_shrinker shrinker;
        mo_input_shrink_init(&shrinker, &r->inputs[tool], MO_CLI_RECOVERY_SHRINK_RUNS);
        while (!s->ended && i < s->best->count) {
            struct mo_cell arguments[MO_LIMIT_BYTECODE_PARAMETERS];
            if (mo_input_shrink_next(&shrinker, s->best->steps[i].arguments, r->heap, arguments) !=
                MO_INPUT_OK)
                break;
            struct mo_recovery_sequence candidate;
            if (!candidate_from(s, s->best->count, s->best->count, &candidate)) {
                for (uint32_t a = 0; a < t->argument_count; a++)
                    mo_value_drop(r->heap, t->argument_types[a], arguments[a]);
                break;
            }
            struct mo_cell *copied = candidate.steps[i].arguments;
            for (uint32_t a = 0; a < t->argument_count; a++) {
                mo_value_drop(r->heap, t->argument_types[a], copied[a]);
                copied[a] = arguments[a];
            }
            bool fails = attempt(s, &candidate);
            mo_input_shrink_feedback(&shrinker, fails);
            progress |= fails;
        }
    }
    return progress;
}

/* Halves each advance while the sequence still fails. */
static bool shorten_advances(struct shrink *s) {
    bool progress = false;
    for (uint32_t i = 0; i < s->best->count && !s->ended; i++)
        while (!s->ended && i < s->best->count && s->best->steps[i].kind == MO_RECOVERY_ADVANCE &&
               s->best->steps[i].duration != 0) {
            struct mo_recovery_sequence candidate;
            if (!candidate_from(s, s->best->count, s->best->count, &candidate))
                break;
            candidate.steps[i].duration /= 2;
            if (!attempt(s, &candidate))
                break;
            progress = true;
        }
    return progress;
}

uint32_t mo_recovery_shrink(struct mo_recovery *r, struct mo_recovery_sequence *sequence,
                            enum mo_recovery_outcome failure, const struct mo_recovery_hooks *hooks,
                            struct mo_recovery_verdict verdict) {
    MO_ASSERT(r != NULL && sequence != NULL);
    MO_ASSERT(failure == MO_RECOVERY_MISMATCH || failure == MO_RECOVERY_REFUSED ||
              failure == MO_RECOVERY_CHECK);
    MO_ASSERT((failure == MO_RECOVERY_CHECK) == (verdict.check != MO_NO_INDEX));
    struct shrink s = {
        .recovery = r, .best = sequence, .failure = failure, .hooks = hooks, .verdict = verdict};
    for (bool progress = true; progress && !s.ended;) {
        progress = delete_steps(&s);
        progress |= shrink_arguments(&s);
        progress |= shorten_advances(&s);
    }
    return s.runs;
}
