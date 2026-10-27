/* CBMC proofs of the bytecode verifier's transfer functions, src/bytecode/verify.c and
 * src/bytecode/verify-instruction.c.
 *
 * The verifier runs on a file the reader decoded and the validator accepted. Its work for one
 * function is three passes: graph builds the blocks and their edges, cases tracks which case
 * each choice register holds, and ownership walks the blocks depth first, applying each
 * instruction's ownership transfer (step) to the registers' states. mo_verify_instruction, the
 * type check of one instruction, runs inside graph. The proofs here are of the transfer
 * functions: mo_verify_instruction and step for any one instruction of any opcode, and the
 * layout lookup both use. The three passes that drive them are not proved (cbmc/README.md).
 *
 * Every file here is one the validator accepts: each index the verifier follows is inside its
 * table. The tables are small (a few types, functions, layouts, constants and globals, and short
 * code), and every value in them is any value the validator allows. */
#include "bytecode/verify.c"
#include "bytecode/verify-instruction.c"

#include <stdlib.h>

/* The checks are for the verifier's code: the harnesses, models and specifications below build
 * and inspect its inputs and need none, and leaving them out keeps the proofs small. */
#pragma CPROVER check push
#pragma CPROVER check disable "bounds"
#pragma CPROVER check disable "pointer"
#pragma CPROVER check disable "pointer-overflow"
#pragma CPROVER check disable "pointer-primitive"
#pragma CPROVER check disable "signed-overflow"
#pragma CPROVER check disable "conversion"
#pragma CPROVER check disable "undefined-shift"
#pragma CPROVER check disable "div-by-zero"

/* The largest table, type component (parameters, fields, cases) and variable operand count
 * the files here hold, and the most code words a function has. */
#ifndef MO_CBMC_ENTRIES
#define MO_CBMC_ENTRIES 2
#endif
#ifndef MO_CBMC_CODE
#define MO_CBMC_CODE 8
#endif
#define ENTRIES MO_CBMC_ENTRIES
#define TYPES (ENTRIES + 1)
#define CODE MO_CBMC_CODE
#define REGISTERS 3
#define VARIABLE (ENTRIES + 1) /* one more than any table, so every count check can fail */

size_t nondet_size(void);
bool nondet_bool(void);
uint32_t nondet_u32(void);
uint8_t nondet_u8(void);
enum mo_case_shape nondet_shape(void);

/* ---- Neighbours ---------------------------------------------------------------------------- */

/* An MO_ASSERT that fails is a failed proof, not the end of a path. */
_Noreturn void mo_assert_failed(const char *expression, const char *file, int line) {
    (void)expression;
    (void)file;
    (void)line;
    __CPROVER_assert(0, "MO_ASSERT holds");
    __CPROVER_assume(0);
    abort();
}

/* Every harness ends here, so prove.sh --reach can check that some input gets to its end. */
void reached_end(void) {
}

/* A new object of n entries of type T. Allocating it by its type, not as bytes, keeps CBMC from
 * treating every field read as a byte extraction. */
#define NEW(T, n)                                                                                  \
    __extension__({                                                                                \
        T *new_ = malloc((size_t)(n) * sizeof(T));                                                 \
        __CPROVER_assume(new_ != NULL);                                                            \
        new_;                                                                                      \
    })

/* A table of n entries of type T, for n up to most: absent when empty, otherwise an object of
 * exactly n entries, so a read past the table is a bounds failure. Each size gets its own
 * allocation of a constant size, which CBMC handles far better than an object whose size or
 * starting offset it must solve for. */
#define TABLE(T, most, n)                                                                          \
    __extension__({                                                                                \
        T *table_ = NULL;                                                                          \
        for (uint32_t size_ = 1; size_ <= (most); size_++) {                                       \
            if ((n) == size_) {                                                                    \
                table_ = NEW(T, size_);                                                            \
            }                                                                                      \
        }                                                                                          \
        table_;                                                                                    \
    })

/* A flow's arena; the transfer functions allocate nothing from it. */
void mo_arena_init(struct mo_arena *arena, size_t limit) {
    *arena = (struct mo_arena){.limit = limit};
}

/* The runtime type table answers for the file's types (the loader builds it from them). */
enum mo_case_shape mo_case_shape(const struct mo_types *types, uint32_t type, uint32_t which) {
    __CPROVER_assert(type < types->count, "case shape of a decoded type");
    __CPROVER_assert(which < types->items[type].case_count, "case shape of a decoded case");
    return nondet_shape();
}

