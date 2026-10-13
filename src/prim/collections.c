#include "prim/internal.h"

#include <math.h>
#include <string.h>

static MO_MUST_USE bool append(struct mo_prim_call *c, uint32_t type, struct mo_cell *list,
                               struct mo_cell value) {
    return mo_list_append(c->heap, type, list, value);
}

static MO_MUST_USE uint32_t tuple_layout(const struct mo_types *types, uint32_t type) {
    for (uint32_t i = 0; i < types->layout_count; i++)
        if (types->layouts[i].kind == MO_LAYOUT_TUPLE && types->layouts[i].type == type)
            return i;
    MO_UNREACHABLE();
}

static MO_MUST_USE bool pair(struct mo_prim_call *c, uint32_t list_type, uint32_t layout,
                             struct mo_cell *list, struct mo_cell a, struct mo_cell b) {
    struct mo_cell fields[] = {a, b}, value;
    if (!mo_value_construct(c->heap, layout, fields, 2, &value))
        return false;
    bool ok = append(c, list_type, list, value);
    mo_value_drop(c->heap, c->types->items[list_type].parameters[0], value);
    return ok;
}

/* This index borrows input elements. Hashing uses the heap's secret domain;
 * comparisons occur only for equal hashes, not once per earlier element. */
struct seen {
    uint64_t hash;
    uint32_t element;
};

static MO_MUST_USE enum mo_prim_status unique(struct mo_prim_call *c, struct mo_cell *out) {
    uint32_t count = mo_list_size(c->args[0]);
    size_t capacity = 1;
    while (capacity < (size_t)count * 2)
        capacity *= 2;
    struct mo_scratch scratch;
    mo_scratch_init(&scratch, c->heap);
    struct seen *slots = mo_scratch_resize(&scratch, NULL, 0, capacity * sizeof *slots);
    enum mo_prim_status status = MO_PRIM_OUT_OF_MEMORY;
    if (slots == NULL)
        goto done;
    memset(slots, 0, capacity * sizeof *slots);
    struct mo_cell list;
    if (!mo_value_list(c->heap, c->result, NULL, 0, &list))
        goto done;
    uint32_t element_type = c->types->items[c->result].parameters[0];
    for (uint32_t i = 0; i < count; i++) {
        struct mo_cell value;
        uint64_t hash;
        MO_ASSERT(mo_list_get(c->args[0], i, &value));
        if (!mo_value_hash(c->heap, element_type, value, &hash))
            goto failed;
        size_t at = (size_t)hash & (capacity - 1);
        bool equal = false;
        while (slots[at].element != 0) {
            if (slots[at].hash == hash) {
                struct mo_cell previous;
                MO_ASSERT(mo_list_get(c->args[0], slots[at].element - 1, &previous));
                if (!mo_value_equal(c->heap, element_type, previous, value, &equal))
                    goto failed;
                if (equal)
                    break;
            }
            at = (at + 1) & (capacity - 1);
        }
        if (!equal) {
            slots[at] = (struct seen){hash, i + 1};
            if (!append(c, c->result, &list, value))
                goto failed;
        }
    }
    *out = list;
    status = MO_PRIM_RETURNED;
    goto done;
failed:
    mo_value_drop(c->heap, c->result, list);
done:
    mo_scratch_free(&scratch);
    return status;
}

