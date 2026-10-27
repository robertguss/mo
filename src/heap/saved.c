#include "heap/internal.h"

#include <string.h>

#include "base/binary.h"

struct saved_output {
    struct mo_buffer *bytes;
    const char *failure;
};

static bool saved_append(struct saved_output *out, const void *bytes, size_t size) {
    if (size > out->bytes->limit - out->bytes->length) {
        out->failure = "MO_LIMIT_SAVED_BYTES (16777216)";
        return false;
    }
    if (!mo_buffer_append(out->bytes, bytes, size)) {
        out->failure = "allocation";
        return false;
    }
    return true;
}

static bool saved_uint(struct saved_output *out, uint64_t value, unsigned width) {
    uint8_t bytes[8];
    MO_ASSERT(width <= sizeof bytes);
    for (unsigned i = 0; i < width; i++) {
        bytes[i] = (uint8_t)value;
        value >>= 8;
    }
    return saved_append(out, bytes, width);
}

struct schema_writer {
    const struct mo_types *types;
    struct saved_output *out;
    uint32_t *seen, next;
};

static bool name(struct saved_output *out, const char *text) {
    size_t length = text == NULL ? 0 : strlen(text);
    return length <= UINT32_MAX && saved_uint(out, length, 4) && saved_append(out, text, length);
}

/* The saved format's fixed v1 tags, independent of native enum order. */
#define SAVED_NO_TAG 255
static uint8_t saved_tag(enum mo_type_kind kind) {
    switch (kind) {
    case MO_TYPE_UNIT:
        return 0;
    case MO_TYPE_BOOL:
        return 1;
    case MO_TYPE_BYTE:
        return 2;
    case MO_TYPE_INT:
        return 3;
    case MO_TYPE_FLOAT:
        return 4;
    case MO_TYPE_CHAR:
        return 5;
    case MO_TYPE_TIME:
        return 6;
    case MO_TYPE_DURATION:
        return 7;
    case MO_TYPE_DECIMAL:
        return 8;
    case MO_TYPE_RANGE:
        return 9;
    case MO_TYPE_TEXT:
        return 10;
    case MO_TYPE_BYTES:
        return 11;
    case MO_TYPE_BIGINT:
        return 12;
    case MO_TYPE_TUPLE:
        return 13;
    case MO_TYPE_LIST:
        return 14;
    case MO_TYPE_SET:
        return 15;
    case MO_TYPE_MAP:
        return 16;
    case MO_TYPE_RECORD:
        return 17;
    case MO_TYPE_CHOICE:
        return 18;
    case MO_TYPE_FUNCTION:
    case MO_TYPE_ENV:
    case MO_TYPE_CAPABILITY:
        return SAVED_NO_TAG;
    }
    MO_UNREACHABLE();
}

static bool schema(struct schema_writer *w, uint32_t index, unsigned depth) {
    if (depth == MO_LIMIT_SAVED_DEPTH) {
        w->out->failure = "MO_LIMIT_SAVED_DEPTH (128)";
        return false;
    }
    if (w->seen[index] != MO_NO_INDEX)
        return saved_uint(w->out, 255, 1) && saved_uint(w->out, w->seen[index], 4);
    w->seen[index] = w->next++;
    const struct mo_type *t = &w->types->items[index];
    uint8_t tag = saved_tag(t->kind);
    if (tag == SAVED_NO_TAG || !saved_uint(w->out, tag, 1))
        return false;
    if (t->kind == MO_TYPE_RECORD || t->kind == MO_TYPE_CHOICE) {
        if (!name(w->out, t->name))
            return false;
    }
    if (!saved_uint(w->out, t->parameter_count, 4))
        return false;
    for (uint32_t i = 0; i < t->parameter_count; i++)
        if (!schema(w, t->parameters[i], depth + 1))
            return false;
    uint32_t groups = t->kind == MO_TYPE_CHOICE ? t->case_count : 1;
    if (!saved_uint(w->out, groups, 4))
        return false;
    for (uint32_t c = 0; c < groups; c++) {
        uint32_t count = t->field_count;
        const struct mo_type_field *fields = t->fields;
        if (t->kind == MO_TYPE_CHOICE) {
            count = t->cases[c].field_count;
            fields = t->cases[c].fields;
            if (!name(w->out, t->cases[c].name) ||
                !saved_uint(w->out, t->cases[c].unnamed ? 1 : 0, 1))
                return false;
        }
        if (!saved_uint(w->out, count, 4))
            return false;
        for (uint32_t i = 0; i < count; i++)
            if (!name(w->out, fields[i].name) || !schema(w, fields[i].type, depth + 1))
                return false;
    }
    return true;
}

