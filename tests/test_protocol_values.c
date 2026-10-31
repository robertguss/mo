#include "base/stack.h"
#include "service_compile.h"
#include "test.h"
#include "tools/protocol.h"

#include <math.h>

static const char source[] =
    "module Main\nrange = 7..-3\n"
    "fn accept(_big: BigInt, _bytes: Bytes, _text: Text, _decimal: Decimal, _duration: Duration, "
    "_time: Time, _char: Char, _float: Float, _int: Int, _byte: Byte, _bool: Bool)\nUnit\nend\n";

static uint32_t function(const struct mo_image *image, const char *wanted) {
    const struct mo_bytecode *file = mo_image_bytecode(image);
    for (uint32_t i = 0; i < file->function_count; i++) {
        const struct mo_bytecode_string *name = &file->strings[file->functions[i].name];
        if (name->length == strlen(wanted) && memcmp(name->bytes, wanted, name->length) == 0)
            return i;
    }
    MO_ASSERT(false);
    return 0;
}

static struct mo_cell call(const struct mo_bytecode *file, struct mo_process *process,
                           uint32_t index, struct mo_cell value) {
    const struct mo_bytecode_function *fn = &file->functions[index];
    if (fn->parameters[0].ownership == MO_PARAMETER_OWNED)
        value = mo_value_dup(mo_process_heap(process), fn->parameters[0].type, value);
    struct mo_cell out;
    struct mo_defect defect;
    MO_ASSERT(mo_call(process, index, &value, 1, &out, &defect) == MO_RETURNED);
    return out;
}

static uint32_t type(const struct mo_image *image, enum mo_type_kind kind) {
    const struct mo_types *types = mo_image_types(image);
    for (uint32_t i = 0; i < types->count; i++)
        if (types->items[i].kind == kind)
            return i;
    MO_ASSERT(false);
    return 0;
}

static void refusals(struct mo_image *image, uint32_t scalar, enum mo_type_kind kind,
                     struct mo_cell native, struct mo_cell packed) {
    const struct mo_types *types = mo_image_types(image);
    unsigned refused = 0, built = 0;
    for (size_t budget = 256; budget <= 65536; budget += 256) {
        struct mo_limits limits = mo_limits_default();
        limits.heap_size = budget;
        struct mo_process *process = mo_process_new(image, &limits);
        if (process == NULL)
            continue;
        struct mo_heap *heap = mo_process_heap(process);
        size_t before = mo_heap_objects(heap);
        struct mo_cell out = mo_cell_int(73);
        enum mo_protocol_value_status status =
            mo_protocol_scalar_pack(types, scalar, heap, kind, native, 1024, &out);
        if (status == MO_PROTOCOL_VALUE_OK) {
            built++;
            mo_value_drop(heap, scalar, out);
        } else {
            refused++;
            CHECK(status == MO_PROTOCOL_VALUE_MEMORY && mo_cell_get_int(out) == 73);
        }
        CHECK(mo_heap_objects(heap) == before);
        out = mo_cell_int(73);
        status = mo_protocol_scalar_unpack(types, scalar, heap, kind, packed, 1024, &out);
        if (status == MO_PROTOCOL_VALUE_OK)
            mo_value_drop(heap, type(image, kind), out);
        else
            CHECK(status == MO_PROTOCOL_VALUE_MEMORY && mo_cell_get_int(out) == 73);
        CHECK(mo_heap_objects(heap) == before);
        mo_process_free(process);
    }
    CHECK(refused != 0 && built != 0);
}