static MO_MUST_USE enum mo_prim_status sum(struct mo_prim_call *c, struct mo_cell *out) {
    uint32_t count = mo_list_size(c->args[0]);
    enum mo_type_kind kind = c->types->items[c->result].kind;
    if (kind == MO_TYPE_BIGINT) {
        struct mo_cell accumulator;
        if (!mo_value_bigint(c->heap, MO_BIGINT_ZERO, NULL, 0, &accumulator))
            return MO_PRIM_OUT_OF_MEMORY;
        for (uint32_t i = 0; i < count; i++) {
            struct mo_cell args[2] = {accumulator}, next;
            MO_ASSERT(mo_list_get(c->args[0], i, &args[1]));
            struct mo_prim_call step = *c;
            step.id = MO_PRIM_BIGINT_ADD;
            step.args = args;
            enum mo_prim_status status = mo_prim_bigint(&step, &next);
            mo_value_drop(c->heap, c->result, accumulator);
            if (status != MO_PRIM_RETURNED)
                return status;
            accumulator = next;
        }
        *out = accumulator;
        return MO_PRIM_RETURNED;
    }
    int64_t integer = 0;
    mo_i128 decimal = 0;
    double floating = 0;
    for (uint32_t i = 0; i < count; i++) {
        struct mo_cell value;
        MO_ASSERT(mo_list_get(c->args[0], i, &value));
        if (kind == MO_TYPE_INT) {
            mo_i128 next = (mo_i128)integer + mo_cell_get_int(value);
            if (next < INT64_MIN || next > INT64_MAX) {
                c->reason = "Int";
                return MO_PRIM_OVERFLOW;
            }
            integer = (int64_t)next;
        } else if (kind == MO_TYPE_DECIMAL) {
            mo_i128 n = mo_cell_get_decimal(value);
            if ((n > 0 && decimal > MO_DECIMAL_MAX - n) ||
                (n < 0 && decimal < -MO_DECIMAL_MAX - n)) {
                c->reason = "Decimal";
                return MO_PRIM_OVERFLOW;
            }
            decimal += n;
        } else {
            MO_ASSERT(kind == MO_TYPE_FLOAT);
            floating += mo_cell_get_float(value);
        }
    }
    if (kind == MO_TYPE_FLOAT && isnan(floating)) {
        uint64_t canonical = UINT64_C(0x7ff8000000000000);
        memcpy(&floating, &canonical, sizeof floating);
    }
    *out = kind == MO_TYPE_INT       ? mo_cell_int(integer)
           : kind == MO_TYPE_DECIMAL ? mo_cell_decimal(decimal)
                                     : mo_cell_float(floating);
    return MO_PRIM_RETURNED;
}

static MO_MUST_USE enum mo_prim_status join(struct mo_prim_call *c, struct mo_cell *out) {
    uint32_t count = mo_list_size(c->args[0]);
    size_t separator_length, length = 0;
    const uint8_t *separator = mo_value_data(c->args[1], &separator_length);
    for (uint32_t i = 0; i < count; i++) {
        struct mo_cell value;
        size_t n;
        MO_ASSERT(mo_list_get(c->args[0], i, &value));
        const uint8_t *text = mo_value_data(value, &n);
        MO_ASSERT(text != NULL);
        if (n > MO_LIMIT_STRING_SIZE - length)
            return MO_PRIM_OUT_OF_MEMORY;
        length += n;
        if (i != 0) {
            if (separator_length > MO_LIMIT_STRING_SIZE - length)
                return MO_PRIM_OUT_OF_MEMORY;
            length += separator_length;
        }
    }
    struct mo_scratch scratch;
    mo_scratch_init(&scratch, c->heap);
    uint8_t *bytes = mo_scratch_resize(&scratch, NULL, 0, length == 0 ? 1 : length);
    bool ok = bytes != NULL;
    size_t at = 0;
    for (uint32_t i = 0; ok && i < count; i++) {
        if (i != 0 && separator_length != 0) {
            memcpy(bytes + at, separator, separator_length);
            at += separator_length;
        }
        struct mo_cell value;
        size_t n;
        MO_ASSERT(mo_list_get(c->args[0], i, &value));
        const uint8_t *text = mo_value_data(value, &n);
        if (n != 0)
            memcpy(bytes + at, text, n);
        at += n;
    }
    if (ok)
        ok = mo_value_text(c->heap, (const char *)bytes, length, out);
    mo_scratch_free(&scratch);
    return ok ? MO_PRIM_RETURNED : MO_PRIM_OUT_OF_MEMORY;
}