static bool write_schema(struct mo_heap *heap, uint32_t type, struct saved_output *out) {
    struct mo_scratch scratch;
    mo_scratch_init(&scratch, heap);
    uint32_t *seen =
        mo_scratch_resize(&scratch, NULL, 0, (size_t)heap->types->count * sizeof *seen);
    bool ok = seen != NULL;
    if (!ok)
        out->failure = "allocation";
    if (ok) {
        for (uint32_t i = 0; i < heap->types->count; i++)
            seen[i] = MO_NO_INDEX;
        struct schema_writer w = {heap->types, out, seen, 0};
        ok = schema(&w, type, 0);
    }
    mo_scratch_free(&scratch);
    return ok;
}

static bool encode(struct mo_heap *heap, uint32_t type, struct mo_cell value,
                   struct saved_output *out, unsigned depth, uint32_t *nodes) {
    if (depth == MO_LIMIT_SAVED_DEPTH || *nodes == MO_LIMIT_SAVED_NODES) {
        out->failure = depth == MO_LIMIT_SAVED_DEPTH ? "MO_LIMIT_SAVED_DEPTH (128)"
                                                     : "MO_LIMIT_SAVED_NODES (1048576)";
        return false;
    }
    (*nodes)++;
    const struct mo_type *t = &heap->types->items[type];
    switch (t->kind) {
    case MO_TYPE_UNIT:
        return true;
    case MO_TYPE_BOOL:
    case MO_TYPE_BYTE:
        return saved_uint(out, mo_cell_get_byte(value), 1);
    case MO_TYPE_CHAR:
        return saved_uint(out, mo_cell_get_char(value), 4);
    case MO_TYPE_INT:
    case MO_TYPE_TIME:
    case MO_TYPE_DURATION:
    case MO_TYPE_FLOAT: {
        uint64_t bits;
        memcpy(&bits, value.bytes, 8);
        return saved_uint(out, bits, 8);
    }
    case MO_TYPE_DECIMAL: {
        mo_u128 bits = (mo_u128)mo_cell_get_decimal(value);
        return saved_uint(out, (uint64_t)bits, 8) && saved_uint(out, (uint64_t)(bits >> 64), 8);
    }
    case MO_TYPE_RANGE: {
        struct mo_range r = mo_cell_get_range(value);
        return saved_uint(out, (uint64_t)r.start, 8) && saved_uint(out, (uint64_t)r.end, 8);
    }
    case MO_TYPE_TEXT:
    case MO_TYPE_BYTES:
    case MO_TYPE_BIGINT: {
        size_t length;
        const uint8_t *bytes = mo_value_data(value, &length);
        return (t->kind != MO_TYPE_BIGINT ||
                saved_uint(out, (uint64_t)mo_value_bigint_sign(value), 1)) &&
               saved_uint(out, length, 4) && saved_append(out, bytes, length);
    }
    case MO_TYPE_MAP:
    case MO_TYPE_SET: {
        uint32_t count = mo_table_size(value), position = 0;
        if (!saved_uint(out, count, 4))
            return false;
        struct mo_cell key, item;
        while (mo_table_next(value, &position, &key, &item)) {
            if (!encode(heap, t->parameters[0], key, out, depth + 1, nodes) ||
                (t->kind == MO_TYPE_MAP &&
                 !encode(heap, t->parameters[1], item, out, depth + 1, nodes)))
                return false;
        }
        return true;
    }
    case MO_TYPE_CHOICE:
    case MO_TYPE_RECORD:
    case MO_TYPE_TUPLE:
    case MO_TYPE_LIST: {
        if (t->kind == MO_TYPE_CHOICE && !saved_uint(out, mo_cell_get_tag(value), 4))
            return false;
        uint32_t count = mo_value_children(heap->types, type, value);
        if (!saved_uint(out, count, 4))
            return false;
        for (uint32_t i = 0; i < count; i++) {
            uint32_t child_type;
            struct mo_cell child = mo_value_child(heap->types, type, value, i, &child_type);
            if (!encode(heap, child_type, child, out, depth + 1, nodes))
                return false;
        }
        return true;
    }
    case MO_TYPE_FUNCTION:
    case MO_TYPE_ENV:
    case MO_TYPE_CAPABILITY:
        return false;
    }
    MO_UNREACHABLE();
}

