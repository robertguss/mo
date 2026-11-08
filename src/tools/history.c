#define _POSIX_C_SOURCE 200809L
/* mo history DIR: what a stopped data folder holds, read without starting its service. */
#include "tools/offline.h"

#include <string.h>

static const char gone[] = "messages before the snapshot are gone, so a message absent from the "
                           "log tail may still have run";
static const char pre_capture[] = "this store predates failure capture, so it holds no failure "
                                  "records";
static const char state_check[] = "the record's pre-call state failed its check";

struct history_output {
    struct mo_buffer *out;
    struct mo_offline_renderer *renderer;
    const struct mo_store_history *h;
    bool ok;
};

static void put(struct history_output *o, const char *text) {
    o->ok = o->ok && mo_buffer_append_text(o->out, text);
}
static void put_uint(struct history_output *o, uint64_t value) {
    o->ok = o->ok && mo_buffer_append_uint(o->out, value);
}
static void put_quoted(struct history_output *o, uint64_t value) {
    put(o, "\"");
    put_uint(o, value);
    put(o, "\"");
}
static void put_json(struct history_output *o, const void *bytes, size_t length) {
    o->ok = o->ok && mo_render_json_text(o->out, bytes, length);
}
static void put_safe(struct history_output *o, const void *bytes, size_t length) {
    o->ok = o->ok && mo_render_terminal_text(o->out, bytes, length);
}

static uint64_t argument_total(const struct mo_store_history_entry *e) {
    uint64_t total = 0;
    for (uint32_t i = 0; i < e->argument_count; i++)
        total += e->argument_bytes[i];
    return total;
}

/* A log tail entry, with the live log view's field names. The log keeps the handler's index and
 * whether it is a timer, not its name, and history reads no program. */
static void entry_json(struct history_output *o, const struct mo_store_history_entry *e) {
    put(o, "{\"handler\":null,\"handler_index\":");
    put_uint(o, e->handler);
    put(o, e->timer ? ",\"timer\":true" : ",\"timer\":false");
    put(o, ",\"arrival_us\":\"");
    o->ok = o->ok && mo_buffer_append_int(o->out, e->arrival);
    put(o, "\",\"sequence\":");
    put_quoted(o, e->sequence);
    put(o, ",\"argument_bytes\":");
    put_quoted(o, argument_total(e));
    put(o, ",\"argument_sizes\":[");
    for (uint32_t i = 0; i < e->argument_count; i++) {
        put(o, i == 0 ? "" : ",");
        put_quoted(o, e->argument_bytes[i]);
    }
    put(o, "],\"size_unavailable_reason\":null}");
}

static void entry_text(struct history_output *o, const struct mo_store_history_entry *e) {
    put(o, "  ");
    put_uint(o, e->sequence);
    put(o, e->timer ? ": timer " : ": tool ");
    put_uint(o, e->handler);
    put(o, " (name not recorded in the log), arrival ");
    o->ok = o->ok && mo_buffer_append_int(o->out, e->arrival);
    put(o, " us, ");
    put_uint(o, e->argument_count);
    put(o, e->argument_count == 1 ? " argument" : " arguments");
    for (uint32_t i = 0; i < e->argument_count; i++) {
        put(o, i == 0 ? " (" : ", ");
        put_uint(o, e->argument_bytes[i]);
        put(o, i + 1 == e->argument_count ? " bytes)" : "");
    }
    put(o, "\n");
}

/* A failure record, with the live failure view's field names, then its build and whether this
 * build can replay it. A record whose message does not decode is listed as unreadable. A
 * record whose pre-call state frame fails its check is listed, but not as replayable here. */
static const char *replay_refusal(const struct mo_store_history_failure *f,
                                  const struct mo_failure_record *r) {
    return !f->state_valid ? state_check : mo_offline_build_refusal(r);
}

static void failure_json(struct history_output *o, const struct mo_store_history_failure *f) {
    struct mo_failure_record r;
    bool readable = mo_failure_read(f->message.bytes, f->message.length, &r) && r.id == f->id;
    put(o, "{\"id\":");
    put_quoted(o, f->id);
    put(o, ",\"kind\":");
    put_json(o, f->kind, strlen(f->kind));
    if (!readable) {
        put(o, ",\"unreadable\":true,\"replayable_here\":false,"
               "\"not_replayable\":\"the record's message does not decode\"}");
        return;
    }
    put(o, ",\"handler\":");
    put_json(o, r.name.bytes, r.name.length);
    put(o, ",\"arrival\":\"");
    o->ok = o->ok && mo_buffer_append_int(o->out, mo_failure_arrival(&r));
    put(o, "\",\"state_bytes\":");
    put_quoted(o, f->state_bytes);
    put(o, ",\"message_bytes\":");
    put_quoted(o, f->message.length);
    put(o, ",\"signature\":");
    struct mo_signature signature = mo_signature_from_record(&r);
    o->ok = o->ok && mo_offline_signature_json(o->out, r.signature_version, &signature);
    put(o, ",\"build_label\":");
    put_json(o, r.label.bytes, r.label.length);
    put(o, ",\"build\":\"");
    o->ok = o->ok && mo_offline_hex(o->out, r.build, sizeof r.build);
    const char *refusal = replay_refusal(f, &r);
    put(o, refusal == NULL ? "\",\"replayable_here\":true,\"not_replayable\":null}"
                           : "\",\"replayable_here\":false,\"not_replayable\":");
    if (refusal != NULL) {
        put_json(o, refusal, strlen(refusal));
        put(o, "}");
    }
}