/* Matching a library signature (src/bytecode/signature.c) is not proved here: it may accept or
 * refuse any types, but it must be given decoded ones. */
bool mo_bytecode_signature(const struct mo_verify_function *v, const char *signature,
                           const uint32_t *parameters, uint32_t count, uint32_t result,
                           const char **reason) {
    __CPROVER_assert(signature != NULL && reason != NULL && *reason != NULL,
                     "signature: a template and a reason");
    __CPROVER_assert(count == 0 || __CPROVER_r_ok(parameters, count * sizeof *parameters),
                     "signature: readable parameter types");
    for (uint32_t i = 0; i < count; i++) {
        __CPROVER_assert(parameters[i] < v->file->type_count, "signature: decoded parameter types");
    }
    __CPROVER_assert(result < v->file->type_count, "signature: a decoded result type");
    return nondet_bool();
}

bool mo_bytecode_ordering(const struct mo_type *type) {
    __CPROVER_assert(__CPROVER_r_ok(type, sizeof *type), "ordering: a decoded type");
    return nondet_bool();
}

/* ---- Validated files ----------------------------------------------------------------------- */

static uint32_t any_count(uint32_t most) {
    uint32_t n = nondet_u32();
    __CPROVER_assume(n <= most);
    return n;
}

static uint32_t any_below(uint32_t bound) {
    uint32_t n = nondet_u32();
    __CPROVER_assume(n < bound);
    return n;
}

/* Any error: none yet, or an earlier one. Its reason, when it has one, is a real text: a
 * nondeterministic pointer could point anywhere, which CBMC handles very slowly. */
static struct mo_load_error any_error(void) {
    return (struct mo_load_error){nondet_u32(), nondet_bool() ? "an earlier reason" : NULL,
                                  (enum mo_bytecode_error_kind)any_count(MO_BYTECODE_NO_MEMORY)};
}

/* Any n indices below bound, in a table that ends where its object ends. */
static uint32_t *any_indices(uint32_t n, uint32_t bound) {
    uint32_t *p = TABLE(uint32_t, ENTRIES, n);
    for (uint32_t i = 0; i < n; i++) {
        p[i] = any_below(bound);
    }
    return p;
}

static struct mo_type_field *any_fields(uint32_t n) {
    struct mo_type_field *p = TABLE(struct mo_type_field, ENTRIES, n);
    for (uint32_t i = 0; i < n; i++) {
        p[i] = (struct mo_type_field){"field", any_below(TYPES)};
    }
    return p;
}

/* Any type the validator accepts: every index it holds is inside its table. Without contents,
 * its parameters, fields and cases are counted but absent, for proofs of passes that never read
 * them (a pass that did would fail its proof on the null table). */
static void any_type(struct mo_type *t, uint32_t functions, bool contents) {
    *t = (struct mo_type){.kind = (enum mo_type_kind)any_count(MO_TYPE_CAPABILITY),
                          .capability = (enum mo_capability)any_count(MO_CAP_RANDOM),
                          .name = "type",
                          .defined = nondet_bool(),
                          .reference = nondet_bool(),
                          .has_fields = nondet_bool()};
    t->parameter_count = any_count(ENTRIES);
    t->field_count = any_count(ENTRIES);
    t->case_count = any_count(ENTRIES);
    t->result = any_below(TYPES);
    t->function = any_below(functions);
    if (!contents) {
        return;
    }
    t->parameters = any_indices(t->parameter_count, TYPES);
    t->fields = any_fields(t->field_count);
    struct mo_type_case *cases = TABLE(struct mo_type_case, ENTRIES, t->case_count);
    for (uint32_t j = 0; j < t->case_count; j++) {
        cases[j].name = "case";
        cases[j].unnamed = nondet_bool();
        cases[j].field_count = any_count(ENTRIES);
        cases[j].fields = any_fields(cases[j].field_count);
    }
    t->cases = cases;
}

/* Any function the validator accepts, without code: counts within their limits, register and
 * capture types inside the type table, each parameter's type its register's, borrowing only in
 * ordinary functions, and a closure's env register an env of its own. */
