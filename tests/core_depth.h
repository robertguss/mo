/* Arena-built depth fixtures also exercise walkers beyond the wire list-depth limit. */
#ifndef MO_TEST_CORE_DEPTH_H
#define MO_TEST_CORE_DEPTH_H

#include "core/core.h"
#include "test.h"

#include <string.h>

static MO_MUST_USE struct mo_core *core_depth(struct mo_arena *arena, uint32_t depth, bool lets) {
    MO_ASSERT(depth != 0 && depth <= MO_LIMIT_CORE_NODES / 2u);
    struct mo_core *p = mo_arena_allocate(arena, sizeof *p, _Alignof(struct mo_core));
    struct mo_core_fun *fun = mo_arena_allocate(arena, sizeof *fun, _Alignof(struct mo_core_fun));
    struct mo_core_ty *ty = mo_arena_allocate(arena, sizeof *ty, _Alignof(struct mo_core_ty));
    struct mo_core_lit *literals =
        mo_arena_allocate_array(arena, 2, sizeof *literals, _Alignof(struct mo_core_lit));
    struct mo_core_expr *nodes =
        mo_arena_allocate_array(arena, depth + 1u, sizeof *nodes, _Alignof(struct mo_core_expr));
    mo_core_expr *args =
        mo_arena_allocate_array(arena, depth, sizeof *args, _Alignof(mo_core_expr));
    char(*names)[16] = mo_arena_allocate_array(arena, depth, sizeof *names, _Alignof(char));
    uint32_t count = lets ? 2u * depth - 1u : depth;
    struct mo_core_span *spans =
        mo_arena_allocate_array(arena, count, sizeof *spans, _Alignof(struct mo_core_span));
    struct mo_core_fun_meta *meta =
        mo_arena_allocate(arena, sizeof *meta, _Alignof(struct mo_core_fun_meta));
    struct mo_core_module_meta *module =
        mo_arena_allocate(arena, sizeof *module, _Alignof(struct mo_core_module_meta));
    uint32_t *index = mo_arena_allocate(arena, sizeof *index, _Alignof(uint32_t));
    CHECK(p != NULL && fun != NULL && ty != NULL && literals != NULL && nodes != NULL &&
          args != NULL && names != NULL && spans != NULL && meta != NULL && module != NULL &&
          index != NULL);
    if (p == NULL || fun == NULL || ty == NULL || literals == NULL || nodes == NULL ||
        args == NULL || names == NULL || spans == NULL || meta == NULL || module == NULL ||
        index == NULL)
        return NULL;
    *ty = (struct mo_core_ty){.tag = MO_CORE_TY_INT};
    literals[0] = (struct mo_core_lit){.tag = MO_CORE_LIT_INT, .as.int_ = {.digits = {"7", 1}}};
    literals[1] = (struct mo_core_lit){.tag = MO_CORE_LIT_INT, .as.int_ = {.digits = {"0", 1}}};
    nodes[0] = (struct mo_core_expr){.tag = MO_CORE_EXPR_LIT, .as.lit = &literals[0]};
    nodes[depth] = (struct mo_core_expr){.tag = MO_CORE_EXPR_LIT, .as.lit = &literals[1]};
    for (uint32_t i = 1; i < depth; i++) {
        if (lets) {
            int length = snprintf(names[i], sizeof names[i], "x%u", i);
            MO_ASSERT(length > 0 && (size_t)length < sizeof names[i]);
            nodes[i] = (struct mo_core_expr){
                .tag = MO_CORE_EXPR_LET,
                .as.let_ = {{names[i], (size_t)length}, &nodes[depth], &nodes[i - 1u]}};
        } else {
            args[i] = &nodes[i - 1u];
            nodes[i] = (struct mo_core_expr){
                .tag = MO_CORE_EXPR_PRIM,
                .as.prim = {.prim = {.tag = MO_CORE_PRIM_INT, .as.int_ = MO_CORE_INT_OP_NEG},
                            .args = {&args[i], 1}}};
        }
    }
    *fun = (struct mo_core_fun){.name = {"main", 4},
                                .module = {"M", 1},
                                .ret = ty,
                                .body = &nodes[depth - 1u],
                                .const_count = {.digits = {"0", 1}}};
    memset(spans, 0, (size_t)count * sizeof *spans);
    *meta = (struct mo_core_fun_meta){.name = {"M.main", 6}, .body.spans = {spans, count}};
    *index = 0;
    *module = (struct mo_core_module_meta){.name = {"M", 1}, .funs = {index, 1}};
    *p = (struct mo_core){.funs = {fun, 1},
                          .entry = {"main", 4},
                          .metadata = {.source = true, .modules = {module, 1}, .funs = {meta, 1}}};
    return p;
}

#endif
