#ifndef MO_TEST_LOWER_FIXTURE_H
#define MO_TEST_LOWER_FIXTURE_H

#include "base/limits.h"
#include "lower/lower.h"

#include <string.h>

/* Equivalent checked-program fixtures while mo_check is being integrated.
 * Only public contracts are used. Graph rows and binder tables are bounded. */
struct lower_fixture {
    struct mo_arena arena;
    struct mo_sources sources;
    struct mo_diags diags;
    struct mo_program program;
    struct mo_module_set set;
    struct mo_module_syntax module, *module_ptr;
    const char *module_name;
    struct mo_syntax tree, declarations, function_syntax[3], *module_children[2], *items[3];
    struct mo_symbol symbols[3];
    struct mo_function_signature signatures[3];
    struct mo_source_type function_types[3], int_, bool_, unit_, text_, range;
    const struct mo_source_type *signature_parts[3][4];
    struct mo_checked_function functions[3];
    struct mo_checked_binding bindings[16];
    struct mo_syntax syntax[256];
    struct mo_checked_expression rows[256];
    uint32_t operands[256][16], count;
    const char *source;
};

static inline void lower_fixture_init(struct lower_fixture *f, const char *source) {
    memset(f, 0, sizeof *f);
    f->source = source;
    mo_arena_init(&f->arena, 1 << 26);
    MO_ASSERT(mo_sources_init(&f->sources) && mo_diags_init(&f->diags));
    uint32_t file;
    struct mo_source_error error;
    MO_ASSERT(mo_sources_add_bytes(&f->sources, "main.mo", (const uint8_t *)source, strlen(source),
                                   &file, &error));
    f->int_ = (struct mo_source_type){.kind = MO_SOURCE_NAMED, .name = "Int", .module = UINT32_MAX};
    f->bool_ = f->unit_ = f->text_ = f->range = f->int_;
    f->bool_.name = "Bool";
    f->unit_.name = "Unit";
    f->text_.name = "Text";
    f->range.name = "Range";
    f->module_name = "Main";
    f->module_ptr = &f->module;
    f->module.tree = &f->tree;
    f->tree.children = f->module_children;
    f->module_children[1] = &f->declarations;
    f->declarations.children = f->items;
    f->declarations.count = 1;
    for (uint32_t i = 0; i < 3; i++) {
        f->items[i] = &f->function_syntax[i];
        f->function_syntax[i] =
            (struct mo_syntax){.kind = MO_S_FN, .span = {file, 0, (uint32_t)strlen(source)}};
        f->symbols[i] = (struct mo_symbol){.name = i == 0 ? "main" : "identity",
                                           .declaration = &f->function_syntax[i],
                                           .owner = UINT32_MAX,
                                           .receiver = UINT32_MAX};
        f->signature_parts[i][0] = &f->int_;
        f->function_types[i] = (struct mo_source_type){
            .kind = MO_SOURCE_FUNCTION, .count = 1, .arguments = f->signature_parts[i]};
        f->signatures[i] = (struct mo_function_signature){.function = &f->symbols[i],
                                                          .type = &f->function_types[i]};
        f->functions[i].signature = &f->signatures[i];
        f->functions[i].body.items = f->rows;
        f->functions[i].body.bindings = f->bindings;
    }
    f->set =
        (struct mo_module_set){.count = 1, .modules = &f->module_ptr, .names = &f->module_name};
    f->program = (struct mo_program){.set = &f->set,
                                     .function_count = 1,
                                     .functions = f->functions,
                                     .entry.result = MO_ENTRY_STATUS};
}

static inline uint32_t lower_fixture_row(struct lower_fixture *f, enum mo_syntax_kind kind,
                                         const struct mo_source_type *type, const char *spelling,
                                         uint32_t count, const uint32_t *operands) {
    MO_ASSERT(f->count < 256 && count <= 16);
    uint32_t index = f->count++;
    struct mo_span span = f->function_syntax[0].span;
    if (spelling != NULL) {
        const char *at = strstr(f->source, spelling);
        MO_ASSERT(at != NULL);
        span.start = (uint32_t)(at - f->source);
        span.end = span.start + (uint32_t)strlen(spelling);
    }
    f->syntax[index] =
        (struct mo_syntax){.kind = kind, .span = span, .text = (const uint8_t *)spelling};
    for (uint32_t i = 0; i < count; i++) {
        MO_ASSERT(operands[i] < index);
        f->operands[index][i] = operands[i];
    }
    f->rows[index] = (struct mo_checked_expression){.syntax = &f->syntax[index],
                                                    .type = type,
                                                    .operand_count = count,
                                                    .operands = f->operands[index],
                                                    .field = UINT32_MAX,
                                                    .receiver = UINT32_MAX};
    f->functions[0].body.count = f->count;
    f->functions[0].body.root = index;
    return index;
}

static inline uint32_t lower_fixture_op(struct lower_fixture *f, enum mo_syntax_token op,
                                        const struct mo_source_type *type, uint32_t a, uint32_t b) {
    uint32_t args[] = {a, b};
    uint32_t index = lower_fixture_row(f, MO_S_BINARY, type, NULL, 2, args);
    f->rows[index].operation = op;
    return index;
}

static inline uint32_t lower_fixture_read(struct lower_fixture *f, uint32_t binding) {
    uint32_t index = lower_fixture_row(f, MO_S_NAME, f->bindings[binding].type, NULL, 0, NULL);
    f->rows[index].binding = &f->bindings[binding];
    return index;
}

static inline uint32_t lower_fixture_bind(struct lower_fixture *f, uint32_t binding, uint32_t value,
                                          bool assignment) {
    uint32_t index =
        lower_fixture_row(f, assignment ? MO_S_BIND : MO_S_VAR, &f->unit_, NULL, 1, &value);
    f->rows[index].binding = &f->bindings[binding];
    f->rows[index].assignment = assignment;
    return index;
}

static inline void lower_fixture_free(struct lower_fixture *f) {
    mo_arena_free(&f->arena);
    mo_sources_free(&f->sources);
    mo_diags_free(&f->diags);
}
#endif
