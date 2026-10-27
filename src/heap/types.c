#include "heap/types.h"

#include <stdlib.h>
#include <string.h>

#include "base/limits.h"
#include "base/utf8.h"

void mo_types_init(struct mo_types *types, struct mo_arena *arena) {
    MO_ASSERT(types != NULL && arena != NULL);
    *types = (struct mo_types){.arena = arena};
}

/* Name limits include instantiation spellings, not just source identifiers.
 * NUL is the terminator, never part of a name. */
static MO_MUST_USE enum mo_type_problem check_name(const char *name) {
    if (name == NULL || name[0] == '\0') {
        return MO_TYPES_INVALID;
    }
    size_t length = 0;
    while (length <= MO_LIMIT_STRING_SIZE && name[length] != '\0') {
        length++;
    }
    if (length > MO_LIMIT_STRING_SIZE) {
        return MO_TYPES_STRING_LIMIT;
    }
    if (!mo_utf8_valid((const uint8_t *)name, length)) {
        return MO_TYPES_INVALID;
    }
    return MO_TYPES_OK;
}

/* The hash affects lookup only, never table order or bytes. A non-cryptographic
 * mixer is enough for compiler-created indices; collisions still compare keys. */
static MO_MUST_USE uint64_t mix(uint64_t hash, uint32_t value) {
    hash ^= value;
    hash *= UINT64_C(0x9e3779b185ebca87);
    return hash ^ (hash >> 32);
}

static MO_MUST_USE uint64_t type_hash(const struct mo_type *type) {
    uint64_t hash = mix(0, (uint32_t)type->kind);
    hash = mix(hash, type->result);
    hash = mix(hash, type->function);
    hash = mix(hash, (uint32_t)type->capability);
    hash = mix(hash, type->parameter_count);
    for (uint32_t i = 0; i < type->parameter_count; i++) {
        hash = mix(hash, type->parameters[i]);
    }
    return hash;
}

static MO_MUST_USE uint64_t layout_hash(const struct mo_layout *layout) {
    uint64_t hash = mix(0, (uint32_t)layout->kind);
    hash = mix(hash, layout->type);
    hash = mix(hash, layout->case_index);
    hash = mix(hash, layout->function);
    hash = mix(hash, layout->field_count);
    for (uint32_t i = 0; i < layout->field_count; i++) {
        hash = mix(hash, layout->fields[i]);
    }
    return hash;
}

static MO_MUST_USE bool same_indices(const uint32_t *a, const uint32_t *b, uint32_t count) {
    return count == 0 || memcmp(a, b, (size_t)count * sizeof *a) == 0;
}

static MO_MUST_USE bool same_type(const struct mo_type *a, const struct mo_type *b) {
    return a->kind == b->kind && a->parameter_count == b->parameter_count &&
           a->result == b->result && a->function == b->function && a->capability == b->capability &&
           same_indices(a->parameters, b->parameters, a->parameter_count);
}

static MO_MUST_USE bool same_layout(const struct mo_layout *a, const struct mo_layout *b) {
    return a->kind == b->kind && a->type == b->type && a->case_index == b->case_index &&
           a->function == b->function && a->field_count == b->field_count &&
           same_indices(a->fields, b->fields, a->field_count);
}

/* Power-of-two slots at most half full. Nominal entries never enter the type
 * index: equal names do not make equal declarations. No stored pointers into
 * the growable tables survive a resize. */
static MO_MUST_USE bool grow_types(struct mo_types *types) {
    if (types->count < types->capacity) {
        return true;
    }
    uint32_t capacity = types->capacity == 0 ? 16 : types->capacity * 2;
    MO_ASSERT(capacity <= MO_LIMIT_TYPES);
    struct mo_type *items =
        mo_arena_allocate_array(types->arena, capacity, sizeof *items, _Alignof(struct mo_type));
    if (items == NULL) {
        return false;
    }
    uint32_t slot_count = capacity * 2;
    uint32_t *slots =
        mo_arena_allocate_array(types->arena, slot_count, sizeof *slots, _Alignof(uint32_t));
    if (slots == NULL) {
        return false;
    }
    memset(slots, 0, (size_t)slot_count * sizeof *slots);
    for (uint32_t i = 0; i < types->count; i++) {
        items[i] = types->items[i];
        if (items[i].kind == MO_TYPE_RECORD || items[i].kind == MO_TYPE_CHOICE) {
            continue;
        }
        uint32_t slot = (uint32_t)(type_hash(&items[i]) & (slot_count - 1));
        while (slots[slot] != 0) {
            slot = (slot + 1) & (slot_count - 1);
        }
        slots[slot] = i + 1;
    }
    types->items = items;
    types->capacity = capacity;
    types->type_slots = slots;
    types->type_slot_count = slot_count;
    return true;
}

