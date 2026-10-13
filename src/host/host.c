#define _POSIX_C_SOURCE 200809L
#include "host/files.h"

#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>

const char *mo_host_start(struct mo_host *host) {
    if (host->started)
        return NULL;
    struct rlimit budget;
    if (getrlimit(RLIMIT_NOFILE, &budget) != 0)
        return "internal error: descriptor budget unavailable";
    rlim_t wanted = MO_LIMIT_OPEN_FOLDERS + 32u;
    if (wanted > budget.rlim_max)
        wanted = budget.rlim_max;
    if (budget.rlim_cur < wanted) {
        budget.rlim_cur = wanted;
        if (setrlimit(RLIMIT_NOFILE, &budget) != 0) {
            /* Best effort: a sandbox may forbid the raise. Keep its existing
             * budget; a later open reports descriptor exhaustion normally. */
        }
    }
    struct sigaction action = {.sa_handler = SIG_IGN};
    if (sigemptyset(&action.sa_mask) != 0 || sigaction(SIGPIPE, &action, NULL) != 0)
        return "internal error: could not ignore SIGPIPE";
    host->started = true;
    return NULL;
}

#define OPERATIONS(fake_)                                                                          \
    {                                                                                              \
        {MO_CAP_ARGS, fake_, mo_host_misc}, {MO_CAP_ENV, fake_, mo_host_misc},                     \
            {MO_CAP_CONSOLE, fake_, mo_host_console}, {MO_CAP_FILES, fake_, mo_host_files},        \
            {MO_CAP_READ_FILES, fake_, mo_host_files}, {MO_CAP_CLOCK, fake_, mo_host_misc}, {      \
            MO_CAP_RANDOM, fake_, mo_host_misc                                                     \
        }                                                                                          \
    }
const struct mo_host_operations mo_host_operations[2][MO_CAP_RANDOM + 1] = {OPERATIONS(false),
                                                                            OPERATIONS(true)};
#undef OPERATIONS

enum mo_host_outcome mo_host_bug(struct mo_host_error *error, const char *reason) {
    *error = (struct mo_host_error){.reason = reason};
    return MO_HOST_BUG;
}

uint32_t mo_host_tag(const struct mo_host *host, uint32_t type, const char *name) {
    const struct mo_type *t = &host->types->types.items[type];
    MO_ASSERT(t->kind == MO_TYPE_CHOICE);
    for (uint32_t i = 0; i < t->case_count; i++) {
        if (strcmp(t->cases[i].name, name) == 0)
            return i;
    }
    MO_UNREACHABLE();
}

bool mo_host_case(struct mo_host *host, struct mo_heap *heap, uint32_t type, const char *name,
                  const struct mo_cell *fields, uint32_t count, struct mo_cell *out) {
    return mo_value_case(heap, type, mo_host_tag(host, type, name), fields, count, out);
}

enum mo_host_outcome mo_host_state_cap(struct mo_host *host, struct mo_heap *heap, uint32_t type,
                                       bool fake, const uint32_t *types,
                                       const struct mo_cell *values, uint32_t count,
                                       struct mo_cell *out) {
    enum mo_capability cap = host->types->types.items[type].capability;
    struct mo_state *state;
    if (!mo_state_new(heap, types, values, count, &state))
        return MO_HOST_OUT_OF_MEMORY;
    struct mo_capability_info info = {.operations = &mo_host_operations[fake][cap], .state = state};
    bool ok = mo_value_capability(heap, type, &info, out);
    mo_state_drop(heap, state);
    return ok ? MO_HOST_RETURNED : MO_HOST_OUT_OF_MEMORY;
}

