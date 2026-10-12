#include "bytecode/io.h"

#include <stdlib.h>
#include <string.h>

#include "heap/types.h"

struct validation {
    const struct mo_bytecode *file;
    struct mo_arena arena;
    struct mo_load_error *error;
};

static MO_MUST_USE bool refuse(struct validation *v, const char *reason) {
    *v->error = (struct mo_load_error){.offset = MO_BYTECODE_HEADER_SIZE, .reason = reason};
    return false;
}

static MO_MUST_USE void *allocate(struct validation *v, size_t n, size_t size, size_t alignment) {
    void *p = mo_arena_allocate_array(&v->arena, n, size, alignment);
    if (p == NULL) {
        *v->error = (struct mo_load_error){.reason = "no memory"};
    }
    return p;
}

static MO_MUST_USE bool problem(struct validation *v, enum mo_type_problem result) {
    if (result == MO_TYPES_OK) {
        return true;
    }
    if (result == MO_TYPES_NO_MEMORY) {
        *v->error = (struct mo_load_error){.reason = "no memory"};
        return false;
    }
    return refuse(v, "invalid type or layout");
}

static MO_MUST_USE bool utf8(const uint8_t *bytes, uint32_t length) {
    for (uint32_t i = 0; i < length;) {
        uint32_t scalar = bytes[i++];
        unsigned n = 0;
        uint32_t minimum = 0;
        if (scalar < 0x80) {
            continue;
        }
        if (scalar >= 0xc2 && scalar <= 0xdf) {
            n = 1;
            minimum = 0x80;
            scalar &= 0x1f;
        } else if (scalar >= 0xe0 && scalar <= 0xef) {
            n = 2;
            minimum = 0x800;
            scalar &= 0x0f;
        } else if (scalar >= 0xf0 && scalar <= 0xf4) {
            n = 3;
            minimum = 0x10000;
            scalar &= 0x07;
        } else {
            return false;
        }
        if (n > length - i) {
            return false;
        }
        for (unsigned j = 0; j < n; j++) {
            uint32_t b = bytes[i++];
            if ((b & 0xc0) != 0x80) {
                return false;
            }
            scalar = (scalar << 6) | (b & 0x3f);
        }
        if (scalar < minimum || scalar >= 0x110000 || (scalar >= 0xd800 && scalar <= 0xdfff)) {
            return false;
        }
    }
    return true;
}

static MO_MUST_USE bool named(const struct mo_bytecode *f, uint32_t index) {
    return index < f->string_count && f->strings[index].length != 0 &&
           memchr(f->strings[index].bytes, 0, f->strings[index].length) == NULL;
}

static MO_MUST_USE bool nominal(const struct mo_type *t) {
    return t->kind == MO_TYPE_RECORD || t->kind == MO_TYPE_CHOICE;
}

struct visit {
    uint32_t type;
    uint32_t next;
};

/* Nominal declarations break recursion. Walk structural dependencies with an explicit stack,
 * then intern their remapped keys. A repeated key is a duplicate, not permission to renumber.
 * Only this scratch builder uses new indices; all published tables keep their file indices. */
