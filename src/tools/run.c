#define _POSIX_C_SOURCE 200809L

#include "tools/run.h"

#include "base/codes.h"
#include "vm/vm.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

extern char **environ;

/* Lowering's entry wrapper, not the source main, turns every supported main
 * result convention into an Int status and supplies error-to-text handling. */
static uint32_t entry(const struct mo_core *core, const struct mo_bytecode *file) {
    for (size_t i = 0; i < core->funs.count; i++) {
        mo_core_string short_name = core->funs.items[i].name;
        if (short_name.length != core->entry.length ||
            memcmp(short_name.bytes, core->entry.bytes, short_name.length) != 0)
            continue;
        mo_core_string name = core->metadata.funs.items[i].name;
        for (uint32_t j = 0; j < file->function_count; j++) {
            const struct mo_bytecode_string *candidate = &file->strings[file->functions[j].name];
            if (candidate->length == name.length &&
                memcmp(candidate->bytes, name.bytes, name.length) == 0)
                return j;
        }
    }
    MO_UNREACHABLE(); /* Successful compilation preserves the checked entry. */
}

struct mo_span mo_runtime_span(const struct mo_bytecode *file, const struct mo_sources *sources,
                               uint32_t path, uint32_t start, uint32_t end) {
    if (path != MO_NO_INDEX) {
        const struct mo_bytecode_bytes *name = &file->files[path];
        for (uint32_t i = 0; i < sources->count; i++)
            if (strlen(sources->files[i].path) == name->length &&
                memcmp(sources->files[i].path, name->bytes, name->length) == 0)
                return (struct mo_span){i, start, end};
    }
    return (struct mo_span){UINT32_MAX, start, end};
}

uint32_t mo_service_function(const struct mo_bytecode *file, const char *module, uint32_t promise,
                             const struct mo_checked_spec *anchor) {
    char suffix[64];
    int n = anchor == NULL ? snprintf(suffix, sizeof suffix, ".service.promise.%u", promise)
                           : snprintf(suffix, sizeof suffix, ".service.promise.%u.%s.%u", promise,
                                      anchor->allows ? "allows" : "forbids", anchor->number);
    MO_ASSERT(n > 0 && (size_t)n < sizeof suffix);
    size_t prefix = strlen(module), length = prefix + (size_t)n;
    for (uint32_t i = 0; i < file->function_count; i++) {
        const struct mo_bytecode_string *name = &file->strings[file->functions[i].name];
        if (name->length == length && memcmp(name->bytes, module, prefix) == 0 &&
            memcmp(name->bytes + prefix, suffix, (size_t)n) == 0)
            return i;
    }
    return MO_NO_INDEX;
}

static bool anchor_error(const struct mo_bytecode *file, const struct mo_sources *sources,
                         uint32_t function, const char *message, const char *detail,
                         const struct mo_span *line, struct mo_diags *diags) {
    struct mo_diag diag = {
        .code = MO_CODE_TOOL_ANCHOR, .message = message, .primary = {{UINT32_MAX, 0, 0}, detail}};
    struct mo_label condition;
    if (function != MO_NO_INDEX) {
        const struct mo_bytecode_debug *debug = &file->debug[function];
        diag.primary.span = mo_runtime_span(file, sources, debug->file, debug->start, debug->end);
        if (line != NULL) {
            condition = (struct mo_label){*line, "this promise condition was false"};
            diag.secondary = &condition;
            diag.secondary_count = 1;
        }
    }
    return mo_diags_add(diags, &diag);
}

static bool anchor_stopped(const struct mo_bytecode *file, const struct mo_sources *sources,
                           uint32_t function, const struct mo_defect *defect,
                           struct mo_diags *diags) {
    struct mo_buffer report;
    mo_buffer_init(&report, MO_LIMIT_DEFECT_TEXT_SIZE + 1);
    bool complete = mo_render_defect(defect->process, defect, &report);
    const char *detail = "anchor execution could not complete; defect report unavailable";
    if (complete && mo_buffer_append_byte(&report, 0))
        detail = (const char *)report.bytes;
    bool added = anchor_error(file, sources, function, "an anchor could not be checked", detail,
                              NULL, diags);
    mo_buffer_free(&report);
    return added;
}