enum mo_host_outcome mo_host_call(struct mo_host *host, struct mo_heap *heap, uint32_t operation,
                                  uint32_t type, const struct mo_cell *args, struct mo_cell *out,
                                  struct mo_host_error *error) {
    MO_ASSERT(host != NULL && heap != NULL && out != NULL && error != NULL);
    *error = (struct mo_host_error){0};
    if (operation == MO_HOST_CLOCK_FIXED)
        return mo_host_state_cap(host, heap, type, true, &host->types->time, args, 1, out);
    if (operation == MO_HOST_RANDOM_SEEDED)
        return mo_host_random_new(host, heap, type, true, (uint64_t)mo_cell_get_int(args[0]), out,
                                  error);
    if (operation == MO_HOST_FILES_IN_MEMORY) {
        enum mo_host_outcome result = mo_host_files_fake(host, heap, type, args[0], out, error);
        if (result == MO_HOST_RETURNED)
            mo_value_drop(heap, host->types->files, args[0]);
        return result;
    }
    if (operation == MO_HOST_CONSOLE_FAKE || operation == MO_HOST_CONSOLE_FAKE_INPUT) {
        enum mo_host_outcome result = mo_host_console_fake(
            host, heap, type, operation == MO_HOST_CONSOLE_FAKE_INPUT ? args : NULL, out);
        if (result == MO_HOST_RETURNED && operation == MO_HOST_CONSOLE_FAKE_INPUT)
            mo_value_drop(heap, host->types->texts, args[0]);
        return result;
    }
    if (operation == MO_HOST_ARGS_OF || operation == MO_HOST_ENV_OF) {
        uint32_t slot = operation == MO_HOST_ARGS_OF ? host->types->texts : host->types->files;
        enum mo_host_outcome result =
            mo_host_state_cap(host, heap, type, true, &slot, args, 1, out);
        if (result == MO_HOST_RETURNED)
            mo_value_drop(heap, slot, args[0]);
        return result;
    }
    const struct mo_capability_info *info = mo_capability_get(args[0]);
    const struct mo_host_operations *operations = info->operations;
    return operations->call(host, heap, operation, type, args, out, error);
}

enum mo_host_outcome mo_host_misc(struct mo_host *host, struct mo_heap *heap, uint32_t operation,
                                  uint32_t type, const struct mo_cell *args, struct mo_cell *out,
                                  struct mo_host_error *error) {
    const struct mo_capability_info *info = mo_capability_get(args[0]);
    if (operation == MO_HOST_ARGS_LIST) {
        *out = mo_value_dup(heap, type, mo_state_get(info->state, 0));
        return MO_HOST_RETURNED;
    }
    if (operation == MO_HOST_ENV_GET) {
        struct mo_cell value;
        bool found;
        if (!mo_table_get(heap, host->types->files, mo_state_get(info->state, 0), args[1], &found,
                          &value))
            return MO_HOST_OUT_OF_MEMORY;
        return mo_host_case(host, heap, type, found ? "Some" : "None", found ? &value : NULL,
                            found ? 1 : 0, out)
                   ? MO_HOST_RETURNED
                   : MO_HOST_OUT_OF_MEMORY;
    }
    if (operation == MO_HOST_RANDOM_INT)
        return mo_host_random(host, heap, type, args, out, error);
    MO_ASSERT(operation == MO_HOST_CLOCK_NOW);
    const struct mo_host_operations *operations = info->operations;
    if (operations->fake) {
        *out = mo_state_get(info->state, 0);
        return MO_HOST_RETURNED;
    }
    struct timespec now;
    if (clock_gettime(CLOCK_REALTIME, &now) != 0) {
        *error = (struct mo_host_error){.reason = "internal error: real-time clock unavailable"};
        return MO_HOST_INTERNAL;
    }
    MO_ASSERT(now.tv_nsec >= 0 && now.tv_nsec < 1000000000);
    mo_i128 microseconds = (mo_i128)now.tv_sec * 1000000 + now.tv_nsec / 1000;
    if (microseconds < INT64_C(-62135596800000000) || microseconds > INT64_C(253402300799999999))
        return mo_host_bug(error, "time outside the years 1 to 9999");
    *out = mo_cell_time((int64_t)microseconds);
    return MO_HOST_RETURNED;
}

enum mo_host_outcome mo_host_files(struct mo_host *host, struct mo_heap *heap, uint32_t operation,
                                   uint32_t type, const struct mo_cell *args, struct mo_cell *out,
                                   struct mo_host_error *error) {
    const struct mo_capability_info *info = mo_capability_get(args[0]);
    const struct mo_host_operations *operations = info->operations;
    if (operations->fake)
        return mo_host_fake_files_call(host, heap, operation, type, args, out, error);
    return mo_host_real_files_call(host, heap, operation, type, args, out, error);
}

