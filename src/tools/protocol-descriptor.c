#include "tools/protocol.h"

#include <string.h>

struct descriptor_builder {
    struct mo_heap *heap;
    const struct mo_types *types;
    const struct mo_bytecode *source;
    const struct mo_bytecode_service *service;
    uint32_t *ids, *queue, count;
    uint32_t root, kinds, kind, tools, tool, fields, field, cases, case_type, payload, ints;
};

static uint32_t field_type(const struct descriptor_builder *b, uint32_t type, uint32_t tag,
                           uint32_t field) {
    return b->types->items[type].cases[tag].fields[field].type;
}

static uint32_t type_id(struct descriptor_builder *b, uint32_t source) {
    MO_ASSERT(source < b->source->type_count);
    if (b->ids[source] == MO_NO_INDEX) {
        MO_ASSERT(b->count < b->source->type_count);
        b->ids[source] = b->count;
        b->queue[b->count++] = source;
    }
    return b->ids[source];
}

static bool finish_case(struct descriptor_builder *b, uint32_t type, uint32_t tag,
                        struct mo_cell *fields, uint32_t count, struct mo_cell *out) {
    bool ok = mo_value_case(b->heap, type, tag, fields, count, out);
    for (uint32_t i = 0; i < count; i++)
        mo_value_drop(b->heap, field_type(b, type, tag, i), fields[i]);
    return ok;
}

static bool append(struct descriptor_builder *b, uint32_t type, struct mo_cell *list,
                   struct mo_cell value) {
    bool ok = mo_list_append(b->heap, type, list, value);
    mo_value_drop(b->heap, b->types->items[type].parameters[0], value);
    return ok;
}

static bool append_field(struct descriptor_builder *b, struct mo_cell *list, const char *name,
                         size_t length, uint32_t source) {
    struct mo_cell fields[2], value;
    if (!mo_value_text(b->heap, name, length, &fields[0]))
        return false;
    fields[1] = mo_cell_int(type_id(b, source));
    return finish_case(b, b->field, 0, fields, 2, &value) && append(b, b->fields, list, value);
}

static bool make_fields(struct descriptor_builder *b, const struct mo_type_field *fields,
                        uint32_t count, struct mo_cell *out) {
    struct mo_cell list;
    if (!mo_value_list(b->heap, b->fields, NULL, 0, &list))
        return false;
    for (uint32_t i = 0; i < count; i++) {
        if (!append_field(b, &list, fields[i].name, strlen(fields[i].name), fields[i].type)) {
            mo_value_drop(b->heap, b->fields, list);
            return false;
        }
    }
    *out = list;
    return true;
}

static bool make_cases(struct descriptor_builder *b, const struct mo_type *type,
                       const struct mo_bytecode_service_shape *shape, struct mo_cell *out) {
    struct mo_cell list;
    if (!mo_value_list(b->heap, b->cases, NULL, 0, &list))
        return false;
    for (uint32_t i = 0; i < type->case_count; i++) {
        const struct mo_type_case *item = &type->cases[i];
        struct mo_cell fields[2], value;
        if (item->field_count == 0)
            fields[1] = mo_cell_case(0, mo_cell_unit());
        else if (shape->unnamed[i])
            fields[1] = mo_cell_case(1, mo_cell_int(type_id(b, item->fields[0].type)));
        else {
            struct mo_cell named;
            if (!make_fields(b, item->fields, item->field_count, &named) ||
                !finish_case(b, b->payload, 2, &named, 1, &fields[1]))
                goto fail;
        }
        if (!mo_value_text(b->heap, item->name, strlen(item->name), &fields[0])) {
            mo_value_drop(b->heap, b->payload, fields[1]);
            goto fail;
        }
        if (!finish_case(b, b->case_type, 0, fields, 2, &value) ||
            !append(b, b->cases, &list, value))
            goto fail;
    }
    *out = list;
    return true;
fail:
    mo_value_drop(b->heap, b->cases, list);
    return false;
}

