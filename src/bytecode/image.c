#include "bytecode/verify.h"

struct mo_image {
    struct mo_arena arena;
    const struct mo_bytecode *file;
    const uint32_t *code_offsets;
    struct mo_types types;
    struct mo_verification verified;
};

struct mo_image *mo_load(const uint8_t *bytes, size_t length, struct mo_load_error *error_out) {
    MO_ASSERT(error_out != NULL && (bytes != NULL || length == 0));
    struct mo_arena arena;
    mo_arena_init(&arena, length <= MO_LIMIT_BYTECODE_SIZE ? mo_bytecode_work_budget(length) : 0);
    const uint32_t *offsets = NULL;
    struct mo_types types;
    struct mo_verification verified;
    struct mo_bytecode *file =
        mo_bytecode_decode_info(&arena, bytes, length, &offsets, &types, error_out);
    if (file == NULL ||
        !mo_bytecode_verify(file, &types, offsets, length, &arena, &verified, error_out)) {
        mo_arena_free(&arena);
        return NULL;
    }
    struct mo_image *image = mo_arena_allocate(&arena, sizeof *image, _Alignof(struct mo_image));
    if (image == NULL) {
        *error_out = (struct mo_load_error){.reason = "no memory", .kind = MO_BYTECODE_NO_MEMORY};
        mo_arena_free(&arena);
        return NULL;
    }
    *image = (struct mo_image){arena, file, offsets, types, verified};
    return image;
}

const struct mo_bytecode *mo_image_bytecode(const struct mo_image *image) {
    MO_ASSERT(image != NULL);
    return image->file;
}

bool mo_image_registers_readable(const struct mo_image *image, uint32_t function, uint32_t pc,
                                 bool *readable, struct mo_load_error *error) {
    MO_ASSERT(image != NULL && readable != NULL && error != NULL);
    if (function >= image->file->function_count) {
        *error = (struct mo_load_error){.reason = "invalid replay function"};
        return false;
    }
    uint64_t work = 0;
    return mo_bytecode_readable(image->file, &image->types, &image->verified, function,
                                image->code_offsets[function], pc, readable, &work, error);
}

bool mo_image_visit_readable(const struct mo_image *image, uint32_t function, bool *readable,
                             mo_readable_visitor visit, void *context,
                             struct mo_load_error *error) {
    MO_ASSERT(image != NULL && readable != NULL && visit != NULL && error != NULL);
    if (function >= image->file->function_count) {
        *error = (struct mo_load_error){.reason = "invalid replay function"};
        return false;
    }
    return mo_bytecode_visit_readable(image->file, &image->types, &image->verified, function,
                                      image->code_offsets[function], readable, visit, context,
                                      error);
}

void mo_image_free(struct mo_image *image) {
    MO_ASSERT(image != NULL);
    struct mo_arena arena = image->arena;
    mo_arena_free(&arena);
}
