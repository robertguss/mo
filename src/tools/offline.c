#define _POSIX_C_SOURCE 200809L
/* mo replay and mo query: a retained failure, or an exported state, run offline. Every
 * compatibility check runs, in a fixed order, before any value is decoded or any handler runs
 * (introspection design 7.4); a refusal names what differs and both values. */
#include "tools/offline.h"

#include "base/codes.h"
#include "base/runtime-identity.h"
#include "check/check.h"
#include "codegen/codegen.h"
#include "lower/lower.h"
#include "tools/run.h"
#include "vm/vm.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* What an offline command reads: a failure record and its pre-call state, or a state export. */
struct input {
    struct mo_arena arena; /* the artifact's bytes, confidential */
    struct mo_store_history_pair pair;
    bool has_pair, has_record;
    struct mo_export_artifact artifact;
    struct mo_failure_record record;
    const uint8_t *state;
    size_t state_length;
    const uint8_t *schema, *build;
    struct mo_failure_slice label;
    /* From an artifact, the plain program checksum and the service; from a folder, the
     * record's image key, which folds the service in. */
    uint64_t program;
    uint32_t service;
    bool image_key;
};

/* One refusal: the check that failed, what differs, and the recorded and found values. */
struct refusal {
    const char *check;
    struct mo_buffer message, recorded, found;
};

struct offline {
    const struct mo_cli *cli;
    FILE *output, *errors;
    struct mo_buffer out; /* one record or report, flushed whole */
    struct mo_offline_renderer renderer;
    bool rendering;
    struct refusal refusal;
    bool refused;
    const char *command; /* "mo replay" or "mo query" */
    const char *library;
};

static void refusal_init(struct refusal *r) {
    r->check = NULL;
    mo_buffer_init(&r->message, MO_CLI_DIAGNOSTIC_SIZE);
    mo_buffer_init(&r->recorded, MO_CLI_DIAGNOSTIC_SIZE);
    mo_buffer_init(&r->found, MO_CLI_DIAGNOSTIC_SIZE);
}
static void refusal_free(struct refusal *r) {
    mo_buffer_free(&r->message);
    mo_buffer_free(&r->recorded);
    mo_buffer_free(&r->found);
}

/* Records a refusal. recorded and found may be NULL; each is text already safe to show. */
static bool refuse(struct offline *o, const char *check, const char *message, const char *recorded,
                   const char *found) {
    MO_ASSERT(!o->refused);
    o->refused = true;
    o->refusal.check = check;
    bool ok = mo_buffer_append_text(&o->refusal.message, message);
    if (ok && recorded != NULL)
        ok = mo_buffer_append_text(&o->refusal.recorded, recorded);
    if (ok && found != NULL)
        ok = mo_buffer_append_text(&o->refusal.found, found);
    MO_ASSERT(ok);
    return false;
}

static bool hex_value(struct mo_buffer *out, const uint8_t *digest) {
    mo_buffer_clear(out);
    return mo_offline_hex(out, digest, 32);
}

static bool refuse_digest(struct offline *o, const char *check, const char *message,
                          const uint8_t *recorded, const uint8_t *found) {
    (void)refuse(o, check, message, NULL, NULL);
    bool ok = hex_value(&o->refusal.recorded, recorded) && hex_value(&o->refusal.found, found);
    MO_ASSERT(ok);
    return false;
}

/* Writes the refusal: one line on stderr as text, or the outcome record as JSON. */
static bool write_refusal_text(struct offline *o) {
    struct mo_buffer *b = &o->out;
    struct refusal *r = &o->refusal;
    bool ok = mo_buffer_append_text(b, o->command) && mo_buffer_append_text(b, ": refused: ") &&
              mo_buffer_append_text(b, r->check) && mo_buffer_append_text(b, ": ") &&
              mo_buffer_append(b, r->message.bytes, r->message.length);
    if (ok && r->recorded.length != 0)
        ok = mo_buffer_append_text(b, "; recorded ") &&
             mo_buffer_append(b, r->recorded.bytes, r->recorded.length);
    if (ok && r->found.length != 0)
        ok = mo_buffer_append_text(b, strcmp(r->check, "failure") == 0 ? "; retained ids "
                                                                       : "; found ") &&
             mo_buffer_append(b, r->found.bytes, r->found.length);
    return ok && mo_buffer_append_byte(b, '\n') && mo_offline_flush(o->errors, b);
}

static bool refusal_json(struct mo_buffer *b, const struct refusal *r) {
    return mo_buffer_append_text(b, "{\"check\":\"") && mo_buffer_append_text(b, r->check) &&
           mo_buffer_append_text(b, "\",\"message\":") &&
           mo_render_json_text(b, r->message.bytes, r->message.length) &&
           mo_buffer_append_text(b, ",\"recorded\":") &&
           (r->recorded.length == 0
                ? mo_buffer_append_text(b, "null")
                : mo_render_json_text(b, r->recorded.bytes, r->recorded.length)) &&
           mo_buffer_append_text(b, ",\"found\":") &&
           (r->found.length == 0 ? mo_buffer_append_text(b, "null")
                                 : mo_render_json_text(b, r->found.bytes, r->found.length)) &&
           mo_buffer_append_byte(b, '}');
}

/* The whole artifact, read into a confidential arena and bounded by MO_LIMIT_EXPORT_BYTES. */
static bool read_artifact(struct offline *o, struct input *in) {
    const char *path = o->cli->artifact;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOCTTY);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        int reason = fd < 0 || errno != 0 ? errno : EINVAL;
        if (fd >= 0)
            (void)close(fd);
        return refuse(o, "format", "the artifact cannot be read", NULL,
                      strerror(reason != 0 ? reason : EINVAL));
    }
    if (st.st_size < 0 || (uint64_t)st.st_size > MO_LIMIT_EXPORT_BYTES) {
        (void)close(fd);
        return refuse(o, "format", "the artifact is larger than any Mo export", NULL, NULL);
    }
    size_t size = (size_t)st.st_size;
    uint8_t *bytes = mo_arena_allocate(&in->arena, size == 0 ? 1 : size, 1);
    size_t read_bytes = 0;
    while (bytes != NULL && read_bytes < size) {
        ssize_t n = read(fd, bytes + read_bytes, size - read_bytes);
        if (n > 0)
            read_bytes += (size_t)n;
        else if (n == 0 || errno != EINTR)
            break;
    }
    bool closed = close(fd) == 0;
    if (bytes == NULL || read_bytes != size || !closed)
        return refuse(o, "format", "the artifact cannot be read", NULL, NULL);
    if (!mo_export_read(bytes, size, &in->artifact))
        return refuse(o, "format",
                      "the artifact is not a complete Mo export: its format, a frame or its digest "
                      "failed its check",
                      NULL, NULL);
    const struct mo_export_identity *id = &in->artifact.identity;
    bool failure = id->kind == 2;
    if (o->cli->failure != 0 && !failure)
        return refuse(o, "kind",
                      "the artifact holds an exported state, not a failure; fetch one with mo "
                      "fetch URL --export failure ID",
                      "state", "failure");
    if (o->cli->failure == 0 && failure)
        return refuse(o, "kind",
                      "the artifact holds a failure; give --failure ID to query its inputs",
                      "failure", "state");
    if (failure && id->failure_id != o->cli->failure) {
        char recorded[24], wanted[24];
        int a = snprintf(recorded, sizeof recorded, "%llu", (unsigned long long)id->failure_id);
        int b = snprintf(wanted, sizeof wanted, "%llu", (unsigned long long)o->cli->failure);
        MO_ASSERT(a > 0 && b > 0);
        return refuse(o, "failure", "the artifact holds another failure", recorded, wanted);
    }
    in->state = in->artifact.state.bytes;
    in->state_length = in->artifact.state.length;
    in->schema = in->artifact.schema;
    in->build = id->build;
    in->label = in->artifact.label;
    in->program = id->program;
    in->service = id->service;
    in->image_key = false;
    if (failure) {
        in->record = in->artifact.failure;
        in->has_record = true;
    }
    return true;
}