enum mo_host_outcome mo_host_real(struct mo_host *host, struct mo_heap *heap, uint32_t type,
                                  const struct mo_cell *contents, int folder, struct mo_cell *out,
                                  struct mo_host_error *error) {
    enum mo_capability cap = host->types->types.items[type].capability;
    if (cap == MO_CAP_FILES || cap == MO_CAP_READ_FILES) {
        if (host->open_folders == MO_LIMIT_OPEN_FOLDERS)
            return mo_host_bug(error, "too many folders open at once");
        int fd = fcntl(folder, F_DUPFD_CLOEXEC, 0);
        if (fd < 0)
            return mo_host_bug(error, "too many folders open at once");
        enum mo_host_outcome result = mo_host_folder_cap(host, heap, type, fd, NULL, out, error);
        if (result != MO_HOST_RETURNED)
            mo_host_close(fd);
        return result;
    }
    if (cap == MO_CAP_ARGS || cap == MO_CAP_ENV) {
        MO_ASSERT(contents != NULL);
        uint32_t slot = cap == MO_CAP_ARGS ? host->types->texts : host->types->files;
        return mo_host_state_cap(host, heap, type, false, &slot, contents, 1, out);
    }
    if (cap == MO_CAP_CONSOLE)
        return mo_host_console_real(heap, type, out);
    if (cap == MO_CAP_CLOCK) {
        struct mo_capability_info info = {.operations = &mo_host_operations[false][cap]};
        return mo_value_capability(heap, type, &info, out) ? MO_HOST_RETURNED
                                                           : MO_HOST_OUT_OF_MEMORY;
    }
    MO_ASSERT(cap == MO_CAP_RANDOM);
    return mo_host_random_new(host, heap, type, false, 0, out, error);
}

enum mo_host_outcome mo_host_fake_restore(struct mo_host *host, struct mo_heap *heap, uint32_t type,
                                          const struct mo_host_fake_state *initial,
                                          struct mo_cell *out, struct mo_host_error *error) {
    MO_ASSERT(host != NULL && heap != NULL && initial != NULL && out != NULL && error != NULL);
    MO_ASSERT(type < host->types->types.count &&
              host->types->types.items[type].kind == MO_TYPE_CAPABILITY &&
              host->types->types.items[type].capability == initial->kind);
    *error = (struct mo_host_error){0};
    switch (initial->kind) {
    case MO_CAP_ARGS:
        return mo_host_state_cap(host, heap, type, true, &host->types->texts, &initial->as.args, 1,
                                 out);
    case MO_CAP_ENV:
        return mo_host_state_cap(host, heap, type, true, &host->types->files, &initial->as.env, 1,
                                 out);
    case MO_CAP_CONSOLE: {
        uint32_t types[] = {host->types->texts, host->types->texts, host->types->texts,
                            host->types->integer};
        struct mo_cell values[] = {initial->as.console.input, initial->as.console.output,
                                   initial->as.console.errors, mo_cell_int(0)};
        return mo_host_state_cap(host, heap, type, true, types, values, 4, out);
    }
    case MO_CAP_FILES:
    case MO_CAP_READ_FILES: {
        struct mo_cell root;
        if (!mo_value_text(heap, "", 0, &root))
            return MO_HOST_OUT_OF_MEMORY;
        uint32_t types[] = {host->types->files, host->types->folders};
        struct mo_cell values[] = {initial->as.files.contents, initial->as.files.folders};
        struct mo_state *state;
        bool ok = mo_state_new(heap, types, values, 2, &state);
        if (ok) {
            struct mo_capability_info info = {.operations =
                                                  &mo_host_operations[true][initial->kind],
                                              .state = state,
                                              .has_prefix = true,
                                              .prefix = root,
                                              .read_only = initial->kind == MO_CAP_READ_FILES};
            ok = mo_value_capability(heap, type, &info, out);
            mo_state_drop(heap, state);
        }
        mo_value_drop(heap, host->types->text, root);
        return ok ? MO_HOST_RETURNED : MO_HOST_OUT_OF_MEMORY;
    }
    case MO_CAP_CLOCK: {
        MO_ASSERT(initial->as.time >= INT64_C(-62135596800000000) &&
                  initial->as.time <= INT64_C(253402300799999999));
        struct mo_cell time = mo_cell_time(initial->as.time);
        return mo_host_state_cap(host, heap, type, true, &host->types->time, &time, 1, out);
    }
    case MO_CAP_RANDOM:
        if ((initial->as.random[0] | initial->as.random[1] | initial->as.random[2] |
             initial->as.random[3]) == 0)
            return mo_host_bug(error, "random state is all zero");
        return mo_host_random_restore(heap, type, true, initial->as.random, out)
                   ? MO_HOST_RETURNED
                   : MO_HOST_OUT_OF_MEMORY;
    }
    MO_UNREACHABLE();
}