static bool in_library(const struct mo_program *program, uint32_t module) {
    return module != 0 && program->set->library != NULL && program->set->library[module];
}

bool mo_program_has_anchors(const struct mo_program *program) {
    bool anchors = false;
    for (uint32_t i = 0; i < program->spec_count; i++)
        anchors |= program->specs[i].kind == MO_SPEC_ANCHOR &&
                   !in_library(program, program->specs[i].module);
    for (uint32_t i = 0; i < program->service_count; i++)
        for (uint32_t j = 0; j < program->services[i].promise_count; j++)
            anchors |= program->services[i].promises[j].anchor_count > 0 &&
                       !in_library(program, program->services[i].module);
    return anchors;
}

static struct mo_span false_line(const struct mo_bytecode *file, const struct mo_sources *sources,
                                 uint32_t function, const struct mo_checked_spec *service,
                                 int64_t line) {
    if (service != NULL) {
        MO_ASSERT((uint64_t)line <= service->condition_count);
        return service->conditions[line - 1].text.span;
    }
    const struct mo_bytecode_debug *promise = &file->debug[file->debug[function].subject];
    MO_ASSERT((uint64_t)line <= promise->line_count);
    const struct mo_bytecode_spec_line *failed = &promise->lines[(uint32_t)line - 1];
    return mo_runtime_span(file, sources, failed->file, failed->start, failed->end);
}

/* Each anchor is an independent observation of its promise, never a call of
 * the subject. The image is shared; all process heaps and frames are fresh. */
static bool check_anchor(struct mo_image *image, const struct mo_cli *cli,
                         const struct mo_sources *sources, uint32_t function, bool allows,
                         const struct mo_checked_spec *service, struct mo_diags *diags) {
    const struct mo_bytecode *file = mo_image_bytecode(image);
    struct mo_process *process = mo_process_new(image, &cli->limits);
    if (process == NULL) {
        bool added = anchor_error(file, sources, function, "an anchor could not be checked",
                                  "could not allocate its fresh process", NULL, diags);
        (void)added;
        return false;
    }
    mo_process_instruction_limit(process, MO_CLI_ANCHOR_INSTRUCTIONS);
    struct mo_cell result;
    struct mo_defect defect;
    enum mo_outcome outcome = mo_call(process, function, NULL, 0, &result, &defect);
    bool added = true;
    if (outcome == MO_RETURNED) {
        int64_t answer = mo_cell_get_int(result);
        if ((answer <= 0) != allows) {
            struct mo_span line;
            if (answer > 0)
                line = false_line(file, sources, function, service, answer);
            added = anchor_error(file, sources, function,
                                 allows ? "an allows anchor was rejected"
                                        : "a forbids anchor was accepted",
                                 "allows must satisfy the promise; forbids must fail it "
                                 "(reference 12.2)",
                                 answer > 0 ? &line : NULL, diags);
        }
        mo_value_drop(mo_process_heap(process), file->functions[function].result_type, result);
        /* Verified bytecode releases every reference it acquires (bytecode 4.2). */
        MO_ASSERT(mo_heap_objects(mo_process_heap(process)) == 0);
    } else
        added = anchor_stopped(file, sources, function, &defect, diags);
    /* Reporting copies everything before the stopped owner is released. */
    mo_process_free(process);
    return added;
}

