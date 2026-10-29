#include "base/stack.h"
#include "service_compile.h"
#include "test.h"
#include "tools/protocol.h"

static const char source[] =
    "module Main\n"
    "struct Record\nname: Text\nkind: EntryKind\nend\n"
    "enum Choice\nEnd\nRaw(Text)\nNamed(value: Text)\nPair(left: Int, right: Text)\nend\n"
    "enum Chain\nDone\nNext(Chain)\nend\n"
    "struct Payload\nentry: Entry\nalias: Record\npair: (Int, Text)\n"
    "list: List(Option(Text))\nset: Set(Int)\nmap: Map(Int, Text)\nchoice: Choice\nchain: "
    "Chain\nend\n"
    "service\nstate Int = 0\ntools echo\ntoken from env \"TOKEN\"\nend\n"
    "fn echo(_state: Int, _now: Time, value: Payload) : Payload\nvalue\nend\n"
    "fn check(value: Payload) : Bool\n"
    "value == Payload(entry: Entry(name: \"f\", kind: File), "
    "alias: Record(name: \"f\", kind: File), pair: (-3, \"pair\"), "
    "list: [None, Some(\"x\")], set: [7, -2].to_set, map: {7: \"a\", -2: \"b\"}, "
    "choice: Pair(left: 11, right: \"right\"), chain: Done)\nend\n";

static const char input[] =
    "{\"choice\":{\"case\":\"Pair\",\"fields\":{\"right\":\"right\",\"left\":\"011\"}},"
    "\"entry\":{\"name\":\"f\",\"kind\":{\"case\":\"File\"}},"
    "\"alias\":{\"name\":\"f\",\"kind\":{\"case\":\"File\"}},"
    "\"pair\":[\"-03\",\"pair\"],\"list\":[{\"case\":\"None\"},{\"case\":\"Some\",\"value\":\"x\"}]"
    ","
    "\"set\":[\"007\",\"-2\"],\"map\":[[\"7\",\"a\"],[\"-2\",\"b\"]],\"chain\":{\"case\":\"Done\"}"
    "}";

static const char expected[] =
    "{\"entry\":{\"name\":\"f\",\"kind\":{\"case\":\"File\"}},"
    "\"alias\":{\"name\":\"f\",\"kind\":{\"case\":\"File\"}},"
    "\"pair\":[\"-3\",\"pair\"],\"list\":[{\"case\":\"None\"},{\"case\":\"Some\",\"value\":\"x\"}],"
    "\"set\":[\"7\",\"-2\"],\"map\":[[\"7\",\"a\"],[\"-2\",\"b\"]],"
    "\"choice\":{\"case\":\"Pair\",\"fields\":{\"left\":\"11\",\"right\":\"right\"}},\"chain\":{"
    "\"case\":\"Done\"}}";

static uint32_t function(const struct mo_image *image, const char *wanted) {
    const struct mo_bytecode *file = mo_image_bytecode(image);
    for (uint32_t i = 0; i < file->function_count; i++) {
        const struct mo_bytecode_string *name = &file->strings[file->functions[i].name];
        if (name->length == strlen(wanted) && memcmp(name->bytes, wanted, name->length) == 0)
            return i;
    }
    MO_UNREACHABLE();
}

static struct mo_cell call(struct mo_image *image, struct mo_process *process, const char *name,
                           const struct mo_cell *values, uint32_t count) {
    uint32_t index = function(image, name);
    const struct mo_bytecode_function *fn = &mo_image_bytecode(image)->functions[index];
    MO_ASSERT(count == fn->parameter_count && count <= 5);
    struct mo_cell args[5];
    for (uint32_t i = 0; i < count; i++)
        args[i] = fn->parameters[i].ownership == MO_PARAMETER_OWNED
                      ? mo_value_dup(mo_process_heap(process), fn->parameters[i].type, values[i])
                      : values[i];
    struct mo_cell out;
    struct mo_defect defect;
    MO_ASSERT(mo_call(process, index, args, count, &out, &defect) == MO_RETURNED);
    return out;
}

static struct mo_cell result(struct mo_image *image, struct mo_process *process, const char *name,
                             const struct mo_cell *args, uint32_t count) {
    uint32_t type = mo_image_bytecode(image)->functions[function(image, name)].result_type;
    struct mo_cell out = call(image, process, name, args, count);
    MO_ASSERT(mo_cell_get_tag(out) == 0);
    struct mo_cell payload = mo_cell_get_payload(out);
    if (mo_case_shape(mo_image_types(image), type, 0) == MO_CASE_BOXED)
        payload = mo_value_field(payload, 0);
    uint32_t child = mo_image_types(image)->items[type].cases[0].fields[0].type;
    payload = mo_value_dup(mo_process_heap(process), child, payload);
    mo_value_drop(mo_process_heap(process), type, out);
    return payload;
}

