#include "heap/internal.h"

#include <math.h>
#include <string.h>

/* One child at a time keeps scratch proportional to depth, not width. */
uint32_t mo_value_children(const struct mo_types *types, uint32_t type, struct mo_cell value) {
    const struct mo_type *kind = &types->items[type];
    if (kind->kind == MO_TYPE_CHOICE) {
        return kind->cases[mo_cell_get_tag(value)].field_count;
    }
    struct mo_allocation *a = mo_heap_allocation(mo_cell_get_ref(value));
    return kind->kind == MO_TYPE_MAP ? a->length * 2 : a->length;
}

struct mo_cell mo_value_child(const struct mo_types *types, uint32_t type, struct mo_cell value,
                              uint32_t index, uint32_t *child_type) {
    const struct mo_type *kind = &types->items[type];
    if (kind->kind == MO_TYPE_CHOICE) {
        uint32_t tag = mo_cell_get_tag(value);
        *child_type = kind->cases[tag].fields[index].type;
        if (mo_case_shape(types, type, tag) != MO_CASE_BOXED) {
            return mo_cell_get_payload(value);
        }
        return mo_heap_allocation(mo_cell_get_ref(mo_cell_get_payload(value)))->cells[index];
    }
    struct mo_allocation *a = mo_heap_allocation(mo_cell_get_ref(value));
    if (kind->kind == MO_TYPE_LIST || kind->kind == MO_TYPE_SET) {
        *child_type = kind->parameters[0];
    } else if (kind->kind == MO_TYPE_MAP) {
        *child_type = kind->parameters[index % 2];
    } else {
        MO_ASSERT(a->object.layout < types->layout_count);
        *child_type = types->layouts[a->object.layout].fields[index];
    }
    return a->cells[index];
}

static MO_MUST_USE bool compound(enum mo_type_kind kind) {
    return kind == MO_TYPE_TUPLE || kind == MO_TYPE_RECORD || kind == MO_TYPE_CHOICE ||
           kind == MO_TYPE_LIST || kind == MO_TYPE_MAP || kind == MO_TYPE_SET;
}

static MO_MUST_USE bool atomic_hash(struct mo_heap *process, uint32_t type, struct mo_cell value,
                                    const uint64_t key[2], uint64_t *out) {
    uint64_t hash = mo_heap_mix(key, type, 0);
    enum mo_type_kind kind = process->types->items[type].kind;
    if (kind == MO_TYPE_TEXT || kind == MO_TYPE_BYTES || kind == MO_TYPE_BIGINT) {
        struct mo_allocation *a = mo_heap_allocation(mo_cell_get_ref(value));
        MO_ASSERT(!a->deep_immortal);
        hash = mo_heap_mix(key, hash, mo_heap_hash(key, a->cells, a->length));
        hash = mo_heap_mix(key, hash, kind == MO_TYPE_BIGINT ? mo_bigint_sign(a) : 0);
        a->hash = hash;
        a->hash_valid = true;
    } else {
        hash = mo_heap_mix(key, hash, mo_heap_hash(key, value.bytes, 16));
    }
    *out = hash;
    return true;
}

struct hash_frame {
    struct mo_cell value;
    struct mo_allocation *cache;
    uint64_t hash;
    uint64_t entry;
    uint32_t type;
    uint32_t next;
    uint32_t count;
    bool entered;
};

static MO_MUST_USE bool grow_stack(struct mo_scratch *scratch, void **stack, size_t *capacity,
                                   size_t count, size_t element_size) {
    if (count < *capacity) {
        return true;
    }
    if (*capacity > SIZE_MAX / 2 / element_size) {
        return false;
    }
    size_t next = *capacity == 0 ? 16 : *capacity * 2;
    void *result =
        mo_scratch_resize(scratch, *stack, *capacity * element_size, next * element_size);
    if (result == NULL) {
        return false;
    }
    *stack = result;
    *capacity = next;
    return true;
}