static void any_function(struct mo_bytecode_function *fn, uint32_t index,
                         const struct mo_type *types) {
    *fn =
        (struct mo_bytecode_function){.name = nondet_u32(),
                                      .module = nondet_u32(),
                                      .kind = (enum mo_function_kind)any_count(MO_FUNCTION_ANCHOR),
                                      .spec_code = nondet_bool()};
    __CPROVER_assume(fn->kind == MO_FUNCTION_CLOSURE ||
                     fn->spec_code == (fn->kind >= MO_FUNCTION_TEST));
    bool closure = fn->kind == MO_FUNCTION_CLOSURE;
    fn->register_count = any_count(REGISTERS);
    fn->parameter_count = any_count(ENTRIES);
    __CPROVER_assume(fn->parameter_count + (closure ? 1U : 0U) <= fn->register_count);
    fn->register_types = TABLE(uint32_t, REGISTERS, fn->register_count);
    for (uint32_t r = 0; r < fn->register_count; r++) {
        ((uint32_t *)fn->register_types)[r] = any_below(TYPES);
    }
    struct mo_parameter *parameters = TABLE(struct mo_parameter, ENTRIES, fn->parameter_count);
    for (uint32_t j = 0; j < fn->parameter_count; j++) {
        parameters[j].type = fn->register_types[j];
        parameters[j].ownership = nondet_bool() ? MO_PARAMETER_OWNED : MO_PARAMETER_BORROWED;
        __CPROVER_assume(fn->kind == MO_FUNCTION_FUNCTION ||
                         parameters[j].ownership == MO_PARAMETER_OWNED);
    }
    fn->parameters = parameters;
    fn->result_type = any_below(TYPES);
    fn->capture_count = closure ? any_count(ENTRIES) : 0;
    fn->capture_types = any_indices(fn->capture_count, TYPES);
    if (closure) {
        const struct mo_type *env = &types[fn->register_types[fn->parameter_count]];
        __CPROVER_assume(env->kind == MO_TYPE_ENV && env->function == index);
    }
    fn->code_count = 0;
    fn->code = NULL;
}

/* A file the validator accepted, the verifier's tables over it, and the function verified. */
struct world {
    struct mo_bytecode *file;
    struct mo_types *types;
    uint8_t *abilities;
    struct layout_key *keys;
    struct mo_verify_program *program;
    struct mo_bytecode_function *fn;
    uint32_t index;
};

/* Any world; contents says whether its types and layouts have their tables. */
static struct world any_world(bool contents) {
    struct world w;
    struct mo_bytecode *f = NEW(struct mo_bytecode, 1);
    f->function_count = ENTRIES;
    struct mo_type *types = NEW(struct mo_type, TYPES);
    for (uint32_t i = 0; i < TYPES; i++) {
        any_type(&types[i], f->function_count, contents);
    }
    f->type_count = TYPES;
    f->types = types;
    struct mo_bytecode_function *functions = NEW(struct mo_bytecode_function, ENTRIES);
    for (uint32_t i = 0; i < f->function_count; i++) {
        any_function(&functions[i], i, types);
    }
    f->functions = functions;
    for (uint32_t i = 0; i < TYPES; i++) {
        __CPROVER_assume(types[i].kind != MO_TYPE_ENV ||
                         functions[types[i].function].kind == MO_FUNCTION_CLOSURE);
    }

    /* Layouts: a record, tuple or case layout for a type of that kind, a case inside it, and a
     * closure layout for a closure; fields inside the type table. The verifier's keys for them
     * name each layout's type and case or function. */
    f->layout_count = any_count(ENTRIES);
    struct mo_layout *layouts = TABLE(struct mo_layout, ENTRIES, f->layout_count);
    w.keys = TABLE(struct layout_key, ENTRIES, f->layout_count);
    for (uint32_t i = 0; i < f->layout_count; i++) {
        struct mo_layout *l = &layouts[i];
        *l = (struct mo_layout){.kind = (enum mo_layout_kind)any_count(MO_LAYOUT_CLOSURE),
                                .case_index = nondet_u32(),
                                .function = nondet_u32()};
        l->type = any_below(TYPES);
        l->field_count = any_count(ENTRIES);
        l->fields = contents ? any_indices(l->field_count, TYPES) : NULL;
        const struct mo_type *t = &types[l->type];
        __CPROVER_assume(l->kind != MO_LAYOUT_RECORD || t->kind == MO_TYPE_RECORD);
        __CPROVER_assume(l->kind != MO_LAYOUT_TUPLE || t->kind == MO_TYPE_TUPLE);
        __CPROVER_assume(l->kind != MO_LAYOUT_CASE ||
                         (t->kind == MO_TYPE_CHOICE && l->case_index < t->case_count));
        __CPROVER_assume(l->kind != MO_LAYOUT_CLOSURE || l->function < f->function_count);
        w.keys[i] = (struct layout_key){l->type,
                                        l->kind == MO_LAYOUT_CASE      ? l->case_index
                                        : l->kind == MO_LAYOUT_CLOSURE ? l->function
                                                                       : MO_NO_INDEX,
                                        i};
    }
    f->layouts = layouts;

