#define _POSIX_C_SOURCE 200809L
#include "base/binary.h"
#include "base/codes.h"
#include "base/compile-context.h"
#include "base/compile-metadata.h"
#include "base/runtime-identity.h"
#include "base/sha256.h"
#include "base/stack.h"
#include "base/utf8.h"
#include "tools/offline.h"
#include "tools/run.h"
#include "tools/upgrade-internal.h"
#include "vm/vm.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int64_t wall_us(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_REALTIME, &now) != 0)
        return 0;
    return (int64_t)now.tv_sec * INT64_C(1000000) + now.tv_nsec / 1000;
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
        (void)mo_upgrade_refuse(report,
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
        ok = mo_upgrade_compile_image(&old_bundle, NULL, NULL, folder ? &recorded_context : NULL,
                                      &old, &report) &&
             mo_upgrade_compile_image(
                 &candidate, folder && work->cli->migration == NULL ? NULL : &old_bundle,
                 work->cli->migration, folder ? &recorded_context : NULL, &next, &report) &&
             mo_upgrade_compare_programs(&old, &next, work->cli->migration, &report);
    if (!ok && mo_diags_any(&diags)) {
        const struct mo_diag *first = diags.count == 0 ? NULL : &diags.items[0];
        (void)mo_upgrade_refuse(&report,
                                first != NULL && first->code >= 1414 && first->code <= 1431
                                    ? first->code
                                    : MO_CODE_UPGRADE_COMPILE_DIAGNOSTIC,
                                "compile",
                                first == NULL ? "source bundling failed" : first->message);
    } else if (!ok && report.code == 0)
        (void)mo_upgrade_refuse(&report, MO_CODE_UPGRADE_COMPILE_DIAGNOSTIC, "compile",
                                "source compilation failed");
    work->status = mo_upgrade_show_report(work->cli, work->output, work->errors, &report);
    if (old.image != NULL)
        mo_image_free(old.image);
    if (next.image != NULL)
        mo_image_free(next.image);
    mo_sources_free(&old_sources);
    mo_sources_free(&sources);
    mo_diags_free(&diags);
    mo_buffer_free(&report.differences);
    mo_upgrade_image_buffers_free(&old);
    mo_upgrade_image_buffers_free(&next);
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

static bool schema_only_metadata(const struct mo_store_program *program, struct mo_buffer *out) {
    struct mo_compile_metadata metadata;
    if (!mo_upgrade_program_metadata(program, &metadata))
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
        (void)mo_upgrade_error(&diags, MO_CODE_TOOL_SERVICE_START, report.detail);
        goto done;
    }
    int folder_status = stat(cli->data, &info);
    if (folder_status != 0 || !S_ISDIR(info.st_mode)) {
        int written = snprintf(report.detail, sizeof report.detail,
                               "mo serve can't use the data folder %s: %s", cli->data,
                               strerror(folder_status != 0 ? errno : ENOTDIR));
        MO_ASSERT(written > 0);
        (void)mo_upgrade_error(&diags, MO_CODE_TOOL_SERVICE_START, report.detail);
        goto done;
    }
    struct mo_bearer upgrade_secret, approval_secret;
    bool secrets = mo_serve_secrets(cli, NULL, &diags, &upgrade_secret, &approval_secret);
    mo_host_wipe(&upgrade_secret, sizeof upgrade_secret);
    mo_host_wipe(&approval_secret, sizeof approval_secret);
    if (!secrets)
        goto done;
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
        (void)mo_upgrade_refuse(&report, MO_CODE_UPGRADE_BUSY, "store",
                                mo_store_status_text(opened));
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
            (void)mo_upgrade_error(&diags, MO_CODE_TOOL_SERVICE_START, report.detail);
        }
        goto cleanup_serve;
    }
    struct mo_store_program recorded;
    bool found;
    if (mo_store_program_read(store, &arena, &recorded, &found) != MO_STORE_OK ||
        mo_store_call(store, MO_STORE_LOAD, NULL, read_restart_head, &head) != MO_STORE_OK) {
        (void)mo_upgrade_refuse(&report, MO_CODE_UPGRADE_INVALID_VALUE, "store",
                                "recorded program or saved state is invalid");
        goto done;
    }
    if (cli->file_count == 0 &&
        (!found || !mo_buffer_append(&candidate, recorded.bundle, recorded.bundle_size))) {
        (void)mo_upgrade_refuse(&report, MO_CODE_UPGRADE_SOURCE_MISMATCH, "source",
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
        (void)mo_upgrade_refuse(&report, MO_CODE_UPGRADE_SOURCE_MISMATCH, "source",
                                written ? report.detail : "source differs; use --upgrade");
        goto done;
    }
    struct mo_buffer fallback = {0};
    bool fallback_state = false;
    if (changed &&
        !mo_upgrade_compile_image(&previous, NULL, NULL, &retained_context, &old, &report)) {
        struct mo_compile_metadata metadata;
        if (head.snapshot.sequence != head.sequence ||
            !mo_upgrade_program_metadata(&recorded, &metadata)) {
            (void)mo_upgrade_refuse(
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
    bool compiled_candidate = mo_upgrade_compile_image(
        &candidate,
        requested_migration == NULL ? NULL
        : fallback_state            ? &fallback
                                    : &previous,
        requested_migration,
        found && (!fallback_state || requested_migration == NULL) ? &retained_context : NULL, &next,
        &report);
    if (!compiled_candidate && fallback_state && requested_migration != NULL &&
        schema_only_metadata(&recorded, &schema_fallback)) {
        mo_upgrade_image_buffers_free(&next);
        report.code = 0;
        compiled_candidate = mo_upgrade_compile_image(&candidate, &schema_fallback,
                                                      requested_migration, NULL, &next, &report);
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
                (void)mo_upgrade_error(
                    &diags, MO_CODE_TOOL_SERVICE_START,
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
            (void)mo_upgrade_refuse(&report, MO_CODE_UPGRADE_RECORDED_COMPILE, "compile",
                                    written ? report.detail : "recorded source could not compile");
        }
        goto done;
    }
    if (changed) {
        if (!mo_upgrade_compare_programs(&old, &next, cli->migration, &report))
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
        (void)mo_upgrade_error(&diags, MO_CODE_TOOL_SERVICE_START, report.detail);
        goto done;
    }
    secrets = mo_serve_secrets(cli, environment, &diags, &upgrade_secret, &approval_secret);
    mo_host_wipe(&upgrade_secret, sizeof upgrade_secret);
    mo_host_wipe(&approval_secret, sizeof approval_secret);
    if (!secrets)
        goto done;
    const struct upgrade_image *recovery = changed ? &old : &next;
    if (!found && head.found && head.image != next.program.image && !cli->adopt) {
        (void)mo_upgrade_refuse(&report, MO_CODE_UPGRADE_SOURCE_MISMATCH, "adoption",
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
            (void)mo_upgrade_error(&diags, MO_CODE_TOOL_SERVICE_START, report.detail);
        } else
            (void)mo_upgrade_refuse(&report, MO_CODE_UPGRADE_INVALID_VALUE, "replay",
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
        if (!mo_upgrade_migration_state(&old, &next, cli->migration, bytes, length, &migrated,
                                        &report, false))
            goto done;
        next.program.ordinal = recorded.ordinal + 1;
        report.ordinal = next.program.ordinal;
        report.head = snapshot.sequence;
        report.activated = true;
        if (!mo_upgrade_report_json(&json, &report, cli->migration))
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
            .report_size = json.length,
            .time = wall_us()};
        if (mo_store_activate(store, &activation) != MO_STORE_ACTIVATED) {
            (void)mo_upgrade_refuse(&report, MO_CODE_UPGRADE_INVALID_VALUE, "store",
                                    "activation failed; reopen the folder before retrying");
            goto done;
        }
        if (mo_upgrade_show_report(cli, work->output, work->errors, &report) != 0)
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
        if (!mo_upgrade_report_json(&json, &report, NULL))
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
                                                 .report_size = json.length,
                                                 .time = wall_us()};
        if (mo_store_adopt(store, &activation, !found) != MO_STORE_ACTIVATED) {
            (void)mo_upgrade_refuse(&report, MO_CODE_UPGRADE_INVALID_VALUE, "store",
                                    "verified adoption failed; reopen the folder before retrying");
            goto done;
        }
        if (mo_upgrade_show_report(cli, work->output, work->errors, &report) != 0)
            goto done;
    } else if (!changed && !found) {
        next.program.ordinal = 0;
        if (mo_store_program_record(store, &next.program, &snapshot) != MO_STORE_OK) {
            (void)mo_upgrade_refuse(&report, MO_CODE_UPGRADE_INVALID_VALUE, "store",
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
        if (mo_upgrade_show_report(cli, work->output, work->errors, &report) != 0)
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
            (void)mo_upgrade_refuse(&report, MO_CODE_UPGRADE_INVALID_VALUE, "restart",
                                    "restart preparation failed");
        work->status = mo_upgrade_show_report(cli, work->output, work->errors, &report);
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
    mo_upgrade_image_buffers_free(&old);
    mo_upgrade_image_buffers_free(&next);
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
        (void)mo_upgrade_refuse(&report, MO_CODE_UPGRADE_INVALID_VALUE, "artifact", report.message);
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
        ok = mo_upgrade_refuse(&report, MO_CODE_UPGRADE_SOURCE_MISMATCH, "build",
                               rendered ? report.detail
                                        : "export build is unavailable or different");
    }
    if (ok) {
        struct mo_compile_context parsed;
        ok = rehearsal_bytes(cli->old_bundle, MO_LIMIT_BUNDLE_BYTES, &old_bundle) &&
             rehearsal_bytes(cli->candidate, MO_LIMIT_BUNDLE_BYTES, &candidate) &&
             (cli->compile_context == NULL ||
              (rehearsal_bytes(cli->compile_context, MO_LIMIT_BUNDLE_BYTES, &recorded_context) &&
               mo_compile_context_read(recorded_context.bytes, recorded_context.length, &parsed)));
        if (!ok)
            (void)mo_upgrade_refuse(&report, MO_CODE_UPGRADE_BUNDLE_FORMAT, "bundle",
                                    "bundle or compilation context cannot be read or validated");
    }
    const struct mo_buffer *retained = cli->compile_context == NULL ? NULL : &recorded_context;
    if (ok)
        ok = mo_upgrade_compile_image(&old_bundle, NULL, NULL, retained, &old, &report);
    if (ok &&
        (old.program.image != input.identity.program || old.service != input.identity.service ||
         memcmp(old.program.schema, input.schema, 32) != 0)) {
        int n = snprintf(report.detail, sizeof report.detail,
                         "export program %016llx differs from old bundle program %016llx (or "
                         "service/schema differs)",
                         (unsigned long long)input.identity.program,
                         (unsigned long long)old.program.image);
        MO_ASSERT(n > 0);
        ok = mo_upgrade_refuse(&report, MO_CODE_UPGRADE_SOURCE_MISMATCH, "program", report.detail);
    }
    if (ok)
        ok = mo_upgrade_compile_image(
                 &candidate, retained != NULL && cli->migration == NULL ? NULL : &old_bundle,
                 cli->migration, retained, &next, &report) &&
             mo_upgrade_compare_programs(&old, &next, cli->migration, &report) &&
             mo_upgrade_migration_state(&old, &next, cli->migration, input.state.bytes,
                                        input.state.length, &migrated, &report, true);
    if (ok) {
        report.state_valid = true;
        report.head = input.identity.sequence;
        report.message = "migrated state is valid; rehearsal is not upgrade acceptance";
    }
    work->status = mo_upgrade_show_report(cli, work->output, work->errors, &report);
    if (old.image != NULL)
        mo_image_free(old.image);
    if (next.image != NULL)
        mo_image_free(next.image);
    mo_upgrade_image_buffers_free(&old);
    mo_upgrade_image_buffers_free(&next);
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
