#include "test.h"
#include "vm/vm.h"

#include <string.h>

static void test_return(void) {
    const struct mo_type types[] = {{.kind = MO_TYPE_INT}};
    const struct mo_bytecode_string strings[] = {{1, (const uint8_t *)"M"}};
    const struct mo_bytecode_constant constant = {.type = 0, .value.immediate = {7}};
    const uint32_t registers[] = {0};
    const uint32_t code[] = {MO_OP_CONST, 0, 0, MO_OP_RET, 0};
    const struct mo_bytecode_function function = {.name = 0,
                                                  .module = 0,
                                                  .result_type = 0,
                                                  .register_count = 1,
                                                  .register_types = registers,
                                                  .code_count = 5,
                                                  .code = code};
    const struct mo_bytecode_debug debug = {
        .text = MO_NO_INDEX, .file = MO_NO_INDEX, .subject = MO_NO_INDEX};
    const struct mo_bytecode_module module = {.name = 0};
    const struct mo_bytecode file = {.strings = strings,
                                     .string_count = 1,
                                     .types = types,
                                     .type_count = 1,
                                     .constants = &constant,
                                     .constant_count = 1,
                                     .functions = &function,
                                     .function_count = 1,
                                     .modules = &module,
                                     .module_count = 1,
                                     .debug = &debug,
                                     .debug_count = 1};
    struct mo_buffer wire;
    mo_buffer_init(&wire, MO_LIMIT_BYTECODE_SIZE);
    struct mo_write_error write_error;
    CHECK(mo_bytecode_write(&file, &wire, &write_error));
    struct mo_load_error error;
    struct mo_image *image = mo_load(wire.bytes, wire.length, &error);
    CHECK(image != NULL);
    mo_buffer_free(&wire);
    if (image == NULL)
        return;
    struct mo_limits limits = mo_limits_default();
    struct mo_defect defect;
    CHECK(mo_image_init(image, &limits, &defect) == MO_RETURNED);
    struct mo_process *process = mo_process_new(image, &limits);
    CHECK(process != NULL);
    if (process != NULL) {
        struct mo_cell result = mo_cell_int(-1);
        CHECK(mo_call(process, 0, NULL, 0, &result, &defect) == MO_RETURNED);
        CHECK(mo_cell_get_int(result) == 7);
        mo_value_drop(mo_process_heap(process), 0, result);
        mo_process_free(process);
    }
    mo_image_free(image);
}

int main(void) {
    test_return();
    return mo_test_report("vm");
}
