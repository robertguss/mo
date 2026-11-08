#define _POSIX_C_SOURCE 200809L
#include "tools/upgrade.h"
#include "base/binary.h"
#include "base/bundle.h"
#include "base/codes.h"
#include "base/compile-context.h"
#include "base/compile-metadata.h"
#include "base/failure.h"
#include "base/runtime-identity.h"
#include "base/sha256.h"
#include "base/stack.h"
#include "base/utf8.h"
#include "codegen/codegen.h"
#include "lower/lower.h"
#include "tools/embedded.h"
#include "tools/offline.h"
#include "tools/run.h"
#include "vm/vm.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static bool upgrade_error(struct mo_diags *diags, uint16_t code, const char *message) {
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
static bool file_bytes(const char *path, size_t limit, struct mo_buffer *out) {
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
            ok = upgrade_error(diags, MO_CODE_UPGRADE_BUNDLE_FORMAT,
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
            (void)upgrade_error(diags, MO_CODE_UPGRADE_BUNDLE_SIZE,
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
    return ok || upgrade_error(diags, MO_CODE_UPGRADE_BUNDLE_FORMAT,
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
            (void)upgrade_error(diags, MO_CODE_UPGRADE_COMPILE_DIAGNOSTIC,
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
    bool ok = file_bytes(path, MO_LIMIT_BUNDLE_BYTES, &bytes);
    if (ok)
        ok = load_bundle_bytes(bytes.bytes, bytes.length, sources, diags);
    else
        (void)upgrade_error(diags, MO_CODE_UPGRADE_BUNDLE_FORMAT,
                            "source bundle is malformed or unreadable");
    mo_buffer_free(&bytes);
    return ok;
}
static bool write_file_bytes(const char *path, const uint8_t *bytes, size_t length);
static bool inject_context(const char *path, struct mo_sources *sources, struct mo_buffer *storage,
                           char migration[MO_LIMIT_UPGRADE_MIGRATION_NAME + 1]) {
    struct mo_compile_context context;
    if (!file_bytes(path, MO_LIMIT_BUNDLE_BYTES, storage) ||
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
         write_file_bytes(path, bytes.bytes, bytes.length);
    mo_buffer_free(&bytes);
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
        ok = file_bytes(work->cli->old_bundle, MO_LIMIT_BUNDLE_BYTES, &old_wire);
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
                (void)upgrade_error(&diags, MO_CODE_UPGRADE_BUNDLE_FORMAT,
                                    "candidate declares the reserved UpgradeOld namespace");
    struct mo_program *program =
        set == NULL || mo_diags_any(&diags) ? NULL : mo_check(&arena, set, &diags);
    ok = program != NULL && !mo_diags_any(&diags);
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
    if (!ok && !runtime_diagnostic && !mo_diags_any(&diags))
        (void)upgrade_error(&diags, MO_CODE_UPGRADE_COMPILE_DIAGNOSTIC,
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

struct upgrade_image {
    struct mo_image *image;
    const struct mo_image *running; /* a live service's image, borrowed in place of image */
    uint32_t service;
    struct mo_store_program program;
    struct mo_buffer context;
    bool schema_synthesized;
};
struct upgrade_report {
    uint16_t code;
    enum mo_compile_status compilation;
    const char *class, *message;
    char detail[MO_LIMIT_UPGRADE_REPORT_BYTES + 1];
    uint8_t base[32], candidate[32];
    bool have_base, have_candidate, activated, already_active;
    uint64_t ordinal, head, vm_work, native_work;
    size_t heap_peak, encoded_bytes;
    struct mo_buffer differences;
    uint8_t old_schema[32], new_schema[32];
    bool compared, implementation_changed, rehearsal, state_valid;
};
static bool refuse_upgrade(struct upgrade_report *r, uint16_t code, const char *class,
                           const char *message) {
    r->code = code;
    r->class = class;
    r->message = message;
    return false;
}
static bool append_digest(struct mo_buffer *out, const uint8_t *digest, bool present) {
    return present ? mo_buffer_append_byte(out, '"') && mo_offline_hex(out, digest, 32) &&
                         mo_buffer_append_byte(out, '"')
                   : mo_buffer_append(out, "null", 4);
}
static bool report_json(struct mo_buffer *out, const struct upgrade_report *r,
                        const char *migration) {
    char prefix[256];
    int n = snprintf(prefix, sizeof prefix, "{\"version\":1,\"status\":\"%s\",\"code\":",
                     r->code != 0        ? "refused"
                     : r->already_active ? "already_active"
                                         : "accepted");
    bool ok = n > 0 && (size_t)n < sizeof prefix && mo_buffer_append(out, prefix, (size_t)n);
    if (r->code == 0)
        ok = ok && mo_buffer_append(out, "null", 4);
    else {
        n = snprintf(prefix, sizeof prefix, "\"E%04u\"", (unsigned)r->code);
        ok = ok && n > 0 && mo_buffer_append(out, prefix, (size_t)n);
    }
    ok = ok && mo_buffer_append(out, ",\"class\":", 9) &&
         mo_render_json_text(out, r->class, strlen(r->class)) &&
         mo_buffer_append(out, ",\"message\":", 11) &&
         mo_render_json_text(out, r->message, strlen(r->message)) &&
         mo_buffer_append(out, ",\"base_source\":", 15) &&
         append_digest(out, r->base, r->have_base) &&
         mo_buffer_append(out, ",\"candidate_source\":", 20) &&
         append_digest(out, r->candidate, r->have_candidate) &&
         mo_buffer_append(out, ",\"active_source\":", 17) &&
         append_digest(out, r->activated ? r->candidate : r->base,
                       r->activated ? r->have_candidate : r->have_base) &&
         mo_buffer_append(out, ",\"migration\":", 13) &&
         (migration == NULL ? mo_buffer_append(out, "null", 4)
                            : mo_render_json_text(out, migration, strlen(migration)));
    if (r->code == 0) {
        if (r->rehearsal)
            n = snprintf(prefix, sizeof prefix, ",\"head\":\"%llu\"", (unsigned long long)r->head);
        else
            n = snprintf(prefix, sizeof prefix, ",\"ordinal\":\"%llu\",\"head\":\"%llu\"",
                         (unsigned long long)r->ordinal, (unsigned long long)r->head);
        ok = ok && n > 0 && (size_t)n < sizeof prefix && mo_buffer_append(out, prefix, (size_t)n);
    }
    if (r->compared)
        ok = ok && mo_buffer_append_text(out, ",\"old_schema\":") &&
             append_digest(out, r->old_schema, true) &&
             mo_buffer_append_text(out, ",\"new_schema\":") &&
             append_digest(out, r->new_schema, true) &&
             mo_buffer_append_text(out, ",\"implementation_changed\":") &&
             mo_buffer_append_text(out, r->implementation_changed ? "true" : "false") &&
             mo_buffer_append_text(out, ",\"differences\":") &&
             mo_buffer_append(out, r->differences.bytes, r->differences.length);
    n = snprintf(prefix, sizeof prefix,
                 ",\"vm_work\":\"%llu\",\"native_work\":\"%llu\",\"heap_peak\":\"%zu\",\"encoded_"
                 "bytes\":\"%zu\"",
                 (unsigned long long)r->vm_work, (unsigned long long)r->native_work, r->heap_peak,
                 r->encoded_bytes);
    ok = ok && n > 0 && (size_t)n < sizeof prefix && mo_buffer_append(out, prefix, (size_t)n);
    if (r->rehearsal)
        ok =
            ok &&
            mo_buffer_append_text(out, ",\"rehearsal\":true,\"accepted\":false,\"state_valid\":") &&
            mo_buffer_append_text(out, r->state_valid ? "true" : "false");
    return ok && mo_buffer_append_byte(out, '}');
}
static bool report_text(struct mo_buffer *text, const struct upgrade_report *report) {
    char prefix[80];
    int n = snprintf(prefix, sizeof prefix,
                     report->code != 0   ? "error E%04u: "
                     : report->rehearsal ? "rehearsal would be accepted: "
                                         : "upgrade accepted: ",
                     (unsigned)report->code);
    bool ok = n > 0 && (size_t)n < sizeof prefix && mo_buffer_append(text, prefix, (size_t)n) &&
              mo_render_terminal_text(text, report->message, strlen(report->message));
    if (ok && report->compared)
        ok = mo_buffer_append_text(text, "\n  descriptor differences: ") &&
             mo_buffer_append(text, report->differences.bytes, report->differences.length) &&
             mo_buffer_append_text(text, report->implementation_changed
                                             ? "\n  implementation changed"
                                             : "\n  implementation unchanged");
    if (report->rehearsal)
        ok = ok && mo_buffer_append_text(text, "\n  rehearsal is not upgrade acceptance");
    return ok && mo_buffer_append_byte(text, '\n');
}
static int show_report(const struct mo_cli *cli, FILE *output, FILE *errors,
                       const struct upgrade_report *report) {
    struct mo_buffer text;
    mo_buffer_init(&text, MO_LIMIT_UPGRADE_REPORT_JSON_BYTES);
    bool ok = cli->json
                  ? report_json(&text, report, cli->migration) && mo_buffer_append_byte(&text, '\n')
                  : report_text(&text, report);
    ok = ok && mo_offline_flush(cli->json || report->code == 0 ? output : errors, &text);
    mo_buffer_free(&text);
    return ok && report->code == 0 ? 0 : 1;
}
static uint64_t monotonic_us(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0)
        return UINT64_MAX;
    return (uint64_t)now.tv_sec * UINT64_C(1000000) + (uint64_t)now.tv_nsec / 1000;
}
static bool write_file_bytes(const char *path, const uint8_t *bytes, size_t length) {
    FILE *file = fopen(path, "wb");
    if (file == NULL)
        return false;
    bool ok = fwrite(bytes, 1, length, file) == length;
    return fclose(file) == 0 && ok;
}
struct compile_job {
    char folder[sizeof "/tmp/mo-upgrade-XXXXXX"];
    char source[128], previous[128], image[128], diagnostics[128], input_context[128],
        output_context[128];
    struct mo_compile_child child;
    bool scratch;
};
static void compile_cleanup(struct compile_job *job) {
    if (!job->scratch)
        return;
    if (job->child.pid != 0)
        mo_compile_cancel(&job->child);
    (void)unlink(job->input_context);
    (void)unlink(job->output_context);
    (void)unlink(job->source);
    (void)unlink(job->previous);
    (void)unlink(job->image);
    (void)unlink(job->diagnostics);
    (void)rmdir(job->folder);
    job->scratch = false;
}
/* Writes the inputs and starts the child; false is a refusal already in report. */
static bool compile_begin(const char *compiler, const struct mo_buffer *bundle,
                          const struct mo_buffer *old, const char *migration,
                          const struct mo_buffer *context, struct compile_job *job,
                          struct upgrade_report *report) {
    memcpy(job->folder, "/tmp/mo-upgrade-XXXXXX", sizeof job->folder);
    if (mkdtemp(job->folder) == NULL)
        return refuse_upgrade(report, MO_CODE_UPGRADE_COMPILE_DIAGNOSTIC, "compile",
                              "cannot create compiler scratch directory");
    job->scratch = true;
    int a = snprintf(job->source, sizeof job->source, "%s/source.mosb", job->folder);
    int b = snprintf(job->previous, sizeof job->previous, "%s/old.mosb", job->folder);
    int c = snprintf(job->image, sizeof job->image, "%s/image.bc", job->folder);
    int d = snprintf(job->diagnostics, sizeof job->diagnostics, "%s/diagnostics", job->folder);
    int e = snprintf(job->input_context, sizeof job->input_context, "%s/input.mocc", job->folder);
    int f = snprintf(job->output_context, sizeof job->output_context, "%s.mocc", job->image);
    MO_ASSERT(a > 0 && b > 0 && c > 0 && d > 0 && e > 0 && f > 0);
    bool ok =
        write_file_bytes(job->source, bundle->bytes, bundle->length) &&
        (old == NULL || write_file_bytes(job->previous, old->bytes, old->length)) &&
        (context == NULL || write_file_bytes(job->input_context, context->bytes, context->length));
    enum mo_compile_status status =
        ok ? mo_compile_start(compiler, job->source, job->image, job->diagnostics,
                              old == NULL ? NULL : job->previous, migration,
                              context == NULL ? NULL : job->input_context, monotonic_us(),
                              &job->child)
           : MO_COMPILE_IO;
    report->compilation = status;
    if (status == MO_COMPILE_RUNNING)
        return true;
    compile_cleanup(job);
    return refuse_upgrade(report, MO_CODE_UPGRADE_COMPILE_DIAGNOSTIC, "compile",
                          "compiler child could not start");
}
static bool compile_end(struct compile_job *job, enum mo_compile_status status,
                        const struct mo_buffer *bundle, struct upgrade_image *out,
                        struct upgrade_report *report) {
    mo_buffer_init(&out->context, MO_LIMIT_BUNDLE_BYTES);
    struct mo_buffer wire;
    mo_buffer_init(&wire, MO_LIMIT_BYTECODE_SIZE);
    bool ok = true;
    report->compilation = status;
    if (status != MO_COMPILE_OK) {
        FILE *errors = fopen(job->diagnostics, "rb");
        report->detail[0] = 0;
        if (errors != NULL) {
            size_t n = fread(report->detail, 1, sizeof report->detail - 1, errors);
            while (n != 0 && !mo_utf8_valid((const uint8_t *)report->detail, n))
                n--;
            report->detail[n] = 0;
            (void)fclose(errors);
        }
        uint16_t code = status == MO_COMPILE_BUDGET ? MO_CODE_UPGRADE_COMPILE_BUDGET
                                                    : MO_CODE_UPGRADE_COMPILE_DIAGNOSTIC;
        if (strstr(report->detail, "E1422:") != NULL)
            code = MO_CODE_UPGRADE_MIGRATION_SIGNATURE;
        ok =
            refuse_upgrade(report, code, status == MO_COMPILE_BUDGET ? "budget" : "compile",
                           report->detail[0] != 0 ? report->detail
                                                  : "compiler child failed or exceeded its budget");
        goto done;
    }
    struct mo_load_error error;
    ok = file_bytes(job->image, MO_LIMIT_BYTECODE_SIZE, &wire);
    out->image = ok ? mo_load(wire.bytes, wire.length, &error) : NULL;
    if (out->image == NULL) {
        ok = refuse_upgrade(report, MO_CODE_UPGRADE_COMPILE_DIAGNOSTIC, "compile",
                            "compiler output failed verification");
        goto done;
    }
    struct mo_compile_context checked_context;
    if (!file_bytes(job->output_context, MO_LIMIT_BUNDLE_BYTES, &out->context) ||
        !mo_compile_context_read(out->context.bytes, out->context.length, &checked_context)) {
        ok = refuse_upgrade(report, MO_CODE_UPGRADE_COMPILE_DIAGNOSTIC, "compile",
                            "compiler context failed validation");
        goto done;
    }
    out->program.context = out->context.bytes;
    out->program.context_size = out->context.length;
    struct mo_sha256 context_hash;
    mo_sha256_init(&context_hash);
    mo_sha256_update(&context_hash, out->context.bytes, out->context.length);
    mo_sha256_finish(&context_hash, out->program.context_digest);
    const struct mo_bytecode *file = mo_image_bytecode(out->image);
    out->service = UINT32_MAX;
    for (uint32_t i = 0; i < file->service_count; i++)
        if (file->services[i].module == 0)
            out->service = i;
    if (out->service == UINT32_MAX) {
        ok = refuse_upgrade(report, MO_CODE_UPGRADE_COMPILE_DIAGNOSTIC, "compile",
                            "entry module declares no service");
        goto done;
    }
    struct mo_limits limits = mo_service_limits();
    limits.heap_size = MO_LIMIT_UPGRADE_RESTART_HEAP;
    struct mo_defect defect;
    if (mo_image_init_bounded(out->image, &limits, MO_LIMIT_UPGRADE_RESTART_WORK, &defect) !=
        MO_RETURNED) {
        ok = refuse_upgrade(report, MO_CODE_UPGRADE_COMPILE_DIAGNOSTIC, "compile",
                            "candidate initialization failed");
        goto done;
    }
    struct mo_process *process = mo_process_new(out->image, &limits);
    ok = process != NULL &&
         mo_type_schema_digest(mo_process_heap(process), file->services[out->service].state_type,
                               out->program.schema);
    mo_process_free(process);
    if (!ok) {
        (void)refuse_upgrade(report, MO_CODE_UPGRADE_COMPILE_DIAGNOSTIC, "compile",
                             "candidate state schema is unavailable");
        goto done;
    }
    struct mo_sha256 hash;
    mo_sha256_init(&hash);
    mo_sha256_update(&hash, bundle->bytes, bundle->length);
    mo_sha256_finish(&hash, out->program.source);
    out->program.image = mo_image_key(out->image, out->service);
    out->program.bundle = bundle->bytes;
    out->program.bundle_size = bundle->length;
    mo_runtime_identity_init();
    ok = mo_runtime_identity(out->program.build);
    if (!ok)
        (void)refuse_upgrade(report, MO_CODE_UPGRADE_COMPILE_DIAGNOSTIC, "compile",
                             "build identity is unavailable");
done:
    if (!ok) {
        if (out->image != NULL)
            mo_image_free(out->image);
        out->image = NULL;
    }
    mo_buffer_free(&wire);
    compile_cleanup(job);
    return ok;
}
static bool compile_image(const struct mo_buffer *bundle, const struct mo_buffer *old,
                          const char *migration, const struct mo_buffer *context,
                          struct upgrade_image *out, struct upgrade_report *report) {
    struct compile_job job = {0};
    if (!compile_begin("/proc/self/exe", bundle, old, migration, context, &job, report)) {
        mo_buffer_init(&out->context, MO_LIMIT_BUNDLE_BYTES);
        return false;
    }
    enum mo_compile_status status = MO_COMPILE_RUNNING;
    while (status == MO_COMPILE_RUNNING) {
        status = mo_compile_poll(&job.child, monotonic_us());
        if (status == MO_COMPILE_RUNNING) {
            struct timespec delay = {0, 10000000};
            (void)nanosleep(&delay, NULL);
        }
    }
    return compile_end(&job, status, bundle, out, report);
}
static bool program_metadata(const struct mo_store_program *program,
                             struct mo_compile_metadata *out) {
    struct mo_compile_context context;
    if (!mo_compile_context_read(program->context, program->context_size, &context) ||
        context.metadata.length == 0 ||
        !mo_compile_metadata_read(context.metadata.bytes, context.metadata.length, out))
        return false;
    struct mo_sha256 hash;
    uint8_t digest[32];
    mo_sha256_init(&hash);
    mo_sha256_update(&hash, out->schema.bytes, out->schema.length);
    mo_sha256_finish(&hash, digest);
    return memcmp(digest, program->schema, 32) == 0;
}
static int span_order(struct mo_bundle_span a, struct mo_bundle_span b) {
    size_t n = a.length < b.length ? a.length : b.length;
    int order = memcmp(a.bytes, b.bytes, n);
    return order != 0 ? order : (a.length > b.length) - (a.length < b.length);
}
static bool difference(struct mo_buffer *out, const char *kind, struct mo_bundle_span name,
                       const char *change, uint32_t *count) {
    bool ok = (*count == 0 || mo_buffer_append_byte(out, ',')) &&
              mo_buffer_append_text(out, "{\"kind\":") &&
              mo_render_json_text(out, kind, strlen(kind)) &&
              mo_buffer_append_text(out, ",\"name\":") &&
              mo_render_json_text(out, name.bytes, name.length) &&
              mo_buffer_append_text(out, ",\"change\":") &&
              mo_render_json_text(out, change, strlen(change)) && mo_buffer_append_byte(out, '}');
    if (ok)
        (*count)++;
    return ok;
}
static bool descriptor_differences(const struct upgrade_image *old,
                                   const struct upgrade_image *next, struct mo_buffer *out) {
    struct mo_compile_metadata a, b;
    if (!program_metadata(&old->program, &a) || !program_metadata(&next->program, &b))
        return false;
    bool ok = mo_buffer_append_byte(out, '[');
    uint32_t changes = 0;
    for (unsigned timer = 0; ok && timer < 2; timer++) {
        struct mo_bundle_span ax = timer ? a.timers : a.tools, bx = timer ? b.timers : b.tools;
        struct mo_binary ai = {ax.bytes, ax.length, 0}, bi = {bx.bytes, bx.length, 0};
        uint32_t ac = timer ? a.timer_count : a.tool_count,
                 bc = timer ? b.timer_count : b.tool_count;
        struct mo_compile_handler ah = {0}, bh = {0};
        bool have_a = false, have_b = false;
        while (ok && (ac != 0 || bc != 0 || have_a || have_b)) {
            if (!have_a && ac != 0) {
                ok = mo_compile_handler_read(&ai, timer != 0, &ah);
                have_a = ok;
                ac--;
            }
            if (ok && !have_b && bc != 0) {
                ok = mo_compile_handler_read(&bi, timer != 0, &bh);
                have_b = ok;
                bc--;
            }
            if (!ok)
                break;
            int order = !have_a ? 1 : !have_b ? -1 : span_order(ah.name, bh.name);
            const char *change = order < 0 ? "removed" : order > 0 ? "added" : NULL;
            if (order == 0 &&
                (timer ? ah.interval != bh.interval : memcmp(ah.digest, bh.digest, 32) != 0))
                change = timer ? "interval_changed" : "schema_or_description_changed";
            if (change != NULL)
                ok = difference(out, timer ? "timer" : "tool", order <= 0 ? ah.name : bh.name,
                                change, &changes);
            if (order <= 0)
                have_a = false;
            if (order >= 0)
                have_b = false;
        }
    }
    return ok && mo_buffer_append_byte(out, ']');
}
static bool compare_programs(const struct upgrade_image *old, const struct upgrade_image *next,
                             const char *migration, struct upgrade_report *report) {
    memcpy(report->base, old->program.source, 32);
    memcpy(report->candidate, next->program.source, 32);
    report->have_base = report->have_candidate = true;
    report->compared = true;
    memcpy(report->old_schema, old->program.schema, 32);
    memcpy(report->new_schema, next->program.schema, 32);
    report->implementation_changed = old->program.image != next->program.image;
    mo_buffer_init(&report->differences, MO_LIMIT_UPGRADE_REPORT_JSON_BYTES);
    if (!descriptor_differences(old, next, &report->differences))
        return refuse_upgrade(report, MO_CODE_UPGRADE_COMPILE_BUDGET, "budget",
                              "descriptor comparison exceeded its work or report bound");
    struct mo_compile_metadata a, b;
    if (!program_metadata(&old->program, &a) || !program_metadata(&next->program, &b))
        return refuse_upgrade(report, MO_CODE_UPGRADE_INVALID_VALUE, "compile",
                              "recorded descriptor is invalid");
    if (span_order(a.token, b.token) != 0) {
        int n =
            snprintf(report->detail, sizeof report->detail,
                     "token from env changed from %.*s to %.*s", (int)a.token.length,
                     (const char *)a.token.bytes, (int)b.token.length, (const char *)b.token.bytes);
        MO_ASSERT(n > 0);
        return refuse_upgrade(report, MO_CODE_UPGRADE_CHANGED_ACCESS, "access", report->detail);
    }
    if (memcmp(old->program.schema, next->program.schema, 32) != 0 && migration == NULL) {
        struct mo_buffer detail;
        mo_buffer_init(&detail, sizeof report->detail - 1);
        bool ok = mo_buffer_append(&detail, "state schema changed from ", 26) &&
                  mo_offline_hex(&detail, old->program.schema, 32) &&
                  mo_buffer_append(&detail, " to ", 4) &&
                  mo_offline_hex(&detail, next->program.schema, 32) &&
                  mo_buffer_append(&detail, "; a migration is required", 25) &&
                  mo_buffer_append_byte(&detail, 0);
        if (ok)
            memcpy(report->detail, detail.bytes, detail.length);
        mo_buffer_free(&detail);
        return refuse_upgrade(report, MO_CODE_UPGRADE_MISSING_MIGRATION, "migration",
                              ok ? report->detail
                                 : "state schema changed; a migration is required");
    }
    report->class = "compatible";
    report->message = migration == NULL ? "state schema unchanged; candidate is compatible"
                                        : "migration signature checked; candidate is compatible";
    return true;
}
static bool recorded_check_input(const char *folder, struct mo_buffer *bundle,
                                 struct mo_buffer *context, struct upgrade_report *report) {
    char path[MO_CLI_ARGUMENT_SIZE + sizeof "/" MO_DATA_FILE];
    struct mo_arena arena;
    mo_arena_init_confidential(&arena, MO_LIMIT_UPGRADE_RESTART_HEAP);
    struct mo_store_program program;
    bool found = false;
    enum mo_store_folder status = mo_offline_store_path(folder, path, sizeof path)
                                      ? mo_store_program_read_file(path, &arena, &program, &found)
                                      : MO_STORE_FOLDER_UNREADABLE;
    bool ok = status == MO_STORE_FOLDER_OK && found;
    if (ok)
        ok = mo_buffer_append(bundle, program.bundle, program.bundle_size) &&
             mo_buffer_append(context, program.context, program.context_size);
    if (!ok) {
        struct mo_buffer message;
        mo_buffer_init(&message, MO_LIMIT_UPGRADE_REPORT_BYTES);
        bool rendered = status != MO_STORE_FOLDER_OK &&
                        mo_offline_folder_message(&message, folder, status, 0) &&
                        mo_buffer_append_byte(&message, 0);
        if (rendered)
            memcpy(report->detail, message.bytes, message.length);
        (void)refuse_upgrade(report,
                             status == MO_STORE_FOLDER_OWNED ? MO_CODE_UPGRADE_BUSY
                             : status == MO_STORE_FOLDER_OK && !found
                                 ? MO_CODE_UPGRADE_SOURCE_MISMATCH
                                 : MO_CODE_UPGRADE_INVALID_VALUE,
                             "store",
                             rendered ? report->detail
                             : found  ? "recorded program exceeds the check memory bound"
                                      : "this folder has no recorded program");
        mo_buffer_free(&message);
    }
    mo_arena_free(&arena);
    return ok;
}
struct check_upgrade_work {
    const struct mo_cli *cli;
    FILE *output, *errors;
    int status;
};
static void check_upgrade(void *context) {
    struct check_upgrade_work *work = context;
    struct mo_buffer old_bundle, candidate, recorded_context;
    mo_buffer_init(&old_bundle, MO_LIMIT_BUNDLE_BYTES);
    mo_buffer_init(&candidate, MO_LIMIT_BUNDLE_BYTES);
    mo_buffer_init(&recorded_context, MO_LIMIT_BUNDLE_BYTES);
    struct mo_sources old_sources = {0}, sources = {0};
    struct mo_diags diags;
    if (!mo_diags_init(&diags))
        return;
    struct upgrade_image old = {0}, next = {0};
    struct upgrade_report report = {.class = "compile", .message = "source bundling failed"};
    struct stat info;
    bool folder = stat(work->cli->upgrade_from, &info) == 0 && S_ISDIR(info.st_mode);
    bool ok =
        mo_sources_init(&old_sources) && mo_sources_init(&sources) &&
        (folder ? recorded_check_input(work->cli->upgrade_from, &old_bundle, &recorded_context,
                                       &report)
                : mo_upgrade_bundle(work->cli->upgrade_from, false, false, &old_bundle, &diags,
                                    &old_sources)) &&
        mo_upgrade_bundle(mo_cli_file(work->cli, 0), true, false, &candidate, &diags, &sources);
    if (ok) {
        struct mo_sha256 hash;
        mo_sha256_init(&hash);
        mo_sha256_update(&hash, old_bundle.bytes, old_bundle.length);
        mo_sha256_finish(&hash, report.base);
        mo_sha256_init(&hash);
        mo_sha256_update(&hash, candidate.bytes, candidate.length);
        mo_sha256_finish(&hash, report.candidate);
        report.have_base = report.have_candidate = true;
    }
    if (ok)
        ok = compile_image(&old_bundle, NULL, NULL, folder ? &recorded_context : NULL, &old,
                           &report) &&
             compile_image(&candidate, folder && work->cli->migration == NULL ? NULL : &old_bundle,
                           work->cli->migration, folder ? &recorded_context : NULL, &next,
                           &report) &&
             compare_programs(&old, &next, work->cli->migration, &report);
    if (!ok && mo_diags_any(&diags)) {
        const struct mo_diag *first = diags.count == 0 ? NULL : &diags.items[0];
        (void)refuse_upgrade(&report,
                             first != NULL && first->code >= 1414 && first->code <= 1431
                                 ? first->code
                                 : MO_CODE_UPGRADE_COMPILE_DIAGNOSTIC,
                             "compile", first == NULL ? "source bundling failed" : first->message);
    } else if (!ok && report.code == 0)
        (void)refuse_upgrade(&report, MO_CODE_UPGRADE_COMPILE_DIAGNOSTIC, "compile",
                             "source compilation failed");
    work->status = show_report(work->cli, work->output, work->errors, &report);
    if (old.image != NULL)
        mo_image_free(old.image);
    if (next.image != NULL)
        mo_image_free(next.image);
    mo_sources_free(&old_sources);
    mo_sources_free(&sources);
    mo_diags_free(&diags);
    mo_buffer_free(&report.differences);
    mo_buffer_free(&old.context);
    mo_buffer_free(&next.context);
    mo_buffer_free(&old_bundle);
    mo_buffer_free(&candidate);
    mo_buffer_free(&recorded_context);
}
int mo_upgrade_check(const struct mo_cli *cli, FILE *output, FILE *errors) {
    struct check_upgrade_work work = {cli, output, errors, 1};
    int status = mo_with_stack(MO_SOURCE_STACK_SIZE, check_upgrade, &work);
    return status == 0 ? work.status : 1;
}

static struct mo_service_time restart_clock(void *context) {
    (void)context;
    return (struct mo_service_time){0, 0};
}
static void restart_reply(const struct mo_service_response *reply, void *context) {
    (void)reply;
    (void)context;
}
static void restart_event(const struct mo_service_event *event, void *context) {
    FILE *errors = context;
    const struct mo_bytecode_string *name = &event->handler;
    const char *reason = mo_service_status_text(event->reason);
    if (errors != NULL) {
        if (event->replay_sequence != 0)
            (void)fprintf(errors, "mo serve: failure: log record %llu: %.*s: %s\n",
                          (unsigned long long)event->replay_sequence,
                          name->length == 0 ? 7 : (int)name->length,
                          name->length == 0 ? "service" : (const char *)name->bytes,
                          reason == NULL ? "recovery failed" : reason);
        else
            (void)fprintf(errors, "mo serve: failure: %.*s: %s\n",
                          name->length == 0 ? 7 : (int)name->length,
                          name->length == 0 ? "service" : (const char *)name->bytes,
                          reason == NULL ? "recovery failed" : reason);
    }
    if (event->defect != NULL && !mo_write_defect(errors, event->defect->process, event->defect))
        (void)fputs("mo: could not write the defect report\n", errors);
}
struct restart_head {
    bool found;
    uint64_t image, sequence, records;
    struct mo_buffer original;
    struct mo_store_record snapshot;
};
static bool read_restart_head(const struct mo_store_record *record, void *context) {
    struct restart_head *head = context;
    if (record->kind == MO_STORE_STATE) {
        if (!mo_buffer_append(&head->original, record->bytes, record->length))
            return false;
        head->snapshot = *record;
        head->snapshot.bytes = head->original.bytes;
    }
    head->found = true;
    head->image = record->image;
    head->sequence = record->sequence;
    head->records += record->kind == MO_STORE_LOG ? 1 : 0;
    return true;
}
static int hex_digit(uint8_t c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}
static bool schema_only_metadata(const struct mo_store_program *program, struct mo_buffer *out) {
    struct mo_compile_metadata metadata;
    if (!program_metadata(program, &metadata))
        return false;
    return mo_buffer_append(out, "MOCS\1", 5) && mo_binary_write(out, metadata.token.length, 4) &&
           mo_buffer_append(out, metadata.token.bytes, metadata.token.length) &&
           mo_binary_write(out, metadata.schema.length, 4) &&
           mo_buffer_append(out, metadata.schema.bytes, metadata.schema.length) &&
           mo_binary_write(out, 0, 4) && mo_binary_write(out, metadata.tool_count, 4) &&
           mo_buffer_append(out, metadata.tools.bytes, metadata.tools.length) &&
           mo_binary_write(out, metadata.timer_count, 4) &&
           mo_buffer_append(out, metadata.timers.bytes, metadata.timers.length);
}
static bool migration_state(const struct upgrade_image *old, const struct upgrade_image *next,
                            const char *migration, const uint8_t *bytes, size_t length,
                            struct mo_buffer *out, struct upgrade_report *report, bool rehearsal) {
    const struct mo_bytecode *file = mo_image_bytecode(next->image);
    const struct mo_bytecode_service *service = &file->services[next->service];
    struct mo_limits limits = mo_service_limits();
    limits.heap_size = rehearsal ? MO_LIMIT_UPGRADE_LIVE_HEAP : MO_LIMIT_UPGRADE_RESTART_HEAP;
    struct mo_process *process = mo_process_new(next->image, &limits);
    struct mo_arena arena;
    mo_arena_init(&arena, limits.heap_size);
    bool ok = process != NULL;
    if (process != NULL)
        mo_process_confidential(process);
    const struct mo_image *old_image = old->running != NULL ? old->running : old->image;
    struct mo_cell value;
    bool owned = false;
    uint32_t result_type = service->state_type;
    uint64_t budget = rehearsal ? MO_LIMIT_UPGRADE_LIVE_WORK : MO_LIMIT_UPGRADE_RESTART_WORK;
    uint64_t work = budget, vm_work = 0;
    if (ok && migration == NULL) {
        ok = mo_value_decode_budget(mo_process_heap(process), result_type, bytes, length, NULL, 0,
                                    &work, &value);
        owned = ok;
    } else if (ok) {
        const struct mo_types *types = mo_image_types(next->image);
        const struct mo_types *old_types = old_image == NULL ? NULL : mo_image_types(old_image);
        const char **names =
            mo_arena_allocate_array(&arena, types->count, sizeof *names, _Alignof(const char *));
        ok = names != NULL;
        const char *root = NULL;
        if (old_image != NULL) {
            const struct mo_bytecode *old_file = mo_image_bytecode(old_image);
            root = old_types->items[old_file->services[old->service].state_type].name;
        } else {
            struct mo_compile_metadata metadata = {0};
            if (!program_metadata(&old->program, &metadata))
                ok = false;
            struct mo_binary schema = {metadata.schema.bytes, metadata.schema.length, 0};
            uint64_t tag, n;
            const uint8_t *name;
            if (ok && mo_binary_uint(&schema, 1, &tag) && (tag == 17 || tag == 18) &&
                mo_binary_uint(&schema, 4, &n) && mo_binary_read(&schema, (size_t)n, &name)) {
                char *copy = mo_arena_allocate(&arena, (size_t)n + 1, 1);
                ok = copy != NULL;
                if (ok) {
                    memcpy(copy, name, (size_t)n);
                    copy[n] = 0;
                    root = copy;
                }
            }
        }
        size_t owner = 0;
        if (root != NULL)
            for (size_t i = 0; root[i] != 0 && root[i] != '('; i++)
                if (root[i] == '.')
                    owner = i;
        struct mo_buffer saved_name;
        mo_buffer_init(&saved_name, MO_LIMIT_STRING_SIZE);
        for (uint32_t i = 0; ok && i < types->count; i++) {
            names[i] = NULL;
            const char *name = types->items[i].name;
            if (name == NULL || strstr(name, "UpgradeOld.") == NULL)
                continue;
            saved_name.length = 0;
            for (size_t at = 0; ok && name[at] != 0;) {
                if (name[at] == '(' || name[at] == ')' || name[at] == ',' || name[at] == ' ') {
                    ok = mo_buffer_append_byte(&saved_name, (uint8_t)name[at++]);
                    continue;
                }
                size_t start = at;
                while (name[at] != 0 && name[at] != '(' && name[at] != ')' && name[at] != ',' &&
                       name[at] != ' ')
                    at++;
                if (at - start >= 11 && memcmp(name + start, "UpgradeOld.", 11) == 0) {
                    start += 11;
                    size_t alias = start;
                    for (size_t j = start; j < at; j++)
                        if (name[j] == '.')
                            alias = j + 1;
                    if (old->schema_synthesized && at - alias > 6 &&
                        memcmp(name + alias, "Saved_", 6) == 0 && (at - alias - 6) % 2 == 0) {
                        for (size_t j = alias + 6; ok && j < at; j += 2) {
                            int high = hex_digit((uint8_t)name[j]),
                                low = hex_digit((uint8_t)name[j + 1]);
                            ok = high >= 0 && low >= 0 &&
                                 mo_buffer_append_byte(&saved_name, (uint8_t)(high * 16 + low));
                        }
                        continue;
                    }
                    bool root_module = memchr(name + start, '.', at - start) == NULL;
                    if (root_module)
                        ok = root != NULL && mo_buffer_append(&saved_name, root, owner) &&
                             mo_buffer_append_byte(&saved_name, '.');
                }
                ok = ok && mo_buffer_append(&saved_name, name + start, at - start);
            }
            ok = ok && mo_buffer_append_byte(&saved_name, 0);
            char *copy = ok ? mo_arena_allocate(&arena, saved_name.length, 1) : NULL;
            ok = copy != NULL;
            if (ok) {
                memcpy(copy, saved_name.bytes, saved_name.length);
                names[i] = copy;
            }
        }
        mo_buffer_free(&saved_name);
        uint32_t selected = UINT32_MAX;
        struct mo_cell argument;
        for (uint32_t i = 0; ok && i < file->function_count; i++) {
            const struct mo_bytecode_function *function = &file->functions[i];
            const struct mo_bytecode_string *full = &file->strings[function->name];
            size_t start = 0, end = 0;
            while (end < full->length && full->bytes[end] != '@') {
                if (full->bytes[end] == '.')
                    start = end + 1;
                end++;
            }
            bool qualified = strchr(migration, '.') != NULL;
            size_t from = qualified ? 0 : start;
            if (function->parameter_count != 1 || strlen(migration) != end - from ||
                memcmp(full->bytes + from, migration, end - from) != 0)
                continue;
            if (mo_value_decode_budget(mo_process_heap(process), function->parameters[0].type,
                                       bytes, length, names, types->count, &work, &argument)) {
                selected = i;
                break;
            }
        }
        ok = ok && selected != UINT32_MAX;
        if (ok) {
            struct mo_defect defect;
            mo_process_instruction_limit(process, work);
            uint64_t before_call = work;
            enum mo_outcome result = mo_call(process, selected, &argument, 1, &value, &defect);
            if (file->functions[selected].parameters[0].ownership == MO_PARAMETER_BORROWED)
                mo_value_drop(mo_process_heap(process),
                              file->functions[selected].parameters[0].type, argument);
            work = mo_process_instructions_remaining(process);
            vm_work += before_call - work;
            ok = result == MO_RETURNED;
            owned = ok;
            if (!ok)
                (void)refuse_upgrade(report,
                                     (work == 0 || defect.memory || defect.limit_name != NULL)
                                         ? MO_CODE_UPGRADE_MIGRATION_BUDGET
                                         : MO_CODE_UPGRADE_MIGRATION_TRAP,
                                     (work == 0 || defect.memory || defect.limit_name != NULL)
                                         ? "budget"
                                         : "migration",
                                     "migration did not return");
        }
    }
    if (ok)
        ok = mo_value_encode_budget(mo_process_heap(process), result_type, value, out, &work);
    if (owned)
        mo_value_drop(mo_process_heap(process), result_type, value);
    if (ok) {
        struct mo_cell verified;
        ok = mo_value_decode_budget(mo_process_heap(process), result_type, out->bytes, out->length,
                                    NULL, 0, &work, &verified);
        if (ok)
            mo_value_drop(mo_process_heap(process), result_type, verified);
    }
    /* All timer expressions run with the same remaining migration instruction budget. */
    bool timers_started = ok;
    if (ok)
        mo_process_instruction_limit(process, work);
    uint64_t before_timers = work;
    for (uint32_t i = 0; ok && i < service->timer_count; i++) {
        struct mo_cell interval;
        struct mo_defect defect;
        enum mo_outcome result =
            mo_call(process, service->timers[i].interval, NULL, 0, &interval, &defect);
        ok = result == MO_RETURNED && mo_cell_get_duration(interval) > 0;
        if (!ok)
            (void)refuse_upgrade(report, MO_CODE_UPGRADE_TIMER_INTERVAL, "timer",
                                 "candidate timer interval is invalid or exceeded its budget");
    }
    if (timers_started) {
        work = mo_process_instructions_remaining(process);
        vm_work += before_timers - work;
    }
    if (!ok && report->code == 0)
        (void)refuse_upgrade(
            report, work == 0 ? MO_CODE_UPGRADE_MIGRATION_BUDGET : MO_CODE_UPGRADE_INVALID_VALUE,
            work == 0 ? "budget" : "migration",
            "migrated state failed exact saved-value validation");
    report->vm_work = vm_work;
    report->native_work = budget - work - vm_work;
    report->heap_peak = process == NULL ? 0 : mo_process_memory_peak(process);
    report->encoded_bytes = out->length;
    mo_arena_free(&arena);
    mo_process_free(process);
    return ok;
}
/* A candidate pushed to a running service: compiled by a child while the service keeps serving,
 * then admitted against the state current at the boundary with the live migration limits. */
struct mo_live_upgrade {
    struct mo_arena arena;
    struct compile_job job;
    struct mo_buffer bundle, previous, context, migrated, json;
    char migration[MO_LIMIT_UPGRADE_MIGRATION_NAME + 1];
    bool has_migration, compiling, staged;
    struct upgrade_image next;
    struct upgrade_report report;
};

static void live_free_buffers(struct mo_live_upgrade *u) {
    compile_cleanup(&u->job);
    if (u->next.image != NULL)
        mo_image_free(u->next.image);
    mo_buffer_free(&u->next.context);
    mo_buffer_free(&u->report.differences);
    mo_buffer_free(&u->bundle);
    mo_buffer_free(&u->previous);
    mo_buffer_free(&u->context);
    mo_buffer_free(&u->migrated);
    mo_buffer_free(&u->json);
}

struct mo_live_upgrade *mo_live_start(const char *compiler, const struct mo_store_program *active,
                                      const uint8_t *bundle, size_t size, const char *migration) {
    MO_ASSERT(compiler != NULL && active != NULL && bundle != NULL);
    struct mo_arena arena;
    mo_arena_init(&arena, sizeof(struct mo_live_upgrade) + MO_ARENA_CHUNK_HEADER);
    struct mo_live_upgrade *u =
        mo_arena_allocate(&arena, sizeof *u, _Alignof(struct mo_live_upgrade));
    if (u == NULL) {
        mo_arena_free(&arena);
        return NULL;
    }
    *u = (struct mo_live_upgrade){.arena = arena,
                                  .report = {.class = "compile", .message = "compiling"}};
    mo_buffer_init(&u->bundle, MO_LIMIT_BUNDLE_BYTES);
    mo_buffer_init(&u->previous, MO_LIMIT_BUNDLE_BYTES);
    mo_buffer_init(&u->context, MO_LIMIT_BUNDLE_BYTES);
    mo_buffer_init(&u->migrated, MO_LIMIT_SAVED_BYTES);
    mo_buffer_init(&u->json, MO_LIMIT_UPGRADE_REPORT_JSON_BYTES);
    mo_buffer_init(&u->next.context, MO_LIMIT_BUNDLE_BYTES);
    mo_buffer_init(&u->report.differences, MO_LIMIT_UPGRADE_REPORT_JSON_BYTES);
    struct mo_sha256 hash;
    mo_sha256_init(&hash);
    mo_sha256_update(&hash, bundle, size);
    mo_sha256_finish(&hash, u->report.candidate);
    memcpy(u->report.base, active->source, 32);
    u->report.have_base = u->report.have_candidate = true;
    u->has_migration = migration != NULL;
    if (migration != NULL) {
        size_t n = strlen(migration);
        MO_ASSERT(n <= MO_LIMIT_UPGRADE_MIGRATION_NAME);
        memcpy(u->migration, migration, n + 1);
    }
    if (!mo_buffer_append(&u->bundle, bundle, size) ||
        (u->has_migration &&
         !mo_buffer_append(&u->previous, active->bundle, active->bundle_size)) ||
        !mo_buffer_append(&u->context, active->context, active->context_size)) {
        mo_live_free(u);
        return NULL;
    }
    struct mo_bundle checked;
    if (!mo_bundle_read(bundle, size, &checked))
        (void)refuse_upgrade(&u->report, MO_CODE_UPGRADE_BUNDLE_FORMAT, "bundle",
                             "source bundle is malformed");
    else
        /* A tool-only candidate inherits the recorded context; a migration also gets the old
         * bundle so the child generates its input types from the running program. */
        u->compiling =
            compile_begin(compiler, &u->bundle, u->has_migration ? &u->previous : NULL,
                          u->has_migration ? u->migration : NULL, &u->context, &u->job, &u->report);
    return u;
}

bool mo_live_poll(struct mo_live_upgrade *u) {
    if (!u->compiling)
        return true;
    enum mo_compile_status status = mo_compile_poll(&u->job.child, monotonic_us());
    if (status == MO_COMPILE_RUNNING)
        return false;
    u->compiling = false;
    mo_buffer_free(&u->next.context);
    u->staged = compile_end(&u->job, status, &u->bundle, &u->next, &u->report);
    return true;
}

uint16_t mo_live_code(const struct mo_live_upgrade *u) {
    return u->report.code;
}

bool mo_live_staged(const struct mo_live_upgrade *u) {
    return u->staged && u->report.code == 0;
}

bool mo_live_admit(struct mo_live_upgrade *u, const struct mo_image *running, uint32_t index,
                   const struct mo_store_program *active, const uint8_t *state, size_t length) {
    MO_ASSERT(mo_live_staged(u));
    struct upgrade_image old = {.running = running, .service = index, .program = *active};
    memcpy(u->report.base, active->source, 32);
    mo_buffer_free(&u->report.differences);
    mo_buffer_clear(&u->migrated);
    const char *migration = u->has_migration ? u->migration : NULL;
    bool ok =
        compare_programs(&old, &u->next, migration, &u->report) &&
        migration_state(&old, &u->next, migration, state, length, &u->migrated, &u->report, true);
    if (ok) {
        u->report.class = "compatible";
        u->report.message = migration == NULL ? "upgrade activated; state schema unchanged"
                                              : "upgrade activated; migrated state validated";
    }
    return ok;
}

void mo_live_refuse(struct mo_live_upgrade *u, uint16_t code, const char *class,
                    const char *message) {
    (void)refuse_upgrade(&u->report, code, class, message);
    u->report.activated = false;
}

const char *mo_live_activation(struct mo_live_upgrade *u, uint64_t ordinal, uint64_t head,
                               size_t *size) {
    u->report.activated = true;
    u->report.ordinal = ordinal;
    u->report.head = head;
    u->next.program.ordinal = ordinal;
    mo_buffer_clear(&u->json);
    if (!report_json(&u->json, &u->report, u->has_migration ? u->migration : NULL))
        return NULL;
    *size = u->json.length;
    return (const char *)u->json.bytes;
}

const struct mo_store_program *mo_live_program(const struct mo_live_upgrade *u) {
    return &u->next.program;
}

const uint8_t *mo_live_state(const struct mo_live_upgrade *u, size_t *length) {
    *length = u->migrated.length;
    return u->migrated.bytes;
}

const struct mo_image *mo_live_image(const struct mo_live_upgrade *u, uint32_t *service) {
    *service = u->next.service;
    return u->next.image;
}

struct mo_image *mo_live_take_image(struct mo_live_upgrade *u) {
    struct mo_image *image = u->next.image;
    u->next.image = NULL;
    return image;
}

const char *mo_live_migration(const struct mo_live_upgrade *u) {
    return u->has_migration ? u->migration : "";
}

bool mo_live_report(const struct mo_live_upgrade *u, bool json, struct mo_buffer *out) {
    return json ? report_json(out, &u->report, u->has_migration ? u->migration : NULL)
                : report_text(out, &u->report);
}

void mo_live_free(struct mo_live_upgrade *u) {
    if (u == NULL)
        return;
    live_free_buffers(u);
    struct mo_arena arena = u->arena;
    mo_arena_free(&arena);
}

bool mo_live_refusal(struct mo_buffer *out, bool json, uint16_t code, const char *class,
                     const char *message, const uint8_t *base, const uint8_t *candidate,
                     const char *migration) {
    struct upgrade_report r = {.code = code, .class = class, .message = message};
    if (base != NULL) {
        memcpy(r.base, base, 32);
        r.have_base = true;
    }
    if (candidate != NULL) {
        memcpy(r.candidate, candidate, 32);
        r.have_candidate = true;
    }
    r.already_active = code == 0;
    return json ? report_json(out, &r, migration) : report_text(out, &r);
}

struct serve_upgrade_work {
    const struct mo_cli *cli;
    FILE *output, *errors;
    int status;
};
static void serve_upgrade(void *context) {
    struct serve_upgrade_work *work = context;
    const struct mo_cli *cli = work->cli;
    struct mo_arena arena;
    mo_arena_init(&arena, MO_LIMIT_UPGRADE_RESTART_HEAP);
    struct mo_sources sources = {0};
    struct mo_diags diags;
    if (!mo_diags_init(&diags)) {
        mo_arena_free(&arena);
        return;
    }
    struct mo_store *store = NULL;
    struct mo_service_loop *staged = NULL;
    struct restart_head head = {0};
    mo_buffer_init(&head.original, MO_LIMIT_SAVED_BYTES);
    struct upgrade_image old = {0}, next = {0};
    struct mo_buffer candidate, migrated, json, schema_fallback;
    mo_buffer_init(&candidate, MO_LIMIT_BUNDLE_BYTES);
    mo_buffer_init(&schema_fallback, MO_LIMIT_BUNDLE_BYTES);
    mo_buffer_init(&migrated, MO_LIMIT_SAVED_BYTES);
    mo_buffer_init(&json, MO_LIMIT_UPGRADE_REPORT_JSON_BYTES);
    struct upgrade_report report = {.class = "store", .message = "restart preparation failed"};
    char path[MO_CLI_ARGUMENT_SIZE + sizeof "/" MO_DATA_FILE];
    int n = snprintf(path, sizeof path, "%s/" MO_DATA_FILE, cli->data);
    MO_ASSERT(n > 0 && (size_t)n < sizeof path);
    struct stat info;
    if (!mo_serve_watch_signals()) {
        int written = snprintf(report.detail, sizeof report.detail,
                               "mo serve could not watch for stop signals: %s", strerror(errno));
        MO_ASSERT(written > 0);
        (void)upgrade_error(&diags, MO_CODE_TOOL_SERVICE_START, report.detail);
        goto done;
    }
    int folder_status = stat(cli->data, &info);
    if (folder_status != 0 || !S_ISDIR(info.st_mode)) {
        int written = snprintf(report.detail, sizeof report.detail,
                               "mo serve can't use the data folder %s: %s", cli->data,
                               strerror(folder_status != 0 ? errno : ENOTDIR));
        MO_ASSERT(written > 0);
        (void)upgrade_error(&diags, MO_CODE_TOOL_SERVICE_START, report.detail);
        goto done;
    }
    /* Capture original files before opening a new database. UpgradeOld placeholders
     * only resolve imports here; the child needs validated recorded context to type them. */
    if (cli->file_count != 0 &&
        (!mo_sources_init(&sources) ||
         !mo_upgrade_bundle(mo_cli_file(cli, 0), true, true, &candidate, &diags, &sources)))
        goto done;
    mode_t mask = umask(S_IRWXG | S_IRWXO);
    enum mo_store_status opened = mo_store_open_wait(path, &store);
    (void)umask(mask);
    if (opened != MO_STORE_OK) {
        (void)refuse_upgrade(&report, MO_CODE_UPGRADE_BUSY, "store", mo_store_status_text(opened));
        work->status = opened == MO_STORE_BUSY ? MO_EXIT_STORE_BUSY : 1;
        if (opened == MO_STORE_BUSY)
            (void)fprintf(work->errors,
                          "mo serve: could not open saved state at %s: %s; stopped waiting\n", path,
                          mo_store_status_text(opened));
        else {
            int written = snprintf(report.detail, sizeof report.detail,
                                   "mo serve could not open saved state at %s: %s", path,
                                   mo_store_status_text(opened));
            MO_ASSERT(written > 0);
            (void)upgrade_error(&diags, MO_CODE_TOOL_SERVICE_START, report.detail);
        }
        goto cleanup_serve;
    }
    struct mo_store_program recorded;
    bool found;
    if (mo_store_program_read(store, &arena, &recorded, &found) != MO_STORE_OK ||
        mo_store_call(store, MO_STORE_LOAD, NULL, read_restart_head, &head) != MO_STORE_OK) {
        (void)refuse_upgrade(&report, MO_CODE_UPGRADE_INVALID_VALUE, "store",
                             "recorded program or saved state is invalid");
        goto done;
    }
    if (cli->file_count == 0 &&
        (!found || !mo_buffer_append(&candidate, recorded.bundle, recorded.bundle_size))) {
        (void)refuse_upgrade(&report, MO_CODE_UPGRADE_SOURCE_MISMATCH, "source",
                             "this folder has no recorded program; provide a source file");
        goto done;
    }
    struct mo_buffer previous = {0}, retained_context = {0};
    if (found) {
        previous.bytes = mo_arena_allocate(&arena, recorded.bundle_size, 1);
        if (previous.bytes == NULL)
            goto done;
        memcpy(previous.bytes, recorded.bundle, recorded.bundle_size);
        previous.length = recorded.bundle_size;
        retained_context.bytes = mo_arena_allocate(&arena, recorded.context_size, 1);
        if (retained_context.bytes == NULL)
            goto done;
        memcpy(retained_context.bytes, recorded.context, recorded.context_size);
        retained_context.length = recorded.context_size;
    }
    struct mo_sha256 source_hash;
    mo_sha256_init(&source_hash);
    mo_sha256_update(&source_hash, candidate.bytes, candidate.length);
    mo_sha256_finish(&source_hash, report.candidate);
    report.have_candidate = true;
    if (found) {
        memcpy(report.base, recorded.source, 32);
        report.have_base = true;
    }
    bool changed = found && memcmp(recorded.source, report.candidate, 32) != 0;
    if (changed && !cli->upgrade) {
        struct mo_buffer reason;
        mo_buffer_init(&reason, sizeof report.detail - 1);
        bool written = mo_buffer_append_text(&reason, "source differs from recorded digest ") &&
                       mo_offline_hex(&reason, recorded.source, 32) &&
                       mo_buffer_append_text(&reason, "; use --upgrade") &&
                       mo_buffer_append_byte(&reason, 0);
        if (written)
            memcpy(report.detail, reason.bytes, reason.length);
        mo_buffer_free(&reason);
        (void)refuse_upgrade(&report, MO_CODE_UPGRADE_SOURCE_MISMATCH, "source",
                             written ? report.detail : "source differs; use --upgrade");
        goto done;
    }
    struct mo_buffer fallback = {0};
    bool fallback_state = false;
    if (changed && !compile_image(&previous, NULL, NULL, &retained_context, &old, &report)) {
        struct mo_compile_metadata metadata;
        if (head.snapshot.sequence != head.sequence || !program_metadata(&recorded, &metadata)) {
            (void)refuse_upgrade(
                &report, MO_CODE_UPGRADE_RECORDED_COMPILE, "compile",
                "recorded source cannot compile and its log tail cannot be replayed");
            goto done;
        }
        struct mo_compile_context context_value;
        if (!mo_compile_context_read(recorded.context, recorded.context_size, &context_value))
            goto done;
        fallback.bytes = mo_arena_allocate(&arena, context_value.metadata.length, 1);
        if (fallback.bytes == NULL)
            goto done;
        memcpy(fallback.bytes, context_value.metadata.bytes, context_value.metadata.length);
        fallback.length = context_value.metadata.length;
        old.program = recorded;
        fallback_state = true;
        report.code = 0;
    }
    const char *requested_migration = changed ? cli->migration : NULL;
    bool compiled_candidate = compile_image(
        &candidate,
        requested_migration == NULL ? NULL
        : fallback_state            ? &fallback
                                    : &previous,
        requested_migration,
        found && (!fallback_state || requested_migration == NULL) ? &retained_context : NULL, &next,
        &report);
    if (!compiled_candidate && fallback_state && requested_migration != NULL &&
        schema_only_metadata(&recorded, &schema_fallback)) {
        mo_buffer_free(&next.context);
        report.code = 0;
        compiled_candidate =
            compile_image(&candidate, &schema_fallback, requested_migration, NULL, &next, &report);
        old.schema_synthesized = compiled_candidate;
    }
    if (!compiled_candidate) {
        bool initializer = report.compilation == MO_COMPILE_INITIALIZER;
        bool service_failure = report.compilation == MO_COMPILE_SERVICE;
        if (!found && !head.found && (initializer || service_failure)) {
            work->status = initializer ? 70 : 1;
            if (fputs(report.detail, work->errors) == EOF)
                work->status = 1;
            if (service_failure)
                (void)upgrade_error(&diags, MO_CODE_TOOL_SERVICE_START,
                                    "mo serve could not start the service: service handler failed");
            goto cleanup_serve;
        }
        if (found && !changed) {
            char diagnostic[MO_LIMIT_UPGRADE_REPORT_BYTES + 1];
            size_t copied = strlen(report.message);
            if (copied >= sizeof diagnostic)
                copied = sizeof diagnostic - 1;
            memcpy(diagnostic, report.message, copied);
            diagnostic[copied] = 0;
            struct mo_buffer reason;
            mo_buffer_init(&reason, sizeof report.detail - 1);
            bool written = mo_buffer_append_text(&reason, "recorded source from build ") &&
                           mo_offline_hex(&reason, recorded.build, 32) &&
                           mo_buffer_append_text(&reason, " could not compile: ");
            size_t room = reason.limit - reason.length;
            if (copied >= room)
                copied = room == 0 ? 0 : room - 1;
            while (copied != 0 && !mo_utf8_valid((const uint8_t *)diagnostic, copied))
                copied--;
            written = written && mo_buffer_append(&reason, diagnostic, copied) &&
                      mo_buffer_append_byte(&reason, 0);
            if (written)
                memcpy(report.detail, reason.bytes, reason.length);
            mo_buffer_free(&reason);
            (void)refuse_upgrade(&report, MO_CODE_UPGRADE_RECORDED_COMPILE, "compile",
                                 written ? report.detail : "recorded source could not compile");
        }
        goto done;
    }
    if (changed) {
        if (!compare_programs(&old, &next, cli->migration, &report))
            goto done;
    }
    const struct mo_bytecode *next_file = mo_image_bytecode(next.image);
    const struct mo_bytecode_string *token =
        &next_file->strings[next_file->services[next.service].token_env];
    char *environment = mo_arena_allocate(&arena, (size_t)token->length + 1, 1);
    if (environment == NULL)
        goto done;
    memcpy(environment, token->bytes, token->length);
    environment[token->length] = 0;
    struct mo_bearer bearer;
    bool credential = mo_bearer_read(&bearer, environment);
    mo_host_wipe(&bearer, sizeof bearer);
    if (!credential) {
        int written = snprintf(report.detail, sizeof report.detail,
                               "mo serve could not read the token from environment variable %s: it "
                               "is missing or invalid",
                               environment);
        MO_ASSERT(written > 0);
        (void)upgrade_error(&diags, MO_CODE_TOOL_SERVICE_START, report.detail);
        goto done;
    }
    const struct upgrade_image *recovery = changed ? &old : &next;
    if (!found && head.found && head.image != next.program.image && !cli->adopt) {
        (void)refuse_upgrade(&report, MO_CODE_UPGRADE_SOURCE_MISMATCH, "adoption",
                             "legacy image differs; explicit --adopt requires verified replay");
        goto done;
    }
    struct mo_service_hooks hooks = {restart_clock, restart_reply, restart_event, work->errors};
    enum mo_service_status recovered =
        fallback_state
            ? MO_SERVICE_OK
            : mo_service_recover(recovery->image, recovery->service, &hooks, store,
                                 head.found ? head.image : recovery->program.image, &staged);
    if (recovered != MO_SERVICE_OK) {
        if (!found && !head.found) {
            int written = snprintf(report.detail, sizeof report.detail,
                                   "mo serve could not start the service: %s",
                                   mo_service_status_text(recovered));
            MO_ASSERT(written > 0);
            (void)upgrade_error(&diags, MO_CODE_TOOL_SERVICE_START, report.detail);
        } else
            (void)refuse_upgrade(&report, MO_CODE_UPGRADE_INVALID_VALUE, "replay",
                                 mo_service_status_text(recovered));
        goto done;
    }
    const uint8_t *bytes = head.snapshot.bytes;
    size_t length = head.snapshot.length;
    if (!fallback_state && !mo_service_encoded_state(staged, &bytes, &length))
        goto done;
    struct mo_store_record snapshot = {
        MO_STORE_STATE, fallback_state ? head.sequence : mo_service_observe(staged).sequence,
        next.program.image, bytes, length};
    if (changed) {
        if (!migration_state(&old, &next, cli->migration, bytes, length, &migrated, &report, false))
            goto done;
        next.program.ordinal = recorded.ordinal + 1;
        report.ordinal = next.program.ordinal;
        report.head = snapshot.sequence;
        report.activated = true;
        if (!report_json(&json, &report, cli->migration))
            goto done;
        struct mo_store_activation activation = {
            .previous = recorded,
            .candidate = next.program,
            .previous_state = {MO_STORE_STATE, snapshot.sequence, recorded.image, bytes, length},
            .candidate_state = {MO_STORE_STATE, snapshot.sequence, next.program.image,
                                migrated.bytes, migrated.length},
            .migration = cli->migration == NULL ? "" : cli->migration,
            .migration_size = cli->migration == NULL ? 0 : strlen(cli->migration),
            .report_json = (const char *)json.bytes,
            .report_size = json.length};
        if (mo_store_activate(store, &activation) != MO_STORE_ACTIVATED) {
            (void)refuse_upgrade(&report, MO_CODE_UPGRADE_INVALID_VALUE, "store",
                                 "activation failed; reopen the folder before retrying");
            goto done;
        }
        if (show_report(cli, work->output, work->errors, &report) != 0)
            goto done;
    }
    bool adopted = !changed && head.found &&
                   (!found || head.image != next.program.image ||
                    memcmp(recorded.build, next.program.build, 32) != 0);
    if (adopted) {
        next.program.ordinal = found ? recorded.ordinal + 1 : 1;
        int written = snprintf(
            report.detail, sizeof report.detail,
            "program adopted after verifying %llu log records%s", (unsigned long long)head.records,
            head.records == 0 ? "; zero records were verified because the tail is empty" : "");
        MO_ASSERT(written > 0);
        report.class = "adoption";
        report.message = report.detail;
        report.activated = true;
        report.ordinal = next.program.ordinal;
        report.head = snapshot.sequence;
        if (!report_json(&json, &report, NULL))
            goto done;
        struct mo_store_program previous_program = found ? recorded : (struct mo_store_program){0};
        if (!found) {
            previous_program.image = head.snapshot.image;
            if (!mo_export_schema(head.snapshot.bytes, head.snapshot.length,
                                  previous_program.schema))
                goto done;
        }
        struct mo_store_activation activation = {.previous = previous_program,
                                                 .candidate = next.program,
                                                 .previous_state = head.snapshot,
                                                 .candidate_state = snapshot,
                                                 .migration = "adopted",
                                                 .migration_size = 7,
                                                 .report_json = (const char *)json.bytes,
                                                 .report_size = json.length};
        if (mo_store_adopt(store, &activation, !found) != MO_STORE_ACTIVATED) {
            (void)refuse_upgrade(&report, MO_CODE_UPGRADE_INVALID_VALUE, "store",
                                 "verified adoption failed; reopen the folder before retrying");
            goto done;
        }
        if (show_report(cli, work->output, work->errors, &report) != 0)
            goto done;
    } else if (!changed && !found) {
        next.program.ordinal = 0;
        if (mo_store_program_record(store, &next.program, &snapshot) != MO_STORE_OK) {
            (void)refuse_upgrade(&report, MO_CODE_UPGRADE_INVALID_VALUE, "store",
                                 "program recording failed; reopen the folder before retrying");
            goto done;
        }
    }
    if (found && !changed && !adopted && cli->upgrade) {
        report.already_active = true;
        report.class = "compatible";
        report.message = "candidate source is already active";
        report.ordinal = recorded.ordinal;
        report.head = snapshot.sequence;
        if (show_report(cli, work->output, work->errors, &report) != 0)
            goto done;
    }
    mo_service_free(staged);
    staged = NULL;
    work->status =
        mo_serve_image(cli, next.image, next.service, work->output, work->errors, &diags, store);
    goto cleanup_serve;
done:
    if (!mo_diags_any(&diags)) {
        if (report.code == 0)
            (void)refuse_upgrade(&report, MO_CODE_UPGRADE_INVALID_VALUE, "restart",
                                 "restart preparation failed");
        work->status = show_report(cli, work->output, work->errors, &report);
    }
cleanup_serve:
    if (mo_diags_any(&diags))
        work->status = mo_write_diagnostics(work->output, work->errors, false, MO_COMMAND_SERVE,
                                            &sources, &diags, NULL);
    mo_service_free(staged);
    mo_store_close(store);
    if (old.image != NULL)
        mo_image_free(old.image);
    if (next.image != NULL)
        mo_image_free(next.image);
    mo_buffer_free(&candidate);
    mo_buffer_free(&report.differences);
    mo_buffer_free(&old.context);
    mo_buffer_free(&next.context);
    mo_buffer_free(&head.original);
    mo_buffer_free(&schema_fallback);
    mo_buffer_free(&migrated);
    mo_buffer_free(&json);
    mo_sources_free(&sources);
    mo_diags_free(&diags);
    mo_arena_free(&arena);
}
int mo_upgrade_serve(const struct mo_cli *cli, FILE *output, FILE *errors) {
    struct serve_upgrade_work work = {cli, output, errors, 1};
    int status = mo_with_stack(MO_SOURCE_STACK_SIZE, serve_upgrade, &work);
    return status == 0 ? work.status : 1;
}

/* Unlike compiler scratch inputs, operator artifacts can name arbitrary filesystem
 * objects. Open without blocking and require a bounded regular file. */
static bool rehearsal_bytes(const char *path, size_t limit, struct mo_buffer *out) {
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0)
        return false;
    struct stat info;
    bool ok = fstat(fd, &info) == 0 && S_ISREG(info.st_mode) && info.st_size >= 0 &&
              (uintmax_t)info.st_size <= limit;
    uint8_t chunk[8192];
    while (ok) {
        ssize_t n = read(fd, chunk, sizeof chunk);
        if (n == 0)
            break;
        if (n < 0) {
            if (errno == EINTR)
                continue;
            ok = false;
        } else
            ok = (size_t)n <= limit - out->length && mo_buffer_append(out, chunk, (size_t)n);
    }
    return close(fd) == 0 && ok;
}

static void rehearse_upgrade(void *context) {
    struct check_upgrade_work *work = context;
    const struct mo_cli *cli = work->cli;
    struct mo_buffer artifact, old_bundle, candidate, recorded_context, migrated;
    mo_buffer_init(&artifact, MO_LIMIT_EXPORT_BYTES);
    mo_buffer_init(&old_bundle, MO_LIMIT_BUNDLE_BYTES);
    mo_buffer_init(&candidate, MO_LIMIT_BUNDLE_BYTES);
    mo_buffer_init(&recorded_context, MO_LIMIT_BUNDLE_BYTES);
    mo_buffer_init(&migrated, MO_LIMIT_SAVED_BYTES);
    struct upgrade_image old = {0}, next = {0};
    struct upgrade_report report = {
        .class = "artifact", .message = "cannot read a complete state export", .rehearsal = true};
    struct mo_export_artifact input = {0};
    bool ok = rehearsal_bytes(cli->artifact, MO_LIMIT_EXPORT_BYTES, &artifact) &&
              mo_export_read(artifact.bytes, artifact.length, &input) && input.identity.kind == 1;
    if (!ok)
        (void)refuse_upgrade(&report, MO_CODE_UPGRADE_INVALID_VALUE, "artifact", report.message);
    uint8_t build[32];
    if (ok && mo_offline_build(input.identity.build, input.label, build) != MO_OFFLINE_BUILD_SAME) {
        struct mo_buffer detail;
        mo_buffer_init(&detail, sizeof report.detail - 1);
        bool rendered = mo_buffer_append_text(&detail, "export build ") &&
                        mo_offline_hex(&detail, input.identity.build, 32) &&
                        mo_buffer_append_text(&detail, " differs from executing build ") &&
                        mo_runtime_identity(build) && mo_offline_hex(&detail, build, 32) &&
                        mo_buffer_append_byte(&detail, 0);
        if (rendered)
            memcpy(report.detail, detail.bytes, detail.length);
        mo_buffer_free(&detail);
        ok = refuse_upgrade(&report, MO_CODE_UPGRADE_SOURCE_MISMATCH, "build",
                            rendered ? report.detail : "export build is unavailable or different");
    }
    if (ok) {
        struct mo_compile_context parsed;
        ok = rehearsal_bytes(cli->old_bundle, MO_LIMIT_BUNDLE_BYTES, &old_bundle) &&
             rehearsal_bytes(cli->candidate, MO_LIMIT_BUNDLE_BYTES, &candidate) &&
             (cli->compile_context == NULL ||
              (rehearsal_bytes(cli->compile_context, MO_LIMIT_BUNDLE_BYTES, &recorded_context) &&
               mo_compile_context_read(recorded_context.bytes, recorded_context.length, &parsed)));
        if (!ok)
            (void)refuse_upgrade(&report, MO_CODE_UPGRADE_BUNDLE_FORMAT, "bundle",
                                 "bundle or compilation context cannot be read or validated");
    }
    const struct mo_buffer *retained = cli->compile_context == NULL ? NULL : &recorded_context;
    if (ok)
        ok = compile_image(&old_bundle, NULL, NULL, retained, &old, &report);
    if (ok &&
        (old.program.image != input.identity.program || old.service != input.identity.service ||
         memcmp(old.program.schema, input.schema, 32) != 0)) {
        int n = snprintf(report.detail, sizeof report.detail,
                         "export program %016llx differs from old bundle program %016llx (or "
                         "service/schema differs)",
                         (unsigned long long)input.identity.program,
                         (unsigned long long)old.program.image);
        MO_ASSERT(n > 0);
        ok = refuse_upgrade(&report, MO_CODE_UPGRADE_SOURCE_MISMATCH, "program", report.detail);
    }
    if (ok)
        ok = compile_image(&candidate,
                           retained != NULL && cli->migration == NULL ? NULL : &old_bundle,
                           cli->migration, retained, &next, &report) &&
             compare_programs(&old, &next, cli->migration, &report) &&
             migration_state(&old, &next, cli->migration, input.state.bytes, input.state.length,
                             &migrated, &report, true);
    if (ok) {
        report.state_valid = true;
        report.head = input.identity.sequence;
        report.message = "migrated state is valid; rehearsal is not upgrade acceptance";
    }
    work->status = show_report(cli, work->output, work->errors, &report);
    if (old.image != NULL)
        mo_image_free(old.image);
    if (next.image != NULL)
        mo_image_free(next.image);
    mo_buffer_free(&old.context);
    mo_buffer_free(&next.context);
    mo_buffer_free(&report.differences);
    mo_buffer_free(&artifact);
    mo_buffer_free(&old_bundle);
    mo_buffer_free(&candidate);
    mo_buffer_free(&recorded_context);
    mo_buffer_free(&migrated);
}
int mo_upgrade_rehearse(const struct mo_cli *cli, FILE *output, FILE *errors) {
    struct check_upgrade_work work = {cli, output, errors, 1};
    int status = mo_with_stack(MO_SOURCE_STACK_SIZE, rehearse_upgrade, &work);
    return status == 0 ? work.status : 1;
}

/* mo replay --failure ID --data DIR without FILE: the program the failure ran under, chosen from
 * the folder's retained programs by image key, build and schema, with its recorded context. */
struct mo_module_set *mo_upgrade_replay_set(struct mo_arena *arena, const char *folder,
                                            uint64_t failure, const char *library,
                                            struct mo_sources *sources, struct mo_diags *diags) {
    char path[MO_CLI_ARGUMENT_SIZE + sizeof "/" MO_DATA_FILE];
    if (!mo_offline_store_path(folder, path, sizeof path)) {
        (void)upgrade_error(diags, MO_CODE_UPGRADE_SOURCE_MISMATCH,
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
        (void)upgrade_error(
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
            (void)upgrade_error(diags, MO_CODE_UPGRADE_INVALID_VALUE,
                                "the retained program or its context is invalid");
    }
    mo_arena_free(&copies);
    return set;
}
