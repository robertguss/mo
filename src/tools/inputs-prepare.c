#include "tools/inputs-internal.h"

#include <string.h>

uint32_t mo_input_scalar_count(enum mo_type_kind kind) {
    switch (kind) {
    case MO_TYPE_UNIT:
        return 1;
    case MO_TYPE_BOOL:
        return 2;
    case MO_TYPE_BYTE:
        return 3;
    case MO_TYPE_INT:
        return 5;
    case MO_TYPE_FLOAT:
        return 11;
    case MO_TYPE_CHAR:
        return 6;
    case MO_TYPE_TIME:
        return 5;
    case MO_TYPE_DURATION:
        return 3;
    case MO_TYPE_DECIMAL:
        return 6;
    case MO_TYPE_RANGE:
        return 6;
    case MO_TYPE_TEXT:
    case MO_TYPE_BYTES:
        return 4;
    case MO_TYPE_BIGINT:
        return 5;
    case MO_TYPE_TUPLE:
    case MO_TYPE_LIST:
    case MO_TYPE_SET:
    case MO_TYPE_MAP:
    case MO_TYPE_RECORD:
    case MO_TYPE_CHOICE:
    case MO_TYPE_FUNCTION:
    case MO_TYPE_ENV:
    case MO_TYPE_CAPABILITY:
        return 0;
    }
    MO_ASSERT(false);
    return 0;
}

uint32_t mo_input_skip_base(const struct mo_inputs *inputs, uint32_t type) {
    return inputs->info[type].factory == NULL &&
                   inputs->types->items[type].kind == MO_TYPE_CHOICE &&
                   inputs->info[type].case_tags[0] != inputs->info[type].base_case
               ? 0
               : 1;
}

uint32_t mo_input_variants(const struct mo_inputs *inputs, uint32_t parent, uint32_t field) {
    const struct mo_input_type_info *container = &inputs->info[parent];
    uint32_t child = container->children[field];
    const struct mo_input_type_info *info = &inputs->info[child];
    if (container->depth > inputs->limits.depth || info->depth >= inputs->limits.depth ||
        info->count == 0)
        return 0;
    enum mo_type_kind kind = inputs->types->items[parent].kind;
    if ((kind == MO_TYPE_LIST || kind == MO_TYPE_MAP || kind == MO_TYPE_SET) &&
        container->count == 1)
        return 0;
    if (kind == MO_TYPE_CHOICE) {
        uint32_t low = 0, high = inputs->types->items[parent].case_count;
        while (low < high) {
            uint32_t middle = low + (high - low) / 2;
            if (container->case_offsets[middle] <= field)
                low = middle + 1;
            else
                high = middle;
        }
        MO_ASSERT(low != 0);
        if (container->case_depths[low - 1] > inputs->limits.depth)
            return 0;
    }
    if (info->order != 0 && info->order < inputs->info[parent].order) {
        return info->count - mo_input_skip_base(inputs, child);
    }
    /* A back edge varies only to the first non-minimum shell. Expanding all
     * descendants here would give a recursive type infinitely many boundaries. */
    return info->count > 1 || info->child_count != 0 ? 1 : 0;
}

