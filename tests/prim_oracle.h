#ifndef MO_PRIM_ORACLE_H
#define MO_PRIM_ORACLE_H

#include "core/core.h"
#include "prim_fixture.h"

#include <stdio.h>

static MO_MUST_USE mo_i128 oracle_integer(mo_core_integer n) {
    mo_u128 magnitude = 0;
    for (size_t i = 0; i < n.digits.length; i++) {
        MO_ASSERT(magnitude <= (((mo_u128)1 << 127) - 1) / 10);
        magnitude = magnitude * 10 + (unsigned)(n.digits.bytes[i] - '0');
    }
    MO_ASSERT(magnitude < (mo_u128)1 << 127);
    return n.negative ? -(mo_i128)magnitude : (mo_i128)magnitude;
}

static MO_MUST_USE struct mo_cell oracle_cell(struct prim_fixture *f, uint32_t type,
                                              mo_core_value v) {
    struct mo_cell result;
    switch ((unsigned)f->types.items[type].kind) {
    case MO_TYPE_INT:
        return mo_cell_int((int64_t)oracle_integer(v->as.int_));
    case MO_TYPE_DURATION:
        return mo_cell_duration((int64_t)oracle_integer(v->as.duration));
    case MO_TYPE_TIME:
        return mo_cell_time((int64_t)oracle_integer(v->as.time));
    case MO_TYPE_DECIMAL:
        return mo_cell_decimal(oracle_integer(v->as.decimal));
    case MO_TYPE_BYTE:
        return mo_cell_byte((uint8_t)oracle_integer(v->as.byte));
    case MO_TYPE_CHAR:
        return mo_cell_char(v->as.char_);
    case MO_TYPE_BOOL:
        return mo_cell_bool(v->as.bool_);
    case MO_TYPE_UNIT:
        return mo_cell_unit();
    case MO_TYPE_FLOAT: {
        double value;
        memcpy(&value, &v->as.float_, sizeof value);
        return mo_cell_float(value);
    }
    case MO_TYPE_TEXT:
        MO_ASSERT(mo_value_text(f->heap, v->as.text.bytes, v->as.text.length, &result));
        return result;
    case MO_TYPE_RANGE:
        return mo_cell_range((int64_t)oracle_integer(v->as.range.start),
                             (int64_t)oracle_integer(v->as.range.end));
    case MO_TYPE_BYTES: {
        uint8_t bytes[4096];
        MO_ASSERT(v->as.bytes.count <= sizeof bytes);
        for (size_t i = 0; i < v->as.bytes.count; i++)
            bytes[i] = (uint8_t)oracle_integer(v->as.bytes.items[i]);
        MO_ASSERT(mo_value_bytes(f->heap, bytes, v->as.bytes.count, &result));
        return result;
    }
    case MO_TYPE_BIGINT: {
        uint8_t bytes[4096] = {0};
        size_t length = 0;
        for (size_t i = 0; i < v->as.big_int.digits.length; i++) {
            unsigned carry = (unsigned)(v->as.big_int.digits.bytes[i] - '0');
            for (size_t j = 0; j < length; j++) {
                carry += 10U * bytes[j];
                bytes[j] = (uint8_t)carry;
                carry >>= 8;
            }
            if (carry != 0) { MO_ASSERT(length < sizeof bytes); bytes[length++] = (uint8_t)carry; }
        }
        enum mo_bigint_sign sign = length == 0 ? MO_BIGINT_ZERO : v->as.big_int.negative ? MO_BIGINT_NEGATIVE : MO_BIGINT_POSITIVE;
        MO_ASSERT(mo_value_bigint(f->heap, sign, bytes, length, &result));
        return result;
    }
    case MO_TYPE_LIST:
    case MO_TYPE_SET:
    case MO_TYPE_TUPLE: {
        mo_core_values values = f->types.items[type].kind == MO_TYPE_LIST ? v->as.list :
                                f->types.items[type].kind == MO_TYPE_SET ? v->as.set : v->as.tuple;
        struct mo_cell cells[4096];
        MO_ASSERT(values.count <= 4096);
        for (size_t i = 0; i < values.count; i++) {
            uint32_t element = f->types.items[type].parameters[f->types.items[type].kind == MO_TYPE_TUPLE ? i : 0];
            cells[i] = oracle_cell(f, element, values.items[i]);
        }
        if (f->types.items[type].kind == MO_TYPE_TUPLE) {
            uint32_t layout = 0;
            while (layout < f->types.layout_count && f->types.layouts[layout].type != type) layout++;
            MO_ASSERT(layout < f->types.layout_count && mo_value_construct(f->heap, layout, cells, (uint32_t)values.count, &result));
        } else if (f->types.items[type].kind == MO_TYPE_LIST)
            MO_ASSERT(mo_value_list(f->heap, type, cells, (uint32_t)values.count, &result));
        else MO_ASSERT(mo_value_set(f->heap, type, cells, (uint32_t)values.count, &result));
        for (size_t i = 0; i < values.count; i++)
            mo_value_drop(f->heap, f->types.items[type].parameters[f->types.items[type].kind == MO_TYPE_TUPLE ? i : 0], cells[i]);
        return result;
    }
    case MO_TYPE_MAP: {
        struct mo_cell keys[4096], values[4096];
        MO_ASSERT(v->as.map.count <= 4096);
        for (size_t i = 0; i < v->as.map.count; i++) {
            keys[i] = oracle_cell(f, f->types.items[type].parameters[0], v->as.map.items[i].key);
            values[i] = oracle_cell(f, f->types.items[type].parameters[1], v->as.map.items[i].value);
        }
        MO_ASSERT(mo_value_map(f->heap, type, keys, values, (uint32_t)v->as.map.count, &result));
        for (size_t i = 0; i < v->as.map.count; i++) {
            mo_value_drop(f->heap, f->types.items[type].parameters[0], keys[i]);
            mo_value_drop(f->heap, f->types.items[type].parameters[1], values[i]);
        }
        return result;
    }
    default:
        MO_UNREACHABLE();
    }
}