static bool nominal(struct descriptor_builder *b, uint32_t source, struct mo_cell *out) {
    const struct mo_type *type = &b->source->types[source];
    uint32_t low = 0, high = b->service->shape_count;
    while (low < high) {
        uint32_t mid = low + (high - low) / 2;
        if (b->service->shapes[mid].type < source)
            low = mid + 1;
        else
            high = mid;
    }
    MO_ASSERT(low < b->service->shape_count && b->service->shapes[low].type == source);
    const struct mo_bytecode_service_shape *shape = &b->service->shapes[low];
    struct mo_cell value;
    if (!shape->is_struct) {
        if (!make_cases(b, type, shape, &value))
            return false;
        return finish_case(b, b->kind, 18, &value, 1, out);
    }
    const struct mo_type_field *fields =
        type->kind == MO_TYPE_RECORD ? type->fields : type->cases[0].fields;
    uint32_t count = type->kind == MO_TYPE_RECORD ? type->field_count : type->cases[0].field_count;
    if (!make_fields(b, fields, count, &value))
        return false;
    return finish_case(b, b->kind, 17, &value, 1, out);
}

static bool make_kind(struct descriptor_builder *b, uint32_t source, struct mo_cell *out) {
    const struct mo_type *type = &b->source->types[source];
    uint32_t tag = 0;
    switch (type->kind) {
    case MO_TYPE_UNIT:
        tag = 0;
        break;
    case MO_TYPE_BOOL:
        tag = 1;
        break;
    case MO_TYPE_BYTE:
        tag = 2;
        break;
    case MO_TYPE_INT:
        tag = 3;
        break;
    case MO_TYPE_FLOAT:
        tag = 4;
        break;
    case MO_TYPE_CHAR:
        tag = 5;
        break;
    case MO_TYPE_TIME:
        tag = 6;
        break;
    case MO_TYPE_DURATION:
        tag = 7;
        break;
    case MO_TYPE_DECIMAL:
        tag = 8;
        break;
    case MO_TYPE_TEXT:
        tag = 9;
        break;
    case MO_TYPE_BYTES:
        tag = 10;
        break;
    case MO_TYPE_BIGINT:
        tag = 11;
        break;
    case MO_TYPE_RANGE:
        tag = 16;
        break;
    case MO_TYPE_RECORD:
    case MO_TYPE_CHOICE:
        return nominal(b, source, out);
    case MO_TYPE_TUPLE: {
        struct mo_cell list;
        if (!mo_value_list(b->heap, b->ints, NULL, 0, &list))
            return false;
        for (uint32_t i = 0; i < type->parameter_count; i++) {
            if (!mo_list_append(b->heap, b->ints, &list,
                                mo_cell_int(type_id(b, type->parameters[i])))) {
                mo_value_drop(b->heap, b->ints, list);
                return false;
            }
        }
        return finish_case(b, b->kind, 12, &list, 1, out);
    }
    case MO_TYPE_LIST:
    case MO_TYPE_SET:
    case MO_TYPE_MAP: {
        struct mo_cell fields[2];
        for (uint32_t i = 0; i < type->parameter_count; i++)
            fields[i] = mo_cell_int(type_id(b, type->parameters[i]));
        tag = type->kind == MO_TYPE_LIST ? 13U : type->kind == MO_TYPE_SET ? 14U : 15U;
        return mo_value_case(b->heap, b->kind, tag, fields, type->parameter_count, out);
    }
    case MO_TYPE_FUNCTION:
    case MO_TYPE_ENV:
    case MO_TYPE_CAPABILITY:
        MO_ASSERT(false);
        return false;
    }
    *out = mo_cell_case(tag, mo_cell_unit());
    return true;
}