static MO_MUST_USE enum mo_input_result edges(struct mo_inputs *inputs, uint32_t index,
                                              struct mo_input_budget *budget) {
    struct mo_input_type_info *info = &inputs->info[index];
    const struct mo_type *type = &inputs->types->items[index];
    if (info->factory != NULL) {
        info->depth = 1;
        info->count = info->factory->boundaries;
        info->hashable = info->factory->hashable;
        return MO_INPUT_OK;
    }
    info->hashable = type->kind != MO_TYPE_FLOAT;
    info->count = mo_input_scalar_count(type->kind);
    info->depth = info->count == 0 ? MO_NO_INDEX : 1;
    uint64_t count = 0;
    switch (type->kind) {
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
        return MO_INPUT_OK;
    case MO_TYPE_FUNCTION:
    case MO_TYPE_ENV:
    case MO_TYPE_CAPABILITY:
        return MO_INPUT_UNSUPPORTED;
    case MO_TYPE_TUPLE:
    case MO_TYPE_LIST:
    case MO_TYPE_SET:
    case MO_TYPE_MAP:
        count = type->parameter_count;
        if (type->kind != MO_TYPE_TUPLE) {
            info->depth = 1;
            info->count = 3;
        } else {
            info->count = 1;
        }
        break;
    case MO_TYPE_RECORD:
        count = type->field_count;
        info->count = 1;
        break;
    case MO_TYPE_CHOICE:
        if (!mo_input_work(budget, type->case_count))
            return MO_INPUT_LIMIT;
        info->case_offsets = mo_arena_allocate_array(&inputs->arena, (size_t)type->case_count + 1,
                                                     sizeof(uint32_t), _Alignof(uint32_t));
        info->case_layouts = mo_arena_allocate_array(&inputs->arena, type->case_count,
                                                     sizeof(uint32_t), _Alignof(uint32_t));
        info->case_tags = mo_arena_allocate_array(&inputs->arena, type->case_count,
                                                  sizeof(uint32_t), _Alignof(uint32_t));
        info->case_depths = mo_arena_allocate_array(&inputs->arena, type->case_count,
                                                    sizeof(uint32_t), _Alignof(uint32_t));
        if (info->case_offsets == NULL || info->case_layouts == NULL || info->case_tags == NULL ||
            info->case_depths == NULL)
            return MO_INPUT_NO_MEMORY;
        for (uint32_t i = 0; i < type->case_count; i++) {
            info->case_offsets[i] = (uint32_t)count;
            info->case_layouts[i] = MO_NO_INDEX;
            count += type->cases[i].field_count;
            if (count > UINT32_MAX)
                return MO_INPUT_LIMIT;
        }
        info->case_offsets[type->case_count] = (uint32_t)count;
        info->count = type->case_count;
        break;
    }
    if (!mo_input_work(budget, count))
        return MO_INPUT_LIMIT;
    info->child_count = (uint32_t)count;
    if (count != 0) {
        info->children = mo_arena_allocate_array(&inputs->arena, (size_t)count, sizeof(uint32_t),
                                                 _Alignof(uint32_t));
        if (info->children == NULL)
            return MO_INPUT_NO_MEMORY;
    }
    if (type->kind == MO_TYPE_RECORD) {
        for (uint32_t i = 0; i < info->child_count; i++)
            info->children[i] = type->fields[i].type;
    } else if (type->kind == MO_TYPE_CHOICE) {
        for (uint32_t c = 0; c < type->case_count; c++) {
            for (uint32_t f = 0; f < type->cases[c].field_count; f++) {
                info->children[info->case_offsets[c] + f] = type->cases[c].fields[f].type;
            }
        }
    } else if (count != 0) {
        memcpy(info->children, type->parameters, (size_t)count * sizeof(uint32_t));
    }
    return MO_INPUT_OK;
}

static MO_MUST_USE uint32_t fields_depth(const struct mo_inputs *inputs,
                                         const struct mo_input_type_info *info, uint32_t start,
                                         uint32_t end) {
    uint32_t depth = 0;
    for (uint32_t i = start; i < end; i++) {
        uint32_t child = inputs->info[info->children[i]].depth;
        if (child == MO_NO_INDEX)
            return MO_NO_INDEX;
        if (child > depth)
            depth = child;
    }
    return depth < MO_LIMIT_INPUT_DEPTH ? depth + 1 : MO_LIMIT_INPUT_DEPTH + 1;
}

