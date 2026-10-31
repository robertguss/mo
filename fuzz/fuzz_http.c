#include "base/limits.h"
#include "tools/protocol.h"
#include "vm/vm.h"

#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static struct mo_image *image;
static uint32_t parse;
static uint32_t scan;
static uint32_t authorization;
static uint32_t chunk_size;
static uint32_t header_type;
static uint32_t authority_parse, authority_check, authority_equal, inspect, body_plan;
static uint32_t fixed, chunked, feed;

static uint32_t function_named(const char *wanted) {
    const struct mo_bytecode *file = mo_image_bytecode(image);
    for (uint32_t i = 0; i < file->function_count; i++) {
        const struct mo_bytecode_string *name = &file->strings[file->functions[i].name];
        if (name->length == strlen(wanted) && memcmp(name->bytes, wanted, name->length) == 0)
            return i;
    }
    MO_UNREACHABLE();
}

static struct mo_cell call(struct mo_process *process, uint32_t index,
                           const struct mo_cell *values) {
    const struct mo_bytecode_function *fn = &mo_image_bytecode(image)->functions[index];
    struct mo_heap *heap = mo_process_heap(process);
    struct mo_cell args[3], result;
    struct mo_defect defect;
    MO_ASSERT(fn->parameter_count <= 3);
    mo_process_instruction_limit(process, MO_LIMIT_MCP_REQUEST_WORK);
    for (uint32_t i = 0; i < fn->parameter_count; i++)
        args[i] = fn->parameters[i].ownership == MO_PARAMETER_OWNED
                      ? mo_value_dup(heap, fn->parameters[i].type, values[i])
                      : values[i];
    MO_ASSERT(mo_call(process, index, args, fn->parameter_count, &result, &defect) == MO_RETURNED);
    return result;
}

static struct mo_cell payload(uint32_t type, struct mo_cell value) {
    struct mo_cell out = mo_cell_get_payload(value);
    return mo_case_shape(mo_image_types(image), type, mo_cell_get_tag(value)) == MO_CASE_BOXED
               ? mo_value_field(out, 0)
               : out;
}

static void drop_result(struct mo_process *process, uint32_t index, struct mo_cell value) {
    mo_value_drop(mo_process_heap(process), mo_image_bytecode(image)->functions[index].result_type,
                  value);
}

static bool equal_values(struct mo_heap *heap, uint32_t type, struct mo_cell left,
                         struct mo_cell right) {
    bool equal;
    MO_ASSERT(mo_value_equal(heap, type, left, right, &equal));
    return equal;
}

static void prepare(void) {
    if (image != NULL)
        return;
    image = mo_protocol_build();
    MO_ASSERT(image != NULL);
    struct mo_limits limits = mo_limits_default();
    struct mo_defect defect;
    MO_ASSERT(mo_image_init(image, &limits, &defect) == MO_RETURNED);
    const struct mo_bytecode *file = mo_image_bytecode(image);
    parse = scan = authorization = chunk_size = UINT32_MAX;
    for (uint32_t i = 0; i < file->function_count; i++) {
        const struct mo_bytecode_string *name = &file->strings[file->functions[i].name];
        if (name->length == 18 && memcmp(name->bytes, "Http.parse_headers", 18) == 0)
            parse = i;
        if (name->length == 17 && memcmp(name->bytes, "Http.scan_headers", 17) == 0)
            scan = i;
        if (name->length == 16 && memcmp(name->bytes, "Http.bearer_span", 16) == 0)
            authorization = i;
        if (name->length == 15 && memcmp(name->bytes, "Http.chunk_size", 15) == 0)
            chunk_size = i;
    }
    MO_ASSERT(parse != UINT32_MAX && file->functions[parse].parameter_count == 2);
    MO_ASSERT(scan != UINT32_MAX && file->functions[scan].parameter_count == 3);
    MO_ASSERT(authorization != UINT32_MAX && chunk_size != UINT32_MAX);
    const struct mo_types *types = mo_image_types(image);
    header_type = file->functions[parse].parameters[1].type;
    MO_ASSERT(types->items[header_type].kind == MO_TYPE_CHOICE &&
              types->items[header_type].case_count == 1 &&
              types->items[header_type].cases[0].field_count == 3);
    authority_parse = function_named("HttpAuthority.parse");
    authority_check = function_named("HttpAuthority.check");
    authority_equal = function_named("HttpAuthority.equal?");
    inspect = function_named("HttpPolicy.inspect");
    body_plan = function_named("HttpPolicy.body_plan");
    fixed = function_named("HttpBody.fixed");
    chunked = function_named("HttpBody.chunked");
    feed = function_named("HttpBody.feed");
}

