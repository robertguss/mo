#define _POSIX_C_SOURCE 200809L
#include "base/binary.h"
#include "base/bundle.h"
#include "base/codes.h"
#include "base/compile-context.h"
#include "base/compile-metadata.h"
#include "base/failure.h"
#include "base/stack.h"
#include "codegen/codegen.h"
#include "lower/lower.h"
#include "tools/embedded.h"
#include "tools/intent.h"
#include "tools/offline.h"
#include "tools/upgrade-internal.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

bool mo_upgrade_error(struct mo_diags *diags, uint16_t code, const char *message) {
    bool added = mo_diags_add(
        diags,
        &(struct mo_diag){.code = code, .message = message, .primary.span.file = UINT32_MAX});
    (void)added;
    return false;
}
static const char *library_path(void) {
    struct stat info;
    return stat(mo_library_path, &info) != 0 && errno == ENOENT ? NULL : mo_library_path;
}
bool mo_upgrade_file_bytes(const char *path, size_t limit, struct mo_buffer *out) {
    FILE *file = fopen(path, "rb");
    if (file == NULL)
        return false;
    uint8_t chunk[8192];
    size_t n;
    bool ok = true;
    while ((n = fread(chunk, 1, sizeof chunk, file)) != 0)
        if (n > limit - out->length || !mo_buffer_append(out, chunk, n)) {
            ok = false;
            break;
        }
    ok = ferror(file) == 0 && ok;
    return fclose(file) == 0 && ok;
}
static bool reserved(const char *name) {
    return strcmp(name, "UpgradeOld") == 0 || strncmp(name, "UpgradeOld.", 11) == 0;
}
struct source_file {
    const char *path;
    const struct mo_source *source;
};
static int source_order(const void *a, const void *b) {
    return strcmp(((const struct source_file *)a)->path, ((const struct source_file *)b)->path);
}
bool mo_upgrade_bundle(const char *path, bool migration, bool serving, struct mo_buffer *out,
                       struct mo_diags *diags, struct mo_sources *sources) {
    struct mo_arena arena;
    mo_arena_init(&arena, MO_CLI_CHECK_SIZE);
    const struct mo_module_set *set =
        migration ? mo_load_upgrade_sources(&arena, sources, path, library_path(), diags)
                  : mo_load_program(&arena, sources, path, library_path(), diags);
    bool ok = set != NULL && !mo_diags_any(diags);
    if (ok && serving) {
        const struct mo_syntax *items = set->modules[0]->tree->children[1];
        bool declared = false;
        for (uint32_t i = 0; i < items->count; i++)
            declared |= items->children[i]->kind == MO_S_SERVICE;
        if (!declared) {
            struct mo_diag diag = {
                .code = MO_CODE_TOOL_SERVICE_START,
                .primary = {set->modules[0]->span, "a service block declares one (reference 11.5)"},
                .message = "this module has no service to start"};
            bool added = mo_diags_add(diags, &diag);
            (void)added;
            ok = false;
        }
    }
    struct source_file files[MO_LIMIT_BUNDLE_FILES];
    uint32_t count = 0;
    const char *entry = NULL;
    for (uint32_t i = 0; ok && i < set->count; i++) {
        if (set->library[i])
            continue;
        if (reserved(set->names[i])) {
            if (migration && i != 0)
                continue;
            ok = mo_upgrade_error(diags, MO_CODE_UPGRADE_BUNDLE_FORMAT,
                                  "UpgradeOld is reserved for recorded state types");
            break;
        }
        const struct mo_source *source = &sources->files[set->modules[i]->span.file];
        size_t root = strlen(set->root);
        if (count == MO_LIMIT_BUNDLE_FILES || strncmp(source->path, set->root, root) != 0) {
            ok = false;
            break;
        }
        files[count++] = (struct source_file){source->path + root, source};
        if (i == 0)
            entry = source->path + root;
    }
    size_t start = out->length;
    if (ok) {
        MO_ASSERT(entry != NULL);
        qsort(files, count, sizeof *files, source_order);
        ok = mo_buffer_append(out, "MOSB\1", 5) && mo_binary_write(out, strlen(entry), 4) &&
             mo_buffer_append(out, entry, strlen(entry)) && mo_binary_write(out, count, 4);
        for (uint32_t i = 0; ok && i < count; i++)
            ok = mo_binary_write(out, strlen(files[i].path), 4) &&
                 mo_buffer_append(out, files[i].path, strlen(files[i].path)) &&
                 mo_binary_write(out, files[i].source->length, 4) &&
                 mo_buffer_append(out, files[i].source->bytes, files[i].source->length);
        struct mo_bundle checked;
        ok = ok && mo_bundle_read(out->bytes + start, out->length - start, &checked);
    }
    if (!ok) {
        out->length = start;
        if (!mo_diags_any(diags))
            (void)mo_upgrade_error(diags, MO_CODE_UPGRADE_BUNDLE_SIZE,
                                   "source bundle exceeds its admission limits");
    }
    mo_arena_free(&arena);
    return ok;
}
static bool load_bundle_bytes(const uint8_t *bytes, size_t length, struct mo_sources *sources,
                              struct mo_diags *diags) {
    struct mo_bundle bundle;
    bool ok = mo_bundle_read(bytes, length, &bundle);
    uint32_t entry = 0;
    if (ok) {
        for (uint32_t i = 0; i < bundle.count; i++)
            if (bundle.files[i].path.length == bundle.entry.length &&
                memcmp(bundle.files[i].path.bytes, bundle.entry.bytes, bundle.entry.length) == 0)
                entry = i;
        for (uint32_t n = 0; ok && n < bundle.count; n++) {
            uint32_t i = n == 0 ? entry : n <= entry ? n - 1 : n;
            char name[MO_LIMIT_BUNDLE_PATH_BYTES + 1];
            memcpy(name, bundle.files[i].path.bytes, bundle.files[i].path.length);
            name[bundle.files[i].path.length] = 0;
            uint32_t file;
            struct mo_source_error error;
            ok = mo_sources_add_bytes(sources, name, bundle.files[i].source.bytes,
                                      bundle.files[i].source.length, &file, &error);
        }
    }
    return ok || mo_upgrade_error(diags, MO_CODE_UPGRADE_BUNDLE_FORMAT,
                                  "source bundle is malformed or unreadable");
}
struct mo_module_set *mo_upgrade_source_set(struct mo_arena *arena, const char *path,
                                            const char *library, struct mo_sources *sources,
                                            struct mo_diags *diags) {
    struct mo_buffer bundle;
    mo_buffer_init(&bundle, MO_LIMIT_BUNDLE_BYTES);
    struct mo_module_set *set = NULL;
    if (mo_upgrade_bundle(path, false, false, &bundle, diags, sources)) {
        /* Keep the original sources on a load error so diagnostics retain their spans. */
        mo_sources_free(sources);
        if (!mo_sources_init(sources))
            (void)mo_upgrade_error(diags, MO_CODE_UPGRADE_COMPILE_DIAGNOSTIC,
                                   "source replay could not allocate its bundled sources");
        else if (load_bundle_bytes(bundle.bytes, bundle.length, sources, diags))
            set = mo_load_bundle_sources(arena, sources, library, diags);
    }
    mo_buffer_free(&bundle);
    return set;
}