static MO_MUST_USE enum mo_prim_status list(struct mo_prim_call *c, struct mo_cell *out) {
    if (c->id == MO_PRIM_LIST_EMPTY)
        return mo_value_list(c->heap, c->result, NULL, 0, out) ? MO_PRIM_RETURNED
                                                               : MO_PRIM_OUT_OF_MEMORY;
    uint32_t count = mo_list_size(c->args[0]);
    if (c->id == MO_PRIM_LIST_SIZE || c->id == MO_PRIM_LIST_IS_EMPTY) {
        *out = c->id == MO_PRIM_LIST_SIZE ? mo_cell_int(count) : mo_cell_bool(count == 0);
        return MO_PRIM_RETURNED;
    }
    if (c->id == MO_PRIM_LIST_GET || c->id == MO_PRIM_LIST_FIRST || c->id == MO_PRIM_LIST_LAST) {
        int64_t index = c->id == MO_PRIM_LIST_GET     ? mo_cell_get_int(c->args[1])
                        : c->id == MO_PRIM_LIST_FIRST ? 0
                                                      : (int64_t)count - 1;
        struct mo_cell value = mo_cell_unit();
        bool found = index >= 0 && (uint64_t)index < count;
        if (found)
            MO_ASSERT(mo_list_get(c->args[0], (uint32_t)index, &value));
        return mo_prim_option(c, found, value, out);
    }
    if (c->id == MO_PRIM_LIST_APPEND) {
        struct mo_cell value = c->args[0];
        if (!append(c, c->result, &value, c->args[1]))
            return MO_PRIM_OUT_OF_MEMORY;
        c->moved = 1;
        *out = value;
        return MO_PRIM_RETURNED;
    }
    if (c->id == MO_PRIM_LIST_CONTAINS) {
        uint32_t type = c->types->items[c->params[0]].parameters[0];
        bool equal = false;
        for (uint32_t i = 0; i < count && !equal; i++) {
            struct mo_cell value;
            MO_ASSERT(mo_list_get(c->args[0], i, &value));
            if (!mo_value_equal(c->heap, type, value, c->args[1], &equal))
                return MO_PRIM_OUT_OF_MEMORY;
        }
        *out = mo_cell_bool(equal);
        return MO_PRIM_RETURNED;
    }
    if (c->id == MO_PRIM_LIST_UNIQUE)
        return unique(c, out);
    if (c->id == MO_PRIM_LIST_SUM)
        return sum(c, out);
    if (c->id == MO_PRIM_LIST_JOIN)
        return join(c, out);
    bool option = c->id == MO_PRIM_LIST_SET || c->id == MO_PRIM_LIST_REMOVE_AT;
    int64_t index = 0;
    uint32_t start = 0, end = count;
    if (option || c->id == MO_PRIM_LIST_TAKE || c->id == MO_PRIM_LIST_DROP) {
        index = mo_cell_get_int(c->args[1]);
        if (option && (index < 0 || (uint64_t)index >= count))
            return mo_prim_option(c, false, mo_cell_unit(), out);
        if (!option && index < 0) {
            c->reason = c->id == MO_PRIM_LIST_TAKE ? "List.take with a negative count"
                                                   : "List.drop with a negative count";
            return MO_PRIM_OUTSIDE;
        }
        uint32_t n = (uint64_t)index > count ? count : (uint32_t)index;
        if (c->id == MO_PRIM_LIST_TAKE)
            end = n;
        if (c->id == MO_PRIM_LIST_DROP)
            start = n;
    }
    uint32_t type = option ? c->types->items[c->result].cases[1].fields[0].type : c->result;
    if (c->id == MO_PRIM_LIST_SET) {
        /* Some(List) has a reference payload and cannot allocate after the update. */
        MO_ASSERT(mo_case_shape(c->types, c->result, 1) == MO_CASE_REF);
        struct mo_cell value = c->args[0];
        if (!mo_list_set(c->heap, type, &value, (uint32_t)index, c->args[2]))
            return MO_PRIM_OUT_OF_MEMORY;
        *out = mo_cell_case(1, value);
        c->moved = 1;
        return MO_PRIM_RETURNED;
    }
    struct mo_cell result;
    if (!mo_value_list(c->heap, type, NULL, 0, &result))
        return MO_PRIM_OUT_OF_MEMORY;
    bool ok = true;
    uint32_t layout = MO_NO_INDEX;
    if (c->id == MO_PRIM_LIST_ENUMERATE || c->id == MO_PRIM_LIST_ZIP)
        layout = tuple_layout(c->types, c->types->items[type].parameters[0]);
    if (c->id == MO_PRIM_LIST_PREPEND)
        ok = append(c, type, &result, c->args[1]);
    if (c->id == MO_PRIM_LIST_ZIP && end > mo_list_size(c->args[1]))
        end = mo_list_size(c->args[1]);
    for (uint32_t i = start; ok && i < end; i++) {
        if (c->id == MO_PRIM_LIST_REMOVE_AT && i == (uint64_t)index)
            continue;
        struct mo_cell value;
        MO_ASSERT(
            mo_list_get(c->args[0], c->id == MO_PRIM_LIST_REVERSE ? count - i - 1 : i, &value));
        if (c->id == MO_PRIM_LIST_ENUMERATE)
            ok = pair(c, type, layout, &result, mo_cell_int(i), value);
        else if (c->id == MO_PRIM_LIST_ZIP) {
            struct mo_cell other;
            MO_ASSERT(mo_list_get(c->args[1], i, &other));
            ok = pair(c, type, layout, &result, value, other);
        } else
            ok = append(c, type, &result, value);
    }
    if (c->id == MO_PRIM_LIST_CONCAT)
        for (uint32_t i = 0; ok && i < mo_list_size(c->args[1]); i++) {
            struct mo_cell value;
            MO_ASSERT(mo_list_get(c->args[1], i, &value));
            ok = append(c, type, &result, value);
        }
    if (!ok) {
        mo_value_drop(c->heap, type, result);
        return MO_PRIM_OUT_OF_MEMORY;
    }
    *out = option ? mo_cell_case(1, result) : result;
    return MO_PRIM_RETURNED;
}