    f->constant_count = any_count(ENTRIES);
    struct mo_bytecode_constant *constants =
        TABLE(struct mo_bytecode_constant, ENTRIES, f->constant_count);
    for (uint32_t i = 0; i < f->constant_count; i++) {
        constants[i] = (struct mo_bytecode_constant){.type = any_below(TYPES)};
        constants[i].value.function = nondet_u32();
        __CPROVER_assume(types[constants[i].type].kind != MO_TYPE_FUNCTION ||
                         constants[i].value.function < f->function_count);
    }
    f->constants = constants;
    f->global_count = any_count(ENTRIES);
    struct mo_bytecode_global *globals = TABLE(struct mo_bytecode_global, ENTRIES, f->global_count);
    for (uint32_t i = 0; i < f->global_count; i++) {
        globals[i].type = any_below(TYPES);
    }
    f->globals = globals;

    w.file = f;
    w.types = NEW(struct mo_types, 1);
    w.types->items = types;
    w.types->count = TYPES;
    w.abilities = NEW(uint8_t, TYPES);
    for (uint32_t i = 0; i < TYPES; i++) {
        w.abilities[i] = nondet_u8();
    }
    /* Initialization ranks: an init function has one of at least 1. */
    w.program = NEW(struct mo_verify_program, 1);
    w.program->global_rank = TABLE(uint32_t, ENTRIES, f->global_count);
    w.program->init_rank = TABLE(uint32_t, ENTRIES, f->function_count);
    w.program->required = TABLE(uint32_t, ENTRIES, f->function_count);
    w.index = 0;
    __CPROVER_assume(functions[w.index].kind != MO_FUNCTION_INIT ||
                     w.program->init_rank[w.index] >= 1);
    w.fn = &functions[w.index];
    return w;
}

/* The instruction starts in the verified function's code, one bit for each word (and one for
 * its end): a bit mask is far cheaper for CBMC than a table written at unknown positions. */
static uint32_t start_mask;
_Static_assert(CODE < 32, "a start mask bit for each code word and the end");
#define starts_at(pc) (((start_mask >> (pc)) & 1U) != 0)

static uint32_t length(const struct mo_bytecode_function *fn, uint32_t pc) {
    const struct mo_opcode_info *info = mo_opcode_find(fn->code[pc] & MO_OPCODE_MASK);
    return 1U + info->fixed_count + (fn->code[pc] >> MO_OPCODE_VARIABLE_SHIFT);
}

/* An operand the validator accepts for its kind. */
static bool operand_ok(const struct world *w, enum mo_operand_kind kind, uint32_t value) {
    const struct mo_bytecode *f = w->file;
    switch (kind) {
    case MO_OPERAND_REGISTER:
        return value < w->fn->register_count;
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
        return value < w->fn->code_count && starts_at(value);
    case MO_OPERAND_FIELD:
        return value < MO_LIMIT_FIELDS;
    case MO_OPERAND_CASE:
        return value < MO_LIMIT_CHOICE_CASES;
    case MO_OPERAND_INDEX:
        return true;
    case MO_OPERAND_NONE:
        return false;
    }
    return false;
}

/* ---- Specifications ----------------------------------------------------------------------- */

/* Each primitive's and capability operation's arity: the length of its parameter modes, as
 * constants, which CBMC reads far more cheaply than it measures a string. */
static const uint8_t primitive_arity[] = {
#define PRIM(id, module, name, signature, params, bugs) sizeof(params) - 1,
#include "base/prims.def"
};
static const uint8_t host_arity[] = {
#define HOSTOP(id, module, name, signature, params, bugs, spec_only) sizeof(params) - 1,
#include "base/hostops.def"
};

/* What mo_verify_instruction establishes for its callers: each count the later passes loop
 * over matches what they index with it. */
