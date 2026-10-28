#define _XOPEN_SOURCE 700

#include "base/stack.h"
#include "bytecode_text.h"
#include "codegen/codegen.h"
#include "lower/lower.h"
#include "program_fixture.h"
#include "vm/vm.h"

#include <time.h>

static const char *corpus;

static void seed(const struct mo_buffer *bytes, const char *name) {
    if (corpus == NULL)
        return;
    char path[MO_LIMIT_PATH_SIZE];
    int n = snprintf(path, sizeof path, "%s/%s", corpus, name);
    MO_ASSERT(n > 0 && (size_t)n < sizeof path);
    FILE *file = fopen(path, "wb");
    MO_ASSERT(file != NULL);
    CHECK(fwrite(bytes->bytes, 1, bytes->length, file) == bytes->length);
    CHECK(fclose(file) == 0);
}

static void patch(struct mo_buffer *wire, size_t offset, uint32_t value) {
    MO_ASSERT(offset + 4 <= wire->length);
    for (unsigned i = 0; i < 4; i++)
        wire->bytes[offset + i] = (uint8_t)(value >> (8 * i));
    uint64_t checksum = MO_BYTECODE_FNV_OFFSET;
    for (size_t i = MO_BYTECODE_HEADER_SIZE; i < wire->length; i++)
        checksum = (checksum ^ wire->bytes[i]) * MO_BYTECODE_FNV_PRIME;
    for (unsigned i = 0; i < 8; i++)
        wire->bytes[12 + i] = (uint8_t)(checksum >> (8 * i));
}

static void refused(const struct mo_buffer *good, size_t offset, uint32_t value,
                    const char *reason) {
    struct mo_buffer wire;
    mo_buffer_init(&wire, MO_LIMIT_BYTECODE_SIZE);
    CHECK(mo_buffer_append(&wire, good->bytes, good->length));
    patch(&wire, offset, value);
    struct mo_load_error error;
    struct mo_image *image = mo_load(wire.bytes, wire.length, &error);
    CHECK(image == NULL);
    if (image != NULL)
        mo_image_free(image);
    else {
        CHECK(error.kind == MO_BYTECODE_INVALID || error.kind == MO_BYTECODE_LIMIT);
        if (reason != NULL) {
            CHECK(strcmp(error.reason, reason) == 0);
            if (strcmp(error.reason, reason) != 0)
                CHECK(fprintf(stderr, "wanted %s; got %s\n", reason, error.reason) >= 0);
        }
    }
    if (value == UINT32_MAX)
        seed(&wire, "service-missing-function.bc");
    mo_buffer_free(&wire);
}

static bool named(const struct mo_bytecode *file, uint32_t index, const char *text) {
    const struct mo_bytecode_string *s = &file->strings[index];
    return s->length == strlen(text) && memcmp(s->bytes, text, s->length) == 0;
}

static void write_refused(const struct mo_bytecode *file, const char *reason) {
    struct mo_buffer wire;
    struct mo_write_error error;
    mo_buffer_init(&wire, MO_LIMIT_BYTECODE_SIZE);
    bool written = mo_bytecode_write(file, &wire, &error);
    CHECK(!written);
    CHECK(wire.length == 0);
    if (!written)
        CHECK(strcmp(error.reason, reason) == 0);
    mo_buffer_free(&wire);
}