static uint32_t child_type(const struct mo_type *t, uint32_t tag, uint32_t i) {
    if (t->kind == MO_TYPE_CHOICE)
        return t->cases[tag].fields[i].type;
    if (t->kind == MO_TYPE_RECORD)
        return t->fields[i].type;
    if (t->kind == MO_TYPE_TUPLE)
        return t->parameters[i];
    return t->parameters[t->kind == MO_TYPE_MAP ? i % 2 : 0];
}

struct decoder {
    struct mo_heap *heap;
    struct mo_binary input;
    uint32_t nodes;
};

static bool decode(struct decoder *d, uint32_t type, unsigned depth, struct mo_cell *out) {
    if (depth == MO_LIMIT_SAVED_DEPTH || d->nodes == MO_LIMIT_SAVED_NODES)
        return false;
    d->nodes++;
    const struct mo_type *t = &d->heap->types->items[type];
    uint64_t a = 0, b = 0;
    unsigned width = 0;
    switch (t->kind) {
    case MO_TYPE_UNIT:
        *out = mo_cell_unit();
        return true;
    case MO_TYPE_BOOL:
    case MO_TYPE_BYTE:
        width = 1;
        break;
    case MO_TYPE_CHAR:
        width = 4;
        break;
    case MO_TYPE_INT:
    case MO_TYPE_FLOAT:
    case MO_TYPE_TIME:
    case MO_TYPE_DURATION:
    case MO_TYPE_DECIMAL:
    case MO_TYPE_RANGE:
        width = 8;
        break;
    case MO_TYPE_TEXT:
    case MO_TYPE_BYTES:
    case MO_TYPE_BIGINT: {
        const uint8_t *bytes;
        if ((t->kind == MO_TYPE_BIGINT && !mo_binary_uint(&d->input, 1, &b)) ||
            !mo_binary_uint(&d->input, 4, &a) || !mo_binary_read(&d->input, (size_t)a, &bytes))
            return false;
        if (t->kind == MO_TYPE_TEXT)
            return mo_value_text(d->heap, (const char *)bytes, (size_t)a, out);
        if (t->kind == MO_TYPE_BYTES)
            return mo_value_bytes(d->heap, bytes, (size_t)a, out);
        return b <= MO_BIGINT_NEGATIVE &&
               mo_value_bigint(d->heap, (enum mo_bigint_sign)b, bytes, (size_t)a, out);
    }
    case MO_TYPE_LIST:
    case MO_TYPE_SET:
    case MO_TYPE_MAP:
    case MO_TYPE_RECORD:
    case MO_TYPE_TUPLE:
    case MO_TYPE_CHOICE:
        break;
    case MO_TYPE_FUNCTION:
    case MO_TYPE_ENV:
    case MO_TYPE_CAPABILITY:
        return false;
    }
    if (width != 0) {
        if (!mo_binary_uint(&d->input, width, &a))
            return false;
        if (t->kind == MO_TYPE_BOOL && a > 1)
            return false;
        if (t->kind == MO_TYPE_CHAR && (a >= 0x110000 || (a >= 0xd800 && a <= 0xdfff)))
            return false;
        int64_t signed_a;
        memcpy(&signed_a, &a, 8);
        if (t->kind == MO_TYPE_TIME && (signed_a < MO_TIME_MIN_US || signed_a > MO_TIME_MAX_US))
            return false;
        struct mo_cell result = {0};
        if (width == 1)
            result = mo_cell_byte((uint8_t)a);
        else if (width == 4)
            result = mo_cell_char((uint32_t)a);
        else
            memcpy(result.bytes, &a, 8);
        if (t->kind == MO_TYPE_DECIMAL || t->kind == MO_TYPE_RANGE) {
            if (!mo_binary_uint(&d->input, 8, &b))
                return false;
            if (t->kind == MO_TYPE_DECIMAL) {
                mo_u128 bits = ((mo_u128)b << 64) | a;
                if (bits == ((mo_u128)1 << 127))
                    return false;
                memcpy(result.bytes, &bits, 16);
            } else
                memcpy(result.bytes + 8, &b, 8);
        }
        *out = result;
        return true;
    }
    uint64_t tag = 0, count;
    if ((t->kind == MO_TYPE_CHOICE &&
         (!mo_binary_uint(&d->input, 4, &tag) || tag >= t->case_count)) ||
        !mo_binary_uint(&d->input, 4, &count))
        return false;
    if ((t->kind == MO_TYPE_CHOICE && count != t->cases[tag].field_count) ||
        (t->kind == MO_TYPE_RECORD && count != t->field_count) ||
        (t->kind == MO_TYPE_TUPLE && count != t->parameter_count) ||
        count > MO_LIMIT_COLLECTION_SIZE)
        return false;
    uint64_t cells = t->kind == MO_TYPE_MAP ? count * 2 : count;
    if (cells > MO_LIMIT_SAVED_NODES - d->nodes)
        return false;
    struct mo_scratch scratch;
    mo_scratch_init(&scratch, d->heap);
    struct mo_cell *values =
        mo_scratch_resize(&scratch, NULL, 0, (size_t)(cells + 1) * sizeof *values);
    uint32_t built = 0;
    bool ok = values != NULL;
    while (ok && built < cells) {
        ok = decode(d, child_type(t, (uint32_t)tag, built), depth + 1, &values[built]);
        if (ok)
            built++;
    }
    struct mo_cell result;
    if (ok) {
        if (t->kind == MO_TYPE_LIST)
            ok = mo_value_list(d->heap, type, values, (uint32_t)count, &result);
        else if (t->kind == MO_TYPE_SET) {
            ok = mo_value_set(d->heap, type, values, (uint32_t)count, &result);
            if (ok && mo_table_size(result) != count) {
                mo_value_drop(d->heap, type, result);
                ok = false;
            }
        } else if (t->kind == MO_TYPE_MAP) {
            ok = mo_value_map(d->heap, type, NULL, NULL, 0, &result);
            if (ok) {
                for (uint32_t i = 0; ok && i < count; i++)
                    ok = mo_table_set(d->heap, type, &result, values[2 * i], values[2 * i + 1]);
                ok = ok && mo_table_size(result) == count;
                if (!ok)
                    mo_value_drop(d->heap, type, result);
            }
        } else if (t->kind == MO_TYPE_CHOICE)
            ok = mo_value_case(d->heap, type, (uint32_t)tag, values, (uint32_t)count, &result);
        else {
            struct mo_layout_lookup *layout = mo_heap_layout(d->heap, 0, type, MO_NO_INDEX);
            ok = layout != NULL &&
                 mo_value_construct(d->heap, layout->index, values, (uint32_t)count, &result);
        }
    }
    for (uint32_t i = 0; i < built; i++)
        mo_value_drop(d->heap, child_type(t, (uint32_t)tag, i), values[i]);
    mo_scratch_free(&scratch);
    if (ok)
        *out = result;
    return ok;
}