static bool read_folder(struct offline *o, struct input *in) {
    char path[MO_LIMIT_PATH_SIZE + sizeof "/" MO_DATA_FILE];
    if (!mo_offline_store_path(o->cli->data, path, sizeof path))
        return refuse(o, "format", "the data folder's path is too long", NULL, NULL);
    enum mo_store_folder status =
        mo_store_history_pair(path, o->cli->recover, o->cli->failure, &in->pair);
    in->has_pair = true;
    if (status != MO_STORE_FOLDER_OK) {
        struct mo_buffer message;
        mo_buffer_init(&message, MO_CLI_DIAGNOSTIC_SIZE);
        bool ok = mo_offline_folder_message(&message, o->cli->data, status, 0) &&
                  mo_buffer_append_byte(&message, 0);
        (void)refuse(o, "folder", ok ? (const char *)message.bytes : "the folder cannot be read",
                     NULL, NULL);
        mo_buffer_free(&message);
        return false;
    }
    if (!in->pair.failure_tables)
        return refuse(o, "folder",
                      "this store predates failure capture, so it holds no failure records", NULL,
                      NULL);
    if (!in->pair.found) {
        (void)refuse(o, "failure",
                     "the folder holds no failure record with that id; ids are never reused, so "
                     "a missing one was replaced or evicted",
                     NULL, NULL);
        bool ok = true;
        for (uint32_t i = 0; ok && i < in->pair.retained_count; i++)
            ok = (i == 0 || mo_buffer_append_text(&o->refusal.found, ", ")) &&
                 mo_buffer_append_uint(&o->refusal.found, in->pair.retained[i]);
        if (ok && in->pair.retained_count == 0)
            ok = mo_buffer_append_text(&o->refusal.found, "none");
        MO_ASSERT(ok);
        return false;
    }
    if (!mo_failure_read(in->pair.message.bytes, in->pair.message.length, &in->record) ||
        in->record.id != o->cli->failure)
        return refuse(o, "format", "the failure record's message does not decode", NULL, NULL);
    in->has_record = true;
    in->state = in->pair.state.bytes;
    in->state_length = in->pair.state.length;
    in->schema = in->record.schema;
    in->build = in->record.build;
    in->label = in->record.label;
    in->program = in->record.program;
    in->image_key = true;
    return true;
}

static bool read_input(struct offline *o, struct input *in) {
    return o->cli->artifact != NULL ? read_artifact(o, in) : read_folder(o, in);
}

static void input_free(struct input *in) {
    if (in->has_pair)
        mo_store_history_pair_free(&in->pair);
    mo_arena_free(&in->arena);
}

static bool check_build(struct offline *o, const struct input *in) {
    uint8_t mine[32];
    switch (mo_offline_build(in->build, in->label, mine)) {
    case MO_OFFLINE_BUILD_SAME:
        return true;
    case MO_OFFLINE_BUILD_UNNAMED:
        return refuse(o, "build",
                      "the record does not identify the build that recorded it, so no build can "
                      "replay it; it is evidence only",
                      NULL, NULL);
    case MO_OFFLINE_BUILD_UNKNOWN:
        return refuse(o, "build", "this build cannot read its own identity", NULL, NULL);
    case MO_OFFLINE_BUILD_OTHER:
        break;
    }
    (void)refuse(o, "build",
                 "recorded by another build; run the mo that recorded it (sha256sum finds it among "
                 "the deployed executables)",
                 NULL, NULL);
    const char *label = mo_runtime_build_label();
    bool ok = mo_render_terminal_text(&o->refusal.recorded, in->label.bytes, in->label.length) &&
              mo_buffer_append_text(&o->refusal.recorded, " (sha256 ") &&
              mo_offline_hex(&o->refusal.recorded, in->build, 32) &&
              mo_buffer_append_text(&o->refusal.recorded, ")") &&
              mo_render_terminal_text(&o->refusal.found, label, strlen(label)) &&
              mo_buffer_append_text(&o->refusal.found, " (sha256 ") &&
              mo_offline_hex(&o->refusal.found, mine, 32) &&
              mo_buffer_append_text(&o->refusal.found, ")");
    MO_ASSERT(ok);
    return false;
}

static bool check_program(struct offline *o, const struct input *in, const struct mo_image *image,
                          uint32_t service) {
    uint64_t found = mo_image_key(image, service);
    uint64_t recorded = in->image_key ? in->program : in->program ^ ((uint64_t)in->service << 32);
    if (recorded == found)
        return true;
    (void)refuse(o, "program",
                 "FILE is not the program that recorded the failure; it must be the untouched "
                 "source, since any byte change in any module, a comment or mo format included, "
                 "changes the program identity",
                 NULL, NULL);
    bool ok = mo_buffer_append_hex(&o->refusal.recorded, recorded, 16) &&
              mo_buffer_append_hex(&o->refusal.found, found, 16);
    MO_ASSERT(ok);
    return false;
}

/* The service FILE's main module declares, or MO_NO_INDEX. */
static uint32_t main_service(const struct mo_program *program) {
    uint32_t service = MO_NO_INDEX;
    for (uint32_t i = 0; i < program->service_count; i++)
        if (program->services[i].module == 0)
            service = i;
    return service;
}