static void span(struct mo_cell value, size_t size) {
    struct mo_cell fields = mo_cell_get_payload(value);
    int64_t start = mo_cell_get_int(mo_value_field(fields, 0));
    int64_t length = mo_cell_get_int(mo_value_field(fields, 1));
    MO_ASSERT(start >= 0 && length >= 0 && (uint64_t)start <= size);
    MO_ASSERT((uint64_t)length <= size - (size_t)start);
}

static bool ascii_equal(const uint8_t *a, size_t a_size, const uint8_t *b, size_t b_size) {
    if (a_size != b_size)
        return false;
    for (size_t i = 0; i < a_size; i++) {
        unsigned x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z')
            x += 'a' - 'A';
        if (y >= 'A' && y <= 'Z')
            y += 'a' - 'A';
        if (x != y)
            return false;
    }
    return true;
}

static struct mo_cell authority_input(struct mo_process *process, const uint8_t *data,
                                      size_t size) {
    struct mo_heap *heap = mo_process_heap(process);
    const struct mo_bytecode *file = mo_image_bytecode(image);
    struct mo_cell text, list;
    MO_ASSERT(mo_value_text(heap, (const char *)data, size, &text));
    struct mo_cell parsed = call(process, authority_parse, &text);
    MO_ASSERT(
        mo_value_list(heap, file->functions[authority_check].parameters[0].type, &text, 1, &list));
    struct mo_cell checked = call(process, authority_check, &list);
    MO_ASSERT(mo_cell_get_bool(checked) == (mo_cell_get_tag(parsed) == 1));
    mo_value_drop(heap, file->functions[authority_check].parameters[0].type, list);
    if (mo_cell_get_tag(parsed) == 1) {
        struct mo_cell value = payload(file->functions[authority_parse].result_type, parsed);
        struct mo_cell record = mo_cell_get_payload(value);
        size_t host_size, port_size;
        const uint8_t *host = mo_value_data(mo_value_field(record, 0), &host_size);
        const uint8_t *port = mo_value_data(mo_value_field(record, 1), &port_size);
        MO_ASSERT(host_size > 0 && host_size <= size && memcmp(host, data, host_size) == 0);
        size_t at = host_size;
        if (at < size) {
            MO_ASSERT(data[at] == ':');
            at++;
        }
        size_t digits = at;
        for (; at < size; at++)
            MO_ASSERT(data[at] >= '0' && data[at] <= '9');
        while (digits + 1 < size && data[digits] == '0')
            digits++;
        if (digits == size)
            MO_ASSERT(port_size == 2 && memcmp(port, "80", 2) == 0);
        else
            MO_ASSERT(port_size == size - digits && memcmp(port, data + digits, port_size) == 0);
        struct mo_cell args[] = {value, value};
        MO_ASSERT(mo_cell_get_bool(call(process, authority_equal, args)));
        uint8_t alternate[MO_LIMIT_MCP_HEADER_BYTES + 4];
        MO_ASSERT(host_size + port_size + 2 <= sizeof alternate);
        for (size_t i = 0; i < host_size; i++)
            alternate[i] = host[i] >= 'a' && host[i] <= 'z' ? (uint8_t)(host[i] - 32) : host[i];
        alternate[host_size] = ':';
        alternate[host_size + 1] = '0';
        memcpy(alternate + host_size + 2, port, port_size);
        struct mo_cell other_text;
        MO_ASSERT(
            mo_value_text(heap, (const char *)alternate, host_size + port_size + 2, &other_text));
        struct mo_cell other = call(process, authority_parse, &other_text);
        MO_ASSERT(mo_cell_get_tag(other) == 1);
        args[1] = payload(file->functions[authority_parse].result_type, other);
        MO_ASSERT(mo_cell_get_bool(call(process, authority_equal, args)));
        drop_result(process, authority_parse, other);
        mo_value_drop(heap, file->functions[authority_parse].parameters[0].type, other_text);
        MO_ASSERT(mo_value_text(heap, "localhost", 9, &other_text));
        other = call(process, authority_parse, &other_text);
        MO_ASSERT(mo_cell_get_tag(other) == 1);
        args[1] = payload(file->functions[authority_parse].result_type, other);
        bool local = ascii_equal(host, host_size, (const uint8_t *)"localhost", 9) &&
                     port_size == 2 && memcmp(port, "80", 2) == 0;
        MO_ASSERT(mo_cell_get_bool(call(process, authority_equal, args)) == local);
        drop_result(process, authority_parse, other);
        mo_value_drop(heap, file->functions[authority_parse].parameters[0].type, other_text);
    }
    mo_value_drop(heap, file->functions[authority_parse].parameters[0].type, text);
    return parsed;
}

