#include "lower/cps.h"

#include <string.h>

#include "lower/scalar.h"

enum mo_syntax_kind lower_cps_kind(const struct mo_checked_expression *r) {
    const struct mo_syntax *s = r->syntax;
    if (r->operation != MO_T_EOF)
        return MO_S_BINARY;
    if (s->kind == MO_S_POSTFIX)
        return s->children[s->count - 1]->kind;
    if (s->kind == MO_S_BINARY_CHAIN)
        return MO_S_BINARY;
    if (s->kind == MO_S_PREFIX)
        return MO_S_NEGATE;
    return s->kind;
}

const struct mo_ability_implementation *lower_cps_ability(struct body *b,
                                                          const struct mo_checked_expression *r,
                                                          enum mo_source_ability wanted,
                                                          uint32_t operand) {
    for (uint32_t i = 0; i < r->ability_count; i++) {
        lower_work(b->p, 1);
        const struct mo_checked_ability_use *use = &r->abilities[i];
        if (use->operand == operand && use->implementation->ability == wanted)
            return use->implementation;
    }
    lower_refuse(b->p, MO_CODE_LOWER_UNFINISHED, "this operation has no selected ability proof");
}

/* to_text or compare through an ability: a dot call, or qualified (T.to_text). */
bool lower_cps_ability_native(const struct mo_checked_expression *r) {
    return strcmp(r->native, "to_text") == 0 || strcmp(r->native, "compare") == 0;
}

bool lower_cps_direct_function(struct body *b, const struct mo_checked_expression *r) {
    if (r->symbol == NULL || r->symbol->declaration->kind != MO_S_FN)
        return false;
    if (r->operand_count == 0)
        return true; /* A function-value adapter invokes its own declaration. */
    const struct mo_checked_expression *callee = lower_cps_row(b, r->operands[0]);
    return callee->operand_count == 0 && callee->symbol == r->symbol;
}

static mo_core_expr list_sum(struct body *b, mo_core_ty number, mo_core_expr values) {
    struct lowering *p = b->p;
    mo_core_ty list = LOWER_NEW(p, struct mo_core_ty, 1);
    list->tag = MO_CORE_TY_LIST;
    list->as.list = number;
    mo_core_expr zero = lower_int(p, 0);
    if (number->tag != MO_CORE_TY_INT) {
        MO_ASSERT(number->tag == MO_CORE_TY_BIG_INT || number->tag == MO_CORE_TY_DECIMAL ||
                  number->tag == MO_CORE_TY_FLOAT);
        enum mo_core_int_op convert = number->tag == MO_CORE_TY_BIG_INT ? MO_CORE_INT_OP_TO_BIG
                                      : number->tag == MO_CORE_TY_DECIMAL
                                          ? MO_CORE_INT_OP_TO_DECIMAL
                                          : MO_CORE_INT_OP_TO_FLOAT;
        zero = lower_cps_unary(
            b, (struct mo_core_prim){.tag = MO_CORE_PRIM_INT, .as.int_ = convert}, zero);
    }
    struct continuation *done = lower_cps_continuation(b, K_JUMP);
    done->name = lower_fresh(p);
    done->result = true;
    mo_core_expr exit = lower_cps_join(b, done, number);
    exit->as.join.body = lower_var(p, exit->as.join.params.items[0].name);
    struct continuation *again = lower_cps_continuation(b, K_JUMP);
    again->name = lower_fresh(p);
    again->result = true;
    again->position = lower_fresh(p);
    again->position_type = list;
    mo_core_expr round = lower_cps_join(b, again, number);
    exit->as.join.rest = round;
    mo_core_expr accumulator = lower_var(p, round->as.join.params.items[0].name);
    mo_core_expr select = lower_expr(p, MO_CORE_EXPR_CASE);
    round->as.join.body = select;
    select->as.case_.expr = lower_cps_unary(
        b,
        (struct mo_core_prim){.tag = MO_CORE_PRIM_LIST,
                              .as.list = {.t = number, .op.tag = MO_CORE_LIST_OP_UNCONS}},
        lower_var(p, again->position));
    select->as.case_.arms.count = 2;
    select->as.case_.arms.items = LOWER_NEW(p, struct mo_core_arm, 2);
    select->as.case_.arms.items[0] = (struct mo_core_arm){
        .ctor = lower_text(p, "None"), .body = lower_cps_jump(b, done, accumulator)};
    struct mo_core_arm *some = &select->as.case_.arms.items[1];
    some->ctor = lower_text(p, "Some");
    some->names.count = 1;
    some->names.items = LOWER_NEW(p, mo_core_string, 1);
    some->names.items[0] = lower_fresh(p);
    mo_core_expr pair = lower_var(p, some->names.items[0]);
    mo_core_expr_list args = lower_cps_arguments(b, 2);
    args.items[0] = accumulator;
    args.items[1] = lower_cps_projection(b, pair, 0);
    again->extra = lower_cps_projection(b, pair, 1);
    some->body =
        lower_cps_jump(b, again, lower_prim(p, lower_operator(p, MO_T_PLUS, number, number), args));
    again->extra = values;
    round->as.join.rest = lower_cps_jump(b, again, zero);
    return exit;
}