static struct mo_failure_slice handler_name(const struct mo_bytecode *file,
                                            const struct mo_bytecode_service *service, bool timer,
                                            uint32_t handler) {
    uint32_t function =
        timer ? service->timers[handler].function : service->tools[handler].function;
    const struct mo_bytecode_string *name = &file->strings[file->functions[function].name];
    return mo_offline_bare_name(name->bytes, name->length);
}

/* What a handler is: its name, whether a timer, a read-only or an updating tool, and how many
 * arguments it takes. */
static bool describe(struct mo_buffer *out, struct mo_failure_slice name, bool timer,
                     bool read_only, uint32_t index, uint32_t arguments) {
    return mo_render_terminal_text(out, name.bytes, name.length) &&
           mo_buffer_append_text(out, timer       ? " (timer "
                                      : read_only ? " (read-only tool "
                                                  : " (updating tool ") &&
           mo_buffer_append_uint(out, index) && mo_buffer_append_text(out, ", ") &&
           mo_buffer_append_uint(out, arguments) &&
           mo_buffer_append_text(out, arguments == 1 ? " argument)" : " arguments)");
}

static bool check_handler(struct offline *o, const struct mo_failure_record *r,
                          const struct mo_image *image, const struct mo_bytecode_service *service) {
    const struct mo_bytecode *file = mo_image_bytecode(image);
    bool timer = r->timer != 0;
    uint32_t count = timer ? service->timer_count : service->tool_count;
    bool same = r->handler < count;
    struct mo_failure_slice name = {0};
    bool read_only = false;
    uint32_t arguments = 0;
    if (same) {
        name = handler_name(file, service, timer, r->handler);
        read_only = !timer && service->tools[r->handler].read_only;
        arguments = timer ? 0 : service->tools[r->handler].argument_count;
        same = mo_failure_slice_equal(name, r->name) && read_only == (r->read_only != 0) &&
               arguments == r->argument_count;
    }
    if (same)
        return true;
    (void)refuse(o, "handler", "FILE's service has no handler like the recorded one", NULL, NULL);
    bool ok = describe(&o->refusal.recorded, r->name, timer, r->read_only != 0, r->handler,
                       r->argument_count) &&
              (r->handler < count
                   ? describe(&o->refusal.found, name, timer, read_only, r->handler, arguments)
                   : mo_buffer_append_text(&o->refusal.found, "no such handler"));
    MO_ASSERT(ok);
    return false;
}

static bool check_schema(struct offline *o, struct mo_heap *heap, uint32_t type,
                         const uint8_t *recorded, const char *check, const char *message) {
    uint8_t found[32];
    if (!mo_type_schema_digest(heap, type, found))
        return refuse(o, check, "the type has no saved form", NULL, NULL);
    return memcmp(found, recorded, 32) == 0 || refuse_digest(o, check, message, recorded, found);
}

/* Each recorded argument's schema against the types given, in order. */
static bool check_arguments(struct offline *o, struct mo_heap *heap,
                            const struct mo_failure_record *r, const uint32_t *types,
                            uint32_t count, const char *check) {
    struct mo_failure_arguments walk = mo_failure_arguments(r);
    struct mo_failure_slice bytes;
    for (uint32_t i = 0; i < count; i++) {
        uint8_t recorded[32];
        if (mo_failure_argument_next(&walk, &bytes) != MO_FAILURE_ARGUMENT ||
            !mo_export_schema(bytes.bytes, bytes.length, recorded))
            return refuse(o, check, "the record holds fewer arguments than the handler takes", NULL,
                          NULL);
        static const char *messages[] = {
            "argument 1's type differs from the recorded one",
            "argument 2's type differs from the recorded one",
            "argument 3's type differs from the recorded one",
        };
        if (!check_schema(o, heap, types[i], recorded, check,
                          i < 3 ? messages[i] : "an argument's type differs from the recorded one"))
            return false;
    }
    return mo_failure_argument_next(&walk, &bytes) == MO_FAILURE_ARGUMENTS_END ||
           refuse(o, check, "the record holds more arguments than the handler takes", NULL, NULL);
}

static bool check_budgets(struct offline *o, const struct mo_failure_record *r) {
    struct mo_service_budget differs;
    if (mo_service_budgets_match(r, &differs))
        return true;
    (void)refuse(o, "budgets",
                 "the record's budgets differ from this build's, so it is damaged or crafted; "
                 "replay never runs with budgets read from a file",
                 NULL, NULL);
    bool ok = mo_buffer_append_text(&o->refusal.recorded, differs.name) &&
              mo_buffer_append_text(&o->refusal.recorded, " = ") &&
              mo_buffer_append_uint(&o->refusal.recorded, differs.recorded) &&
              mo_buffer_append_text(&o->refusal.found, differs.name) &&
              mo_buffer_append_text(&o->refusal.found, " = ") &&
              mo_buffer_append_uint(&o->refusal.found, differs.expected);
    MO_ASSERT(ok);
    return false;
}

struct revision {
    struct mo_arena arena, lowered;
    struct mo_sources sources;
    struct mo_diags diags;
    struct mo_buffer wire;
    struct mo_image *image;
    struct mo_process *process; /* for the revision's own type schemas */
    uint32_t service;
};

static void revision_free(struct revision *r) {
    mo_process_free(r->process);
    if (r->image != NULL)
        mo_image_free(r->image);
    mo_buffer_free(&r->wire);
    mo_arena_free(&r->lowered);
    mo_arena_free(&r->arena);
    mo_sources_free(&r->sources);
    mo_diags_free(&r->diags);
}

/* REVISED, compiled as FILE is, with its sibling modules from its own folder. Its diagnostics
 * go to stderr as text: a revision that does not compile is refused, and mo check REVISED is
 * the command that reports it. */