static void shape_refusals(const struct mo_bytecode *file, const struct mo_buffer *wire) {
    struct mo_arena arena;
    mo_arena_init(&arena, MO_LIMIT_LOWER_WORK);
    struct mo_bytecode changed = *file;
    struct mo_bytecode_service service = file->services[0];
    changed.services = &service;
    size_t size = service.shape_count * sizeof *service.shapes;
    struct mo_bytecode_service_shape *shapes =
        mo_arena_copy(&arena, service.shapes, size, _Alignof(struct mo_bytecode_service_shape));
    MO_ASSERT(shapes != NULL && service.shape_count >= 2);
    service.shapes = shapes;
    service.shape_count--;
    write_refused(&changed, "missing reachable service shape");
    service.shape_count++;
    shapes[0].type = file->type_count;
    write_refused(&changed, "service shape type out of range");
    shapes[0] = shapes[1];
    write_refused(&changed, "duplicate or unsorted service shape");
    shapes[1] = file->services[0].shapes[0];
    write_refused(&changed, "duplicate or unsorted service shape");
    memcpy(shapes, file->services[0].shapes, size);
    for (uint32_t i = 0; i < file->type_count; i++)
        if (file->types[i].name != NULL && strcmp(file->types[i].name, "Main.Unused") == 0) {
            uint32_t at = 0;
            while (at + 1 < service.shape_count && shapes[at + 1].type < i)
                at++;
            shapes[at] = (struct mo_bytecode_service_shape){.type = i, .is_struct = true};
            write_refused(&changed, "unreachable service shape");
            shapes[at] = file->services[0].shapes[at];
        }
    size_t wire_size = 4;
    for (uint32_t i = 0; i < service.shape_count; i++)
        wire_size += 12 + 4 * shapes[i].case_count;
    size_t offset = wire->length - wire_size;
    refused(wire, offset, MO_LIMIT_TYPES + 1, "count past limit or remaining bytes");
    offset += 4;
    for (uint32_t i = 0; i < service.shape_count; i++) {
        struct mo_bytecode_service_shape *shape = &shapes[i];
        const struct mo_type *type = &file->types[shape->type];
        refused(wire, offset + 4, 2, "invalid boolean");
        refused(wire, offset + 8, MO_LIMIT_FIELDS + 1, "count past limit or remaining bytes");
        bool flag = false;
        if (shape->is_struct) {
            shape->case_count = 1;
            shape->unnamed = &flag;
            write_refused(&changed, "struct service shape has payload flags");
            if (type->kind == MO_TYPE_RECORD) {
                shape->is_struct = false;
                write_refused(&changed, "invalid service enum shape");
            }
        } else {
            bool *flags = mo_arena_copy(&arena, shape->unnamed, shape->case_count * sizeof *flags,
                                        _Alignof(bool));
            MO_ASSERT(flags != NULL);
            shape->unnamed = flags;
            shape->case_count--;
            write_refused(&changed, "invalid service enum shape");
            shape->case_count++;
            for (uint32_t j = 0; j < shape->case_count; j++) {
                refused(wire, offset + 12 + 4 * j, 2, "invalid boolean");
                if (type->cases[j].field_count != 1 || type->cases[j].unnamed) {
                    flags[j] = !flags[j];
                    write_refused(&changed, type->cases[j].unnamed
                                                ? "service shape conflicts with unnamed case"
                                                : "invalid service unnamed payload");
                    flags[j] = !flags[j];
                }
            }
            if (shape->case_count != 1) {
                shape->is_struct = true;
                shape->case_count = 0;
                write_refused(&changed, "invalid service struct shape");
            }
        }
        *shape = file->services[0].shapes[i];
        offset += 12 + 4 * shape->case_count;
    }
    mo_arena_free(&arena);
}

