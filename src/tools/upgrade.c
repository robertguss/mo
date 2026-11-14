#define _POSIX_C_SOURCE 200809L
#include "tools/pause-timing.h"
#include "tools/upgrade-internal.h"
#ifdef MO_UPGRADE_PAUSE_TIMING
uint64_t mo_pause_attempt;
#endif
#include "base/binary.h"
#include "base/bundle.h"
#include "base/codes.h"
#include "base/compile-context.h"
#include "base/compile-metadata.h"
#include "base/intent-list.h"
#include "base/runtime-identity.h"
#include "base/sha256.h"
#include "base/utf8.h"
#include "tools/offline.h"
#include "vm/vm.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

bool mo_upgrade_refuse(struct upgrade_report *r, uint16_t code, const char *class,
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
bool mo_upgrade_report_json(struct mo_buffer *out, const struct upgrade_report *r,
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
bool mo_upgrade_report_text(struct mo_buffer *text, const struct upgrade_report *report) {
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
int mo_upgrade_show_report(const struct mo_cli *cli, FILE *output, FILE *errors,
                           const struct upgrade_report *report) {
    struct mo_buffer text;
    mo_buffer_init(&text, MO_LIMIT_UPGRADE_REPORT_JSON_BYTES);
    bool ok = cli->json ? mo_upgrade_report_json(&text, report, cli->migration) &&
                              mo_buffer_append_byte(&text, '\n')
                        : mo_upgrade_report_text(&text, report);
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
void mo_upgrade_compile_cleanup(struct compile_job *job) {
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
    (void)unlink(job->items);
    (void)unlink(job->list_diagnostics);
    (void)rmdir(job->folder);
    job->scratch = false;
}
/* Writes the inputs and starts the child; false is a refusal already in report. */
bool mo_upgrade_compile_begin(const char *compiler, const struct mo_buffer *bundle,
                              const struct mo_buffer *old, const char *migration,
                              const struct mo_buffer *context, struct compile_job *job,
                              struct upgrade_report *report) {
    memcpy(job->folder, "/tmp/mo-upgrade-XXXXXX", sizeof job->folder);
    if (mkdtemp(job->folder) == NULL)
        return mo_upgrade_refuse(report, MO_CODE_UPGRADE_COMPILE_DIAGNOSTIC, "compile",
                                 "cannot create compiler scratch directory");
    job->scratch = true;
    int a = snprintf(job->source, sizeof job->source, "%s/source.mosb", job->folder);
    int b = snprintf(job->previous, sizeof job->previous, "%s/old.mosb", job->folder);
    int c = snprintf(job->image, sizeof job->image, "%s/image.bc", job->folder);
    int d = snprintf(job->diagnostics, sizeof job->diagnostics, "%s/diagnostics", job->folder);
    int e = snprintf(job->input_context, sizeof job->input_context, "%s/input.mocc", job->folder);
    int f = snprintf(job->output_context, sizeof job->output_context, "%s.mocc", job->image);
    int g = snprintf(job->items, sizeof job->items, "%s/items.moil", job->folder);
    int h = snprintf(job->list_diagnostics, sizeof job->list_diagnostics, "%s/list-diagnostics",
                     job->folder);
    MO_ASSERT(a > 0 && b > 0 && c > 0 && d > 0 && e > 0 && f > 0 && g > 0 && h > 0);
    job->compiler = compiler;
    job->has_old = old != NULL;
    job->has_context = context != NULL;
    bool ok =
        mo_upgrade_write_file_bytes(job->source, bundle->bytes, bundle->length) &&
        (old == NULL || mo_upgrade_write_file_bytes(job->previous, old->bytes, old->length)) &&
        (context == NULL ||
         mo_upgrade_write_file_bytes(job->input_context, context->bytes, context->length));
    enum mo_compile_status status =
        ok ? mo_compile_start(compiler, job->source, job->image, job->diagnostics,
                              old == NULL ? NULL : job->previous, migration,
                              context == NULL ? NULL : job->input_context, monotonic_us(),
                              &job->child)
           : MO_COMPILE_IO;
    report->compilation = status;
    if (status == MO_COMPILE_RUNNING)
        return true;
    mo_upgrade_compile_cleanup(job);
    return mo_upgrade_refuse(report, MO_CODE_UPGRADE_COMPILE_DIAGNOSTIC, "compile",
                             "compiler child could not start");
}
enum mo_compile_status mo_upgrade_compile_poll(struct compile_job *job) {
    uint64_t now = monotonic_us();
    enum mo_compile_status status = mo_compile_poll(&job->child, now);
    if (status != MO_COMPILE_OK || job->listing)
        return status;
    job->listing = true;
    status = mo_list_start(job->compiler, job->source, job->items, job->list_diagnostics,
                           job->has_old ? job->previous : NULL,
                           job->has_context ? job->input_context : NULL, now, &job->child);
    return status == MO_COMPILE_RUNNING ? status : MO_COMPILE_IO;
}
void mo_upgrade_image_buffers_init(struct upgrade_image *image) {
    mo_buffer_init(&image->context, MO_LIMIT_BUNDLE_BYTES);
    mo_buffer_init(&image->items, MO_LIMIT_INTENT_LIST_BYTES);
}
void mo_upgrade_image_buffers_free(struct upgrade_image *image) {
    mo_buffer_free(&image->context);
    mo_buffer_free(&image->items);
}
bool mo_upgrade_compile_end(struct compile_job *job, enum mo_compile_status status,
                            const struct mo_buffer *bundle, struct upgrade_image *out,
                            struct upgrade_report *report) {
    mo_upgrade_image_buffers_init(out);
    struct mo_buffer wire;
    mo_buffer_init(&wire, MO_LIMIT_BYTECODE_SIZE);
    bool ok = true;
    report->compilation = status;
    if (status != MO_COMPILE_OK) {
        FILE *errors = fopen(job->listing ? job->list_diagnostics : job->diagnostics, "rb");
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
        ok = mo_upgrade_refuse(report, code, status == MO_COMPILE_BUDGET ? "budget" : "compile",
                               report->detail[0] != 0
                                   ? report->detail
                                   : "compiler child failed or exceeded its budget");
        goto done;
    }
    struct mo_load_error error;
    ok = mo_upgrade_file_bytes(job->image, MO_LIMIT_BYTECODE_SIZE, &wire);
    out->image = ok ? mo_load(wire.bytes, wire.length, &error) : NULL;
    if (out->image == NULL) {
        ok = mo_upgrade_refuse(report, MO_CODE_UPGRADE_COMPILE_DIAGNOSTIC, "compile",
                               "compiler output failed verification");
        goto done;
    }
    struct mo_compile_context checked_context;
    if (!mo_upgrade_file_bytes(job->output_context, MO_LIMIT_BUNDLE_BYTES, &out->context) ||
        !mo_compile_context_read(out->context.bytes, out->context.length, &checked_context)) {
        ok = mo_upgrade_refuse(report, MO_CODE_UPGRADE_COMPILE_DIAGNOSTIC, "compile",
                               "compiler context failed validation");
        goto done;
    }
    struct mo_intent_list items;
    if (!mo_upgrade_file_bytes(job->items, MO_LIMIT_INTENT_LIST_BYTES, &out->items) ||
        !mo_intent_list_read(out->items.bytes, out->items.length, &items)) {
        ok = mo_upgrade_refuse(report, MO_CODE_UPGRADE_COMPILE_DIAGNOSTIC, "compile",
                               "the candidate's intent item list failed validation");
        goto done;
    }
    out->program.items = out->items.bytes;
    out->program.items_size = out->items.length;
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
        ok = mo_upgrade_refuse(report, MO_CODE_UPGRADE_COMPILE_DIAGNOSTIC, "compile",
                               "entry module declares no service");
        goto done;
    }
    struct mo_limits limits = mo_service_limits();
    limits.heap_size = MO_LIMIT_UPGRADE_RESTART_HEAP;
    struct mo_defect defect;
    if (mo_image_init_bounded(out->image, &limits, MO_LIMIT_UPGRADE_RESTART_WORK, &defect) !=
        MO_RETURNED) {
        ok = mo_upgrade_refuse(report, MO_CODE_UPGRADE_COMPILE_DIAGNOSTIC, "compile",
                               "candidate initialization failed");
        goto done;
    }
    struct mo_process *process = mo_process_new(out->image, &limits);
    ok = process != NULL &&
         mo_type_schema_digest(mo_process_heap(process), file->services[out->service].state_type,
                               out->program.schema);
    mo_process_free(process);
    if (!ok) {
        (void)mo_upgrade_refuse(report, MO_CODE_UPGRADE_COMPILE_DIAGNOSTIC, "compile",
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
        (void)mo_upgrade_refuse(report, MO_CODE_UPGRADE_COMPILE_DIAGNOSTIC, "compile",
                                "build identity is unavailable");
done:
    if (!ok) {
        if (out->image != NULL)
            mo_image_free(out->image);
        out->image = NULL;
    }
    mo_buffer_free(&wire);
    mo_upgrade_compile_cleanup(job);
    return ok;
}
bool mo_upgrade_compile_image(const struct mo_buffer *bundle, const struct mo_buffer *old,
                              const char *migration, const struct mo_buffer *context,
                              struct upgrade_image *out, struct upgrade_report *report) {
    struct compile_job job = {0};
    if (!mo_upgrade_compile_begin("/proc/self/exe", bundle, old, migration, context, &job,
                                  report)) {
        mo_upgrade_image_buffers_init(out);
        return false;
    }
    enum mo_compile_status status = MO_COMPILE_RUNNING;
    while (status == MO_COMPILE_RUNNING) {
        status = mo_upgrade_compile_poll(&job);
        if (status == MO_COMPILE_RUNNING) {
            struct timespec delay = {0, 10000000};
            (void)nanosleep(&delay, NULL);
        }
    }
    return mo_upgrade_compile_end(&job, status, bundle, out, report);
}
bool mo_upgrade_program_metadata(const struct mo_store_program *program,
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
    if (!mo_upgrade_program_metadata(&old->program, &a) ||
        !mo_upgrade_program_metadata(&next->program, &b))
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
bool mo_upgrade_compare_programs(const struct upgrade_image *old, const struct upgrade_image *next,
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
        return mo_upgrade_refuse(report, MO_CODE_UPGRADE_COMPILE_BUDGET, "budget",
                                 "descriptor comparison exceeded its work or report bound");
    struct mo_compile_metadata a, b;
    if (!mo_upgrade_program_metadata(&old->program, &a) ||
        !mo_upgrade_program_metadata(&next->program, &b))
        return mo_upgrade_refuse(report, MO_CODE_UPGRADE_INVALID_VALUE, "compile",
                                 "recorded descriptor is invalid");
    if (span_order(a.token, b.token) != 0) {
        int n =
            snprintf(report->detail, sizeof report->detail,
                     "token from env changed from %.*s to %.*s", (int)a.token.length,
                     (const char *)a.token.bytes, (int)b.token.length, (const char *)b.token.bytes);
        MO_ASSERT(n > 0);
        return mo_upgrade_refuse(report, MO_CODE_UPGRADE_CHANGED_ACCESS, "access", report->detail);
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
        return mo_upgrade_refuse(report, MO_CODE_UPGRADE_MISSING_MIGRATION, "migration",
                                 ok ? report->detail
                                    : "state schema changed; a migration is required");
    }
    report->class = "compatible";
    report->message = migration == NULL ? "state schema unchanged; candidate is compatible"
                                        : "migration signature checked; candidate is compatible";
    return true;
}
static int hex_digit(uint8_t c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}
bool mo_upgrade_migration_state(const struct upgrade_image *old, const struct upgrade_image *next,
                                const char *migration, const uint8_t *bytes, size_t length,
                                struct mo_buffer *out, struct upgrade_report *report,
                                bool rehearsal) {
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
        PAUSE_START(identity);
        ok = mo_value_decode_budget(mo_process_heap(process), result_type, bytes, length, NULL, 0,
                                    &work, &value);
        PAUSE_END(identity, "identity_decode", ok ? "ok_no_migration" : "refused");
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
            if (!mo_upgrade_program_metadata(&old->program, &metadata))
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
            PAUSE_START(decode);
            bool decoded =
                mo_value_decode_budget(mo_process_heap(process), function->parameters[0].type,
                                       bytes, length, names, types->count, &work, &argument);
            PAUSE_END(decode, "old_decode", decoded ? "ok" : "refused");
            if (decoded) {
                selected = i;
                break;
            }
        }
        ok = ok && selected != UINT32_MAX;
        if (ok) {
            struct mo_defect defect;
            mo_process_instruction_limit(process, work);
            uint64_t before_call = work;
            PAUSE_START(call);
            enum mo_outcome result = mo_call(process, selected, &argument, 1, &value, &defect);
            PAUSE_END(call, "migration_call", result == MO_RETURNED ? "ok" : "refused");
            if (file->functions[selected].parameters[0].ownership == MO_PARAMETER_BORROWED)
                mo_value_drop(mo_process_heap(process),
                              file->functions[selected].parameters[0].type, argument);
            work = mo_process_instructions_remaining(process);
            vm_work += before_call - work;
            ok = result == MO_RETURNED;
            owned = ok;
            if (!ok)
                (void)mo_upgrade_refuse(report,
                                        (work == 0 || defect.memory || defect.limit_name != NULL)
                                            ? MO_CODE_UPGRADE_MIGRATION_BUDGET
                                            : MO_CODE_UPGRADE_MIGRATION_TRAP,
                                        (work == 0 || defect.memory || defect.limit_name != NULL)
                                            ? "budget"
                                            : "migration",
                                        "migration did not return");
        }
    }
    if (ok) {
        PAUSE_START(encode);
        ok = mo_value_encode_budget(mo_process_heap(process), result_type, value, out, &work);
        PAUSE_END(encode, "output_encode", ok ? "ok" : "refused");
    }
    if (owned)
        mo_value_drop(mo_process_heap(process), result_type, value);
    if (ok) {
        PAUSE_START(validation);
        struct mo_cell verified;
        ok = mo_value_decode_budget(mo_process_heap(process), result_type, out->bytes, out->length,
                                    NULL, 0, &work, &verified);
        PAUSE_END(validation, "validation_decode", ok ? "ok" : "refused");
        if (ok)
            mo_value_drop(mo_process_heap(process), result_type, verified);
    }
    /* All timer expressions run with the same remaining migration instruction budget. */
    bool timers_started = ok;
    if (ok)
        mo_process_instruction_limit(process, work);
    PAUSE_START(timers);
    uint64_t before_timers = work;
    for (uint32_t i = 0; ok && i < service->timer_count; i++) {
        struct mo_cell interval;
        struct mo_defect defect;
        enum mo_outcome result =
            mo_call(process, service->timers[i].interval, NULL, 0, &interval, &defect);
        ok = result == MO_RETURNED && mo_cell_get_duration(interval) > 0;
        if (!ok)
            (void)mo_upgrade_refuse(report, MO_CODE_UPGRADE_TIMER_INTERVAL, "timer",
                                    "candidate timer interval is invalid or exceeded its budget");
    }
    PAUSE_END(timers, "timer_validation", !timers_started ? "not_run" : ok ? "ok" : "refused");
    if (timers_started) {
        work = mo_process_instructions_remaining(process);
        vm_work += before_timers - work;
    }
    if (!ok && report->code == 0)
        (void)mo_upgrade_refuse(
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