static MO_MUST_USE enum mo_type_problem
canonical_type(const struct mo_types *types, const struct mo_type *input, struct mo_type *out) {
    *out = (struct mo_type){.kind = input->kind, .defined = true};
    uint32_t limit = 0;
    switch (input->kind) {
    case MO_TYPE_UNIT:
    case MO_TYPE_BOOL:
    case MO_TYPE_BYTE:
    case MO_TYPE_INT:
    case MO_TYPE_FLOAT:
    case MO_TYPE_CHAR:
    case MO_TYPE_TIME:
    case MO_TYPE_DURATION:
    case MO_TYPE_DECIMAL:
    case MO_TYPE_RANGE:
    case MO_TYPE_TEXT:
    case MO_TYPE_BYTES:
    case MO_TYPE_BIGINT:
        break;
    case MO_TYPE_TUPLE:
        if (input->parameter_count < 2) {
            return MO_TYPES_INVALID;
        }
        limit = MO_LIMIT_FIELDS;
        break;
    case MO_TYPE_LIST:
    case MO_TYPE_SET:
        if (input->parameter_count != 1) {
            return MO_TYPES_INVALID;
        }
        limit = 1;
        break;
    case MO_TYPE_MAP:
        if (input->parameter_count != 2) {
            return MO_TYPES_INVALID;
        }
        limit = 2;
        break;
    case MO_TYPE_FUNCTION:
        limit = MO_LIMIT_BYTECODE_PARAMETERS;
        if (input->result >= types->count) {
            return MO_TYPES_INVALID;
        }
        out->result = input->result;
        break;
    case MO_TYPE_ENV:
        if (input->function >= MO_LIMIT_FUNCTIONS) {
            return MO_TYPES_INVALID;
        }
        out->function = input->function;
        break;
    case MO_TYPE_CAPABILITY:
        if (input->capability < MO_CAP_ARGS || input->capability > MO_CAP_SIMULATION) {
            return MO_TYPES_INVALID;
        }
        out->capability = input->capability;
        break;
    case MO_TYPE_RECORD:
    case MO_TYPE_CHOICE:
        return MO_TYPES_INVALID;
    default:
        return MO_TYPES_INVALID;
    }
    if (input->parameter_count > limit) {
        return input->kind == MO_TYPE_FUNCTION ? MO_TYPES_PARAMETER_LIMIT
               : input->kind == MO_TYPE_TUPLE  ? MO_TYPES_FIELD_LIMIT
                                               : MO_TYPES_INVALID;
    }
    if (input->parameter_count != 0 && input->parameters == NULL) {
        return MO_TYPES_INVALID;
    }
    for (uint32_t i = 0; i < input->parameter_count; i++) {
        if (input->parameters[i] >= types->count) {
            return MO_TYPES_INVALID;
        }
    }
    out->parameter_count = input->parameter_count;
    out->parameters = input->parameter_count == 0 ? NULL : input->parameters;
    return MO_TYPES_OK;
}