static void schema_refusals(const struct mo_bytecode *file, const struct mo_buffer *wire) {
    struct mo_bytecode changed = *file;
    struct mo_bytecode_service service = file->services[0];
    struct mo_bytecode_service_tool tool = service.tools[1];
    changed.services = &service;
    service.tools = &tool;
    service.tool_count = 1;
    tool.description = file->string_count;
    write_refused(&changed, "service tool schema differs from handler");
    tool = file->services[0].tools[1];
    for (uint32_t count = 0; count <= 3; count++) {
        uint32_t names[] = {tool.name, tool.name, tool.name};
        tool.argument_names = names;
        tool.argument_name_count = count;
        write_refused(&changed, count == 2 ? "duplicate service tool parameter name"
                                           : "service tool schema differs from handler");
    }
    tool = file->services[0].tools[1];
    uint32_t names[] = {tool.argument_names[0], file->string_count};
    tool.argument_names = names;
    write_refused(&changed, "service tool schema differs from handler");
    names[1] = file->services[0].tools[0].description;
    CHECK(named(file, names[1], ""));
    write_refused(&changed, "service tool schema differs from handler");

    struct mo_arena arena;
    mo_arena_init(&arena, MO_LIMIT_LOWER_WORK);
    struct mo_bytecode_string *strings = mo_arena_allocate_array(
        &arena, file->string_count + 1, sizeof *strings, _Alignof(struct mo_bytecode_string));
    MO_ASSERT(strings != NULL);
    memcpy(strings, file->strings, file->string_count * sizeof *strings);
    strings[file->string_count] = strings[names[0]];
    changed.strings = strings;
    changed.string_count++;
    names[1] = file->string_count;
    write_refused(&changed, "duplicate service tool parameter name");
    strings[file->string_count] = (struct mo_bytecode_string){3, (const uint8_t *)"a\0b"};
    write_refused(&changed, "service tool schema differs from handler");
    tool = file->services[0].tools[0];
    tool.name = file->string_count;
    write_refused(&changed, "invalid service tool name");
    tool.name = tool.description;
    write_refused(&changed, "invalid service tool name");
    tool.name = changed.string_count;
    write_refused(&changed, "invalid service tool name");
    mo_arena_free(&arena);

    struct mo_buffer text;
    struct mo_load_error le;
    mo_buffer_init(&text, MO_LIMIT_BYTECODE_TEXT_SIZE);
    CHECK(mo_bytecode_disassemble(wire->bytes, wire->length, &text, &le));
    CHECK(mo_buffer_append(&text, "\0", 1));
    const char *start = strstr((const char *)text.bytes, " tools [");
    MO_ASSERT(start != NULL);
    start += strlen(" tools [");
    const char *end = strchr(start, ')');
    MO_ASSERT(end != NULL);
    tool = file->services[0].tools[0];
    for (unsigned i = 0; i < 3; i++) {
        char entry[160];
        int n = snprintf(entry, sizeof entry, "(s%u, f%u, 1, [], t%u, s%u, [%s])",
                         i == 2 ? tool.description : tool.name, tool.function, tool.result_type,
                         i == 1 ? file->string_count : tool.description, i == 0 ? "s0" : "");
        MO_ASSERT(n > 0 && (size_t)n < sizeof entry);
        struct mo_buffer input, output;
        mo_buffer_init(&input, MO_LIMIT_BYTECODE_TEXT_SIZE);
        mo_buffer_init(&output, MO_LIMIT_BYTECODE_SIZE);
        CHECK(mo_buffer_append(&input, text.bytes, (size_t)(start - (const char *)text.bytes)));
        CHECK(mo_buffer_append(&input, entry, (size_t)n));
        CHECK(mo_buffer_append(&input, end + 1, strlen(end + 1)));
        struct mo_assemble_error error;
        bool assembled =
            mo_bytecode_assemble((const char *)input.bytes, input.length, &output, &error);
        CHECK(!assembled && output.length == 0);
        if (!assembled)
            CHECK(strcmp(error.reason, i == 2   ? "invalid service tool name"
                                       : i == 1 ? "expected string index in final pool"
                                                : "service tool schema differs from handler") == 0);
        mo_buffer_free(&input);
        mo_buffer_free(&output);
    }
    mo_buffer_free(&text);
}

static void table_refusals(const struct mo_bytecode *file) {
    struct mo_arena arena;
    mo_arena_init(&arena, MO_LIMIT_LOWER_WORK);
    struct mo_bytecode changed = *file;
    struct mo_bytecode_service services[2] = {file->services[0], file->services[0]};
    changed.services = services;
    changed.service_count = 2;
    write_refused(&changed, "duplicate service module");
    changed.service_count = 1;
    services[0].tool_count = 0;
    write_refused(&changed, "invalid service handler count");
    services[0] = file->services[0];
    struct mo_bytecode_service_timer *timers =
        mo_arena_allocate_array(&arena, MO_LIMIT_SERVICE_HANDLERS, sizeof *timers,
                                _Alignof(struct mo_bytecode_service_timer));
    MO_ASSERT(timers != NULL);
    for (uint32_t i = 0; i < MO_LIMIT_SERVICE_HANDLERS; i++)
        timers[i] = services[0].timers[0];
    services[0].timers = timers;
    services[0].timer_count = MO_LIMIT_SERVICE_HANDLERS - services[0].tool_count;
    write_refused(&changed, "duplicate service handler or tool name");
    services[0].timer_count++;
    write_refused(&changed, "invalid service handler count");
    services[0] = file->services[0];
    struct mo_bytecode_function *functions =
        mo_arena_copy(&arena, file->functions, file->function_count * sizeof *functions,
                      _Alignof(struct mo_bytecode_function));
    MO_ASSERT(functions != NULL);
    changed.functions = functions;
    struct mo_bytecode_function *fn = &functions[services[0].tools[0].function];
    struct mo_parameter parameters[2] = {fn->parameters[0], fn->parameters[1]};
    uint32_t *registers = mo_arena_copy(&arena, fn->register_types,
                                        fn->register_count * sizeof *registers, _Alignof(uint32_t));
    MO_ASSERT(registers != NULL);
    parameters[1].type = services[0].state_type;
    registers[1] = services[0].state_type;
    fn->parameters = parameters;
    fn->register_types = registers;
    write_refused(&changed, "invalid service handler parameters");
    changed.functions = file->functions;

    struct mo_bytecode_service *many = mo_arena_allocate_array(
        &arena, MO_LIMIT_SERVICES, sizeof *many, _Alignof(struct mo_bytecode_service));
    struct mo_bytecode_string *strings =
        mo_arena_copy(&arena, file->strings, file->string_count * sizeof *strings,
                      _Alignof(struct mo_bytecode_string));
    uint8_t *token = mo_arena_allocate(&arena, 1 << 20, 1);
    MO_ASSERT(many != NULL && strings != NULL && token != NULL);
    memset(token, 'A', 1 << 20);
    strings[services[0].token_env] = (struct mo_bytecode_string){1 << 20, token};
    for (uint32_t i = 0; i < MO_LIMIT_SERVICES; i++)
        many[i] = file->services[0];
    changed.services = many;
    changed.service_count = MO_LIMIT_SERVICES;
    changed.strings = strings;
    clock_t before = clock();
    write_refused(&changed, "duplicate service module");
    CHECK(before != (clock_t)-1 && (double)(clock() - before) / CLOCKS_PER_SEC < 2.0);
    mo_arena_free(&arena);
}