static bool encode_value(struct mo_heap *heap, uint32_t type, struct mo_cell value,
                         struct mo_buffer *out, const char **failure) {
    MO_ASSERT(type < heap->types->count);
    struct mo_buffer bytes, shape;
    mo_buffer_init(&bytes, MO_LIMIT_SAVED_BYTES);
    mo_buffer_init(&shape, MO_LIMIT_SAVED_BYTES);
    struct saved_output output = {&bytes, NULL}, schema_output = {&shape, NULL};
    uint32_t nodes = 0;
    bool ok = write_schema(heap, type, &schema_output) && saved_append(&output, "MOSV\1", 5) &&
              saved_uint(&output, shape.length, 4) &&
              saved_append(&output, shape.bytes, shape.length) &&
              encode(heap, type, value, &output, 0, &nodes);
    if (ok)
        ok = mo_buffer_append(out, bytes.bytes, bytes.length);
    *failure = schema_output.failure != NULL ? schema_output.failure
               : output.failure != NULL      ? output.failure
                                             : "allocation";
    mo_buffer_free(&shape);
    mo_buffer_free(&bytes);
    return ok;
}

bool mo_value_encode(struct mo_heap *heap, uint32_t type, struct mo_cell value,
                     struct mo_buffer *out) {
    const char *failure;
    return encode_value(heap, type, value, out, &failure);
}