enum mo_type_problem mo_types_intern(struct mo_types *types, const struct mo_type *type,
                                     uint32_t *out) {
    MO_ASSERT(types != NULL && type != NULL && out != NULL);
    if (types->finished) {
        return MO_TYPES_INVALID;
    }
    struct mo_type key;
    enum mo_type_problem problem = canonical_type(types, type, &key);
    if (problem != MO_TYPES_OK) {
        return problem;
    }
    uint64_t hash = type_hash(&key);
    if (types->type_slot_count != 0) {
        uint32_t slot = (uint32_t)(hash & (types->type_slot_count - 1));
        while (types->type_slots[slot] != 0) {
            uint32_t index = types->type_slots[slot] - 1;
            if (same_type(&key, &types->items[index])) {
                *out = index;
                return MO_TYPES_OK;
            }
            slot = (slot + 1) & (types->type_slot_count - 1);
        }
    }
    if (types->count == MO_LIMIT_TYPES) {
        return MO_TYPES_TYPE_LIMIT;
    }
    /* Copy before table growth: input may point into the current table. */
    if (key.parameter_count != 0) {
        key.parameters =
            mo_arena_copy(types->arena, key.parameters,
                          (size_t)key.parameter_count * sizeof(uint32_t), _Alignof(uint32_t));
        if (key.parameters == NULL) {
            return MO_TYPES_NO_MEMORY;
        }
    }
    if (!grow_types(types)) {
        return MO_TYPES_NO_MEMORY;
    }
    uint32_t slot = (uint32_t)(hash & (types->type_slot_count - 1));
    while (types->type_slots[slot] != 0) {
        slot = (slot + 1) & (types->type_slot_count - 1);
    }
    *out = types->count;
    types->items[types->count++] = key;
    types->type_slots[slot] = *out + 1;
    return MO_TYPES_OK;
}

enum mo_type_problem mo_types_declare(struct mo_types *types, enum mo_type_kind kind,
                                      const char *name, uint32_t *out) {
    MO_ASSERT(types != NULL && out != NULL);
    if (types->finished || (kind != MO_TYPE_RECORD && kind != MO_TYPE_CHOICE)) {
        return MO_TYPES_INVALID;
    }
    enum mo_type_problem problem = check_name(name);
    if (problem != MO_TYPES_OK) {
        return problem;
    }
    if (types->count == MO_LIMIT_TYPES) {
        return MO_TYPES_TYPE_LIMIT;
    }
    const char *copy = mo_arena_copy_text(types->arena, name);
    if (copy == NULL || !grow_types(types)) {
        return MO_TYPES_NO_MEMORY;
    }
    *out = types->count;
    types->items[types->count++] = (struct mo_type){.kind = kind, .name = copy};
    return MO_TYPES_OK;
}

/* Sort scratch pointers, not the fields/cases: declaration order is data. */
static MO_MUST_USE int compare_names(const void *a, const void *b) {
    const char *const *left = a;
    const char *const *right = b;
    return strcmp(*left, *right);
}

static MO_MUST_USE bool names_unique(const char **names, uint32_t count) {
    qsort(names, count, sizeof *names, compare_names);
    for (uint32_t i = 1; i < count; i++) {
        if (strcmp(names[i - 1], names[i]) == 0) {
            return false;
        }
    }
    return true;
}

static MO_MUST_USE enum mo_type_problem check_fields(const struct mo_types *types,
                                                     const struct mo_type_field *fields,
                                                     uint32_t count, bool unnamed) {
    if (count > MO_LIMIT_FIELDS) {
        return MO_TYPES_FIELD_LIMIT;
    }
    if ((count != 0 && fields == NULL) || (unnamed && count != 1)) {
        return MO_TYPES_INVALID;
    }
    for (uint32_t i = 0; i < count; i++) {
        if (fields[i].type >= types->count) {
            return MO_TYPES_INVALID;
        }
        if (unnamed) {
            if (fields[i].name != NULL) {
                return MO_TYPES_INVALID;
            }
        } else {
            enum mo_type_problem problem = check_name(fields[i].name);
            if (problem != MO_TYPES_OK) {
                return problem;
            }
        }
    }
    if (count < 2) {
        return MO_TYPES_OK;
    }
    struct mo_arena scratch;
    mo_arena_init(&scratch, (size_t)count * sizeof(const char *) + MO_ARENA_CHUNK_HEADER);
    const char **names =
        mo_arena_allocate_array(&scratch, count, sizeof *names, _Alignof(const char *));
    if (names == NULL) {
        mo_arena_free(&scratch);
        return MO_TYPES_NO_MEMORY;
    }
    for (uint32_t i = 0; i < count; i++) {
        names[i] = fields[i].name;
    }
    bool unique = names_unique(names, count);
    mo_arena_free(&scratch);
    return unique ? MO_TYPES_OK : MO_TYPES_INVALID;
}