static bool load_bundle(const char *path, struct mo_sources *sources, struct mo_diags *diags) {
    struct mo_buffer bytes;
    mo_buffer_init(&bytes, MO_LIMIT_BUNDLE_BYTES);
    bool ok = mo_upgrade_file_bytes(path, MO_LIMIT_BUNDLE_BYTES, &bytes);
    if (ok)
        ok = load_bundle_bytes(bytes.bytes, bytes.length, sources, diags);
    else
        (void)mo_upgrade_error(diags, MO_CODE_UPGRADE_BUNDLE_FORMAT,
                               "source bundle is malformed or unreadable");
    mo_buffer_free(&bytes);
    return ok;
}
static bool inject_context(const char *path, struct mo_sources *sources, struct mo_buffer *storage,
                           char migration[MO_LIMIT_UPGRADE_MIGRATION_NAME + 1]) {
    struct mo_compile_context context;
    if (!mo_upgrade_file_bytes(path, MO_LIMIT_BUNDLE_BYTES, storage) ||
        !mo_compile_context_read(storage->bytes, storage->length, &context))
        return false;
    memcpy(migration, context.migration.bytes, context.migration.length);
    migration[context.migration.length] = 0;
    for (uint32_t i = 0; i < context.count; i++) {
        char name[MO_LIMIT_BUNDLE_PATH_BYTES + 1];
        memcpy(name, context.files[i].path.bytes, context.files[i].path.length);
        name[context.files[i].path.length] = 0;
        uint32_t file;
        struct mo_source_error error;
        if (!mo_sources_add_bytes(sources, name, context.files[i].source.bytes,
                                  context.files[i].source.length, &file, &error))
            return false;
    }
    return true;
}
static bool save_context(const struct mo_sources *sources, const char *migration,
                         const char *output, const struct mo_buffer *metadata) {
    struct source_file files[MO_LIMIT_BUNDLE_FILES];
    uint32_t count = 0;
    for (uint32_t i = 0; i < sources->count; i++) {
        const struct mo_source *source = &sources->files[i];
        if (strncmp(source->path, "UpgradeOld.", 11) != 0)
            continue;
        if (count == MO_LIMIT_BUNDLE_FILES)
            return false;
        files[count++] = (struct source_file){source->path, source};
    }
    qsort(files, count, sizeof *files, source_order);
    struct mo_buffer bytes;
    mo_buffer_init(&bytes, MO_LIMIT_BUNDLE_BYTES);
    size_t n = migration == NULL ? 0 : strlen(migration);
    bool ok = mo_buffer_append(&bytes, "MOCC\1", 5) && mo_binary_write(&bytes, n, 4) &&
              mo_buffer_append(&bytes, migration, n) && mo_binary_write(&bytes, count, 4);
    for (uint32_t i = 0; ok && i < count; i++)
        ok = mo_binary_write(&bytes, strlen(files[i].path), 4) &&
             mo_buffer_append(&bytes, files[i].path, strlen(files[i].path)) &&
             mo_binary_write(&bytes, files[i].source->length, 4) &&
             mo_buffer_append(&bytes, files[i].source->bytes, files[i].source->length);
    ok = ok && mo_binary_write(&bytes, metadata->length, 4) &&
         mo_buffer_append(&bytes, metadata->bytes, metadata->length);
    struct mo_compile_context checked;
    ok = ok && mo_compile_context_read(bytes.bytes, bytes.length, &checked);
    char path[MO_CLI_ARGUMENT_SIZE + 6];
    int written = snprintf(path, sizeof path, "%s.mocc", output);
    ok = ok && written > 0 && (size_t)written < sizeof path &&
         mo_upgrade_write_file_bytes(path, bytes.bytes, bytes.length);
    mo_buffer_free(&bytes);
    return ok;
}
static bool list_items(struct mo_arena *arena, const struct mo_program *program,
                       const char *output) {
    struct mo_intent intent;
    struct mo_buffer list;
    mo_buffer_init(&list, MO_LIMIT_INTENT_LIST_BYTES);
    bool ok = mo_intent_items(arena, program, &intent) && mo_intent_list_write(&intent, &list) &&
              mo_upgrade_write_file_bytes(output, list.bytes, list.length);
    mo_buffer_free(&list);
    return ok;
}
struct compile_bundle_work {
    const struct mo_cli *cli;
    FILE *output, *errors;
    int status;
};
static void compile_bundle(void *context) {
    struct compile_bundle_work *work = context;
    struct mo_arena arena, lower;
    mo_arena_init(&arena, MO_CLI_CHECK_SIZE);
    mo_arena_init(&lower, MO_LIMIT_LOWER_WORK);
    struct mo_diags diags;
    if (!mo_diags_init(&diags)) {
        mo_arena_free(&lower);
        mo_arena_free(&arena);
        return;
    }
    struct mo_sources sources = {0}, old_sources = {0};
    bool ok = mo_sources_init(&sources) && mo_sources_init(&old_sources) &&
              load_bundle(mo_cli_file(work->cli, 0), &sources, &diags);
    uint32_t candidate_count = sources.count;
    struct mo_buffer context_storage;
    mo_buffer_init(&context_storage, MO_LIMIT_BUNDLE_BYTES);
    char retained_migration[MO_LIMIT_UPGRADE_MIGRATION_NAME + 1] = {0};
    const char *selected_migration = work->cli->migration;
    if (ok && work->cli->old_bundle == NULL && work->cli->compile_context != NULL) {
        ok = inject_context(work->cli->compile_context, &sources, &context_storage,
                            retained_migration);
        if (selected_migration == NULL && retained_migration[0] != 0)
            selected_migration = retained_migration;
    }
    struct mo_buffer old_wire;
    mo_buffer_init(&old_wire, MO_LIMIT_BUNDLE_BYTES);
    if (ok && work->cli->old_bundle != NULL) {
        ok = mo_upgrade_file_bytes(work->cli->old_bundle, MO_LIMIT_BUNDLE_BYTES, &old_wire);
        bool metadata_input =
            ok && old_wire.length >= 5 && memcmp(old_wire.bytes, "MOCS\1", 5) == 0;
        if (metadata_input) {
            struct mo_compile_metadata metadata;
            ok = mo_compile_metadata_read(old_wire.bytes, old_wire.length, &metadata);
            if (ok && metadata.type_count == 0) {
                struct mo_buffer saved;
                mo_buffer_init(&saved, MO_LIMIT_SAVED_BYTES);
                ok = mo_buffer_append(&saved, "MOSV\1", 5) &&
                     mo_binary_write(&saved, metadata.schema.length, 4) &&
                     mo_buffer_append(&saved, metadata.schema.bytes, metadata.schema.length) &&
                     mo_upgrade_schema_sources(saved.bytes, saved.length, &sources);
                mo_buffer_free(&saved);
            } else {
                for (uint32_t i = 0; ok && i < metadata.type_count; i++) {
                    char path[MO_LIMIT_BUNDLE_PATH_BYTES + 1];
                    memcpy(path, metadata.types[i].path.bytes, metadata.types[i].path.length);
                    path[metadata.types[i].path.length] = 0;
                    uint32_t file;
                    struct mo_source_error error;
                    ok = mo_sources_add_bytes(&sources, path, metadata.types[i].source.bytes,
                                              metadata.types[i].source.length, &file, &error);
                }
            }
        } else {
            ok = ok && load_bundle_bytes(old_wire.bytes, old_wire.length, &old_sources, &diags);
            if (ok && work->cli->compile_context != NULL)
                ok = inject_context(work->cli->compile_context, &old_sources, &context_storage,
                                    retained_migration);
            struct mo_module_set *old_set =
                ok ? mo_load_bundle_sources(&arena, &old_sources, library_path(), &diags) : NULL;
            struct mo_program *old = old_set == NULL ? NULL : mo_check(&arena, old_set, &diags);
            ok = old != NULL && !mo_diags_any(&diags) && mo_upgrade_old_sources(old, &sources);
        }
    }
    struct mo_module_set *set =
        ok ? mo_load_bundle_sources(&arena, &sources, library_path(), &diags) : NULL;
    if (set != NULL)
        for (uint32_t i = 0; i < set->count; i++)
            if (set->modules[i]->span.file < candidate_count && reserved(set->names[i]))
                (void)mo_upgrade_error(&diags, MO_CODE_UPGRADE_BUNDLE_FORMAT,
                                       "candidate declares the reserved UpgradeOld namespace");
    struct mo_program *program =
        set == NULL || mo_diags_any(&diags) ? NULL : mo_check(&arena, set, &diags);
    ok = program != NULL && !mo_diags_any(&diags);
    bool listing = work->cli->command == MO_COMMAND_TEST;
    if (listing) {
        if (ok && !list_items(&arena, program, work->cli->output))
            (void)mo_upgrade_error(&diags, MO_CODE_UPGRADE_COMPILE_DIAGNOSTIC,
                                   "the program's intent items exceed their limits");
        ok = false;
    }
    if (ok && selected_migration == retained_migration && retained_migration[0] != 0) {
        bool present = false;
        for (uint32_t i = 0; i < program->function_count; i++) {
            const struct mo_symbol *symbol = program->functions[i].signature->function;
            if (strcmp(symbol->name, selected_migration) == 0)
                present = true;
            const char *module = set->names[symbol->module];
            size_t n = strlen(module);
            if (strncmp(selected_migration, module, n) == 0 && selected_migration[n] == '.' &&
                strcmp(selected_migration + n + 1, symbol->name) == 0)
                present = true;
        }
        if (!present)
            selected_migration = NULL;
    }
    uint32_t migration = UINT32_MAX;
    if (ok && selected_migration != NULL) {
        const struct mo_source_type *old_state = NULL;
        for (uint32_t i = 0; i < program->function_count; i++) {
            const struct mo_function_signature *f = program->functions[i].signature;
            if (strcmp(set->names[f->function->module], "UpgradeOld") == 0 &&
                strcmp(f->function->name, "upgrade_input") == 0)
                old_state = f->type->arguments[0];
        }
        ok = old_state != NULL &&
             mo_check_migration_entry(&arena, program, (const uint8_t *)selected_migration,
                                      strlen(selected_migration), old_state, &diags,
                                      &migration) == MO_QUERY_ENTRY_ACCEPTED;
    }
    struct mo_services services;
    uint32_t index;
    struct mo_core *core =
        !ok ? NULL
        : migration == UINT32_MAX
            ? mo_lower_services(&lower, program, &sources, &diags, NULL, &services)
            : mo_lower_query(&lower, program, &sources, &diags, migration, &services, &index);
    struct mo_buffer wire;
    mo_buffer_init(&wire, MO_LIMIT_BYTECODE_SIZE);
    ok = core != NULL && !mo_diags_any(&diags) && mo_core_check_services(core, &services, &diags) &&
         mo_compile_services(core, &services, &wire, &diags);
    if (ok) {
        FILE *output = fopen(work->cli->output, "wb");
        ok = output != NULL;
        if (output != NULL) {
            ok = fwrite(wire.bytes, 1, wire.length, output) == wire.length;
            ok = fclose(output) == 0 && ok;
        }
    }
    struct mo_buffer metadata;
    mo_buffer_init(&metadata, MO_LIMIT_BUNDLE_BYTES);
    enum mo_upgrade_metadata_status metadata_status = MO_UPGRADE_METADATA_ERROR;
    if (ok) {
        metadata_status = mo_upgrade_metadata(program, &wire, &metadata, work->errors);
        ok = metadata_status == MO_UPGRADE_METADATA_OK &&
             save_context(&sources, selected_migration, work->cli->output, &metadata);
    }
    bool runtime_diagnostic = metadata_status == MO_UPGRADE_METADATA_INITIALIZER ||
                              metadata_status == MO_UPGRADE_METADATA_TIMER;
    mo_buffer_free(&metadata);
    if (!ok && !listing && !runtime_diagnostic && !mo_diags_any(&diags))
        (void)mo_upgrade_error(&diags, MO_CODE_UPGRADE_COMPILE_DIAGNOSTIC,
                               "bundle compilation failed");
    work->status = runtime_diagnostic
                       ? (metadata_status == MO_UPGRADE_METADATA_INITIALIZER ? 70 : 69)
                       : mo_write_diagnostics(work->output, work->errors, false, MO_COMMAND_EMIT,
                                              &sources, &diags, NULL);
    mo_buffer_free(&wire);
    mo_buffer_free(&context_storage);
    mo_buffer_free(&old_wire);
    mo_sources_free(&old_sources);
    mo_sources_free(&sources);
    mo_diags_free(&diags);
    mo_arena_free(&lower);
    mo_arena_free(&arena);
}
int mo_upgrade_emit(const struct mo_cli *cli, FILE *output, FILE *errors) {
    struct compile_bundle_work work = {cli, output, errors, 1};
    int status = mo_with_stack(MO_SOURCE_STACK_SIZE, compile_bundle, &work);
    return status == 0 ? work.status : 1;
}