static MO_MUST_USE enum mo_prim_status table(struct mo_prim_call *c, struct mo_cell *out) {
    bool map = c->id >= MO_PRIM_MAP_EMPTY && c->id <= MO_PRIM_MAP_ENTRIES;
    if (c->id == MO_PRIM_MAP_EMPTY || c->id == MO_PRIM_SET_EMPTY) {
        bool ok = map ? mo_value_map(c->heap, c->result, NULL, NULL, 0, out)
                      : mo_value_set(c->heap, c->result, NULL, 0, out);
        return ok ? MO_PRIM_RETURNED : MO_PRIM_OUT_OF_MEMORY;
    }
    if (c->id == MO_PRIM_MAP_SIZE || c->id == MO_PRIM_SET_SIZE || c->id == MO_PRIM_MAP_IS_EMPTY ||
        c->id == MO_PRIM_SET_IS_EMPTY) {
        uint32_t n = mo_table_size(c->args[0]);
        *out = c->id == MO_PRIM_MAP_SIZE || c->id == MO_PRIM_SET_SIZE ? mo_cell_int(n)
                                                                      : mo_cell_bool(n == 0);
        return MO_PRIM_RETURNED;
    }
    uint32_t type = c->params[0];
    if (c->id == MO_PRIM_MAP_GET || c->id == MO_PRIM_MAP_HAS || c->id == MO_PRIM_SET_HAS ||
        c->id == MO_PRIM_MAP_LITERAL_INSERT) {
        bool found;
        struct mo_cell value = mo_cell_unit();
        if (!mo_table_get(c->heap, type, c->args[0], c->args[1], &found, &value))
            return MO_PRIM_OUT_OF_MEMORY;
        if (c->id == MO_PRIM_MAP_GET)
            return mo_prim_option(c, found, value, out);
        if (c->id != MO_PRIM_MAP_LITERAL_INSERT) {
            *out = mo_cell_bool(found);
            return MO_PRIM_RETURNED;
        }
        if (found) {
            c->reason = "two keys of a map literal that turn out equal";
            return MO_PRIM_REPEATED_KEY;
        }
    }
    if (c->id == MO_PRIM_MAP_SET || c->id == MO_PRIM_MAP_LITERAL_INSERT ||
        c->id == MO_PRIM_SET_ADD || c->id == MO_PRIM_MAP_REMOVE || c->id == MO_PRIM_SET_REMOVE) {
        struct mo_cell value = c->args[0];
        bool remove = c->id == MO_PRIM_MAP_REMOVE || c->id == MO_PRIM_SET_REMOVE;
        bool ok = remove ? mo_table_remove(c->heap, type, &value, c->args[1])
                         : mo_table_set(c->heap, type, &value, c->args[1],
                                        map ? c->args[2] : mo_cell_unit());
        if (!ok)
            return MO_PRIM_OUT_OF_MEMORY;
        c->moved = 1;
        *out = value;
        return MO_PRIM_RETURNED;
    }
    bool union_ = c->id == MO_PRIM_SET_UNION;
    bool filter = c->id == MO_PRIM_SET_INTERSECTION || c->id == MO_PRIM_SET_DIFFERENCE;
    struct mo_cell result;
    bool ok = union_ || filter ? mo_value_set(c->heap, c->result, NULL, 0, &result)
                               : mo_value_list(c->heap, c->result, NULL, 0, &result);
    if (!ok)
        return MO_PRIM_OUT_OF_MEMORY;
    uint32_t layout = c->id == MO_PRIM_MAP_ENTRIES
                          ? tuple_layout(c->types, c->types->items[c->result].parameters[0])
                          : MO_NO_INDEX;
    for (unsigned pass = 0; ok && pass < (union_ ? 2U : 1U); pass++) {
        uint32_t position = 0;
        struct mo_cell key, value;
        while (ok && mo_table_next(c->args[pass], &position, &key, &value)) {
            bool include = true;
            if (filter) {
                if (!mo_table_get(c->heap, type, c->args[1], key, &include, &value)) {
                    ok = false;
                    break;
                }
                if (c->id == MO_PRIM_SET_DIFFERENCE)
                    include = !include;
            }
            if (!include)
                continue;
            if (union_ || filter)
                ok = mo_table_set(c->heap, c->result, &result, key, mo_cell_unit());
            else if (c->id == MO_PRIM_MAP_ENTRIES)
                ok = pair(c, c->result, layout, &result, key, value);
            else
                ok = append(c, c->result, &result, c->id == MO_PRIM_MAP_VALUES ? value : key);
        }
    }
    if (!ok) {
        mo_value_drop(c->heap, c->result, result);
        return MO_PRIM_OUT_OF_MEMORY;
    }
    *out = result;
    return MO_PRIM_RETURNED;
}

