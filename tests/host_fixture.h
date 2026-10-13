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
    uint32_t capabilities[MO_CAP_RANDOM + 1];
    uint32_t option, file_error, result_text, result_bytes, result_unit, result_names,
        result_entries;
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
    for (unsigned i = 0; i <= MO_CAP_RANDOM; i++)
        f->capabilities[i] =
            heap_intern(&f->source, (struct mo_type){.kind = MO_TYPE_CAPABILITY,
                                                     .capability = (enum mo_capability)i});
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
    uint32_t payloads[] = {text, f->bytes, f->unit, names, f->entries};
    uint32_t *results[] = {&f->result_text, &f->result_bytes, &f->result_unit, &f->result_names,
                           &f->result_entries};
    for (unsigned i = 0; i < 5; i++) {
        MO_ASSERT(mo_types_declare(&f->source, MO_TYPE_CHOICE, "Result", results[i]) ==
                  MO_TYPES_OK);
        struct mo_type_field ok = {NULL, payloads[i]}, error = {NULL, f->file_error};
        struct mo_type_case cases[] = {
            {.name = "Ok", .fields = &ok, .field_count = 1, .unnamed = true},
            {.name = "Error", .fields = &error, .field_count = 1, .unnamed = true}};
        MO_ASSERT(mo_types_define_choice(&f->source, *results[i], cases, 2) == MO_TYPES_OK);
    }
    MO_ASSERT(mo_types_finish(&f->source) == MO_TYPES_OK);
    for (uint32_t i = 0; i < f->source.count; i++) {
        const struct mo_type *t = &f->source.items[i];
        if (t->kind == MO_TYPE_RECORD) {
            uint32_t types[] = {text, kind};
            uint32_t layout = heap_layout(&f->source, (struct mo_layout){.kind = MO_LAYOUT_RECORD,
                                                                         .type = i,
                                                                         .case_index = MO_NO_INDEX,
                                                                         .function = MO_NO_INDEX,
                                                                         .fields = types,
                                                                         .field_count = 2});
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
    MO_ASSERT(f->host.open_folders == 0);
    mo_heap_image_free(f->image);
    free(f->storage);
    mo_arena_free(&f->arena);
}

static inline struct mo_cell host_text(struct host_fixture *f, const char *s) {
    struct mo_cell cell;
    MO_ASSERT(mo_value_text(f->heap, s, strlen(s), &cell));
    return cell;
}

#endif