bool mo_upgrade_write_file_bytes(const char *path, const uint8_t *bytes, size_t length) {
    FILE *file = fopen(path, "wb");
    if (file == NULL)
        return false;
    bool ok = fwrite(bytes, 1, length, file) == length;
    return fclose(file) == 0 && ok;
}

/* mo replay --failure ID --data DIR without FILE: the program the failure ran under, chosen from
 * the folder's retained programs by image key, build and schema, with its recorded context. */
struct mo_module_set *mo_upgrade_replay_set(struct mo_arena *arena, const char *folder,
                                            uint64_t failure, const char *library,
                                            struct mo_sources *sources, struct mo_diags *diags) {
    char path[MO_CLI_ARGUMENT_SIZE + sizeof "/" MO_DATA_FILE];
    if (!mo_offline_store_path(folder, path, sizeof path)) {
        (void)mo_upgrade_error(diags, MO_CODE_UPGRADE_SOURCE_MISMATCH,
                               "the data folder path is too long");
        return NULL;
    }
    struct mo_store_history_pair pair;
    enum mo_store_folder read = mo_store_history_pair(path, false, failure, &pair);
    struct mo_failure_record record;
    bool found = read == MO_STORE_FOLDER_OK && pair.found &&
                 mo_failure_read(pair.message.bytes, pair.message.length, &record);
    struct mo_arena copies;
    mo_arena_init_confidential(&copies, 2 * MO_LIMIT_BUNDLE_BYTES + 2 * MO_ARENA_CHUNK_HEADER);
    struct mo_store_program program;
    uint32_t matches = 0;
    bool distinct = false;
    if (found)
        found = mo_store_retained_program_read_file(path, &copies, record.program, record.build,
                                                    record.schema, &program, &matches,
                                                    &distinct) == MO_STORE_FOLDER_OK;
    if (read == MO_STORE_FOLDER_OK)
        mo_store_history_pair_free(&pair);
    struct mo_module_set *set = NULL;
    char message[192];
    int n = 0;
    if (!found || matches == 0)
        n = snprintf(message, sizeof message,
                     "no retained program matches failure %llu; replay it with its source FILE",
                     (unsigned long long)failure);
    else if (distinct)
        n = snprintf(message, sizeof message,
                     "%u retained programs with different sources match failure %llu; replay it "
                     "with the intended source FILE",
                     (unsigned)matches, (unsigned long long)failure);
    MO_ASSERT(n >= 0 && (size_t)n < sizeof message);
    if (n > 0)
        (void)mo_upgrade_error(
            diags, distinct ? MO_CODE_UPGRADE_AMBIGUOUS_PROGRAM : MO_CODE_UPGRADE_SOURCE_MISMATCH,
            message);
    else {
        struct mo_compile_context context;
        bool ok = load_bundle_bytes(program.bundle, program.bundle_size, sources, diags) &&
                  mo_compile_context_read(program.context, program.context_size, &context);
        for (uint32_t i = 0; ok && i < context.count; i++) {
            char name[MO_LIMIT_BUNDLE_PATH_BYTES + 1];
            memcpy(name, context.files[i].path.bytes, context.files[i].path.length);
            name[context.files[i].path.length] = 0;
            uint32_t file;
            struct mo_source_error error;
            ok = mo_sources_add_bytes(sources, name, context.files[i].source.bytes,
                                      context.files[i].source.length, &file, &error);
        }
        set = ok ? mo_load_bundle_sources(arena, sources, library, diags) : NULL;
        if (!ok && !mo_diags_any(diags))
            (void)mo_upgrade_error(diags, MO_CODE_UPGRADE_INVALID_VALUE,
                                   "the retained program or its context is invalid");
    }
    mo_arena_free(&copies);
    return set;
}