static bool compile_revision(struct offline *o, struct revision *r) {
    *r = (struct revision){.service = MO_NO_INDEX};
    bool have_sources = mo_sources_init(&r->sources), have_diags = mo_diags_init(&r->diags);
    mo_arena_init(&r->arena, MO_CLI_CHECK_SIZE);
    mo_arena_init(&r->lowered, MO_LIMIT_LOWER_WORK);
    mo_buffer_init(&r->wire, MO_LIMIT_BYTECODE_SIZE);
    if (!have_sources || !have_diags)
        return refuse(o, "revised_program", "out of memory compiling REVISED", NULL, NULL);
    struct mo_module_set *set =
        mo_load_program(&r->arena, &r->sources, o->cli->revised, o->library, &r->diags);
    struct mo_program *program =
        set == NULL || mo_diags_any(&r->diags) ? NULL : mo_check(&r->arena, set, &r->diags);
    if (program != NULL)
        r->service = main_service(program);
    struct mo_services services;
    struct mo_core *core =
        program == NULL || mo_diags_any(&r->diags)
            ? NULL
            : mo_lower_services(&r->lowered, program, &r->sources, &r->diags, NULL, &services);
    bool ok = core != NULL && !mo_diags_any(&r->diags) &&
              mo_core_check_services(core, &services, &r->diags) &&
              mo_compile_services(core, &services, &r->wire, &r->diags);
    if (!ok || mo_diags_any(&r->diags)) {
        int written = mo_write_diagnostics(o->output, o->errors, false, MO_COMMAND_CHECK,
                                           &r->sources, &r->diags, NULL);
        (void)written;
        return refuse(o, "revised_program", "REVISED does not compile; mo check REVISED shows why",
                      NULL, NULL);
    }
    if (r->service == MO_NO_INDEX)
        return refuse(o, "revised_program", "REVISED's main module declares no service", NULL,
                      NULL);
    struct mo_load_error error;
    r->image = mo_load(r->wire.bytes, r->wire.length, &error);
    struct mo_limits limits = mo_limits_default();
    struct mo_defect defect;
    if (r->image == NULL || mo_image_init(r->image, &limits, &defect) != MO_RETURNED)
        return refuse(o, "revised_program", "REVISED's compiled program could not start", NULL,
                      NULL);
    struct mo_limits service = mo_service_limits();
    r->process = mo_process_new(r->image, &service);
    if (r->process == NULL)
        return refuse(o, "revised_program", "out of memory for REVISED's checks", NULL, NULL);
    return true;
}

/* The revision's handler: same name and kind; same argument types in order; the same result
 * shape class (timer, read-only tool, updating tool). */
static bool check_revision(struct offline *o, const struct mo_failure_record *rec,
                           struct revision *r, const uint8_t *schema, uint32_t *handler) {
    const struct mo_bytecode *file = mo_image_bytecode(r->image);
    struct mo_heap *heap = mo_process_heap(r->process);
    const struct mo_bytecode_service *service = &file->services[r->service];
    if (!check_schema(o, heap, service->state_type, schema, "revised_state_schema",
                      "the fix changes the state type, so its saved state could not be read"))
        return false;
    bool timer = rec->timer != 0;
    uint32_t count = timer ? service->timer_count : service->tool_count;
    *handler = MO_NO_INDEX;
    for (uint32_t i = 0; i < count; i++)
        if (mo_failure_slice_equal(handler_name(file, service, timer, i), rec->name))
            *handler = i;
    if (*handler == MO_NO_INDEX) {
        (void)refuse(o, "revised_handler",
                     timer ? "REVISED's service has no timer of the recorded handler's name"
                           : "REVISED's service has no tool of the recorded handler's name",
                     NULL, "none");
        bool ok = describe(&o->refusal.recorded, rec->name, timer, rec->read_only != 0,
                           rec->handler, rec->argument_count);
        MO_ASSERT(ok);
        return false;
    }
    bool read_only = !timer && service->tools[*handler].read_only;
    uint32_t arguments = timer ? 0 : service->tools[*handler].argument_count;
    if (read_only != (rec->read_only != 0) || arguments != rec->argument_count) {
        (void)refuse(o, "revised_handler",
                     "the revised handler's shape differs: a fix keeps the handler's kind, its "
                     "result shape class and its arguments",
                     NULL, NULL);
        bool ok = describe(&o->refusal.recorded, rec->name, timer, rec->read_only != 0,
                           rec->handler, rec->argument_count) &&
                  describe(&o->refusal.found, rec->name, timer, read_only, *handler, arguments);
        MO_ASSERT(ok);
        return false;
    }
    return timer || check_arguments(o, heap, rec, service->tools[*handler].argument_types,
                                    arguments, "revised_handler");
}

static bool put(struct offline *o, const char *text) {
    return mo_buffer_append_text(&o->out, text);
}

static const char *context_status(enum mo_failure_context status) {
    switch (status) {
    case MO_FAILURE_CONTEXT_COMPLETE:
        return "complete";
    case MO_FAILURE_CONTEXT_TRUNCATED:
        return "truncated";
    case MO_FAILURE_CONTEXT_UNAVAILABLE:
        return "unavailable";
    }
    MO_UNREACHABLE();
}

static const char *outcome_status(enum mo_outcome_status status) {
    switch (status) {
    case MO_OUTCOME_SUCCEEDED:
        return "succeeded";
    case MO_OUTCOME_DEFECT:
        return "defect";
    case MO_OUTCOME_WORK:
        return "work";
    case MO_OUTCOME_MEMORY:
        return "memory";
    case MO_OUTCOME_STATE_SIZE:
        return "state_size";
    case MO_OUTCOME_REFUSED:
        return "refused";
    }
    MO_UNREACHABLE();
}

static const char *replay_class(enum mo_replay_class c) {
    switch (c) {
    case MO_REPLAY_REPRODUCED:
        return "reproduced";
    case MO_REPLAY_DIFFERENT:
        return "different";
    case MO_REPLAY_NOT_REPRODUCED:
        return "not_reproduced";
    case MO_REPLAY_REFUSED:
        return "refused";
    }
    MO_UNREACHABLE();
}

static const char *verdict_name(enum mo_verdict v) {
    switch (v) {
    case MO_VERDICT_VERIFIED:
        return "verified";
    case MO_VERDICT_NOT_FIXED:
        return "not_fixed";
    case MO_VERDICT_UNVERIFIED:
        return "unverified";
    case MO_VERDICT_REFUSED:
        return "refused";
    }
    MO_UNREACHABLE();
}

static const char memory_note[] =
    "live heap occupants, such as arguments queued in the mailbox, are not reconstructed offline, "
    "so a memory failure that depended on them does not recur";

/* A value as structural text, decoded into the process's heap from its saved bytes. */
static bool value_text(struct offline *o, struct mo_heap *heap, uint32_t type, const uint8_t *bytes,
                       size_t length) {
    struct mo_cell value;
    if (!mo_value_decode(heap, type, bytes, length, &value))
        return put(o, "<unavailable: does not decode>");
    struct mo_buffer printed;
    mo_buffer_init(&printed, MO_LIMIT_OFFLINE_VALUE_BYTES);
    bool shown = mo_value_print_literal(heap, type, value, &printed);
    mo_value_drop(heap, type, value);
    bool ok = shown ? mo_render_terminal_text(&o->out, printed.bytes, printed.length)
                    : put(o, "<unavailable: longer than MO_LIMIT_OFFLINE_VALUE_BYTES; --json "
                             "shows it truncated>");
    mo_buffer_free(&printed);
    return ok;
}

static bool value_json(struct offline *o, const uint8_t *bytes, size_t length) {
    return mo_offline_render(&o->renderer, bytes, length, &o->out);
}