mo_core_expr lower_cps_application(struct body *b, const struct mo_checked_expression *r,
                                   mo_core_expr_list args) {
    struct lowering *p = b->p;
    mo_core_ty result = lower_cps_type(b, r->type);
    if (r->operation != MO_T_EOF) {
        mo_core_ty left = lower_cps_type(b, lower_cps_row(b, r->operands[0])->type);
        mo_core_ty right =
            args.count == 2 ? lower_cps_type(b, lower_cps_row(b, r->operands[1])->type) : NULL;
        if (r->operation == MO_T_LT || r->operation == MO_T_LE || r->operation == MO_T_GT ||
            r->operation == MO_T_GE) {
            for (uint32_t i = 0; i < r->ability_count; i++) {
                lower_work(p, 1);
                const struct mo_checked_ability_use *use = &r->abilities[i];
                if (use->operand != r->operands[0] ||
                    use->implementation->ability != MO_SOURCE_ORDER)
                    continue;
                const struct mo_ability_implementation *proof = use->implementation;
                const struct substitution *substitution = &b->instance->substitution;
                lower_proof(p, &proof, &substitution);
                if (proof->kind == MO_ABILITY_NATIVE)
                    continue;
                static const struct mo_source_type ordering = {
                    .kind = MO_SOURCE_NAMED, .name = "Ordering", .module = UINT32_MAX};
                mo_core_ty order = lower_cps_type(b, &ordering);
                mo_core_expr_list values = lower_cps_arguments(b, 2);
                values.items[0] = lower_compare(p, proof, substitution, b->instance->owner,
                                                b->instance->depth, args.items[0], args.items[1]);
                values.items[1] = lower_expr(p, MO_CORE_EXPR_CON);
                values.items[1]->as.con.ty = order;
                values.items[1]->as.con.ctor = lower_text(
                    p, r->operation == MO_T_LT || r->operation == MO_T_GE ? "Less" : "Greater");
                mo_core_expr e = lower_prim(
                    p, (struct mo_core_prim){.tag = MO_CORE_PRIM_EQ, .as.eq = order}, values);
                if (r->operation == MO_T_LE || r->operation == MO_T_GE)
                    e = lower_cps_unary(b, (struct mo_core_prim){.tag = MO_CORE_PRIM_NOT}, e);
                return e;
            }
        }
        if (left->tag == MO_CORE_TY_UNIT && (r->operation == MO_T_LT || r->operation == MO_T_LE ||
                                             r->operation == MO_T_GT || r->operation == MO_T_GE))
            return lower_bool(p, r->operation == MO_T_LE || r->operation == MO_T_GE);
        mo_core_expr e = lower_prim(p, lower_operator(p, r->operation, left, right), args);
        if (r->operation == MO_T_NE) {
            mo_core_expr_list one = lower_cps_arguments(b, 1);
            one.items[0] = e;
            return lower_prim(p, (struct mo_core_prim){.tag = MO_CORE_PRIM_NOT}, one);
        }
        return e;
    }
    if (r->native != NULL && strncmp(r->native, "Simulation.", 11) == 0) {
        static const struct {
            const char *name;
            enum mo_sim_operation operation;
        } operations[] = {
#define HOSTOP(id, module, member, signature, params, bugs, spec_only)
#define SIMOP(id, module, member, spec_only) {module "." member, MO_SIM_##id},
#include "base/hostops.def"
        };
        size_t index = 0;
        while (index < MO_SIM_COUNT && strcmp(r->native, operations[index].name) != 0)
            index++;
        MO_ASSERT(index < MO_SIM_COUNT);
        struct mo_core_sim_op op = {.tag = operations[index].operation,
                                    .descriptor = {.digits = lower_number(p, r->service)},
                                    .tool = {.digits = lower_number(p, r->tool)}};
        if (op.tag == MO_SIM_SEND) {
            MO_ASSERT(r->operand_count >= 1);
            op.arguments.count = r->operand_count - 1;
            op.arguments.items = LOWER_NEW(p, mo_core_ty, op.arguments.count);
            for (size_t i = 0; i < op.arguments.count; i++)
                op.arguments.items[i] =
                    lower_cps_type(b, lower_cps_row(b, r->operands[i + 1])->type);
            op.result = lower_cps_type(b, r->type->arguments[0]);
        }
        return lower_prim(p, (struct mo_core_prim){.tag = MO_CORE_PRIM_SIM, .as.sim = op}, args);
    }
    if (r->native != NULL) {
        static const struct {
            const char *name;
            enum mo_core_int_op operation;
        } natives[] = {{"Int.abs", MO_CORE_INT_OP_ABS},
                       {"Int.min", MO_CORE_INT_OP_MIN},
                       {"Int.max", MO_CORE_INT_OP_MAX},
                       {"Int.pow", MO_CORE_INT_OP_POW},
                       {"Int.floor_div", MO_CORE_INT_OP_FLOOR_DIV},
                       {"Int.floor_mod", MO_CORE_INT_OP_FLOOR_MOD},
                       {"Int.wrapping_add", MO_CORE_INT_OP_WRAPPING_ADD},
                       {"Int.wrapping_sub", MO_CORE_INT_OP_WRAPPING_SUB},
                       {"Int.wrapping_mul", MO_CORE_INT_OP_WRAPPING_MUL},
                       {"Int.saturating_add", MO_CORE_INT_OP_SATURATING_ADD},
                       {"Int.saturating_sub", MO_CORE_INT_OP_SATURATING_SUB},
                       {"Int.saturating_mul", MO_CORE_INT_OP_SATURATING_MUL},
                       {"Int.checked_add", MO_CORE_INT_OP_CHECKED_ADD},
                       {"Int.checked_sub", MO_CORE_INT_OP_CHECKED_SUB},
                       {"Int.checked_mul", MO_CORE_INT_OP_CHECKED_MUL},
                       {"Int.to_decimal", MO_CORE_INT_OP_TO_DECIMAL},
                       {"Int.to_float", MO_CORE_INT_OP_TO_FLOAT},
                       {"Int.to_big", MO_CORE_INT_OP_TO_BIG},
                       {"Int.to_byte", MO_CORE_INT_OP_TO_BYTE},
                       {"Int.to_char", MO_CORE_INT_OP_TO_CHAR},
                       {"Int.seconds", MO_CORE_INT_OP_SECONDS},
                       {"Int.milliseconds", MO_CORE_INT_OP_MILLISECONDS},
                       {"Int.minutes", MO_CORE_INT_OP_MINUTES},
                       {"Int.hours", MO_CORE_INT_OP_HOURS},
                       {"Int.days", MO_CORE_INT_OP_DAYS}};
        for (size_t i = 0; i < sizeof natives / sizeof natives[0]; i++) {
            lower_work(p, 1);
            if (strcmp(r->native, natives[i].name) == 0)
                return lower_prim(
                    p,
                    (struct mo_core_prim){.tag = MO_CORE_PRIM_INT, .as.int_ = natives[i].operation},
                    args);
        }
        static const struct {
            const char *name;
            enum mo_core_big_op operation;
        } bigs[] = {{"BigInt.abs", MO_CORE_BIG_OP_ABS},
                    {"BigInt.min", MO_CORE_BIG_OP_MIN},
                    {"BigInt.max", MO_CORE_BIG_OP_MAX},
                    {"BigInt.pow", MO_CORE_BIG_OP_POW},
                    {"BigInt.floor_div", MO_CORE_BIG_OP_FLOOR_DIV},
                    {"BigInt.floor_mod", MO_CORE_BIG_OP_FLOOR_MOD},
                    {"BigInt.to_int", MO_CORE_BIG_OP_TO_INT}};
        for (size_t i = 0; i < sizeof bigs / sizeof bigs[0]; i++) {
            lower_work(p, 1);
            if (strcmp(r->native, bigs[i].name) == 0)
                return lower_prim(
                    p, (struct mo_core_prim){.tag = MO_CORE_PRIM_BIG, .as.big = bigs[i].operation},
                    args);
        }
        static const struct {
            const char *name;
            enum mo_core_float_op operation;
        } floats[] = {{"Float.abs", MO_CORE_FLOAT_OP_ABS},
                      {"Float.floor", MO_CORE_FLOAT_OP_FLOOR},
                      {"Float.ceil", MO_CORE_FLOAT_OP_CEIL},
                      {"Float.round", MO_CORE_FLOAT_OP_ROUND},
                      {"Float.sqrt", MO_CORE_FLOAT_OP_SQRT},
                      {"Float.nan?", MO_CORE_FLOAT_OP_IS_NAN},
                      {"Float.infinite?", MO_CORE_FLOAT_OP_IS_INFINITE},
                      {"Float.to_decimal", MO_CORE_FLOAT_OP_TO_DECIMAL}};
        for (size_t i = 0; i < sizeof floats / sizeof floats[0]; i++) {
            lower_work(p, 1);
            if (strcmp(r->native, floats[i].name) == 0)
                return lower_prim(p,
                                  (struct mo_core_prim){.tag = MO_CORE_PRIM_FLOAT,
                                                        .as.float_ = floats[i].operation},
                                  args);
        }
        if (strcmp(r->native, "Byte.to_int") == 0)
            return lower_prim(
                p,
                (struct mo_core_prim){.tag = MO_CORE_PRIM_BYTE, .as.byte = MO_CORE_BYTE_OP_TO_INT},
                args);
        if (strcmp(r->native, "List.sum") == 0) {
            MO_ASSERT(args.count == 1);
            return list_sum(b, result->tag == MO_CORE_TY_FN ? result->as.fn.ret : result,
                            args.items[0]);
        }
        static const struct {
            const char *name;
            enum mo_core_bytes_op operation;
        } bytes[] = {{"Bytes.size", MO_CORE_BYTES_OP_SIZE},
                     {"Bytes.get", MO_CORE_BYTES_OP_GET},
                     {"Bytes.decode_utf8", MO_CORE_BYTES_OP_DECODE_UTF8}};
        for (size_t i = 0; i < sizeof bytes / sizeof bytes[0]; i++) {
            lower_work(p, 1);
            if (strcmp(r->native, bytes[i].name) == 0)
                return lower_prim(p,
                                  (struct mo_core_prim){.tag = MO_CORE_PRIM_BYTES,
                                                        .as.bytes = bytes[i].operation},
                                  args);
        }
        static const struct {
            const char *name;
            enum mo_core_range_op operation;
        } ranges[] = {{"Range.to_list", MO_CORE_RANGE_OP_TO_LIST},
                      {"Range.contains?", MO_CORE_RANGE_OP_CONTAINS}};
        for (size_t i = 0; i < sizeof ranges / sizeof ranges[0]; i++) {
            lower_work(p, 1);
            if (strcmp(r->native, ranges[i].name) == 0)
                return lower_prim(p,
                                  (struct mo_core_prim){.tag = MO_CORE_PRIM_RANGE,
                                                        .as.range = ranges[i].operation},
                                  args);
        }
        static const struct {
            const char *name;
            enum mo_core_dec_op operation;
        } decimals[] = {{"Decimal.abs", MO_CORE_DEC_OP_ABS},
                        {"Decimal.min", MO_CORE_DEC_OP_MIN},
                        {"Decimal.max", MO_CORE_DEC_OP_MAX},
                        {"Decimal.floor", MO_CORE_DEC_OP_FLOOR},
                        {"Decimal.ceil", MO_CORE_DEC_OP_CEIL},
                        {"Decimal.round", MO_CORE_DEC_OP_ROUND},
                        {"Decimal.to_int", MO_CORE_DEC_OP_TO_INT},
                        {"Decimal.to_fixed", MO_CORE_DEC_OP_TO_FIXED},
                        {"Decimal.to_float", MO_CORE_DEC_OP_TO_FLOAT}};
        for (size_t i = 0; i < sizeof decimals / sizeof decimals[0]; i++) {
            lower_work(p, 1);
            if (strcmp(r->native, decimals[i].name) == 0)
                return lower_prim(
                    p,
                    (struct mo_core_prim){.tag = MO_CORE_PRIM_DEC, .as.dec = decimals[i].operation},
                    args);
        }
        static const struct {
            const char *name;
            enum mo_core_time_op operation;
        } times[] = {{"Time.checked_add", MO_CORE_TIME_OP_CHECKED_ADD},
                     {"Time.parse", MO_CORE_TIME_OP_PARSE},
                     {"Duration.in_milliseconds", MO_CORE_TIME_OP_IN_MILLISECONDS},
                     {"Duration.in_seconds", MO_CORE_TIME_OP_IN_SECONDS}};
        for (size_t i = 0; i < sizeof times / sizeof times[0]; i++) {
            lower_work(p, 1);
            if (strcmp(r->native, times[i].name) == 0)
                return lower_prim(
                    p,
                    (struct mo_core_prim){.tag = MO_CORE_PRIM_TIME, .as.time = times[i].operation},
                    args);
        }
        static const struct {
            const char *name;
            enum mo_core_unicode_op operation;
        } unicodes[] = {{"Unicode16.graphemes", MO_CORE_UNICODE_OP_GRAPHEMES},
                        {"Unicode16.display_width", MO_CORE_UNICODE_OP_DISPLAY_WIDTH},
                        {"Unicode16.normalize_nfc", MO_CORE_UNICODE_OP_NORMALIZE_NFC},
                        {"Unicode16.lowercase", MO_CORE_UNICODE_OP_LOWERCASE},
                        {"Unicode16.uppercase", MO_CORE_UNICODE_OP_UPPERCASE},
                        {"Unicode16.letter?", MO_CORE_UNICODE_OP_LETTER},
                        {"Unicode16.digit?", MO_CORE_UNICODE_OP_DIGIT}};
        for (size_t i = 0; i < sizeof unicodes / sizeof unicodes[0]; i++) {
            lower_work(p, 1);
            if (strcmp(r->native, unicodes[i].name) == 0)
                return lower_prim(p,
                                  (struct mo_core_prim){.tag = MO_CORE_PRIM_UNICODE16,
                                                        .as.unicode16 = unicodes[i].operation},
                                  args);
        }
    }
    if (r->native != NULL) {
        static const struct {
            const char *name;
            struct mo_core_io_op operation;
        } io[] = {{"Console.print", {MO_CORE_IO_OP_PRINT, false}},
                  {"Console.error", {MO_CORE_IO_OP_PRINT_ERROR, false}},
                  {"Console.read_line", {MO_CORE_IO_OP_READ_LINE, false}},
                  {"Args.list", {MO_CORE_IO_OP_ARGS_LIST, false}},
                  {"Args.of", {MO_CORE_IO_OP_ARGS_OF, false}},
                  {"Env.get", {MO_CORE_IO_OP_ENV_GET, false}},
                  {"Env.of", {MO_CORE_IO_OP_ENV_OF, false}},
                  {"Console.fake", {MO_CORE_IO_OP_CONSOLE_FAKE, false}},
                  {"Console.fake_input", {MO_CORE_IO_OP_CONSOLE_FAKE_INPUT, false}},
                  {"Console.printed", {MO_CORE_IO_OP_PRINTED, false}},
                  {"Console.printed_errors", {MO_CORE_IO_OP_PRINTED_ERRORS, false}},
                  {"Files.in_memory", {MO_CORE_IO_OP_FILES_IN_MEMORY, false}},
                  {"Files.read_only", {MO_CORE_IO_OP_READ_ONLY, false}},
                  {"Files.read_text", {MO_CORE_IO_OP_READ_TEXT, false}},
                  {"Files.write_text", {MO_CORE_IO_OP_WRITE_TEXT, false}},
                  {"Files.read_bytes", {MO_CORE_IO_OP_READ_BYTES, false}},
                  {"Files.append_text", {MO_CORE_IO_OP_APPEND_TEXT, false}},
                  {"Files.remove", {MO_CORE_IO_OP_REMOVE, false}},
                  {"Files.make_folder", {MO_CORE_IO_OP_MAKE_FOLDER, false}},
                  {"Files.list", {MO_CORE_IO_OP_LIST, false}},
                  {"Files.entries", {MO_CORE_IO_OP_ENTRIES, false}},
                  {"Files.exists?", {MO_CORE_IO_OP_EXISTS, false}},
                  {"Files.within", {MO_CORE_IO_OP_WITHIN, false}},
                  {"ReadFiles.read_text", {MO_CORE_IO_OP_READ_TEXT, true}},
                  {"ReadFiles.read_bytes", {MO_CORE_IO_OP_READ_BYTES, true}},
                  {"ReadFiles.list", {MO_CORE_IO_OP_LIST, true}},
                  {"ReadFiles.entries", {MO_CORE_IO_OP_ENTRIES, true}},
                  {"ReadFiles.exists?", {MO_CORE_IO_OP_EXISTS, true}},
                  {"ReadFiles.within", {MO_CORE_IO_OP_WITHIN, true}},
                  {"Clock.now", {MO_CORE_IO_OP_NOW, false}},
                  {"Clock.fixed", {MO_CORE_IO_OP_CLOCK_FIXED, false}},
                  {"Random.int", {MO_CORE_IO_OP_RANDOM_INT, false}},
                  {"Random.seeded", {MO_CORE_IO_OP_RANDOM_SEEDED, false}}};
        for (size_t i = 0; i < sizeof io / sizeof io[0]; i++) {
            lower_work(p, 1);
            if (strcmp(r->native, io[i].name) == 0)
                return lower_prim(
                    p, (struct mo_core_prim){.tag = MO_CORE_PRIM_IO, .as.io = io[i].operation},
                    args);
        }
        if (strcmp(r->native, "List.join") == 0)
            return lower_prim(
                p, (struct mo_core_prim){.tag = MO_CORE_PRIM_TEXT, .as.text = MO_CORE_TEXT_OP_JOIN},
                args);
        static const struct {
            const char *name;
            enum mo_core_list_op_tag operation;
        } lists[] = {{"List.first", MO_CORE_LIST_OP_FIRST},
                     {"List.last", MO_CORE_LIST_OP_LAST},
                     {"List.get", MO_CORE_LIST_OP_GET},
                     {"List.size", MO_CORE_LIST_OP_SIZE},
                     {"List.empty?", MO_CORE_LIST_OP_IS_EMPTY},
                     {"List.append", MO_CORE_LIST_OP_APPEND},
                     {"List.prepend", MO_CORE_LIST_OP_PREPEND},
                     {"List.concat", MO_CORE_LIST_OP_CONCAT},
                     {"List.reverse", MO_CORE_LIST_OP_REVERSE},
                     {"List.take", MO_CORE_LIST_OP_TAKE},
                     {"List.drop", MO_CORE_LIST_OP_DROP},
                     {"List.set", MO_CORE_LIST_OP_SET},
                     {"List.remove_at", MO_CORE_LIST_OP_REMOVE_AT},
                     {"List.enumerate", MO_CORE_LIST_OP_ENUMERATE},
                     {"List.contains?", MO_CORE_LIST_OP_CONTAINS},
                     {"List.unique", MO_CORE_LIST_OP_UNIQUE}};
        if (strcmp(r->native, "List.zip") == 0) {
            MO_ASSERT(args.count == 2);
            mo_core_ty zipped = result->tag == MO_CORE_TY_FN ? result->as.fn.ret : result;
            MO_ASSERT(zipped->tag == MO_CORE_TY_LIST && zipped->as.list->tag == MO_CORE_TY_TUPLE &&
                      zipped->as.list->as.tuple.count == 2);
            return lower_prim(
                p,
                (struct mo_core_prim){
                    .tag = MO_CORE_PRIM_LIST,
                    .as.list = {.t = zipped->as.list->as.tuple.items[0],
                                .op = {.tag = MO_CORE_LIST_OP_ZIP,
                                       .as.zip = zipped->as.list->as.tuple.items[1]}}},
                args);
        }
        for (size_t i = 0; i < sizeof lists / sizeof lists[0]; i++) {
            lower_work(p, 1);
            if (strcmp(r->native, lists[i].name) == 0) {
                mo_core_ty receiver =
                    r->operand_count == 0
                        ? result->as.fn.params.items[0]
                        : lower_cps_type(b,
                                         lower_cps_row(b, r->receiver == UINT32_MAX ? r->operands[0]
                                                                                    : r->receiver)
                                             ->type);
                if (receiver->tag == MO_CORE_TY_FN)
                    receiver = receiver->as.fn.params.items[0];
                MO_ASSERT(receiver->tag == MO_CORE_TY_LIST);
                return lower_prim(p,
                                  (struct mo_core_prim){.tag = MO_CORE_PRIM_LIST,
                                                        .as.list = {.t = receiver->as.list,
                                                                    .op.tag = lists[i].operation}},
                                  args);
            }
        }
        static const struct {
            const char *name;
            enum mo_core_char_op operation;
        } chars[] = {{"Char.code", MO_CORE_CHAR_OP_CODE},
                     {"Char.ascii_digit?", MO_CORE_CHAR_OP_IS_DIGIT},
                     {"Char.ascii_letter?", MO_CORE_CHAR_OP_IS_LETTER},
                     {"Char.ascii_white_space?", MO_CORE_CHAR_OP_IS_WHITE_SPACE},
                     {"Char.ascii_lowercase", MO_CORE_CHAR_OP_LOWER},
                     {"Char.ascii_uppercase", MO_CORE_CHAR_OP_UPPER},
                     {"Char.to_text", MO_CORE_CHAR_OP_TO_TEXT}};
        for (size_t i = 0; i < sizeof chars / sizeof chars[0]; i++) {
            lower_work(p, 1);
            if (strcmp(r->native, chars[i].name) == 0)
                return lower_prim(
                    p,
                    (struct mo_core_prim){.tag = MO_CORE_PRIM_CHAR, .as.char_ = chars[i].operation},
                    args);
        }
        static const struct {
            const char *name;
            enum mo_core_text_op operation;
        } texts[] = {{"Text.bytes", MO_CORE_TEXT_OP_BYTES},
                     {"Text.byte_count", MO_CORE_TEXT_OP_BYTE_COUNT},
                     {"Text.char_count", MO_CORE_TEXT_OP_CHAR_COUNT},
                     {"Text.empty?", MO_CORE_TEXT_OP_IS_EMPTY},
                     {"Text.chars", MO_CORE_TEXT_OP_CHARS},
                     {"Text.from_chars", MO_CORE_TEXT_OP_FROM_CHARS},
                     {"Text.words", MO_CORE_TEXT_OP_WORDS},
                     {"Text.split", MO_CORE_TEXT_OP_SPLIT},
                     {"Text.trim", MO_CORE_TEXT_OP_TRIM},
                     {"Text.trim_start", MO_CORE_TEXT_OP_TRIM_START},
                     {"Text.trim_end", MO_CORE_TEXT_OP_TRIM_END},
                     {"Text.pad_left", MO_CORE_TEXT_OP_PAD_LEFT},
                     {"Text.pad_right", MO_CORE_TEXT_OP_PAD_RIGHT},
                     {"Text.starts_with?", MO_CORE_TEXT_OP_STARTS_WITH},
                     {"Text.ends_with?", MO_CORE_TEXT_OP_ENDS_WITH},
                     {"Text.drop_prefix", MO_CORE_TEXT_OP_DROP_PREFIX},
                     {"Text.drop_suffix", MO_CORE_TEXT_OP_DROP_SUFFIX},
                     {"Text.replace", MO_CORE_TEXT_OP_REPLACE},
                     {"Text.repeat", MO_CORE_TEXT_OP_REPEAT},
                     {"Text.lines", MO_CORE_TEXT_OP_LINES},
                     {"Text.contains?", MO_CORE_TEXT_OP_CONTAINS},
                     {"Text.ascii_lowercase", MO_CORE_TEXT_OP_LOWER},
                     {"Text.ascii_uppercase", MO_CORE_TEXT_OP_UPPER},
                     {"Text.to_int", MO_CORE_TEXT_OP_TO_INT},
                     {"Text.to_decimal", MO_CORE_TEXT_OP_TO_DECIMAL},
                     {"Text.to_float", MO_CORE_TEXT_OP_TO_FLOAT}};
        for (size_t i = 0; i < sizeof texts / sizeof texts[0]; i++) {
            lower_work(p, 1);
            if (strcmp(r->native, texts[i].name) == 0)
                return lower_prim(
                    p,
                    (struct mo_core_prim){.tag = MO_CORE_PRIM_TEXT, .as.text = texts[i].operation},
                    args);
        }
    }
    if (r->native != NULL && strcmp(r->native, "List.to_map") == 0) {
        mo_core_ty map_ty = result->tag == MO_CORE_TY_FN ? result->as.fn.ret : result;
        MO_ASSERT(map_ty->tag == MO_CORE_TY_MAP);
        struct mo_core_prim prim = {.tag = MO_CORE_PRIM_MAP,
                                    .as.map = {.k = map_ty->as.map.k,
                                               .v = map_ty->as.map.v,
                                               .op.tag = MO_CORE_MAP_OP_OF_LIST}};
        return lower_prim(p, prim, args);
    }
    if (r->native != NULL && strcmp(r->native, "List.to_set") == 0) {
        mo_core_ty set_ty = result->tag == MO_CORE_TY_FN ? result->as.fn.ret : result;
        MO_ASSERT(set_ty->tag == MO_CORE_TY_SET);
        return lower_prim(
            p,
            (struct mo_core_prim){.tag = MO_CORE_PRIM_SET,
                                  .as.set = {.t = set_ty->as.set, .op = MO_CORE_SET_OP_OF_LIST}},
            args);
    }
    static const struct {
        const char *name;
        enum mo_core_map_op_tag operation;
    } maps[] = {{"Map.get", MO_CORE_MAP_OP_GET},         {"Map.set", MO_CORE_MAP_OP_SET},
                {"Map.remove", MO_CORE_MAP_OP_REMOVE},   {"Map.size", MO_CORE_MAP_OP_SIZE},
                {"Map.empty?", MO_CORE_MAP_OP_IS_EMPTY}, {"Map.has?", MO_CORE_MAP_OP_HAS},
                {"Map.keys", MO_CORE_MAP_OP_KEYS},       {"Map.values", MO_CORE_MAP_OP_VALUES},
                {"Map.entries", MO_CORE_MAP_OP_ENTRIES}};
    for (size_t i = 0; r->native != NULL && i < sizeof maps / sizeof maps[0]; i++) {
        if (strcmp(r->native, maps[i].name) != 0)
            continue;
        mo_core_ty receiver =
            r->operand_count == 0
                ? result->as.fn.params.items[0]
                : lower_cps_type(
                      b, lower_cps_row(b, r->receiver == UINT32_MAX ? r->operands[0] : r->receiver)
                             ->type);
        if (receiver->tag == MO_CORE_TY_FN)
            receiver = receiver->as.fn.params.items[0];
        MO_ASSERT(receiver->tag == MO_CORE_TY_MAP);
        struct mo_core_prim prim = {.tag = MO_CORE_PRIM_MAP,
                                    .as.map = {.k = receiver->as.map.k,
                                               .v = receiver->as.map.v,
                                               .op.tag = maps[i].operation}};
        return lower_prim(p, prim, args);
    }
    if (r->native != NULL && strncmp(r->native, "Set.", 4) == 0) {
        static const struct {
            const char *name;
            enum mo_core_set_op operation;
        } sets[] = {{"Set.empty", MO_CORE_SET_OP_EMPTY},
                    {"Set.size", MO_CORE_SET_OP_SIZE},
                    {"Set.empty?", MO_CORE_SET_OP_IS_EMPTY},
                    {"Set.has?", MO_CORE_SET_OP_HAS},
                    {"Set.add", MO_CORE_SET_OP_ADD},
                    {"Set.remove", MO_CORE_SET_OP_REMOVE},
                    {"Set.union", MO_CORE_SET_OP_UNION},
                    {"Set.intersection", MO_CORE_SET_OP_INTERSECTION},
                    {"Set.difference", MO_CORE_SET_OP_DIFFERENCE},
                    {"Set.to_list", MO_CORE_SET_OP_TO_LIST}};
        for (size_t i = 0; i < sizeof sets / sizeof sets[0]; i++) {
            lower_work(p, 1);
            if (strcmp(r->native, sets[i].name) != 0)
                continue;
            mo_core_ty receiver =
                r->operand_count == 0
                    ? (result->tag == MO_CORE_TY_FN ? result->as.fn.params.items[0] : result)
                    : lower_cps_type(b, lower_cps_row(b, r->receiver == UINT32_MAX ? r->operands[0]
                                                                                   : r->receiver)
                                            ->type);
            if (receiver->tag == MO_CORE_TY_FN)
                receiver = receiver->as.fn.params.items[0];
            MO_ASSERT(receiver->tag == MO_CORE_TY_SET);
            return lower_prim(
                p,
                (struct mo_core_prim){.tag = MO_CORE_PRIM_SET,
                                      .as.set = {.t = receiver->as.set, .op = sets[i].operation}},
                args);
        }
    }
    if (r->native != NULL) {
        const char *member = strrchr(r->native, '.');
        if (member != NULL && strcmp(member, ".to_text") == 0) {
            MO_ASSERT(args.count == 1);
            /* Qualified natives are already selected by the shared signature
             * table; an ordinary same-name helper has a source symbol instead. */
            mo_core_ty receiver =
                r->operand_count == 0
                    ? lower_cps_type(b, r->type)->as.fn.params.items[0]
                    : lower_cps_type(b, lower_cps_row(b, r->operands[r->operand_count - 1])->type);
            return lower_error_text(p, receiver, args.items[0]);
        }
    }
    /* A qualified ability function value proves its ability on its own row. */
    uint32_t proved = r->operand_count == 0 ? (uint32_t)(r - b->source->items) : r->operands[0];
    if (r->native != NULL && strcmp(r->native, "to_text") == 0) {
        MO_ASSERT(args.count == 1);
        return lower_to_text(p, lower_cps_ability(b, r, MO_SOURCE_TEXT, proved),
                             &b->instance->substitution, b->instance->owner, b->instance->depth,
                             args.items[0]);
    }
    if (r->native != NULL && strcmp(r->native, "compare") == 0) {
        MO_ASSERT(args.count == 2);
        return lower_compare(p, lower_cps_ability(b, r, MO_SOURCE_ORDER, proved),
                             &b->instance->substitution, b->instance->owner, b->instance->depth,
                             args.items[0], args.items[1]);
    }
    if (r->native != NULL &&
        (strcmp(r->native, "Entry.name") == 0 || strcmp(r->native, "Entry.kind") == 0)) {
        MO_ASSERT(r->field != UINT32_MAX && r->operand_count == 1 && args.count == 1);
        return lower_cps_field_read(b, lower_cps_type(b, lower_cps_row(b, r->operands[0])->type),
                                    args.items[0], r->field);
    }
    if (r->native != NULL && strcmp(r->native, "None") != 0 && strcmp(r->native, "Some") != 0 &&
        strcmp(r->native, "Ok") != 0 && strcmp(r->native, "Error") != 0 &&
        strcmp(r->native, "Less") != 0 && strcmp(r->native, "Equal") != 0 &&
        strcmp(r->native, "Greater") != 0 && strcmp(r->native, "NotFound") != 0 &&
        strcmp(r->native, "NotAllowed") != 0 && strcmp(r->native, "NotText") != 0 &&
        strcmp(r->native, "Failed") != 0 && strcmp(r->native, "Entry") != 0 &&
        strcmp(r->native, "File") != 0 && strcmp(r->native, "Folder") != 0 &&
        strcmp(r->native, "Link") != 0 && strcmp(r->native, "Other") != 0 &&
        strcmp(r->native, "ServiceDown") != 0 && strcmp(r->native, "ToolFailed") != 0)
        lower_refuse(p, MO_CODE_LOWER_UNFINISHED, "this native call is not lowered yet");
    if (lower_cps_kind(r) == MO_S_TUPLE || lower_cps_kind(r) == MO_S_MAP_ENTRY) {
        mo_core_expr e = lower_expr(p, MO_CORE_EXPR_TUPLE);
        e->as.tuple = args;
        return e;
    }
    if (r->field != UINT32_MAX && r->operand_count == 1 && lower_cps_kind(r) != MO_S_NAMED_ARG)
        return lower_cps_field_read(b, lower_cps_type(b, lower_cps_row(b, r->operands[0])->type),
                                    args.items[0], r->field);
    if (lower_cps_kind(r) == MO_S_LIST) {
        struct mo_core_prim prim = {
            .tag = MO_CORE_PRIM_LIST,
            .as.list = {
                .t = result->as.list,
                .op = {.tag = MO_CORE_LIST_OP_LIT, .as.lit.digits = lower_number(p, args.count)}}};
        return lower_prim(p, prim, args);
    }
    if (lower_cps_kind(r) == MO_S_MAP) {
        struct mo_core_prim prim = {
            .tag = MO_CORE_PRIM_MAP,
            .as.map = {
                .k = result->as.map.k,
                .v = result->as.map.v,
                .op = {.tag = MO_CORE_MAP_OP_LIT, .lit.digits = lower_number(p, args.count / 2)}}};
        return lower_prim(p, prim, args);
    }
    if (lower_cps_direct_function(b, r)) {
        struct instance *f = lower_instance(p, r->symbol, r->arguments, &b->instance->substitution,
                                            r->ability_count, r->abilities, b->instance->owner,
                                            b->instance->depth + (r->arguments.count != 0));
        mo_core_expr e = lower_expr(p, MO_CORE_EXPR_CALL);
        e->as.call.name = f->fun.name;
        e->as.call.args = args;
        return e;
    }
    if ((r->symbol != NULL && r->symbol->declaration->kind != MO_S_CONST &&
         r->symbol->declaration->kind != MO_S_FN) ||
        (r->native != NULL && strchr(r->native, '.') == NULL)) {
        mo_core_expr e = lower_expr(p, MO_CORE_EXPR_CON);
        e->as.con.ty = result;
        e->as.con.ctor = lower_text(p, r->symbol != NULL ? r->symbol->name : r->native);
        e->as.con.args = lower_cps_arguments(b, args.count);
        for (size_t i = 0; i < args.count; i++) {
            const struct mo_checked_expression *operand = lower_cps_row(b, r->operands[i]);
            size_t slot = lower_cps_kind(operand) == MO_S_NAMED_ARG ? operand->field : i;
            MO_ASSERT(slot < args.count);
            e->as.con.args.items[slot] = args.items[i];
        }
        return e;
    }
    if (lower_cps_kind(r) == MO_S_CALL) {
        MO_ASSERT(args.count != 0);
        mo_core_expr e = lower_expr(p, MO_CORE_EXPR_APP);
        e->as.app.f = args.items[0];
        e->as.app.args = (mo_core_expr_list){args.items + 1, args.count - 1};
        return e;
    }
    lower_refuse(p, MO_CODE_LOWER_UNFINISHED, "this checked operation is not lowered yet");
}
