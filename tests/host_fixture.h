#ifndef MO_HOST_FIXTURE_H
#define MO_HOST_FIXTURE_H

#include "heap_fixture.h"
#include "host/host.h"

#include <stdlib.h>
#include <string.h>

struct host_fixture {
    struct mo_arena arena;
    struct mo_types source;
    struct mo_host_types types;
    struct mo_type *storage;
    struct mo_heap_image *image;
    struct mo_heap *heap;
    struct mo_host host;
    uint32_t unit, boolean, bytes, time;
    uint32_t capabilities[MO_CAP_COMMAND + 1];
    uint32_t option, file_error, result_text, result_bytes, result_unit, result_names,
        result_entries;
    uint32_t integer, network_error, result_connection, result_sent, result_received;
    uint32_t sqlite_value, sqlite_rows, sqlite_error, result_database, result_sqlite_unit,
        result_sqlite_rows;
    uint32_t command_tuple, result_command;
    uint32_t entry, entries;
};

static inline void host_fixture_init(struct host_fixture *f, size_t heap_limit) {
    *f = (struct host_fixture){0};
    mo_arena_init(&f->arena, MO_LIMIT_BYTECODE_SIZE);
    mo_types_init(&f->source, &f->arena);
    uint32_t text = heap_intern(&f->source, (struct mo_type){.kind = MO_TYPE_TEXT});
    f->unit = heap_intern(&f->source, (struct mo_type){.kind = MO_TYPE_UNIT});
    f->boolean = heap_intern(&f->source, (struct mo_type){.kind = MO_TYPE_BOOL});
    f->bytes = heap_intern(&f->source, (struct mo_type){.kind = MO_TYPE_BYTES});
    f->time = heap_intern(&f->source, (struct mo_type){.kind = MO_TYPE_TIME});
    uint32_t names = heap_intern(
        &f->source,
        (struct mo_type){.kind = MO_TYPE_LIST, .parameters = &text, .parameter_count = 1});
    uint32_t real = heap_intern(&f->source, (struct mo_type){.kind = MO_TYPE_FLOAT});
    f->integer = heap_intern(&f->source, (struct mo_type){.kind = MO_TYPE_INT});
    for (unsigned i = 0; i <= MO_CAP_COMMAND; i++) {
        if (i != MO_CAP_SIMULATION)
            f->capabilities[i] =
                heap_intern(&f->source, (struct mo_type){.kind = MO_TYPE_CAPABILITY,
                                                         .capability = (enum mo_capability)i});
    }
    MO_ASSERT(mo_types_declare(&f->source, MO_TYPE_CHOICE, "Option(Text)", &f->option) ==
              MO_TYPES_OK);
    struct mo_type_field one = {NULL, text};
    struct mo_type_case options[] = {
        {.name = "None"}, {.name = "Some", .fields = &one, .field_count = 1, .unnamed = true}};
    MO_ASSERT(mo_types_define_choice(&f->source, f->option, options, 2) == MO_TYPES_OK);
    MO_ASSERT(mo_types_declare(&f->source, MO_TYPE_CHOICE, "FileError", &f->file_error) ==
              MO_TYPES_OK);
    struct mo_type_field fields[] = {{"path", text}, {"reason", text}};
    struct mo_type_case errors[] = {{.name = "NotFound", .fields = fields, .field_count = 1},
                                    {.name = "NotAllowed", .fields = fields, .field_count = 1},
                                    {.name = "NotText", .fields = fields, .field_count = 1},
                                    {.name = "Failed", .fields = fields, .field_count = 2}};
    MO_ASSERT(mo_types_define_choice(&f->source, f->file_error, errors, 4) == MO_TYPES_OK);
    MO_ASSERT(mo_types_declare(&f->source, MO_TYPE_CHOICE, "NetworkError", &f->network_error) ==
              MO_TYPES_OK);
    struct mo_type_field refused[] = {{"host", text}, {"port", f->integer}};
    struct mo_type_field broken = {"reason", text};
    struct mo_type_case network_errors[] = {
        {.name = "UnknownHost", .fields = refused, .field_count = 1},
        {.name = "Refused", .fields = refused, .field_count = 2},
        {.name = "TimedOut"},
        {.name = "Closed"},
        {.name = "Broken", .fields = &broken, .field_count = 1},
        {.name = "Certificate", .fields = &broken, .field_count = 1}};
    MO_ASSERT(mo_types_define_choice(&f->source, f->network_error, network_errors, 6) ==
              MO_TYPES_OK);
    uint32_t kind;
    MO_ASSERT(mo_types_declare(&f->source, MO_TYPE_CHOICE, "EntryKind", &kind) == MO_TYPES_OK);
    struct mo_type_case kinds[] = {
        {.name = "File"}, {.name = "Folder"}, {.name = "Link"}, {.name = "Other"}};
    MO_ASSERT(mo_types_define_choice(&f->source, kind, kinds, 4) == MO_TYPES_OK);
    MO_ASSERT(mo_types_declare(&f->source, MO_TYPE_RECORD, "Entry", &f->entry) == MO_TYPES_OK);
    struct mo_type_field entry_fields[] = {{"name", text}, {"kind", kind}};
    MO_ASSERT(mo_types_define_record(&f->source, f->entry, entry_fields, 2) == MO_TYPES_OK);
    f->entries = heap_intern(
        &f->source,
        (struct mo_type){.kind = MO_TYPE_LIST, .parameters = &f->entry, .parameter_count = 1});
    MO_ASSERT(mo_types_declare(&f->source, MO_TYPE_CHOICE, "SqliteError", &f->sqlite_error) ==
              MO_TYPES_OK);
    struct mo_type_field sql_path = {"path", text}, sql_reason = {"reason", text},
                         sql_message = {"message", text};
    struct mo_type_case sqlite_errors[] = {
        {.name = "OutsideFolder", .fields = &sql_path, .field_count = 1},
        {.name = "NotPermitted", .fields = &sql_reason, .field_count = 1},
        {.name = "SqlFailed", .fields = &sql_message, .field_count = 1}};
    MO_ASSERT(mo_types_define_choice(&f->source, f->sqlite_error, sqlite_errors, 3) == MO_TYPES_OK);
    MO_ASSERT(mo_types_declare(&f->source, MO_TYPE_CHOICE, "Sqlite.Value", &f->sqlite_value) ==
              MO_TYPES_OK);
    struct mo_type_field val_int = {NULL, f->integer}, val_real = {NULL, real},
                         val_text = {NULL, text}, val_blob = {NULL, f->bytes};
    struct mo_type_case sqlite_values[] = {
        {.name = "Null"},
        {.name = "Integer", .fields = &val_int, .field_count = 1, .unnamed = true},
        {.name = "Real", .fields = &val_real, .field_count = 1, .unnamed = true},
        {.name = "String", .fields = &val_text, .field_count = 1, .unnamed = true},
        {.name = "Blob", .fields = &val_blob, .field_count = 1, .unnamed = true}};
    MO_ASSERT(mo_types_define_choice(&f->source, f->sqlite_value, sqlite_values, 5) == MO_TYPES_OK);
    uint32_t value_list = heap_intern(&f->source, (struct mo_type){.kind = MO_TYPE_LIST,
                                                                   .parameters = &f->sqlite_value,
                                                                   .parameter_count = 1});
    uint32_t rows_matrix = heap_intern(
        &f->source,
        (struct mo_type){.kind = MO_TYPE_LIST, .parameters = &value_list, .parameter_count = 1});
    MO_ASSERT(mo_types_declare(&f->source, MO_TYPE_RECORD, "Sqlite.Rows", &f->sqlite_rows) ==
              MO_TYPES_OK);
    struct mo_type_field rows_fields[] = {{"columns", names}, {"rows", rows_matrix}};
    MO_ASSERT(mo_types_define_record(&f->source, f->sqlite_rows, rows_fields, 2) == MO_TYPES_OK);
    uint32_t cmd_parts[3] = {f->integer, f->bytes, f->bytes};
    f->command_tuple = heap_intern(
        &f->source,
        (struct mo_type){.kind = MO_TYPE_TUPLE, .parameters = cmd_parts, .parameter_count = 3});
    MO_ASSERT(mo_types_declare(&f->source, MO_TYPE_CHOICE, "Result", &f->result_command) ==
              MO_TYPES_OK);
    struct mo_type_field cmd_ok = {NULL, f->command_tuple}, cmd_err = {NULL, text};
    struct mo_type_case cmd_cases[] = {
        {.name = "Ok", .fields = &cmd_ok, .field_count = 1, .unnamed = true},
        {.name = "Error", .fields = &cmd_err, .field_count = 1, .unnamed = true}};
    MO_ASSERT(mo_types_define_choice(&f->source, f->result_command, cmd_cases, 2) == MO_TYPES_OK);
    uint32_t payloads[] = {text,    f->bytes,      f->unit,
                           names,   f->entries,    f->capabilities[MO_CAP_CONNECTION],
                           f->unit, f->bytes,      f->capabilities[MO_CAP_DATABASE],
                           f->unit, f->sqlite_rows};
    uint32_t *results[] = {&f->result_text,        &f->result_bytes,      &f->result_unit,
                           &f->result_names,       &f->result_entries,    &f->result_connection,
                           &f->result_sent,        &f->result_received,   &f->result_database,
                           &f->result_sqlite_unit, &f->result_sqlite_rows};
    for (unsigned i = 0; i < 11; i++) {
        MO_ASSERT(mo_types_declare(&f->source, MO_TYPE_CHOICE, "Result", results[i]) ==
                  MO_TYPES_OK);
        uint32_t err_type = i < 5 ? f->file_error : (i < 8 ? f->network_error : f->sqlite_error);
        struct mo_type_field ok = {NULL, payloads[i]}, error = {NULL, err_type};
        struct mo_type_case cases[] = {
            {.name = "Ok", .fields = &ok, .field_count = 1, .unnamed = true},
            {.name = "Error", .fields = &error, .field_count = 1, .unnamed = true}};
        MO_ASSERT(mo_types_define_choice(&f->source, *results[i], cases, 2) == MO_TYPES_OK);
    }
    MO_ASSERT(mo_types_finish(&f->source) == MO_TYPES_OK);
    for (uint32_t i = 0; i < f->source.count; i++) {
        const struct mo_type *t = &f->source.items[i];
        if (t->kind == MO_TYPE_RECORD) {
            uint32_t rec_types[2];
            MO_ASSERT(t->field_count <= 2);
            for (uint32_t k = 0; k < t->field_count; k++)
                rec_types[k] = t->fields[k].type;
            uint32_t layout =
                heap_layout(&f->source, (struct mo_layout){.kind = MO_LAYOUT_RECORD,
                                                           .type = i,
                                                           .case_index = MO_NO_INDEX,
                                                           .function = MO_NO_INDEX,
                                                           .fields = rec_types,
                                                           .field_count = t->field_count});
            MO_ASSERT(layout != MO_NO_INDEX);
        }
        if (t->kind == MO_TYPE_TUPLE) {
            uint32_t layout =
                heap_layout(&f->source, (struct mo_layout){.kind = MO_LAYOUT_TUPLE,
                                                           .type = i,
                                                           .case_index = MO_NO_INDEX,
                                                           .function = MO_NO_INDEX,
                                                           .fields = t->parameters,
                                                           .field_count = t->parameter_count});
            MO_ASSERT(layout != MO_NO_INDEX);
        }
        if (t->kind != MO_TYPE_CHOICE)
            continue;
        for (uint32_t j = 0; j < t->case_count; j++) {
            if (mo_case_shape(&f->source, i, j) != MO_CASE_BOXED)
                continue;
            uint32_t types[2];
            for (uint32_t k = 0; k < t->cases[j].field_count; k++)
                types[k] = t->cases[j].fields[k].type;
            uint32_t layout =
                heap_layout(&f->source, (struct mo_layout){.kind = MO_LAYOUT_CASE,
                                                           .type = i,
                                                           .case_index = j,
                                                           .function = MO_NO_INDEX,
                                                           .fields = types,
                                                           .field_count = t->cases[j].field_count});
            MO_ASSERT(layout != MO_NO_INDEX);
        }
    }
    f->storage = calloc(f->source.count + MO_HOST_PRIVATE_TYPES, sizeof *f->storage);
    MO_ASSERT(f->storage != NULL);
    mo_host_prepare_types(&f->types, &f->source, f->storage);
    struct mo_limits limits = mo_limits_default();
    f->image = mo_heap_image_new(&f->types.types, &limits);
    MO_ASSERT(f->image != NULL);
    limits.heap_size = heap_limit;
    f->heap = mo_heap_new(f->image, &limits);
    MO_ASSERT(f->heap != NULL);
    mo_host_init(&f->host, &f->types);
}