static void descriptor(void) {
    struct fixture f;
    init(&f,
         "module Main\nstruct Wrapped\nstate: Int\nend\n"
         "initial = 7\n"
         "service\nstate Int = identity(initial)\n"
         "tools snapshots, update, attempt, optional, wrapped, reverse, error_state, result_list\n"
         "every 2.minutes z_tick\nevery 3.hours a_tick\ntoken from env \"_TOKEN2\"\nend\n"
         "fn identity(value: T) : T\nvalue\nend\n"
         "fn snapshots(state: Int, _now: Time) : List(Int)\n[state]\nend\n"
         "## Update the counter.\n##\n# not a description\n##Keep the reply flag.\n"
         "fn update(state: Int, _now: Time, amount: Int, enabled: Bool) : (Int, Bool)\n"
         "(state + amount, enabled)\nend\n"
         "fn attempt(state: Int, _now: Time) : Result((Int, Bool), Text)\nOk((state, true))\nend\n"
         "fn optional(state: Int, _now: Time) : Option(Int)\nSome(state)\nend\n"
         "fn wrapped(state: Int, _now: Time) : Wrapped\nWrapped(state: state)\nend\n"
         "fn reverse(state: Int, _now: Time) : (Bool, Int)\n(true, state)\nend\n"
         "fn error_state(state: Int, _now: Time) : Result(Bool, Int)\nError(state)\nend\n"
         "fn result_list(state: Int, _now: Time) : Result(List(Int), Text)\nOk([state])\nend\n"
         "fn bare_result(state: Int, _now: Time) : Result(Int, Text)\nOk(state)\nend\n"
         "fn impure(_state: Int, _now: Time, _values: List(Console)) : Bool\ntrue\nend\n"
         "fn functional(_state: Int, _now: Time, _values: List(fn(Int) : Int)) : Bool\ntrue\nend\n"
         "fn a_tick(state: Int, _now: Time) : Int\nstate + 3\nend\n"
         "fn z_tick(state: Int, _now: Time) : Int\nstate + 2\nend\n"
         "fn main() : Int\n0\nend\n");
    CHECK(f.program != NULL && !mo_diags_any(&f.diags));
    if (f.program == NULL) {
        for (uint32_t i = 0; i < f.diags.count; i++)
            CHECK(fprintf(stderr, "%s: %s\n", f.diags.items[i].message, f.diags.items[i].reason) >=
                  0);
        free_fixture(&f);
        return;
    }
    const char *modules[] = {"Main"};
    f.set.names = modules;
    struct mo_arena arena;
    mo_arena_init(&arena, MO_LIMIT_LOWER_WORK);
    struct mo_services services;
    struct mo_core *core =
        mo_lower_services(&arena, f.program, &f.sources, &f.diags, NULL, &services);
    CHECK(core != NULL && !mo_diags_any(&f.diags));
    if (core == NULL)
        goto done;
    CHECK(mo_core_check(core, &f.diags));
    free_fixture(&f);
    memset(&f, 0, sizeof f);
    CHECK(mo_diags_init(&f.diags));
    struct mo_buffer wire;
    mo_buffer_init(&wire, MO_LIMIT_BYTECODE_SIZE);
    bool compiled = mo_compile_services(core, &services, &wire, &f.diags);
    CHECK(compiled);
    if (!compiled) {
        for (uint32_t i = 0; i < f.diags.count; i++)
            CHECK(fprintf(stderr, "%s: %s\n", f.diags.items[i].message, f.diags.items[i].reason) >=
                  0);
        mo_buffer_free(&wire);
        goto done;
    }
    seed(&wire, "service.bc");
    text_roundtrip(&wire);
    struct mo_load_error error;
    struct mo_image *image = mo_load(wire.bytes, wire.length, &error);
    CHECK(image != NULL);
    if (image == NULL) {
        mo_buffer_free(&wire);
        goto done;
    }
    const struct mo_bytecode *file = mo_image_bytecode(image);
    table_refusals(file);
    schema_refusals(file, &wire);
    shape_refusals(file, &wire);
    CHECK(file->service_count == 1 && file->header.flags == MO_BYTECODE_SERVICE_FLAG);
    MO_ASSERT(file->service_count == 1);
    const struct mo_bytecode_service *s = &file->services[0];
    CHECK(named(file, file->modules[s->module].name, "Main"));
    CHECK(named(file, s->token_env, "_TOKEN2"));
    CHECK(named(file, file->functions[s->start].name, "Main.%service.start"));
    CHECK(named(file, file->functions[s->timers[0].interval].name, "Main.%service.interval.1"));
    CHECK(named(file, file->functions[s->timers[1].interval].name, "Main.%service.interval.2"));
    CHECK(file->types[s->state_type].kind == MO_TYPE_INT);
    CHECK(s->tool_count == 8 && s->timer_count == 2);
    const char *names[] = {"snapshots", "update",  "attempt",     "optional",
                           "wrapped",   "reverse", "error_state", "result_list"};
    for (uint32_t i = 0; i < s->tool_count; i++) {
        CHECK(named(file, s->tools[i].name, names[i]));
        CHECK(named(file, s->tools[i].description,
                    i == 1 ? "Update the counter.\n\nKeep the reply flag." : ""));
        CHECK(s->tools[i].argument_name_count == s->tools[i].argument_count);
        CHECK(s->tools[i].read_only == (i != 1 && i != 2));
        CHECK(file->functions[s->tools[i].function].parameters[0].type == s->state_type);
    }
    CHECK(s->tools[1].argument_count == 2);
    CHECK(named(file, s->tools[1].argument_names[0], "amount"));
    CHECK(named(file, s->tools[1].argument_names[1], "enabled"));
    CHECK(file->types[s->tools[1].argument_types[0]].kind == MO_TYPE_INT);
    CHECK(file->types[s->tools[1].argument_types[1]].kind == MO_TYPE_BOOL);
    struct mo_limits limits = mo_limits_default();
    struct mo_defect defect;
    CHECK(mo_image_init(image, &limits, &defect) == MO_RETURNED);
    struct mo_process *process = mo_process_new(image, &limits);
    MO_ASSERT(process != NULL);
    struct mo_cell result;
    CHECK(mo_call(process, s->start, NULL, 0, &result, &defect) == MO_RETURNED);
    CHECK(mo_cell_get_int(result) == 7);
    const int64_t durations[] = {INT64_C(120000000), INT64_C(10800000000)};
    for (uint32_t i = 0; i < 2; i++) {
        CHECK(mo_call(process, s->timers[i].interval, NULL, 0, &result, &defect) == MO_RETURNED);
        CHECK(mo_cell_get_duration(result) == durations[i]);
        struct mo_cell args[] = {mo_cell_int(7), mo_cell_time(0)};
        CHECK(mo_call(process, s->timers[i].function, args, 2, &result, &defect) == MO_RETURNED);
        CHECK(mo_cell_get_int(result) == (i == 0 ? 9 : 10));
    }
    mo_process_free(process);
    size_t shape_size = 4;
    for (uint32_t i = 0; i < s->shape_count; i++)
        shape_size += 12 + 4 * s->shapes[i].case_count;
    size_t size = 28 + 8 * s->timer_count + shape_size;
    for (uint32_t i = 0; i < s->tool_count; i++)
        size += 28 + 8 * s->tools[i].argument_count;
    size_t start = wire.length - size;
    refused(&wire, 8, 2, "unsupported version or flags");
    refused(&wire, start, 0, "empty service extension");
    refused(&wire, start, MO_LIMIT_SERVICES + 1, "count past limit or remaining bytes");
    refused(&wire, start + 4, file->module_count,
            "invalid service module, start function or state type");
    refused(&wire, start + 12, UINT32_MAX, "invalid service module, start function or state type");
    refused(&wire, start + 8, file->functions[s->tools[0].function].result_type,
            "invalid service module, start function or state type");
    refused(&wire, start + 16, s->tools[0].name, "invalid service token environment name");
    refused(&wire, start + 20, MO_LIMIT_SERVICE_HANDLERS + 1,
            "count past limit or remaining bytes");
    refused(&wire, start + 24 + 4, s->start, "invalid service handler parameters");
    refused(&wire, start + 24 + 8, 0, "invalid service tool result classification");
    refused(&wire, start + 24 + 8, 2, "invalid boolean");
    refused(&wire, start + 24 + 16, s->state_type, "service tool schema differs from handler");
    refused(&wire, start + 24, s->tools[0].description, "invalid service tool name");
    refused(&wire, start + 44, file->string_count, "service tool schema differs from handler");
    refused(&wire, start + 52, s->tools[0].name, "duplicate service handler or tool name");
    refused(&wire, start + 52 + 16, file->functions[s->tools[0].function].result_type,
            "service tool schema differs from handler");
    refused(&wire, wire.length - shape_size - 4, s->start,
            "invalid service timer handler or interval function");
    refused(&wire, wire.length - shape_size - 8, s->timers[0].function,
            "duplicate service handler or tool name");
    uint32_t invalid_functions[] = {s->timers[0].function, UINT32_MAX, UINT32_MAX, UINT32_MAX};
    const char *invalid_names[] = {"Main.bare_result", "Main.impure", "Main.functional"};
    for (uint32_t i = 0; i < file->function_count; i++)
        for (uint32_t j = 0; j < 3; j++)
            if (named(file, file->functions[i].name, invalid_names[j]))
                invalid_functions[j + 1] = i;
    for (uint32_t i = 0; i < 4; i++) {
        CHECK(invalid_functions[i] != UINT32_MAX);
        if (invalid_functions[i] == UINT32_MAX)
            continue;
        struct mo_bytecode changed = *file;
        struct mo_bytecode_service service = *s;
        struct mo_bytecode_service_tool tool = s->tools[0];
        service.tool_count = 1;
        service.tools = &tool;
        changed.services = &service;
        tool.function = invalid_functions[i];
        const struct mo_bytecode_function *fn = &file->functions[tool.function];
        tool.result_type = fn->result_type;
        tool.argument_count = fn->parameter_count - 2;
        uint32_t argument = tool.argument_count == 0 ? 0 : fn->parameters[2].type;
        tool.argument_types = &argument;
        struct mo_buffer output;
        struct mo_write_error write_error;
        mo_buffer_init(&output, MO_LIMIT_BYTECODE_SIZE);
        CHECK(!mo_bytecode_write(&changed, &output, &write_error));
        CHECK(output.length == 0);
        CHECK(strcmp(write_error.reason,
                     i < 2 ? "invalid service tool result classification"
                           : "service type contains a capability or function") == 0);
        mo_buffer_free(&output);
    }
    struct mo_buffer copied;
    struct mo_write_error write_error;
    mo_buffer_init(&copied, MO_LIMIT_BYTECODE_SIZE);
    memset(wire.bytes, 0xa5, wire.length);
    CHECK(mo_bytecode_write(file, &copied, &write_error));
    text_roundtrip(&copied);
    mo_buffer_free(&copied);
    mo_image_free(image);
    mo_buffer_free(&wire);
done:
    mo_arena_free(&arena);
    free_fixture(&f);
}