bool mo_check_anchors(struct mo_image *image, const struct mo_cli *cli,
                      const struct mo_sources *sources, const struct mo_program *program,
                      struct mo_diags *diags) {
    const struct mo_bytecode *file = mo_image_bytecode(image);
    for (uint32_t i = 0; i < file->function_count; i++)
        if (file->functions[i].kind == MO_FUNCTION_ANCHOR &&
            !check_anchor(image, cli, sources, i, file->debug[i].expectation == MO_BYTECODE_ALLOWS,
                          NULL, diags))
            return false;
    for (uint32_t i = 0; i < program->service_count; i++) {
        const struct mo_checked_service *service = &program->services[i];
        for (uint32_t j = 0; j < service->promise_count; j++) {
            const struct mo_checked_service_promise *promise = &service->promises[j];
            uint32_t module = promise->spec.module;
            if (in_library(program, module))
                continue;
            for (uint32_t k = 0; k < promise->anchor_count; k++) {
                const struct mo_checked_spec *anchor = &promise->anchors[k];
                uint32_t function = mo_service_function(file, program->set->names[module],
                                                        promise->spec.number, anchor);
                MO_ASSERT(function != MO_NO_INDEX);
                if (!check_anchor(image, cli, sources, function, anchor->allows, &promise->spec,
                                  diags))
                    return false;
            }
        }
    }
    return !mo_diags_any(diags);
}

static bool arguments(const struct mo_cli *cli, struct mo_host *host, struct mo_heap *heap,
                      struct mo_cell *out) {
    if (!mo_value_list(heap, host->types->texts, NULL, 0, out))
        return false;
    for (uint32_t i = 0; i < cli->argument_count; i++) {
        struct mo_cell value;
        const char *arg = cli->arguments[i];
        bool ok = mo_value_text(heap, arg, strlen(arg), &value);
        if (ok) {
            ok = mo_list_append(heap, host->types->texts, out, value);
            mo_value_drop(heap, host->types->text, value);
        }
        if (!ok) {
            mo_value_drop(heap, host->types->texts, *out);
            return false;
        }
    }
    return true;
}

static bool environment(struct mo_host *host, struct mo_heap *heap, struct mo_cell *out) {
    if (!mo_value_map(heap, host->types->files, NULL, NULL, 0, out))
        return false;
    for (char **item = environ; item != NULL && *item != NULL; item++) {
        const char *equals = strchr(*item, '=');
        if (equals == NULL)
            continue; /* Only NAME=VALUE entries represent environment bindings. */
        struct mo_cell key, value;
        bool ok = mo_value_text(heap, *item, (size_t)(equals - *item), &key);
        if (ok) {
            ok = mo_value_text(heap, equals + 1, strlen(equals + 1), &value);
            if (ok) {
                ok = mo_table_set(heap, host->types->files, out, key, value);
                mo_value_drop(heap, host->types->text, value);
            }
            mo_value_drop(heap, host->types->text, key);
        }
        if (!ok) {
            mo_value_drop(heap, host->types->files, *out);
            return false;
        }
    }
    return true;
}

static int stopped(struct mo_process *process, const char *reason, const char *limit, size_t amount,
                   FILE *errors) {
    struct mo_defect defect = {.reason = reason,
                               .limit_name = limit,
                               .limit = amount,
                               .span.file = MO_NO_INDEX,
                               .process = process};
    bool shown = mo_write_defect(errors, process, &defect);
    if (!shown)
        (void)fputs("mo: could not write the defect report\n", errors);
    return 70;
}

static bool is_bytecode_actor(const struct mo_bytecode *file, uint32_t service_idx) {
    const struct mo_bytecode_string *tok = &file->strings[file->services[service_idx].token_env];
    return tok->length == sizeof "_MO_ACTOR" - 1 &&
           memcmp(tok->bytes, "_MO_ACTOR", sizeof "_MO_ACTOR" - 1) == 0;
}