static bool verified(const struct mo_bytecode *f, const struct mo_bytecode_function *fn,
                     uint32_t pc) {
    const uint32_t *a = fn->code + pc + 1;
    uint32_t op = fn->code[pc] & MO_OPCODE_MASK, n = fn->code[pc] >> MO_OPCODE_VARIABLE_SHIFT;
    const uint32_t *types = fn->register_types;
    switch (op) {
    case MO_OP_SWITCH:
        return n == f->types[types[a[0]]].case_count;
    case MO_OP_CALL:
        return n == f->functions[a[1]].parameter_count;
    case MO_OP_TAIL_CALL:
        return n == f->functions[a[0]].parameter_count;
    case MO_OP_CALL_CLOSURE:
        return n == f->types[types[a[1]]].parameter_count;
    case MO_OP_TAIL_CALL_CLOSURE:
        return n == f->types[types[a[0]]].parameter_count;
    case MO_OP_CONSTRUCT:
        return n == f->layouts[a[1]].field_count;
    case MO_OP_REUSE:
        return n == f->layouts[a[2]].field_count;
    case MO_OP_MAKE_CLOSURE:
        return n == f->functions[a[1]].capture_count;
    case MO_OP_PRIM:
        return n == primitive_arity[a[1]];
    case MO_OP_HOST:
        return n == host_arity[a[1]];
    case MO_OP_MAKE_CASE:
        /* A file without case tables (harness_step's) has no case field counts to match. */
        return f->types[a[1]].kind == MO_TYPE_CHOICE && a[2] < f->types[a[1]].case_count &&
               (f->types[a[1]].cases == NULL || n == f->types[a[1]].cases[a[2]].field_count);
    default:
        return true;
    }
}

/* A refusal: a reason, and a position inside the function's code (step allocates nothing). */
static bool refused(const struct flow *f) {
    const struct mo_load_error *e = f->v.error;
    return e->reason != NULL && (e->kind == MO_BYTECODE_INVALID || e->kind == MO_BYTECODE_LIMIT) &&
           e->offset >= f->v.base && e->offset - f->v.base <= 4 * f->v.fn->code_count;
}

/* The layout cases gives a reset, which the ownership pass reads; every other instruction's
 * entry may hold anything. */
static bool reset_ok(const struct flow *f, uint32_t pc) {
    uint32_t op = f->v.fn->code[pc] & MO_OPCODE_MASK;
    return op != MO_OP_RESET || f->reset[pc] < f->v.file->layout_count;
}

/* A register's ownership state: undefined, a value, owned, borrowed, moved, or a token for
 * reusing a block of one of the file's layouts. The last slot says whether an init set its
 * global. */
static bool state_ok(const struct flow *f, uint32_t s) {
    return s < TOKEN || s - TOKEN < f->v.file->layout_count;
}

static bool states_ok(const struct flow *f, const uint32_t *state) {
    for (uint32_t r = 0; r < f->v.fn->register_count; r++) {
        if (!state_ok(f, state[r])) {
            return false;
        }
    }
    return state[f->v.fn->register_count] <= 1;
}

static uint32_t any_state(const struct flow *f) {
    uint32_t s = nondet_u32();
    __CPROVER_assume(state_ok(f, s));
    return s;
}

static bool readable_state(uint32_t s) {
    return s == VALUE || s == OWNED || s == BORROWED;
}

static bool reads(enum mo_operand_access mode) {
    return mode != MO_ACCESS_NONE && mode != MO_ACCESS_WRITE && mode != MO_ACCESS_TOKEN_WRITE;
}

static bool consumes(enum mo_operand_access mode, uint32_t s) {
    return mode == MO_ACCESS_TOKEN_CONSUME || mode == MO_ACCESS_CONSUME ||
           (mode == MO_ACCESS_MOVE && s == OWNED);
}

/* The ownership rules, of an instruction at pc 0 that step accepted, with the slots' states
 * before (old) and after (state):
 * - each register it reads is readable, and each token it consumes a token, and a reuse's
 *   token is of its layout;
 * - a reference it consumes is owned, a tail call borrows only borrowed references, and no
 *   reference or token is consumed and used again by the same instruction;
 * - it writes over no owned register or token (once its moves are done), and leaves each register
 * it writes owned (a reference, borrowed from a constant or global), a value, undefined (clear),
 * the moved state (move) or a token of the instruction's layout (reset); each owned reference or
 * token it consumes and does not write is moved, and every other register is as it was;
 * - an init reads only globals set before it, sets its own once, and returns only after setting
 *   it; and nothing returns or tail calls with an owned register or a token left. */