static void selected_overload(void) {
    struct fixture f;
    init(&f, "module Main\n"
             "struct Unselected\nvalue: Int\nend\nstruct Selected\nvalue: Int\nend\n"
             "service\nstate Selected = Selected(value: 7)\ntools query\n"
             "token from env \"TOKEN\"\nend\n"
             "## Wrong overload.\n"
             "fn query(_state: Unselected, _now: Time, wrong: Text) : Text\nwrong\nend\n"
             "## Selected overload.\n"
             "fn query(_state: Selected, _now: Time, wanted: Text, _ignored: Int) : Text\n"
             "wanted\nend\n");
    MO_ASSERT(f.program != NULL && !mo_diags_any(&f.diags));
    const char *modules[] = {"Main"};
    f.set.names = modules;
    struct mo_arena arena;
    mo_arena_init(&arena, MO_LIMIT_LOWER_WORK);
    struct mo_services services;
    struct mo_core *core =
        mo_lower_services(&arena, f.program, &f.sources, &f.diags, NULL, &services);
    MO_ASSERT(core != NULL && !mo_diags_any(&f.diags));
    free_fixture(&f);
    struct mo_diags diags;
    CHECK(mo_diags_init(&diags));
    struct mo_buffer wire;
    mo_buffer_init(&wire, MO_LIMIT_BYTECODE_SIZE);
    MO_ASSERT(mo_compile_services(core, &services, &wire, &diags));
    struct mo_load_error error;
    struct mo_image *image = mo_load(wire.bytes, wire.length, &error);
    MO_ASSERT(image != NULL);
    const struct mo_bytecode *file = mo_image_bytecode(image);
    MO_ASSERT(file->service_count == 1 && file->services[0].tool_count == 1);
    const struct mo_bytecode_service_tool *tool = &file->services[0].tools[0];
    CHECK(named(file, tool->name, "query"));
    CHECK(named(file, tool->description, "Selected overload."));
    MO_ASSERT(tool->argument_name_count == 2);
    CHECK(named(file, tool->argument_names[0], "wanted"));
    CHECK(named(file, tool->argument_names[1], "_ignored"));
    text_roundtrip(&wire);
    mo_image_free(image);
    mo_buffer_free(&wire);
    mo_diags_free(&diags);
    mo_arena_free(&arena);
}