static bool context_text(struct offline *o, const char *label, enum mo_failure_context status,
                         const uint8_t *text, size_t length) {
    bool ok = put(o, label) && put(o, " (") && put(o, context_status(status)) && put(o, "):\n");
    size_t at = 0;
    while (ok && at < length) {
        size_t end = at;
        while (end < length && text[end] != '\n')
            end++;
        ok = put(o, "  ") && mo_render_terminal_text(&o->out, text + at, end - at) && put(o, "\n");
        at = end + 1;
    }
    return ok;
}

struct run_report {
    struct mo_offline_run run;
    struct mo_buffer context;
};

static bool run_json(struct offline *o, const struct run_report *r) {
    const struct mo_run_outcome *x = &r->run.outcome;
    bool failed = x->status != MO_OUTCOME_SUCCEEDED && x->status != MO_OUTCOME_REFUSED;
    bool returned = x->status == MO_OUTCOME_SUCCEEDED || x->status == MO_OUTCOME_STATE_SIZE;
    bool ok = put(o, "{\"status\":\"") && put(o, outcome_status(x->status)) &&
              put(o, "\",\"signature\":") &&
              (failed ? mo_offline_signature_json(&o->out, 1, &x->signature) : put(o, "null")) &&
              put(o, ",\"reply\":") &&
              put(o, !returned                    ? "null"
                     : x->reply == MO_REPLY_ERROR ? "\"error\""
                                                  : "\"ok\"") &&
              put(o, ",\"state_changed\":") &&
              put(o, !returned          ? "null"
                     : x->state_changed ? "true"
                                        : "false") &&
              put(o, ",\"context_status\":");
    if (ok && failed)
        ok = put(o, "\"") && put(o, context_status(r->run.context)) && put(o, "\",\"context\":") &&
             mo_render_json_text(&o->out, r->context.bytes, r->context.length);
    else if (ok)
        ok = put(o, "null,\"context\":null");
    if (ok && x->status == MO_OUTCOME_REFUSED)
        ok = put(o, ",\"refusal\":") &&
             mo_render_json_text(&o->out, r->run.refusal, strlen(r->run.refusal));
    return ok && put(o, "}");
}

static bool run_text(struct offline *o, const char *label, const struct run_report *r) {
    const struct mo_run_outcome *x = &r->run.outcome;
    bool ok = put(o, label) && put(o, ": ");
    switch (x->status) {
    case MO_OUTCOME_SUCCEEDED:
        ok = ok && put(o, "succeeded, reply ") &&
             put(o, x->reply == MO_REPLY_ERROR ? "error" : "ok") &&
             put(o, x->state_changed ? ", state changed\n" : ", state unchanged\n");
        break;
    case MO_OUTCOME_REFUSED:
        ok = ok && put(o, "refused before it started: ") && put(o, r->run.refusal) && put(o, "\n");
        break;
    case MO_OUTCOME_DEFECT:
    case MO_OUTCOME_WORK:
    case MO_OUTCOME_MEMORY:
    case MO_OUTCOME_STATE_SIZE:
        ok = ok && mo_offline_signature_text(&o->out, &x->signature) && put(o, "\n") &&
             context_text(o, "stack", r->run.context, r->context.bytes, r->context.length);
        break;
    }
    return ok;
}

static void run_report_init(struct run_report *r) {
    mo_buffer_init(&r->context, MO_LIMIT_FAILURE_CONTEXT_BYTES);
}

/* The original message, its pre-call state and its recorded signature and context. */
static bool original_text(struct offline *o, const struct mo_failure_record *rec,
                          const struct input *in, struct mo_heap *heap,
                          const struct mo_bytecode_service *service) {
    bool timer = rec->timer != 0;
    bool ok = put(o, "replay of failure ") && mo_buffer_append_uint(&o->out, rec->id) &&
              put(o, ": ") &&
              describe(&o->out, rec->name, timer, rec->read_only != 0, rec->handler,
                       rec->argument_count) &&
              put(o, ", sequence ") && mo_buffer_append_uint(&o->out, rec->sequence) &&
              put(o, ", arrival ") && mo_buffer_append_int(&o->out, mo_failure_arrival(rec)) &&
              put(o, " us\nstate: ") &&
              value_text(o, heap, service->state_type, in->state, in->state_length) && put(o, "\n");
    struct mo_failure_arguments walk = mo_failure_arguments(rec);
    struct mo_failure_slice bytes;
    for (uint32_t i = 0; ok && !timer && i < rec->argument_count; i++)
        ok = mo_failure_argument_next(&walk, &bytes) == MO_FAILURE_ARGUMENT &&
             put(o, "argument ") && mo_buffer_append_uint(&o->out, i + 1) && put(o, ": ") &&
             value_text(o, heap, service->tools[rec->handler].argument_types[i], bytes.bytes,
                        bytes.length) &&
             put(o, "\n");
    struct mo_signature recorded = mo_signature_from_record(rec);
    return ok && put(o, "recorded: ") && mo_offline_signature_text(&o->out, &recorded) &&
           put(o, "\n") &&
           context_text(o, "recorded stack", (enum mo_failure_context)rec->context_status,
                        rec->context.bytes, rec->context.length);
}

static bool original_json(struct offline *o, const struct mo_failure_record *rec,
                          const struct input *in) {
    bool ok = put(o, "{\"record\":\"original\",\"failure\":\"") &&
              mo_buffer_append_uint(&o->out, rec->id) && put(o, "\",\"handler\":") &&
              mo_render_json_text(&o->out, rec->name.bytes, rec->name.length) &&
              put(o, ",\"handler_index\":") && mo_buffer_append_uint(&o->out, rec->handler) &&
              put(o, rec->timer ? ",\"timer\":true" : ",\"timer\":false") &&
              put(o, rec->read_only ? ",\"read_only\":true" : ",\"read_only\":false") &&
              put(o, ",\"sequence\":\"") && mo_buffer_append_uint(&o->out, rec->sequence) &&
              put(o, "\",\"arrival_us\":\"") &&
              mo_buffer_append_int(&o->out, mo_failure_arrival(rec)) && put(o, "\",\"state\":") &&
              value_json(o, in->state, in->state_length) && put(o, ",\"arguments\":[");
    struct mo_failure_arguments walk = mo_failure_arguments(rec);
    struct mo_failure_slice bytes;
    for (uint32_t i = 0; ok && i < rec->argument_count; i++)
        ok = mo_failure_argument_next(&walk, &bytes) == MO_FAILURE_ARGUMENT &&
             (i == 0 || put(o, ",")) && value_json(o, bytes.bytes, bytes.length);
    struct mo_signature recorded = mo_signature_from_record(rec);
    return ok && put(o, "],\"recorded\":{\"signature\":") &&
           mo_offline_signature_json(&o->out, rec->signature_version, &recorded) &&
           put(o, ",\"context_status\":\"") &&
           put(o, context_status((enum mo_failure_context)rec->context_status)) &&
           put(o, "\",\"context\":") &&
           mo_render_json_text(&o->out, rec->context.bytes, rec->context.length) && put(o, "}");
}

