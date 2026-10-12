#include "bytecode/signature.h"
#include "bytecode/verify.h"

#include <string.h>

static const struct {
    const char *signature, *modes;
} primitives[] = {
#define PRIM(id, module, name, signature, params, bugs) {signature, params},
#include "base/prims.def"
};
static const struct {
    const char *signature, *modes;
    bool spec;
} hosts[] = {
#define HOSTOP(id, module, name, signature, params, bugs, spec_only) {signature, params, spec_only},
#include "base/hostops.def"
};

const char *mo_verify_parameter_modes(const uint32_t *instruction) {
    uint32_t op = instruction[0] & MO_OPCODE_MASK;
    return op == MO_OP_PRIM   ? primitives[instruction[2]].modes
           : op == MO_OP_HOST ? hosts[instruction[2]].modes
                              : NULL;
}

static MO_MUST_USE uint32_t reg(const struct mo_verify_function *v, uint32_t r) {
    return v->fn->register_types[r];
}
static MO_MUST_USE bool kind(const struct mo_verify_function *v, uint32_t r, enum mo_type_kind k) {
    return v->file->types[reg(v, r)].kind == k;
}
static MO_MUST_USE bool closure(const struct mo_bytecode *f, uint32_t type, uint32_t function) {
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
static MO_MUST_USE uint32_t field(const struct mo_bytecode *f, uint32_t type, uint32_t i) {
    const struct mo_type *t = &f->types[type];
    if (t->kind == MO_TYPE_RECORD && i < t->field_count) {
        return t->fields[i].type;
    }
    if (t->kind == MO_TYPE_TUPLE && i < t->parameter_count) {
        return t->parameters[i];
    }
    if (t->kind == MO_TYPE_ENV && i < f->functions[t->function].capture_count) {
        return f->functions[t->function].capture_types[i];
    }
    return MO_NO_INDEX;
}
static MO_MUST_USE bool args(const struct mo_verify_function *v, const uint32_t *a, uint32_t count,
                             const uint32_t *types, uint32_t n) {
    if (count != n) {
        return false;
    }
    for (uint32_t i = 0; i < n; i++) {
        if (reg(v, a[i]) != types[i]) {
            return false;
        }
    }
    return true;
}

static MO_MUST_USE bool types(struct mo_verify_function *v, uint32_t pc) {
    const struct mo_bytecode *f = v->file;
    const struct mo_bytecode_function *fn = v->fn;
    const uint32_t *a = fn->code + pc + 1;
    uint32_t op = fn->code[pc] & MO_OPCODE_MASK, n = fn->code[pc] >> MO_OPCODE_VARIABLE_SHIFT;
    const struct mo_opcode_info *info = mo_opcode_find(op);
    MO_ASSERT(info != NULL);
    if (op >= MO_OP_ADD_INT && op <= MO_OP_REM_FLOAT) {
        static const enum mo_type_kind k[] = {MO_TYPE_INT, MO_TYPE_DECIMAL, MO_TYPE_FLOAT};
        enum mo_type_kind t = k[(op - MO_OP_ADD_INT) % 3];
        return kind(v, a[0], t) && kind(v, a[1], t) && kind(v, a[2], t);
    }
    if (op >= MO_OP_NEG_INT && op <= MO_OP_NEG_DURATION) {
        static const enum mo_type_kind k[] = {MO_TYPE_INT, MO_TYPE_DECIMAL, MO_TYPE_FLOAT,
                                              MO_TYPE_DURATION};
        return kind(v, a[0], k[op - MO_OP_NEG_INT]) && kind(v, a[1], k[op - MO_OP_NEG_INT]);
    }
    if (op >= MO_OP_EQ_INT && op <= MO_OP_LE_CHOICE) {
        static const enum mo_type_kind k[] = {MO_TYPE_INT,  MO_TYPE_DECIMAL,  MO_TYPE_FLOAT,
                                              MO_TYPE_BOOL, MO_TYPE_BYTE,     MO_TYPE_CHAR,
                                              MO_TYPE_TIME, MO_TYPE_DURATION, MO_TYPE_CHOICE};
        enum mo_type_kind t = k[(op - MO_OP_EQ_INT) % 9];
        return kind(v, a[0], MO_TYPE_BOOL) && kind(v, a[1], t) && reg(v, a[1]) == reg(v, a[2]) &&
               (t != MO_TYPE_CHOICE || !f->types[reg(v, a[1])].has_fields);
    }
    switch (op) {
    case MO_OP_MOVE:
    case MO_OP_DUP:
        return reg(v, a[0]) == reg(v, a[1]);
    case MO_OP_DROP:
    case MO_OP_JUMP:
    case MO_OP_FREE_TOKEN:
        return true;
    case MO_OP_CONST:
        return reg(v, a[0]) == f->constants[a[1]].type &&
               (fn->spec_code || f->types[f->constants[a[1]].type].kind != MO_TYPE_FUNCTION ||
                !f->functions[f->constants[a[1]].value.function].spec_code);
    case MO_OP_GLOBAL:
        return reg(v, a[0]) == f->globals[a[1]].type;
    case MO_OP_SET_GLOBAL:
        return fn->kind == MO_FUNCTION_INIT && f->globals[a[0]].type == reg(v, a[1]);
    case MO_OP_ADD_DURATION:
    case MO_OP_SUB_DURATION:
        return kind(v, a[0], MO_TYPE_DURATION) && kind(v, a[1], MO_TYPE_DURATION) &&
               kind(v, a[2], MO_TYPE_DURATION);
    case MO_OP_SCALE_DURATION:
        return kind(v, a[0], MO_TYPE_DURATION) && kind(v, a[1], MO_TYPE_DURATION) &&
               kind(v, a[2], MO_TYPE_INT);
    case MO_OP_ADD_TIME:
    case MO_OP_SUB_TIME:
        return kind(v, a[0], MO_TYPE_TIME) && kind(v, a[1], MO_TYPE_TIME) &&
               kind(v, a[2], MO_TYPE_DURATION);
    case MO_OP_DIFF_TIME:
        return kind(v, a[0], MO_TYPE_DURATION) && kind(v, a[1], MO_TYPE_TIME) &&
               kind(v, a[2], MO_TYPE_TIME);
    case MO_OP_CMP_FLOAT:
        return mo_bytecode_ordering(&f->types[reg(v, a[0])]) && kind(v, a[1], MO_TYPE_FLOAT) &&
               kind(v, a[2], MO_TYPE_FLOAT);
    case MO_OP_NOT:
        return kind(v, a[0], MO_TYPE_BOOL) && kind(v, a[1], MO_TYPE_BOOL);
    case MO_OP_MAKE_RANGE:
        return kind(v, a[0], MO_TYPE_RANGE) && kind(v, a[1], MO_TYPE_INT) &&
               kind(v, a[2], MO_TYPE_INT);
    case MO_OP_RANGE_START:
    case MO_OP_RANGE_END:
        return kind(v, a[0], MO_TYPE_INT) && kind(v, a[1], MO_TYPE_RANGE);
    case MO_OP_BRANCH:
        return kind(v, a[0], MO_TYPE_BOOL);
    case MO_OP_SWITCH:
        return kind(v, a[0], MO_TYPE_CHOICE) && n == f->types[reg(v, a[0])].case_count;
    case MO_OP_CALL:
    case MO_OP_TAIL_CALL: {
        bool tail = op == MO_OP_TAIL_CALL;
        const struct mo_bytecode_function *target = &f->functions[a[tail ? 0 : 1]];
        if (target->kind == MO_FUNCTION_CLOSURE || target->kind == MO_FUNCTION_INIT ||
            (!fn->spec_code && target->spec_code) || n != target->parameter_count ||
            (tail ? fn->result_type : reg(v, a[0])) != target->result_type) {
            return false;
        }
        for (uint32_t i = 0; i < n; i++) {
            if (reg(v, a[info->fixed_count + i]) != target->parameters[i].type) {
                return false;
            }
        }
        return true;
    }
    case MO_OP_CALL_CLOSURE:
    case MO_OP_TAIL_CALL_CLOSURE: {
        bool tail = op == MO_OP_TAIL_CALL_CLOSURE;
        const struct mo_type *t = &f->types[reg(v, a[tail ? 0 : 1])];
        return t->kind == MO_TYPE_FUNCTION &&
               (tail ? fn->result_type : reg(v, a[0])) == t->result &&
               args(v, a + info->fixed_count, n, t->parameters, t->parameter_count);
    }
    case MO_OP_RET:
        return reg(v, a[0]) == fn->result_type;
    case MO_OP_FAIL:
        return fn->spec_code && kind(v, a[0], MO_TYPE_TEXT);
    case MO_OP_STOP:
        return kind(v, a[0], MO_TYPE_TEXT);
    case MO_OP_CONSTRUCT:
    case MO_OP_REUSE: {
        uint32_t l = a[op == MO_OP_REUSE ? 2 : 1];
        const struct mo_layout *layout = &f->layouts[l];
        return layout->kind != MO_LAYOUT_CLOSURE && reg(v, a[0]) == layout->type &&
               args(v, a + info->fixed_count, n, layout->fields, layout->field_count);
    }
    case MO_OP_MAKE_CASE: {
        const struct mo_type *t = &f->types[a[1]];
        if (t->kind != MO_TYPE_CHOICE || a[2] >= t->case_count || reg(v, a[0]) != a[1]) {
            return false;
        }
        const struct mo_type_case *c = &t->cases[a[2]];
        if (n != c->field_count) {
            return false;
        }
        for (uint32_t i = 0; i < n; i++) {
            if (reg(v, a[3 + i]) != c->fields[i].type) {
                return false;
            }
        }
        return true;
    }
    case MO_OP_FIELD:
        return reg(v, a[0]) == field(f, reg(v, a[1]), a[2]);
    case MO_OP_CASE_FIELD: {
        const struct mo_type *t = &f->types[reg(v, a[1])];
        return t->kind == MO_TYPE_CHOICE && a[2] < t->case_count &&
               a[3] < t->cases[a[2]].field_count &&
               reg(v, a[0]) == t->cases[a[2]].fields[a[3]].type;
    }
    case MO_OP_SET_FIELD:
        return reg(v, a[0]) == reg(v, a[1]) &&
               (kind(v, a[1], MO_TYPE_RECORD) || kind(v, a[1], MO_TYPE_TUPLE)) &&
               reg(v, a[3]) == field(f, reg(v, a[1]), a[2]);
    case MO_OP_MAKE_CLOSURE:
        return closure(f, reg(v, a[0]), a[1]) && (fn->spec_code || !f->functions[a[1]].spec_code) &&
               args(v, a + 2, n, f->functions[a[1]].capture_types,
                    f->functions[a[1]].capture_count);
    case MO_OP_EQUAL:
        return kind(v, a[0], MO_TYPE_BOOL) && reg(v, a[2]) == a[1] && reg(v, a[3]) == a[1] &&
               (v->abilities[a[1]] & MO_ABILITY_EQUAL) != 0;
    case MO_OP_PRIM: {
        const struct mo_type *t = &f->types[a[2]];
        return t->kind == MO_TYPE_FUNCTION && reg(v, a[0]) == t->result &&
               args(v, a + 3, n, t->parameters, t->parameter_count) &&
               n == strlen(primitives[a[1]].modes) &&
               mo_bytecode_signature(v, primitives[a[1]].signature, t->parameters, n, t->result);
    }
    case MO_OP_HOST: {
        if (n != strlen(hosts[a[1]].modes) || (hosts[a[1]].spec && !fn->spec_code)) {
            return false;
        }
        uint32_t parameters[MO_LIMIT_BYTECODE_PARAMETERS];
        for (uint32_t i = 0; i < n; i++) {
            parameters[i] = reg(v, a[2 + i]);
        }
        return mo_bytecode_signature(v, hosts[a[1]].signature, parameters, n, reg(v, a[0]));
    }
    case MO_OP_RESET:
        return kind(v, a[1], MO_TYPE_RECORD) || kind(v, a[1], MO_TYPE_TUPLE) ||
               kind(v, a[1], MO_TYPE_CHOICE);
    default:
        MO_UNREACHABLE();
    }
}

bool mo_verify_instruction(struct mo_verify_function *v, uint32_t pc) {
    if (types(v, pc)) {
        return true;
    }
    *v->error =
        (struct mo_load_error){v->base + pc * 4, "invalid instruction types or function kind"};
    return false;
}