struct body_result {
    struct mo_cell state;
    int64_t error;
    size_t consumed;
    size_t size;
    uint8_t bytes[MO_LIMIT_MCP_HEADER_BYTES];
};

static struct body_result body_fragments(struct mo_process *process, struct mo_cell initial,
                                         const struct mo_cell *bounds, const uint8_t *data,
                                         size_t size, size_t fragment_size) {
    struct mo_heap *heap = mo_process_heap(process);
    const struct mo_bytecode_function *fn = &mo_image_bytecode(image)->functions[feed];
    struct body_result out = {.state = mo_value_dup(heap, fn->parameters[0].type, initial)};
    size_t offset = 0;
    do {
        size_t amount = size - offset;
        if (amount > fragment_size)
            amount = fragment_size;
        struct mo_cell cells[MO_LIMIT_MCP_HEADER_BYTES];
        for (size_t i = 0; i < amount; i++)
            cells[i] = mo_cell_byte(data[offset + i]);
        struct mo_cell args[] = {out.state, mo_cell_unit(), mo_cell_unit()};
        MO_ASSERT(mo_value_list(heap, fn->parameters[1].type, cells, (uint32_t)amount, &args[1]));
        MO_ASSERT(mo_value_case(heap, fn->parameters[2].type, 0, bounds, 6, &args[2]));
        struct mo_cell result = call(process, feed, args);
        mo_value_drop(heap, fn->parameters[1].type, args[1]);
        mo_value_drop(heap, fn->parameters[2].type, args[2]);
        if (mo_cell_get_tag(result) == 1) {
            out.error = mo_cell_get_int(payload(fn->result_type, result));
            MO_ASSERT(out.error == 400 || out.error == 413);
            drop_result(process, feed, result);
            return out;
        }
        struct mo_cell step = mo_cell_get_payload(payload(fn->result_type, result));
        struct mo_cell state = mo_value_field(step, 0);
        struct mo_cell record = mo_cell_get_payload(state);
        struct mo_cell before = mo_cell_get_payload(out.state);
        int64_t consumed = mo_cell_get_int(mo_value_field(step, 1));
        MO_ASSERT(consumed >= 0 && (uint64_t)consumed <= amount);
        int64_t encoded = mo_cell_get_int(mo_value_field(record, 1));
        int64_t decoded = mo_cell_get_int(mo_value_field(record, 2));
        MO_ASSERT(encoded - mo_cell_get_int(mo_value_field(before, 1)) == consumed);
        MO_ASSERT(encoded <= mo_cell_get_int(bounds[1]) && decoded <= mo_cell_get_int(bounds[0]));
        MO_ASSERT(mo_cell_get_int(mo_value_field(record, 3)) <= mo_cell_get_int(bounds[2]));
        MO_ASSERT(mo_cell_get_int(mo_value_field(record, 4)) <= mo_cell_get_int(bounds[4]));
        MO_ASSERT(mo_cell_get_int(mo_value_field(record, 5)) <= mo_cell_get_int(bounds[5]));
        for (uint32_t i = 3; i < 6; i++)
            MO_ASSERT(mo_cell_get_int(mo_value_field(record, i)) >=
                      mo_cell_get_int(mo_value_field(before, i)));
        struct mo_cell phase = mo_value_field(record, 0);
        uint32_t tag = mo_cell_get_tag(phase);
        MO_ASSERT(tag <= 6);
        if (tag == 1 || tag == 5)
            MO_ASSERT(mo_list_size(mo_cell_get_payload(phase)) <=
                      (uint64_t)mo_cell_get_int(bounds[3]) + 1);
        struct mo_cell spans = mo_value_field(step, 2);
        size_t previous = 0, collected = 0;
        for (uint32_t i = 0; i < mo_list_size(spans); i++) {
            struct mo_cell part;
            MO_ASSERT(mo_list_get(spans, i, &part));
            span(part, (size_t)consumed);
            struct mo_cell fields = mo_cell_get_payload(part);
            size_t start = (size_t)mo_cell_get_int(mo_value_field(fields, 0));
            size_t length = (size_t)mo_cell_get_int(mo_value_field(fields, 1));
            MO_ASSERT(start >= previous && length > 0 && out.size + length <= sizeof out.bytes);
            memcpy(out.bytes + out.size, data + offset + start, length);
            previous = start + length;
            out.size += length;
            collected += length;
        }
        MO_ASSERT(decoded - mo_cell_get_int(mo_value_field(before, 2)) == (int64_t)collected);
        bool complete = mo_cell_get_tag(mo_value_field(record, 0)) == 6;
        MO_ASSERT(complete || (size_t)consumed == amount);
        mo_value_drop(heap, fn->parameters[0].type, out.state);
        out.state = mo_value_dup(heap, fn->parameters[0].type, state);
        out.consumed += (size_t)consumed;
        drop_result(process, feed, result);
        offset += amount;
    } while (offset < size);
    return out;
}

