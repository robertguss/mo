#include "bytecode/io.h"
#include "bytecode/names.h"
#include "bytecode/verify.h"

#include <stdlib.h>
#include <string.h>

#include "heap/types.h"

struct validation {
    const struct mo_bytecode *file;
    const struct mo_bytecode_read_info *info;
    struct mo_arena arena;
    struct mo_load_error *error;
    struct mo_bytecode_error_site current;
};

static MO_MUST_USE bool refuse(struct validation *v, const char *reason) {
    *v->error = (struct mo_load_error){.offset = MO_BYTECODE_HEADER_SIZE, .reason = reason};
    if (v->info->site != NULL)
        *v->info->site = v->current;
    return false;
}

static MO_MUST_USE bool refuse_at(struct validation *v, uint32_t offset, const char *reason) {
    *v->error = (struct mo_load_error){.offset = offset, .reason = reason};
    return false;
}

static MO_MUST_USE void *allocate(struct validation *v, size_t n, size_t size, size_t alignment) {
    void *p = mo_arena_allocate_array(&v->arena, n, size, alignment);
    if (p == NULL) {
        *v->error = (struct mo_load_error){.reason = "no memory", .kind = MO_BYTECODE_NO_MEMORY};
    }
    return p;
}

static MO_MUST_USE bool named(const struct validation *v, uint32_t index) {
    return index < v->file->string_count &&
           v->info->names->canonical[index] != MO_BYTECODE_NO_INDEX;
}

static MO_MUST_USE bool nominal(const struct mo_type *t) {
    return t->kind == MO_TYPE_RECORD || t->kind == MO_TYPE_CHOICE;
}

struct visit {
    uint32_t type;
    uint32_t next;
};

