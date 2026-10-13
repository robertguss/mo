#include "vm/internal.h"

#include <stdlib.h>
#include <string.h>

static void release_image(void *pointer) {
    struct mo_vm_image *runtime = pointer;
    mo_vm_dispose(&runtime->initializer);
    mo_heap_image_free(runtime->heap);
    free(runtime);
}

static MO_MUST_USE bool constant(struct mo_process *p, const struct mo_bytecode_constant *c,
                                 struct mo_cell *out) {
    switch (c->encoding) {
    case MO_CONSTANT_IMMEDIATE64:
    case MO_CONSTANT_DECIMAL128: {
        mo_u128 bits = 0;
        for (unsigned i = 0; i < 16; i++)
            bits |= (mo_u128)c->value.immediate[i] << (8 * i);
        *out = mo_cell_unit();
        if (c->encoding == MO_CONSTANT_DECIMAL128)
            memcpy(out->bytes, &bits, sizeof bits);
        else {
            uint64_t low = (uint64_t)bits;
            memcpy(out->bytes, &low, sizeof low);
        }
        return true;
    }
    case MO_CONSTANT_TEXT: {
        const struct mo_bytecode_string *s = &p->file->strings[c->value.string];
        return mo_value_text(p->heap, (const char *)s->bytes, s->length, out);
    }
    case MO_CONSTANT_BYTES:
        return mo_value_bytes(p->heap, c->value.bytes.bytes, c->value.bytes.length, out);
    case MO_CONSTANT_BIGINT:
        return mo_value_bigint(p->heap, c->value.bigint.sign, c->value.bigint.magnitude.bytes,
                               c->value.bigint.magnitude.length, out);
    case MO_CONSTANT_CLOSURE:
        return mo_value_closure_owned(p->heap, c->value.function, NULL, 0, out);
    }
    MO_UNREACHABLE();
}

enum mo_outcome mo_image_init(struct mo_image *image, const struct mo_limits *limits,
                              struct mo_defect *defect_out) {
    MO_ASSERT(image != NULL && limits != NULL && defect_out != NULL);
    struct mo_vm_image *runtime = mo_image_runtime(image);
    if (runtime != NULL) {
        if (runtime->outcome != MO_RETURNED)
            *defect_out = runtime->initializer.defect;
        return runtime->outcome;
    }
    const struct mo_bytecode *file = mo_image_bytecode(image);
    size_t cells = (size_t)file->constant_count + file->global_count;
    size_t bytes = sizeof *runtime + cells * sizeof(struct mo_cell) + file->global_count;
    if (bytes > limits->heap_size || (runtime = calloc(1, bytes)) == NULL) {
        *defect_out = (struct mo_defect){.reason = "running out of memory",
                                         .limit_name = "--max-heap",
                                         .limit = limits->heap_size,
                                         .span = {.file = MO_NO_INDEX}};
        return MO_STOPPED;
    }
    runtime->outcome = MO_STOPPED;
    runtime->constants = (struct mo_cell *)(runtime + 1);
    runtime->globals = runtime->constants + file->constant_count;
    runtime->set = (bool *)(runtime->globals + file->global_count);
    struct mo_process *p = &runtime->initializer;
    p->image = image;
    p->file = file;
    p->types = mo_image_types(image);
    p->runtime = runtime;
    p->limits = *limits;
    mo_image_attach_runtime(image, runtime, release_image);
    struct mo_limits heap_limits = *limits;
    heap_limits.heap_size -= bytes;
    runtime->heap = mo_heap_image_new(p->types, &heap_limits);
    if (runtime->heap == NULL) {
        runtime->outcome =
            mo_vm_stop(p, "running out of memory", "--max-heap", false, mo_cell_unit(), MO_STOPPED);
        *defect_out = p->defect;
        return runtime->outcome;
    }
    p->heap = mo_heap_image_constants(runtime->heap);
    for (uint32_t i = 0; i < file->constant_count; i++) {
        if (!constant(p, &file->constants[i], &runtime->constants[i]) ||
            !mo_value_make_immortal(p->heap, file->constants[i].type, runtime->constants[i])) {
            runtime->outcome = mo_vm_stop(p, "running out of memory", "--max-heap", false,
                                          mo_cell_unit(), MO_STOPPED);
            *defect_out = p->defect;
            return runtime->outcome;
        }
    }
    for (uint32_t i = 0; i < file->init_count; i++) {
        struct mo_cell result;
        runtime->outcome = mo_call(p, file->init[i], NULL, 0, &result, defect_out);
        if (runtime->outcome != MO_RETURNED)
            return runtime->outcome;
        mo_value_drop(p->heap, file->functions[file->init[i]].result_type, result);
    }
    runtime->ready = true;
    runtime->outcome = MO_RETURNED;
    return MO_RETURNED;
}