static void run(void *unused) {
    (void)unused;
    struct mo_image *left = compile(source), *right = compile(source);
    struct mo_image *protocol = mo_protocol_build();
    MO_ASSERT(protocol != NULL);
    struct mo_limits limits = mo_limits_default();
    struct mo_defect defect;
    MO_ASSERT(mo_image_init(protocol, &limits, &defect) == MO_RETURNED);
    struct mo_process *a = mo_process_new(left, &limits);
    struct mo_process *b = mo_process_new(right, &limits);
    struct mo_process *p = mo_process_new(protocol, &limits);
    MO_ASSERT(a != NULL && b != NULL && p != NULL);
    struct mo_heap *from = mo_process_heap(a), *to = mo_process_heap(b), *heap = mo_process_heap(p);
    const struct mo_types *types = mo_image_types(protocol);
    const struct mo_bytecode *file = mo_image_bytecode(protocol);
    uint32_t scalar = file->functions[function(protocol, "McpScalar.encode")].parameters[0].type;
    struct {
        enum mo_type_kind kind;
        struct mo_cell native, packed, copied;
        struct mo_buffer expected;
        size_t limit;
    } cases[] = {
        {.kind = MO_TYPE_UNIT, .native = mo_cell_unit()},
        {.kind = MO_TYPE_BOOL, .native = mo_cell_bool(true)},
        {.kind = MO_TYPE_BYTE, .native = mo_cell_byte(253)},
        {.kind = MO_TYPE_INT, .native = mo_cell_int(INT64_MIN)},
        {.kind = MO_TYPE_FLOAT, .native = mo_cell_float(-0.0)},
        {.kind = MO_TYPE_FLOAT, .native = mo_cell_float(NAN)},
        {.kind = MO_TYPE_CHAR, .native = mo_cell_char(0x1f600)},
        {.kind = MO_TYPE_TIME, .native = mo_cell_time(-1)},
        {.kind = MO_TYPE_TIME, .native = mo_cell_time(MO_TIME_MIN_US)},
        {.kind = MO_TYPE_TIME, .native = mo_cell_time(MO_TIME_MAX_US)},
        {.kind = MO_TYPE_DURATION, .native = mo_cell_duration(INT64_MIN)},
        {.kind = MO_TYPE_DURATION, .native = mo_cell_duration(INT64_MAX)},
        {.kind = MO_TYPE_DECIMAL, .native = mo_cell_decimal(-INT64_C(1000000000000000001))},
        {.kind = MO_TYPE_RANGE, .native = mo_cell_range(7, -3)},
        {.kind = MO_TYPE_TEXT, .limit = 4},
        {.kind = MO_TYPE_BYTES, .limit = 257},
        {.kind = MO_TYPE_BIGINT, .limit = 3},
        {.kind = MO_TYPE_TEXT},
        {.kind = MO_TYPE_BYTES},
        {.kind = MO_TYPE_BIGINT},
    };
    MO_ASSERT(mo_value_text(from, "a\0\xc3\xa9", 4, &cases[14].native));
    uint8_t bytes[257];
    for (size_t i = 0; i < sizeof bytes; i++)
        bytes[i] = (uint8_t)(i % 256);
    MO_ASSERT(mo_value_bytes(from, bytes, sizeof bytes, &cases[15].native));
    const uint8_t magnitude[] = {1, 0, 1};
    MO_ASSERT(
        mo_value_bigint(from, MO_BIGINT_NEGATIVE, magnitude, sizeof magnitude, &cases[16].native));
    MO_ASSERT(mo_value_text(from, "", 0, &cases[17].native));
    MO_ASSERT(mo_value_bytes(from, NULL, 0, &cases[18].native));
    MO_ASSERT(mo_value_bigint(from, MO_BIGINT_ZERO, NULL, 0, &cases[19].native));
    char byte_json[1100];
    size_t position = 0;
    for (size_t i = 0; i < sizeof bytes; i++) {
        int n = snprintf(byte_json + position, sizeof byte_json - position, "%c%u",
                         i == 0 ? '[' : ',', (unsigned)(i % 256));
        MO_ASSERT(n > 0 && (size_t)n < sizeof byte_json - position);
        position += (size_t)n;
    }
    byte_json[position++] = ']';
    byte_json[position] = '\0';
    const char *expected[] = {
        "null",
        "true",
        "253",
        "\"-9223372036854775808\"",
        "\"-0\"",
        "\"nan\"",
        "\"😀\"",
        "\"1969-12-31T23:59:59.999999Z\"",
        "\"0001-01-01T00:00:00Z\"",
        "\"9999-12-31T23:59:59.999999Z\"",
        "\"-9223372036854775808\"",
        "\"9223372036854775807\"",
        "\"-1.000000000000000001\"",
        "{\"start\":\"7\",\"end\":\"-3\"}",
        "\"a\\u0000é\"",
        byte_json,
        "\"-65537\"",
        "\"\"",
        "[]",
        "\"0\"",
    };
    uint32_t encode = function(protocol, "McpScalar.encode");
    uint32_t print = function(protocol, "Json.to_text@Json");
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        mo_buffer_init(&cases[i].expected, 4096);
        MO_ASSERT(
            mo_value_encode(from, type(left, cases[i].kind), cases[i].native, &cases[i].expected));
        size_t limit = cases[i].limit;
        MO_ASSERT(mo_protocol_scalar_pack(types, scalar, heap, cases[i].kind, cases[i].native,
                                          limit, &cases[i].packed) == MO_PROTOCOL_VALUE_OK);
        struct mo_cell json = call(file, p, encode, cases[i].packed);
        struct mo_cell text = call(file, p, print, json);
        size_t length;
        const uint8_t *data = mo_value_data(text, &length);
        CHECK(length == strlen(expected[i]) && memcmp(data, expected[i], length) == 0);
        mo_value_drop(heap, file->functions[print].result_type, text);
        mo_value_drop(heap, file->functions[encode].result_type, json);
        struct mo_cell sentinel = mo_cell_int(73);
        CHECK(mo_protocol_scalar_unpack(types, scalar, to, MO_TYPE_FUNCTION, cases[i].packed, 1024,
                                        &sentinel) == MO_PROTOCOL_VALUE_INVALID);
        CHECK(mo_cell_get_int(sentinel) == 73);
        CHECK(mo_protocol_scalar_unpack(
                  types, scalar, to, cases[i].kind == MO_TYPE_UNIT ? MO_TYPE_INT : MO_TYPE_UNIT,
                  cases[i].packed, 1024, &sentinel) == MO_PROTOCOL_VALUE_INVALID);
        CHECK(mo_cell_get_int(sentinel) == 73);
        if (limit != 0) {
            CHECK(mo_protocol_scalar_pack(types, scalar, heap, cases[i].kind, cases[i].native,
                                          limit - 1, &sentinel) == MO_PROTOCOL_VALUE_LIMIT);
            CHECK(mo_cell_get_int(sentinel) == 73);
            CHECK(mo_protocol_scalar_unpack(types, scalar, to, cases[i].kind, cases[i].packed,
                                            limit - 1, &sentinel) == MO_PROTOCOL_VALUE_LIMIT);
            CHECK(mo_cell_get_int(sentinel) == 73);
        }
    }
    refusals(protocol, scalar, MO_TYPE_BYTES, cases[15].native, cases[15].packed);
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++)
        mo_value_drop(from, type(left, cases[i].kind), cases[i].native);
    CHECK(mo_heap_objects(from) == 0);
    mo_process_free(a);
    mo_image_free(left);
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        MO_ASSERT(mo_protocol_scalar_unpack(types, scalar, to, cases[i].kind, cases[i].packed,
                                            cases[i].limit,
                                            &cases[i].copied) == MO_PROTOCOL_VALUE_OK);
        mo_value_drop(heap, scalar, cases[i].packed);
    }
    CHECK(mo_heap_objects(heap) == 0);
    mo_process_free(p);
    mo_image_free(protocol);
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        struct mo_buffer actual;
        mo_buffer_init(&actual, 4096);
        MO_ASSERT(mo_value_encode(to, type(right, cases[i].kind), cases[i].copied, &actual));
        CHECK(actual.length == cases[i].expected.length);
        CHECK(actual.length == cases[i].expected.length &&
              memcmp(actual.bytes, cases[i].expected.bytes, actual.length) == 0);
        mo_buffer_free(&actual);
        mo_buffer_free(&cases[i].expected);
        mo_value_drop(to, type(right, cases[i].kind), cases[i].copied);
    }
    CHECK(mo_heap_objects(to) == 0);
    mo_process_free(b);
    mo_image_free(right);
}

int main(void) {
    MO_ASSERT(mo_with_stack(MO_SOURCE_STACK_SIZE, run, NULL) == 0);
    return mo_test_report("protocol scalar values");
}
