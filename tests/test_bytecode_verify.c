#include "bytecode/io.h"
#include "test.h"

#include <string.h>

static const struct mo_bytecode_string strings[] = {{1, (const uint8_t *)"M"}};
static const struct mo_type types[] = {{.kind = MO_TYPE_UNIT}};
static const uint32_t registers[] = {0}, code[] = {MO_OP_RET, 0};
static const struct mo_bytecode_function functions[] = {{.name = 0,
                                                         .module = 0,
                                                         .kind = MO_FUNCTION_FUNCTION,
                                                         .result_type = 0,
                                                         .register_count = 1,
                                                         .register_types = registers,
                                                         .code_count = 2,
                                                         .code = code}};
static const struct mo_bytecode_module modules[] = {{.name = 0}};
static const struct mo_bytecode_debug debug[] = {
    {.text = MO_BYTECODE_NO_INDEX, .file = MO_BYTECODE_NO_INDEX, .subject = MO_BYTECODE_NO_INDEX}};

int main(void) {
    struct mo_bytecode file = {.string_count = 1,
                               .strings = strings,
                               .type_count = 1,
                               .types = types,
                               .function_count = 1,
                               .functions = functions,
                               .module_count = 1,
                               .modules = modules,
                               .debug_count = 1,
                               .debug = debug};
    struct mo_buffer out;
    struct mo_write_error write_error;
    struct mo_load_error load_error;
    mo_buffer_init(&out, MO_LIMIT_BYTECODE_SIZE);
    CHECK(mo_bytecode_write(&file, &out, &write_error));
    struct mo_image *image = mo_load(out.bytes, out.length, &load_error);
    CHECK(image == NULL);
    CHECK(strcmp(load_error.reason, "register is not readable") == 0);
    if (image != NULL) {
        mo_image_free(image);
    }
    mo_buffer_free(&out);
    return mo_test_report("bytecode verifier");
}