static bool find_actor_service_for_param(const struct mo_bytecode *file, uint32_t main_fn,
                                         uint32_t param_idx, uint32_t *service_out) {
    static const char prefix[] = "%actor.";
    size_t plen = sizeof prefix - 1;
    const struct mo_bytecode_debug *dbg = &file->debug[main_fn];
    for (uint32_t i = 0; i < dbg->name_count; i++) {
        if (dbg->names[i].reg != param_idx || dbg->names[i].code_start != 0)
            continue;
        const struct mo_bytecode_string *nm = &file->strings[dbg->names[i].name];
        if (nm->length <= plen || memcmp(nm->bytes, prefix, plen) != 0)
            return false;
        const uint8_t *mod_bytes = nm->bytes + plen;
        size_t mod_len = nm->length - plen;
        for (uint32_t s = 0; s < file->service_count; s++) {
            if (!is_bytecode_actor(file, s))
                continue;
            const struct mo_bytecode_string *mname =
                &file->strings[file->modules[file->services[s].module].name];
            if (mname->length == mod_len && memcmp(mname->bytes, mod_bytes, mod_len) == 0) {
                *service_out = s;
                return true;
            }
        }
        return false;
    }
    return false;
}

static int run_loaded_main(const struct mo_cli *cli, struct mo_image *image, uint32_t main,
                           FILE *output, FILE *errors) {
    const struct mo_bytecode *file = mo_image_bytecode(image);
    const struct mo_types *types = mo_image_types(image);
    const struct mo_bytecode_function *fn = &file->functions[main];
    struct mo_process *process = NULL;
    int status = 70, folder = -1;
    bool have_args = false, have_env = false, has_actors = false;
    struct mo_cell args, env;
    struct mo_live_actor *live_actors[MO_LIMIT_BYTECODE_PARAMETERS] = {NULL};
    struct mo_cell supplied_cells[MO_LIMIT_BYTECODE_PARAMETERS] = {{{0}}};
    bool supplied_owned_by_us[MO_LIMIT_BYTECODE_PARAMETERS] = {false};
    struct mo_cell cached_host_caps[MO_CAP_DATABASE + 1] = {{{0}}};
    bool cached_host_set[MO_CAP_DATABASE + 1] = {false};

    errno = 0;
    process = mo_process_new(image, &cli->limits);
    if (process == NULL) {
        status = stopped(NULL, "running out of memory", errno == ENOMEM ? NULL : "--max-heap",
                         cli->limits.heap_size, errors);
        goto done;
    }
    struct mo_host *host = mo_process_host(process);
    struct mo_heap *heap = mo_process_heap(process);
    if (fflush(output) != 0 || fflush(errors) != 0 || fileno(output) < 0 || fileno(errors) < 0) {
        status = stopped(process, "could not prepare console output", NULL, 0, errors);
        goto done;
    }
    host->output = fileno(output);
    host->errors = fileno(errors);
    for (uint32_t i = 0; i < fn->parameter_count; i++) {
        uint32_t type = fn->parameters[i].type;
        enum mo_capability cap = types->items[type].capability;
        bool ok = true;
        if (cap == MO_CAP_SIMULATION)
            has_actors = true;
        else if (cap == MO_CAP_ARGS && !have_args)
            ok = have_args = arguments(cli, host, heap, &args);
        else if (cap == MO_CAP_ENV && !have_env)
            ok = have_env = environment(host, heap, &env);
        else if ((cap == MO_CAP_FILES || cap == MO_CAP_READ_FILES || cap == MO_CAP_SQLITE) &&
                 folder < 0) {
            folder = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            if (folder < 0) {
                status = stopped(process, "could not open the starting folder", NULL, 0, errors);
                goto done;
            }
        }
        if (!ok) {
            const char *reason = errno == EINVAL ? "arguments or environment are not valid UTF-8"
                                                 : "running out of memory";
            const char *limit = errno == ENOSPC ? "--max-heap" : NULL;
            status = stopped(process, reason, limit, cli->limits.heap_size, errors);
            goto done;
        }
    }

    struct mo_main_capabilities supplied = {0};
    if (has_actors) {
        const char *setup = mo_host_start(host);
        if (setup != NULL) {
            status = stopped(process, setup, NULL, 0, errors);
            goto done;
        }
        for (uint32_t i = 0; i < fn->parameter_count; i++) {
            uint32_t type = fn->parameters[i].type;
            enum mo_capability cap = types->items[type].capability;
            if (cap == MO_CAP_SIMULATION) {
                uint32_t svc_idx = 0;
                if (!find_actor_service_for_param(file, main, i, &svc_idx)) {
                    status =
                        stopped(process, "could not resolve main actor parameter", NULL, 0, errors);
                    goto done;
                }
                enum mo_service_status ast =
                    mo_live_actor_new(image, svc_idx, errors, &live_actors[i]);
                if (ast != MO_SERVICE_OK) {
                    status = stopped(process, mo_service_status_text(ast), NULL, 0, errors);
                    goto done;
                }
                if (!mo_value_live_actor(heap, type, live_actors[i], &supplied_cells[i])) {
                    status = stopped(process, "running out of memory", "--max-heap",
                                     cli->limits.heap_size, errors);
                    goto done;
                }
                supplied_owned_by_us[i] = true;
            } else {
                if (!cached_host_set[cap]) {
                    struct mo_host_error herr;
                    const struct mo_cell *contents = cap == MO_CAP_ARGS  ? &args
                                                     : cap == MO_CAP_ENV ? &env
                                                                         : NULL;
                    enum mo_host_outcome hout = mo_host_real(host, heap, type, contents, folder,
                                                             &cached_host_caps[cap], &herr);
                    if (hout != MO_HOST_RETURNED) {
                        const char *reason =
                            hout == MO_HOST_OUT_OF_MEMORY ? "running out of memory" : herr.reason;
                        const char *limit =
                            hout == MO_HOST_OUT_OF_MEMORY && errno != ENOMEM ? "--max-heap" : NULL;
                        status = stopped(process, reason, limit, cli->limits.heap_size, errors);
                        goto done;
                    }
                    cached_host_set[cap] = true;
                }
                supplied_cells[i] = cached_host_caps[cap];
            }
        }
        supplied =
            (struct mo_main_capabilities){.values = supplied_cells, .count = fn->parameter_count};
    }

    struct mo_cell result;
    struct mo_defect defect;
    enum mo_outcome outcome =
        mo_call_main(process, main, have_args ? &args : NULL, have_env ? &env : NULL, folder,
                     has_actors ? &supplied : NULL, &result, &defect);
    if (outcome == MO_RETURNED) {
        int64_t value = mo_cell_get_int(result);
        MO_ASSERT(value >= 0 && value <= 255);
        status = (int)value;
        mo_value_drop(heap, fn->result_type, result);
    } else {
        bool shown = mo_write_defect(errors, process, &defect);
        if (!shown)
            (void)fputs("mo: could not write the defect report\n", errors);
    }
done:
    if (process != NULL) {
        for (uint32_t i = 0; i < fn->parameter_count; i++)
            if (supplied_owned_by_us[i])
                mo_value_drop(mo_process_heap(process), fn->parameters[i].type, supplied_cells[i]);
        for (uint32_t c = 0; c <= MO_CAP_DATABASE; c++)
            if (cached_host_set[c]) {
                uint32_t cty = 0;
                for (uint32_t i = 0; i < fn->parameter_count; i++)
                    if (types->items[fn->parameters[i].type].capability == (enum mo_capability)c) {
                        cty = fn->parameters[i].type;
                        break;
                    }
                mo_value_drop(mo_process_heap(process), cty, cached_host_caps[c]);
            }
        if (have_args)
            mo_value_drop(mo_process_heap(process), mo_process_host(process)->types->texts, args);
        if (have_env)
            mo_value_drop(mo_process_heap(process), mo_process_host(process)->types->files, env);
    }
    mo_process_free(process);
    for (uint32_t i = 0; i < fn->parameter_count; i++)
        mo_live_actor_free(live_actors[i]);
    if (folder >= 0 && close(folder) != 0)
        status = 70;
    return status;
}