static MO_MUST_USE bool type_table(struct validation *v, struct mo_types *types,
                                   uint32_t **map_out) {
    const struct mo_bytecode *f = v->file;
    uint32_t *map = allocate(v, f->type_count, sizeof *map, _Alignof(uint32_t));
    uint8_t *state = allocate(v, f->type_count, sizeof *state, 1);
    struct visit *stack = allocate(v, f->type_count, sizeof *stack, _Alignof(struct visit));
    uint32_t *parameters = allocate(v, MO_LIMIT_FIELDS, sizeof *parameters, _Alignof(uint32_t));
    if (map == NULL || state == NULL || stack == NULL || parameters == NULL) {
        return false;
    }
    memset(state, 0, f->type_count);
    for (uint32_t i = 0; i < f->type_count; i++) {
        const struct mo_type *t = &f->types[i];
        if (nominal(t)) {
            if (!problem(v, mo_types_declare(types, t->kind, t->name, &map[i]))) {
                return false;
            }
            state[i] = 2;
        } else {
            uint32_t limit = t->kind == MO_TYPE_FUNCTION ? MO_LIMIT_BYTECODE_PARAMETERS
                             : t->kind == MO_TYPE_TUPLE  ? MO_LIMIT_FIELDS
                             : t->kind == MO_TYPE_MAP    ? 2U
                             : t->kind == MO_TYPE_LIST || t->kind == MO_TYPE_SET ? 1U
                                                                                 : 0U;
            if (t->parameter_count > limit ||
                (t->kind == MO_TYPE_TUPLE && t->parameter_count < 2) ||
                ((t->kind == MO_TYPE_LIST || t->kind == MO_TYPE_SET) && t->parameter_count != 1) ||
                (t->kind == MO_TYPE_MAP && t->parameter_count != 2)) {
                return refuse(v, "invalid type parameter count");
            }
            for (uint32_t j = 0; j < t->parameter_count; j++) {
                if (t->parameters[j] >= f->type_count) {
                    return refuse(v, "type index out of range");
                }
            }
            if ((t->kind == MO_TYPE_FUNCTION && t->result >= f->type_count) ||
                (t->kind == MO_TYPE_ENV &&
                 (t->function >= f->function_count ||
                  f->functions[t->function].kind != MO_FUNCTION_CLOSURE))) {
                return refuse(v, "type result or env function out of range/kind");
            }
        }
    }
    for (uint32_t root = 0; root < f->type_count; root++) {
        if (state[root] == 2) {
            continue;
        }
        uint32_t depth = 1;
        stack[0] = (struct visit){.type = root};
        state[root] = 1;
        while (depth != 0) {
            struct visit *frame = &stack[depth - 1];
            const struct mo_type *t = &f->types[frame->type];
            uint32_t dependencies = t->parameter_count + (t->kind == MO_TYPE_FUNCTION ? 1U : 0U);
            if (frame->next < dependencies) {
                uint32_t dep =
                    frame->next < t->parameter_count ? t->parameters[frame->next] : t->result;
                frame->next++;
                if (state[dep] == 1) {
                    return refuse(v, "structural type cycle");
                }
                if (state[dep] == 0) {
                    MO_ASSERT(depth < f->type_count);
                    stack[depth++] = (struct visit){.type = dep};
                    state[dep] = 1;
                }
                continue;
            }
            struct mo_type key = *t;
            for (uint32_t j = 0; j < t->parameter_count; j++) {
                parameters[j] = map[t->parameters[j]];
            }
            key.parameters = parameters;
            if (t->kind == MO_TYPE_FUNCTION) {
                key.result = map[t->result];
            }
            uint32_t before = types->count;
            if (!problem(v, mo_types_intern(types, &key, &map[frame->type]))) {
                return false;
            }
            if (types->count == before) {
                return refuse(v, "duplicate interned type");
            }
            state[frame->type] = 2;
            depth--;
        }
    }
    for (uint32_t i = 0; i < f->type_count; i++) {
        const struct mo_type *t = &f->types[i];
        if (!nominal(t)) {
            continue;
        }
        uint32_t n = t->kind == MO_TYPE_RECORD ? 1U : t->case_count;
        struct mo_type_case *cases = allocate(v, n, sizeof *cases, _Alignof(struct mo_type_case));
        if (cases == NULL) {
            return false;
        }
        for (uint32_t j = 0; j < n; j++) {
            uint32_t count = t->kind == MO_TYPE_RECORD ? t->field_count : t->cases[j].field_count;
            const struct mo_type_field *source =
                t->kind == MO_TYPE_RECORD ? t->fields : t->cases[j].fields;
            struct mo_type_field *fields =
                allocate(v, count, sizeof *fields, _Alignof(struct mo_type_field));
            if (fields == NULL) {
                return false;
            }
            for (uint32_t k = 0; k < count; k++) {
                if (source[k].type >= f->type_count) {
                    return refuse(v, "field type out of range");
                }
                fields[k] = source[k];
                fields[k].type = map[source[k].type];
            }
            cases[j] =
                (struct mo_type_case){.name = t->kind == MO_TYPE_RECORD ? NULL : t->cases[j].name,
                                      .fields = fields,
                                      .field_count = count,
                                      .unnamed = count == 1 && fields[0].name == NULL};
        }
        enum mo_type_problem result =
            t->kind == MO_TYPE_RECORD
                ? mo_types_define_record(types, map[i], cases[0].fields, cases[0].field_count)
                : mo_types_define_choice(types, map[i], cases, n);
        if (!problem(v, result)) {
            return false;
        }
    }
    *map_out = map;
    return problem(v, mo_types_finish(types));
}