static MO_MUST_USE struct mo_type_field *
copy_fields(struct mo_types *types, const struct mo_type_field *fields, uint32_t count) {
    struct mo_type_field *copy =
        mo_arena_allocate_array(types->arena, count, sizeof *copy, _Alignof(struct mo_type_field));
    if (copy == NULL) {
        return NULL;
    }
    for (uint32_t i = 0; i < count; i++) {
        copy[i] = fields[i];
        if (fields[i].name != NULL) {
            copy[i].name = mo_arena_copy_text(types->arena, fields[i].name);
            if (copy[i].name == NULL) {
                return NULL;
            }
        }
    }
    return copy;
}

enum mo_type_problem mo_types_define_record(struct mo_types *types, uint32_t type,
                                            const struct mo_type_field *fields, uint32_t count) {
    MO_ASSERT(types != NULL);
    if (types->finished || type >= types->count || types->items[type].kind != MO_TYPE_RECORD ||
        types->items[type].defined) {
        return MO_TYPES_INVALID;
    }
    enum mo_type_problem problem = check_fields(types, fields, count, false);
    if (problem != MO_TYPES_OK) {
        return problem;
    }
    struct mo_type_field *copy = NULL;
    if (count != 0) {
        copy = copy_fields(types, fields, count);
        if (copy == NULL) {
            return MO_TYPES_NO_MEMORY;
        }
    }
    types->items[type].fields = copy;
    types->items[type].field_count = count;
    types->items[type].defined = true;
    return MO_TYPES_OK;
}

enum mo_type_problem mo_types_define_choice(struct mo_types *types, uint32_t type,
                                            const struct mo_type_case *cases, uint32_t count) {
    MO_ASSERT(types != NULL);
    if (types->finished || type >= types->count || types->items[type].kind != MO_TYPE_CHOICE ||
        types->items[type].defined || count == 0) {
        return MO_TYPES_INVALID;
    }
    if (count > MO_LIMIT_CHOICE_CASES) {
        return MO_TYPES_CASE_LIMIT;
    }
    if (cases == NULL) {
        return MO_TYPES_INVALID;
    }
    for (uint32_t i = 0; i < count; i++) {
        enum mo_type_problem problem = check_name(cases[i].name);
        if (problem != MO_TYPES_OK) {
            return problem;
        }
        problem = check_fields(types, cases[i].fields, cases[i].field_count, cases[i].unnamed);
        if (problem != MO_TYPES_OK) {
            return problem;
        }
    }
    struct mo_arena scratch;
    mo_arena_init(&scratch, (size_t)count * sizeof(const char *) + MO_ARENA_CHUNK_HEADER);
    const char **names =
        mo_arena_allocate_array(&scratch, count, sizeof *names, _Alignof(const char *));
    if (names == NULL) {
        mo_arena_free(&scratch);
        return MO_TYPES_NO_MEMORY;
    }
    for (uint32_t i = 0; i < count; i++) {
        names[i] = cases[i].name;
    }
    bool unique = names_unique(names, count);
    mo_arena_free(&scratch);
    if (!unique) {
        return MO_TYPES_INVALID;
    }
    struct mo_type_case *copy =
        mo_arena_allocate_array(types->arena, count, sizeof *copy, _Alignof(struct mo_type_case));
    if (copy == NULL) {
        return MO_TYPES_NO_MEMORY;
    }
    bool has_fields = false;
    for (uint32_t i = 0; i < count; i++) {
        copy[i] = cases[i];
        copy[i].name = mo_arena_copy_text(types->arena, cases[i].name);
        if (copy[i].name == NULL) {
            return MO_TYPES_NO_MEMORY;
        }
        if (cases[i].field_count != 0) {
            has_fields = true;
            copy[i].fields = copy_fields(types, cases[i].fields, cases[i].field_count);
            if (copy[i].fields == NULL) {
                return MO_TYPES_NO_MEMORY;
            }
        } else {
            copy[i].fields = NULL;
        }
    }
    types->items[type].cases = copy;
    types->items[type].case_count = count;
    types->items[type].has_fields = has_fields;
    types->items[type].defined = true;
    return MO_TYPES_OK;
}