static bool make_tool(struct descriptor_builder *b, const struct mo_bytecode_service_tool *tool,
                      struct mo_cell *out) {
    struct mo_cell fields[6];
    uint32_t made = 0;
    const uint32_t strings[] = {tool->name, tool->description};
    for (uint32_t i = 0; i < 2; i++) {
        const struct mo_bytecode_string *text = &b->source->strings[strings[i]];
        if (!mo_value_text(b->heap, (const char *)text->bytes, text->length, &fields[i]))
            goto fail;
        made++;
    }
    fields[made++] = mo_cell_bool(tool->read_only);
    if (!mo_value_list(b->heap, b->fields, NULL, 0, &fields[3]))
        goto fail;
    made++;
    for (uint32_t i = 0; i < tool->argument_count; i++) {
        const struct mo_bytecode_string *name = &b->source->strings[tool->argument_names[i]];
        if (!append_field(b, &fields[3], (const char *)name->bytes, name->length,
                          tool->argument_types[i]))
            goto fail;
    }
    uint32_t reply = tool->result_type;
    const struct mo_type *result = &b->source->types[reply];
    fields[5] = mo_cell_case(0, mo_cell_unit());
    if (result->kind == MO_TYPE_CHOICE && strncmp(result->name, "Result(", 7) == 0 &&
        result->case_count == 2 && strcmp(result->cases[0].name, "Ok") == 0 &&
        strcmp(result->cases[1].name, "Error") == 0 && result->cases[0].unnamed &&
        result->cases[1].unnamed) {
        reply = result->cases[0].fields[0].type;
        fields[5] = mo_cell_case(1, mo_cell_int(type_id(b, result->cases[1].fields[0].type)));
    }
    if (!tool->read_only)
        reply = b->source->types[reply].parameters[1];
    fields[4] = mo_cell_int(type_id(b, reply));
    return finish_case(b, b->tool, 0, fields, 6, out);
fail:
    for (uint32_t i = 0; i < made; i++)
        mo_value_drop(b->heap, field_type(b, b->tool, 0, i), fields[i]);
    return false;
}

bool mo_protocol_descriptor(const struct mo_image *protocol, struct mo_heap *heap,
                            const struct mo_image *service, uint32_t index,
                            struct mo_protocol_descriptor *out) {
    const struct mo_bytecode *file = mo_image_bytecode(protocol);
    struct descriptor_builder b = {.heap = heap,
                                   .types = mo_image_types(protocol),
                                   .source = mo_image_bytecode(service),
                                   .root = MO_NO_INDEX};
    MO_ASSERT(index < b.source->service_count);
    b.service = &b.source->services[index];
    for (uint32_t i = 0; i < file->function_count; i++) {
        const struct mo_bytecode_function *function = &file->functions[i];
        const struct mo_bytecode_string *name = &file->strings[function->name];
        if (name->length == sizeof "Mcp.catalog" - 1 &&
            memcmp(name->bytes, "Mcp.catalog", name->length) == 0)
            b.root = function->parameters[0].type;
    }
    MO_ASSERT(b.root != MO_NO_INDEX);
    b.kinds = field_type(&b, b.root, 0, 0);
    b.kind = b.types->items[b.kinds].parameters[0];
    b.tools = field_type(&b, b.root, 0, 1);
    b.tool = b.types->items[b.tools].parameters[0];
    b.fields = field_type(&b, b.tool, 0, 3);
    b.field = b.types->items[b.fields].parameters[0];
    b.cases = field_type(&b, b.kind, 18, 0);
    b.case_type = b.types->items[b.cases].parameters[0];
    b.payload = field_type(&b, b.case_type, 0, 1);
    b.ints = field_type(&b, b.kind, 12, 0);
    struct mo_scratch scratch;
    mo_scratch_init(&scratch, heap);
    b.ids = mo_scratch_resize(&scratch, NULL, 0, (size_t)b.source->type_count * 2 * sizeof *b.ids);
    bool ok = false;
    if (b.ids == NULL)
        goto done;
    b.queue = b.ids + b.source->type_count;
    for (uint32_t i = 0; i < b.source->type_count; i++)
        b.ids[i] = MO_NO_INDEX;
    struct mo_cell fields[2], value;
    if (!mo_value_list(heap, b.tools, NULL, 0, &fields[1]))
        goto done;
    for (uint32_t i = 0; i < b.service->tool_count; i++) {
        if (!make_tool(&b, &b.service->tools[i], &value) || !append(&b, b.tools, &fields[1], value))
            goto tools;
    }
    if (!mo_value_list(heap, b.kinds, NULL, 0, &fields[0]))
        goto tools;
    for (uint32_t i = 0; i < b.count; i++) {
        if (!make_kind(&b, b.queue[i], &value) || !append(&b, b.kinds, &fields[0], value))
            goto kinds;
    }
    ok = mo_value_case(heap, b.root, 0, fields, 2, &value);
    if (ok)
        *out = (struct mo_protocol_descriptor){b.root, value};
kinds:
    mo_value_drop(heap, b.kinds, fields[0]);
tools:
    mo_value_drop(heap, b.tools, fields[1]);
done:
    mo_scratch_free(&scratch);
    return ok;
}