static MO_MUST_USE bool signature(const struct mo_bytecode *f, uint32_t type, uint32_t function) {
    if (type >= f->type_count || function >= f->function_count) {
        return false;
    }
    const struct mo_type *t = &f->types[type];
    const struct mo_bytecode_function *fn = &f->functions[function];
    if (t->kind != MO_TYPE_FUNCTION || fn->kind != MO_FUNCTION_CLOSURE ||
        t->parameter_count != fn->parameter_count || t->result != fn->result_type) {
        return false;
    }
    for (uint32_t i = 0; i < t->parameter_count; i++) {
        if (t->parameters[i] != fn->parameters[i].type) {
            return false;
        }
    }
    return true;
}

static MO_MUST_USE bool layouts(struct validation *v, struct mo_types *types, const uint32_t *map) {
    const struct mo_bytecode *f = v->file;
    uint32_t *fields = allocate(v, MO_LIMIT_FIELDS, sizeof *fields, _Alignof(uint32_t));
    if (fields == NULL) {
        return false;
    }
    for (uint32_t i = 0; i < f->layout_count; i++) {
        const struct mo_layout *l = &f->layouts[i];
        if (l->type >= f->type_count) {
            return refuse(v, "layout type out of range");
        }
        struct mo_layout key = *l;
        key.type = map[l->type];
        for (uint32_t j = 0; j < l->field_count; j++) {
            if (l->fields[j] >= f->type_count) {
                return refuse(v, "layout field type out of range");
            }
            fields[j] = map[l->fields[j]];
        }
        key.fields = fields;
        if (l->kind == MO_LAYOUT_CLOSURE) {
            if (!signature(f, l->type, l->function)) {
                return refuse(v, "closure layout signature mismatch");
            }
            const struct mo_bytecode_function *fn = &f->functions[l->function];
            if (l->field_count != fn->capture_count ||
                (l->field_count != 0 && memcmp(l->fields, fn->capture_types,
                                               (size_t)l->field_count * sizeof(uint32_t)) != 0)) {
                return refuse(v, "closure layout captures mismatch");
            }
        }
        uint32_t unused;
        if (!problem(v, mo_types_layout(types, &key, &unused))) {
            return false;
        }
    }
    return true;
}

static MO_MUST_USE uint64_t immediate(const uint8_t *bytes) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; i++) {
        value |= (uint64_t)bytes[i] << (i * 8);
    }
    return value;
}

static MO_MUST_USE bool constants(struct validation *v) {
    const struct mo_bytecode *f = v->file;
    for (uint32_t i = 0; i < f->constant_count; i++) {
        const struct mo_bytecode_constant *k = &f->constants[i];
        enum mo_type_kind kind = f->types[k->type].kind;
        uint64_t value = immediate(k->value.immediate);
        if ((kind == MO_TYPE_UNIT && value != 0) || (kind == MO_TYPE_BOOL && value > 1) ||
            (kind == MO_TYPE_BYTE && value > UINT8_MAX) ||
            (kind == MO_TYPE_CHAR && (value >= 0x110000 || (value >= 0xd800 && value <= 0xdfff)))) {
            return refuse(v, "invalid scalar literal");
        }
        if (kind == MO_TYPE_TIME) {
            /* Compare two's-complement bits without an implementation-defined signed cast.
             * 0001-01-01 inclusive; 10000-01-01 exclusive, in microseconds from the epoch. */
            const uint64_t minimum = UINT64_MAX - UINT64_C(62135596800000000) + 1;
            const uint64_t maximum = UINT64_C(253402300800000000);
            if ((value >> 63) != 0 ? value < minimum : value >= maximum) {
                return refuse(v, "time literal out of range");
            }
        }
        if (kind == MO_TYPE_DECIMAL && immediate(k->value.immediate) == 0 &&
            immediate(k->value.immediate + 8) == (UINT64_C(1) << 63)) {
            return refuse(v, "decimal literal out of range");
        }
        if (kind == MO_TYPE_TEXT && k->value.string >= f->string_count) {
            return refuse(v, "text string index out of range");
        }
        if (kind == MO_TYPE_BIGINT) {
            const struct mo_bytecode_bigint *b = &k->value.bigint;
            if ((b->sign == MO_BIGINT_ZERO) != (b->magnitude.length == 0) ||
                (b->magnitude.length != 0 && b->magnitude.bytes[b->magnitude.length - 1] == 0)) {
                return refuse(v, "noncanonical bigint literal");
            }
        }
        if (kind == MO_TYPE_FUNCTION && (!signature(f, k->type, k->value.function) ||
                                         f->functions[k->value.function].capture_count != 0)) {
            return refuse(v, "invalid closure literal");
        }
    }
    return true;
}