static bool step_ok(const struct flow *f, const uint32_t *old, const uint32_t *state) {
    const struct mo_bytecode_function *fn = f->v.fn;
    const struct mo_bytecode *file = f->v.file;
    const uint32_t *a = fn->code + 1;
    uint32_t op = fn->code[0] & MO_OPCODE_MASK, global = fn->register_count;
    const struct mo_opcode_info *info = mo_opcode_find(op);
    uint32_t count = info->fixed_count + (fn->code[0] >> MO_OPCODE_VARIABLE_SHIFT);
    enum mo_operand_access modes[CODE];
    bool reference[CODE];
    uint32_t move = UNDEFINED, expected[REGISTERS];
    for (uint32_t r = 0; r < fn->register_count; r++) {
        expected[r] = old[r];
    }
    for (uint32_t i = 0; i < count; i++) {
        modes[i] = access(f, 0, i);
        reference[i] =
            modes[i] != MO_ACCESS_NONE && file->types[fn->register_types[a[i]]].reference;
    }
    for (uint32_t i = 0; i < count; i++) {
        enum mo_operand_access mode = modes[i];
        if (!reads(mode)) {
            continue;
        }
        uint32_t r = a[i], s = old[r];
        bool token = mode == MO_ACCESS_TOKEN_CONSUME, consumed = consumes(mode, s);
        if (token ? s < TOKEN : !readable_state(s)) {
            return false;
        }
        if (consumed && reference[i] && !token && s != OWNED) {
            return false;
        }
        if (op == MO_OP_TAIL_CALL && i >= info->fixed_count && mode == MO_ACCESS_READ &&
            reference[i] && s != BORROWED) {
            return false;
        }
        for (uint32_t j = 0; j < i; j++) {
            if (a[j] == r && reads(modes[j]) &&
                (reference[i] || modes[j] == MO_ACCESS_TOKEN_CONSUME) &&
                ((consumed && (reference[i] || token)) || consumes(modes[j], s))) {
                return false;
            }
        }
        if (mode == MO_ACCESS_MOVE) {
            move = s;
        }
        if (token ||
            ((mode == MO_ACCESS_CONSUME || mode == MO_ACCESS_MOVE) && reference[i] && s == OWNED)) {
            expected[r] = MOVED;
        }
    }
    if (op == MO_OP_REUSE && old[a[1]] != TOKEN + a[2]) {
        return false;
    }
    /* Writes come after the moves, so a register an instruction moves from it may write. */
    for (uint32_t i = 0; i < info->fixed_count; i++) {
        enum mo_operand_access mode = modes[i];
        if (mode != MO_ACCESS_WRITE && mode != MO_ACCESS_TOKEN_WRITE) {
            continue;
        }
        uint32_t r = a[i];
        if (expected[r] == OWNED || expected[r] >= TOKEN) {
            return false;
        }
        expected[r] = mode == MO_ACCESS_TOKEN_WRITE             ? TOKEN + f->reset[0]
                      : op == MO_OP_CLEAR                       ? UNDEFINED
                      : op == MO_OP_MOVE                        ? move
                      : !reference[i]                           ? VALUE
                      : op == MO_OP_CONST || op == MO_OP_GLOBAL ? BORROWED
                                                                : OWNED;
    }
    for (uint32_t r = 0; r < fn->register_count; r++) {
        if (state[r] != expected[r]) {
            return false;
        }
    }
    if (op == MO_OP_SET_GLOBAL ? old[global] != 0 || state[global] != 1
                               : state[global] != old[global]) {
        return false;
    }
    if (f->program != NULL && fn->kind == MO_FUNCTION_INIT) {
        uint32_t needed = op == MO_OP_GLOBAL      ? f->program->global_rank[a[1]]
                          : op == MO_OP_CALL      ? f->program->required[a[1]]
                          : op == MO_OP_TAIL_CALL ? f->program->required[a[0]]
                                                  : 0;
        if ((uint64_t)needed > (uint64_t)f->program->init_rank[f->index] - 1 + old[global]) {
            return false;
        }
    }
    if (op == MO_OP_RET || op == MO_OP_TAIL_CALL || op == MO_OP_TAIL_CALL_CLOSURE) {
        for (uint32_t r = 0; r < fn->register_count; r++) {
            if (state[r] == OWNED || state[r] >= TOKEN) {
                return false;
            }
        }
        if (fn->kind == MO_FUNCTION_INIT && state[global] == 0) {
            return false;
        }
    }
    return true;
}

/* The readable mask after an instruction at pc 0: when visiting, each register's readability
 * after it; otherwise, when it is the one replayed to, each register's readability before it,
 * and else the mask as it was. */
static bool mask_ok(const struct flow *f, const bool *before, const uint32_t *old,
                    const uint32_t *state) {
    for (uint32_t r = 0; f->readable != NULL && r < f->v.fn->register_count; r++) {
        bool expected = f->visit != NULL ? readable_state(state[r])
                        : f->replay == 0 ? readable_state(old[r])
                                         : before[r];
        if (f->readable[r] != expected) {
            return false;
        }
    }
    return true;
}

