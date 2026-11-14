#include "tools/intent.h"

#include "tools/offline.h"
#include "tools/tests.h"

#include <stdio.h>
#include <string.h>

enum change { UNCHANGED, EDITED, MOVED, REMOVED, ADDED, CHANGES };
static const char *const change_names[CHANGES] = {"unchanged", "edited", "moved", "removed",
                                                  "added"};

struct out {
    FILE *file;
    bool json, ok;
    struct mo_buffer buffer, scratch;
};

static void put(struct out *o, const char *text) {
    o->ok = o->ok && mo_buffer_append_text(&o->buffer, text);
}

static void put_text(struct out *o, const void *bytes, size_t length) {
    o->ok = o->ok && (o->json ? mo_render_json_text(&o->buffer, bytes, length)
                              : mo_render_terminal_text(&o->buffer, bytes, length));
}

static void put_name(struct out *o, const char *name) {
    put_text(o, name, strlen(name));
}

static void put_read_name(struct out *o, const struct mo_intent_read *read) {
    mo_buffer_clear(&o->scratch);
    o->ok = o->ok && mo_buffer_append_text(&o->scratch, read->module) &&
            mo_buffer_append_byte(&o->scratch, '.') &&
            mo_buffer_append_text(&o->scratch, read->name);
    put_text(o, o->scratch.bytes, o->scratch.length);
}

static void put_digest(struct out *o, const struct mo_intent_item *item, size_t bytes) {
    o->ok = o->ok && mo_offline_hex(&o->buffer, item->digest, bytes);
}

static void flush(struct out *o) {
    o->ok = o->ok && fwrite(o->buffer.bytes, 1, o->buffer.length, o->file) == o->buffer.length &&
            fflush(o->file) == 0;
    mo_buffer_clear(&o->buffer);
}

static void lines(struct out *o, const char *first, const char *rest, struct mo_intent_text text) {
    for (size_t at = 0; at < text.length;) {
        const uint8_t *end = memchr(text.bytes + at, '\n', text.length - at);
        size_t length = end == NULL ? text.length - at : (size_t)(end - text.bytes) - at;
        if (length != 0) {
            put(o, at == 0 ? first : rest);
            put_text(o, text.bytes + at, length);
        }
        put(o, "\n");
        at += length + 1;
    }
}

static void json_item(struct out *o, const struct mo_intent_item *item) {
    put(o, "\"name\":");
    put_name(o, item->name);
    put(o, ",\"kind\":");
    put_name(o, item->kind);
    put(o, ",\"owner\":");
    put_name(o, item->owner);
    put(o, ",\"digest\":\"");
    put_digest(o, item, 32);
    put(o, "\",\"text\":");
    put_text(o, item->text.bytes, item->text.length);
    put(o, ",\"reads\":[");
    for (uint32_t i = 0; i < item->read_count; i++) {
        put(o, i == 0 ? "" : ",");
        put_read_name(o, &item->reads[i]);
    }
    put(o, "]");
}

static void text_reads(struct out *o, const char *first, const struct mo_intent_item *item) {
    for (uint32_t i = 0; i < item->read_count; i++) {
        put(o, i == 0 ? first : ", ");
        put_read_name(o, &item->reads[i]);
    }
    put(o, item->read_count == 0 ? "" : "\n");
}

static bool example(const struct mo_intent_item *item) {
    return item->spec->kind == MO_SPEC_EXAMPLE;
}

static void list(struct out *o, const struct mo_intent *intent) {
    for (uint32_t i = 0; o->ok && i < intent->count; i++) {
        const struct mo_intent_item *item = &intent->items[i];
        if (o->json) {
            put(o, "{\"record\":\"item\",");
            json_item(o, item);
            put(o, "}\n");
        } else if (example(item)) {
            const struct mo_intent_item *previous = i == 0 ? NULL : &intent->items[i - 1];
            if (previous == NULL || !example(previous) || strcmp(previous->owner, item->owner)) {
                put(o, i == 0 ? "examples " : "\nexamples ");
                put_name(o, item->owner);
                put(o, "\n");
            }
            put(o, "  ");
            put_name(o, item->name);
            put(o, " ");
            put_digest(o, item, 6);
            lines(o, "  ", "    ", item->text);
            text_reads(o, "    reads ", item);
        } else {
            put(o, i == 0 ? "" : "\n");
            put_name(o, item->kind);
            put(o, " ");
            put_name(o, item->name);
            put(o, " ");
            put_digest(o, item, 6);
            put(o, "\n");
            lines(o, "  ", "  ", item->text);
            text_reads(o, "  reads ", item);
        }
        flush(o);
    }
    put(o, o->json ? "{\"record\":\"intent\",\"items\":" : "intent: ");
    o->ok = o->ok && mo_buffer_append_uint(&o->buffer, intent->count);
    put(o, o->json ? "}\n" : " items\n");
    flush(o);
}

static bool same_text(struct mo_intent_text a, struct mo_intent_text b) {
    return a.length == b.length && (a.length == 0 || memcmp(a.bytes, b.bytes, a.length) == 0);
}

