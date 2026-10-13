#include "tools/inputs-internal.h"

#include <string.h>

struct build_frame {
    struct mo_input_value_request request;
    struct mo_input_walk_frame source;
    struct mo_cell *cells;
    uint32_t count;
    uint32_t cursor;
    uint32_t filled;
    uint32_t tag;
    uint32_t special;
    uint32_t variant;
    uint32_t nodes_before;
    size_t bytes_before;
    bool leaf;
};

static MO_MUST_USE uint32_t field_type(const struct mo_inputs *inputs,
                                       const struct build_frame *frame, uint32_t index) {
    const struct mo_type *type = &inputs->types->items[frame->request.type];
    if (type->kind == MO_TYPE_CHOICE)
        return type->cases[frame->tag].fields[index].type;
    if (type->kind == MO_TYPE_MAP)
        return type->parameters[index % 2];
    if (type->kind == MO_TYPE_LIST || type->kind == MO_TYPE_SET)
        return type->parameters[0];
    return inputs->info[frame->request.type].children[index];
}

static MO_MUST_USE uint32_t slot(const struct mo_inputs *inputs, const struct build_frame *frame,
                                 uint32_t child) {
    return inputs->types->items[frame->request.type].kind == MO_TYPE_MAP
               ? child % 2 == 0 ? child / 2 : frame->count / 2 + child / 2
               : child;
}

static void drop_fields(struct mo_input_builder *builder, struct build_frame *frame) {
    for (uint32_t i = 0; i < frame->filled; i++) {
        mo_value_drop(builder->heap, field_type(builder->inputs, frame, i),
                      frame->cells[slot(builder->inputs, frame, i)]);
    }
    frame->filled = 0;
}

static MO_MUST_USE enum mo_input_result special_field(struct mo_input_builder *builder,
                                                      struct build_frame *frame, uint32_t index) {
    const struct mo_input_type_info *info = &builder->inputs->info[frame->request.type];
    for (uint32_t i = 0; i < info->child_count; i++) {
        if (!mo_input_work(&builder->budget, 1))
            return MO_INPUT_LIMIT;
        uint32_t count = mo_input_variants(builder->inputs, frame->request.type, i);
        if (index < count) {
            frame->special = i;
            frame->variant = index + mo_input_skip_base(builder->inputs, info->children[i]);
            return MO_INPUT_OK;
        }
        index -= count;
    }
    MO_ASSERT(false);
    return MO_INPUT_LIMIT;
}

