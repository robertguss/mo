#include "base/limits.h"
#include "base/stack.h"
#include "check/check.h"
#include "codegen/codegen.h"
#include "heap/types.h"
#include "lower/lower.h"
#include "vm/vm.h"

#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

enum { MOST = 4096, WORK = 10000000 };

static const char program[] = "module Main\nuse Http\n\nfn main() : Int\n"
                              "  Http.get(\"http://example.org/\").headers.size\nend\n";
static const char chunked_head[] = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n";

static struct mo_image *image;
static uint32_t reader;

static void build(void *argument) {
    (void)argument;
    struct mo_arena input, output;
    struct mo_sources sources;
    struct mo_diags diags;
    struct mo_buffer bytes;
    struct mo_source_error source_error;
    struct mo_load_error load_error;
    uint32_t file;
    mo_arena_init(&input, MO_LIMIT_LOWER_WORK);
    mo_arena_init(&output, MO_LIMIT_LOWER_WORK);
    mo_buffer_init(&bytes, MO_LIMIT_BYTECODE_SIZE);
    MO_ASSERT(mo_sources_init(&sources) && mo_diags_init(&diags));
    MO_ASSERT(mo_sources_add_bytes(&sources, "main.mo", (const uint8_t *)program,
                                   sizeof program - 1, &file, &source_error));
    struct mo_module_set *set = mo_load_bundle_sources(&input, &sources, "lib", &diags);
    MO_ASSERT(set != NULL && !mo_diags_any(&diags));
    struct mo_program *checked = mo_check(&input, set, &diags);
    MO_ASSERT(checked != NULL && !mo_diags_any(&diags));
    struct mo_core *core = mo_lower(&output, checked, &sources, &diags);
    MO_ASSERT(core != NULL && !mo_diags_any(&diags) && mo_core_check(core, &diags) &&
              mo_compile(core, &bytes, &diags) && !mo_diags_any(&diags));
    image = mo_load(bytes.bytes, bytes.length, &load_error);
    MO_ASSERT(image != NULL);
    mo_diags_free(&diags);
    mo_sources_free(&sources);
    mo_buffer_free(&bytes);
    mo_arena_free(&output);
    mo_arena_free(&input);
}

static void prepare(void) {
    if (image != NULL)
        return;
    MO_ASSERT(mo_with_stack(MO_SOURCE_STACK_SIZE, build, NULL) == 0);
    struct mo_limits limits = mo_limits_default();
    struct mo_defect defect;
    MO_ASSERT(mo_image_init(image, &limits, &defect) == MO_RETURNED);
    const struct mo_bytecode *file = mo_image_bytecode(image);
    static const char wanted[] = "Http.read";
    for (reader = 0; reader < file->function_count; reader++) {
        const struct mo_bytecode_string *name = &file->strings[file->functions[reader].name];
        if (name->length == sizeof wanted - 1 && memcmp(name->bytes, wanted, name->length) == 0)
            break;
    }
    MO_ASSERT(reader < file->function_count && file->functions[reader].parameter_count == 5);
}

static bool inside(uint32_t *type, struct mo_cell *value, const char *wanted) {
    const struct mo_types *types = mo_image_types(image);
    uint32_t tag = mo_cell_get_tag(*value);
    const struct mo_type_case *found = &types->items[*type].cases[tag];
    if (strcmp(found->name, wanted) != 0)
        return false;
    MO_ASSERT(found->field_count == 1);
    struct mo_cell payload = mo_cell_get_payload(*value);
    *value =
        mo_case_shape(types, *type, tag) == MO_CASE_BOXED ? mo_value_field(payload, 0) : payload;
    *type = found->fields[0].type;
    return true;
}

static size_t line_end(const uint8_t *data, size_t size, size_t at) {
    while (at + 1 < size && memcmp(data + at, "\r\n", 2) != 0)
        at++;
    MO_ASSERT(at + 2 <= size);
    return at;
}

static void chunks_equal(const uint8_t *data, size_t size, const uint8_t *body, size_t length) {
    size_t at = 0, decoded = 0;
    for (;;) {
        size_t amount = 0, digits = 0;
        for (; at < size; at++, digits++) {
            unsigned c = data[at], letter = c | 32u;
            unsigned digit = c >= '0' && c <= '9'             ? c - '0'
                             : letter >= 'a' && letter <= 'f' ? letter - 'a' + 10
                                                              : 16;
            if (digit == 16)
                break;
            amount = amount * 16 + digit;
        }
        MO_ASSERT(digits >= 1 && digits <= 8);
        size_t end = line_end(data, size, at);
        if (amount == 0) {
            at -= digits;
            break;
        }
        at = end + 2;
        MO_ASSERT(amount <= size - at && amount <= length - decoded);
        MO_ASSERT(memcmp(data + at, body + decoded, amount) == 0);
        at += amount;
        decoded += amount;
        MO_ASSERT(at + 2 <= size && memcmp(data + at, "\r\n", 2) == 0);
        at += 2;
    }
    MO_ASSERT(decoded == length);
    while (at + 3 < size && memcmp(data + at, "\r\n\r\n", 4) != 0)
        at++;
    MO_ASSERT(at + 4 <= size);
}