/* Independent of a choice's reference classification: a nested tagged value
 * takes sixteen bytes even when its active payload is a pointer. The boxed
 * row of 2.3 takes precedence over its overlapping reference-type row. */
static MO_MUST_USE enum mo_case_shape case_shape(const struct mo_types *types,
                                                 const struct mo_type_case *type_case) {
    if (type_case->field_count == 0) {
        return MO_CASE_EMPTY;
    }
    if (type_case->field_count != 1) {
        return MO_CASE_BOXED;
    }
    const struct mo_type *field = &types->items[type_case->fields[0].type];
    if (field->kind == MO_TYPE_CHOICE) {
        return field->has_fields ? MO_CASE_BOXED : MO_CASE_INLINE;
    }
    if (field->kind == MO_TYPE_DECIMAL || field->kind == MO_TYPE_RANGE) {
        return MO_CASE_BOXED;
    }
    return field->kind <= MO_TYPE_DURATION ? MO_CASE_INLINE : MO_CASE_REF;
}

enum mo_type_problem mo_types_finish(struct mo_types *types) {
    MO_ASSERT(types != NULL);
    if (types->finished) {
        return MO_TYPES_OK;
    }
    for (uint32_t i = 0; i < types->count; i++) {
        if (!types->items[i].defined) {
            return MO_TYPES_INVALID;
        }
    }
    for (uint32_t i = 0; i < types->count; i++) {
        struct mo_type *type = &types->items[i];
        type->reference = type->kind >= MO_TYPE_TEXT;
        if (type->kind == MO_TYPE_CHOICE) {
            type->reference = false;
            for (uint32_t j = 0; j < type->case_count; j++) {
                enum mo_case_shape shape = case_shape(types, &type->cases[j]);
                if (shape == MO_CASE_REF || shape == MO_CASE_BOXED) {
                    type->reference = true;
                    break;
                }
            }
        }
    }
    types->finished = true;
    return MO_TYPES_OK;
}

bool mo_type_is_reference(const struct mo_types *types, uint32_t type) {
    MO_ASSERT(types != NULL && types->finished && type < types->count);
    return types->items[type].reference;
}

enum mo_case_shape mo_case_shape(const struct mo_types *types, uint32_t type, uint32_t case_index) {
    MO_ASSERT(types != NULL && types->finished && type < types->count);
    MO_ASSERT(types->items[type].kind == MO_TYPE_CHOICE);
    MO_ASSERT(case_index < types->items[type].case_count);
    return case_shape(types, &types->items[type].cases[case_index]);
}

static MO_MUST_USE enum mo_type_problem check_layout(const struct mo_types *types,
                                                     const struct mo_layout *layout) {
    if (!types->finished || layout->type >= types->count) {
        return MO_TYPES_INVALID;
    }
    if (layout->field_count > MO_LIMIT_FIELDS) {
        return MO_TYPES_FIELD_LIMIT;
    }
    if (layout->field_count != 0 && layout->fields == NULL) {
        return MO_TYPES_INVALID;
    }
    for (uint32_t i = 0; i < layout->field_count; i++) {
        if (layout->fields[i] >= types->count) {
            return MO_TYPES_INVALID;
        }
    }
    const struct mo_type *type = &types->items[layout->type];
    const struct mo_type_field *fields = NULL;
    uint32_t count = 0;
    switch (layout->kind) {
    case MO_LAYOUT_RECORD:
        if (type->kind != MO_TYPE_RECORD) {
            return MO_TYPES_INVALID;
        }
        fields = type->fields;
        count = type->field_count;
        break;
    case MO_LAYOUT_TUPLE:
        return type->kind == MO_TYPE_TUPLE && layout->field_count == type->parameter_count &&
                       same_indices(layout->fields, type->parameters, layout->field_count) &&
                       layout->case_index == MO_NO_INDEX && layout->function == MO_NO_INDEX
                   ? MO_TYPES_OK
                   : MO_TYPES_INVALID;
    case MO_LAYOUT_CASE:
        if (type->kind != MO_TYPE_CHOICE || layout->case_index >= type->case_count ||
            mo_case_shape(types, layout->type, layout->case_index) != MO_CASE_BOXED) {
            return MO_TYPES_INVALID;
        }
        fields = type->cases[layout->case_index].fields;
        count = type->cases[layout->case_index].field_count;
        break;
    case MO_LAYOUT_CLOSURE:
        return type->kind == MO_TYPE_FUNCTION && layout->function < MO_LIMIT_FUNCTIONS &&
                       layout->case_index == MO_NO_INDEX
                   ? MO_TYPES_OK
                   : MO_TYPES_INVALID;
    default:
        return MO_TYPES_INVALID;
    }
    if (layout->field_count != count || layout->function != MO_NO_INDEX ||
        (layout->kind != MO_LAYOUT_CASE && layout->case_index != MO_NO_INDEX)) {
        return MO_TYPES_INVALID;
    }
    for (uint32_t i = 0; i < count; i++) {
        if (layout->fields[i] != fields[i].type) {
            return MO_TYPES_INVALID;
        }
    }
    return MO_TYPES_OK;
}