bool mo_is_bytecode_path(const char *path) {
    size_t n = strlen(path);
    if (n >= 3 && strcmp(path + n - 3, ".bc") == 0)
        return true;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NONBLOCK);
    if (fd < 0)
        return false;
    struct stat st;
    uint8_t magic[4] = {0};
    bool is_bc = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && read(fd, magic, 4) == 4 &&
                 memcmp(magic, "MOBC", 4) == 0;
    (void)close(fd);
    return is_bc;
}

static bool valid_bytecode_entry(const struct mo_image *image, uint32_t main) {
    const struct mo_bytecode *file = mo_image_bytecode(image);
    const struct mo_types *types = mo_image_types(image);
    const struct mo_bytecode_function *fn = &file->functions[main];
    if (fn->kind != MO_FUNCTION_FUNCTION || types->items[fn->result_type].kind != MO_TYPE_INT)
        return false;
    uint32_t seen = 0;
    uint32_t seen_actors[MO_LIMIT_BYTECODE_PARAMETERS];
    uint32_t actor_n = 0;
    for (uint32_t i = 0; i < fn->parameter_count; i++) {
        const struct mo_type *t = &types->items[fn->parameters[i].type];
        if (t->kind != MO_TYPE_CAPABILITY || t->capability >= 31)
            return false;
        if (t->capability == MO_CAP_SIMULATION) {
            uint32_t svc_idx = 0;
            if (!find_actor_service_for_param(file, main, i, &svc_idx))
                return false;
            for (uint32_t prev = 0; prev < actor_n; prev++)
                if (seen_actors[prev] == svc_idx)
                    return false;
            seen_actors[actor_n++] = svc_idx;
            continue;
        }
        uint32_t bit = UINT32_C(1) << t->capability;
        if ((seen & bit) != 0)
            return false;
        seen |= bit;
    }
    return true;
}