static MO_MUST_USE mo_core_value oracle_value(struct prim_fixture *f, struct mo_arena *arena,
                                              uint32_t type, struct mo_cell cell) {
    mo_core_value v = mo_arena_allocate(arena, sizeof *v, _Alignof(struct mo_core_value));
    MO_ASSERT(v != NULL);
    memset(v, 0, sizeof *v);
    mo_i128 n;
    switch ((unsigned)f->types.items[type].kind) {
    case MO_TYPE_INT:
        v->tag = MO_CORE_VALUE_INT;
        n = mo_cell_get_int(cell);
        break;
    case MO_TYPE_DURATION:
        v->tag = MO_CORE_VALUE_DURATION;
        n = mo_cell_get_duration(cell);
        break;
    case MO_TYPE_TIME:
        v->tag = MO_CORE_VALUE_TIME;
        n = mo_cell_get_time(cell);
        break;
    case MO_TYPE_DECIMAL:
        v->tag = MO_CORE_VALUE_DECIMAL;
        n = mo_cell_get_decimal(cell);
        break;
    case MO_TYPE_BYTE:
        v->tag = MO_CORE_VALUE_BYTE;
        n = mo_cell_get_byte(cell);
        break;
    case MO_TYPE_CHAR:
        v->tag = MO_CORE_VALUE_CHAR;
        v->as.char_ = mo_cell_get_char(cell);
        return v;
    case MO_TYPE_BOOL:
        v->tag = MO_CORE_VALUE_BOOL;
        v->as.bool_ = mo_cell_get_bool(cell);
        return v;
    case MO_TYPE_UNIT:
        v->tag = MO_CORE_VALUE_UNIT;
        return v;
    case MO_TYPE_FLOAT: {
        v->tag = MO_CORE_VALUE_FLOAT;
        double value = mo_cell_get_float(cell);
        memcpy(&v->as.float_, &value, sizeof value);
        return v;
    }
    case MO_TYPE_TEXT:
        v->tag = MO_CORE_VALUE_TEXT;
        v->as.text.bytes = (const char *)mo_value_data(cell, &v->as.text.length);
        return v;
    case MO_TYPE_BYTES: {
        v->tag = MO_CORE_VALUE_BYTES;
        size_t length;
        const uint8_t *bytes = mo_value_data(cell, &length);
        v->as.bytes.count = length;
        v->as.bytes.items = mo_arena_allocate_array(arena, length, sizeof(mo_core_integer), _Alignof(mo_core_integer));
        MO_ASSERT(v->as.bytes.items != NULL);
        for (size_t i = 0; i < length; i++)
            v->as.bytes.items[i] = oracle_value(f, arena, f->builtin[MO_TYPE_BYTE], mo_cell_byte(bytes[i]))->as.byte;
        return v;
    }
    case MO_TYPE_LIST:
    case MO_TYPE_SET:
    case MO_TYPE_TUPLE: {
        enum mo_type_kind kind = f->types.items[type].kind;
        v->tag = kind == MO_TYPE_LIST ? MO_CORE_VALUE_LIST : kind == MO_TYPE_SET ? MO_CORE_VALUE_SET : MO_CORE_VALUE_TUPLE;
        size_t count = kind == MO_TYPE_LIST ? mo_list_size(cell) : kind == MO_TYPE_SET ? mo_table_size(cell) : f->types.items[type].parameter_count;
        v->as.list.count = count;
        v->as.list.items = mo_arena_allocate_array(arena, count, sizeof(mo_core_value), _Alignof(mo_core_value));
        MO_ASSERT(v->as.list.items != NULL);
        uint32_t position = 0;
        for (size_t i = 0; i < count; i++) {
            struct mo_cell element, ignored;
            if (kind == MO_TYPE_LIST) MO_ASSERT(mo_list_get(cell, (uint32_t)i, &element));
            else if (kind == MO_TYPE_SET) MO_ASSERT(mo_table_next(cell, &position, &element, &ignored));
            else element = mo_value_field(cell, (uint32_t)i);
            v->as.list.items[i] = oracle_value(f, arena, f->types.items[type].parameters[kind == MO_TYPE_TUPLE ? i : 0], element);
        }
        return v;
    }
    case MO_TYPE_MAP: {
        v->tag = MO_CORE_VALUE_MAP;
        v->as.map.count = mo_table_size(cell);
        v->as.map.items = mo_arena_allocate_array(arena, v->as.map.count, sizeof *v->as.map.items, _Alignof(struct mo_core_value_pair));
        MO_ASSERT(v->as.map.items != NULL);
        uint32_t position = 0;
        for (size_t i = 0; i < v->as.map.count; i++) {
            struct mo_cell key, value;
            MO_ASSERT(mo_table_next(cell, &position, &key, &value));
            v->as.map.items[i].key = oracle_value(f, arena, f->types.items[type].parameters[0], key);
            v->as.map.items[i].value = oracle_value(f, arena, f->types.items[type].parameters[1], value);
        }
        return v;
    }
    case MO_TYPE_BIGINT: {
        v->tag = MO_CORE_VALUE_BIG_INT;
        struct mo_buffer text;
        mo_buffer_init(&text, MO_LIMIT_STRING_SIZE);
        MO_ASSERT(mo_value_print(f->heap, type, cell, &text));
        bool negative = text.bytes[0] == '-';
        size_t skip = negative ? 1U : 0U;
        char *copy = mo_arena_copy(arena, text.bytes + skip, text.length - skip, 1);
        MO_ASSERT(copy != NULL);
        v->as.big_int = (mo_core_integer){.negative = negative,
                                          .digits = {.bytes = copy, .length = text.length - skip}};
        mo_buffer_free(&text);
        return v;
    }
    case MO_TYPE_CHOICE: {
        v->tag = MO_CORE_VALUE_CON;
        uint32_t tag = mo_cell_get_tag(cell);
        const struct mo_type_case *k = &f->types.items[type].cases[tag];
        v->as.con.ctor = (mo_core_string){k->name, strlen(k->name)};
        v->as.con.fields.count = k->field_count;
        v->as.con.fields.items = mo_arena_allocate_array(
            arena, k->field_count, sizeof(mo_core_value), _Alignof(mo_core_value));
        MO_ASSERT(v->as.con.fields.items != NULL);
        bool boxed = mo_case_shape(&f->types, type, tag) == MO_CASE_BOXED;
        for (uint32_t i = 0; i < k->field_count; i++) {
            struct mo_cell field =
                boxed ? mo_value_field(mo_cell_get_payload(cell), i) : mo_cell_get_payload(cell);
            v->as.con.fields.items[i] = oracle_value(f, arena, k->fields[i].type, field);
        }
        return v;
    }
    default:
        MO_UNREACHABLE();
    }
    char reversed[40], *digits = mo_arena_allocate(arena, 40, 1);
    MO_ASSERT(digits != NULL);
    size_t count = 0;
    mo_u128 magnitude = (mo_u128)(n < 0 ? -n : n);
    do {
        reversed[count++] = (char)('0' + magnitude % 10);
        magnitude /= 10;
    } while (magnitude != 0);
    for (size_t i = 0; i < count; i++)
        digits[i] = reversed[count - i - 1];
    v->as.int_ = (mo_core_integer){.negative = n < 0, .digits = {.bytes = digits, .length = count}};
    return v;
}