static MO_MUST_USE bool functions(struct validation *v) {
    const struct mo_bytecode *f = v->file;
    for (uint32_t i = 0; i < f->function_count; i++) {
        const struct mo_bytecode_function *fn = &f->functions[i];
        if (!named(f, fn->name) || fn->module >= f->module_count ||
            fn->result_type >= f->type_count) {
            return refuse(v, "function name, module or result out of range");
        }
        uint32_t slots = fn->parameter_count + (fn->kind == MO_FUNCTION_CLOSURE ? 1U : 0U);
        if (slots > MO_LIMIT_BYTECODE_PARAMETERS || fn->register_count < slots ||
            (fn->kind != MO_FUNCTION_CLOSURE && fn->capture_count != 0)) {
            return refuse(v, "invalid function parameter/register/capture counts");
        }
        bool spec = fn->kind >= MO_FUNCTION_TEST;
        if (fn->kind != MO_FUNCTION_CLOSURE && fn->spec_code != spec) {
            return refuse(v, "function spec flag disagrees with kind");
        }
        enum mo_type_kind result = f->types[fn->result_type].kind;
        if ((fn->kind >= MO_FUNCTION_INIT && fn->kind <= MO_FUNCTION_EXAMPLE &&
             (fn->parameter_count != 0 || result != MO_TYPE_UNIT)) ||
            (fn->kind == MO_FUNCTION_PROMISE &&
             (fn->parameter_count == 0 || result != MO_TYPE_INT)) ||
            (fn->kind == MO_FUNCTION_ANCHOR &&
             (fn->parameter_count != 0 || result != MO_TYPE_INT))) {
            return refuse(v, "invalid function kind signature");
        }
        for (uint32_t j = 0; j < fn->register_count; j++) {
            if (fn->register_types[j] >= f->type_count) {
                return refuse(v, "register type out of range");
            }
        }
        for (uint32_t j = 0; j < fn->parameter_count; j++) {
            if (fn->parameters[j].type != fn->register_types[j] ||
                (fn->kind != MO_FUNCTION_FUNCTION &&
                 fn->parameters[j].ownership != MO_PARAMETER_OWNED)) {
                return refuse(v, "invalid parameter type or borrowing");
            }
        }
        if (fn->kind == MO_FUNCTION_CLOSURE) {
            const struct mo_type *env = &f->types[fn->register_types[fn->parameter_count]];
            if (env->kind != MO_TYPE_ENV || env->function != i) {
                return refuse(v, "closure env mismatch");
            }
        }
        for (uint32_t j = 0; j < fn->capture_count; j++) {
            if (fn->capture_types[j] >= f->type_count) {
                return refuse(v, "capture type out of range");
            }
        }
    }
    return true;
}