static void failure_text(struct history_output *o, const struct mo_store_history_failure *f) {
    struct mo_failure_record r;
    put(o, "  ");
    put_uint(o, f->id);
    if (!mo_failure_read(f->message.bytes, f->message.length, &r) || r.id != f->id) {
        put(o, ": unreadable record (its message does not decode); not replayable\n");
        return;
    }
    put(o, ": ");
    put_safe(o, r.name.bytes, r.name.length);
    put(o, r.timer ? " (timer), " : " (tool), ");
    struct mo_signature signature = mo_signature_from_record(&r);
    o->ok = o->ok && mo_offline_signature_text(o->out, &signature);
    put(o, "\n     recorded by ");
    put_safe(o, r.label.bytes, r.label.length);
    put(o, " (sha256 ");
    o->ok = o->ok && mo_offline_hex(o->out, r.build, sizeof r.build);
    const char *refusal = replay_refusal(f, &r);
    put(o, "); ");
    put(o, refusal == NULL ? "replayable here" : "not replayable here: ");
    put(o, refusal == NULL ? "" : refusal);
    put(o, "\n");
}

static void program_json(struct history_output *o, const struct mo_store_program *p) {
    put(o, "{\"ordinal\":");
    put_quoted(o, p->ordinal);
    put(o, ",\"image\":");
    put_quoted(o, p->image);
    const char *names[] = {"source", "build", "schema", "context_digest"};
    const uint8_t *digests[] = {p->source, p->build, p->schema, p->context_digest};
    for (size_t i = 0; i < 4; i++) {
        put(o, ",\"");
        put(o, names[i]);
        put(o, "\":\"");
        o->ok = o->ok && mo_offline_hex(o->out, digests[i], 32);
        put(o, "\"");
    }
    put(o, "}");
}
static bool source_unavailable(const uint8_t source[32]) {
    static const uint8_t zero[32] = {0};
    return memcmp(source, zero, sizeof zero) == 0;
}
static void programs_json(struct history_output *o) {
    put(o, ",\"program\":");
    if (o->h->has_program)
        program_json(o, &o->h->program);
    else
        put(o, "null");
    put(o, ",\"previous_programs\":[");
    for (const struct mo_store_history_program *p = o->h->previous_programs; p != NULL;
         p = p->next) {
        put(o, p == o->h->previous_programs ? "" : ",");
        program_json(o, &p->program);
    }
    put(o, "],\"upgrades\":[");
    for (const struct mo_store_history_upgrade *u = o->h->upgrades; u != NULL; u = u->next) {
        put(o, u == o->h->upgrades ? "" : ",");
        put(o, "{\"ordinal\":");
        put_quoted(o, u->ordinal);
        put(o, ",\"head\":");
        put_quoted(o, u->head);
        put(o, ",\"old_source\":");
        if (source_unavailable(u->old_source))
            put(o, "null");
        else {
            put(o, "\"");
            o->ok = o->ok && mo_offline_hex(o->out, u->old_source, 32);
            put(o, "\"");
        }
        put(o, ",\"new_source\":\"");
        o->ok = o->ok && mo_offline_hex(o->out, u->new_source, 32);
        put(o, u->program_retained
                   ? "\",\"program_retained\":true}"
                   : "\",\"program_retained\":false,\"reason\":\"program not retained\"}");
    }
    put(o, "],\"upgrade_records\":");
    put_quoted(o, o->h->upgrade_count);
    put(o, o->h->upgrades_truncated ? ",\"upgrades_truncated\":true"
                                    : ",\"upgrades_truncated\":false");
}
static void programs_text(struct history_output *o) {
    if (!o->h->has_program) {
        put(o, "program: not recorded (legacy folder)\n");
        return;
    }
    put(o, "program: source ");
    o->ok = o->ok && mo_offline_hex(o->out, o->h->program.source, 32);
    put(o, ", context ");
    o->ok = o->ok && mo_offline_hex(o->out, o->h->program.context_digest, 32);
    put(o, ", activation ");
    put_uint(o, o->h->program.ordinal);
    put(o, "\n");
    for (const struct mo_store_history_upgrade *u = o->h->upgrades; u != NULL; u = u->next) {
        put(o, "upgrade ");
        put_uint(o, u->ordinal);
        put(o, " at head ");
        put_uint(o, u->head);
        put(o, ": ");
        if (source_unavailable(u->old_source))
            put(o, "source unavailable (legacy)");
        else
            o->ok = o->ok && mo_offline_hex(o->out, u->old_source, 32);
        put(o, " -> ");
        o->ok = o->ok && mo_offline_hex(o->out, u->new_source, 32);
        put(o, u->program_retained ? " (program retained)\n" : " (program not retained)\n");
    }
    if (o->h->upgrades_truncated)
        put(o, "more upgrade records not copied: over the history bound\n");
}
static void history_json(struct history_output *o, const char *folder) {
    const struct mo_store_history *h = o->h;
    put(o, "{\"record\":\"history\",\"data\":");
    put_json(o, folder, strnlen(folder, MO_LIMIT_PATH_SIZE));
    put(o, ",\"folder\":\"ok\",\"recovered\":");
    put(o, h->recovered ? "true" : "false");
    programs_json(o);
    put(o, ",\"snapshot\":");
    if (!h->has_snapshot)
        put(o, "null");
    else {
        put(o, "{\"sequence\":");
        put_quoted(o, h->snapshot.sequence);
        put(o, ",\"later_messages\":");
        put_uint(o, h->head - h->snapshot.sequence);
        put(o, ",\"producer_build\":null,\"state_bytes\":");
        put_quoted(o, h->snapshot.length);
        put(o, ",\"state\":");
        if (h->snapshot.bytes == NULL)
            put(o, "{\"value\":null,\"complete\":false}");
        else
            o->ok = o->ok &&
                    mo_offline_render(o->renderer, h->snapshot.bytes, h->snapshot.length, o->out);
        put(o, "}");
    }
    put(o, ",\"head\":");
    put_quoted(o, h->head);
    put(o, ",\"log\":[");
    for (uint32_t i = 0; i < h->entry_count; i++) {
        put(o, i == 0 ? "" : ",");
        entry_json(o, &h->entries[i]);
    }
    put(o, "],\"log_entries\":");
    put_uint(o, h->log_count);
    put(o, ",\"log_note\":");
    put_json(o, gone, sizeof gone - 1);
    put(o, ",\"failure_capture\":");
    put(o, h->failure_tables ? "true,\"failure_note\":null" : "false,\"failure_note\":");
    if (!h->failure_tables)
        put_json(o, pre_capture, sizeof pre_capture - 1);
    put(o, ",\"failures\":[");
    for (uint32_t i = 0; i < h->failure_count; i++) {
        put(o, i == 0 ? "" : ",");
        failure_json(o, &h->failures[i]);
    }
    put(o, "],\"failure_records\":");
    put_uint(o, h->failure_total);
    put(o, ",\"retained_bytes\":");
    put_quoted(o, h->retained_bytes);
    put(o, ",\"left_bytes\":");
    put_quoted(o, h->left_bytes);
    put(o, "}\n");
}

