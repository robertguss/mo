#define _POSIX_C_SOURCE 200809L

#include "tools/run.h"

#include "base/codes.h"
#include "vm/vm.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
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

int mo_run_program(const struct mo_cli *cli, const struct mo_core *core,
                   const struct mo_buffer *wire, FILE *output, FILE *errors,
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
    enum mo_outcome outcome = mo_image_init(image, &cli->limits, &defect);
    struct mo_process *process = NULL;
    int status = 70, folder = -1;
    bool have_args = false, have_env = false;
    struct mo_cell args, env;
    if (outcome != MO_RETURNED) {
        bool shown = mo_write_defect(errors, NULL, &defect);
        if (!shown)
            (void)fputs("mo: could not write the initialization defect\n", errors);
        goto done;
    }
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
    for (uint32_t i = 0; i < file->functions[main].parameter_count; i++) {
        uint32_t type = file->functions[main].parameters[i].type;
        enum mo_capability cap = mo_image_types(image)->items[type].capability;
        bool ok = true;
        if (cap == MO_CAP_ARGS && !have_args)
            ok = have_args = arguments(cli, host, heap, &args);
        else if (cap == MO_CAP_ENV && !have_env)
            ok = have_env = environment(host, heap, &env);
        else if ((cap == MO_CAP_FILES || cap == MO_CAP_READ_FILES) && folder < 0) {
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
    struct mo_cell result;
    outcome = mo_call_main(process, main, have_args ? &args : NULL, have_env ? &env : NULL, folder,
                           NULL, &result, &defect);
    if (outcome == MO_RETURNED) {
        int64_t value = mo_cell_get_int(result);
        MO_ASSERT(value >= 0 && value <= 255);
        status = (int)value;
        mo_value_drop(heap, file->functions[main].result_type, result);
    } else {
        bool shown = mo_write_defect(errors, process, &defect);
        if (!shown)
            (void)fputs("mo: could not write the defect report\n", errors);
    }
done:
    if (have_args)
        mo_value_drop(mo_process_heap(process), mo_process_host(process)->types->texts, args);
    if (have_env)
        mo_value_drop(mo_process_heap(process), mo_process_host(process)->types->files, env);
    mo_process_free(process);
    if (folder >= 0 && close(folder) != 0)
        status = 70;
    mo_image_free(image);
    return status;
}