bool mo_value_hash(struct mo_heap *process, uint32_t type, struct mo_cell value, uint64_t *out) {
    MO_ASSERT(process != NULL && type < process->types->count && out != NULL);
    const uint64_t *key = process->hash_key;
    if ((process->abilities[type] & MO_ABILITY_HASH) == 0) {
        return false;
    }
    enum mo_type_kind root_kind = process->types->items[type].kind;
    struct mo_object *root = mo_value_object(process->types, type, value);
    if (root != NULL &&
        (root_kind != MO_TYPE_CHOICE ||
         mo_case_shape(process->types, type, mo_cell_get_tag(value)) == MO_CASE_BOXED)) {
        struct mo_allocation *a = mo_heap_allocation(root);
        if (a->hash_valid) {
            *out = a->hash;
            return true;
        }
    }
    if (!compound(root_kind)) {
        return atomic_hash(process, type, value, key, out);
    }
    struct mo_scratch scratch;
    mo_scratch_init(&scratch, process);
    void *storage = NULL;
    size_t capacity = 0, depth = 0;
    bool ok = grow_stack(&scratch, &storage, &capacity, 0, sizeof(struct hash_frame));
    if (ok) {
        ((struct hash_frame *)storage)[depth++] = (struct hash_frame){.type = type, .value = value};
    }
    uint64_t result = 0;
    while (ok && depth != 0) {
        struct hash_frame *stack = storage;
        struct hash_frame *f = &stack[depth - 1];
        enum mo_type_kind kind = process->types->items[f->type].kind;
        if (!f->entered) {
            if (kind == MO_TYPE_FLOAT || kind == MO_TYPE_FUNCTION || kind == MO_TYPE_ENV ||
                kind == MO_TYPE_CAPABILITY) {
                ok = false;
                break;
            }
            struct mo_object *object = mo_value_object(process->types, f->type, f->value);
            if (object != NULL && (kind != MO_TYPE_CHOICE ||
                                   mo_case_shape(process->types, f->type,
                                                 mo_cell_get_tag(f->value)) == MO_CASE_BOXED)) {
                f->cache = mo_heap_allocation(object);
                MO_ASSERT(!f->cache->deep_immortal || f->cache->hash_valid);
                if (f->cache->hash_valid) {
                    result = f->cache->hash;
                    goto hash_done;
                }
            }
            f->hash =
                mo_heap_mix(key, f->type, kind == MO_TYPE_CHOICE ? mo_cell_get_tag(f->value) : 0);
            f->entered = true;
            if (!compound(kind)) {
                ok = atomic_hash(process, f->type, f->value, key, &result);
                if (!ok) {
                    break;
                }
                goto hash_done;
            }
            f->count = mo_value_children(process->types, f->type, f->value);
        }
        if (kind == MO_TYPE_MAP || kind == MO_TYPE_SET) {
            uint32_t stride = mo_table_stride(f->cache);
            while (f->next < f->count && mo_table_live(f->cache)[f->next / stride] == 0) {
                f->next += stride;
            }
        }
        if (f->next == f->count) {
            uint32_t count = kind == MO_TYPE_MAP || kind == MO_TYPE_SET
                                 ? mo_table_info(f->cache)->active
                                 : f->count;
            result = mo_heap_mix(key, f->hash, count);
            goto hash_done;
        }
        uint32_t child_type;
        struct mo_cell item =
            mo_value_child(process->types, f->type, f->value, f->next++, &child_type);
        ok = grow_stack(&scratch, &storage, &capacity, depth, sizeof(struct hash_frame));
        if (ok) {
            ((struct hash_frame *)storage)[depth++] =
                (struct hash_frame){.type = child_type, .value = item};
        }
        continue;
    hash_done:
        if (f->cache != NULL && !f->cache->deep_immortal) {
            f->cache->hash = result;
            f->cache->hash_valid = true;
        }
        depth--;
        if (depth != 0) {
            struct hash_frame *parent = &stack[depth - 1];
            enum mo_type_kind pk = process->types->items[parent->type].kind;
            if (pk == MO_TYPE_SET) {
                parent->hash += result;
            } else if (pk == MO_TYPE_MAP) {
                if (parent->next % 2 == 1) {
                    parent->entry = result;
                } else {
                    parent->hash += mo_heap_mix(key, parent->entry, result);
                }
            } else {
                parent->hash = mo_heap_mix(key, parent->hash, result);
            }
        }
    }
    mo_scratch_free(&scratch);
    if (ok) {
        *out = result;
    }
    return ok;
}