static void source_shapes(void) {
    struct fixture f;
    init(&f, "module Main\n"
             "struct Unused\nvalue: Int\nend\n"
             "struct Record\nname: Text\nkind: EntryKind\nend\n"
             "enum One\nOnly(name: Text, kind: EntryKind)\nend\n"
             "enum Payload\nEmpty\nUnnamed(Int)\nNamed(value: Int)\nend\n"
             "service\nstate Int = 0\ntools echo\ntoken from env \"TOKEN\"\nend\n"
             "fn echo(_state: Int, _now: Time, record: Record, one: One, payload: Payload, "
             "entry: Entry, entries: List(Entry)) : (Record, One, Payload, Entry, List(Entry))\n"
             "(record, one, payload, entry, entries)\nend\n");
    MO_ASSERT(f.program != NULL && !mo_diags_any(&f.diags));
    const char *modules[] = {"Main"};
    f.set.names = modules;
    struct mo_arena arena;
    mo_arena_init(&arena, MO_LIMIT_LOWER_WORK);
    struct mo_services services;
    struct mo_core *core =
        mo_lower_services(&arena, f.program, &f.sources, &f.diags, NULL, &services);
    MO_ASSERT(core != NULL && !mo_diags_any(&f.diags));
    free_fixture(&f);
    const char *names[] = {"Main.Record", "Main.One", "Main.Payload"};
    uint32_t found = 0;
    CHECK(services.data_count == core->data.count);
    for (uint32_t i = 0; i < services.data_count; i++) {
        const struct mo_service_shape *shape = &services.data_shapes[i];
        mo_core_string name = core->data.items[i].name;
        for (uint32_t j = 0; j < sizeof names / sizeof *names; j++) {
            if (name.length != strlen(names[j]) || memcmp(name.bytes, names[j], name.length) != 0)
                continue;
            found++;
            CHECK(shape->is_struct == (j == 0));
            CHECK(shape->case_count == (j == 0 ? 0U : j == 1 ? 1U : 3U));
            if (shape->is_struct)
                CHECK(shape->unnamed == NULL);
            else
                for (uint32_t k = 0; k < shape->case_count; k++)
                    CHECK(shape->unnamed[k] == (j == 2 && k == 1));
        }
    }
    CHECK(found == 3);
    struct mo_diags diags;
    CHECK(mo_diags_init(&diags));
    struct mo_buffer wire;
    mo_buffer_init(&wire, MO_LIMIT_BYTECODE_SIZE);
    CHECK(mo_compile_services(core, &services, &wire, &diags));
    struct mo_load_error error;
    struct mo_image *image = mo_load(wire.bytes, wire.length, &error);
    MO_ASSERT(image != NULL);
    const struct mo_bytecode *file = mo_image_bytecode(image);
    const struct mo_bytecode_service *service = &file->services[0];
    CHECK(service->shape_count == 5);
    const char *shape_names[] = {"Main.Record", "Main.One", "Main.Payload", "Entry", "EntryKind"};
    found = 0;
    for (uint32_t i = 0; i < service->shape_count; i++) {
        const struct mo_bytecode_service_shape *shape = &service->shapes[i];
        CHECK(i == 0 || service->shapes[i - 1].type < shape->type);
        for (uint32_t j = 0; j < sizeof shape_names / sizeof *shape_names; j++) {
            if (strcmp(file->types[shape->type].name, shape_names[j]) != 0)
                continue;
            found++;
            CHECK(shape->is_struct == (j == 0 || j == 3));
            CHECK(shape->case_count == (j == 0 || j == 3 ? 0U : j == 1 ? 1U : j == 2 ? 3U : 4U));
            for (uint32_t k = 0; k < shape->case_count; k++)
                CHECK(shape->unnamed[k] == (j == 2 && k == 1));
        }
    }
    CHECK(found == 5);
    shape_refusals(file, &wire);
    seed(&wire, "service-shapes.bc");
    mo_image_free(image);
    text_roundtrip(&wire);
    mo_buffer_free(&wire);
    mo_diags_free(&diags);
    mo_arena_free(&arena);
}