static inline void host_fixture_free(struct host_fixture *f) {
    mo_heap_free(f->heap);
    MO_ASSERT(f->host.open_folders == 0 && f->host.open_connections == 0 &&
              f->host.open_databases == 0);
    mo_heap_image_free(f->image);
    free(f->storage);
    mo_arena_free(&f->arena);
}

static inline struct mo_cell host_text(struct host_fixture *f, const char *s) {
    struct mo_cell cell;
    MO_ASSERT(mo_value_text(f->heap, s, strlen(s), &cell));
    return cell;
}

static inline uint32_t host_result_type(const struct host_fixture *f, uint32_t operation) {
    switch (operation) {
    case MO_HOST_ARGS_LIST:
    case MO_HOST_CONSOLE_PRINTED:
    case MO_HOST_CONSOLE_PRINTED_ERRORS:
        return f->types.texts;
    case MO_HOST_ARGS_OF:
        return f->capabilities[MO_CAP_ARGS];
    case MO_HOST_ENV_OF:
        return f->capabilities[MO_CAP_ENV];
    case MO_HOST_ENV_GET:
    case MO_HOST_CONSOLE_READ_LINE:
        return f->option;
    case MO_HOST_CONSOLE_FAKE:
    case MO_HOST_CONSOLE_FAKE_INPUT:
        return f->capabilities[MO_CAP_CONSOLE];
    case MO_HOST_CLOCK_FIXED:
        return f->capabilities[MO_CAP_CLOCK];
    case MO_HOST_CLOCK_NOW:
        return f->time;
    case MO_HOST_RANDOM_SEEDED:
        return f->capabilities[MO_CAP_RANDOM];
    case MO_HOST_RANDOM_INT:
        return f->types.integer;
    case MO_HOST_FILES_IN_MEMORY:
    case MO_HOST_FILES_WITHIN:
        return f->capabilities[MO_CAP_FILES];
    case MO_HOST_FILES_READ_ONLY:
    case MO_HOST_READFILES_WITHIN:
        return f->capabilities[MO_CAP_READ_FILES];
    case MO_HOST_FILES_READ_TEXT:
    case MO_HOST_READFILES_READ_TEXT:
        return f->result_text;
    case MO_HOST_FILES_READ_BYTES:
    case MO_HOST_READFILES_READ_BYTES:
        return f->result_bytes;
    case MO_HOST_FILES_LIST:
    case MO_HOST_READFILES_LIST:
        return f->result_names;
    case MO_HOST_FILES_ENTRIES:
    case MO_HOST_READFILES_ENTRIES:
        return f->result_entries;
    case MO_HOST_FILES_EXISTS:
    case MO_HOST_READFILES_EXISTS:
        return f->boolean;
    case MO_HOST_CONSOLE_PRINT:
    case MO_HOST_CONSOLE_ERROR:
        return f->unit;
    case MO_HOST_FILES_WRITE_TEXT:
    case MO_HOST_FILES_APPEND_TEXT:
    case MO_HOST_FILES_REMOVE:
    case MO_HOST_FILES_MAKE_FOLDER:
        return f->result_unit;
    case MO_HOST_NETWORK_ALLOW_HOSTS:
    case MO_HOST_NETWORK_FAKE:
        return f->capabilities[MO_CAP_NETWORK];
    case MO_HOST_NETWORK_CONNECT:
    case MO_HOST_NETWORK_CONNECT_SECURE:
        return f->result_connection;
    case MO_HOST_CONNECTION_SEND:
        return f->result_sent;
    case MO_HOST_CONNECTION_RECEIVE:
        return f->result_received;
    case MO_HOST_SQLITE_FAKE:
        return f->capabilities[MO_CAP_SQLITE];
    case MO_HOST_SQLITE_OPEN:
        return f->result_database;
    case MO_HOST_SQLITE_RUN:
        return f->result_sqlite_unit;
    case MO_HOST_SQLITE_QUERY:
        return f->result_sqlite_rows;
    case MO_HOST_COMMAND_FAKE:
    case MO_HOST_COMMAND_WITHIN:
    case MO_HOST_COMMAND_ALLOW_PROGRAMS:
        return f->capabilities[MO_CAP_COMMAND];
    case MO_HOST_COMMAND_RUN_RAW:
        return f->result_command;
    default:
        MO_UNREACHABLE();
    }
}

#endif
