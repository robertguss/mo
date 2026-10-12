#include "codegen/codegen.h"

#include <string.h>

#include "base/codes.h"
#include "bytecode/bytecode.h"
#include "heap/types.h"

static MO_MUST_USE bool refusal(struct mo_diags *diags, uint16_t code, const char *reason) {
    struct mo_diag d = {.code = code,
                        .message = "Core code generation refused",
                        .reason = reason,
                        .primary = {.span = {.file = UINT32_MAX}, .text = reason}};
    if (!mo_diags_add(diags, &d))
        return false;
    return false;
}

bool mo_compile(const struct mo_core *p, struct mo_buffer *out, struct mo_diags *diags) {
    MO_ASSERT(p != NULL && out != NULL && out->length == 0 && diags != NULL);
    if (!mo_core_check(p, diags))
        return false;
    if (p->metadata.source || p->consts.count != 0 || p->data.count != 0 || p->funs.count != 1 ||
        p->funs.items[0].params.count != 0 || p->funs.items[0].kind != MO_CORE_FUNCTION ||
        p->funs.items[0].body->tag != MO_CORE_EXPR_LIT ||
        p->funs.items[0].body->as.lit->tag != MO_CORE_LIT_UNIT)
        return refusal(diags, MO_CODE_CODEGEN_UNSUPPORTED,
                       "This Core form is not implemented by code generation yet.");
    const struct mo_core_fun *fun = &p->funs.items[0];
    struct mo_buffer name;
    mo_buffer_init(&name, MO_LIMIT_STRING_SIZE);
    if (!mo_buffer_append(&name, fun->module.bytes, fun->module.length) ||
        !mo_buffer_append_byte(&name, '.') ||
        !mo_buffer_append(&name, fun->name.bytes, fun->name.length)) {
        mo_buffer_free(&name);
        return refusal(diags, MO_CODE_CODEGEN_MEMORY,
                       "Function name exceeds its limit or memory is unavailable.");
    }
    static const uint32_t regs[] = {0}, code[] = {MO_OP_CONST, 0, 0, MO_OP_RET, 0};
    const struct mo_bytecode_string strings[] = {
        {(uint32_t)fun->module.length, (const uint8_t *)fun->module.bytes},
        {(uint32_t)name.length, name.bytes}};
    const struct mo_type ty = {.kind = MO_TYPE_UNIT};
    const struct mo_bytecode_constant k = {.type = 0};
    const struct mo_bytecode_function f = {.name = 1,
                                           .module = 0,
                                           .kind = MO_FUNCTION_FUNCTION,
                                           .register_count = 1,
                                           .register_types = regs,
                                           .code_count = 5,
                                           .code = code};
    const struct mo_bytecode_module m = {.name = 0};
    const struct mo_bytecode_debug d = {.text = MO_BYTECODE_NO_INDEX,
                                        .file = MO_BYTECODE_NO_INDEX,
                                        .subject = MO_BYTECODE_NO_INDEX};
    const struct mo_bytecode file = {.string_count = 2,
                                     .strings = strings,
                                     .type_count = 1,
                                     .types = &ty,
                                     .constant_count = 1,
                                     .constants = &k,
                                     .function_count = 1,
                                     .functions = &f,
                                     .module_count = 1,
                                     .modules = &m,
                                     .debug_count = 1,
                                     .debug = &d};
    struct mo_write_error error;
    bool ok = mo_bytecode_write(&file, out, &error);
    mo_buffer_free(&name);
    return ok || refusal(diags, MO_CODE_CODEGEN_INVALID, error.reason);
}
