#include "prim/internal.h"

#include <string.h>

enum mo_prim_status mo_prim_option(struct mo_prim_call *call, bool some, struct mo_cell value,
                                   struct mo_cell *out) {
    MO_ASSERT(call->types->items[call->result].kind == MO_TYPE_CHOICE);
    return mo_value_case(call->heap, call->result, some ? 1U : 0U, some ? &value : NULL,
                         some ? 1U : 0U, out)
               ? MO_PRIM_RETURNED
               : MO_PRIM_OUT_OF_MEMORY;
}

enum mo_prim_status mo_prim_print(struct mo_prim_call *call, uint32_t type, struct mo_cell value,
                                  struct mo_cell *out) {
    struct mo_buffer text;
    mo_buffer_init(&text, MO_LIMIT_STRING_SIZE);
    bool ok = mo_value_print(call->heap, type, value, &text) &&
              mo_value_text(call->heap, (const char *)text.bytes, text.length, out);
    mo_buffer_free(&text);
    return ok ? MO_PRIM_RETURNED : MO_PRIM_OUT_OF_MEMORY;
}

static MO_MUST_USE bool multiply(int64_t a, int64_t b, int64_t *out) {
    mo_i128 wide = (mo_i128)a * b;
    if (wide < INT64_MIN || wide > INT64_MAX)
        return false;
    *out = (int64_t)wide;
    return true;
}

static MO_MUST_USE enum mo_prim_status power(const struct mo_cell *args, struct mo_cell *out,
                                             const char **reason) {
    int64_t a = mo_cell_get_int(args[0]), n = mo_cell_get_int(args[1]), result = 1;
    if (n < 0) {
        *reason = "Int.pow with a negative exponent";
        return MO_PRIM_OUTSIDE;
    }
    /* Squaring only when another bit remains accepts (-2)^63 and INT64_MIN^1. */
    while (n != 0) {
        if ((n & 1) != 0 && !multiply(result, a, &result)) {
            *reason = "Int";
            return MO_PRIM_OVERFLOW;
        }
        n >>= 1;
        if (n != 0 && !multiply(a, a, &a)) {
            *reason = "Int";
            return MO_PRIM_OVERFLOW;
        }
    }
    *out = mo_cell_int(result);
    return MO_PRIM_RETURNED;
}

enum mo_prim_status mo_prim_call(struct mo_heap *heap, const struct mo_types *types,
                                 enum mo_primitive_id id, uint32_t instantiation,
                                 const struct mo_cell *arguments, struct mo_cell *out,
                                 const char **reason_out) {
    MO_ASSERT(heap != NULL && types != NULL && types->finished && instantiation < types->count &&
              id < MO_PRIM_COUNT && out != NULL && reason_out != NULL);
    static const char *const ownership[] = {
#define PRIM(id, module, name, signature, params, bugs) params,
#include "base/prims.def"
    };
    const struct mo_type *signature = &types->items[instantiation];
    size_t count = strlen(ownership[id]);
    MO_ASSERT(count == 0 || (arguments != NULL && signature->kind == MO_TYPE_FUNCTION &&
                             count == signature->parameter_count));
    struct mo_prim_call call = {.heap = heap,
                                .types = types,
                                .id = id,
                                .args = arguments,
                                .params = count == 0 ? NULL : signature->parameters,
                                .result = count == 0 ? instantiation : signature->result};
    struct mo_cell result;
    enum mo_prim_status status;
    if (id == MO_PRIM_INT_POW) {
        status = power(arguments, &result, &call.reason);
    } else if (id <= MO_PRIM_FLOAT_TO_TEXT &&
               !(id >= MO_PRIM_BIGINT_ADD && id <= MO_PRIM_BIGINT_TO_TEXT)) {
        status = mo_prim_number(&call, &result);
    } else {
        call.reason = "primitive not implemented";
        status = MO_PRIM_NOT_IMPLEMENTED;
    }
    if (status == MO_PRIM_RETURNED) {
        for (size_t i = 0; i < count; i++) {
            if (ownership[id][i] == 'o' && (call.moved & (1U << i)) == 0)
                mo_value_drop(heap, call.params[i], arguments[i]);
        }
        *out = result;
    } else if (status == MO_PRIM_OUT_OF_MEMORY) {
        call.reason = "out of memory";
    }
    *reason_out = call.reason;
    return status;
}