static void bytecode_error(struct mo_diags *diags, uint16_t code, const char *message,
                           const char *detail) {
    struct mo_diag diag = {
        .code = code, .message = message, .primary = {{UINT32_MAX, 0, 0}, detail}};
    bool added = mo_diags_add(diags, &diag);
    (void)added;
}

int mo_run_bytecode(const struct mo_cli *cli, const char *path, FILE *output, FILE *errors,
                    struct mo_diags *diags) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        bytecode_error(diags, MO_CODE_TOOL_PIPELINE, "bytecode could not be loaded",
                       strerror(errno));
        return 1;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        int err = errno;
        (void)close(fd);
        bytecode_error(diags, MO_CODE_TOOL_PIPELINE, "bytecode could not be loaded",
                       err != 0 ? strerror(err) : "not a regular file");
        return 1;
    }
    if (st.st_size < 0 || (uint64_t)st.st_size > MO_LIMIT_BYTECODE_SIZE) {
        (void)close(fd);
        bytecode_error(diags, MO_CODE_TOOL_PIPELINE, "bytecode could not be loaded",
                       "bytecode exceeds MO_LIMIT_BYTECODE_SIZE");
        return 1;
    }
    struct mo_buffer wire;
    mo_buffer_init(&wire, MO_LIMIT_BYTECODE_SIZE);
    uint8_t chunk[8192];
    bool ok = true;
    for (;;) {
        ssize_t got = read(fd, chunk, sizeof chunk);
        if (got == 0)
            break;
        if (got < 0) {
            if (errno == EINTR)
                continue;
            ok = false;
            break;
        }
        if (!mo_buffer_append(&wire, chunk, (size_t)got)) {
            ok = false;
            break;
        }
    }
    int read_err = errno;
    if (close(fd) != 0 && ok) {
        ok = false;
        read_err = errno;
    }
    if (!ok) {
        mo_buffer_free(&wire);
        bytecode_error(diags, MO_CODE_TOOL_PIPELINE, "bytecode could not be loaded",
                       read_err != 0 ? strerror(read_err) : "read failed");
        return 1;
    }
    struct mo_load_error load_err;
    struct mo_image *image = mo_load(wire.bytes, wire.length, &load_err);
    mo_buffer_free(&wire);
    if (image == NULL) {
        bytecode_error(diags, MO_CODE_TOOL_PIPELINE, "bytecode could not be loaded",
                       load_err.reason);
        return 1;
    }
    const struct mo_bytecode *file = mo_image_bytecode(image);
    for (uint32_t i = 0; i < file->service_count; i++) {
        if (file->services[i].module == 0 && !is_bytecode_actor(file, i)) {
            bytecode_error(diags, MO_CODE_TOOL_SERVICE_UNAVAILABLE,
                           "mo run does not start services yet", NULL);
            mo_image_free(image);
            return 1;
        }
    }
    uint32_t main = MO_NO_INDEX;
    if (file->module_count > 0) {
        const struct mo_bytecode_string *mod0 = &file->strings[file->modules[0].name];
        static const char suffix[] = ".main.entry";
        size_t want = (size_t)mod0->length + (sizeof suffix - 1);
        for (uint32_t i = 0; i < file->function_count; i++) {
            if (file->functions[i].module != 0 || file->functions[i].kind != MO_FUNCTION_FUNCTION)
                continue;
            const struct mo_bytecode_string *name = &file->strings[file->functions[i].name];
            if (name->length == want && memcmp(name->bytes, mod0->bytes, mod0->length) == 0 &&
                memcmp(name->bytes + mod0->length, suffix, sizeof suffix - 1) == 0) {
                main = i;
                break;
            }
        }
    }
    if (main == MO_NO_INDEX) {
        bytecode_error(diags, MO_CODE_MAIN_CONTRACT, "the entry module has no main function", NULL);
        mo_image_free(image);
        return 1;
    }
    if (!valid_bytecode_entry(image, main)) {
        bytecode_error(diags, MO_CODE_TOOL_PIPELINE, "bytecode could not be loaded",
                       "entry function does not satisfy main's capability contract");
        mo_image_free(image);
        return 1;
    }
    struct mo_defect defect;
    if (mo_image_init(image, &cli->limits, &defect) != MO_RETURNED) {
        bool shown = mo_write_defect(errors, NULL, &defect);
        if (!shown)
            (void)fputs("mo: could not write the initialization defect\n", errors);
        mo_image_free(image);
        return 70;
    }
    int status = run_loaded_main(cli, image, main, output, errors);
    mo_image_free(image);
    return status;
}