bool mo_value_index_hash(struct mo_heap *process, uint32_t type, struct mo_cell value,
                         uint64_t *out) {
    uint64_t hash;
    if (!mo_value_hash(process, type, value, &hash)) {
        return false;
    }
    *out = mo_heap_mix(process->image->index_key, type, hash);
    return true;
}

static MO_MUST_USE int compare_i128(mo_i128 a, mo_i128 b) {
    return (a > b) - (a < b);
}

/* Equal on Float is deliberately different from compare on NaN. */
static MO_MUST_USE bool scalar_compare(enum mo_type_kind kind, struct mo_cell a, struct mo_cell b,
                                       bool order, int *out) {
    switch (kind) {
    case MO_TYPE_UNIT:
        *out = 0;
        return true;
    case MO_TYPE_BOOL:
    case MO_TYPE_BYTE:
        *out = compare_i128(a.bytes[0], b.bytes[0]);
        return true;
    case MO_TYPE_INT:
    case MO_TYPE_TIME:
    case MO_TYPE_DURATION:
        *out = compare_i128(mo_cell_get_int(a), mo_cell_get_int(b));
        return true;
    case MO_TYPE_CHAR:
        *out = compare_i128(mo_cell_get_char(a), mo_cell_get_char(b));
        return true;
    case MO_TYPE_DECIMAL:
        *out = compare_i128(mo_cell_get_decimal(a), mo_cell_get_decimal(b));
        return true;
    case MO_TYPE_FLOAT: {
        double x = mo_cell_get_float(a), y = mo_cell_get_float(b);
        if (isnan(x) || isnan(y)) {
            *out = order ? (isnan(x) ? (isnan(y) ? 0 : 1) : -1) : 1;
        } else {
            *out = (x > y) - (x < y);
        }
        return true;
    }
    case MO_TYPE_RANGE:
        if (order) {
            return false;
        }
        *out = memcmp(a.bytes, b.bytes, 16) == 0 ? 0 : 1;
        return true;
    case MO_TYPE_TEXT:
    case MO_TYPE_BYTES:
    case MO_TYPE_BIGINT: {
        struct mo_allocation *x = mo_heap_allocation(mo_cell_get_ref(a));
        struct mo_allocation *y = mo_heap_allocation(mo_cell_get_ref(b));
        size_t common = x->length < y->length ? x->length : y->length;
        if (kind == MO_TYPE_BIGINT) {
            int xs = mo_bigint_sign(x) == MO_BIGINT_NEGATIVE ? -1
                     : mo_bigint_sign(x) == MO_BIGINT_ZERO   ? 0
                                                             : 1;
            int ys = mo_bigint_sign(y) == MO_BIGINT_NEGATIVE ? -1
                     : mo_bigint_sign(y) == MO_BIGINT_ZERO   ? 0
                                                             : 1;
            *out = (xs > ys) - (xs < ys);
            if (*out == 0 && xs != 0) {
                *out = compare_i128(x->length, y->length);
                const uint8_t *xb = (const uint8_t *)x->cells;
                const uint8_t *yb = (const uint8_t *)y->cells;
                for (size_t i = common; *out == 0 && i != 0; i--) {
                    *out = compare_i128(xb[i - 1], yb[i - 1]);
                }
                *out *= xs;
            }
        } else {
            int cmp = memcmp(x->cells, y->cells, common);
            *out = cmp == 0 ? compare_i128(x->length, y->length) : (cmp > 0 ? 1 : -1);
        }
        return true;
    }
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

enum compare_stage {
    COMPARE_ENTER,
    COMPARE_FIELDS,
    COMPARE_ENTRY,
    COMPARE_PROBE,
    COMPARE_KEY,
    COMPARE_VALUE
};
struct compare_frame {
    struct mo_cell left, right;
    struct mo_allocation *a, *b;
    uint64_t hash;
    uint64_t memo_hash;
    uint32_t type, next, count, other_count, slot, probes, candidate;
    enum compare_stage stage;
    bool memoizable;
};

struct compare_memo_entry {
    struct mo_cell left, right;
    uint64_t hash;
    uint32_t type;
    int result;
    bool used;
};
struct compare_memo {
    struct compare_memo_entry *entries;
    size_t capacity;
    size_t count;
};

static MO_MUST_USE size_t memo_slot(const struct compare_memo *memo, uint64_t hash, uint32_t type,
                                    struct mo_cell left, struct mo_cell right) {
    size_t slot = (size_t)hash & (memo->capacity - 1);
    for (;;) {
        const struct compare_memo_entry *e = &memo->entries[slot];
        if (!e->used ||
            (e->hash == hash && e->type == type && memcmp(e->left.bytes, left.bytes, 16) == 0 &&
             memcmp(e->right.bytes, right.bytes, 16) == 0)) {
            return slot;
        }
        slot = (slot + 1) & (memo->capacity - 1);
    }
}

static MO_MUST_USE bool memo_room(struct mo_scratch *scratch, struct compare_memo *memo) {
    if (memo->capacity != 0 && memo->count < memo->capacity / 2) {
        return true;
    }
    if (memo->capacity > SIZE_MAX / 2 / sizeof(struct compare_memo_entry)) {
        return false;
    }
    size_t capacity = memo->capacity == 0 ? 32 : memo->capacity * 2;
    struct mo_scratch replacement;
    mo_scratch_init(&replacement, scratch->process);
    struct compare_memo_entry *entries =
        mo_scratch_resize(&replacement, NULL, 0, capacity * sizeof(struct compare_memo_entry));
    if (entries == NULL) {
        mo_scratch_free(&replacement);
        return false;
    }
    memset(entries, 0, capacity * sizeof *entries);
    struct compare_memo next = {.entries = entries, .capacity = capacity, .count = memo->count};
    for (size_t i = 0; i < memo->capacity; i++) {
        const struct compare_memo_entry *e = &memo->entries[i];
        if (e->used) {
            size_t slot = memo_slot(&next, e->hash, e->type, e->left, e->right);
            next.entries[slot] = *e;
        }
    }
    mo_scratch_free(scratch);
    *scratch = replacement;
    *memo = next;
    return true;
}

static MO_MUST_USE bool memo_store(struct mo_scratch *scratch, struct compare_memo *memo,
                                   const struct compare_frame *f, int result) {
    if (!f->memoizable) {
        return true;
    }
    if (!memo_room(scratch, memo)) {
        return false;
    }
    size_t slot = memo_slot(memo, f->memo_hash, f->type, f->left, f->right);
    if (!memo->entries[slot].used) {
        memo->count++;
    }
    memo->entries[slot] = (struct compare_memo_entry){.left = f->left,
                                                      .right = f->right,
                                                      .hash = f->memo_hash,
                                                      .type = f->type,
                                                      .result = result,
                                                      .used = true};
    return true;
}

static MO_MUST_USE bool compare_values(struct mo_heap *process, uint32_t type, struct mo_cell left,
                                       struct mo_cell right, bool order, int *out) {
    MO_ASSERT(process != NULL && type < process->types->count && out != NULL);
    if ((process->abilities[type] & (order ? MO_ABILITY_ORDER : MO_ABILITY_EQUAL)) == 0) {
        return false;
    }
    if ((process->abilities[type] & MO_ABILITY_HASH) != 0 &&
        memcmp(left.bytes, right.bytes, 16) == 0) {
        *out = 0;
        return true;
    }
    if (!compound(process->types->items[type].kind)) {
        return scalar_compare(process->types->items[type].kind, left, right, order, out);
    }
    struct mo_scratch scratch, memo_scratch;
    mo_scratch_init(&scratch, process);
    mo_scratch_init(&memo_scratch, process);
    void *storage = NULL;
    struct compare_memo memo = {0};
    size_t capacity = 0, depth = 0;
    bool ok = grow_stack(&scratch, &storage, &capacity, 0, sizeof(struct compare_frame));
    if (ok) {
        ((struct compare_frame *)storage)[depth++] =
            (struct compare_frame){.type = type, .left = left, .right = right};
    }
    int result = 0;
    while (ok && depth != 0) {
        struct compare_frame *stack = storage;
        struct compare_frame *f = &stack[depth - 1];
        enum mo_type_kind kind = process->types->items[f->type].kind;
        uint32_t child_type = 0;
        struct mo_cell a = mo_cell_unit(), b = mo_cell_unit();
        if (f->stage == COMPARE_ENTER) {
            if ((process->abilities[f->type] & MO_ABILITY_HASH) != 0 &&
                memcmp(f->left.bytes, f->right.bytes, 16) == 0) {
                result = 0;
                goto compare_done;
            }
            if (!compound(kind)) {
                ok = scalar_compare(kind, f->left, f->right, order, &result);
                if (!ok) {
                    break;
                }
                goto compare_done;
            }
            struct mo_object *left_object = mo_value_object(process->types, f->type, f->left);
            struct mo_object *right_object = mo_value_object(process->types, f->type, f->right);
            f->memoizable = (left_object != NULL && left_object->count != 1) ||
                            (right_object != NULL && right_object->count != 1);
            if (f->memoizable) {
                f->memo_hash =
                    mo_heap_mix(process->hash_key, f->type,
                                mo_heap_mix(process->hash_key,
                                            mo_heap_hash(process->hash_key, f->left.bytes, 16),
                                            mo_heap_hash(process->hash_key, f->right.bytes, 16)));
                if (!memo_room(&memo_scratch, &memo)) {
                    ok = false;
                    break;
                }
                size_t slot = memo_slot(&memo, f->memo_hash, f->type, f->left, f->right);
                if (memo.entries[slot].used) {
                    result = memo.entries[slot].result;
                    goto compare_done;
                }
            }
            if (kind == MO_TYPE_CHOICE) {
                result = compare_i128(mo_cell_get_tag(f->left), mo_cell_get_tag(f->right));
                if (result != 0) {
                    goto compare_done;
                }
            }
            if (kind == MO_TYPE_MAP || kind == MO_TYPE_SET) {
                if (order) {
                    ok = false;
                    break;
                }
                f->a = mo_heap_allocation(mo_cell_get_ref(f->left));
                f->b = mo_heap_allocation(mo_cell_get_ref(f->right));
                result = compare_i128(mo_table_info(f->a)->active, mo_table_info(f->b)->active);
                if (result != 0) {
                    goto compare_done;
                }
                f->stage = COMPARE_ENTRY;
            } else {
                f->count = mo_value_children(process->types, f->type, f->left);
                f->other_count = mo_value_children(process->types, f->type, f->right);
                if (!order && f->count != f->other_count) {
                    result = 1;
                    goto compare_done;
                }
                f->stage = COMPARE_FIELDS;
            }
        }
        if (f->stage == COMPARE_FIELDS) {
            if (f->next == f->count || f->next == f->other_count) {
                result = compare_i128(f->count, f->other_count);
                goto compare_done;
            }
            uint32_t other_type;
            a = mo_value_child(process->types, f->type, f->left, f->next, &child_type);
            b = mo_value_child(process->types, f->type, f->right, f->next++, &other_type);
            MO_ASSERT(child_type == other_type);
        } else {
            uint32_t stride = mo_table_stride(f->a);
            if (f->stage == COMPARE_ENTRY) {
                while (f->next < f->a->length && mo_table_live(f->a)[f->next] == 0) {
                    f->next++;
                }
                if (f->next == f->a->length) {
                    result = 0;
                    goto compare_done;
                }
                child_type = process->types->items[f->type].parameters[0];
                /* Hash is a separate iterative walk, never an equality call. */
                if (!mo_value_index_hash(process, child_type, f->a->cells[stride * f->next],
                                         &f->hash)) {
                    ok = false;
                    break;
                }
                f->slot = (uint32_t)(f->hash & (mo_table_info(f->b)->slots - 1));
                f->probes = 0;
                f->stage = COMPARE_PROBE;
            }
            if (f->stage == COMPARE_PROBE) {
                bool candidate = false;
                while (f->probes++ < mo_table_info(f->b)->slots) {
                    uint32_t entry = mo_table_slots(f->b)[f->slot];
                    f->slot = (f->slot + 1) & (mo_table_info(f->b)->slots - 1);
                    if (entry == 0) {
                        result = 1;
                        goto compare_done;
                    }
                    if (entry != MO_NO_INDEX && mo_table_hashes(f->b)[entry - 1] == f->hash) {
                        f->candidate = entry - 1;
                        candidate = true;
                        break;
                    }
                }
                if (!candidate) {
                    ok = false;
                    break;
                }
                child_type = process->types->items[f->type].parameters[0];
                a = f->a->cells[stride * f->next];
                b = f->b->cells[stride * f->candidate];
                f->stage = COMPARE_KEY;
            } else {
                MO_ASSERT(f->stage == COMPARE_VALUE && stride == 2);
                child_type = process->types->items[f->type].parameters[1];
                a = f->a->cells[stride * f->next + 1];
                b = f->b->cells[stride * f->candidate + 1];
            }
        }
        ok = grow_stack(&scratch, &storage, &capacity, depth, sizeof(struct compare_frame));
        if (ok) {
            ((struct compare_frame *)storage)[depth++] =
                (struct compare_frame){.type = child_type, .left = a, .right = b};
        }
        continue;
    compare_done:
        if (!memo_store(&memo_scratch, &memo, f, result)) {
            ok = false;
            break;
        }
        depth--;
        while (depth != 0) {
            struct compare_frame *parent = &stack[depth - 1];
            if (parent->stage == COMPARE_KEY) {
                if (result != 0) {
                    parent->stage = COMPARE_PROBE;
                } else if (mo_table_stride(parent->a) == 2) {
                    parent->stage = COMPARE_VALUE;
                } else {
                    parent->next++;
                    parent->stage = COMPARE_ENTRY;
                }
                break;
            }
            if (result != 0) {
                if (!memo_store(&memo_scratch, &memo, parent, result)) {
                    ok = false;
                    break;
                }
                depth--;
                continue;
            }
            if (parent->stage == COMPARE_VALUE) {
                parent->next++;
                parent->stage = COMPARE_ENTRY;
            }
            break;
        }
    }
    mo_scratch_free(&memo_scratch);
    mo_scratch_free(&scratch);
    if (ok) {
        *out = result;
    }
    return ok;
}

bool mo_value_equal(struct mo_heap *process, uint32_t type, struct mo_cell left,
                    struct mo_cell right, bool *out) {
    MO_ASSERT(out != NULL);
    int result;
    if (!compare_values(process, type, left, right, false, &result)) {
        return false;
    }
    *out = result == 0;
    return true;
}

bool mo_value_compare(struct mo_heap *process, uint32_t type, struct mo_cell left,
                      struct mo_cell right, int *out) {
    return compare_values(process, type, left, right, true, out);
}
