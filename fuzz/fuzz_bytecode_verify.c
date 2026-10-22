#include "bytecode/io.h"
#include "vm/vm.h"

#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

enum verifier_fuzz_gate {
    VERIFIER_FUZZ_STRUCTURAL = 1,
    VERIFIER_FUZZ_VERIFIER = 2,
    VERIFIER_FUZZ_ACCEPTED = 3
};

enum {
    VERIFIER_FUZZ_MAX_WIRE = 64 << 10,
    VERIFIER_FUZZ_INSTRUCTIONS = 512,
    VERIFIER_FUZZ_MAX_CALLS = 64
};

_Static_assert(VERIFIER_FUZZ_MAX_WIRE <= MO_LIMIT_BYTECODE_SIZE,
               "the fuzz wire cap stays inside the bytecode size limit");

struct verifier_fuzz_refusal {
    enum mo_bytecode_error_kind kind;
    uint32_t offset;
};

struct verifier_fuzz_entry {
    uint32_t function;
};

struct verifier_fuzz_accepted {
    struct mo_image *image;
    struct verifier_fuzz_entry entries[VERIFIER_FUZZ_MAX_CALLS];
    uint32_t call_count;
};

struct verifier_fuzz_input {
    enum verifier_fuzz_gate gate;
    union {
        struct verifier_fuzz_refusal refusal;
        struct verifier_fuzz_accepted accepted;
    } as;
};

static MO_MUST_USE struct mo_limits verifier_fuzz_limits(void) {
    struct mo_limits limits = mo_limits_default();
    limits.heap_size = 8 << 20;
    return limits;
}

static void verifier_fuzz_repair_checksum(uint8_t *wire, size_t length) {
    uint64_t checksum = MO_BYTECODE_FNV_OFFSET;
    for (size_t i = MO_BYTECODE_HEADER_SIZE; i < length; i++)
        checksum = (checksum ^ wire[i]) * MO_BYTECODE_FNV_PRIME;
    for (unsigned i = 0; i < 8; i++)
        wire[12 + i] = (uint8_t)(checksum >> (8 * i));
}

static MO_MUST_USE struct verifier_fuzz_refusal
verifier_fuzz_checked_refusal(struct mo_load_error error, size_t wire_length) {
    switch (error.kind) {
    case MO_BYTECODE_INVALID:
    case MO_BYTECODE_LIMIT:
    case MO_BYTECODE_NO_MEMORY:
        break;
    }
    MO_ASSERT(error.reason != NULL);
    MO_ASSERT((size_t)error.offset <= wire_length);
    return (struct verifier_fuzz_refusal){.kind = error.kind, .offset = error.offset};
}

static void verifier_fuzz_select_entries(const struct mo_image *image,
                                         struct verifier_fuzz_entry *entries, uint32_t *count) {
    const struct mo_bytecode *file = mo_image_bytecode(image);
    *count = 0;
    for (uint32_t i = 0; i < file->function_count; i++) {
        const struct mo_bytecode_function *fn = &file->functions[i];
        bool direct = false;
        switch (fn->kind) {
        case MO_FUNCTION_FUNCTION:
        case MO_FUNCTION_TEST:
        case MO_FUNCTION_EXAMPLE:
        case MO_FUNCTION_PROMISE:
        case MO_FUNCTION_ANCHOR:
            direct = fn->parameter_count == 0;
            break;
        case MO_FUNCTION_CLOSURE:
        case MO_FUNCTION_INIT:
            break;
        }
        if (!direct)
            continue;
        if (*count == VERIFIER_FUZZ_MAX_CALLS)
            return;
        entries[*count].function = i;
        (*count)++;
    }
}

