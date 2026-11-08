#include "base/binary.h"
#include "base/compile-metadata.h"
#include "base/sha256.h"
#include "tools/upgrade.h"
#include "vm/vm.h"
#include <stdlib.h>
#include <string.h>

struct named {
    struct mo_bytecode_string name;
    uint32_t index;
};
static int order(const void *left, const void *right) {
    const struct named *a = left, *b = right;
    size_t n = a->name.length < b->name.length ? a->name.length : b->name.length;
    int compared = memcmp(a->name.bytes, b->name.bytes, n);
    return compared != 0 ? compared
                         : (a->name.length > b->name.length) - (a->name.length < b->name.length);
}
static bool span(struct mo_buffer *out, const void *bytes, size_t n) {
    return mo_binary_write(out, n, 4) && mo_buffer_append(out, bytes, n);
}
static void hash_number(struct mo_sha256 *hash, uint32_t n) {
    uint8_t bytes[4];
    for (unsigned i = 0; i < 4; i++)
        bytes[i] = (uint8_t)(n >> (8 * i));
    mo_sha256_update(hash, bytes, sizeof bytes);
}
static void hash_string(struct mo_sha256 *hash, struct mo_bytecode_string text) {
    hash_number(hash, text.length);
    mo_sha256_update(hash, text.bytes, text.length);
}
static bool shapes(const struct mo_types *types, uint32_t index, const uint8_t *flags, bool *seen,
                   struct mo_sha256 *hash, unsigned depth) {
    if (depth == MO_LIMIT_SAVED_DEPTH)
        return false;
    if (seen[index])
        return true;
    seen[index] = true;
    mo_sha256_update(hash, &flags[index], 1);
    const struct mo_type *type = &types->items[index];
    for (uint32_t i = 0; i < type->parameter_count; i++)
        if (!shapes(types, type->parameters[i], flags, seen, hash, depth + 1))
            return false;
    for (uint32_t i = 0; i < type->field_count; i++)
        if (!shapes(types, type->fields[i].type, flags, seen, hash, depth + 1))
            return false;
    for (uint32_t c = 0; c < type->case_count; c++)
        for (uint32_t i = 0; i < type->cases[c].field_count; i++)
            if (!shapes(types, type->cases[c].fields[i].type, flags, seen, hash, depth + 1))
                return false;
    return true;
}
static bool hash_type(struct mo_sha256 *hash, struct mo_heap *heap, const struct mo_types *types,
                      uint32_t type, const uint8_t *flags, bool *seen) {
    uint8_t digest[32];
    if (!mo_type_schema_digest(heap, type, digest))
        return false;
    mo_sha256_update(hash, digest, sizeof digest);
    memset(seen, 0, types->count * sizeof *seen);
    return shapes(types, type, flags, seen, hash, 0);
}
static struct mo_bytecode_string timer_name(const struct mo_bytecode *file, uint32_t function) {
    struct mo_bytecode_string full = file->strings[file->functions[function].name];
    uint32_t at = 0, start = 0;
    while (at < full.length && full.bytes[at] != '@') {
        if (full.bytes[at] == '.')
            start = at + 1;
        at++;
    }
    return (struct mo_bytecode_string){at - start, full.bytes + start};
}
enum mo_upgrade_metadata_status mo_upgrade_metadata(const struct mo_program *program,
                                                    const struct mo_buffer *wire,
                                                    struct mo_buffer *out, FILE *errors) {
    struct mo_load_error error;
    struct mo_image *image = mo_load(wire->bytes, wire->length, &error);
    if (image == NULL)
        return MO_UPGRADE_METADATA_ERROR;
    struct mo_limits limits = mo_service_limits();
    limits.heap_size = MO_LIMIT_UPGRADE_RESTART_HEAP;
    struct mo_defect defect;
    bool ok = mo_image_init_bounded(image, &limits, MO_LIMIT_UPGRADE_RESTART_WORK, &defect) ==
              MO_RETURNED;
    enum mo_upgrade_metadata_status status = MO_UPGRADE_METADATA_ERROR;
    if (!ok) {
        status = MO_UPGRADE_METADATA_INITIALIZER;
        if (!mo_write_defect(errors, NULL, &defect))
            (void)fputs("mo: could not write the initialization defect\n", errors);
    }
    const struct mo_bytecode *file = mo_image_bytecode(image);
    const struct mo_bytecode_service *service = NULL;
    for (uint32_t i = 0; i < file->service_count; i++)
        if (file->services[i].module == 0)
            service = &file->services[i];
    struct mo_process *process = ok && service != NULL ? mo_process_new(image, &limits) : NULL;
    struct mo_sources types_sources = {0};
    ok = process != NULL && mo_sources_init(&types_sources) &&
         mo_upgrade_old_sources(program, &types_sources);
    struct mo_arena arena;
    mo_arena_init(&arena, MO_LIMIT_UPGRADE_RESTART_HEAP);
    struct named *files =
        mo_arena_allocate_array(&arena, types_sources.count, sizeof *files, _Alignof(struct named));
    struct mo_buffer schema;
    mo_buffer_init(&schema, MO_LIMIT_SAVED_BYTES);
    ok = ok && files != NULL &&
         mo_type_schema(mo_process_heap(process), service->state_type, &schema);
    if (ok) {
        const struct mo_bytecode_string *token = &file->strings[service->token_env];
        ok = mo_buffer_append(out, "MOCS\1", 5) && span(out, token->bytes, token->length) &&
             span(out, schema.bytes, schema.length) && mo_binary_write(out, types_sources.count, 4);
        for (uint32_t i = 0; i < types_sources.count; i++) {
            size_t n = strlen(types_sources.files[i].path);
            MO_ASSERT(n <= UINT32_MAX);
            files[i] =
                (struct named){{(uint32_t)n, (const uint8_t *)types_sources.files[i].path}, i};
        }
        qsort(files, types_sources.count, sizeof *files, order);
        for (uint32_t i = 0; ok && i < types_sources.count; i++) {
            const struct mo_source *source = &types_sources.files[files[i].index];
            ok = span(out, files[i].name.bytes, files[i].name.length) &&
                 span(out, source->bytes, source->length);
        }
    }
    const struct mo_types *types = mo_image_types(image);
    bool *seen = mo_arena_allocate_array(&arena, types->count, sizeof *seen, _Alignof(bool));
    uint8_t *flags = mo_arena_allocate(&arena, types->count, 1);
    uint32_t count = service == NULL ? 0 : service->tool_count + service->timer_count;
    struct named *handlers =
        mo_arena_allocate_array(&arena, count, sizeof *handlers, _Alignof(struct named));
    ok = ok && seen != NULL && flags != NULL && handlers != NULL;
    if (ok) {
        memset(flags, 0, types->count);
        for (uint32_t i = 0; i < service->shape_count; i++)
            flags[service->shapes[i].type] = service->shapes[i].is_struct ? 1 : 2;
        mo_process_instruction_limit(process, MO_LIMIT_UPGRADE_RESTART_WORK);
    }
    for (unsigned timer = 0; ok && timer < 2; timer++) {
        uint32_t n = timer ? service->timer_count : service->tool_count;
        for (uint32_t i = 0; i < n; i++)
            handlers[i] = (struct named){timer ? timer_name(file, service->timers[i].function)
                                               : file->strings[service->tools[i].name],
                                         i};
        qsort(handlers, n, sizeof *handlers, order);
        ok = mo_binary_write(out, n, 4);
        for (uint32_t i = 0; ok && i < n; i++) {
            ok = span(out, handlers[i].name.bytes, handlers[i].name.length);
            if (timer) {
                struct mo_cell value;
                if (ok && mo_call(process, service->timers[handlers[i].index].interval, NULL, 0,
                                  &value, &defect) != MO_RETURNED) {
                    ok = false;
                    status = MO_UPGRADE_METADATA_TIMER;
                    (void)fprintf(errors, "mo serve: failure: %.*s: service handler failed\n",
                                  (int)handlers[i].name.length,
                                  (const char *)handlers[i].name.bytes);
                    if (!mo_write_defect(errors, process, &defect))
                        (void)fputs("mo: could not write the defect report\n", errors);
                }
                if (ok) {
                    int64_t duration = mo_cell_get_duration(value);
                    ok = duration > 0 && mo_binary_write(out, (uint64_t)duration, 8);
                }
            } else {
                const struct mo_bytecode_service_tool *tool = &service->tools[handlers[i].index];
                struct mo_sha256 hash;
                uint8_t digest[32];
                mo_sha256_init(&hash);
                hash_number(&hash, tool->read_only ? 1 : 0);
                hash_string(&hash, file->strings[tool->description]);
                hash_number(&hash, tool->argument_count);
                for (uint32_t j = 0; ok && j < tool->argument_count; j++) {
                    hash_string(&hash, file->strings[tool->argument_names[j]]);
                    ok = hash_type(&hash, mo_process_heap(process), types, tool->argument_types[j],
                                   flags, seen);
                }
                ok = ok && hash_type(&hash, mo_process_heap(process), types, tool->result_type,
                                     flags, seen);
                mo_sha256_finish(&hash, digest);
                ok = ok && mo_buffer_append(out, digest, sizeof digest);
            }
        }
    }
    struct mo_compile_metadata checked;
    ok = ok && mo_compile_metadata_read(out->bytes, out->length, &checked);
    mo_buffer_free(&schema);
    mo_sources_free(&types_sources);
    mo_arena_free(&arena);
    mo_process_free(process);
    mo_image_free(image);
    return ok ? MO_UPGRADE_METADATA_OK : status;
}