static MO_MUST_USE bool modules(struct validation *v) {
    const struct mo_bytecode *f = v->file;
    if (f->init_count != f->global_count || f->debug_count != f->function_count) {
        return refuse(v, "init/global or debug/function count mismatch");
    }
    uint8_t *inits = allocate(v, f->function_count, 1, 1);
    uint8_t *globals = allocate(v, f->global_count, 1, 1);
    if (inits == NULL || globals == NULL) {
        return false;
    }
    memset(inits, 0, f->function_count);
    memset(globals, 0, f->global_count);
    for (uint32_t i = 0; i < f->init_count; i++) {
        uint32_t fn = f->init[i];
        if (fn >= f->function_count || f->functions[fn].kind != MO_FUNCTION_INIT ||
            inits[fn] != 0) {
            return refuse(v, "invalid or repeated init function");
        }
        inits[fn] = 1;
    }
    for (uint32_t i = 0; i < f->function_count; i++) {
        if (f->functions[i].kind == MO_FUNCTION_INIT && inits[i] == 0) {
            return refuse(v, "init function absent from init section");
        }
    }
    for (uint32_t i = 0; i < f->global_count; i++) {
        const struct mo_bytecode_global *g = &f->globals[i];
        if (g->type >= f->type_count || !named(f, g->name) || g->module >= f->module_count) {
            return refuse(v, "global type, name or module out of range");
        }
    }
    for (uint32_t i = 0; i < f->module_count; i++) {
        const struct mo_bytecode_module *m = &f->modules[i];
        if (!named(f, m->name)) {
            return refuse(v, "invalid module name");
        }
        for (uint32_t j = 0; j < m->type_count; j++) {
            if (m->types[j].type >= f->type_count) {
                return refuse(v, "exposed type out of range");
            }
        }
        for (uint32_t j = 0; j < m->function_count; j++) {
            if (m->functions[j] >= f->function_count || f->functions[m->functions[j]].module != i) {
                return refuse(v, "exposed function out of range/module");
            }
        }
        for (uint32_t j = 0; j < m->global_count; j++) {
            uint32_t g = m->globals[j];
            if (g >= f->global_count || f->globals[g].module != i || globals[g] != 0) {
                return refuse(v, "module global out of range/module or repeated");
            }
            globals[g] = 1;
        }
    }
    for (uint32_t i = 0; i < f->global_count; i++) {
        if (globals[i] == 0) {
            return refuse(v, "global absent from module");
        }
    }
    return true;
}

static MO_MUST_USE bool operand(const struct mo_bytecode *f, const struct mo_bytecode_function *fn,
                                enum mo_operand_kind kind, uint32_t value, const uint8_t *starts) {
    switch (kind) {
    case MO_OPERAND_REGISTER:
        return value < fn->register_count;
    case MO_OPERAND_TYPE:
        return value < f->type_count;
    case MO_OPERAND_LAYOUT:
        return value < f->layout_count;
    case MO_OPERAND_CONSTANT:
        return value < f->constant_count;
    case MO_OPERAND_GLOBAL:
        return value < f->global_count;
    case MO_OPERAND_FUNCTION:
        return value < f->function_count;
    case MO_OPERAND_PRIMITIVE:
        return value < MO_PRIM_COUNT;
    case MO_OPERAND_HOST:
        return value < MO_HOST_COUNT;
    case MO_OPERAND_TARGET:
        return value < fn->code_count && starts[value] != 0;
    case MO_OPERAND_FIELD:
        return value < MO_LIMIT_FIELDS;
    case MO_OPERAND_CASE:
        return value < MO_LIMIT_CHOICE_CASES;
    case MO_OPERAND_NONE:
        return false;
    }
    MO_UNREACHABLE();
}

static MO_MUST_USE int compare_ranges(const void *a, const void *b) {
    const struct mo_bytecode_register_name *const *left = a;
    const struct mo_bytecode_register_name *const *right = b;
    if ((*left)->reg != (*right)->reg) {
        return (*left)->reg < (*right)->reg ? -1 : 1;
    }
    return ((*left)->code_start > (*right)->code_start) -
           ((*left)->code_start < (*right)->code_start);
}