static void chunk_payload(const uint8_t *data, const struct body_result *result) {
    size_t at = 0, decoded = 0;
    while (true) {
        size_t length = 0, digits = 0;
        while (at < result->consumed) {
            unsigned c = data[at];
            unsigned digit = c >= '0' && c <= '9'   ? c - '0'
                             : c >= 'a' && c <= 'f' ? c - 'a' + 10
                             : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                                    : 16;
            if (digit == 16)
                break;
            MO_ASSERT(length <= MO_LIMIT_MCP_BODY_BYTES / 16);
            length = length * 16 + digit;
            digits++;
            at++;
        }
        MO_ASSERT(digits > 0);
        while (at + 1 < result->consumed && memcmp(data + at, "\r\n", 2) != 0)
            at++;
        MO_ASSERT(at + 2 <= result->consumed);
        at += 2;
        if (length == 0)
            break;
        MO_ASSERT(length <= result->consumed - at && length <= result->size - decoded);
        MO_ASSERT(memcmp(data + at, result->bytes + decoded, length) == 0);
        at += length;
        decoded += length;
        MO_ASSERT(at + 2 <= result->consumed && memcmp(data + at, "\r\n", 2) == 0);
        at += 2;
    }
    while (at + 1 < result->consumed && memcmp(data + at, "\r\n", 2) != 0) {
        while (at + 1 < result->consumed && memcmp(data + at, "\r\n", 2) != 0)
            at++;
        MO_ASSERT(at + 2 <= result->consumed);
        at += 2;
    }
    MO_ASSERT(at + 2 == result->consumed && decoded == result->size);
}

static void body_input(struct mo_process *process, const uint8_t *data, size_t size, bool chunks,
                       int64_t length, bool strict) {
    const struct mo_bytecode_function *fn = &mo_image_bytecode(image)->functions[feed];
    struct mo_heap *heap = mo_process_heap(process);
    const int64_t limits[] = {MO_LIMIT_MCP_BODY_BYTES,    MO_LIMIT_MCP_INGRESS_BYTES,
                              MO_LIMIT_MCP_CHUNKS,        MO_LIMIT_MCP_LINE_BYTES,
                              MO_LIMIT_MCP_TRAILER_BYTES, MO_LIMIT_MCP_TRAILER_FIELDS};
    struct mo_cell bounds[6];
    for (size_t i = 0; i < 6; i++)
        bounds[i] = mo_cell_int(strict && size > i ? data[i] % 32 : limits[i]);
    struct mo_cell argument = mo_cell_int(length);
    struct mo_cell initial = call(process, chunks ? chunked : fixed, &argument);
    struct body_result whole = body_fragments(process, initial, bounds, data, size, size + 1);
    size_t fragment_size = size == 0 ? 1 : 1 + data[size - 1] % 31;
    struct body_result pieces = body_fragments(process, initial, bounds, data, size, fragment_size);
    MO_ASSERT(whole.error == pieces.error);
    if (whole.error == 0) {
        MO_ASSERT(whole.consumed == pieces.consumed && whole.size == pieces.size);
        MO_ASSERT(memcmp(whole.bytes, pieces.bytes, whole.size) == 0);
        MO_ASSERT(equal_values(heap, fn->parameters[0].type, whole.state, pieces.state));
        bool complete = mo_cell_get_tag(mo_value_field(mo_cell_get_payload(whole.state), 0)) == 6;
        if (!chunks) {
            size_t expected = (uint64_t)length < size ? (size_t)length : size;
            MO_ASSERT(whole.consumed == expected && whole.size == expected);
            MO_ASSERT(memcmp(whole.bytes, data, expected) == 0);
            MO_ASSERT(complete == ((uint64_t)length <= size));
        } else if (complete)
            chunk_payload(data, &whole);
        if (complete) {
            static const uint8_t suffix[] = "POST /mcp HTTP/1.1\r\n";
            struct body_result next = body_fragments(process, whole.state, bounds, suffix,
                                                     sizeof suffix - 1, sizeof suffix);
            MO_ASSERT(next.error == 0 && next.consumed == 0 && next.size == 0);
            MO_ASSERT(equal_values(heap, fn->parameters[0].type, whole.state, next.state));
            mo_value_drop(heap, fn->parameters[0].type, next.state);
        }
    }
    mo_value_drop(heap, fn->parameters[0].type, whole.state);
    mo_value_drop(heap, fn->parameters[0].type, pieces.state);
    mo_value_drop(heap, fn->parameters[0].type, initial);
}