static MO_MUST_USE enum mo_input_result frame_init(struct mo_input_builder *builder,
                                                   struct build_frame *frame, uint32_t depth,
                                                   const struct mo_input_edit *edit) {
    const struct mo_inputs *inputs = builder->inputs;
    const struct mo_type *type = &inputs->types->items[frame->request.type];
    const struct mo_input_type_info *info = &inputs->info[frame->request.type];
    if (builder->budget.left.nodes == 0 || !mo_input_work(&builder->budget, 1))
        return MO_INPUT_LIMIT;
    builder->budget.left.nodes--;
    frame->nodes_before = builder->budget.left.nodes;
    frame->bytes_before = builder->budget.left.bytes;
    uint64_t node = builder->seen++;
    frame->special = MO_NO_INDEX;
    if (frame->request.mode == MO_INPUT_COPY && edit != NULL && !builder->edited &&
        edit->node == node) {
        frame->request.edit = edit;
        builder->edited = true;
        if (edit->kind == MO_INPUT_EDIT_BASE)
            frame->request.mode = MO_INPUT_MINIMUM;
    }
    frame->leaf = info->factory != NULL || mo_input_scalar_count(type->kind) != 0;
    frame->request.remaining_depth = inputs->limits.depth - depth + 1;
    if (frame->leaf)
        return MO_INPUT_OK;
    if (frame->request.mode == MO_INPUT_COPY) {
        enum mo_input_result result = mo_input_frame_init(
            inputs, &frame->source, frame->request.type, frame->request.source, &builder->budget);
        if (result != MO_INPUT_OK)
            return result;
        frame->count = frame->source.count;
        frame->tag = type->kind == MO_TYPE_CHOICE ? mo_cell_get_tag(frame->request.source) : 0;
        if (frame->request.edit != NULL && frame->request.edit->kind == MO_INPUT_EDIT_ERASE) {
            uint32_t erased = frame->request.edit->count;
            if (type->kind == MO_TYPE_MAP)
                erased *= 2;
            MO_ASSERT(erased <= frame->count);
            frame->count -= erased;
        }
    } else if (type->kind == MO_TYPE_CHOICE) {
        frame->tag = info->base_case;
        if (frame->request.mode == MO_INPUT_SAMPLE) {
            if (frame->request.boundary < info->case_count)
                frame->tag = info->case_tags[frame->request.boundary];
            else {
                enum mo_input_result result =
                    special_field(builder, frame, frame->request.boundary - info->case_count);
                if (result != MO_INPUT_OK)
                    return result;
                uint32_t low = 0, high = type->case_count;
                while (low < high) {
                    uint32_t middle = low + (high - low) / 2;
                    if (info->case_offsets[middle] <= frame->special)
                        low = middle + 1;
                    else
                        high = middle;
                }
                MO_ASSERT(low != 0);
                frame->tag = low - 1;
                frame->special -= info->case_offsets[frame->tag];
            }
        } else if (frame->request.mode == MO_INPUT_PRNG) {
            uint32_t start = (uint32_t)(mo_input_random(&builder->random) % info->case_count);
            bool found = false;
            for (uint32_t i = 0; i < info->case_count; i++) {
                uint32_t tag = info->case_tags[(start + i) % info->case_count];
                if (!mo_input_work(&builder->budget, 1))
                    return MO_INPUT_LIMIT;
                if (info->case_depths[tag] <= frame->request.remaining_depth) {
                    frame->tag = tag;
                    found = true;
                    break;
                }
            }
            if (!found)
                return MO_INPUT_LIMIT;
        }
        frame->count = type->cases[frame->tag].field_count;
    } else if (type->kind == MO_TYPE_RECORD || type->kind == MO_TYPE_TUPLE) {
        frame->count = info->child_count;
        if (frame->request.mode == MO_INPUT_SAMPLE && frame->request.boundary != 0) {
            enum mo_input_result result =
                special_field(builder, frame, frame->request.boundary - 1);
            if (result != MO_INPUT_OK)
                return result;
        }
    } else {
        uint32_t count = 0;
        if (frame->request.mode == MO_INPUT_SAMPLE) {
            count = frame->request.boundary == 0 ? 0 : frame->request.boundary == 2 ? 3 : 1;
            if (frame->request.boundary >= 3) {
                enum mo_input_result result =
                    special_field(builder, frame, frame->request.boundary - 3);
                if (result != MO_INPUT_OK)
                    return result;
            }
        } else if (frame->request.mode == MO_INPUT_PRNG) {
            uint32_t maximum = frame->request.size;
            if (maximum > builder->budget.left.nodes / type->parameter_count)
                maximum = builder->budget.left.nodes / type->parameter_count;
            for (uint32_t p = 0; p < type->parameter_count; p++) {
                if (info->factory == NULL &&
                    inputs->info[type->parameters[p]].depth > inputs->limits.depth - depth)
                    maximum = 0;
            }
            count = (uint32_t)(mo_input_random(&builder->random) % ((uint64_t)maximum + 1));
        }
        if (count > inputs->limits.elements)
            return MO_INPUT_LIMIT;
        frame->count = type->kind == MO_TYPE_MAP ? count * 2 : count;
    }
    if (frame->count > builder->budget.left.nodes || !mo_input_work(&builder->budget, frame->count))
        return MO_INPUT_LIMIT;
    if (frame->count != 0) {
        frame->cells =
            mo_scratch_resize(&builder->scratch, NULL, 0, frame->count * sizeof *frame->cells);
        if (frame->cells == NULL)
            return MO_INPUT_NO_MEMORY;
    }
    return MO_INPUT_OK;
}