static MO_MUST_USE struct verifier_fuzz_input verifier_fuzz_classify(const uint8_t *wire,
                                                                     size_t length) {
    struct mo_arena arena;
    struct mo_load_error error;
    mo_arena_init(&arena, length <= MO_LIMIT_BYTECODE_SIZE ? mo_bytecode_work_budget(length) : 0);
    const struct mo_bytecode *file = mo_bytecode_decode(&arena, wire, length, &error);
    if (file == NULL) {
        struct verifier_fuzz_refusal refusal = verifier_fuzz_checked_refusal(error, length);
        mo_arena_free(&arena);
        return (struct verifier_fuzz_input){.gate = VERIFIER_FUZZ_STRUCTURAL,
                                            .as.refusal = refusal};
    }
    mo_arena_free(&arena);
    struct mo_image *image = mo_load(wire, length, &error);
    if (image == NULL) {
        struct verifier_fuzz_refusal refusal = verifier_fuzz_checked_refusal(error, length);
        return (struct verifier_fuzz_input){.gate = VERIFIER_FUZZ_VERIFIER, .as.refusal = refusal};
    }
    struct verifier_fuzz_input input = {.gate = VERIFIER_FUZZ_ACCEPTED};
    input.as.accepted.image = image;
    verifier_fuzz_select_entries(image, input.as.accepted.entries, &input.as.accepted.call_count);
    return input;
}

static void verifier_fuzz_release(struct verifier_fuzz_input *input) {
    if (input->gate == VERIFIER_FUZZ_ACCEPTED && input->as.accepted.image != NULL) {
        mo_image_free(input->as.accepted.image);
        input->as.accepted.image = NULL;
    }
}

static void verifier_fuzz_execute(struct verifier_fuzz_input input) {
    switch (input.gate) {
    case VERIFIER_FUZZ_STRUCTURAL:
    case VERIFIER_FUZZ_VERIFIER:
        break;
    case VERIFIER_FUZZ_ACCEPTED: {
        struct mo_image *image = input.as.accepted.image;
        MO_ASSERT(image != NULL);
        struct mo_limits limits = verifier_fuzz_limits();
        struct mo_defect defect;
        enum mo_outcome outcome =
            mo_image_init_bounded(image, &limits, VERIFIER_FUZZ_INSTRUCTIONS, &defect);
        if (outcome == MO_RETURNED) {
            const struct mo_bytecode *file = mo_image_bytecode(image);
            for (uint32_t i = 0; i < input.as.accepted.call_count; i++) {
                struct mo_process *process = mo_process_new(image, &limits);
                if (process == NULL)
                    break;
                mo_process_instruction_limit(process, VERIFIER_FUZZ_INSTRUCTIONS);
                struct mo_cell result;
                uint32_t function = input.as.accepted.entries[i].function;
                outcome = mo_call(process, function, NULL, 0, &result, &defect);
                if (outcome == MO_RETURNED) {
                    struct mo_heap *heap = mo_process_heap(process);
                    mo_value_drop(heap, file->functions[function].result_type, result);
                    /* The oracle for reference tracking: a verified call that returns has
                     * released every reference it acquired. */
                    MO_ASSERT(mo_heap_objects(heap) == 0);
                }
                mo_process_free(process);
            }
        }
        break;
    }
    default:
        MO_UNREACHABLE();
    }
    verifier_fuzz_release(&input);
}

static void verifier_fuzz_changed_repair(const uint8_t *data, size_t size) {
    uint8_t *owned = malloc(size);
    if (owned == NULL)
        return;
    memcpy(owned, data, size);
    verifier_fuzz_repair_checksum(owned, size);
    if (memcmp(owned, data, size) != 0) {
        struct verifier_fuzz_input input = verifier_fuzz_classify(owned, size);
        verifier_fuzz_execute(input);
    }
    free(owned);
}

static void verifier_fuzz_one(const uint8_t *data, size_t size) {
    if (size > VERIFIER_FUZZ_MAX_WIRE)
        return;
    verifier_fuzz_execute(verifier_fuzz_classify(data, size));
    if (size >= MO_BYTECODE_HEADER_SIZE)
        verifier_fuzz_changed_repair(data, size);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    verifier_fuzz_one(data, size);
    return 0;
}
