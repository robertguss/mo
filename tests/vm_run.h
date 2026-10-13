/* Run transport for the differential driver. Real I/O stays native and isolated;
 * semantic fake snapshots never depend on the host's private slot layout. */
#ifndef MO_VM_RUN_H
#define MO_VM_RUN_H

#include "host/host.h"

#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>

static bool vm_run_clock_active;
static int64_t vm_run_now;
MO_MUST_USE int mo_vm_run_clock(clockid_t clock, struct timespec *out);
int mo_vm_run_clock(clockid_t clock, struct timespec *out) {
    if (!vm_run_clock_active)
        return clock_gettime(clock, out);
    MO_ASSERT(clock == CLOCK_REALTIME);
    int64_t seconds = vm_run_now / 1000000, remainder = vm_run_now % 1000000;
    if (remainder < 0) {
        seconds--;
        remainder += 1000000;
    }
    out->tv_sec = (time_t)seconds;
    out->tv_nsec = (long)(remainder * 1000);
    return 0;
}

struct vm_run_fake {
    bool present;
    uint32_t type;
    struct mo_cell value;
};
struct vm_run_context {
    struct mo_arena *arena;
    struct mo_host *host;
    struct mo_heap *heap;
    struct mo_core_run *run;
    struct vm_run_fake *fakes;
    struct vm_run_fake real[MO_CAP_RANDOM + 1];
    struct mo_cell arguments[MO_LIMIT_BYTECODE_PARAMETERS];
    int folder, streams[3];
};