int mo_run_program(const struct mo_cli *cli, const struct mo_program *program,
                   const struct mo_core *core, const struct mo_buffer *wire,
                   const struct mo_sources *sources, FILE *output, FILE *errors,
                   struct mo_diags *diags) {
    struct mo_load_error error;
    struct mo_image *image = mo_load(wire->bytes, wire->length, &error);
    if (image == NULL) {
        struct mo_diag diag = {.code = MO_CODE_TOOL_PIPELINE,
                               .message = "compiled bytecode could not be loaded",
                               .primary = {{UINT32_MAX, 0, 0}, error.reason}};
        bool added = mo_diags_add(diags, &diag);
        (void)added;
        return 1;
    }
    const struct mo_bytecode *file = mo_image_bytecode(image);
    uint32_t main = entry(core, file);
    struct mo_defect defect;
    bool anchors = mo_program_has_anchors(program);
    enum mo_outcome outcome =
        anchors ? mo_image_init_bounded(image, &cli->limits, MO_CLI_ANCHOR_INSTRUCTIONS, &defect)
                : mo_image_init(image, &cli->limits, &defect);
    int status = 70;
    if (outcome != MO_RETURNED) {
        if (anchors) {
            bool added = anchor_stopped(file, sources, MO_NO_INDEX, &defect, diags);
            (void)added;
            status = 1;
        } else {
            bool shown = mo_write_defect(errors, NULL, &defect);
            if (!shown)
                (void)fputs("mo: could not write the initialization defect\n", errors);
        }
        mo_image_free(image);
        return status;
    }
    if (!mo_check_anchors(image, cli, sources, program, diags)) {
        mo_image_free(image);
        return 1;
    }
    if (cli->command == MO_COMMAND_CHECK || cli->command == MO_COMMAND_EMIT) {
        mo_image_free(image);
        return 0;
    }
    status = run_loaded_main(cli, image, main, output, errors);
    mo_image_free(image);
    return status;
}