static void history_text(struct history_output *o, const char *folder) {
    const struct mo_store_history *h = o->h;
    put(o, "history of ");
    put_safe(o, folder, strnlen(folder, MO_LIMIT_PATH_SIZE));
    put(o, h->recovered ? " (recovered: a journal was rolled back, writing database pages but "
                          "changing no record)\n"
                        : "\n");
    programs_text(o);
    if (!h->has_snapshot)
        put(o, "snapshot: none; the service's first start never finished\n");
    else {
        put(o, "snapshot: as of sequence ");
        put_uint(o, h->snapshot.sequence);
        put(o, ", ");
        put_uint(o, h->head - h->snapshot.sequence);
        put(o, " later messages not applied; producer build not recorded\nstate (");
        put_uint(o, h->snapshot.length);
        put(o, " saved bytes): ");
        if (h->snapshot.bytes == NULL)
            put(o, "not copied: over the history bound");
        else {
            /* The renderer escapes only what JSON must; the terminal escapes the rest. */
            struct mo_buffer rendered;
            mo_buffer_init(&rendered, 2 * (size_t)MO_LIMIT_OFFLINE_VALUE_BYTES);
            o->ok =
                o->ok &&
                mo_offline_render(o->renderer, h->snapshot.bytes, h->snapshot.length, &rendered) &&
                mo_render_terminal_text(o->out, rendered.bytes, rendered.length);
            mo_buffer_free(&rendered);
        }
        put(o, "\n");
    }
    put(o, "log tail: ");
    put_uint(o, h->log_count);
    put(o, h->log_count == 1 ? " message" : " messages");
    put(o, " after the snapshot\n");
    for (uint32_t i = 0; i < h->entry_count; i++)
        entry_text(o, &h->entries[i]);
    if (h->entry_count < h->log_count) {
        put(o, "  ");
        put_uint(o, h->log_count - h->entry_count);
        put(o, " more not copied: over the history bound\n");
    }
    put(o, "note: ");
    put(o, gone);
    put(o, "\n");
    if (!h->failure_tables) {
        put(o, "failures: none; ");
        put(o, pre_capture);
        put(o, "\n");
    } else {
        put(o, "failures: ");
        put_uint(o, h->failure_total);
        put(o, " retained\n");
        for (uint32_t i = 0; i < h->failure_count; i++)
            failure_text(o, &h->failures[i]);
        if (h->failure_count < h->failure_total) {
            put(o, "  ");
            put_uint(o, h->failure_total - h->failure_count);
            put(o, " more not copied: over the history bound\n");
        }
    }
    if (h->left_bytes != 0) {
        put(o, "copied ");
        put_uint(o, h->retained_bytes);
        put(o, " bytes and left ");
        put_uint(o, h->left_bytes);
        put(o, " bytes over MO_LIMIT_HISTORY_BYTES\n");
    }
}