static MO_MUST_USE enum mo_input_result ranks(struct mo_inputs *inputs, const uint32_t *graph,
                                              uint32_t count, struct mo_input_budget *budget) {
    bool settled = false;
    for (uint32_t pass = 0; pass <= MO_LIMIT_INPUT_DEPTH && !settled; pass++) {
        bool changed = false;
        for (uint32_t i = 0; i < count; i++) {
            struct mo_input_type_info *info = &inputs->info[graph[i]];
            const struct mo_type *type = &inputs->types->items[graph[i]];
            if (!mo_input_work(budget, (uint64_t)info->child_count + type->case_count + 1)) {
                return MO_INPUT_LIMIT;
            }
            if (info->factory != NULL)
                continue;
            if (info->hashable) {
                for (uint32_t c = 0; c < info->child_count; c++) {
                    if (!inputs->info[info->children[c]].hashable) {
                        info->hashable = false;
                        changed = true;
                        break;
                    }
                }
            }
            uint32_t depth = info->depth, base = info->base_case;
            if (type->kind == MO_TYPE_RECORD || type->kind == MO_TYPE_TUPLE) {
                depth = fields_depth(inputs, info, 0, info->child_count);
            } else if (type->kind == MO_TYPE_CHOICE) {
                depth = MO_NO_INDEX;
                for (uint32_t c = 0; c < type->case_count; c++) {
                    uint32_t candidate = fields_depth(inputs, info, info->case_offsets[c],
                                                      info->case_offsets[c + 1]);
                    if (candidate < depth) {
                        depth = candidate;
                        base = c;
                    }
                }
            }
            if (depth != info->depth || base != info->base_case) {
                info->depth = depth;
                info->base_case = base;
                changed = true;
            }
        }
        settled = !changed;
    }
    if (!settled)
        return MO_INPUT_LIMIT;
    for (uint32_t i = 0; i < inputs->parameter_count; i++) {
        uint32_t depth = inputs->info[inputs->parameters[i]].depth;
        if (depth == MO_NO_INDEX)
            return MO_INPUT_UNINHABITED;
        if (depth > inputs->limits.depth)
            return MO_INPUT_LIMIT;
    }
    for (uint32_t i = 0; i < count; i++) {
        struct mo_input_type_info *info = &inputs->info[graph[i]];
        const struct mo_type *type = &inputs->types->items[graph[i]];
        if (!mo_input_work(budget, (uint64_t)info->child_count + type->case_count + 1))
            return MO_INPUT_LIMIT;
        if (info->depth > inputs->limits.depth) {
            info->count = 0;
            continue;
        }
        if (info->factory != NULL)
            continue;
        if (type->kind == MO_TYPE_CHOICE) {
            for (uint32_t c = 0; c < type->case_count; c++) {
                info->case_depths[c] =
                    fields_depth(inputs, info, info->case_offsets[c], info->case_offsets[c + 1]);
                if (info->case_depths[c] <= inputs->limits.depth)
                    info->case_tags[info->case_count++] = c;
            }
            info->count = info->case_count;
        } else if (type->kind == MO_TYPE_LIST || type->kind == MO_TYPE_SET ||
                   type->kind == MO_TYPE_MAP) {
            for (uint32_t c = 0; c < info->child_count; c++)
                if (inputs->info[info->children[c]].depth >= inputs->limits.depth)
                    info->count = 1;
        }
        if (info->factory == NULL && (type->kind == MO_TYPE_MAP || type->kind == MO_TYPE_SET) &&
            !inputs->info[type->parameters[0]].hashable)
            return MO_INPUT_UNSUPPORTED;
    }
    return MO_INPUT_OK;
}

struct count_frame {
    uint32_t type, child;
};

static MO_MUST_USE enum mo_input_result counts(struct mo_inputs *inputs, const uint32_t *graph,
                                               uint32_t count, struct mo_input_budget *budget) {
    struct count_frame *stack =
        mo_arena_allocate_array(&inputs->arena, count, sizeof *stack, _Alignof(struct count_frame));
    if (stack == NULL)
        return MO_INPUT_NO_MEMORY;
    uint32_t order = 0;
    for (uint32_t root = 0; root < count; root++) {
        if (inputs->info[graph[root]].visit != 0)
            continue;
        uint32_t depth = 1;
        stack[0] = (struct count_frame){.type = graph[root]};
        inputs->info[graph[root]].visit = 1;
        while (depth != 0) {
            if (!mo_input_work(budget, 1))
                return MO_INPUT_LIMIT;
            struct count_frame *frame = &stack[depth - 1];
            struct mo_input_type_info *info = &inputs->info[frame->type];
            if (frame->child < info->child_count) {
                uint32_t child = info->children[frame->child++];
                if (inputs->info[child].visit == 0) {
                    MO_ASSERT(depth < count);
                    inputs->info[child].visit = 1;
                    stack[depth++] = (struct count_frame){.type = child};
                }
                continue;
            }
            info->order = ++order;
            for (uint32_t c = 0; c < info->child_count; c++) {
                uint64_t total = (uint64_t)info->count + mo_input_variants(inputs, frame->type, c);
                info->count = total > UINT32_MAX ? UINT32_MAX : (uint32_t)total;
            }
            info->visit = 2;
            depth--;
        }
    }
    return MO_INPUT_OK;
}

