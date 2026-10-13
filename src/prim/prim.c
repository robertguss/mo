#include "prim/internal.h"

#include <fenv.h>
#include <string.h>

bool mo_prim_bytes_fit(struct mo_prim_call *call, size_t length) {
    if (length <= MO_LIMIT_VALUE_BYTES)
        return true;
    call->reason = "result exceeds MO_LIMIT_VALUE_BYTES (u32 byte length)";
    return false;
}

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
    struct mo_scratch available;
    mo_scratch_init(&available, call->heap);
    size_t limit =
        available.arena.limit < MO_LIMIT_VALUE_BYTES ? available.arena.limit : MO_LIMIT_VALUE_BYTES;
    mo_scratch_free(&available);
    mo_buffer_init(&text, limit);
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
    MO_ASSERT(signature->kind == MO_TYPE_FUNCTION && count == signature->parameter_count &&
              (count == 0 || arguments != NULL));
    MO_ASSERT(fesetround(FE_TONEAREST) == 0 && fegetround() == FE_TONEAREST);
    struct mo_prim_call call = {.heap = heap,
                                .types = types,
                                .id = id,
                                .args = arguments,
                                .params = signature->parameters,
                                .result = signature->result};
    struct mo_cell result;
    enum mo_prim_status status;
    _Static_assert(MO_PRIM_FLOAT_TO_TEXT == 61 && MO_PRIM_BYTES_DECODE_UTF8 == 103 &&
                       MO_PRIM_RANGE_CONTAINS == 148 && MO_PRIM_UNICODE16_UPPERCASE == 159,
                   "primitive group endpoints keep their wire ids");
    switch (id) {
    case MO_PRIM_INT_POW:
        status = power(arguments, &result, &call.reason);
        break;
    case MO_PRIM_BIGINT_ADD:
    case MO_PRIM_BIGINT_SUB:
    case MO_PRIM_BIGINT_MUL:
    case MO_PRIM_BIGINT_DIV:
    case MO_PRIM_BIGINT_REM:
    case MO_PRIM_BIGINT_NEG:
    case MO_PRIM_BIGINT_COMPARE:
    case MO_PRIM_BIGINT_ABS:
    case MO_PRIM_BIGINT_MIN:
    case MO_PRIM_BIGINT_MAX:
    case MO_PRIM_BIGINT_POW:
    case MO_PRIM_BIGINT_FLOOR_DIV:
    case MO_PRIM_BIGINT_FLOOR_MOD:
    case MO_PRIM_BIGINT_TO_INT:
    case MO_PRIM_BIGINT_TO_TEXT:
        status = mo_prim_bigint(&call, &result);
        break;
    case MO_PRIM_INT_ABS:
    case MO_PRIM_INT_MIN:
    case MO_PRIM_INT_MAX:
    case MO_PRIM_INT_FLOOR_DIV:
    case MO_PRIM_INT_FLOOR_MOD:
    case MO_PRIM_INT_WRAPPING_ADD:
    case MO_PRIM_INT_WRAPPING_SUB:
    case MO_PRIM_INT_WRAPPING_MUL:
    case MO_PRIM_INT_SATURATING_ADD:
    case MO_PRIM_INT_SATURATING_SUB:
    case MO_PRIM_INT_SATURATING_MUL:
    case MO_PRIM_INT_CHECKED_ADD:
    case MO_PRIM_INT_CHECKED_SUB:
    case MO_PRIM_INT_CHECKED_MUL:
    case MO_PRIM_INT_TO_DECIMAL:
    case MO_PRIM_INT_TO_FLOAT:
    case MO_PRIM_INT_TO_BIG:
    case MO_PRIM_INT_TO_BYTE:
    case MO_PRIM_INT_TO_CHAR:
    case MO_PRIM_INT_TO_TEXT:
    case MO_PRIM_INT_SECONDS:
    case MO_PRIM_INT_MILLISECONDS:
    case MO_PRIM_INT_MINUTES:
    case MO_PRIM_INT_HOURS:
    case MO_PRIM_INT_DAYS:
    case MO_PRIM_BYTE_TO_INT:
    case MO_PRIM_BYTE_TO_TEXT:
    case MO_PRIM_DECIMAL_ABS:
    case MO_PRIM_DECIMAL_MIN:
    case MO_PRIM_DECIMAL_MAX:
    case MO_PRIM_DECIMAL_ROUND:
    case MO_PRIM_DECIMAL_FLOOR:
    case MO_PRIM_DECIMAL_CEIL:
    case MO_PRIM_DECIMAL_TO_INT:
    case MO_PRIM_DECIMAL_TO_FLOAT:
    case MO_PRIM_DECIMAL_TO_FIXED:
    case MO_PRIM_DECIMAL_TO_TEXT:
    case MO_PRIM_FLOAT_ABS:
    case MO_PRIM_FLOAT_FLOOR:
    case MO_PRIM_FLOAT_CEIL:
    case MO_PRIM_FLOAT_ROUND:
    case MO_PRIM_FLOAT_SQRT:
    case MO_PRIM_FLOAT_IS_NAN:
    case MO_PRIM_FLOAT_IS_INFINITE:
    case MO_PRIM_FLOAT_TO_DECIMAL:
    case MO_PRIM_FLOAT_TO_TEXT:
        status = mo_prim_number(&call, &result);
        break;
    case MO_PRIM_TEXT_CONCAT:
    case MO_PRIM_TEXT_COMPARE:
    case MO_PRIM_TEXT_BYTE_COUNT:
    case MO_PRIM_TEXT_CHAR_COUNT:
    case MO_PRIM_TEXT_IS_EMPTY:
    case MO_PRIM_TEXT_CHARS:
    case MO_PRIM_TEXT_BYTES:
    case MO_PRIM_TEXT_LINES:
    case MO_PRIM_TEXT_WORDS:
    case MO_PRIM_TEXT_SPLIT:
    case MO_PRIM_TEXT_TRIM:
    case MO_PRIM_TEXT_TRIM_START:
    case MO_PRIM_TEXT_TRIM_END:
    case MO_PRIM_TEXT_STARTS_WITH:
    case MO_PRIM_TEXT_ENDS_WITH:
    case MO_PRIM_TEXT_CONTAINS:
    case MO_PRIM_TEXT_DROP_PREFIX:
    case MO_PRIM_TEXT_DROP_SUFFIX:
    case MO_PRIM_TEXT_REPLACE:
    case MO_PRIM_TEXT_REPEAT:
    case MO_PRIM_TEXT_PAD_LEFT:
    case MO_PRIM_TEXT_PAD_RIGHT:
    case MO_PRIM_TEXT_ASCII_LOWERCASE:
    case MO_PRIM_TEXT_ASCII_UPPERCASE:
    case MO_PRIM_TEXT_TO_INT:
    case MO_PRIM_TEXT_TO_DECIMAL:
    case MO_PRIM_TEXT_TO_FLOAT:
    case MO_PRIM_TEXT_FROM_CHARS:
    case MO_PRIM_TEXT_QUOTED:
    case MO_PRIM_TEXT_STRUCTURAL:
    case MO_PRIM_CHAR_CODE:
    case MO_PRIM_CHAR_IS_ASCII_DIGIT:
    case MO_PRIM_CHAR_IS_ASCII_LETTER:
    case MO_PRIM_CHAR_IS_ASCII_WHITE_SPACE:
    case MO_PRIM_CHAR_ASCII_LOWERCASE:
    case MO_PRIM_CHAR_ASCII_UPPERCASE:
    case MO_PRIM_CHAR_TO_TEXT:
    case MO_PRIM_CHAR_QUOTED:
    case MO_PRIM_BYTES_COMPARE:
    case MO_PRIM_BYTES_SIZE:
    case MO_PRIM_BYTES_GET:
    case MO_PRIM_BYTES_DECODE_UTF8:
        status = mo_prim_text(&call, &result);
        break;
    case MO_PRIM_LIST_EMPTY:
    case MO_PRIM_LIST_SIZE:
    case MO_PRIM_LIST_IS_EMPTY:
    case MO_PRIM_LIST_GET:
    case MO_PRIM_LIST_FIRST:
    case MO_PRIM_LIST_LAST:
    case MO_PRIM_LIST_APPEND:
    case MO_PRIM_LIST_PREPEND:
    case MO_PRIM_LIST_CONCAT:
    case MO_PRIM_LIST_REVERSE:
    case MO_PRIM_LIST_TAKE:
    case MO_PRIM_LIST_DROP:
    case MO_PRIM_LIST_SET:
    case MO_PRIM_LIST_REMOVE_AT:
    case MO_PRIM_LIST_ENUMERATE:
    case MO_PRIM_LIST_ZIP:
    case MO_PRIM_LIST_CONTAINS:
    case MO_PRIM_LIST_UNIQUE:
    case MO_PRIM_LIST_TO_SET:
    case MO_PRIM_LIST_SUM:
    case MO_PRIM_LIST_JOIN:
    case MO_PRIM_LIST_TO_MAP:
    case MO_PRIM_MAP_EMPTY:
    case MO_PRIM_MAP_LITERAL_INSERT:
    case MO_PRIM_MAP_SIZE:
    case MO_PRIM_MAP_IS_EMPTY:
    case MO_PRIM_MAP_GET:
    case MO_PRIM_MAP_HAS:
    case MO_PRIM_MAP_SET:
    case MO_PRIM_MAP_REMOVE:
    case MO_PRIM_MAP_KEYS:
    case MO_PRIM_MAP_VALUES:
    case MO_PRIM_MAP_ENTRIES:
    case MO_PRIM_SET_EMPTY:
    case MO_PRIM_SET_SIZE:
    case MO_PRIM_SET_IS_EMPTY:
    case MO_PRIM_SET_HAS:
    case MO_PRIM_SET_ADD:
    case MO_PRIM_SET_REMOVE:
    case MO_PRIM_SET_UNION:
    case MO_PRIM_SET_INTERSECTION:
    case MO_PRIM_SET_DIFFERENCE:
    case MO_PRIM_SET_TO_LIST:
    case MO_PRIM_RANGE_TO_LIST:
    case MO_PRIM_RANGE_CONTAINS:
        status = mo_prim_collection(&call, &result);
        break;
    case MO_PRIM_TIME_CHECKED_ADD:
    case MO_PRIM_TIME_TO_TEXT:
    case MO_PRIM_TIME_PARSE:
    case MO_PRIM_DURATION_IN_MILLISECONDS:
    case MO_PRIM_DURATION_IN_SECONDS:
    case MO_PRIM_DURATION_TO_TEXT:
    case MO_PRIM_UNICODE16_GRAPHEMES:
    case MO_PRIM_UNICODE16_DISPLAY_WIDTH:
    case MO_PRIM_UNICODE16_NORMALIZE_NFC:
    case MO_PRIM_UNICODE16_LOWERCASE:
    case MO_PRIM_UNICODE16_UPPERCASE:
        call.reason = "primitive not implemented";
        status = MO_PRIM_NOT_IMPLEMENTED;
        break;
    case MO_PRIM_COUNT:
    default:
        MO_UNREACHABLE();
    }
    if (status == MO_PRIM_RETURNED) {
        for (size_t i = 0; i < count; i++) {
            if (ownership[id][i] == 'o' && (call.moved & (1U << i)) == 0)
                mo_value_drop(heap, call.params[i], arguments[i]);
        }
        *out = result;
    } else if (status == MO_PRIM_OUT_OF_MEMORY && call.reason == NULL) {
        call.reason = "out of memory (--max-heap)";
    }
    *reason_out = call.reason;
    return status;
}