static struct mo_cell decode(struct mo_image *image, struct mo_process *process,
                             struct mo_cell graph, struct mo_cell index, const char *text) {
    struct mo_heap *heap = mo_process_heap(process);
    struct mo_cell string;
    MO_ASSERT(mo_value_text(heap, text, strlen(text), &string));
    struct mo_cell json = result(image, process, "Json.parse", &string, 1);
    const struct mo_bytecode *file = mo_image_bytecode(image);
    mo_value_drop(heap, file->functions[function(image, "Json.parse")].parameters[0].type, string);
    struct mo_cell args[] = {graph, index, json, mo_cell_int(60), mo_cell_int(4096)};
    struct mo_cell value = result(image, process, "McpValue.decode", args, 5);
    mo_value_drop(heap, file->functions[function(image, "McpValue.decode")].parameters[2].type,
                  json);
    return value;
}

static void depths(const struct mo_protocol_values *types, struct mo_heap *native,
                   struct mo_heap *protocol, uint32_t chain) {
    size_t before_native = mo_heap_objects(native), before_protocol = mo_heap_objects(protocol);
    struct mo_cell value;
    MO_ASSERT(mo_value_case(native, chain, 0, NULL, 0, &value));
    for (unsigned i = 1; i < 60; i++) {
        struct mo_cell next;
        MO_ASSERT(mo_value_case(native, chain, 1, &value, 1, &next));
        mo_value_drop(native, chain, value);
        value = next;
    }
    struct mo_cell packed, copied;
    MO_ASSERT(mo_protocol_value_pack(types, protocol, chain, value, 1024, &packed) ==
              MO_PROTOCOL_VALUE_OK);
    MO_ASSERT(mo_protocol_value_unpack(types, native, chain, packed, 1024, &copied) ==
              MO_PROTOCOL_VALUE_OK);
    bool equal;
    CHECK(mo_value_equal(native, chain, value, copied, &equal) && equal);
    mo_value_drop(native, chain, copied);
    struct mo_cell next;
    MO_ASSERT(mo_value_case(native, chain, 1, &value, 1, &next));
    mo_value_drop(native, chain, value);
    struct mo_cell out = mo_cell_int(73);
    CHECK(mo_protocol_value_pack(types, protocol, chain, next, 1024, &out) ==
          MO_PROTOCOL_VALUE_LIMIT);
    CHECK(mo_cell_get_int(out) == 73);
    mo_value_drop(native, chain, next);
    uint32_t list_type = types->protocol->items[types->value].cases[1].fields[0].type;
    struct mo_cell list;
    MO_ASSERT(mo_value_list(protocol, list_type, &packed, 1, &list));
    struct mo_cell fields[] = {mo_cell_int(1), list};
    MO_ASSERT(mo_value_case(protocol, types->value, 2, fields, 2, &next));
    mo_value_drop(protocol, types->value, packed);
    mo_value_drop(protocol, list_type, list);
    CHECK(mo_protocol_value_unpack(types, native, chain, next, 1024, &out) ==
          MO_PROTOCOL_VALUE_LIMIT);
    CHECK(mo_cell_get_int(out) == 73);
    mo_value_drop(protocol, types->value, next);
    CHECK(mo_heap_objects(native) == before_native && mo_heap_objects(protocol) == before_protocol);
}

