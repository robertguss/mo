#include "tools/inputs-internal.h"

#include <string.h>

enum mo_input_result mo_input_frame_init(const struct mo_inputs *inputs,
                                         struct mo_input_walk_frame *frame, uint32_t type_index,
                                         struct mo_cell value, struct mo_input_budget *budget) {
    *frame = (struct mo_input_walk_frame){.value = value, .type = type_index};
    if (inputs->info[type_index].factory != NULL)
        return MO_INPUT_OK;
    const struct mo_type *type = &inputs->types->items[type_index];
    switch (type->kind) {
    case MO_TYPE_RECORD:
        frame->count = type->field_count;
        break;
    case MO_TYPE_TUPLE:
        frame->count = type->parameter_count;
        break;
    case MO_TYPE_CHOICE: {
        uint32_t tag = mo_cell_get_tag(value);
        MO_ASSERT(tag < type->case_count);
        frame->count = type->cases[tag].field_count;
        break;
    }
    case MO_TYPE_LIST:
        frame->count = mo_list_size(value);
        if (frame->count > budget->left.elements)
            return MO_INPUT_LIMIT;
        break;
    case MO_TYPE_SET:
    case MO_TYPE_MAP:
        frame->count = mo_table_size(value);
        if (frame->count > budget->left.elements)
            return MO_INPUT_LIMIT;
        if (type->kind == MO_TYPE_MAP)
            frame->count *= 2;
        break;
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
    case MO_TYPE_FUNCTION:
    case MO_TYPE_ENV:
    case MO_TYPE_CAPABILITY:
        return MO_INPUT_UNSUPPORTED;
    }
    return MO_INPUT_OK;
}

struct mo_cell mo_input_child(const struct mo_inputs *inputs, struct mo_input_walk_frame *frame,
                              uint32_t *child_type) {
    MO_ASSERT(frame->index < frame->count);
    const struct mo_type *type = &inputs->types->items[frame->type];
    uint32_t index = frame->index++;
    if (type->kind == MO_TYPE_CHOICE) {
        uint32_t tag = mo_cell_get_tag(frame->value);
        *child_type = type->cases[tag].fields[index].type;
        struct mo_cell payload = mo_cell_get_payload(frame->value);
        return mo_case_shape(inputs->types, frame->type, tag) == MO_CASE_BOXED
                   ? mo_value_field(payload, index)
                   : payload;
    }
    if (type->kind == MO_TYPE_LIST) {
        struct mo_cell result;
        *child_type = type->parameters[0];
        MO_ASSERT(mo_list_get(frame->value, index, &result));
        return result;
    }
    if (type->kind == MO_TYPE_SET || type->kind == MO_TYPE_MAP) {
        if (type->kind == MO_TYPE_MAP && index % 2 != 0) {
            *child_type = type->parameters[1];
            return frame->pending;
        }
        struct mo_cell key;
        MO_ASSERT(mo_table_next(frame->value, &frame->position, &key, &frame->pending));
        *child_type = type->parameters[0];
        return key;
    }
    *child_type = inputs->info[frame->type].children[index];
    return mo_value_field(frame->value, index);
}