static enum change fits(uint32_t pass, const struct mo_intent_item *a,
                        const struct mo_intent_item *b) {
    bool owner = strcmp(a->owner, b->owner) == 0, kind = strcmp(a->kind, b->kind) == 0;
    if (pass == 0)
        return memcmp(a->digest, b->digest, 32) == 0 ? UNCHANGED : CHANGES;
    if (pass == 1)
        return owner && kind && same_text(a->text, b->text) ? EDITED : CHANGES;
    if (pass == 2)
        return kind && same_text(a->text, b->text) ? MOVED : CHANGES;
    return owner && kind ? EDITED : CHANGES;
}

static int by_read(const struct mo_intent_read *a, const struct mo_intent_read *b) {
    int c = strcmp(a->module, b->module);
    return c != 0 ? c : strcmp(a->name, b->name);
}

static void changed_read(struct out *o, const struct mo_intent_read *before,
                         const struct mo_intent_read *after, bool *first) {
    const struct mo_intent_read *named = before != NULL ? before : after;
    if (o->json) {
        put(o, *first ? "{\"name\":" : ",{\"name\":");
        put_read_name(o, named);
        const struct mo_intent_read *sides[2] = {before, after};
        for (uint32_t s = 0; s < 2; s++) {
            put(o, s == 0 ? ",\"before\":" : ",\"after\":");
            if (sides[s] == NULL)
                put(o, "null");
            else
                put_text(o, sides[s]->text.bytes, sides[s]->text.length);
        }
        put(o, "}");
    } else {
        put(o, before == NULL ? "  now reads " : after == NULL ? "  no longer reads " : "  ");
        put_read_name(o, named);
        put(o, before == NULL || after == NULL ? "\n" : " before\n");
        lines(o, "    ", "    ", (before != NULL ? before : after)->text);
        if (before != NULL && after != NULL) {
            put(o, "  ");
            put_read_name(o, named);
            put(o, " after\n");
            lines(o, "    ", "    ", after->text);
        }
    }
    *first = false;
}

static void changed_reads(struct out *o, const struct mo_intent_item *a,
                          const struct mo_intent_item *b) {
    bool first = true;
    uint32_t i = 0, j = 0;
    while (o->ok && (i < a->read_count || j < b->read_count)) {
        const struct mo_intent_read *x = i < a->read_count ? &a->reads[i] : NULL;
        const struct mo_intent_read *y = j < b->read_count ? &b->reads[j] : NULL;
        int c = x == NULL ? 1 : y == NULL ? -1 : by_read(x, y);
        if (c < 0)
            changed_read(o, x, NULL, &first);
        else if (c > 0)
            changed_read(o, NULL, y, &first);
        else if (!same_text(x->text, y->text))
            changed_read(o, x, y, &first);
        i += c <= 0;
        j += c >= 0;
    }
}

static void change(struct out *o, enum change kind, const struct mo_intent_item *before,
                   const struct mo_intent_item *after) {
    if (o->json) {
        put(o, "{\"record\":\"change\",\"change\":\"");
        put(o, change_names[kind]);
        put(o, "\"");
        if (before != NULL) {
            put(o, ",\"before\":{");
            json_item(o, before);
            put(o, "}");
        }
        if (after != NULL) {
            put(o, ",\"after\":{");
            json_item(o, after);
            put(o, "}");
        }
        if (before != NULL && after != NULL) {
            put(o, ",\"reads\":[");
            changed_reads(o, before, after);
            put(o, "]");
        }
        put(o, "}\n");
        flush(o);
        return;
    }
    const struct mo_intent_item *shown = before != NULL ? before : after;
    put(o, change_names[kind]);
    put(o, " ");
    put_name(o, shown->name);
    if (kind == MOVED) {
        put(o, " to ");
        put_name(o, after->name);
    }
    put(o, "\n");
    if (before == NULL || after == NULL || same_text(before->text, after->text)) {
        lines(o, "  ", "  ", shown->text);
        if (before == NULL || after == NULL)
            text_reads(o, "  reads ", shown);
    } else {
        put(o, "  before\n");
        lines(o, "    ", "    ", before->text);
        put(o, "  after\n");
        lines(o, "    ", "    ", after->text);
    }
    if (before != NULL && after != NULL)
        changed_reads(o, before, after);
    put(o, "\n");
    flush(o);
}