enum mo_prim_status mo_prim_collection(struct mo_prim_call *c, struct mo_cell *out) {
    if (c->id == MO_PRIM_RANGE_CONTAINS) {
        struct mo_range r = mo_cell_get_range(c->args[0]);
        int64_t n = mo_cell_get_int(c->args[1]);
        *out = mo_cell_bool(r.start <= n && n < r.end);
        return MO_PRIM_RETURNED;
    }
    if (c->id == MO_PRIM_RANGE_TO_LIST) {
        struct mo_range r = mo_cell_get_range(c->args[0]);
        mo_i128 count = r.end > r.start ? (mo_i128)r.end - r.start : 0;
        if (count > MO_LIMIT_COLLECTION_SIZE)
            return MO_PRIM_OUT_OF_MEMORY;
        struct mo_cell result;
        if (!mo_value_list(c->heap, c->result, NULL, 0, &result))
            return MO_PRIM_OUT_OF_MEMORY;
        for (int64_t n = r.start; n < r.end; n++)
            if (!append(c, c->result, &result, mo_cell_int(n))) {
                mo_value_drop(c->heap, c->result, result);
                return MO_PRIM_OUT_OF_MEMORY;
            }
        *out = result;
        return MO_PRIM_RETURNED;
    }
    if (c->id == MO_PRIM_LIST_TO_SET || c->id == MO_PRIM_LIST_TO_MAP) {
        struct mo_cell result;
        bool ok = c->id == MO_PRIM_LIST_TO_MAP
                      ? mo_value_map(c->heap, c->result, NULL, NULL, 0, &result)
                      : mo_value_set(c->heap, c->result, NULL, 0, &result);
        if (!ok)
            return MO_PRIM_OUT_OF_MEMORY;
        for (uint32_t i = 0; i < mo_list_size(c->args[0]); i++) {
            struct mo_cell key, value = mo_cell_unit();
            MO_ASSERT(mo_list_get(c->args[0], i, &key));
            if (c->id == MO_PRIM_LIST_TO_MAP) {
                value = mo_value_field(key, 1);
                key = mo_value_field(key, 0);
            }
            if (!mo_table_set(c->heap, c->result, &result, key, value)) {
                mo_value_drop(c->heap, c->result, result);
                return MO_PRIM_OUT_OF_MEMORY;
            }
        }
        *out = result;
        return MO_PRIM_RETURNED;
    }
    if (c->id <= MO_PRIM_LIST_TO_MAP)
        return list(c, out);
    return table(c, out);
}