struct compiled {
    struct mo_image *image;
    uint32_t service;
};

static int replay(struct offline *o, const struct compiled *program) {
    const struct mo_cli *cli = o->cli;
    struct input in = {0};
    mo_arena_init_confidential(&in.arena, MO_LIMIT_EXPORT_BYTES + 4096);
    struct revision revision = {.service = MO_NO_INDEX};
    bool revising = cli->revised != NULL;
    const struct mo_bytecode *file = mo_image_bytecode(program->image);
    const struct mo_bytecode_service *service = NULL;
    struct mo_limits limits = mo_service_limits();
    struct mo_process *process = NULL;
    uint32_t revised_handler = MO_NO_INDEX;
    bool ok = read_input(o, &in) && check_build(o, &in);
    if (ok && program->service == MO_NO_INDEX)
        ok = refuse(o, "program", "FILE's main module declares no service", NULL, NULL);
    if (ok) {
        service = &file->services[program->service];
        ok = check_program(o, &in, program->image, program->service) &&
             check_handler(o, &in.record, program->image, service);
    }
    if (ok) {
        process = mo_process_new(program->image, &limits);
        ok = process != NULL || refuse(o, "run", "out of memory for the checks", NULL, NULL);
    }
    struct mo_heap *heap = process == NULL ? NULL : mo_process_heap(process);
    ok = ok &&
         check_schema(o, heap, service->state_type, in.schema, "state_schema",
                      "FILE's state type differs from the recorded state's") &&
         (in.record.timer != 0 ||
          check_arguments(o, heap, &in.record, service->tools[in.record.handler].argument_types,
                          in.record.argument_count, "argument_schema"));
    if (ok && revising)
        ok = compile_revision(o, &revision) &&
             check_revision(o, &in.record, &revision, in.schema, &revised_handler);
    ok = ok && check_budgets(o, &in.record);
    int status = 1;
    bool written = true;
    if (!ok) {
        if (cli->json) {
            written = put(o, "{\"record\":\"original\",\"failure\":\"") &&
                      mo_buffer_append_uint(&o->out, cli->failure) &&
                      put(o, "\",\"outcome\":\"refused\",\"refusal\":") &&
                      refusal_json(&o->out, &o->refusal) && put(o, "}\n") &&
                      (!revising || put(o, "{\"record\":\"revised\",\"verdict\":\"refused\"}\n")) &&
                      mo_offline_flush(o->output, &o->out);
        } else
            written = write_refusal_text(o);
        goto done;
    }
    struct run_report original, revised;
    run_report_init(&original);
    run_report_init(&revised);
    original.run = mo_offline_call(program->image, service, in.record.handler, in.state,
                                   in.state_length, &in.record, &original.context);
    struct mo_signature recorded = mo_signature_from_record(&in.record);
    enum mo_replay_class class = mo_replay_classify(&recorded, &original.run.outcome);
    enum mo_verdict verdict = MO_VERDICT_REFUSED;
    if (revising) {
        revised.run = mo_offline_call(
            revision.image, &mo_image_bytecode(revision.image)->services[revision.service],
            revised_handler, in.state, in.state_length, &in.record, &revised.context);
        verdict = mo_verdict(class, &revised.run.outcome);
    }
    bool note = class == MO_REPLAY_NOT_REPRODUCED && recorded.class == MO_SIGNATURE_MEMORY;
    if (cli->json) {
        written = original_json(o, &in.record, &in) && put(o, ",\"outcome\":\"") &&
                  put(o, replay_class(class)) && put(o, "\",\"run\":") && run_json(o, &original) &&
                  put(o, ",\"note\":") &&
                  (note ? mo_render_json_text(&o->out, memory_note, sizeof memory_note - 1)
                        : put(o, "null")) &&
                  put(o, "}\n");
        if (written && revising)
            written = put(o, "{\"record\":\"revised\",\"file\":") &&
                      mo_render_json_text(&o->out, cli->revised, strlen(cli->revised)) &&
                      put(o, ",\"handler\":") &&
                      mo_render_json_text(&o->out, in.record.name.bytes, in.record.name.length) &&
                      put(o, ",\"handler_index\":") &&
                      mo_buffer_append_uint(&o->out, revised_handler) && put(o, ",\"run\":") &&
                      run_json(o, &revised) && put(o, ",\"verdict\":\"") &&
                      put(o, verdict_name(verdict)) && put(o, "\"}\n");
    } else {
        written = original_text(o, &in.record, &in, heap, service) &&
                  run_text(o, "offline", &original) && put(o, "outcome: ") &&
                  put(o, replay_class(class)) && put(o, "\n") &&
                  (!note || (put(o, "note: ") && put(o, memory_note) && put(o, "\n")));
        if (written && revising)
            written = run_text(o, "revised", &revised) && put(o, "verdict: ") &&
                      put(o, verdict_name(verdict)) && put(o, "\n");
    }
    written = written && mo_offline_flush(o->output, &o->out);
    status = revising ? (verdict == MO_VERDICT_VERIFIED ? 0 : 1)
                      : (class == MO_REPLAY_REPRODUCED ? 0 : 1);
    mo_buffer_free(&original.context);
    mo_buffer_free(&revised.context);
done:
    mo_process_free(process);
    if (revising)
        revision_free(&revision);
    input_free(&in);
    if (!written) {
        (void)fprintf(o->errors, "%s: could not write the outcome\n", o->command);
        return 1;
    }
    return status;
}

struct query_work {
    uint64_t remaining;
};
static bool query_running(void *context) {
    struct query_work *w = context;
    if (w->remaining == 0)
        return false;
    w->remaining--;
    return true;
}

/* The result as text, or as JSON through its saved encoding. */
static bool result_output(struct offline *o, struct mo_heap *heap, uint32_t type,
                          struct mo_cell value) {
    if (!o->cli->json) {
        struct mo_buffer printed;
        mo_buffer_init(&printed, MO_LIMIT_OFFLINE_VALUE_BYTES);
        bool shown = mo_value_print(heap, type, value, &printed);
        bool ok = shown ? mo_render_terminal_text(&o->out, printed.bytes, printed.length)
                        : put(o, "<unavailable: longer than MO_LIMIT_OFFLINE_VALUE_BYTES; --json "
                                 "shows it truncated>");
        mo_buffer_free(&printed);
        return ok && put(o, "\n");
    }
    struct mo_buffer saved;
    mo_buffer_init(&saved, MO_LIMIT_SAVED_BYTES);
    bool ok = mo_value_encode(heap, type, value, &saved)
                  ? value_json(o, saved.bytes, saved.length)
                  : put(o, "{\"value\":null,\"complete\":false}");
    mo_buffer_free(&saved);
    return ok;
}

