#include "heap/internal.h"

#include <string.h>

struct mo_token mo_value_reset(struct mo_heap *process, uint32_t type, struct mo_cell value) {
    struct mo_object *object = mo_value_object(process->types, type, value);
    MO_ASSERT(object != NULL && object->layout < process->types->layout_count);
    const struct mo_layout *layout = &process->types->layouts[object->layout];
    MO_ASSERT(layout->kind != MO_LAYOUT_CLOSURE);
    struct mo_token token = {.layout = object->layout};
    if (object->count != 1) {
        mo_value_drop(process, type, value);
        return token;
    }
    struct mo_allocation *a = mo_heap_allocation(object);
    for (uint32_t i = 0; i < layout->field_count; i++) {
        mo_value_drop(process, layout->fields[i], a->cells[i]);
    }
    memset(a->cells, 0, (size_t)layout->field_count * sizeof(struct mo_cell));
    a->length = 0;
    a->hash_valid = false;
    token.memory = object;
    return token;
}

static MO_MUST_USE bool reuse(struct mo_heap *process, struct mo_token *token,
                              const struct mo_cell *fields, uint32_t count, bool owned,
                              struct mo_cell *out) {
    MO_ASSERT(process != NULL && token != NULL && out != NULL);
    if (token->layout >= process->types->layout_count || (count != 0 && fields == NULL)) {
        MO_ASSERT(!owned);
        return false;
    }
    const struct mo_layout *layout = &process->types->layouts[token->layout];
    if (layout->kind == MO_LAYOUT_CLOSURE || count != layout->field_count) {
        MO_ASSERT(!owned);
        return false;
    }
    if (token->memory == NULL) {
        bool ok = owned ? mo_value_construct_owned(process, token->layout, fields, count, out)
                        : mo_value_construct(process, token->layout, fields, count, out);
        if (!ok) {
            return false;
        }
    } else {
        struct mo_allocation *a = mo_heap_allocation(token->memory);
        MO_ASSERT(a->object.count == 1 && a->object.layout == token->layout && a->length == 0);
        a->length = count;
        for (uint32_t i = 0; i < count; i++) {
            a->cells[i] = owned ? fields[i] : mo_value_dup(process, layout->fields[i], fields[i]);
        }
        struct mo_cell reference = mo_cell_ref(&a->object);
        *out = layout->kind == MO_LAYOUT_CASE ? mo_cell_case(layout->case_index, reference)
                                              : reference;
    }
    *token = (struct mo_token){.layout = MO_NO_INDEX};
    return true;
}

bool mo_value_reuse(struct mo_heap *process, struct mo_token *token, const struct mo_cell *fields,
                    uint32_t count, struct mo_cell *out) {
    return reuse(process, token, fields, count, false, out);
}

bool mo_value_reuse_owned(struct mo_heap *process, struct mo_token *token,
                          const struct mo_cell *fields, uint32_t count, struct mo_cell *out) {
    return reuse(process, token, fields, count, true, out);
}

void mo_token_free(struct mo_heap *process, struct mo_token *token) {
    MO_ASSERT(process != NULL && token != NULL);
    if (token->memory != NULL) {
        struct mo_allocation *a = mo_heap_allocation(token->memory);
        MO_ASSERT(a->object.count == 1 && a->length == 0);
        mo_heap_release(process, a);
    }
    *token = (struct mo_token){.layout = MO_NO_INDEX};
}

static void mark(const struct mo_types *types, uint32_t type, struct mo_cell value,
                 struct mo_allocation **pending) {
    struct mo_object *object = mo_value_object(types, type, value);
    if (object == NULL) {
        return;
    }
    struct mo_allocation *a = mo_heap_allocation(object);
    MO_ASSERT(a->object.layout != MO_STATE_LAYOUT &&
              a->object.layout != MO_BUILTIN_LAYOUT(MO_TYPE_CAPABILITY));
    if (a->deep_immortal || a->preparing) {
        return;
    }
    MO_ASSERT(object->count != 0);
    a->preparing = true;
    a->pending = *pending;
    *pending = a;
}

bool mo_value_make_immortal(struct mo_heap *process, uint32_t type, struct mo_cell value) {
    MO_ASSERT(process != NULL);
    struct mo_allocation *pending = NULL, *visited = NULL;
    bool ok = true;
    mark(process->types, type, value, &pending);
    while (pending != NULL) {
        struct mo_allocation *a = pending;
        pending = a->pending;
        a->pending = visited;
        visited = a;
        if ((process->abilities[a->type] & MO_ABILITY_HASH) != 0) {
            struct mo_cell cell = mo_cell_ref(&a->object);
            if (a->object.layout < process->types->layout_count &&
                process->types->layouts[a->object.layout].kind == MO_LAYOUT_CASE) {
                cell = mo_cell_case(process->types->layouts[a->object.layout].case_index, cell);
            }
            uint64_t hash;
            if (!mo_value_hash(process, a->type, cell, &hash)) {
                ok = false;
                break;
            }
        }
        if (a->object.layout < process->types->layout_count) {
            const struct mo_layout *layout = &process->types->layouts[a->object.layout];
            for (uint32_t i = 0; i < layout->field_count; i++) {
                mark(process->types, layout->fields[i], a->cells[i], &pending);
            }
        } else {
            const struct mo_type *t = &process->types->items[a->type];
            if (t->kind == MO_TYPE_LIST || t->kind == MO_TYPE_MAP || t->kind == MO_TYPE_SET) {
                uint32_t stride = t->kind == MO_TYPE_MAP ? 2 : 1;
                for (uint32_t i = 0; i < a->length; i++) {
                    if (t->kind != MO_TYPE_LIST && mo_table_live(a)[i] == 0) {
                        continue;
                    }
                    for (uint32_t j = 0; j < stride; j++) {
                        uint32_t index = (t->kind == MO_TYPE_LIST ? a->start : 0) + stride * i + j;
                        mark(process->types, t->parameters[j], a->cells[index], &pending);
                    }
                }
            }
        }
    }
    /* On failure undo only temporary marks; cached hashes remain valid. */
    while (visited != NULL) {
        struct mo_allocation *a = visited;
        visited = a->pending;
        a->pending = NULL;
        a->preparing = false;
        if (ok) {
            a->object.count = MO_IMMORTAL_COUNT;
            a->deep_immortal = true;
        }
    }
    while (pending != NULL) {
        struct mo_allocation *a = pending;
        pending = a->pending;
        a->pending = NULL;
        a->preparing = false;
    }
    return ok;
}