static void shorthand_shapes(void) {
    const char source[] =
        "bytecode 0.3 shorthand\n"
        "strings\n s0 \"M\"\n s1 \"TOKEN\"\n s2 \"echo\"\n s3 \"entry\"\n s4 \"\"\n"
        " s5 \"Item\"\n s6 \"name\"\n s7 \"kind\"\n s8 \"Kind\"\n s9 \"File\"\n"
        " s10 \"Next\"\nend\n"
        "types\n t0 record s5 [(s6, t2), (s7, t1)]\n t1 choice s8 [(s9, []), (s10, [(-, t0)])]\n"
        " t2 text\n t3 int\n t4 time\nend\n"
        "module M exposes [M.echo]\n"
        "function M.start() : Int function in M\n registers [r0: Int]\n"
        " const r0, 0\n ret r0\nend\n"
        "function M.echo(state: Int owned, now: Time owned, entry: Item owned) : Item function in "
        "M\n"
        " registers []\n ret r2\nend\n"
        "services\n S0 m0 t3 f0 s1 tools [(s2, f1, 1, [t0], t0, s4, [s3])] timers []"
        " shapes [(t0, struct, []), (t1, enum, [0, 1])]\nend\n";
    struct mo_buffer wire;
    struct mo_assemble_error error;
    mo_buffer_init(&wire, MO_LIMIT_BYTECODE_SIZE);
    bool assembled = mo_bytecode_assemble(source, sizeof source - 1, &wire, &error);
    CHECK(assembled);
    if (assembled) {
        struct mo_load_error load_error;
        struct mo_image *image = mo_load(wire.bytes, wire.length, &load_error);
        MO_ASSERT(image != NULL);
        const struct mo_bytecode *file = mo_image_bytecode(image);
        const struct mo_bytecode_service *service = &file->services[0];
        CHECK(service->shape_count == 2 && service->shapes[0].type == 0 &&
              service->shapes[0].is_struct && service->shapes[0].case_count == 0 &&
              service->shapes[1].type == 1 && !service->shapes[1].is_struct &&
              service->shapes[1].case_count == 2 && !service->shapes[1].unnamed[0] &&
              service->shapes[1].unnamed[1]);
        size_t types = 24;
        for (uint32_t i = 0; i < file->string_count; i++)
            types += 4 + file->strings[i].length;
        refused(&wire, types + 16, UINT32_MAX, "name string index out of range");
        text_roundtrip(&wire);
        seed(&wire, "service-shorthand-shapes.bc");
        mo_image_free(image);
    } else
        CHECK(fprintf(stderr, "shorthand refusal at %u: %s\n", error.offset, error.reason) >= 0);
    mo_buffer_free(&wire);
}

static void run(void *context) {
    (void)context;
    descriptor();
    selected_overload();
    source_shapes();
    shorthand_shapes();
}

int main(int argc, char **argv) {
    MO_ASSERT(argc == 1 || (argc == 3 && strcmp(argv[1], "--corpus") == 0));
    if (argc == 3)
        corpus = argv[2];
    CHECK(mo_with_stack(MO_SOURCE_STACK_SIZE, run, NULL) == 0);
    return mo_test_report("service descriptors");
}