static struct mo_cell read_from(struct mo_process *process, const struct mo_cell *mine) {
    struct mo_heap *heap = mo_process_heap(process);
    const struct mo_bytecode_function *fn = &mo_image_bytecode(image)->functions[reader];
    struct mo_cell passed[5], result;
    struct mo_defect defect;
    for (uint32_t i = 0; i < 5; i++)
        passed[i] = fn->parameters[i].ownership == MO_PARAMETER_OWNED
                        ? mo_value_dup(heap, fn->parameters[i].type, mine[i])
                        : mine[i];
    mo_process_instruction_limit(process, WORK);
    MO_ASSERT(mo_call(process, reader, passed, 5, &result, &defect) == MO_RETURNED);
    return result;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    static uint8_t received[sizeof chunked_head - 1 + MOST];
    if (size < 3 || size - 3 > MOST)
        return 0;
    prepare();
    bool finished = (data[0] & 1) != 0, chunked = (data[0] & 2) != 0, head = (data[0] & 4) != 0;
    int64_t headers = 64 + data[1], body = data[2];
    size_t prefix = chunked ? sizeof chunked_head - 1 : 0, length = prefix + size - 3;
    size_t first = (data[1] * 251u + data[2]) % (length + 1);
    memcpy(received, chunked_head, prefix);
    memcpy(received + prefix, data + 3, size - 3);

    struct mo_limits limits = mo_limits_default();
    struct mo_process *process = mo_process_new(image, &limits);
    MO_ASSERT(process != NULL);
    struct mo_heap *heap = mo_process_heap(process);
    const struct mo_types *types = mo_image_types(image);
    const struct mo_bytecode_function *fn = &mo_image_bytecode(image)->functions[reader];
    uint32_t walk_type = fn->parameters[4].type;
    struct mo_cell fields[] = {mo_cell_int(headers), mo_cell_int(body)};
    struct mo_cell start[] = {mo_cell_int(0), mo_cell_int(0), mo_cell_int(0)};
    struct mo_cell mine[5];
    MO_ASSERT(types->items[walk_type].cases[0].field_count == 3);
    MO_ASSERT(
        mo_value_list(heap, types->items[walk_type].cases[0].fields[1].type, NULL, 0, &start[1]));
    MO_ASSERT(mo_value_text(heap, head ? "HEAD" : "GET", head ? 4 : 3, &mine[0]));
    MO_ASSERT(mo_value_bytes(heap, received, first, &mine[1]));
    MO_ASSERT(mo_value_case(heap, fn->parameters[3].type, 0, fields, 2, &mine[3]));
    MO_ASSERT(mo_value_case(heap, walk_type, 0, start, 3, &mine[4]));
    mine[2] = mo_cell_bool(false);

    struct mo_cell early = read_from(process, mine);
    uint32_t type = fn->result_type;
    struct mo_cell value = early;
    if (inside(&type, &value, "Error") && inside(&type, &value, "More")) {
        struct mo_cell walk = mo_value_dup(heap, walk_type, value);
        mo_value_drop(heap, walk_type, mine[4]);
        mine[4] = walk;
    }
    mo_value_drop(heap, fn->result_type, early);
    mo_value_drop(heap, fn->parameters[1].type, mine[1]);
    MO_ASSERT(mo_value_bytes(heap, received, length, &mine[1]));
    mine[2] = mo_cell_bool(finished);

    struct mo_cell result = read_from(process, mine);
    type = fn->result_type;
    value = result;
    if (inside(&type, &value, "Ok")) {
        size_t got;
        MO_ASSERT(mo_case_shape(types, type, 0) == MO_CASE_BOXED);
        const uint8_t *bytes = mo_value_data(mo_value_field(mo_cell_get_payload(value), 2), &got);
        MO_ASSERT(got <= (size_t)body && (!head || got == 0));
        size_t blank = 0;
        while (blank + 4 <= length && memcmp(received + blank, "\r\n\r\n", 4) != 0)
            blank++;
        MO_ASSERT(blank + 4 <= length && blank + 4 <= (size_t)headers);
        for (size_t i = 0; i < blank; i++)
            MO_ASSERT(received[i] == '\t' || (received[i] >= ' ' && received[i] != 127) ||
                      (received[i] == '\r' && received[i + 1] == '\n') ||
                      (received[i] == '\n' && i > 0 && received[i - 1] == '\r'));
        if (chunked && !head)
            chunks_equal(received + prefix, size - 3, bytes, got);
    } else if (inside(&type, &value, "Error") && inside(&type, &value, "More"))
        MO_ASSERT(!finished);
    mo_value_drop(heap, fn->result_type, result);
    for (uint32_t i = 0; i < 5; i++)
        mo_value_drop(heap, fn->parameters[i].type, mine[i]);
    mo_process_free(process);
    return 0;
}