static MO_MUST_USE void *vm_run_array(struct mo_arena *arena, size_t count, size_t size,
                                      size_t alignment) {
    void *out = mo_arena_allocate_array(arena, count, size, alignment);
    MO_ASSERT(out != NULL || count == 0);
    return out;
}
static MO_MUST_USE int64_t vm_run_integer(mo_core_integer value) {
    MO_ASSERT(value.digits.length < 30);
    char buffer[32];
    size_t start = value.negative ? 1u : 0u;
    buffer[0] = '-';
    memcpy(buffer + start, value.digits.bytes, value.digits.length);
    buffer[start + value.digits.length] = 0;
    char *end;
    errno = 0;
    long long out = strtoll(buffer, &end, 10);
    MO_ASSERT(errno == 0 && end == buffer + start + value.digits.length);
    return (int64_t)out;
}
static MO_MUST_USE struct mo_cell vm_run_text(struct vm_run_context *c, mo_core_string s) {
    struct mo_cell out;
    MO_ASSERT(mo_value_text(c->heap, s.bytes, s.length, &out));
    return out;
}
static MO_MUST_USE struct mo_cell vm_run_texts(struct vm_run_context *c,
                                               mo_core_string_list strings, bool set) {
    struct mo_cell *items =
        vm_run_array(c->arena, strings.count, sizeof *items, _Alignof(struct mo_cell));
    for (size_t i = 0; i < strings.count; i++)
        items[i] = vm_run_text(c, strings.items[i]);
    struct mo_cell out;
    MO_ASSERT(strings.count <= UINT32_MAX);
    if (set)
        MO_ASSERT(
            mo_value_set(c->heap, c->host->types->folders, items, (uint32_t)strings.count, &out));
    else
        MO_ASSERT(
            mo_value_list(c->heap, c->host->types->texts, items, (uint32_t)strings.count, &out));
    for (size_t i = 0; i < strings.count; i++)
        mo_value_drop(c->heap, c->host->types->text, items[i]);
    return out;
}
static MO_MUST_USE struct mo_cell vm_run_pairs(struct vm_run_context *c,
                                               mo_core_string_pairs pairs) {
    struct mo_cell *keys =
        vm_run_array(c->arena, pairs.count, sizeof *keys, _Alignof(struct mo_cell));
    struct mo_cell *values =
        vm_run_array(c->arena, pairs.count, sizeof *values, _Alignof(struct mo_cell));
    for (size_t i = 0; i < pairs.count; i++) {
        keys[i] = vm_run_text(c, pairs.items[i].key);
        values[i] = vm_run_text(c, pairs.items[i].value);
    }
    struct mo_cell out;
    MO_ASSERT(pairs.count <= UINT32_MAX && mo_value_map(c->heap, c->host->types->files, keys,
                                                        values, (uint32_t)pairs.count, &out));
    for (size_t i = 0; i < pairs.count; i++) {
        mo_value_drop(c->heap, c->host->types->text, keys[i]);
        mo_value_drop(c->heap, c->host->types->text, values[i]);
    }
    return out;
}
static MO_MUST_USE unsigned vm_run_family(enum mo_capability kind) {
    return kind == MO_CAP_READ_FILES ? MO_CAP_FILES : (unsigned)kind;
}
static MO_MUST_USE struct vm_run_fake *vm_run_fake(struct vm_run_context *c, size_t id,
                                                   enum mo_capability kind) {
    MO_ASSERT(id < c->run->world.fakes.count);
    return &c->fakes[id * (MO_CAP_RANDOM + 1) + vm_run_family(kind)];
}
static MO_MUST_USE struct mo_cell vm_run_restore(struct vm_run_context *c, size_t id,
                                                 uint32_t type) {
    enum mo_capability kind = c->host->types->types.items[type].capability;
    struct vm_run_fake *cached = vm_run_fake(c, id, kind);
    if (!cached->present) {
        uint32_t backing_type = type;
        if (kind == MO_CAP_READ_FILES &&
            c->host->types->capabilities[MO_CAP_FILES] != MO_NO_INDEX) {
            backing_type = c->host->types->capabilities[MO_CAP_FILES];
        }
        struct mo_core_cap_state *s = &c->run->world.fakes.items[id];
        struct mo_host_fake_state initial = {
            .kind = c->host->types->types.items[backing_type].capability};
        switch (kind) {
        case MO_CAP_ARGS:
            initial.as.args = vm_run_texts(c, s->args, false);
            break;
        case MO_CAP_ENV:
            initial.as.env = vm_run_pairs(c, s->env);
            break;
        case MO_CAP_CONSOLE:
            initial.as.console.input = vm_run_texts(c, s->stdin_, false);
            initial.as.console.output = vm_run_texts(c, s->stdout_, false);
            initial.as.console.errors = vm_run_texts(c, s->stderr_, false);
            break;
        case MO_CAP_FILES:
        case MO_CAP_READ_FILES:
            initial.as.files.contents = vm_run_pairs(c, s->files);
            initial.as.files.folders = vm_run_texts(c, s->folders, true);
            break;
        case MO_CAP_CLOCK:
            initial.as.time = vm_run_integer(s->now);
            break;
        case MO_CAP_RANDOM:
            initial.as.random[0] = s->random.s0;
            initial.as.random[1] = s->random.s1;
            initial.as.random[2] = s->random.s2;
            initial.as.random[3] = s->random.s3;
            break;
        }
        struct mo_host_error error;
        MO_ASSERT(mo_host_fake_restore(c->host, c->heap, backing_type, &initial, &cached->value,
                                       &error) == MO_HOST_RETURNED);
        mo_host_fake_state_drop(c->host, c->heap, &initial);
        cached->present = true;
        cached->type = backing_type;
    }
    struct mo_cell result = mo_value_dup(c->heap, cached->type, cached->value);
    if (kind == MO_CAP_READ_FILES && cached->type != type) {
        struct mo_host_error error;
        struct mo_cell narrowed;
        MO_ASSERT(mo_host_call(c->host, c->heap, MO_HOST_FILES_READ_ONLY, type, &result, &narrowed,
                               &error) == MO_HOST_RETURNED);
        mo_value_drop(c->heap, cached->type, result);
        result = narrowed;
    }
    return result;
}