struct part {
    const uint8_t *bytes;
    size_t size;
};

static struct part part_view(const uint8_t *data, struct mo_cell value) {
    struct mo_cell fields = mo_cell_get_payload(value);
    return (struct part){data + (size_t)mo_cell_get_int(mo_value_field(fields, 0)),
                         (size_t)mo_cell_get_int(mo_value_field(fields, 1))};
}

static bool part_equal(struct part part, const char *text, bool insensitive) {
    size_t size = strlen(text);
    return insensitive ? ascii_equal(part.bytes, part.size, (const uint8_t *)text, size)
                       : part.size == size && memcmp(part.bytes, text, size) == 0;
}

static int authority_status(struct mo_process *process, struct part part) {
    for (size_t i = 0; i < part.size; i++)
        if (part.bytes[i] < 33 || part.bytes[i] > 126)
            return 400;
    struct mo_cell parsed = authority_input(process, part.bytes, part.size);
    int status = 400;
    if (mo_cell_get_tag(parsed) == 1) {
        struct mo_cell record = mo_cell_get_payload(
            payload(mo_image_bytecode(image)->functions[authority_parse].result_type, parsed));
        size_t host_size, port_size;
        const uint8_t *host = mo_value_data(mo_value_field(record, 0), &host_size);
        const uint8_t *port = mo_value_data(mo_value_field(record, 1), &port_size);
        status = ascii_equal(host, host_size, (const uint8_t *)"localhost", 9) && port_size == 2 &&
                         memcmp(port, "80", 2) == 0
                     ? 0
                     : 421;
    }
    drop_result(process, authority_parse, parsed);
    return status;
}