enum mo_input_result mo_input_walk_next(struct mo_input_walk *walk, uint32_t *type,
                                        struct mo_cell *value) {
    while (walk->parameter < walk->end) {
        if (walk->depth != 0) {
            struct mo_input_walk_frame *frame = &walk->stack[walk->depth - 1];
            if (!frame->emitted) {
                frame->emitted = true;
                *type = frame->type;
                *value = frame->value;
                return MO_INPUT_OK;
            }
            if (frame->index == frame->count) {
                walk->depth--;
                if (walk->depth == 0)
                    walk->parameter++;
                continue;
            }
            *value = mo_input_child(walk->inputs, frame, type);
        } else {
            *type = walk->inputs->parameters[walk->parameter];
            *value = walk->roots[walk->parameter];
        }
        if (walk->depth == walk->inputs->limits.depth || walk->budget.left.nodes == 0 ||
            !mo_input_work(&walk->budget, 1))
            return MO_INPUT_LIMIT;
        walk->budget.left.nodes--;
        enum mo_input_result result = mo_input_frame_init(walk->inputs, &walk->stack[walk->depth++],
                                                          *type, *value, &walk->budget);
        if (result != MO_INPUT_OK)
            return result;
        enum mo_type_kind kind = walk->inputs->types->items[*type].kind;
        if (walk->inputs->info[*type].factory == NULL &&
            (kind == MO_TYPE_TEXT || kind == MO_TYPE_BYTES || kind == MO_TYPE_BIGINT)) {
            size_t length;
            const uint8_t *bytes = mo_value_data(*value, &length);
            (void)bytes;
            if (!mo_input_bytes(&walk->budget, length))
                return MO_INPUT_LIMIT;
        }
    }
    return MO_INPUT_DONE;
}

static MO_MUST_USE bool erase(uint32_t length, uint32_t variant, struct mo_input_edit *edit) {
    if (length == 0)
        return false;
    if (variant == 0) {
        edit->start = 0;
        edit->count = length;
        return true;
    }
    variant--;
    for (uint32_t chunk = length == 1 ? 0 : (length + 1) / 2; chunk != 0;
         chunk = chunk == 1 ? 0 : (chunk + 1) / 2) {
        uint32_t pieces = 1 + (length - 1) / chunk;
        if (variant < pieces) {
            edit->start = variant * chunk;
            edit->count = length - edit->start < chunk ? length - edit->start : chunk;
            return true;
        }
        variant -= pieces;
    }
    return false;
}

static MO_MUST_USE enum mo_input_result candidate(struct mo_input_walk *walk, uint32_t type_index,
                                                  struct mo_cell value, uint32_t variant,
                                                  struct mo_input_edit *edit) {
    const struct mo_input_type_info *info = &walk->inputs->info[type_index];
    if (info->factory != NULL) {
        if (info->factory->shrink == NULL || variant == UINT32_MAX)
            return MO_INPUT_DONE;
        edit->kind = MO_INPUT_EDIT_FACTORY;
        return MO_INPUT_OK;
    }
    enum mo_type_kind kind = walk->inputs->types->items[type_index].kind;
    uint32_t count = 3;
    uint32_t length = 0;
    switch (kind) {
    case MO_TYPE_UNIT:
    case MO_TYPE_RECORD:
    case MO_TYPE_TUPLE:
    case MO_TYPE_FUNCTION:
    case MO_TYPE_ENV:
    case MO_TYPE_CAPABILITY:
        return MO_INPUT_DONE;
    case MO_TYPE_BOOL:
        count = 1;
        break;
    case MO_TYPE_BYTE:
        count = 9;
        break;
    case MO_TYPE_INT:
    case MO_TYPE_TIME:
    case MO_TYPE_DURATION:
        count = 65;
        break;
    case MO_TYPE_FLOAT:
        count = 75;
        break;
    case MO_TYPE_CHAR:
        count = 22;
        break;
    case MO_TYPE_DECIMAL:
        count = 129;
        break;
    case MO_TYPE_RANGE:
        count = 130;
        break;
    case MO_TYPE_BIGINT: {
        size_t bytes;
        const uint8_t *data = mo_value_data(value, &bytes);
        if (bytes == 0)
            return MO_INPUT_DONE;
        count = 2 + (uint32_t)bytes * 8;
        for (; edit->variant < count; edit->variant++) {
            if (!mo_input_work(&walk->budget, bytes))
                return MO_INPUT_LIMIT;
            if (mo_input_bigint_smaller(data, bytes, edit->variant, NULL)) {
                edit->kind = MO_INPUT_EDIT_SCALAR;
                return MO_INPUT_OK;
            }
        }
        return MO_INPUT_DONE;
    }
    case MO_TYPE_TEXT:
    case MO_TYPE_BYTES: {
        size_t bytes;
        const uint8_t *data = mo_value_data(value, &bytes);
        if (kind == MO_TYPE_TEXT) {
            if (!mo_input_work(&walk->budget, bytes))
                return MO_INPUT_LIMIT;
            for (size_t i = 0; i < bytes; i++)
                if ((data[i] & 0xc0) != 0x80)
                    length++;
        } else
            length = (uint32_t)bytes;
        break;
    }
    case MO_TYPE_LIST:
        length = mo_list_size(value);
        break;
    case MO_TYPE_SET:
    case MO_TYPE_MAP:
        length = mo_table_size(value);
        break;
    case MO_TYPE_CHOICE:
        if (variant != 0 || mo_cell_get_tag(value) == info->base_case)
            return MO_INPUT_DONE;
        edit->kind = MO_INPUT_EDIT_BASE;
        return MO_INPUT_OK;
    }
    if (kind == MO_TYPE_TEXT || kind == MO_TYPE_BYTES || kind == MO_TYPE_LIST ||
        kind == MO_TYPE_SET || kind == MO_TYPE_MAP) {
        edit->kind = MO_INPUT_EDIT_ERASE;
        return erase(length, variant, edit) ? MO_INPUT_OK : MO_INPUT_DONE;
    }
    if (variant >= count)
        return MO_INPUT_DONE;
    edit->kind = MO_INPUT_EDIT_SCALAR;
    if (kind != MO_TYPE_BIGINT) {
        struct mo_input_builder builder = {.inputs = walk->inputs};
        struct mo_input_value_request request = {
            .type = type_index, .mode = MO_INPUT_COPY, .source = value, .edit = edit};
        struct mo_cell ignored;
        for (; edit->variant < count; edit->variant++) {
            if (!mo_input_work(&walk->budget, 1))
                return MO_INPUT_LIMIT;
            enum mo_input_result result = mo_input_immediate_build(&builder, &request, &ignored);
            if (result != MO_INPUT_DONE)
                return result;
        }
        return MO_INPUT_DONE;
    }
    return MO_INPUT_OK;
}