static MO_MUST_USE bool next_child(struct mo_input_builder *builder, struct build_frame *frame,
                                   struct mo_input_value_request *out) {
    const struct mo_type *type = &builder->inputs->types->items[frame->request.type];
    if (frame->request.mode == MO_INPUT_COPY) {
        if (frame->source.index == frame->source.count)
            return false;
        uint32_t index = frame->source.index;
        out->source = mo_input_child(builder->inputs, &frame->source, &out->type);
        out->mode = MO_INPUT_COPY;
        if (frame->request.edit != NULL && frame->request.edit->kind == MO_INPUT_EDIT_ERASE) {
            const struct mo_input_edit *edit = frame->request.edit;
            uint32_t entry = type->kind == MO_TYPE_MAP ? index / 2 : index;
            if (entry >= edit->start && entry - edit->start < edit->count)
                out->type = MO_NO_INDEX;
        }
        return true;
    }
    if (frame->cursor == frame->count)
        return false;
    uint32_t index = frame->cursor++;
    out->type = field_type(builder->inputs, frame, index);
    out->mode = frame->request.mode == MO_INPUT_PRNG ? MO_INPUT_PRNG : MO_INPUT_MINIMUM;
    /* Share shape/blob size among siblings and reduce it on every descent.
     * Immediate numeric magnitudes still grow with the trial's global size. */
    out->size = frame->request.size / (frame->count + 1);
    uint32_t parameter = type->kind == MO_TYPE_MAP                                 ? index % 2
                         : type->kind == MO_TYPE_LIST || type->kind == MO_TYPE_SET ? 0
                                                                                   : index;
    if (parameter == frame->special) {
        out->mode = MO_INPUT_SAMPLE;
        out->boundary = frame->variant;
    } else if (frame->request.mode == MO_INPUT_SAMPLE && frame->request.boundary == 2 &&
               (type->kind == MO_TYPE_LIST || type->kind == MO_TYPE_SET ||
                type->kind == MO_TYPE_MAP)) {
        const struct mo_input_type_info *child = &builder->inputs->info[out->type];
        uint32_t entry = type->kind == MO_TYPE_MAP ? index / 2 : index;
        out->mode = MO_INPUT_SAMPLE;
        if (child->order >= builder->inputs->info[frame->request.type].order) {
            out->mode = entry % 2 == 0 ? MO_INPUT_MINIMUM : MO_INPUT_SAMPLE;
            out->boundary = mo_input_skip_base(builder->inputs, out->type);
        } else
            out->boundary = entry % child->count;
    }
    return true;
}

static MO_MUST_USE enum mo_input_result finish(struct mo_input_builder *builder,
                                               struct build_frame *frame, struct mo_cell *out) {
    const struct mo_type *type = &builder->inputs->types->items[frame->request.type];
    if (frame->leaf)
        return mo_input_scalar_build(builder, &frame->request, out);
    MO_ASSERT(frame->filled == frame->count);
    bool ok;
    if (type->kind == MO_TYPE_MAP || type->kind == MO_TYPE_SET) {
        /* Charge this table's child trees, not every previous root and not
         * the whole table once per entry. Reserve paired equality walks and
         * hashing for every bounded probe, including adversarial collisions. */
        uint64_t cost = (uint64_t)(frame->nodes_before - builder->budget.left.nodes) +
                        frame->bytes_before - builder->budget.left.bytes;
        if (!mo_input_work(&builder->budget, cost * (2 * MO_LIMIT_HEAP_PROBES + 1)))
            return MO_INPUT_LIMIT;
        ok =
            type->kind == MO_TYPE_MAP
                ? mo_value_map(builder->heap, frame->request.type, frame->cells,
                               frame->count == 0 ? NULL : frame->cells + frame->count / 2,
                               frame->count / 2, out)
                : mo_value_set(builder->heap, frame->request.type, frame->cells, frame->count, out);
    } else if (type->kind == MO_TYPE_LIST) {
        ok = mo_value_list(builder->heap, frame->request.type, frame->cells, frame->count, out);
    } else if (type->kind == MO_TYPE_CHOICE) {
        ok = mo_value_case(builder->heap, frame->request.type, frame->tag, frame->cells,
                           frame->count, out);
    } else {
        ok = mo_value_construct(builder->heap, builder->inputs->info[frame->request.type].layout,
                                frame->cells, frame->count, out);
    }
    return ok ? MO_INPUT_OK : MO_INPUT_NO_MEMORY;
}