static void policy_input(struct mo_process *process, const uint8_t *data, size_t size,
                         const struct mo_cell *cells, struct mo_cell head) {
    struct mo_heap *heap = mo_process_heap(process);
    const struct mo_bytecode *file = mo_image_bytecode(image);
    struct mo_cell record = mo_cell_get_payload(head);
    struct mo_cell fields = mo_value_field(record, 3);
    size_t hosts = 0, origins = 0, lengths = 0, transfers = 0;
    struct part host = {data, 0}, length_part = {data, 0};
    for (uint32_t i = 0; i < mo_list_size(fields); i++) {
        struct mo_cell field;
        MO_ASSERT(mo_list_get(fields, i, &field));
        struct mo_cell parts = mo_cell_get_payload(field);
        struct part name = part_view(data, mo_value_field(parts, 0));
        struct part value = part_view(data, mo_value_field(parts, 1));
        if (part_equal(name, "host", true)) {
            hosts++;
            host = value;
        }
        if (part_equal(name, "origin", true))
            origins++;
        if (part_equal(name, "content-length", true)) {
            lengths++;
            length_part = value;
        }
        if (part_equal(name, "transfer-encoding", true))
            transfers++;
    }
    int expected = 0;
    struct part target = part_view(data, mo_value_field(record, 1));
    if (!part_equal(part_view(data, mo_value_field(record, 2)), "HTTP/1.1", false))
        expected = 505;
    else if (hosts != 1)
        expected = 400;
    else {
        expected = authority_status(process, host);
        if (expected != 400 && target.size >= 7 &&
            ascii_equal(target.bytes, 7, (const uint8_t *)"http://", 7)) {
            size_t at = 7;
            while (at < target.size && target.bytes[at] != '/' && target.bytes[at] != '?' &&
                   target.bytes[at] != '#')
                at++;
            expected = authority_status(process, (struct part){target.bytes + 7, at - 7});
            target.bytes += at;
            target.size -= at;
        }
        if (expected == 0 && origins != 0)
            expected = 403;
        if (expected == 0 && !part_equal(target, "/mcp", false))
            expected = 404;
        if (expected == 0 && !part_equal(part_view(data, mo_value_field(record, 0)), "POST", false))
            expected = 405;
    }
    struct mo_cell allowed;
    MO_ASSERT(mo_value_text(heap, "localhost", 9, &allowed));
    struct mo_cell args[] = {mo_cell_unit(), head, mo_cell_unit()};
    MO_ASSERT(mo_value_list(heap, file->functions[inspect].parameters[0].type, cells,
                            (uint32_t)size, &args[0]));
    MO_ASSERT(
        mo_value_list(heap, file->functions[inspect].parameters[2].type, &allowed, 1, &args[2]));
    struct mo_cell result = call(process, inspect, args);
    if (expected != 0) {
        MO_ASSERT(mo_cell_get_tag(result) == 1);
        MO_ASSERT(mo_cell_get_int(payload(file->functions[inspect].result_type, result)) ==
                  expected);
    } else {
        MO_ASSERT(mo_cell_get_tag(result) == 0);
        struct mo_cell token = payload(file->functions[inspect].result_type, result);
        struct mo_cell actual = call(process, authorization, args);
        MO_ASSERT(equal_values(heap, file->functions[authorization].result_type, token, actual));
        if (mo_cell_get_tag(token) == 1) {
            struct mo_cell value = payload(file->functions[authorization].result_type, token);
            size_t after = (size_t)mo_cell_get_int(mo_value_field(record, 4));
            span(value, after);
            struct part credential = part_view(data, value);
            const struct mo_bearer bearer = {.bytes = "fuzz-credential", .length = 15};
            if (mo_bearer_check(&bearer, credential.bytes, credential.size)) {
                struct mo_cell plan_args[] = {args[0], head, mo_cell_int(MO_LIMIT_MCP_BODY_BYTES)};
                struct mo_cell plan = call(process, body_plan, plan_args);
                uint32_t tag = mo_cell_get_tag(plan);
                MO_ASSERT(tag <= 2);
                if (tag == 0) {
                    int64_t code = mo_cell_get_int(mo_cell_get_payload(plan));
                    MO_ASSERT(code == 417 || code == 415 || code == 406 || code == 400 ||
                              code == 413 || code == 501);
                } else {
                    MO_ASSERT(lengths <= 1 && (lengths == 0 || transfers == 0));
                    int64_t length = 0;
                    if (tag == 1) {
                        MO_ASSERT(transfers == 0);
                        if (lengths == 1) {
                            MO_ASSERT(length_part.size > 0);
                            for (size_t i = 0; i < length_part.size; i++) {
                                unsigned c = length_part.bytes[i];
                                MO_ASSERT(c >= '0' && c <= '9');
                                MO_ASSERT(length <= MO_LIMIT_MCP_BODY_BYTES);
                                length = length * 10 + (int64_t)(c - '0');
                            }
                        }
                        MO_ASSERT(length <= MO_LIMIT_MCP_BODY_BYTES);
                        MO_ASSERT(mo_cell_get_int(mo_value_field(mo_cell_get_payload(plan), 0)) ==
                                  length);
                    } else
                        MO_ASSERT(transfers > 0 && lengths == 0);
                    body_input(process, data + after, size - after, tag == 2, length, false);
                }
                drop_result(process, body_plan, plan);
            }
        }
        drop_result(process, authorization, actual);
    }
    drop_result(process, inspect, result);
    mo_value_drop(heap, file->functions[inspect].parameters[0].type, args[0]);
    mo_value_drop(heap, file->functions[inspect].parameters[2].type, args[2]);
    mo_value_drop(heap, file->functions[authority_parse].parameters[0].type, allowed);
}