static bool totals(struct mo_buffer *out, int status) {
    return mo_buffer_append_text(out,
                                 "{\"record\":\"totals\",\"errors\":0,\"dropped\":0,\"status\":") &&
           mo_buffer_append_uint(out, (uint64_t)status) && mo_buffer_append_text(out, "}\n");
}

int mo_history(const struct mo_cli *cli, FILE *output, FILE *errors) {
    const char *folder = mo_cli_file(cli, 0);
    char path[MO_LIMIT_PATH_SIZE + sizeof "/" MO_DATA_FILE];
    if (!mo_offline_store_path(folder, path, sizeof path)) {
        (void)fputs("mo history: the data folder's path is too long\n", errors);
        return 2;
    }
    if (cli->json && !mo_write_version(output, MO_COMMAND_HISTORY)) {
        (void)fputs("mo: could not write version record\n", errors);
        return 1;
    }
    struct mo_store_history history;
    enum mo_store_folder status = mo_store_history_read(path, cli->recover, &history);
    struct mo_buffer out;
    /* The rendered snapshot takes at most twice MO_LIMIT_OFFLINE_VALUE_BYTES, and the text
     * form's terminal escapes write at most four bytes for each of those bytes. */
    mo_buffer_init(&out, MO_LIMIT_HISTORY_BYTES + 8 * (size_t)MO_LIMIT_OFFLINE_VALUE_BYTES);
    bool ok = true;
    if (status != MO_STORE_FOLDER_OK) {
        struct mo_buffer message;
        mo_buffer_init(&message, MO_CLI_DIAGNOSTIC_SIZE);
        ok = mo_offline_folder_message(&message, folder, status, history.damaged_sequence);
        if (ok && cli->json)
            ok = mo_buffer_append_text(&out, "{\"record\":\"history\",\"data\":") &&
                 mo_render_json_text(&out, folder, strnlen(folder, MO_LIMIT_PATH_SIZE)) &&
                 mo_buffer_append_text(&out, ",\"folder\":\"") &&
                 mo_buffer_append_text(&out, mo_offline_folder_code(status)) &&
                 mo_buffer_append_text(&out, "\",\"reason\":") &&
                 mo_render_json_text(&out, message.bytes, message.length) &&
                 mo_buffer_append_text(&out, "}\n") && totals(&out, 1) &&
                 mo_offline_flush(output, &out);
        else if (ok)
            ok = mo_buffer_append_text(&out, "mo history: ") &&
                 mo_buffer_append(&out, message.bytes, message.length) &&
                 mo_buffer_append_byte(&out, '\n') && mo_offline_flush(errors, &out);
        if (!ok)
            (void)fputs("mo history: could not write why the folder was not read\n", errors);
        mo_buffer_free(&message);
        mo_buffer_free(&out);
        return 1;
    }
    /* Only the snapshot's state is rendered. */
    struct mo_offline_renderer renderer = {0};
    if (history.has_snapshot && history.snapshot.bytes != NULL &&
        !mo_offline_renderer_open(&renderer)) {
        mo_store_history_free(&history);
        mo_buffer_free(&out);
        (void)fputs("mo history: the value renderer is unavailable\n", errors);
        return 1;
    }
    struct history_output o = {.out = &out, .renderer = &renderer, .h = &history, .ok = true};
    if (cli->json)
        history_json(&o, folder);
    else
        history_text(&o, folder);
    ok = o.ok && (!cli->json || totals(&out, 0)) && mo_offline_flush(output, &out);
    mo_offline_renderer_close(&renderer);
    mo_store_history_free(&history);
    mo_buffer_free(&out);
    if (!ok) {
        (void)fputs("mo history: could not write the history\n", errors);
        return 1;
    }
    return 0;
}