bool mo_value_copy_saved(struct mo_heap *from, struct mo_heap *to, uint32_t type,
                         struct mo_cell value, struct mo_cell *out, size_t *encoded_length,
                         const char **failure) {
    MO_ASSERT(from->image == to->image && encoded_length != NULL && failure != NULL);
    struct mo_buffer bytes;
    mo_buffer_init(&bytes, MO_LIMIT_SAVED_BYTES);
    bool ok = encode_value(from, type, value, &bytes, failure);
    if (ok) {
        ok = mo_value_decode(to, type, bytes.bytes, bytes.length, out);
        if (!ok)
            *failure = "allocation (--max-heap or native memory)";
    }
    if (ok)
        *encoded_length = bytes.length;
    mo_buffer_free(&bytes);
    return ok;
}

bool mo_value_decode(struct mo_heap *heap, uint32_t type, const uint8_t *bytes, size_t length,
                     struct mo_cell *out) {
    MO_ASSERT(type < heap->types->count && (bytes != NULL || length == 0));
    if (length > MO_LIMIT_SAVED_BYTES || length < 9 || memcmp(bytes, "MOSV\1", 5) != 0)
        return false;
    struct decoder d = {.heap = heap, .input = {bytes, length, 5}};
    uint64_t shape_length;
    const uint8_t *shape_bytes;
    if (!mo_binary_uint(&d.input, 4, &shape_length) ||
        !mo_binary_read(&d.input, (size_t)shape_length, &shape_bytes))
        return false;
    struct mo_buffer shape;
    mo_buffer_init(&shape, MO_LIMIT_SAVED_BYTES);
    struct saved_output schema_output = {&shape, NULL};
    bool ok = write_schema(heap, type, &schema_output) && shape.length == shape_length &&
              memcmp(shape.bytes, shape_bytes, shape.length) == 0;
    mo_buffer_free(&shape);
    struct mo_cell value;
    if (ok)
        ok = decode(&d, type, 0, &value);
    if (ok && d.input.position != length) {
        mo_value_drop(heap, type, value);
        ok = false;
    }
    if (ok)
        *out = value;
    return ok;
}
