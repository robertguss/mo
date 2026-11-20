#ifndef MO_TEST_CONFINEMENT_H
#define MO_TEST_CONFINEMENT_H
#define _GNU_SOURCE
#include "host/host.h"
#include "test.h"
#include <stdlib.h>
#include <string.h>

#if defined(__linux__)
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/kcmp.h>
#include <linux/keyctl.h>
#include <linux/sockios.h>
#include <mqueue.h>
#include <netinet/in.h>
#include <sched.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/shm.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum { NEEDS = 9, ATTEMPTS = 30 };

static void cases(void);
static bool show;

static char marker[8] = "parent!";
static unsigned attempts;

static void need(const char *what, bool worked) {
    (void)fprintf(stderr, "need %s %s\n", what, worked ? "ok" : "failed");
}
static void attempt(const char *what, bool worked) {
    (void)fprintf(stderr, "try %s %s\n", what, worked ? "allowed" : "refused");
    attempts++;
}
static void join(char *out, size_t size, const char *folder, const char *name) {
    int n = snprintf(out, size, "%s/%s", folder, name);
    MO_ASSERT(n > 0 && (size_t)n < size);
}
static bool opens(const char *path, int flags) {
    int fd = open(path, flags | O_CLOEXEC, 0600);
    return fd >= 0 && close(fd) == 0;
}
static bool reads(const char *path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    char byte;
    bool worked = fd >= 0 && read(fd, &byte, 1) == 1;
    return fd >= 0 && close(fd) == 0 && worked;
}
static bool writes(const char *path) {
    int fd = open(path, O_WRONLY | O_APPEND | O_CLOEXEC);
    bool worked = fd >= 0 && write(fd, "x", 1) == 1;
    return fd >= 0 && close(fd) == 0 && worked;
}
static bool reaches(int family, int type, const struct sockaddr *address, socklen_t size) {
    int fd = socket(family, type | SOCK_CLOEXEC, 0);
    bool worked = fd >= 0 && (type == SOCK_DGRAM ? sendto(fd, "x", 1, 0, address, size) == 1
                                                 : connect(fd, address, size) == 0);
    return fd >= 0 && close(fd) == 0 && worked;
}
static bool forks(long pid) {
    if (pid == 0)
        _exit(0);
    int status;
    return pid > 0 && waitpid((pid_t)pid, &status, 0) == pid;
}
static bool rewrites_here(void) {
    int fd = open("probe-file", O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    char back[5] = {0};
    bool worked = fd >= 0 && write(fd, "mine", 4) == 4 && lseek(fd, 0, SEEK_SET) == 0 &&
                  read(fd, back, 4) == 4 && strcmp(back, "mine") == 0;
    return fd >= 0 && close(fd) == 0 && unlink("probe-file") == 0 && worked;
}
static bool makes_temporary(void) {
    char name[] = "probe-XXXXXX";
    int fd = mkstemp(name);
    return fd >= 0 && unlink(name) == 0 && close(fd) == 0;
}

static int probe(int argc, char **argv) {
    need("execute", true);
    MO_ASSERT(argc == 10 && strcmp(argv[8], "--lib") == 0);
    char plan[512] = {0}, root[256] = {0}, path[512];
    int fd = open(argv[2], O_RDONLY | O_CLOEXEC);
    ssize_t got = fd < 0 ? -1 : read(fd, plan, sizeof plan - 1);
    need("read the plan", fd >= 0 && close(fd) == 0 && got > 0);
    int tcp = 0, udp = 0, parent = 0;
    unsigned long long address = 0;
    MO_ASSERT(sscanf(plan, "%d %d %d %llx %255s", &tcp, &udp, &parent, &address, root) == 5);
    need("read the old bundle", reads(argv[5]));
    need("read the context", reads(argv[7]));
    need("read the executable", reads("/proc/self/exe"));
    join(path, sizeof path, root, "lib/list.mo");
    need("read the library", reads(path));
    need("create and reread a file in its folder", rewrites_here());
    need("make a temporary file in its folder", makes_temporary());
    need("open /dev/null", opens("/dev/null", O_RDWR));

    attempt("write the input", opens(argv[2], O_WRONLY | O_APPEND));
    join(path, sizeof path, root, "new");
    attempt("create a file beside the inputs", opens(path, O_WRONLY | O_CREAT | O_EXCL));
    struct stat info;
    attempt("truncate the input",
            stat(argv[2], &info) == 0 && truncate(argv[2], info.st_size) == 0);
    join(path, sizeof path, root, "compile/image.bc");
    attempt("write the compile child's output", writes(path));
    join(path, sizeof path, root, "list/items.moil");
    attempt("write the listing child's output", writes(path));
    join(path, sizeof path, root, "outside/secret");
    attempt("read a file outside", reads(path));
    join(path, sizeof path, root, "outside/new");
    attempt("create a file outside", opens(path, O_WRONLY | O_CREAT | O_EXCL));
    join(path, sizeof path, root, "data/service.sqlite");
    attempt("read the service's data", reads(path));
    attempt("read /etc/passwd", reads("/etc/passwd"));

    struct sockaddr_in loopback = {.sin_family = AF_INET,
                                   .sin_port = htons((uint16_t)tcp),
                                   .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    attempt("connect over TCP",
            reaches(AF_INET, SOCK_STREAM, (struct sockaddr *)&loopback, sizeof loopback));
    loopback.sin_port = htons((uint16_t)udp);
    attempt("send a UDP datagram",
            reaches(AF_INET, SOCK_DGRAM, (struct sockaddr *)&loopback, sizeof loopback));
    struct sockaddr_un local = {.sun_family = AF_UNIX};
    join(local.sun_path, sizeof local.sun_path, root, "socket");
    attempt("connect to a Unix socket by path",
            reaches(AF_UNIX, SOCK_STREAM, (struct sockaddr *)&local, sizeof local));
    memset(local.sun_path, 0, sizeof local.sun_path);
    int named = snprintf(local.sun_path + 1, sizeof local.sun_path - 1, "mo-confine-%d", parent);
    MO_ASSERT(named > 0);
    attempt("connect to an abstract Unix socket",
            reaches(AF_UNIX, SOCK_STREAM, (struct sockaddr *)&local,
                    (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + (size_t)named)));

    attempt("signal its parent", kill(parent, 0) == 0);
    siginfo_t queued = {.si_code = SI_QUEUE};
    attempt("queue a signal to its parent", syscall(SYS_rt_sigqueueinfo, parent, 0, &queued) == 0);
    attempt("signal its parent's thread", syscall(SYS_tgkill, parent, parent, 0) == 0);
    char word[8];
    struct iovec to = {word, sizeof word}, from = {(void *)(uintptr_t)address, sizeof word};
    attempt("read its parent's memory", process_vm_readv(parent, &to, 1, &from, 1, 0) > 0);
    char memory[64];
    int written = snprintf(memory, sizeof memory, "/proc/%d/mem", parent);
    MO_ASSERT(written > 0 && (size_t)written < sizeof memory);
    attempt("open its parent's memory file", opens(memory, O_RDONLY));
    long pidfd = syscall(SYS_pidfd_open, parent, 0);
    attempt("open a pidfd for its parent", pidfd >= 0 && close((int)pidfd) == 0);
    int ends[2];
    MO_ASSERT(pipe(ends) == 0);
    attempt("make its parent a pipe's owner", fcntl(ends[0], F_SETOWN, parent) == 0);
    MO_ASSERT(close(ends[0]) == 0 && close(ends[1]) == 0);
    bool paired = socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, ends) == 0;
    attempt("make its parent a socket's owner", paired && ioctl(ends[0], FIOSETOWN, &parent) == 0);
    MO_ASSERT(!paired || (close(ends[0]) == 0 && close(ends[1]) == 0));
    struct rlimit limits;
    attempt("read its parent's limits", prlimit(parent, RLIMIT_NOFILE, NULL, &limits) == 0);
    errno = 0;
    int nice = getpriority(PRIO_PROCESS, (id_t)parent);
    attempt("set its parent's priority",
            errno == 0 && setpriority(PRIO_PROCESS, (id_t)parent, nice) == 0);
    attempt("compare itself with its parent",
            syscall(SYS_kcmp, getpid(), parent, KCMP_VM, 0, 0) >= 0);
    attempt("find the user's keyring",
            syscall(SYS_keyctl, KEYCTL_GET_KEYRING_ID, KEY_SPEC_USER_KEYRING, 0) >= 0);
    int shared = shmget(IPC_PRIVATE, 4096, IPC_CREAT | 0600);
    attempt("make shared memory", shared >= 0 && shmctl(shared, IPC_RMID, NULL) == 0);
    char queue[64];
    written = snprintf(queue, sizeof queue, "/mo-confine-%d", parent);
    MO_ASSERT(written > 0 && (size_t)written < sizeof queue);
    mqd_t opened = mq_open(queue, O_RDWR | O_CREAT | O_EXCL, 0600, NULL);
    attempt("open a message queue",
            opened != (mqd_t)-1 && mq_close(opened) == 0 && mq_unlink(queue) == 0);
    uint64_t clone_args[8] = {0, 0, 0, 0, SIGCHLD, 0, 0, 0};
    attempt("start a process by clone3", forks(syscall(SYS_clone3, clone_args, sizeof clone_args)));
    attempt("start a process in a new user namespace",
            forks(syscall(SYS_clone, CLONE_NEWUSER | SIGCHLD, 0, 0, 0, 0)));
    attempt("enter a new user namespace", unshare(CLONE_NEWUSER) == 0);
    (void)fprintf(stderr, "attempts %u\n", attempts);
    return 0;
}

struct tree {
    char root[sizeof "/tmp/mo-confine-XXXXXX"];
    int tcp, udp, by_path, abstract;
};
static void put(const char *root, const char *name, const char *text) {
    char path[512];
    join(path, sizeof path, root, name);
    FILE *file = fopen(path, "w");
    MO_ASSERT(file != NULL && fputs(text, file) >= 0 && fclose(file) == 0);
}
static bool same(const char *root, const char *name, const char *text) {
    char path[512], content[64] = {0};
    join(path, sizeof path, root, name);
    FILE *file = fopen(path, "r");
    MO_ASSERT(file != NULL);
    size_t n = fread(content, 1, sizeof content - 1, file);
    MO_ASSERT(fclose(file) == 0);
    return n == strlen(text) && strcmp(content, text) == 0;
}
static void make(const char *root, const char *name) {
    char path[512];
    join(path, sizeof path, root, name);
    MO_ASSERT(mkdir(path, 0700) == 0);
}
static int listen_on(int family, int type, const struct sockaddr *address, socklen_t size) {
    int fd = socket(family, type | SOCK_CLOEXEC, 0);
    MO_ASSERT(fd >= 0 && bind(fd, address, size) == 0 &&
              (type == SOCK_DGRAM || listen(fd, 8) == 0));
    return fd;
}
static int port(int fd) {
    struct sockaddr_in bound;
    socklen_t size = sizeof bound;
    MO_ASSERT(getsockname(fd, (struct sockaddr *)&bound, &size) == 0);
    return ntohs(bound.sin_port);
}
static struct tree grow(void) {
    struct tree t;
    memcpy(t.root, "/tmp/mo-confine-XXXXXX", sizeof t.root);
    MO_ASSERT(mkdtemp(t.root) != NULL);
    const char *folders[] = {"lib", "compile", "list", "check", "outside", "data"};
    for (size_t i = 0; i < sizeof folders / sizeof *folders; i++)
        make(t.root, folders[i]);
    put(t.root, "lib/list.mo", "module List\n");
    put(t.root, "compile/image.bc", "image\n");
    put(t.root, "list/items.moil", "items\n");
    put(t.root, "outside/secret", "secret\n");
    put(t.root, "data/service.sqlite", "state\n");
    put(t.root, "old.mosb", "old\n");
    put(t.root, "input.mocc", "context\n");
    struct sockaddr_in loopback = {.sin_family = AF_INET,
                                   .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    t.tcp = listen_on(AF_INET, SOCK_STREAM, (struct sockaddr *)&loopback, sizeof loopback);
    t.udp = listen_on(AF_INET, SOCK_DGRAM, (struct sockaddr *)&loopback, sizeof loopback);
    struct sockaddr_un local = {.sun_family = AF_UNIX};
    join(local.sun_path, sizeof local.sun_path, t.root, "socket");
    t.by_path = listen_on(AF_UNIX, SOCK_STREAM, (struct sockaddr *)&local, sizeof local);
    memset(local.sun_path, 0, sizeof local.sun_path);
    int named =
        snprintf(local.sun_path + 1, sizeof local.sun_path - 1, "mo-confine-%d", (int)getpid());
    MO_ASSERT(named > 0);
    t.abstract = listen_on(AF_UNIX, SOCK_STREAM, (struct sockaddr *)&local,
                           (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + (size_t)named));
    char plan[512];
    int n = snprintf(plan, sizeof plan, "%d %d %d %llx %s\n", port(t.tcp), port(t.udp),
                     (int)getpid(), (unsigned long long)(uintptr_t)marker, t.root);
    MO_ASSERT(n > 0 && (size_t)n < sizeof plan);
    put(t.root, "source.mosb", plan);
    return t;
}
static void fell(struct tree *t) {
    MO_ASSERT(close(t->tcp) == 0 && close(t->udp) == 0 && close(t->by_path) == 0 &&
              close(t->abstract) == 0);
    const char *files[] = {"lib/list.mo",     "compile/image.bc",
                           "list/items.moil", "outside/secret",
                           "outside/new",     "data/service.sqlite",
                           "old.mosb",        "input.mocc",
                           "source.mosb",     "new",
                           "socket",          "check-diagnostics"};
    char path[512];
    for (size_t i = 0; i < sizeof files / sizeof *files; i++) {
        join(path, sizeof path, t->root, files[i]);
        (void)unlink(path);
    }
    const char *folders[] = {"lib", "compile", "list", "check", "outside", "data", ""};
    for (size_t i = 0; i < sizeof folders / sizeof *folders; i++) {
        join(path, sizeof path, t->root, folders[i]);
        CHECK(rmdir(path) == 0);
    }
}

struct outcome {
    enum mo_compile_status status;
    unsigned needs, needed, tries, refused, allowed, reported;
    bool outputs_kept;
};
static struct outcome run_probe(void) {
    struct tree t = grow();
    char lib[512], folder[512], plan[512], old[512], context[512], diagnostics[512];
    join(lib, sizeof lib, t.root, "lib");
    join(folder, sizeof folder, t.root, "check");
    join(plan, sizeof plan, t.root, "source.mosb");
    join(old, sizeof old, t.root, "old.mosb");
    join(context, sizeof context, t.root, "input.mocc");
    join(diagnostics, sizeof diagnostics, t.root, "check-diagnostics");
    const struct mo_compiler compiler = {"/proc/self/exe", lib};
    struct mo_compile_child child = {0};
    struct outcome o = {
        .status = mo_check_start(&compiler, folder, plan, diagnostics, old, context, 1, &child)};
    for (unsigned i = 0; o.status == MO_COMPILE_RUNNING && i < 3000; i++) {
        struct timespec delay = {0, 10000000};
        (void)nanosleep(&delay, NULL);
        o.status = mo_compile_poll(&child, 2);
    }
    if (o.status == MO_COMPILE_RUNNING)
        mo_compile_cancel(&child);
    FILE *lines = fopen(diagnostics, "r");
    MO_ASSERT(lines != NULL);
    char line[256];
    while (fgets(line, sizeof line, lines) != NULL) {
        if (show)
            (void)fputs(line, stdout);
        size_t n = strcspn(line, "\n");
        line[n] = 0;
        const char *last = strrchr(line, ' ');
        if (strncmp(line, "need ", 5) == 0) {
            o.needs++;
            o.needed += last != NULL && strcmp(last, " ok") == 0;
        } else if (strncmp(line, "try ", 4) == 0) {
            o.tries++;
            o.refused += last != NULL && strcmp(last, " refused") == 0;
            o.allowed += last != NULL && strcmp(last, " allowed") == 0;
        } else if (strncmp(line, "attempts ", 9) == 0)
            o.reported = (unsigned)strtoul(line + 9, NULL, 10);
    }
    MO_ASSERT(fclose(lines) == 0);
    o.outputs_kept =
        same(t.root, "compile/image.bc", "image\n") && same(t.root, "list/items.moil", "items\n");
    fell(&t);
    return o;
}

int main(int argc, char **argv) {
    if (argc >= 8 && strcmp(argv[1], "test") == 0)
        return probe(argc, argv);
    show = argc == 2 && strcmp(argv[1], "--show") == 0;
    (void)prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
    cases();
    return mo_test_report(CONFINED ? "confinement" : "confinement control");
}
#else
int main(void) {
    CHECK(!mo_compile_can_confine());
    return mo_test_report(CONFINED ? "confinement" : "confinement control");
}
#endif
#endif