/* ---- Flows -------------------------------------------------------------------------------- */

static struct mo_load_error error;

static bool any_visit(uint32_t pc, const bool *readable, void *context) {
    (void)pc;
    (void)readable;
    (void)context;
    return nondet_bool();
}

/* A flow as mo_bytecode_verify, mo_bytecode_readable and mo_bytecode_visit_readable start one:
 * verifying (with initialization ranks, or without), replaying up to an instruction, or visiting
 * each one; any work done within its limit, and the code at any offset in the file. */
static struct flow *any_flow(const struct world *w) {
    struct flow *f = NEW(struct flow, 1);
    uint32_t base = nondet_u32();
    __CPROVER_assume(base <= UINT32_MAX - 4 * CODE);
    *f = (struct flow){.v = {w->file, w->fn, w->abilities, base, &error, w->types, w->keys},
                       .layouts = w->keys,
                       .types = w->types,
                       .index = w->index,
                       .replay = nondet_u32(),
                       .work = nondet_u32(),
                       .work_limit = nondet_u32()};
    __CPROVER_assume(f->work <= f->work_limit);
    if (nondet_bool()) {
        f->program = w->program;
    } else {
        f->readable = TABLE(bool, REGISTERS, w->fn->register_count);
        for (uint32_t r = 0; r < w->fn->register_count; r++) {
            f->readable[r] = nondet_bool();
        }
        f->visit = nondet_bool() ? any_visit : NULL;
    }
    mo_arena_init(&f->arena, nondet_size());
    error = any_error();
    return f;
}

/* ---- Harnesses ---------------------------------------------------------------------------- */

/* Any one instruction with an opcode from first to last at the start of its function's code,
 * followed by up to more words of any value, in an object of exactly its size. */
static void any_instruction(struct world *w, uint32_t first, uint32_t last, uint32_t more) {
    uint32_t op = nondet_u32();
    __CPROVER_assume(op >= first && op <= last);
    const struct mo_opcode_info *info = mo_opcode_find(op);
    uint32_t n = any_count(VARIABLE);
    __CPROVER_assume(n <= info->variable_limit);
    uint32_t size = 1U + info->fixed_count + n, count = size + any_count(more);
    __CPROVER_assume(count <= CODE);
    uint32_t *code = TABLE(uint32_t, CODE, count);
    w->fn->code_count = count;
    w->fn->code = code;
    start_mask = 1U | 1U << size;
    code[0] = op | n << MO_OPCODE_VARIABLE_SHIFT;
    for (uint32_t j = 1; j < count; j++) {
        code[j] = nondet_u32();
    }
    for (uint32_t j = 0; j < info->fixed_count + n; j++) {
        __CPROVER_assume(operand_ok(
            w, j < info->fixed_count ? info->fixed[j].kind : info->variable.kind, code[1 + j]));
    }
}

/* The type check of any instruction with an opcode from MO_CBMC_FIRST to MO_CBMC_LAST (every
 * opcode by default). */
#ifndef MO_CBMC_FIRST
#define MO_CBMC_FIRST 1
#endif
#ifndef MO_CBMC_LAST
#define MO_CBMC_LAST MO_OPCODE_COUNT
#endif
void harness_instruction(void) {
    struct world w = any_world(true);
    any_instruction(&w, MO_CBMC_FIRST, MO_CBMC_LAST, 0);
    uint32_t base = nondet_u32();
    __CPROVER_assume(base <= UINT32_MAX - 4 * CODE);
    struct mo_verify_function v = {w.file, w.fn, w.abilities, base, &error, w.types, w.keys};
    error = any_error();
    struct mo_load_error old = error;
    if (mo_verify_instruction(&v, 0)) {
        __CPROVER_assert(verified(w.file, w.fn, 0), "instruction: its counts match its tables");
        __CPROVER_assert(error.offset == old.offset && error.reason == old.reason &&
                             error.kind == old.kind,
                         "instruction: no error when it holds");
    } else {
        __CPROVER_assert(error.offset == base && error.reason != NULL &&
                             error.kind == MO_BYTECODE_INVALID,
                         "instruction: a reason at the instruction");
    }
    reached_end();
}