void mo_input_shrink_init(struct mo_input_shrinker *shrinker, const struct mo_inputs *inputs,
                          uint32_t budget) {
    MO_ASSERT(shrinker != NULL && inputs != NULL);
    *shrinker = (struct mo_input_shrinker){.inputs = inputs, .budget = budget};
}

enum mo_input_result mo_input_shrink_next(struct mo_input_shrinker *shrinker,
                                          const struct mo_cell *best, struct mo_heap *destination,
                                          struct mo_cell *out) {
    MO_ASSERT(shrinker != NULL && !shrinker->awaiting && destination != NULL);
    const struct mo_inputs *inputs = shrinker->inputs;
    MO_ASSERT((best != NULL && out != NULL) || inputs->parameter_count == 0);
    if (shrinker->calls == shrinker->budget)
        return MO_INPUT_DONE;
    struct mo_input_shrinker cursor = *shrinker;
    struct mo_input_walk walk = {.inputs = inputs,
                                 .roots = best,
                                 .end = inputs->parameter_count,
                                 .budget.left = inputs->limits};
    uint64_t node = 0;
    for (;;) {
        uint32_t type;
        struct mo_cell value;
        enum mo_input_result result = mo_input_walk_next(&walk, &type, &value);
        if (result == MO_INPUT_DONE && cursor.changed) {
            /* Later changes can make an earlier node reducible again. Keep
             * this call's work charge when restarting the bounded walk. */
            uint64_t work = walk.budget.left.work;
            walk = (struct mo_input_walk){.inputs = inputs,
                                          .roots = best,
                                          .end = inputs->parameter_count,
                                          .budget.left = inputs->limits};
            walk.budget.left.work = work;
            cursor.node = node = 0;
            cursor.variant = 0;
            cursor.changed = false;
            continue;
        }
        if (result == MO_INPUT_DONE)
            *shrinker = cursor;
        if (result != MO_INPUT_OK)
            return result;
        if (node++ < cursor.node)
            continue;
        for (;;) {
            if (!mo_input_work(&walk.budget, 1))
                return MO_INPUT_LIMIT;
            struct mo_input_edit edit = {.node = cursor.node, .variant = cursor.variant};
            result = candidate(&walk, type, value, cursor.variant, &edit);
            if (result == MO_INPUT_DONE) {
                cursor.node++;
                cursor.variant = 0;
                break;
            }
            if (result != MO_INPUT_OK)
                return result;
            cursor.variant = edit.variant;
            struct mo_input_limits left = inputs->limits;
            left.work = walk.budget.left.work;
            result = mo_input_build(inputs, destination, best, NULL, 0, 0, &edit, &left, out);
            walk.budget.left.work = left.work;
            if (result == MO_INPUT_DONE) {
                if (edit.kind == MO_INPUT_EDIT_FACTORY) {
                    cursor.node++;
                    cursor.variant = 0;
                    break;
                }
                cursor.variant++;
                continue;
            }
            if (result != MO_INPUT_OK)
                return result;
            cursor.calls++;
            cursor.awaiting = true;
            cursor.numeric = edit.kind == MO_INPUT_EDIT_SCALAR;
            *shrinker = cursor;
            return MO_INPUT_OK;
        }
    }
}