static void scan_input(const uint8_t *data, size_t size, const struct mo_cell *cells) {
    struct mo_limits limits = mo_limits_default();
    limits.heap_size = MO_LIMIT_MCP_HEADER_HEAP;
    struct mo_process *process = mo_process_new(image, &limits);
    MO_ASSERT(process != NULL);
    mo_process_confidential(process);
    mo_process_instruction_limit(process, MO_LIMIT_MCP_HEADER_WORK);
    struct mo_heap *heap = mo_process_heap(process);
    const struct mo_bytecode_function *function = &mo_image_bytecode(image)->functions[scan];
    struct mo_cell args[3];
    MO_ASSERT(mo_value_list(heap, function->parameters[0].type, cells, (uint32_t)size, &args[0]));
    struct mo_cell state[] = {mo_cell_int(0), mo_cell_int(0), mo_cell_bool(false)};
    MO_ASSERT(mo_value_case(heap, function->parameters[1].type, 0, state, 3, &args[1]));
    args[2] = mo_cell_int(MO_LIMIT_MCP_HEADER_BYTES);
    struct mo_cell result;
    struct mo_defect defect;
    MO_ASSERT(mo_call(process, scan, args, 3, &result, &defect) == MO_RETURNED);
    MO_ASSERT(mo_cell_get_tag(result) == 0);
    struct mo_cell record = mo_cell_get_payload(mo_value_field(mo_cell_get_payload(result), 0));
    size_t consumed = size;
    bool complete = false;
    for (size_t i = 0; i + 4 <= size; i++)
        if (memcmp(data + i, "\r\n\r\n", 4) == 0) {
            consumed = i + 4;
            complete = true;
            break;
        }
    MO_ASSERT(mo_cell_get_int(mo_value_field(record, 1)) == (int64_t)consumed);
    MO_ASSERT(mo_cell_get_bool(mo_value_field(record, 2)) == complete);
    mo_process_free(process);
}