enum mo_input_result mo_input_build(const struct mo_inputs *inputs, struct mo_heap *destination,
                                    const struct mo_cell *source, const uint32_t *boundaries,
                                    uint32_t size, uint64_t seed, const struct mo_input_edit *edit,
                                    uint64_t *work_left, struct mo_cell *out) {
    struct mo_input_builder builder = {.inputs = inputs,
                                       .heap = destination,
                                       .budget.left = inputs->limits,
                                       .random = seed,
                                       .size = size};
    builder.budget.left.work = *work_left;
    mo_scratch_init(&builder.scratch, destination);
    struct build_frame stack[MO_LIMIT_INPUT_DEPTH];
    struct mo_cell roots[MO_LIMIT_BYTECODE_PARAMETERS];
    uint32_t depth = 0, completed = 0;
    enum mo_input_result result = MO_INPUT_OK;
    while (completed < inputs->parameter_count) {
        if (depth == 0) {
            stack[depth++] = (struct build_frame){
                .request = {.type = inputs->parameters[completed],
                            .mode = source != NULL       ? MO_INPUT_COPY
                                    : boundaries != NULL ? MO_INPUT_SAMPLE
                                                         : MO_INPUT_PRNG,
                            .boundary = boundaries == NULL ? 0 : boundaries[completed],
                            .size = size,
                            .source = source == NULL ? mo_cell_unit() : source[completed]}};
            if (inputs->limits.depth == 0) {
                result = MO_INPUT_LIMIT;
                break;
            }
            result = frame_init(&builder, &stack[0], depth, edit);
            if (result != MO_INPUT_OK)
                break;
        }
        struct build_frame *frame = &stack[depth - 1];
        struct mo_input_value_request child = {0};
        if (!frame->leaf && next_child(&builder, frame, &child)) {
            if (child.type == MO_NO_INDEX)
                continue;
            if (depth == inputs->limits.depth) {
                result = MO_INPUT_LIMIT;
                break;
            }
            stack[depth++] = (struct build_frame){.request = child};
            result = frame_init(&builder, &stack[depth - 1], depth, edit);
            if (result != MO_INPUT_OK)
                break;
            continue;
        }
        struct mo_cell value;
        result = finish(&builder, frame, &value);
        if (result != MO_INPUT_OK)
            break;
        drop_fields(&builder, frame);
        depth--;
        if (depth == 0)
            roots[completed++] = value;
        else {
            struct build_frame *parent = &stack[depth - 1];
            MO_ASSERT(parent->filled < parent->count);
            parent->cells[slot(inputs, parent, parent->filled++)] = value;
        }
    }
    while (depth != 0)
        drop_fields(&builder, &stack[--depth]);
    if (result == MO_INPUT_OK) {
        MO_ASSERT(edit == NULL || builder.edited);
        if (completed != 0)
            memcpy(out, roots, completed * sizeof *out);
    } else {
        for (uint32_t i = 0; i < completed; i++)
            mo_value_drop(destination, inputs->parameters[i], roots[i]);
    }
    mo_scratch_free(&builder.scratch);
    *work_left = builder.budget.left.work;
    return result;
}