static void oracle_query(struct prim_fixture *f, const struct mo_buffer *input) {
    struct mo_arena arena;
    struct mo_diags diags;
    mo_arena_init(&arena, 16 << 20);
    MO_ASSERT(mo_diags_init(&diags));
    mo_core_value query = mo_core_text_parse(&arena, MO_CORE_TEXT_VALUE, (const char *)input->bytes,
                                             input->length, &diags);
    MO_ASSERT(query != NULL && query->tag == MO_CORE_VALUE_TUPLE && query->as.tuple.count >= 2);
    static const char *const names[] = {
#define PRIM(id, module, name, signature, params, bugs) module "." name,
#include "base/prims.def"
    };
    mo_core_string name = query->as.tuple.items[0]->as.text;
    unsigned id = 0;
    for (; id < MO_PRIM_COUNT; id++)
        if (strlen(names[id]) == name.length && memcmp(names[id], name.bytes, name.length) == 0)
            break;
    MO_ASSERT(id < MO_PRIM_COUNT);
    const struct mo_type *signature = &f->types.items[f->signature[id]];
    uint32_t result_type =
        signature->kind == MO_TYPE_FUNCTION ? signature->result : f->signature[id];
    size_t count = query->as.tuple.count - 2;
    MO_ASSERT(count <= 3);
    struct mo_cell args[3], held[3], result = mo_cell_int(41);
    for (size_t i = 0; i < count; i++) {
        args[i] = oracle_cell(f, signature->parameters[i], query->as.tuple.items[i + 2]);
        held[i] = mo_value_dup(f->heap, signature->parameters[i], args[i]);
    }
    const char *reason;
    enum mo_prim_status status = mo_prim_call(f->heap, &f->types, (enum mo_primitive_id)id,
                                              f->signature[id], args, &result, &reason);
    struct mo_buffer text;
    mo_buffer_init(&text, MO_LIMIT_CORE_TEXT_SIZE);
    if (status == MO_PRIM_RETURNED) {
        mo_core_value v = oracle_value(f, &arena, result_type, result);
        MO_ASSERT(mo_core_text_print(MO_CORE_TEXT_VALUE, v, &text));
        MO_ASSERT(fputs("ok ", stdout) >= 0);
        mo_value_drop(f->heap, result_type, result);
    } else {
        struct mo_core_bug bug = {.detail = {reason, reason == NULL ? 0 : strlen(reason)}};
        switch (status) {
        case MO_PRIM_OVERFLOW:
            bug.tag = MO_CORE_BUG_OVERFLOW;
            break;
        case MO_PRIM_DIV_BY_ZERO:
            bug.tag = MO_CORE_BUG_DIV_BY_ZERO;
            break;
        case MO_PRIM_OUTSIDE:
            bug.tag = MO_CORE_BUG_OUTSIDE;
            break;
        case MO_PRIM_REPEATED_KEY:
            bug.tag = MO_CORE_BUG_REPEATED_KEY;
            break;
        case MO_PRIM_RETURNED:
        case MO_PRIM_OUT_OF_MEMORY:
        case MO_PRIM_NOT_IMPLEMENTED:
            MO_UNREACHABLE();
        }
        MO_ASSERT(mo_cell_get_int(result) == 41);
        MO_ASSERT(mo_core_text_print(MO_CORE_TEXT_BUG, &bug, &text));
        MO_ASSERT(fputs("stopped ", stdout) >= 0);
    }
    MO_ASSERT(fwrite(text.bytes, 1, text.length, stdout) == text.length);
    static const char *const ownership[] = {
#define PRIM(id, module, name, signature, params, bugs) params,
#include "base/prims.def"
    };
    for (size_t i = 0; i < count; i++) {
        /* The retained duplicate stays readable even when the call consumed its input. */
        if (status != MO_PRIM_RETURNED || ownership[id][i] == 'b')
            mo_value_drop(f->heap, signature->parameters[i], args[i]);
        mo_value_drop(f->heap, signature->parameters[i], held[i]);
    }
    mo_buffer_free(&text);
    mo_diags_free(&diags);
    mo_arena_free(&arena);
}

static void primitive_oracle(void) {
    struct prim_fixture f;
    prim_fixture_init(&f, MO_DEFAULT_HEAP_SIZE);
    struct mo_buffer input;
    mo_buffer_init(&input, MO_LIMIT_CORE_TEXT_SIZE);
    int byte;
    while ((byte = getchar()) != EOF) {
        MO_ASSERT(mo_buffer_append_byte(&input, (uint8_t)byte));
        if (byte == '\n') {
            oracle_query(&f, &input);
            mo_buffer_clear(&input);
            MO_ASSERT(mo_heap_objects(f.heap) == 0);
        }
    }
    MO_ASSERT(!ferror(stdin) && input.length == 0 && fflush(stdout) == 0);
    mo_buffer_free(&input);
    prim_fixture_free(&f);
}

#endif