static void chunk_input(const uint8_t *data, size_t size) {
    static const char hex_digits[] = "0123456789abcdef";
    struct mo_limits limits = mo_limits_default();
    limits.heap_size = MO_LIMIT_MCP_HEADER_HEAP;
    struct mo_process *process = mo_process_new(image, &limits);
    MO_ASSERT(process != NULL);
    mo_process_confidential(process);
    mo_process_instruction_limit(process, MO_LIMIT_MCP_REQUEST_WORK);
    struct mo_heap *heap = mo_process_heap(process);
    const struct mo_bytecode_function *function = &mo_image_bytecode(image)->functions[chunk_size];
    struct mo_cell cells[MO_LIMIT_MCP_LINE_BYTES + 2];
    for (size_t i = 0; i < size; i++)
        cells[i] = mo_cell_byte(data[i]);
    struct mo_cell args[3];
    MO_ASSERT(mo_value_list(heap, function->parameters[0].type, cells, (uint32_t)size, &args[0]));
    args[1] = mo_cell_int(MO_LIMIT_MCP_BODY_BYTES);
    args[2] = mo_cell_int(MO_LIMIT_MCP_LINE_BYTES);
    struct mo_cell result;
    struct mo_defect defect;
    MO_ASSERT(mo_call(process, chunk_size, args, 3, &result, &defect) == MO_RETURNED);
    if (mo_cell_get_tag(result) == 0) {
        int64_t amount = mo_cell_get_int(mo_cell_get_payload(result));
        MO_ASSERT(size >= 3 && memcmp(data + size - 2, "\r\n", 2) == 0);
        MO_ASSERT(amount >= 0 && amount <= MO_LIMIT_MCP_BODY_BYTES);
        uint64_t expected = 0;
        size_t digits = 0;
        for (; digits < size; digits++) {
            const char *hex = memchr(hex_digits, data[digits] | 32, 16);
            if (hex == NULL)
                break;
            expected = expected * 16 + (uint64_t)(hex - hex_digits);
            MO_ASSERT(expected <= MO_LIMIT_MCP_BODY_BYTES);
        }
        MO_ASSERT(digits > 0 && expected == (uint64_t)amount);
    }
    mo_process_free(process);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    size_t marks;
    bool counted = mo_protocol_body_marks(data, size, &marks);
    MO_ASSERT(counted == (size <= MO_LIMIT_MCP_BODY_BYTES));
    size_t expected = 0;
    bool quoted = false;
    if (counted) {
        for (size_t i = 0; i < size; i++) {
            if (data[i] == '"') {
                size_t before = i;
                while (before > 0 && data[before - 1] == '\\')
                    before--;
                if (!quoted || (i - before) % 2 == 0)
                    quoted = !quoted;
            } else if (!quoted && (data[i] == '[' || data[i] == '{' || data[i] == ','))
                expected++;
        }
    }
    MO_ASSERT(marks == expected);
    if (size <= MO_LIMIT_MCP_LINE_BYTES + 2) {
        prepare();
        chunk_input(data, size);
    }
    if (size > MO_LIMIT_MCP_HEADER_BYTES)
        return 0;
    const struct mo_bearer bearer = {.bytes = "fuzz-credential", .length = 15};
    MO_ASSERT(mo_bearer_check(&bearer, data, size) ==
              (size == bearer.length && memcmp(data, bearer.bytes, size) == 0));
    prepare();
    struct mo_limits limits = mo_limits_default();
    limits.heap_size = MO_LIMIT_MCP_HEADER_HEAP;
    struct mo_process *process = mo_process_new(image, &limits);
    MO_ASSERT(process != NULL);
    mo_process_confidential(process);
    mo_process_instruction_limit(process, MO_LIMIT_MCP_HEADER_WORK);
    struct mo_heap *heap = mo_process_heap(process);
    struct mo_cell cells[MO_LIMIT_MCP_HEADER_BYTES];
    for (size_t i = 0; i < size; i++)
        cells[i] = mo_cell_byte(data[i]);
    scan_input(data, size, cells);
    body_input(process, data, size, true, 0, false);
    body_input(process, data, size, true, 0, true);
    body_input(process, data, size, false, size == 0 ? 0 : data[0], false);
    body_input(process, data, size, false, size == 0 ? 0 : data[0], true);
    bool ascii = true;
    size_t authority_size = 0;
    while (authority_size < size && data[authority_size] != '\r' && data[authority_size] != '\n')
        authority_size++;
    for (size_t i = 0; i < authority_size; i++)
        if (data[i] > 127)
            ascii = false;
    if (ascii)
        drop_result(process, authority_parse, authority_input(process, data, authority_size));
    mo_process_instruction_limit(process, MO_LIMIT_MCP_HEADER_WORK);
    const struct mo_bytecode_function *function = &mo_image_bytecode(image)->functions[parse];
    struct mo_cell arguments[2];
    MO_ASSERT(
        mo_value_list(heap, function->parameters[0].type, cells, (uint32_t)size, &arguments[0]));
    struct mo_cell bounds[] = {mo_cell_int(MO_LIMIT_MCP_HEADER_BYTES),
                               mo_cell_int(MO_LIMIT_MCP_LINE_BYTES),
                               mo_cell_int(MO_LIMIT_MCP_HEADER_FIELDS)};
    MO_ASSERT(mo_value_case(heap, header_type, 0, bounds, 3, &arguments[1]));
    struct mo_cell result;
    struct mo_defect defect;
    enum mo_outcome outcome = mo_call(process, parse, arguments, 2, &result, &defect);
    MO_ASSERT(outcome != MO_FAILED);
    if (outcome == MO_RETURNED && mo_cell_get_tag(result) == 0) {
        struct mo_cell value = mo_value_field(mo_cell_get_payload(result), 0);
        struct mo_cell head = mo_cell_get_payload(value);
        int64_t after = mo_cell_get_int(mo_value_field(head, 4));
        MO_ASSERT(after >= 4 && (uint64_t)after <= size);
        size_t consumed = (size_t)after;
        MO_ASSERT(memcmp(data + consumed - 4, "\r\n\r\n", 4) == 0);
        for (uint32_t i = 0; i < 3; i++)
            span(mo_value_field(head, i), consumed);
        struct mo_cell fields = mo_value_field(head, 3);
        uint32_t count = mo_list_size(fields);
        MO_ASSERT(count <= MO_LIMIT_MCP_HEADER_FIELDS);
        for (uint32_t i = 0; i < count; i++) {
            struct mo_cell field;
            MO_ASSERT(mo_list_get(fields, i, &field));
            struct mo_cell parts = mo_cell_get_payload(field);
            span(mo_value_field(parts, 0), consumed);
            span(mo_value_field(parts, 1), consumed);
        }
        policy_input(process, data, size, cells, value);
        const struct mo_bytecode_function *auth =
            &mo_image_bytecode(image)->functions[authorization];
        struct mo_cell args[2];
        MO_ASSERT(mo_value_list(heap, auth->parameters[0].type, cells, (uint32_t)size, &args[0]));
        args[1] = mo_value_dup(heap, auth->parameters[1].type, value);
        struct mo_cell token;
        outcome = mo_call(process, authorization, args, 2, &token, &defect);
        MO_ASSERT(outcome != MO_FAILED);
        if (outcome == MO_RETURNED && mo_cell_get_tag(token) == 1)
            span(mo_value_field(mo_cell_get_payload(token), 0), consumed);
    }
    mo_process_free(process);
    return 0;
}