void harness_lookup(void) {
    uint32_t count = any_count(ENTRIES + 1), type = nondet_u32(), tag = nondet_u32();
    struct world w = {.keys = TABLE(struct layout_key, ENTRIES + 1, count)};
    for (uint32_t i = 0; i < count; i++) {
        w.keys[i] = (struct layout_key){nondet_u32(), nondet_u32(), any_below(count)};
    }
    uint32_t found = mo_verify_layout_lookup(w.keys, count, type, tag);
    if (found != MO_NO_INDEX) {
        __CPROVER_assert(found < count, "lookup: a layout of the file");
        bool matches = false;
        for (uint32_t i = 0; i < count; i++) {
            matches |= w.keys[i].index == found && w.keys[i].type == type && w.keys[i].tag == tag;
        }
        __CPROVER_assert(matches, "lookup: the layout of that type and tag");
    }
    bool sorted = true, present = false;
    for (uint32_t i = 0; i < count; i++) {
        sorted &= i == 0 || layout_order(&w.keys[i - 1], &w.keys[i]) < 0;
        present |= w.keys[i].type == type && w.keys[i].tag == tag;
    }
    if (sorted && present) {
        __CPROVER_assert(found != MO_NO_INDEX, "lookup: finds a layout in sorted keys");
    }
    reached_end();
}

/* The ownership transfer of any instruction, with any room the undo log has left for it: step
 * reads no code after the instruction, so the function's code is the instruction alone, and the
 * undo log as long. */
void harness_step(void) {
    struct world w = any_world(false);
    any_instruction(&w, MO_CBMC_FIRST, MO_CBMC_LAST, 0);
    struct flow *f = any_flow(&w);
    const struct mo_bytecode_function *fn = w.fn;
    uint32_t pc = 0;
    __CPROVER_assume(verified(w.file, fn, pc));
    f->reset = TABLE(uint32_t, CODE, fn->code_count);
    for (uint32_t i = 0; i < fn->code_count; i++) {
        f->reset[i] = nondet_u32();
    }
    __CPROVER_assume(reset_ok(f, pc));
    uint32_t slots = fn->register_count + 1;
    struct registers regs = {.state = TABLE(uint32_t, REGISTERS + 1, slots),
                             .seen = TABLE(uint32_t, REGISTERS, fn->register_count),
                             .consumed = TABLE(uint32_t, REGISTERS, fn->register_count),
                             .undo = TABLE(struct undo, CODE, fn->code_count),
                             .changes = nondet_u32()};
    uint32_t old[REGISTERS + 1];
    bool mask[REGISTERS];
    for (uint32_t r = 0; r < slots; r++) {
        regs.state[r] = r == fn->register_count ? nondet_bool() : any_state(f);
        old[r] = regs.state[r];
    }
    /* Every block runs once (ownership), so no register is marked for this instruction yet, and
     * a visit keeps the mask matching the states. */
    for (uint32_t r = 0; r < fn->register_count; r++) {
        regs.seen[r] = nondet_u32();
        regs.consumed[r] = nondet_u32();
        __CPROVER_assume(regs.seen[r] != pc + 1 && regs.consumed[r] != pc + 1);
        if (f->readable != NULL) {
            __CPROVER_assume(f->visit == NULL || f->readable[r] == readable_state(old[r]));
            mask[r] = f->readable[r];
        }
    }
    __CPROVER_assume(regs.changes <= fn->code_count &&
                     length(fn, pc) - 1 <= fn->code_count - regs.changes);
    uint32_t before = regs.changes;
    if (step(f, &regs, pc)) {
        __CPROVER_assert(states_ok(f, regs.state), "step: register states");
        __CPROVER_assert(regs.changes >= before && regs.changes - before < length(fn, pc),
                         "step: fewer changes than the instruction has words");
        /* Undoing the changes, newest first, gives back the states before it. */
        uint32_t undone[REGISTERS + 1];
        for (uint32_t r = 0; r < slots; r++) {
            undone[r] = regs.state[r];
        }
        for (uint32_t i = regs.changes; i > before; i--) {
            struct undo change = regs.undo[i - 1];
            __CPROVER_assert(change.reg < slots, "step: the undo log names a slot");
            undone[change.reg] = change.state;
        }
        for (uint32_t r = 0; r < slots; r++) {
            __CPROVER_assert(undone[r] == old[r], "step: the undo log restores every slot");
        }
        __CPROVER_assert(step_ok(f, old, regs.state), "step: the ownership rules hold");
        __CPROVER_assert(mask_ok(f, mask, old, regs.state), "step: the readable mask");
    } else {
        __CPROVER_assert(refused(f), "step: a refusal with its reason");
    }
    __CPROVER_assert(f->work <= f->work_limit, "step: work within its limit");
    reached_end();
}

#pragma CPROVER check pop