static MO_MUST_USE int compare_indices(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a;
    uint32_t y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

static MO_MUST_USE bool unique(uint32_t *indices, uint32_t n) {
    qsort(indices, n, sizeof *indices, compare_indices);
    for (uint32_t i = 1; i < n; i++) {
        if (indices[i - 1] == indices[i]) {
            return false;
        }
    }
    return true;
}

static MO_MUST_USE bool field_names(struct validation *v, const struct mo_type_field *fields,
                                    uint32_t n, uint32_t *names, bool optional) {
    for (uint32_t i = 0; i < n; i++) {
        if (fields[i].type >= v->file->type_count) {
            return refuse(v, "field type out of range");
        }
        if (optional && n == 1 && fields[i].name == NULL) {
            return true;
        }
        names[i] = mo_bytecode_name_pointer(v->info->names, fields[i].name);
        if (names[i] == MO_BYTECODE_NO_INDEX) {
            return refuse(v, "invalid field name");
        }
    }
    return unique(names, n) || refuse(v, "duplicate field name");
}

static MO_MUST_USE int type_key(const struct mo_type *x, const struct mo_type *y) {
    if (x->kind != y->kind) {
        return (x->kind > y->kind) - (x->kind < y->kind);
    }
    const uint32_t a[] = {x->result, x->function, (uint32_t)x->capability, x->parameter_count};
    const uint32_t b[] = {y->result, y->function, (uint32_t)y->capability, y->parameter_count};
    for (size_t i = 0; i < sizeof a / sizeof a[0]; i++) {
        if (a[i] != b[i]) {
            return (a[i] > b[i]) - (a[i] < b[i]);
        }
    }
    for (uint32_t i = 0; i < x->parameter_count; i++) {
        if (x->parameters[i] != y->parameters[i]) {
            return (x->parameters[i] > y->parameters[i]) - (x->parameters[i] < y->parameters[i]);
        }
    }
    return 0;
}

static MO_MUST_USE int compare_types(const void *a, const void *b) {
    const struct mo_type *const *x = a;
    const struct mo_type *const *y = b;
    return type_key(*x, *y);
}

/* Nominal identities break recursion. Check structural cycles with an explicit stack, then
 * sort structural keys on file indices. Duplicate dependencies are themselves refused, so
 * unequal indices cannot conceal equal structural keys. No hash table sees untrusted keys. */
static MO_MUST_USE bool type_table(struct validation *v) {
    const struct mo_bytecode *f = v->file;
    uint8_t *state = allocate(v, f->type_count, sizeof *state, 1);
    struct visit *stack = allocate(v, f->type_count, sizeof *stack, _Alignof(struct visit));
    const struct mo_type **keys = allocate(v, f->type_count, sizeof *keys, _Alignof(const void *));
    uint32_t max_fields = 0;
    uint32_t max_cases = 0;
    for (uint32_t i = 0; i < f->type_count; i++) {
        const struct mo_type *t = &f->types[i];
        if (t->field_count > max_fields) {
            max_fields = t->field_count;
        }
        if (t->case_count > max_cases) {
            max_cases = t->case_count;
        }
        for (uint32_t j = 0; j < t->case_count; j++) {
            if (t->cases[j].field_count > max_fields) {
                max_fields = t->cases[j].field_count;
            }
        }
    }
    uint32_t *field_ids = allocate(v, max_fields, sizeof *field_ids, _Alignof(uint32_t));
    uint32_t *case_ids = allocate(v, max_cases, sizeof *case_ids, _Alignof(uint32_t));
    if (state == NULL || stack == NULL || keys == NULL || field_ids == NULL || case_ids == NULL) {
        return false;
    }
    memset(state, 0, f->type_count);
    uint32_t key_count = 0;
    for (uint32_t i = 0; i < f->type_count; i++) {
        v->current = (struct mo_bytecode_error_site){MO_SECTION_TYPES, i};
        const struct mo_type *t = &f->types[i];
        if (nominal(t)) {
            if (t->kind == MO_TYPE_RECORD &&
                !field_names(v, t->fields, t->field_count, field_ids, false)) {
                return false;
            }
            if (t->kind == MO_TYPE_CHOICE) {
                for (uint32_t j = 0; j < t->case_count; j++) {
                    case_ids[j] = mo_bytecode_name_pointer(v->info->names, t->cases[j].name);
                    MO_ASSERT(case_ids[j] != MO_BYTECODE_NO_INDEX);
                    if (!field_names(v, t->cases[j].fields, t->cases[j].field_count, field_ids,
                                     true)) {
                        return false;
                    }
                }
                if (!unique(case_ids, t->case_count)) {
                    return refuse(v, "duplicate case name");
                }
            }
            state[i] = 2;
        } else {
            keys[key_count++] = t;
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
                    v->current = (struct mo_bytecode_error_site){MO_SECTION_TYPES, frame->type};
                    return refuse(v, "structural type cycle");
                }
                if (state[dep] == 0) {
                    MO_ASSERT(depth < f->type_count);
                    stack[depth++] = (struct visit){.type = dep};
                    state[dep] = 1;
                }
                continue;
            }
            state[frame->type] = 2;
            depth--;
        }
    }
    qsort(keys, key_count, sizeof *keys, compare_types);
    for (uint32_t i = 1; i < key_count; i++) {
        if (type_key(keys[i - 1], keys[i]) == 0) {
            uint32_t a = (uint32_t)(keys[i - 1] - f->types), b = (uint32_t)(keys[i] - f->types);
            v->current = (struct mo_bytecode_error_site){MO_SECTION_TYPES, a > b ? a : b};
            return refuse(v, "duplicate interned type");
        }
    }
    return true;
}

static MO_MUST_USE bool signature(const struct mo_bytecode *f, uint32_t type, uint32_t function) {
    return type < f->type_count && function < f->function_count &&
           mo_verify_closure_signature(f, type, function);
}

static MO_MUST_USE int compare_layouts(const void *a, const void *b) {
    const struct mo_layout *const *x = a;
    const struct mo_layout *const *y = b;
    const uint32_t left[] = {(uint32_t)(*x)->kind, (*x)->type, (*x)->case_index, (*x)->function};
    const uint32_t right[] = {(uint32_t)(*y)->kind, (*y)->type, (*y)->case_index, (*y)->function};
    for (size_t i = 0; i < sizeof left / sizeof left[0]; i++) {
        if (left[i] != right[i]) {
            return (left[i] > right[i]) - (left[i] < right[i]);
        }
    }
    /* Fields are determined by these keys, checked before sorting. */
    return 0;
}