/* A refused query: its record as JSON, or one line on stderr. */
static bool write_query_refusal(struct offline *o) {
    if (!o->cli->json)
        return write_refusal_text(o);
    return put(o, "{\"record\":\"query\",\"function\":") &&
           mo_render_json_text(&o->out, o->cli->function, strlen(o->cli->function)) &&
           put(o, ",\"outcome\":\"refused\",\"refusal\":") && refusal_json(&o->out, &o->refusal) &&
           put(o, "}\n") && mo_offline_flush(o->output, &o->out);
}

static int stop_report(struct offline *o, struct mo_process *process, struct mo_defect *defect) {
    if (!mo_write_defect(o->errors, process, defect))
        (void)fputs("mo: could not write the defect report\n", o->errors);
    if (!o->cli->json)
        return 70;
    bool ok = put(o, "{\"record\":\"query\",\"function\":") &&
              mo_render_json_text(&o->out, o->cli->function, strlen(o->cli->function)) &&
              put(o, ",\"outcome\":\"stopped\",\"reason\":") &&
              mo_render_json_text(&o->out, defect->reason, strlen(defect->reason)) &&
              put(o, ",\"limit\":") &&
              (defect->limit_name == NULL ? put(o, "null")
                                          : mo_render_json_text(&o->out, defect->limit_name,
                                                                strlen(defect->limit_name))) &&
              put(o, "}\n") && mo_offline_flush(o->output, &o->out);
    if (!ok)
        (void)fprintf(o->errors, "%s: could not write the outcome\n", o->command);
    return 70;
}

static int query(struct offline *o, const struct compiled *program, uint32_t function,
                 struct input *in) {
    const struct mo_cli *cli = o->cli;
    const struct mo_bytecode *file = mo_image_bytecode(program->image);
    const struct mo_bytecode_function *fn = &file->functions[function];
    const struct mo_bytecode_service *service = &file->services[program->service];
    struct mo_process *process = mo_process_new(program->image, &cli->limits);
    if (process == NULL) {
        struct mo_defect defect = {.reason = "running out of memory",
                                   .limit_name = "--max-heap",
                                   .limit = cli->limits.heap_size,
                                   .span.file = MO_NO_INDEX};
        return stop_report(o, NULL, &defect);
    }
    struct mo_heap *heap = mo_process_heap(process);
    /* Format and kind were checked when the input was read; then the build, the state
     * schema and, over a failure, each argument's schema. */
    bool ok =
        check_build(o, in) && check_schema(o, heap, service->state_type, in->schema, "state_schema",
                                           "FILE's state type differs from the exported state's");
    if (ok && in->has_record) {
        uint32_t types[MO_LIMIT_BYTECODE_PARAMETERS];
        uint32_t count = fn->parameter_count - 2;
        for (uint32_t i = 0; i < count; i++)
            types[i] = fn->parameters[i + 2].type;
        ok = check_arguments(o, heap, &in->record, types, count, "argument_schema");
    }
    int status = 1;
    if (!ok) {
        if (!write_query_refusal(o))
            (void)fprintf(o->errors, "%s: could not write the outcome\n", o->command);
        mo_process_free(process);
        return 1;
    }
    struct mo_cell values[MO_LIMIT_BYTECODE_PARAMETERS];
    uint32_t decoded = 0;
    bool inputs =
        mo_value_decode(heap, fn->parameters[0].type, in->state, in->state_length, &values[0]);
    decoded += inputs ? 1 : 0;
    if (inputs && in->has_record) {
        values[decoded++] = mo_cell_time(mo_failure_arrival(&in->record));
        struct mo_failure_arguments walk = mo_failure_arguments(&in->record);
        struct mo_failure_slice bytes;
        for (uint32_t i = 2; inputs && i < fn->parameter_count; i++) {
            inputs = mo_failure_argument_next(&walk, &bytes) == MO_FAILURE_ARGUMENT &&
                     mo_value_decode(heap, fn->parameters[i].type, bytes.bytes, bytes.length,
                                     &values[i]);
            decoded += inputs ? 1 : 0;
        }
    }
    if (!inputs) {
        /* The schemas matched, so only the heap can refuse a decode. */
        struct mo_defect defect = {.reason = "running out of memory",
                                   .limit_name = "--max-heap",
                                   .limit = cli->limits.heap_size,
                                   .span.file = MO_NO_INDEX,
                                   .process = process};
        for (uint32_t i = 0; i < decoded; i++)
            mo_value_drop(heap, fn->parameters[i].type, values[i]);
        status = stop_report(o, process, &defect);
        mo_process_free(process);
        return status;
    }
    struct mo_cell arguments[MO_LIMIT_BYTECODE_PARAMETERS];
    for (uint32_t i = 0; i < fn->parameter_count; i++)
        arguments[i] = fn->parameters[i].ownership == MO_PARAMETER_OWNED
                           ? mo_value_dup(heap, fn->parameters[i].type, values[i])
                           : values[i];
    struct query_work work = {cli->max_work == UINT64_MAX ? UINT64_MAX : cli->max_work + 1};
    struct mo_cell result;
    struct mo_defect defect;
    enum mo_controlled_outcome outcome = mo_call_controlled(
        process, function, arguments, fn->parameter_count, query_running, &work, &result, &defect);
    if (outcome == MO_CONTROL_RETURNED) {
        bool written =
            cli->json ? put(o, "{\"record\":\"query\",\"function\":") &&
                            mo_render_json_text(&o->out, cli->function, strlen(cli->function)) &&
                            put(o, ",\"outcome\":\"returned\",\"result\":") &&
                            result_output(o, heap, fn->result_type, result) && put(o, "}\n")
                      : result_output(o, heap, fn->result_type, result);
        written = written && mo_offline_flush(o->output, &o->out);
        mo_value_drop(heap, fn->result_type, result);
        status = written ? 0 : 1;
        if (!written)
            (void)fprintf(o->errors, "%s: could not write the result\n", o->command);
    } else if (outcome == MO_CONTROL_CANCELLED) {
        struct mo_defect stop = {.reason = "running out of work",
                                 .limit_name = "--max-work",
                                 .limit =
                                     (size_t)(cli->max_work > SIZE_MAX ? SIZE_MAX : cli->max_work),
                                 .span.file = MO_NO_INDEX,
                                 .process = process};
        status = stop_report(o, process, &stop);
    } else
        status = stop_report(o, process, &defect);
    for (uint32_t i = 0; i < fn->parameter_count; i++)
        mo_value_drop(heap, fn->parameters[i].type, values[i]);
    mo_process_free(process);
    return status;
}