static MO_MUST_USE bool grow_layouts(struct mo_types *types) {
    if (types->layout_count < types->layout_capacity) {
        return true;
    }
    uint32_t capacity = types->layout_capacity == 0 ? 16 : types->layout_capacity * 2;
    MO_ASSERT(capacity <= MO_LIMIT_LAYOUTS);
    struct mo_layout *layouts = mo_arena_allocate_array(types->arena, capacity, sizeof *layouts,
                                                        _Alignof(struct mo_layout));
    if (layouts == NULL) {
        return false;
    }
    uint32_t slot_count = capacity * 2;
    uint32_t *slots =
        mo_arena_allocate_array(types->arena, slot_count, sizeof *slots, _Alignof(uint32_t));
    if (slots == NULL) {
        return false;
    }
    memset(slots, 0, (size_t)slot_count * sizeof *slots);
    for (uint32_t i = 0; i < types->layout_count; i++) {
        layouts[i] = types->layouts[i];
        uint32_t slot = (uint32_t)(layout_hash(&layouts[i]) & (slot_count - 1));
        while (slots[slot] != 0) {
            slot = (slot + 1) & (slot_count - 1);
        }
        slots[slot] = i + 1;
    }
    types->layouts = layouts;
    types->layout_capacity = capacity;
    types->layout_slots = slots;
    types->layout_slot_count = slot_count;
    return true;
}

enum mo_type_problem mo_types_layout(struct mo_types *types, const struct mo_layout *layout,
                                     uint32_t *out) {
    MO_ASSERT(types != NULL && layout != NULL && out != NULL);
    enum mo_type_problem problem = check_layout(types, layout);
    if (problem != MO_TYPES_OK) {
        return problem;
    }
    uint64_t hash = layout_hash(layout);
    if (types->layout_slot_count != 0) {
        uint32_t slot = (uint32_t)(hash & (types->layout_slot_count - 1));
        while (types->layout_slots[slot] != 0) {
            uint32_t index = types->layout_slots[slot] - 1;
            if (same_layout(layout, &types->layouts[index])) {
                *out = index;
                return MO_TYPES_OK;
            }
            slot = (slot + 1) & (types->layout_slot_count - 1);
        }
    }
    if (types->layout_count == MO_LIMIT_LAYOUTS) {
        return MO_TYPES_LAYOUT_LIMIT;
    }
    struct mo_layout copy = *layout;
    if (copy.field_count != 0) {
        copy.fields =
            mo_arena_copy(types->arena, copy.fields, (size_t)copy.field_count * sizeof(uint32_t),
                          _Alignof(uint32_t));
        if (copy.fields == NULL) {
            return MO_TYPES_NO_MEMORY;
        }
    } else {
        copy.fields = NULL;
    }
    if (!grow_layouts(types)) {
        return MO_TYPES_NO_MEMORY;
    }
    uint32_t slot = (uint32_t)(hash & (types->layout_slot_count - 1));
    while (types->layout_slots[slot] != 0) {
        slot = (slot + 1) & (types->layout_slot_count - 1);
    }
    *out = types->layout_count;
    types->layouts[types->layout_count++] = copy;
    types->layout_slots[slot] = *out + 1;
    return MO_TYPES_OK;
}
