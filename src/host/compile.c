#define _POSIX_C_SOURCE 200809L
#include "host/host.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

static bool limit(int resource, rlim_t value) {
    struct rlimit bounds = {value, value};
    return setrlimit(resource, &bounds) == 0;
}
static pid_t reap(pid_t pid, int *status, int options) {
    pid_t result;
    do {
        result = waitpid(pid, status, options);
    } while (result < 0 && errno == EINTR);
    return result;
}
void mo_compile_cancel(struct mo_compile_child *child) {
    if (child->pid <= 0)
        return;
    (void)kill(child->pid, SIGKILL);
    int status;
    (void)reap(child->pid, &status, 0);
    child->pid = 0;
}
struct child_limits {
    rlim_t memory, cpu;
    uint64_t wall;
};
static enum mo_compile_status spawn(const char *const *arguments, unsigned count,
                                    const char *diagnostics, struct child_limits limits,
                                    char *const *environment, uint64_t now,
                                    struct mo_compile_child *child) {
    MO_ASSERT(child != NULL && child->pid == 0 && count < 16 && arguments[count] == NULL);
    if (now > UINT64_MAX - limits.wall)
        return MO_COMPILE_IO;
    int errors = open(diagnostics, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (errors < 0)
        return MO_COMPILE_IO;
    long descriptors = sysconf(_SC_OPEN_MAX);
    if (descriptors < 0 || descriptors > INT_MAX) {
        (void)close(errors);
        return MO_COMPILE_IO;
    }
    pid_t pid = fork();
    if (pid == 0) {
        int input = open("/dev/null", O_RDWR);
        if (input < 0 || dup2(input, STDIN_FILENO) < 0 || dup2(input, STDOUT_FILENO) < 0 ||
            dup2(errors, STDERR_FILENO) < 0 ||
            (!MO_UPGRADE_COMPILE_ASAN && !limit(RLIMIT_AS, limits.memory)) ||
            !limit(RLIMIT_CPU, limits.cpu) || !limit(RLIMIT_CORE, 0))
            _exit(125);
        for (int fd = 3; fd < descriptors; fd++)
            (void)close(fd);
        char *mutable[16];
        for (unsigned i = 0; i <= count; i++)
            memcpy(&mutable[i], &arguments[i], sizeof mutable[i]);
        if (environment == NULL)
            execv(arguments[0], mutable);
        else
            execve(arguments[0], mutable, environment);
        _exit(125);
    }
    (void)close(errors);
    if (pid < 0)
        return MO_COMPILE_IO;
    *child = (struct mo_compile_child){pid, now + limits.wall};
    return MO_COMPILE_RUNNING;
}
static unsigned upgrade_inputs(const char **arguments, unsigned at, const char *old_bundle,
                               const char *migration, const char *context) {
    if (old_bundle != NULL) {
        arguments[at++] = "--old-bundle";
        arguments[at++] = old_bundle;
    }
    if (migration != NULL) {
        arguments[at++] = "--migrate";
        arguments[at++] = migration;
    }
    if (context != NULL) {
        arguments[at++] = "--context";
        arguments[at++] = context;
    }
    arguments[at] = NULL;
    return at;
}
enum mo_compile_status mo_compile_start(const char *executable, const char *entry,
                                        const char *output, const char *diagnostics,
                                        const char *old_bundle, const char *migration,
                                        const char *context, uint64_t now,
                                        struct mo_compile_child *child) {
    const char *arguments[16] = {executable, "emit", entry, "-o", output, "--bundle"};
    unsigned count = upgrade_inputs(arguments, 6, old_bundle, migration, context);
    struct child_limits limits = {MO_LIMIT_UPGRADE_COMPILE_MEMORY,
                                  MO_LIMIT_UPGRADE_COMPILE_CPU_SECONDS,
                                  MO_LIMIT_UPGRADE_COMPILE_WALL_US};
    return spawn(arguments, count, diagnostics, limits, NULL, now, child);
}
enum mo_compile_status mo_list_start(const char *executable, const char *bundle, const char *output,
                                     const char *diagnostics, const char *old_bundle,
                                     const char *context, uint64_t now,
                                     struct mo_compile_child *child) {
    const char *arguments[16] = {executable, "test", bundle, "--bundle", "--list", "-o", output};
    unsigned count = upgrade_inputs(arguments, 7, old_bundle, NULL, context);
    struct child_limits limits = {MO_LIMIT_CHECK_MEMORY, MO_LIMIT_CHECK_CPU_SECONDS,
                                  MO_LIMIT_CHECK_WALL_US};
    static char *const empty[] = {NULL};
    return spawn(arguments, count, diagnostics, limits, empty, now, child);
}
enum mo_compile_status mo_compile_poll(struct mo_compile_child *child, uint64_t now) {
    MO_ASSERT(child != NULL && child->pid > 0);
    int status;
    pid_t result = reap(child->pid, &status, WNOHANG);
    if (result < 0) {
        mo_compile_cancel(child);
        return MO_COMPILE_IO;
    }
    if (result == 0) {
        if (now < child->deadline)
            return MO_COMPILE_RUNNING;
        mo_compile_cancel(child);
        return MO_COMPILE_BUDGET;
    }
    child->pid = 0;
    if (WIFSIGNALED(status))
        return WTERMSIG(status) == SIGXCPU || WTERMSIG(status) == SIGKILL ? MO_COMPILE_BUDGET
                                                                          : MO_COMPILE_CRASH;
    if (!WIFEXITED(status) || WEXITSTATUS(status) == 125)
        return MO_COMPILE_IO;
    switch (WEXITSTATUS(status)) {
    case 0:
        return MO_COMPILE_OK;
    case 70:
        return MO_COMPILE_INITIALIZER;
    case 69:
        return MO_COMPILE_SERVICE;
    default:
        return MO_COMPILE_DIAGNOSTIC;
    }
}