static MO_MUST_USE bool layouts(struct validation *v) {
    const struct mo_bytecode *f = v->file;
    const struct mo_layout **keys =
        allocate(v, f->layout_count, sizeof *keys, _Alignof(const void *));
    if (keys == NULL) {
        return false;
    }
    for (uint32_t i = 0; i < f->layout_count; i++) {
        v->current = (struct mo_bytecode_error_site){MO_SECTION_LAYOUTS, i};
        const struct mo_layout *l = &f->layouts[i];
        if (l->type >= f->type_count) {
            return refuse(v, "layout type out of range");
        }
        const struct mo_type *t = &f->types[l->type];
        for (uint32_t j = 0; j < l->field_count; j++) {
            if (l->fields[j] >= f->type_count) {
                return refuse(v, "layout field type out of range");
            }
        }
        const struct mo_type_field *fields = NULL;
        uint32_t count = 0;
        switch (l->kind) {
        case MO_LAYOUT_RECORD:
            if (t->kind != MO_TYPE_RECORD) {
                return refuse(v, "layout kind disagrees with type");
            }
            fields = t->fields;
            count = t->field_count;
            break;
        case MO_LAYOUT_TUPLE:
            if (t->kind != MO_TYPE_TUPLE || l->field_count != t->parameter_count ||
                (l->field_count != 0 && memcmp(l->fields, t->parameters,
                                               (size_t)l->field_count * sizeof(uint32_t)) != 0)) {
                return refuse(v, "tuple layout fields mismatch");
            }
            break;
        case MO_LAYOUT_CASE:
            if (t->kind != MO_TYPE_CHOICE || l->case_index >= t->case_count ||
                mo_case_shape(&v->info->types, l->type, l->case_index) != MO_CASE_BOXED) {
                return refuse(v, "invalid case layout");
            }
            fields = t->cases[l->case_index].fields;
            count = t->cases[l->case_index].field_count;
            break;
        case MO_LAYOUT_CLOSURE: {
            if (!signature(f, l->type, l->function)) {
                return refuse(v, "closure layout signature mismatch");
            }
            const struct mo_bytecode_function *fn = &f->functions[l->function];
            if (l->field_count != fn->capture_count ||
                (l->field_count != 0 && memcmp(l->fields, fn->capture_types,
                                               (size_t)l->field_count * sizeof(uint32_t)) != 0)) {
                return refuse(v, "closure layout captures mismatch");
            }
            break;
        }
        }
        if (l->kind == MO_LAYOUT_RECORD || l->kind == MO_LAYOUT_CASE) {
            if (l->field_count != count) {
                return refuse(v, "layout field count mismatch");
            }
            for (uint32_t j = 0; j < count; j++) {
                if (l->fields[j] != fields[j].type) {
                    return refuse(v, "layout field type mismatch");
                }
            }
        }
        keys[i] = l;
    }
    qsort(keys, f->layout_count, sizeof *keys, compare_layouts);
    for (uint32_t i = 1; i < f->layout_count; i++) {
        if (compare_layouts(&keys[i - 1], &keys[i]) == 0) {
            uint32_t a = (uint32_t)(keys[i - 1] - f->layouts), b = (uint32_t)(keys[i] - f->layouts);
            v->current = (struct mo_bytecode_error_site){MO_SECTION_LAYOUTS, a > b ? a : b};
            return refuse(v, "duplicate layout");
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
        v->current = (struct mo_bytecode_error_site){MO_SECTION_CONSTANTS, i};
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
        v->current = (struct mo_bytecode_error_site){MO_SECTION_FUNCTIONS, i};
        const struct mo_bytecode_function *fn = &f->functions[i];
        if (!named(v, fn->name) || fn->module >= f->module_count ||
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
        /* A missing init declaration belongs to the first unpaired global. */
        v->current = f->init_count < f->global_count
                         ? (struct mo_bytecode_error_site){MO_SECTION_GLOBALS, f->init_count}
                     : f->init_count != f->global_count
                         ? (struct mo_bytecode_error_site){MO_SECTION_INIT, MO_BYTECODE_NO_INDEX}
                         : (struct mo_bytecode_error_site){MO_SECTION_DEBUG, MO_BYTECODE_NO_INDEX};
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
        v->current = (struct mo_bytecode_error_site){MO_SECTION_INIT, i};
        uint32_t fn = f->init[i];
        if (fn >= f->function_count || f->functions[fn].kind != MO_FUNCTION_INIT ||
            inits[fn] != 0) {
            return refuse(v, "invalid or repeated init function");
        }
        inits[fn] = 1;
    }
    for (uint32_t i = 0; i < f->function_count; i++) {
        if (f->functions[i].kind == MO_FUNCTION_INIT && inits[i] == 0) {
            v->current = (struct mo_bytecode_error_site){MO_SECTION_FUNCTIONS, i};
            return refuse(v, "init function absent from init section");
        }
    }
    for (uint32_t i = 0; i < f->global_count; i++) {
        v->current = (struct mo_bytecode_error_site){MO_SECTION_GLOBALS, i};
        const struct mo_bytecode_global *g = &f->globals[i];
        if (g->type >= f->type_count || !named(v, g->name) || g->module >= f->module_count) {
            return refuse(v, "global type, name or module out of range");
        }
    }
    for (uint32_t i = 0; i < f->module_count; i++) {
        v->current = (struct mo_bytecode_error_site){MO_SECTION_MODULES, i};
        const struct mo_bytecode_module *m = &f->modules[i];
        if (!named(v, m->name)) {
            return refuse(v, "invalid module name");
        }
        uint32_t *exports = allocate(v, m->type_count, sizeof *exports, _Alignof(uint32_t));
        if (exports == NULL) {
            return false;
        }
        for (uint32_t j = 0; j < m->type_count; j++) {
            if (m->types[j].type >= f->type_count) {
                return refuse(v, "exposed type out of range");
            }
            exports[j] = m->types[j].type;
        }
        if (!unique(exports, m->type_count)) {
            return refuse(v, "repeated exposed type");
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
            v->current = (struct mo_bytecode_error_site){MO_SECTION_GLOBALS, i};
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
    case MO_OPERAND_INDEX:
        return true;
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

static MO_MUST_USE bool source_span(const struct validation *v, uint32_t file, uint32_t start,
                                    uint32_t end) {
    return file == MO_BYTECODE_NO_INDEX
               ? start == 0 && end == 0
               : file < v->file->file_count && start <= end && end <= MO_LIMIT_FILE_SIZE;
}

/* Each span, register name and condition line occupies sixteen wire bytes. The file bound
 * therefore enforces the aggregate debug bound, including entries across functions. */
_Static_assert(MO_LIMIT_BYTECODE_SIZE / 16 <= MO_LIMIT_DEBUG_ENTRIES,
               "wire size enforces the aggregate debug-entry limit");

static MO_MUST_USE bool code_debug(struct validation *v, uint32_t index) {
    const struct mo_bytecode *f = v->file;
    const struct mo_bytecode_function *fn = &f->functions[index];
    const struct mo_bytecode_debug *d = &f->debug[index];
    const struct mo_bytecode_debug_positions *positions = &v->info->debug[index];
    uint32_t code = v->info->code[index];
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
            1U + op->fixed_count + n > fn->code_count - pc) {
            bool ok = refuse_at(v, code + pc * 4, "invalid instruction shape or instruction limit");
            if (op != NULL && op->variable_limit != 0 && n > op->variable_limit) {
                v->error->kind = MO_BYTECODE_LIMIT;
            }
            return ok;
        }
        if (++instructions > MO_LIMIT_FUNCTION_INSTRUCTIONS) {
            bool ok = refuse_at(v, code + pc * 4, "invalid instruction shape or instruction limit");
            v->error->kind = MO_BYTECODE_LIMIT;
            return ok;
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
                return refuse_at(v, code + (pc + 1 + j) * 4,
                                 "instruction index out of range or target not a boundary");
            }
        }
        pc += 1U + op->fixed_count + n;
    }
    for (uint32_t j = 0; j < d->span_count; j++) {
        const struct mo_bytecode_span *s = &d->spans[j];
        if (s->code_offset >= fn->code_count || starts[s->code_offset] == 0 ||
            !source_span(v, s->file, s->start, s->end) ||
            (j != 0 && d->spans[j - 1].code_offset >= s->code_offset)) {
            return refuse_at(v, positions->spans + 4 + j * 16, "invalid debug span");
        }
    }
    const struct mo_bytecode_register_name **ranges =
        allocate(v, d->name_count, sizeof *ranges, _Alignof(const void *));
    if (ranges == NULL) {
        return false;
    }
    for (uint32_t j = 0; j < d->name_count; j++) {
        const struct mo_bytecode_register_name *r = &d->names[j];
        if (r->reg >= fn->register_count || !named(v, r->name) || r->code_start >= r->code_end ||
            r->code_end > fn->code_count || starts[r->code_start] == 0 ||
            starts[r->code_end] == 0) {
            return refuse_at(v, positions->names + 4 + j * 16, "invalid debug register range");
        }
        ranges[j] = r;
    }
    qsort(ranges, d->name_count, sizeof *ranges, compare_ranges);
    for (uint32_t j = 1; j < d->name_count; j++) {
        if (ranges[j - 1]->reg == ranges[j]->reg &&
            ranges[j - 1]->code_end > ranges[j]->code_start) {
            uint32_t row = (uint32_t)(ranges[j] - d->names);
            return refuse_at(v, positions->names + 4 + row * 16,
                             "overlapping debug register ranges");
        }
    }
    bool spec = fn->kind >= MO_FUNCTION_TEST;
    if (d->text != MO_BYTECODE_NO_INDEX && (!spec || d->text >= f->string_count)) {
        return refuse_at(v, positions->metadata, "invalid debug text index");
    }
    if (!source_span(v, d->file, d->start, d->end)) {
        return refuse_at(v, positions->metadata + 4, "invalid declaration span");
    }
    if ((d->line_count != 0 && fn->kind != MO_FUNCTION_PROMISE) ||
        (d->expectation != MO_BYTECODE_NOT_ANCHOR && fn->kind != MO_FUNCTION_ANCHOR)) {
        return refuse_at(v,
                         d->line_count != 0 && fn->kind != MO_FUNCTION_PROMISE
                             ? positions->lines
                             : positions->metadata + 20,
                         "condition lines or expectation on wrong function kind");
    }
    if (d->subject != MO_BYTECODE_NO_INDEX) {
        if ((fn->kind != MO_FUNCTION_PROMISE && fn->kind != MO_FUNCTION_ANCHOR) ||
            d->subject >= f->function_count ||
            f->functions[d->subject].kind !=
                (fn->kind == MO_FUNCTION_PROMISE ? MO_FUNCTION_FUNCTION : MO_FUNCTION_PROMISE) ||
            (fn->kind == MO_FUNCTION_PROMISE && d->line_count == 0) ||
            (fn->kind == MO_FUNCTION_ANCHOR && d->expectation == MO_BYTECODE_NOT_ANCHOR)) {
            return refuse_at(v, positions->metadata + 16,
                             "invalid spec subject or missing linked metadata");
        }
    }
    for (uint32_t j = 0; j < d->line_count; j++) {
        const struct mo_bytecode_spec_line *line = &d->lines[j];
        if (!source_span(v, line->file, line->start, line->end) || line->text >= f->string_count) {
            return refuse_at(v, positions->lines + 4 + j * 16, "invalid condition line");
        }
    }
    return true;
}

static MO_MUST_USE bool service_type(struct validation *v, uint32_t root, bool *seen,
                                     uint32_t *queue) {
    uint32_t count = 0;
    if (!seen[root]) {
        seen[root] = true;
        queue[count++] = root;
    }
    for (uint32_t i = 0; i < count; i++) {
        const struct mo_type *t = &v->file->types[queue[i]];
        if (t->kind == MO_TYPE_FUNCTION || t->kind == MO_TYPE_CAPABILITY || t->kind == MO_TYPE_ENV)
            return refuse(v, "service type contains a capability or function");
        for (uint32_t j = 0; j < t->parameter_count + t->field_count; j++) {
            uint32_t child =
                j < t->parameter_count ? t->parameters[j] : t->fields[j - t->parameter_count].type;
            if (!seen[child]) {
                seen[child] = true;
                queue[count++] = child;
            }
        }
        for (uint32_t j = 0; j < t->case_count; j++)
            for (uint32_t k = 0; k < t->cases[j].field_count; k++) {
                uint32_t child = t->cases[j].fields[k].type;
                if (!seen[child]) {
                    seen[child] = true;
                    queue[count++] = child;
                }
            }
    }
    return true;
}

static MO_MUST_USE bool service_function(const struct mo_bytecode *f, uint32_t module,
                                         uint32_t index) {
    return index < f->function_count && f->functions[index].module == module &&
           f->functions[index].kind == MO_FUNCTION_FUNCTION && !f->functions[index].spec_code;
}

static MO_MUST_USE bool update_pair(const struct mo_bytecode *f, uint32_t type, uint32_t state) {
    const struct mo_type *t = &f->types[type];
    return t->kind == MO_TYPE_TUPLE && t->parameter_count == 2 && t->parameters[0] == state;
}

static MO_MUST_USE bool services(struct validation *v) {
    const struct mo_bytecode *f = v->file;
    if (f->service_count == 0)
        return true;
    uint32_t maximum = 0;
    for (uint32_t i = 0; i < f->service_count; i++) {
        uint32_t count = f->services[i].tool_count + f->services[i].timer_count;
        if (count > maximum)
            maximum = count;
    }
    bool *seen = allocate(v, f->type_count, sizeof *seen, _Alignof(bool));
    bool *tokens_seen = allocate(v, f->string_count, sizeof *tokens_seen, _Alignof(bool));
    uint32_t *queue = allocate(v, f->type_count, sizeof *queue, _Alignof(uint32_t));
    uint32_t *modules_seen =
        allocate(v, f->service_count, sizeof *modules_seen, _Alignof(uint32_t));
    uint32_t *handlers = allocate(v, maximum, sizeof *handlers, _Alignof(uint32_t));
    uint32_t *names = allocate(v, maximum, sizeof *names, _Alignof(uint32_t));
    if (seen == NULL || tokens_seen == NULL || queue == NULL || modules_seen == NULL ||
        handlers == NULL || names == NULL)
        return false;
    memset(seen, 0, f->type_count * sizeof *seen);
    memset(tokens_seen, 0, f->string_count * sizeof *tokens_seen);
    for (uint32_t i = 0; i < f->service_count; i++) {
        v->current = (struct mo_bytecode_error_site){MO_SECTION_SERVICES, i};
        const struct mo_bytecode_service *s = &f->services[i];
        if (s->module >= f->module_count || s->state_type >= f->type_count ||
            !service_function(f, s->module, s->start) ||
            f->functions[s->start].parameter_count != 0 ||
            f->functions[s->start].result_type != s->state_type)
            return refuse(v, "invalid service module, start function or state type");
        modules_seen[i] = s->module;
        if (!named(v, s->token_env))
            return refuse(v, "invalid service token environment name");
        uint32_t token = v->info->names->canonical[s->token_env];
        if (!tokens_seen[token]) {
            const struct mo_bytecode_string *env = &f->strings[token];
            for (uint32_t j = 0; j < env->length; j++) {
                uint8_t c = env->bytes[j];
                if (!(c == '_' || (c >= 'A' && c <= 'Z') || (j != 0 && c >= '0' && c <= '9')))
                    return refuse(v, "invalid service token environment name");
            }
            tokens_seen[token] = true;
        }
        if (s->tool_count == 0 || s->timer_count > MO_LIMIT_SERVICE_HANDLERS - s->tool_count)
            return refuse(v, "invalid service handler count");
        if (!service_type(v, s->state_type, seen, queue))
            return false;
        for (uint32_t j = 0; j < s->tool_count + s->timer_count; j++) {
            bool tool = j < s->tool_count;
            uint32_t index = tool ? s->tools[j].function : s->timers[j - s->tool_count].function;
            if (!service_function(f, s->module, index))
                return refuse(v, "invalid service handler function");
            handlers[j] = index;
            const struct mo_bytecode_function *fn = &f->functions[index];
            if (fn->parameter_count < 2 || fn->parameters[0].type != s->state_type ||
                f->types[fn->parameters[1].type].kind != MO_TYPE_TIME)
                return refuse(v, "invalid service handler parameters");
            for (uint32_t k = 0; k < fn->parameter_count; k++)
                if (!service_type(v, fn->parameters[k].type, seen, queue))
                    return false;
            if (!service_type(v, fn->result_type, seen, queue))
                return false;
            if (!tool) {
                uint32_t interval = s->timers[j - s->tool_count].interval;
                if (fn->parameter_count != 2 || fn->result_type != s->state_type ||
                    !service_function(f, s->module, interval) ||
                    f->functions[interval].parameter_count != 0 ||
                    f->types[f->functions[interval].result_type].kind != MO_TYPE_DURATION)
                    return refuse(v, "invalid service timer handler or interval function");
                continue;
            }
            const struct mo_bytecode_service_tool *t = &s->tools[j];
            if (!named(v, t->name))
                return refuse(v, "invalid service tool name");
            if (t->argument_count != fn->parameter_count - 2 ||
                t->argument_name_count != t->argument_count || t->description >= f->string_count ||
                t->result_type != fn->result_type)
                return refuse(v, "service tool schema differs from handler");
            names[j] = v->info->names->canonical[t->name];
            uint32_t argument_names[MO_LIMIT_BYTECODE_PARAMETERS - 2];
            for (uint32_t k = 0; k < t->argument_count; k++) {
                if (t->argument_types[k] != fn->parameters[k + 2].type ||
                    !named(v, t->argument_names[k]))
                    return refuse(v, "service tool schema differs from handler");
                argument_names[k] = v->info->names->canonical[t->argument_names[k]];
            }
            if (!unique(argument_names, t->argument_count))
                return refuse(v, "duplicate service tool parameter name");
            const struct mo_type *result = &f->types[t->result_type];
            uint32_t success = MO_BYTECODE_NO_INDEX;
            if (result->kind == MO_TYPE_CHOICE && strncmp(result->name, "Result(", 7) == 0 &&
                result->case_count == 2 && strcmp(result->cases[0].name, "Ok") == 0 &&
                strcmp(result->cases[1].name, "Error") == 0 && result->cases[0].unnamed &&
                result->cases[1].unnamed)
                success = result->cases[0].fields[0].type;
            bool update =
                update_pair(f, t->result_type, s->state_type) ||
                (success != MO_BYTECODE_NO_INDEX && update_pair(f, success, s->state_type));
            if (t->result_type == s->state_type || success == s->state_type ||
                t->read_only == update)
                return refuse(v, "invalid service tool result classification");
        }
        if (!unique(handlers, s->tool_count + s->timer_count) || !unique(names, s->tool_count))
            return refuse(v, "duplicate service handler or tool name");
    }
    return unique(modules_seen, f->service_count) || refuse(v, "duplicate service module");
}

static MO_MUST_USE bool validate(struct validation *v) {
    const struct mo_bytecode *f = v->file;
    if (!functions(v) || !modules(v)) {
        return false;
    }
    mo_arena_free(&v->arena);
    if (!type_table(v) || !layouts(v) || !constants(v)) {
        return false;
    }
    mo_arena_free(&v->arena);
    if (!services(v))
        return false;
    mo_arena_free(&v->arena);
    for (uint32_t i = 0; i < f->function_count; i++) {
        bool ok = code_debug(v, i);
        mo_arena_free(&v->arena);
        if (!ok) {
            return false;
        }
    }
    return true;
}

bool mo_bytecode_validate(const struct mo_bytecode *file, const struct mo_bytecode_read_info *info,
                          struct mo_load_error *error) {
    MO_ASSERT(file != NULL && info != NULL && error != NULL);
    struct validation v = {.file = file, .info = info, .error = error};
    mo_arena_init(&v.arena, mo_bytecode_work_budget(info->wire_size));
    bool ok = validate(&v);
    mo_arena_free(&v.arena);
    return ok;
}