static MO_MUST_USE bool code_debug(struct validation *v, uint32_t index, uint32_t *total) {
    const struct mo_bytecode *f = v->file;
    const struct mo_bytecode_function *fn = &f->functions[index];
    const struct mo_bytecode_debug *d = &f->debug[index];
    uint8_t *starts = allocate(v, (size_t)fn->code_count + 1, 1, 1);
    if (starts == NULL) {
        return false;
    }
    memset(starts, 0, (size_t)fn->code_count + 1);
    uint32_t instructions = 0;
    for (uint32_t pc = 0; pc < fn->code_count;) {
        starts[pc] = 1;
        const struct mo_opcode_info *op = mo_opcode_find(fn->code[pc] & MO_OPCODE_MASK);
        uint32_t n = fn->code[pc] >> MO_OPCODE_VARIABLE_SHIFT;
        if (op == NULL || n > op->variable_limit ||
            1U + op->fixed_count + n > fn->code_count - pc ||
            ++instructions > MO_LIMIT_FUNCTION_INSTRUCTIONS) {
            return refuse(v, "invalid instruction shape or instruction limit");
        }
        pc += 1U + op->fixed_count + n;
    }
    starts[fn->code_count] = 1;
    for (uint32_t pc = 0; pc < fn->code_count;) {
        const struct mo_opcode_info *op = mo_opcode_find(fn->code[pc] & MO_OPCODE_MASK);
        MO_ASSERT(op != NULL);
        uint32_t n = fn->code[pc] >> MO_OPCODE_VARIABLE_SHIFT;
        for (uint32_t j = 0; j < op->fixed_count + n; j++) {
            enum mo_operand_kind kind = j < op->fixed_count ? op->fixed[j].kind : op->variable.kind;
            if (!operand(f, fn, kind, fn->code[pc + 1 + j], starts)) {
                return refuse(v, "instruction index out of range or target not a boundary");
            }
        }
        pc += 1U + op->fixed_count + n;
    }
    if (d->span_count > MO_LIMIT_DEBUG_ENTRIES - *total) {
        return refuse(v, "debug entries past limit");
    }
    *total += d->span_count;
    if (d->name_count > MO_LIMIT_DEBUG_ENTRIES - *total) {
        return refuse(v, "debug entries past limit");
    }
    *total += d->name_count;
    for (uint32_t j = 0; j < d->span_count; j++) {
        const struct mo_bytecode_span *s = &d->spans[j];
        if (s->code_offset >= fn->code_count || starts[s->code_offset] == 0 || !named(f, s->file) ||
            s->start > s->end || s->end > MO_LIMIT_FILE_SIZE ||
            (j != 0 && d->spans[j - 1].code_offset >= s->code_offset)) {
            return refuse(v, "invalid debug span");
        }
    }
    const struct mo_bytecode_register_name **ranges =
        allocate(v, d->name_count, sizeof *ranges, _Alignof(const void *));
    if (ranges == NULL) {
        return false;
    }
    for (uint32_t j = 0; j < d->name_count; j++) {
        const struct mo_bytecode_register_name *r = &d->names[j];
        if (r->reg >= fn->register_count || !named(f, r->name) || r->code_start >= r->code_end ||
            r->code_end > fn->code_count || starts[r->code_start] == 0 ||
            starts[r->code_end] == 0) {
            return refuse(v, "invalid debug register range");
        }
        ranges[j] = r;
    }
    qsort(ranges, d->name_count, sizeof *ranges, compare_ranges);
    for (uint32_t j = 1; j < d->name_count; j++) {
        if (ranges[j - 1]->reg == ranges[j]->reg &&
            ranges[j - 1]->code_end > ranges[j]->code_start) {
            return refuse(v, "overlapping debug register ranges");
        }
    }
    bool spec = fn->kind >= MO_FUNCTION_TEST;
    if (spec ? d->text >= f->string_count : d->text != MO_BYTECODE_NO_INDEX) {
        return refuse(v, "invalid debug text index");
    }
    return true;
}

static MO_MUST_USE bool validate(struct validation *v) {
    const struct mo_bytecode *f = v->file;
    for (uint32_t i = 0; i < f->string_count; i++) {
        if (!utf8(f->strings[i].bytes, f->strings[i].length)) {
            return refuse(v, "invalid UTF-8 string");
        }
    }
    if (!functions(v) || !modules(v)) {
        return false;
    }
    struct mo_types types;
    mo_types_init(&types, &v->arena);
    uint32_t *map;
    if (!type_table(v, &types, &map) || !layouts(v, &types, map) || !constants(v)) {
        return false;
    }
    mo_arena_free(&v->arena);
    uint32_t total = 0;
    for (uint32_t i = 0; i < f->function_count; i++) {
        bool ok = code_debug(v, i, &total);
        mo_arena_free(&v->arena);
        if (!ok) {
            return false;
        }
    }
    return true;
}

bool mo_bytecode_validate(const struct mo_bytecode *file, struct mo_load_error *error) {
    MO_ASSERT(file != NULL && error != NULL);
    struct validation v = {.file = file, .error = error};
    mo_arena_init(&v.arena, MO_LIMIT_BYTECODE_SIZE);
    bool ok = validate(&v);
    mo_arena_free(&v.arena);
    return ok;
}