static void vm_run_setup(struct vm_run_context *c, struct mo_arena *arena,
                         struct mo_process *process, struct mo_core_run *run,
                         const struct mo_bytecode_function *fn, const char *directory) {
    *c = (struct vm_run_context){.arena = arena,
                                 .host = mo_process_host(process),
                                 .heap = mo_process_heap(process),
                                 .run = run};
    c->fakes = vm_run_array(arena, run->world.fakes.count * (MO_CAP_RANDOM + 1), sizeof *c->fakes,
                            _Alignof(struct vm_run_fake));
    if (run->world.fakes.count != 0)
        memset(c->fakes, 0, run->world.fakes.count * (MO_CAP_RANDOM + 1) * sizeof *c->fakes);
    int root = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    MO_ASSERT(root >= 0);
    c->folder = openat(root, "files", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    c->streams[0] = openat(root, "input", O_RDONLY | O_CLOEXEC);
    c->streams[1] = openat(root, "output", O_WRONLY | O_APPEND | O_CLOEXEC);
    c->streams[2] = openat(root, "errors", O_WRONLY | O_APPEND | O_CLOEXEC);
    MO_ASSERT(close(root) == 0 && c->folder >= 0 && c->streams[0] >= 0 && c->streams[1] >= 0 &&
              c->streams[2] >= 0);
    c->host->input = c->streams[0];
    c->host->output = c->streams[1];
    c->host->errors = c->streams[2];
    vm_run_now = vm_run_integer(run->world.now);
    vm_run_clock_active = true;
    struct mo_cell args = vm_run_texts(c, run->world.args, false);
    struct mo_cell env = vm_run_pairs(c, run->world.env);
    MO_ASSERT(run->selection.tag == MO_CORE_SELECTION_ENTRY &&
              fn->parameter_count == run->selection.as.args.count);
    for (uint32_t i = 0; i < fn->parameter_count; i++) {
        mo_core_value v = run->selection.as.args.items[i];
        MO_ASSERT(v->tag == MO_CORE_VALUE_CAP);
        uint32_t type = fn->parameters[i].type;
        enum mo_capability kind = c->host->types->types.items[type].capability;
        if (v->as.cap.fake.some) {
            int64_t id = vm_run_integer(v->as.cap.fake.index);
            MO_ASSERT(id >= 0);
            c->arguments[i] = vm_run_restore(c, (size_t)id, type);
        } else {
            /* Random's real entropy cannot be replayed from an arbitrary World.
             * No fixture uses it; refuse instead of inventing a passing fake. */
            MO_ASSERT(kind != MO_CAP_RANDOM);
            const struct mo_cell *contents = kind == MO_CAP_ARGS  ? &args
                                             : kind == MO_CAP_ENV ? &env
                                                                  : NULL;
            struct vm_run_fake *cached = &c->real[kind];
            if (!cached->present) {
                struct mo_host_error error;
                MO_ASSERT(mo_host_real(c->host, c->heap, type, contents, c->folder, &cached->value,
                                       &error) == MO_HOST_RETURNED);
                cached->present = true;
                cached->type = type;
            }
            c->arguments[i] = mo_value_dup(c->heap, type, cached->value);
        }
        if (kind == MO_CAP_FILES || kind == MO_CAP_READ_FILES) {
            mo_core_string prefix =
                v->as.cap.root.some ? v->as.cap.root.text : (mo_core_string){"..", 2};
            if (prefix.length != 0) {
                struct mo_cell input[] = {c->arguments[i], vm_run_text(c, prefix)}, narrowed;
                struct mo_host_error error;
                uint32_t op =
                    kind == MO_CAP_FILES ? MO_HOST_FILES_WITHIN : MO_HOST_READFILES_WITHIN;
                MO_ASSERT(mo_host_call(c->host, c->heap, op, type, input, &narrowed, &error) ==
                          MO_HOST_RETURNED);
                mo_value_drop(c->heap, type, c->arguments[i]);
                mo_value_drop(c->heap, c->host->types->text, input[1]);
                c->arguments[i] = narrowed;
            }
        }
    }
    mo_value_drop(c->heap, c->host->types->texts, args);
    mo_value_drop(c->heap, c->host->types->files, env);
}

static MO_MUST_USE mo_core_string vm_run_string(struct vm_run_context *c, struct mo_cell value) {
    size_t length;
    const uint8_t *bytes = mo_value_data(value, &length);
    void *copy = vm_run_array(c->arena, length, 1, 1);
    if (length != 0)
        memcpy(copy, bytes, length);
    return (mo_core_string){copy, length};
}
static MO_MUST_USE mo_core_string_list vm_run_strings(struct vm_run_context *c, struct mo_cell list,
                                                      bool set) {
    mo_core_string_list out = {.count = set ? mo_table_size(list) : mo_list_size(list)};
    out.items = vm_run_array(c->arena, out.count, sizeof *out.items, _Alignof(mo_core_string));
    uint32_t position = 0;
    for (uint32_t i = 0; i < out.count; i++) {
        struct mo_cell value, ignored;
        if (set)
            MO_ASSERT(mo_table_next(list, &position, &value, &ignored));
        else
            MO_ASSERT(mo_list_get(list, i, &value));
        out.items[i] = vm_run_string(c, value);
    }
    return out;
}
static MO_MUST_USE mo_core_string_pairs vm_run_string_pairs(struct vm_run_context *c,
                                                            struct mo_cell map) {
    mo_core_string_pairs out = {.count = mo_table_size(map)};
    out.items =
        vm_run_array(c->arena, out.count, sizeof *out.items, _Alignof(struct mo_core_string_pair));
    uint32_t position = 0;
    for (uint32_t i = 0; i < out.count; i++) {
        struct mo_cell key, value;
        MO_ASSERT(mo_table_next(map, &position, &key, &value));
        out.items[i] = (struct mo_core_string_pair){vm_run_string(c, key), vm_run_string(c, value)};
    }
    return out;
}
static void vm_run_effects(struct vm_run_context *c) {
    struct mo_core_effects effects = {.fakes = c->run->world.fakes};
    for (size_t id = 0; id < effects.fakes.count; id++) {
        struct mo_core_cap_state *s = &effects.fakes.items[id];
        for (unsigned kind = 0; kind <= MO_CAP_RANDOM; kind++) {
            struct vm_run_fake *cached = &c->fakes[id * (MO_CAP_RANDOM + 1) + kind];
            if (!cached->present)
                continue;
            struct mo_host_fake_state snapshot;
            MO_ASSERT(mo_host_fake_snapshot(c->host, c->heap, cached->value, &snapshot));
            switch (snapshot.kind) {
            case MO_CAP_ARGS:
                s->args = vm_run_strings(c, snapshot.as.args, false);
                break;
            case MO_CAP_ENV:
                s->env = vm_run_string_pairs(c, snapshot.as.env);
                break;
            case MO_CAP_CONSOLE:
                s->stdin_ = vm_run_strings(c, snapshot.as.console.input, false);
                s->stdout_ = vm_run_strings(c, snapshot.as.console.output, false);
                s->stderr_ = vm_run_strings(c, snapshot.as.console.errors, false);
                break;
            case MO_CAP_FILES:
            case MO_CAP_READ_FILES:
                s->files = vm_run_string_pairs(c, snapshot.as.files.contents);
                s->folders = vm_run_strings(c, snapshot.as.files.folders, true);
                break;
            case MO_CAP_CLOCK:
                break; /* Fixed clocks never advance. */
            case MO_CAP_RANDOM:
                s->random =
                    (struct mo_core_random_state){snapshot.as.random[0], snapshot.as.random[1],
                                                  snapshot.as.random[2], snapshot.as.random[3]};
                break;
            }
            mo_host_fake_state_drop(c->host, c->heap, &snapshot);
        }
    }
    struct mo_buffer out;
    mo_buffer_init(&out, MO_LIMIT_CORE_TEXT_SIZE);
    MO_ASSERT(mo_core_text_print(MO_CORE_TEXT_EFFECTS, &effects, &out));
    MO_ASSERT(fwrite(out.bytes, 1, out.length, stdout) == out.length);
    mo_buffer_free(&out);
}
static void vm_run_dispose(struct vm_run_context *c, const struct mo_bytecode_function *fn) {
    for (uint32_t i = 0; i < fn->parameter_count; i++)
        mo_value_drop(c->heap, fn->parameters[i].type, c->arguments[i]);
    for (size_t i = 0; i < c->run->world.fakes.count * (MO_CAP_RANDOM + 1); i++) {
        if (c->fakes[i].present)
            mo_value_drop(c->heap, c->fakes[i].type, c->fakes[i].value);
    }
    for (unsigned i = 0; i <= MO_CAP_RANDOM; i++) {
        if (c->real[i].present)
            mo_value_drop(c->heap, c->real[i].type, c->real[i].value);
    }
    MO_ASSERT(close(c->folder) == 0);
    for (unsigned i = 0; i < 3; i++)
        MO_ASSERT(close(c->streams[i]) == 0);
    vm_run_clock_active = false;
}

#endif