/* The failure's handler in FILE's own service, by its recorded name and kind: a failure query's
 * program may differ from the one that recorded it. */
static uint32_t query_handler(const struct mo_program *program, uint32_t service,
                              const struct mo_failure_record *r) {
    const struct mo_checked_service *s = &program->services[service];
    uint32_t count = r->timer ? s->timer_count : s->tool_count;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t f = r->timer ? s->timers[i].function : s->tools[i].function;
        const struct mo_symbol *symbol = program->functions[f].signature->function;
        struct mo_failure_slice name = {(const uint8_t *)symbol->name, symbol->name_length};
        if (mo_failure_slice_equal(name, r->name))
            return f;
    }
    return MO_NO_INDEX;
}

int mo_offline_program(const struct mo_cli *cli, struct mo_offline_pipeline *p) {
    MO_ASSERT(cli->command == MO_COMMAND_REPLAY || cli->command == MO_COMMAND_QUERY);
    struct offline o = {.cli = cli,
                        .output = p->output,
                        .errors = p->errors,
                        .command = cli->command == MO_COMMAND_REPLAY ? "mo replay" : "mo query",
                        .library = p->library};
    mo_buffer_init(&o.out, MO_LIMIT_HISTORY_BYTES + 4 * (size_t)MO_LIMIT_OFFLINE_VALUE_BYTES);
    refusal_init(&o.refusal);
    struct input in = {0};
    mo_arena_init_confidential(&in.arena, MO_LIMIT_EXPORT_BYTES + 4096);
    const struct mo_program *program = p->program;
    uint32_t service = main_service(program);
    /* Diagnostics set the exit status themselves. */
    int status = 0;
    struct mo_arena lowered;
    mo_arena_init(&lowered, MO_LIMIT_LOWER_WORK);
    struct mo_buffer wire;
    mo_buffer_init(&wire, MO_LIMIT_BYTECODE_SIZE);
    struct compiled compiled = {.service = service};
    uint32_t function = MO_NO_INDEX, core_index = MO_NO_INDEX;
    bool querying = cli->command == MO_COMMAND_QUERY;
    if (querying) {
        /* A failure query is checked against the failure's handler, so its record is read
         * first; a state query's entry is checked before its artifact is read. */
        uint32_t handler = MO_NO_INDEX;
        enum mo_query_input input = cli->failure == 0 ? MO_QUERY_STATE : MO_QUERY_FAILURE;
        if (input == MO_QUERY_FAILURE) {
            bool read =
                read_input(&o, &in) &&
                (service != MO_NO_INDEX ||
                 refuse(&o, "handler", "FILE's main module declares no service", NULL, NULL));
            if (read) {
                handler = query_handler(program, service, &in.record);
                if (handler == MO_NO_INDEX) {
                    (void)refuse(&o, "handler",
                                 "FILE's service has no handler of the failure's name and kind",
                                 NULL, "none");
                    bool shown = describe(&o.refusal.recorded, in.record.name, in.record.timer != 0,
                                          in.record.read_only != 0, in.record.handler,
                                          in.record.argument_count);
                    MO_ASSERT(shown);
                }
            }
            if (o.refused) {
                status = 1;
                if (cli->json && !mo_write_version(p->output, cli->command))
                    goto done;
                *p->version = cli->json;
                (void)write_query_refusal(&o);
                goto done;
            }
        }
        enum mo_query_entry entry =
            mo_check_query_entry(p->arena, program, (const uint8_t *)cli->function,
                                 strlen(cli->function), input, handler, p->diags, &function);
        if (entry == MO_QUERY_ENTRY_NO_MODULE || entry == MO_QUERY_ENTRY_NO_FUNCTION) {
            *p->mistake = entry == MO_QUERY_ENTRY_NO_MODULE
                              ? "mo query: no module of that name in FILE's program"
                              : "mo query: no function of that name in that module";
            status = 2;
            goto done;
        }
        if (entry != MO_QUERY_ENTRY_ACCEPTED)
            goto done;
    }
    struct mo_services services;
    struct mo_core *core =
        querying ? mo_lower_query(&lowered, program, p->sources, p->diags, function, &services,
                                  &core_index)
                 : mo_lower_services(&lowered, program, p->sources, p->diags, NULL, &services);
    if (core == NULL || mo_diags_any(p->diags) ||
        !mo_core_check_services(core, &services, p->diags) ||
        !mo_compile_services(core, &services, &wire, p->diags))
        goto done;
    if (cli->json && !*p->version) {
        status = 1;
        if (!mo_write_version(p->output, cli->command))
            goto done;
        *p->version = true;
        status = 0;
    }
    struct mo_load_error error;
    compiled.image = mo_load(wire.bytes, wire.length, &error);
    struct mo_limits limits = mo_limits_default();
    struct mo_defect defect;
    if (compiled.image == NULL) {
        struct mo_diag diag = {.code = MO_CODE_TOOL_PIPELINE,
                               .message = "compiled bytecode could not be loaded",
                               .primary = {{UINT32_MAX, 0, 0}, error.reason}};
        bool added = mo_diags_add(p->diags, &diag);
        (void)added;
        goto done;
    }
    if (mo_image_init(compiled.image, &limits, &defect) != MO_RETURNED) {
        if (!mo_write_defect(p->errors, NULL, &defect))
            (void)fputs("mo: could not write the initialization defect\n", p->errors);
        status = 70;
        goto done;
    }
    /* Only JSON renders values through the renderer; text prints them itself. */
    if (cli->json) {
        if (!mo_offline_renderer_open(&o.renderer)) {
            (void)fprintf(p->errors, "%s: the value renderer is unavailable\n", o.command);
            status = 1;
            goto done;
        }
        o.rendering = true;
    }
    if (querying) {
        MO_ASSERT(core_index < mo_image_bytecode(compiled.image)->function_count);
        if (cli->failure == 0 && !read_input(&o, &in)) {
            (void)write_query_refusal(&o);
            status = 1;
        } else
            status = query(&o, &compiled, core_index, &in);
    } else
        status = replay(&o, &compiled);
done:
    if (o.rendering)
        mo_offline_renderer_close(&o.renderer);
    if (compiled.image != NULL)
        mo_image_free(compiled.image);
    mo_buffer_free(&wire);
    mo_arena_free(&lowered);
    input_free(&in);
    refusal_free(&o.refusal);
    mo_buffer_free(&o.out);
    return status;
}