static void compare(struct out *o, struct mo_arena *arena, const struct mo_intent *old,
                    const struct mo_intent *now) {
    uint32_t *partner = mo_arena_allocate_array(arena, old->count == 0 ? 1 : old->count,
                                                sizeof *partner, _Alignof(uint32_t));
    enum change *kinds = mo_arena_allocate_array(arena, old->count == 0 ? 1 : old->count,
                                                 sizeof *kinds, _Alignof(enum change));
    bool *taken = mo_arena_allocate_array(arena, now->count == 0 ? 1 : now->count, sizeof *taken,
                                          _Alignof(bool));
    if (partner == NULL || kinds == NULL || taken == NULL) {
        o->ok = false;
        return;
    }
    for (uint32_t i = 0; i < old->count; i++)
        partner[i] = UINT32_MAX;
    memset(taken, 0, now->count * sizeof *taken);
    for (uint32_t pass = 0; pass < 4; pass++)
        for (uint32_t i = 0; i < old->count; i++)
            for (uint32_t j = 0; partner[i] == UINT32_MAX && j < now->count; j++)
                if (!taken[j] &&
                    (kinds[i] = fits(pass, &old->items[i], &now->items[j])) != CHANGES) {
                    partner[i] = j;
                    taken[j] = true;
                }
    uint32_t counts[CHANGES] = {0};
    for (uint32_t i = 0; o->ok && i < old->count; i++) {
        enum change kind = partner[i] == UINT32_MAX ? REMOVED : kinds[i];
        counts[kind]++;
        if (kind != UNCHANGED)
            change(o, kind, &old->items[i],
                   partner[i] == UINT32_MAX ? NULL : &now->items[partner[i]]);
    }
    for (uint32_t j = 0; o->ok && j < now->count; j++)
        if (!taken[j]) {
            counts[ADDED]++;
            change(o, ADDED, NULL, &now->items[j]);
        }
    if (o->json) {
        put(o, "{\"record\":\"intent\",\"items\":");
        o->ok = o->ok && mo_buffer_append_uint(&o->buffer, now->count);
    } else
        put(o, "intent: ");
    for (uint32_t c = 0; c < CHANGES; c++) {
        static const uint32_t order[CHANGES] = {UNCHANGED, EDITED, REMOVED, ADDED, MOVED};
        put(o, o->json ? ",\"" : c == 0 ? "" : ", ");
        if (!o->json) {
            o->ok = o->ok && mo_buffer_append_uint(&o->buffer, counts[order[c]]);
            put(o, " ");
        }
        put(o, change_names[order[c]]);
        if (o->json) {
            put(o, "\":");
            o->ok = o->ok && mo_buffer_append_uint(&o->buffer, counts[order[c]]);
        }
    }
    put(o, o->json ? "}\n" : "\n");
    flush(o);
}

static void report(struct mo_diags *diags, const char *message, const char *detail) {
    mo_test_report_error(diags, message, (struct mo_span){UINT32_MAX, 0, 0}, detail);
}

static void against(const struct mo_cli *cli, struct out *o, struct mo_arena *arena,
                    const char *library, const struct mo_intent *now, struct mo_diags *diags) {
    struct mo_arena old_arena;
    mo_arena_init(&old_arena, MO_CLI_CHECK_SIZE);
    struct mo_sources sources;
    struct mo_diags old_diags;
    bool ready = mo_sources_init(&sources);
    ready = mo_diags_init(&old_diags) && ready;
    struct mo_module_set *set =
        ready ? mo_load_program(&old_arena, &sources, cli->against, library, &old_diags) : NULL;
    struct mo_program *program =
        set == NULL || mo_diags_any(&old_diags) ? NULL : mo_check(&old_arena, set, &old_diags);
    struct mo_intent old;
    if (program == NULL || mo_diags_any(&old_diags)) {
        char detail[512] = "";
        if (old_diags.count != 0) {
            int n = snprintf(detail, sizeof detail, "E%04u: %s", (unsigned)old_diags.items[0].code,
                             old_diags.items[0].message);
            MO_ASSERT(n >= 0);
        }
        report(diags, "the --against program does not check",
               detail[0] == '\0' ? NULL : mo_arena_copy(arena, detail, strlen(detail) + 1, 1));
    } else if (!mo_intent_items(&old_arena, program, &old))
        report(diags, "intent items exceed tool resources", NULL);
    else {
        compare(o, &old_arena, &old, now);
        if (!o->ok)
            report(diags, "the intent difference could not be written", NULL);
    }
    mo_diags_free(&old_diags);
    mo_sources_free(&sources);
    mo_arena_free(&old_arena);
}

void mo_intent_show(const struct mo_cli *cli, struct mo_arena *arena,
                    const struct mo_program *program, const char *library, FILE *output,
                    struct mo_diags *diags) {
    struct mo_intent now;
    if (!mo_intent_items(arena, program, &now)) {
        report(diags, "intent items exceed tool resources", NULL);
        return;
    }
    struct out o = {.file = output, .json = cli->json, .ok = true};
    mo_buffer_init(&o.buffer, MO_CLI_DIAGNOSTIC_SIZE);
    mo_buffer_init(&o.scratch, MO_CLI_DIAGNOSTIC_SIZE);
    if (cli->against != NULL)
        against(cli, &o, arena, library, &now, diags);
    else {
        list(&o, &now);
        if (!o.ok)
            report(diags, "the intent listing could not be written", NULL);
    }
    mo_buffer_free(&o.scratch);
    mo_buffer_free(&o.buffer);
}
