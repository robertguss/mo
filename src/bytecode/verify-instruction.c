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
bool mo_verify_closure_signature(const struct mo_bytecode *f, uint32_t type, uint32_t function) {
    MO_ASSERT(type < f->type_count && function < f->function_count);
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

/* Each refusal names the failed contract, not a catch-all that can hide a different failure
 * in a one-change regression fixture. Structural decoding has already bounded all operands. */
#define REQUIRE(condition, reason)                                                                 \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            *v->error = (struct mo_load_error){v->base + pc * 4, reason, MO_BYTECODE_INVALID};     \
            return false;                                                                          \
        }                                                                                          \
    } while (0)

bool mo_verify_instruction(struct mo_verify_function *v, uint32_t pc) {
    const struct mo_bytecode *f = v->file;
    const struct mo_bytecode_function *fn = v->fn;
    const uint32_t *a = fn->code + pc + 1;
    uint32_t op = fn->code[pc] & MO_OPCODE_MASK, n = fn->code[pc] >> MO_OPCODE_VARIABLE_SHIFT;
    const struct mo_opcode_info *info = mo_opcode_find(op);
    MO_ASSERT(info != NULL);
    if (op >= MO_OP_ADD_INT && op <= MO_OP_REM_FLOAT) {
        static const enum mo_type_kind k[] = {MO_TYPE_INT, MO_TYPE_DECIMAL, MO_TYPE_FLOAT};
        enum mo_type_kind t = k[(op - MO_OP_ADD_INT) % 3];
        REQUIRE(kind(v, a[0], t), "arithmetic result type");
        REQUIRE(kind(v, a[1], t) && kind(v, a[2], t), "arithmetic argument type");
        return true;
    }
    if (op >= MO_OP_NEG_INT && op <= MO_OP_NEG_DURATION) {
        static const enum mo_type_kind k[] = {MO_TYPE_INT, MO_TYPE_DECIMAL, MO_TYPE_FLOAT,
                                              MO_TYPE_DURATION};
        REQUIRE(kind(v, a[0], k[op - MO_OP_NEG_INT]), "negation result type");
        REQUIRE(kind(v, a[1], k[op - MO_OP_NEG_INT]), "negation argument type");
        return true;
    }
    if (op >= MO_OP_EQ_INT && op <= MO_OP_LE_CHOICE) {
        static const enum mo_type_kind k[] = {MO_TYPE_INT,  MO_TYPE_DECIMAL,  MO_TYPE_FLOAT,
                                              MO_TYPE_BOOL, MO_TYPE_BYTE,     MO_TYPE_CHAR,
                                              MO_TYPE_TIME, MO_TYPE_DURATION, MO_TYPE_CHOICE};
        enum mo_type_kind t = k[(op - MO_OP_EQ_INT) % 9];
        REQUIRE(kind(v, a[0], MO_TYPE_BOOL), "comparison result type");
        REQUIRE(kind(v, a[1], t) && reg(v, a[1]) == reg(v, a[2]), "comparison argument type");
        REQUIRE(t != MO_TYPE_CHOICE || !f->types[reg(v, a[1])].has_fields,
                "choice comparison has fields");
        return true;
    }
    switch (op) {
    case MO_OP_MOVE:
    case MO_OP_DUP:
        REQUIRE(reg(v, a[0]) == reg(v, a[1]), "move/dup type mismatch");
        break;
    case MO_OP_DROP:
    case MO_OP_CLEAR:
    case MO_OP_JUMP:
    case MO_OP_FREE_TOKEN:
        return true;
    case MO_OP_CONST:
        MO_ASSERT(a[1] < f->constant_count);
        REQUIRE(reg(v, a[0]) == f->constants[a[1]].type, "constant result type");
        REQUIRE(fn->spec_code || f->types[f->constants[a[1]].type].kind != MO_TYPE_FUNCTION ||
                    !f->functions[f->constants[a[1]].value.function].spec_code,
                "spec closure constant in non-spec code");
        break;
    case MO_OP_GLOBAL:
        MO_ASSERT(a[1] < f->global_count);
        REQUIRE(reg(v, a[0]) == f->globals[a[1]].type, "global result type");
        break;
    case MO_OP_SET_GLOBAL:
        MO_ASSERT(a[0] < f->global_count);
        REQUIRE(fn->kind == MO_FUNCTION_INIT, "global store outside init");
        REQUIRE(f->globals[a[0]].type == reg(v, a[1]), "global store type");
        break;
    case MO_OP_ADD_DURATION:
    case MO_OP_SUB_DURATION:
        REQUIRE(kind(v, a[0], MO_TYPE_DURATION), "duration arithmetic result type");
        REQUIRE(kind(v, a[1], MO_TYPE_DURATION) && kind(v, a[2], MO_TYPE_DURATION),
                "duration arithmetic argument type");
        break;
    case MO_OP_SCALE_DURATION:
        REQUIRE(kind(v, a[0], MO_TYPE_DURATION), "duration scale result type");
        REQUIRE(kind(v, a[1], MO_TYPE_DURATION) && kind(v, a[2], MO_TYPE_INT),
                "duration scale argument type");
        break;
    case MO_OP_ADD_TIME:
    case MO_OP_SUB_TIME:
        REQUIRE(kind(v, a[0], MO_TYPE_TIME), "time arithmetic result type");
        REQUIRE(kind(v, a[1], MO_TYPE_TIME) && kind(v, a[2], MO_TYPE_DURATION),
                "time arithmetic argument type");
        break;
    case MO_OP_DIFF_TIME:
        REQUIRE(kind(v, a[0], MO_TYPE_DURATION), "time difference result type");
        REQUIRE(kind(v, a[1], MO_TYPE_TIME) && kind(v, a[2], MO_TYPE_TIME),
                "time difference argument type");
        break;
    case MO_OP_CMP_FLOAT:
        REQUIRE(mo_bytecode_ordering(&f->types[reg(v, a[0])]), "float comparison needs Ordering");
        REQUIRE(kind(v, a[1], MO_TYPE_FLOAT) && kind(v, a[2], MO_TYPE_FLOAT),
                "float comparison argument type");
        break;
    case MO_OP_NOT:
        REQUIRE(kind(v, a[0], MO_TYPE_BOOL), "not result type");
        REQUIRE(kind(v, a[1], MO_TYPE_BOOL), "not argument type");
        break;
    case MO_OP_MAKE_RANGE:
        REQUIRE(kind(v, a[0], MO_TYPE_RANGE), "range result type");
        REQUIRE(kind(v, a[1], MO_TYPE_INT) && kind(v, a[2], MO_TYPE_INT), "range argument type");
        break;
    case MO_OP_RANGE_START:
    case MO_OP_RANGE_END:
        REQUIRE(kind(v, a[0], MO_TYPE_INT), "range projection result type");
        REQUIRE(kind(v, a[1], MO_TYPE_RANGE), "range projection argument type");
        break;
    case MO_OP_BRANCH:
        REQUIRE(kind(v, a[0], MO_TYPE_BOOL), "branch needs Bool");
        break;
    case MO_OP_SWITCH:
        REQUIRE(kind(v, a[0], MO_TYPE_CHOICE), "switch needs choice");
        REQUIRE(n == f->types[reg(v, a[0])].case_count, "switch target count");
        break;
    case MO_OP_CALL:
    case MO_OP_TAIL_CALL: {
        bool tail = op == MO_OP_TAIL_CALL;
        MO_ASSERT(a[tail ? 0 : 1] < f->function_count);
        const struct mo_bytecode_function *target = &f->functions[a[tail ? 0 : 1]];
        REQUIRE(target->kind != MO_FUNCTION_CLOSURE && target->kind != MO_FUNCTION_INIT,
                "direct call target kind");
        REQUIRE(fn->spec_code || !target->spec_code, "direct call to spec code");
        REQUIRE(n == target->parameter_count, "direct call argument count");
        REQUIRE((tail ? fn->result_type : reg(v, a[0])) == target->result_type,
                "direct call result type");
        for (uint32_t i = 0; i < n; i++) {
            REQUIRE(reg(v, a[info->fixed_count + i]) == target->parameters[i].type,
                    "direct call argument type");
        }
        return true;
    }
    case MO_OP_CALL_CLOSURE:
    case MO_OP_TAIL_CALL_CLOSURE: {
        bool tail = op == MO_OP_TAIL_CALL_CLOSURE;
        const struct mo_type *t = &f->types[reg(v, a[tail ? 0 : 1])];
        REQUIRE(t->kind == MO_TYPE_FUNCTION, "closure call needs function");
        REQUIRE((tail ? fn->result_type : reg(v, a[0])) == t->result, "closure call result type");
        REQUIRE(n == t->parameter_count, "closure call argument count");
        REQUIRE(args(v, a + info->fixed_count, n, t->parameters, t->parameter_count),
                "closure call argument type");
        break;
    }
    case MO_OP_RET:
        REQUIRE(reg(v, a[0]) == fn->result_type, "return type");
        break;
    case MO_OP_FAIL:
        REQUIRE(fn->spec_code, "fail outside spec code");
        REQUIRE(kind(v, a[0], MO_TYPE_TEXT), "fail needs Text");
        break;
    case MO_OP_STOP:
        REQUIRE(kind(v, a[0], MO_TYPE_TEXT), "stop needs Text");
        break;
    case MO_OP_CONSTRUCT:
    case MO_OP_REUSE: {
        uint32_t l = a[op == MO_OP_REUSE ? 2 : 1];
        MO_ASSERT(l < f->layout_count);
        const struct mo_layout *layout = &f->layouts[l];
        REQUIRE(layout->kind != MO_LAYOUT_CLOSURE, "construct/reuse closure layout");
        REQUIRE(reg(v, a[0]) == layout->type, "construct/reuse result type");
        REQUIRE(n == layout->field_count, "construct/reuse field count");
        REQUIRE(args(v, a + info->fixed_count, n, layout->fields, layout->field_count),
                "construct/reuse field type");
        break;
    }
    case MO_OP_MAKE_CASE: {
        MO_ASSERT(a[1] < f->type_count);
        const struct mo_type *t = &f->types[a[1]];
        REQUIRE(t->kind == MO_TYPE_CHOICE, "make_case needs choice");
        REQUIRE(a[2] < t->case_count, "make_case case index");
        REQUIRE(reg(v, a[0]) == a[1], "make_case result type");
        const struct mo_type_case *c = &t->cases[a[2]];
        REQUIRE(n == c->field_count, "make_case field count");
        for (uint32_t i = 0; i < n; i++) {
            REQUIRE(reg(v, a[3 + i]) == c->fields[i].type, "make_case field type");
        }
        return true;
    }
    case MO_OP_FIELD:
        REQUIRE(field(f, reg(v, a[1]), a[2]) != MO_NO_INDEX, "field source or index");
        REQUIRE(reg(v, a[0]) == field(f, reg(v, a[1]), a[2]), "field result type");
        break;
    case MO_OP_CASE_FIELD: {
        const struct mo_type *t = &f->types[reg(v, a[1])];
        REQUIRE(t->kind == MO_TYPE_CHOICE, "case_field needs choice");
        REQUIRE(a[2] < t->case_count, "case_field case index");
        REQUIRE(a[3] < t->cases[a[2]].field_count, "case_field field index");
        REQUIRE(reg(v, a[0]) == t->cases[a[2]].fields[a[3]].type, "case_field result type");
        break;
    }
    case MO_OP_SET_FIELD:
        REQUIRE(kind(v, a[1], MO_TYPE_RECORD) || kind(v, a[1], MO_TYPE_TUPLE),
                "set_field needs record or tuple");
        REQUIRE(reg(v, a[0]) == reg(v, a[1]), "set_field result type");
        REQUIRE(field(f, reg(v, a[1]), a[2]) != MO_NO_INDEX, "set_field field index");
        REQUIRE(reg(v, a[3]) == field(f, reg(v, a[1]), a[2]), "set_field value type");
        break;
    case MO_OP_MAKE_CLOSURE:
        REQUIRE(mo_verify_closure_signature(f, reg(v, a[0]), a[1]), "closure signature");
        REQUIRE(fn->spec_code || !f->functions[a[1]].spec_code, "spec closure in non-spec code");
        REQUIRE(
            args(v, a + 2, n, f->functions[a[1]].capture_types, f->functions[a[1]].capture_count),
            "closure capture types or count");
        break;
    case MO_OP_EQUAL:
        REQUIRE(kind(v, a[0], MO_TYPE_BOOL), "equal result type");
        REQUIRE(reg(v, a[2]) == a[1] && reg(v, a[3]) == a[1], "equal argument type");
        REQUIRE((v->abilities[a[1]] & MO_ABILITY_EQUAL) != 0, "equal lacks Equal");
        break;
    case MO_OP_PRIM: {
        MO_ASSERT(a[1] < sizeof primitives / sizeof primitives[0]);
        const struct mo_type *t = &f->types[a[2]];
        REQUIRE(t->kind == MO_TYPE_FUNCTION, "primitive needs function type");
        REQUIRE(reg(v, a[0]) == t->result, "primitive result type");
        REQUIRE(args(v, a + 3, n, t->parameters, t->parameter_count),
                "primitive argument types or count");
        REQUIRE(n == strlen(primitives[a[1]].modes), "primitive arity");
        const char *reason = "primitive signature or constraint";
        REQUIRE(mo_bytecode_signature(v, primitives[a[1]].signature, t->parameters, n, t->result,
                                      &reason),
                reason);
        break;
    }
    case MO_OP_HOST: {
        MO_ASSERT(a[1] < sizeof hosts / sizeof hosts[0]);
        REQUIRE(n == strlen(hosts[a[1]].modes), "host arity");
        REQUIRE(!hosts[a[1]].spec || fn->spec_code, "spec host in non-spec code");
        uint32_t parameters[MO_LIMIT_BYTECODE_PARAMETERS];
        for (uint32_t i = 0; i < n; i++) {
            parameters[i] = reg(v, a[2 + i]);
        }
        const char *reason = "host signature or capability";
        REQUIRE(
            mo_bytecode_signature(v, hosts[a[1]].signature, parameters, n, reg(v, a[0]), &reason),
            reason);
        break;
    }
    case MO_OP_SIM: {
        REQUIRE(a[1] < MO_SIM_COUNT, "unknown simulation operation");
        if (a[1] == MO_SIM_START) {
            REQUIRE(fn->spec_code, "simulation start in non-spec code");
            REQUIRE(a[2] < f->service_count && a[3] == 0, "simulation start descriptor");
            REQUIRE(n == 1 && kind(v, a[4], MO_TYPE_TIME), "simulation start argument");
            REQUIRE(kind(v, a[0], MO_TYPE_CAPABILITY) &&
                        f->types[reg(v, a[0])].capability == MO_CAP_SIMULATION,
                    "simulation start result");
            break;
        }
        REQUIRE(n != 0 && kind(v, a[4], MO_TYPE_CAPABILITY) &&
                    f->types[reg(v, a[4])].capability == MO_CAP_SIMULATION,
                "simulation handle argument");
        if (a[1] == MO_SIM_SEND) {
            REQUIRE(a[2] < f->service_count, "simulation send descriptor");
            const struct mo_bytecode_service *service = &f->services[a[2]];
            REQUIRE(a[3] < service->tool_count, "simulation send tool");
            const struct mo_bytecode_service_tool *tool = &service->tools[a[3]];
            REQUIRE(args(v, a + 5, n - 1, tool->argument_types, tool->argument_count),
                    "simulation send argument types or count");
            const char *reason = "simulation send result";
            REQUIRE(mo_bytecode_signature(v, "fn(T) : Result(T, SimulationError)",
                                          &tool->result_type, 1, reg(v, a[0]), &reason),
                    reason);
        } else {
            REQUIRE(a[2] == 0 && a[3] == 0, "simulation control has selector indices");
            REQUIRE(kind(v, a[0], MO_TYPE_UNIT), "simulation control result");
            REQUIRE(a[1] == MO_SIM_ADVANCE ? n == 2 && kind(v, a[5], MO_TYPE_DURATION) : n == 1,
                    "simulation control arguments");
        }
        break;
    }
    case MO_OP_RESET:
        REQUIRE(kind(v, a[1], MO_TYPE_RECORD) || kind(v, a[1], MO_TYPE_TUPLE) ||
                    kind(v, a[1], MO_TYPE_CHOICE),
                "reset source type");
        break;
    default:
        MO_UNREACHABLE();
    }
    return true;
}
#undef REQUIRE