bool mo_host_fake_snapshot(struct mo_host *host, struct mo_heap *heap, struct mo_cell capability,
                           struct mo_host_fake_state *out) {
    const struct mo_capability_info *info = mo_capability_get(capability);
    const struct mo_host_operations *operations = info->operations;
    MO_ASSERT(operations->fake);
    struct mo_host_fake_state result = {.kind = operations->capability};
    switch (result.kind) {
    case MO_CAP_ARGS:
        result.as.args = mo_value_dup(heap, host->types->texts, mo_state_get(info->state, 0));
        break;
    case MO_CAP_ENV:
        result.as.env = mo_value_dup(heap, host->types->files, mo_state_get(info->state, 0));
        break;
    case MO_CAP_CONSOLE: {
        struct mo_cell input = mo_state_get(info->state, 0);
        uint32_t start = (uint32_t)mo_cell_get_int(mo_state_get(info->state, 3));
        uint32_t count = mo_list_size(input);
        MO_ASSERT(start <= count);
        result.as.console.input = mo_value_dup(heap, host->types->texts, input);
        if (!mo_list_slice(heap, host->types->texts, &result.as.console.input, start,
                           count - start)) {
            mo_value_drop(heap, host->types->texts, result.as.console.input);
            return false;
        }
        result.as.console.output =
            mo_value_dup(heap, host->types->texts, mo_state_get(info->state, 1));
        result.as.console.errors =
            mo_value_dup(heap, host->types->texts, mo_state_get(info->state, 2));
        break;
    }
    case MO_CAP_FILES:
    case MO_CAP_READ_FILES:
        result.as.files.contents =
            mo_value_dup(heap, host->types->files, mo_state_get(info->state, 0));
        result.as.files.folders =
            mo_value_dup(heap, host->types->folders, mo_state_get(info->state, 1));
        break;
    case MO_CAP_CLOCK:
        result.as.time = mo_cell_get_time(mo_state_get(info->state, 0));
        break;
    case MO_CAP_RANDOM:
        mo_host_random_snapshot(capability, result.as.random);
        break;
    }
    *out = result;
    return true;
}

void mo_host_fake_state_drop(struct mo_host *host, struct mo_heap *heap,
                             struct mo_host_fake_state *state) {
    switch (state->kind) {
    case MO_CAP_ARGS:
        mo_value_drop(heap, host->types->texts, state->as.args);
        break;
    case MO_CAP_ENV:
        mo_value_drop(heap, host->types->files, state->as.env);
        break;
    case MO_CAP_CONSOLE:
        mo_value_drop(heap, host->types->texts, state->as.console.input);
        mo_value_drop(heap, host->types->texts, state->as.console.output);
        mo_value_drop(heap, host->types->texts, state->as.console.errors);
        break;
    case MO_CAP_FILES:
    case MO_CAP_READ_FILES:
        mo_value_drop(heap, host->types->files, state->as.files.contents);
        mo_value_drop(heap, host->types->folders, state->as.files.folders);
        break;
    case MO_CAP_CLOCK:
    case MO_CAP_RANDOM:
        break;
    }
}