static void run(void *unused) {
    (void)unused;
    struct mo_image *service = compile(source), *protocol = mo_protocol_build();
    MO_ASSERT(protocol != NULL);
    struct mo_limits limits = mo_limits_default();
    struct mo_defect defect;
    MO_ASSERT(mo_image_init(protocol, &limits, &defect) == MO_RETURNED);
    struct mo_process *p = mo_process_new(protocol, &limits), *s = mo_process_new(service, &limits);
    MO_ASSERT(p != NULL && s != NULL);
    struct mo_heap *heap = mo_process_heap(p), *to = mo_process_heap(s);
    struct mo_protocol_descriptor descriptor;
    MO_ASSERT(mo_protocol_descriptor(protocol, heap, service, 0, &descriptor));
    struct mo_cell root = mo_cell_get_payload(descriptor.value);
    struct mo_cell graph = mo_value_field(root, 0), tools = mo_value_field(root, 1), tool, argument;
    MO_ASSERT(mo_list_get(tools, 0, &tool));
    struct mo_cell arguments = mo_value_field(mo_cell_get_payload(tool), 3);
    MO_ASSERT(mo_list_get(arguments, 0, &argument));
    struct mo_cell graph_index = mo_value_field(mo_cell_get_payload(argument), 1);
    uint32_t native =
        mo_image_bytecode(service)->functions[function(service, "Main.check")].parameters[0].type;
    uint32_t value_type = mo_image_bytecode(protocol)
                              ->functions[function(protocol, "McpValue.encode")]
                              .parameters[2]
                              .type;
    struct mo_protocol_values types = {mo_image_types(protocol), mo_image_types(service),
                                       &mo_image_bytecode(service)->services[0], value_type};
    depths(&types, to, heap, types.service->items[native].cases[0].fields[7].type);
    struct mo_cell kind;
    MO_ASSERT(mo_list_get(graph, (uint32_t)mo_cell_get_int(graph_index), &kind));
    struct mo_cell members = mo_cell_get_payload(kind);
    const char *duplicates[] = {"[\"01\",\"1\"]", "[[\"01\",\"a\"],[\"1\",\"b\"]]"};
    for (uint32_t i = 0; i < 2; i++) {
        struct mo_cell member;
        MO_ASSERT(mo_list_get(members, 4 + i, &member));
        struct mo_cell member_index = mo_value_field(mo_cell_get_payload(member), 1);
        uint32_t member_type = types.service->items[native].cases[0].fields[4 + i].type;
        struct mo_cell value = decode(protocol, p, graph, member_index, duplicates[i]);
        struct mo_cell out = mo_cell_int(73);
        CHECK(mo_protocol_value_unpack(&types, to, member_type, value, 1024, &out) ==
              MO_PROTOCOL_VALUE_INVALID);
        CHECK(mo_cell_get_int(out) == 73 && mo_heap_objects(to) == 0);
        mo_value_drop(heap, value_type, value);
        value = decode(protocol, p, graph, member_index, "[]");
        MO_ASSERT(mo_protocol_value_unpack(&types, to, member_type, value, 1024, &out) ==
                  MO_PROTOCOL_VALUE_OK);
        CHECK(mo_table_size(out) == 0);
        mo_value_drop(to, member_type, out);
        mo_value_drop(heap, value_type, value);
    }
    struct mo_cell value = decode(protocol, p, graph, graph_index, input), copied;
    copied = mo_cell_int(73);
    CHECK(mo_protocol_value_unpack(&types, to, native, value, 4, &copied) ==
          MO_PROTOCOL_VALUE_LIMIT);
    CHECK(mo_cell_get_int(copied) == 73 && mo_heap_objects(to) == 0);
    MO_ASSERT(mo_protocol_value_unpack(&types, to, native, value, 1024, &copied) ==
              MO_PROTOCOL_VALUE_OK);
    mo_value_drop(heap, value_type, value);
    CHECK(mo_cell_get_bool(call(service, s, "Main.check", &copied, 1)));
    size_t before = mo_heap_objects(heap);
    value = mo_cell_int(73);
    CHECK(mo_protocol_value_pack(&types, heap, native, copied, 4, &value) ==
          MO_PROTOCOL_VALUE_LIMIT);
    CHECK(mo_cell_get_int(value) == 73 && mo_heap_objects(heap) == before);
    MO_ASSERT(mo_protocol_value_pack(&types, heap, native, copied, 1024, &value) ==
              MO_PROTOCOL_VALUE_OK);
    mo_value_drop(to, native, copied);
    CHECK(mo_heap_objects(to) == 0);
    mo_process_free(s);
    mo_image_free(service);
    struct mo_cell args[] = {graph, graph_index, value, mo_cell_int(60)};
    struct mo_cell json = result(protocol, p, "McpValue.encode", args, 4);
    mo_value_drop(heap, value_type, value);
    struct mo_cell text = call(protocol, p, "Json.to_text@Json", &json, 1);
    size_t size;
    const uint8_t *bytes = mo_value_data(text, &size);
    CHECK(size == strlen(expected) && memcmp(bytes, expected, size) == 0);
    const struct mo_bytecode_function *render =
        &mo_image_bytecode(protocol)->functions[function(protocol, "Json.to_text@Json")];
    mo_value_drop(heap, render->result_type, text);
    mo_value_drop(heap, render->parameters[0].type, json);
    mo_value_drop(heap, descriptor.type, descriptor.value);
    CHECK(mo_heap_objects(heap) == 0);
    mo_process_free(p);
    mo_image_free(protocol);
}

int main(void) {
    MO_ASSERT(mo_with_stack(MO_SOURCE_STACK_SIZE, run, NULL) == 0);
    return mo_test_report("protocol composites");
}