void mo_input_shrink_feedback(struct mo_input_shrinker *shrinker, bool still_fails) {
    MO_ASSERT(shrinker != NULL && shrinker->awaiting);
    shrinker->awaiting = false;
    if (still_fails) {
        shrinker->changed = true;
        if (!shrinker->numeric)
            shrinker->variant = 0;
    } else
        shrinker->variant++;
}

enum mo_input_result mo_inputs_extreme(const struct mo_inputs *inputs, const struct mo_cell *values,
                                       uint32_t parameter, struct mo_input_extreme *out) {
    MO_ASSERT(inputs != NULL && parameter < inputs->parameter_count && values != NULL &&
              out != NULL);
    struct mo_input_walk walk = {.inputs = inputs,
                                 .roots = values,
                                 .parameter = parameter,
                                 .end = parameter + 1,
                                 .budget.left = inputs->limits};
    for (;;) {
        uint32_t type;
        struct mo_cell value;
        enum mo_input_result result = mo_input_walk_next(&walk, &type, &value);
        if (result != MO_INPUT_OK)
            return result;
        if (inputs->info[type].factory != NULL)
            continue;
        enum mo_type_kind kind = inputs->types->items[type].kind;
        enum mo_input_end end;
        if (kind == MO_TYPE_INT || kind == MO_TYPE_DURATION || kind == MO_TYPE_RANGE) {
            int64_t number;
            if (kind == MO_TYPE_RANGE) {
                struct mo_range range = mo_cell_get_range(value);
                number =
                    range.start == INT64_MIN || range.start == INT64_MAX ? range.start : range.end;
                kind = MO_TYPE_INT;
            } else
                number = mo_cell_get_int(value);
            if (number != INT64_MIN && number != INT64_MAX)
                continue;
            end = number == INT64_MIN ? MO_INPUT_SMALLEST : MO_INPUT_LARGEST;
        } else if (kind == MO_TYPE_TIME) {
            int64_t number = mo_cell_get_time(value);
            if (number != INT64_C(-62135596800000000) && number != INT64_C(253402300799999999))
                continue;
            end = number < 0 ? MO_INPUT_SMALLEST : MO_INPUT_LARGEST;
        } else if (kind == MO_TYPE_DECIMAL) {
            mo_i128 max = (mo_i128)(((mo_u128)1 << 127) - 1);
            mo_i128 number = mo_cell_get_decimal(value);
            if (number != -max && number != max)
                continue;
            end = number < 0 ? MO_INPUT_SMALLEST : MO_INPUT_LARGEST;
        } else
            continue;
        *out = (struct mo_input_extreme){.parameter = parameter, .type = kind, .end = end};
        return MO_INPUT_OK;
    }
}