enum mo_input_result mo_input_prepare(struct mo_inputs *inputs,
                                      const struct mo_input_factory *factories,
                                      uint32_t factory_count, size_t name_work) {
    if (inputs->parameter_count == 0)
        return MO_INPUT_OK;
    const struct mo_types *types = inputs->types;
    struct mo_input_budget budget = {.left.work = inputs->limits.prepare_work - name_work};
    if (!mo_input_work(&budget, types->count))
        return MO_INPUT_LIMIT;
    inputs->info = mo_arena_allocate_array(&inputs->arena, types->count, sizeof *inputs->info,
                                           _Alignof(struct mo_input_type_info));
    uint32_t *graph =
        mo_arena_allocate_array(&inputs->arena, types->count, sizeof(uint32_t), _Alignof(uint32_t));
    if (inputs->info == NULL || graph == NULL)
        return MO_INPUT_NO_MEMORY;
    memset(inputs->info, 0, types->count * sizeof *inputs->info);
    for (uint32_t i = 0; i < types->count; i++) {
        inputs->info[i].depth = MO_NO_INDEX;
        inputs->info[i].base_case = MO_NO_INDEX;
        inputs->info[i].layout = MO_NO_INDEX;
    }
    for (uint32_t i = 0; i < factory_count; i++) {
        MO_ASSERT(factories[i].type < types->count && factories[i].boundaries != 0 &&
                  factories[i].build != NULL && inputs->info[factories[i].type].factory == NULL);
        inputs->info[factories[i].type].factory = &factories[i];
    }
    uint32_t count = 0;
    for (uint32_t i = 0; i < inputs->parameter_count; i++) {
        uint32_t type = inputs->parameters[i];
        if (!inputs->info[type].reachable) {
            inputs->info[type].reachable = true;
            graph[count++] = type;
        }
    }
    for (uint32_t i = 0; i < count; i++) {
        enum mo_input_result result = edges(inputs, graph[i], &budget);
        if (result != MO_INPUT_OK)
            return result;
        const struct mo_input_type_info *info = &inputs->info[graph[i]];
        for (uint32_t c = 0; c < info->child_count; c++) {
            uint32_t child = info->children[c];
            if (!inputs->info[child].reachable) {
                inputs->info[child].reachable = true;
                graph[count++] = child;
            }
        }
    }
    enum mo_input_result result = ranks(inputs, graph, count, &budget);
    if (result != MO_INPUT_OK)
        return result;
    if (!mo_input_work(&budget, types->layout_count))
        return MO_INPUT_LIMIT;
    for (uint32_t i = 0; i < types->layout_count; i++) {
        const struct mo_layout *layout = &types->layouts[i];
        struct mo_input_type_info *info = &inputs->info[layout->type];
        if (!info->reachable || info->factory != NULL)
            continue;
        if (layout->kind == MO_LAYOUT_RECORD || layout->kind == MO_LAYOUT_TUPLE) {
            info->layout = i;
        } else if (layout->kind == MO_LAYOUT_CASE) {
            info->case_layouts[layout->case_index] = i;
        }
    }
    for (uint32_t i = 0; i < count; i++) {
        const struct mo_type *type = &types->items[graph[i]];
        const struct mo_input_type_info *info = &inputs->info[graph[i]];
        if (info->factory != NULL || info->depth > inputs->limits.depth)
            continue;
        if ((type->kind == MO_TYPE_RECORD || type->kind == MO_TYPE_TUPLE) &&
            info->layout == MO_NO_INDEX)
            return MO_INPUT_UNSUPPORTED;
        if (type->kind == MO_TYPE_CHOICE) {
            if (!mo_input_work(&budget, type->case_count))
                return MO_INPUT_LIMIT;
            for (uint32_t c = 0; c < type->case_count; c++) {
                if (info->case_depths[c] <= inputs->limits.depth &&
                    mo_case_shape(types, graph[i], c) == MO_CASE_BOXED &&
                    info->case_layouts[c] == MO_NO_INDEX)
                    return MO_INPUT_UNSUPPORTED;
            }
        }
    }
    return counts(inputs, graph, count, &budget);
}
