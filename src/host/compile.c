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
enum mo_compile_status mo_compile_start(const char *executable, const char *entry,
                                        const char *output, const char *diagnostics,
                                        const char *old_bundle, const char *migration,
                                        const char *context, uint64_t now,
                                        struct mo_compile_child *child) {
    MO_ASSERT(child != NULL && child->pid == 0);
    if (now > UINT64_MAX - MO_LIMIT_UPGRADE_COMPILE_WALL_US)
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
            (!MO_UPGRADE_COMPILE_ASAN && !limit(RLIMIT_AS, MO_LIMIT_UPGRADE_COMPILE_MEMORY)) ||
            !limit(RLIMIT_CPU, MO_LIMIT_UPGRADE_COMPILE_CPU_SECONDS) || !limit(RLIMIT_CORE, 0))
            _exit(125);
        for (int fd = 3; fd < descriptors; fd++)
            (void)close(fd);
        const char *arguments[16] = {executable, "emit", entry, "-o", output, "--bundle"};
        unsigned at = 6;
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
        /* execv does not mutate argument strings. POSIX predates const. */
        char *mutable[16];
        for (unsigned i = 0; i <= at; i++)
            memcpy(&mutable[i], &arguments[i], sizeof mutable[i]);
        execv(executable, mutable);
        _exit(125);
    }
    (void)close(errors);
    if (pid < 0)
        return MO_COMPILE_IO;
    *child = (struct mo_compile_child){pid, now + MO_LIMIT_UPGRADE_COMPILE_WALL_US};
    return MO_COMPILE_RUNNING;
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
