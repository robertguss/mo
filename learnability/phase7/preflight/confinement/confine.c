#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <netinet/in.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__x86_64__)
#define NATIVE_ARCH AUDIT_ARCH_X86_64
#elif defined(__aarch64__)
#define NATIVE_ARCH AUDIT_ARCH_AARCH64
#else
#error "add this machine's audit architecture"
#endif

static char base[256], self[4096];

static int unavailable(const char *step) {
    printf("unavailable: %s: %s\n", step, strerror(errno));
    return -1;
}

static void put(const char *path, const char *text) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0 || write(fd, text, strlen(text)) < 0 || close(fd) != 0) {
        perror(path);
        exit(3);
    }
}

enum {
    FS_EXECUTE = 1 << 0,
    FS_WRITE_FILE = 1 << 1,
    FS_READ_FILE = 1 << 2,
    FS_READ_DIR = 1 << 3,
    FS_REMOVE_FILE = 1 << 5,
    FS_MAKE_REG = 1 << 8,
    FS_TRUNCATE = 1 << 14,
    FS_FILE_RIGHTS = FS_EXECUTE | FS_WRITE_FILE | FS_READ_FILE | FS_TRUNCATE,
};
struct ruleset_attr {
    uint64_t handled_fs, handled_net, scoped;
};
struct path_beneath_attr {
    uint64_t allowed;
    int32_t parent;
} __attribute__((packed));

static int allow(int ruleset, const char *path, uint64_t rights, uint64_t handled) {
    int fd = open(path, O_PATH | O_CLOEXEC);
    if (fd < 0)
        return errno == ENOENT ? 0 : unavailable(path);
    struct stat st;
    if (fstat(fd, &st) != 0)
        return unavailable(path);
    if (!S_ISDIR(st.st_mode))
        rights &= FS_FILE_RIGHTS;
    struct path_beneath_attr rule = {rights & handled, fd};
    if (syscall(SYS_landlock_add_rule, ruleset, 1, &rule, 0) != 0)
        return unavailable("landlock_add_rule");
    return close(fd);
}

static int landlock(void) {
    long abi = syscall(SYS_landlock_create_ruleset, NULL, 0, 1);
    if (abi < 0)
        return unavailable("landlock_create_ruleset");
    printf("landlock abi: %ld\n", abi);
    uint64_t fs = (1u << 13) - 1;
    if (abi >= 2)
        fs |= 1u << 13;
    if (abi >= 3)
        fs |= FS_TRUNCATE;
    if (abi >= 5)
        fs |= 1u << 15;
    struct ruleset_attr attr = {fs, abi >= 4 ? 3 : 0, abi >= 6 ? 3 : 0};
    size_t size = abi >= 6 ? 24 : abi >= 4 ? 16 : 8;
    int ruleset = (int)syscall(SYS_landlock_create_ruleset, &attr, size, 0);
    if (ruleset < 0)
        return unavailable("landlock_create_ruleset");
    uint64_t read_only = FS_EXECUTE | FS_READ_FILE | FS_READ_DIR;
    uint64_t read_write =
        FS_READ_FILE | FS_READ_DIR | FS_WRITE_FILE | FS_REMOVE_FILE | FS_MAKE_REG | FS_TRUNCATE;
    char path[512];
    const char *shared[] = {"/usr", "/lib", "/lib64", "/etc/ld.so.cache", self};
    for (size_t i = 0; i < sizeof shared / sizeof *shared; i++)
        if (allow(ruleset, shared[i], read_only, fs) != 0)
            return -1;
    const char *mine[] = {"lib", "earlier"};
    for (size_t i = 0; i < 2; i++) {
        snprintf(path, sizeof path, "%s/%s", base, mine[i]);
        if (allow(ruleset, path, FS_READ_FILE | FS_READ_DIR, fs) != 0)
            return -1;
    }
    snprintf(path, sizeof path, "%s/scratch", base);
    if (allow(ruleset, path, read_write, fs) != 0 ||
        allow(ruleset, "/dev/null", FS_READ_FILE | FS_WRITE_FILE, fs) != 0)
        return -1;
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
        return unavailable("no_new_privs");
    if (syscall(SYS_landlock_restrict_self, ruleset, 0) != 0)
        return unavailable("landlock_restrict_self");
    return 0;
}

#define DENY(number)                                                                             \
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (number), 0, 1),                                         \
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM)

static int seccomp(void) {
    struct sock_filter filter[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, NATIVE_ARCH, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
        BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, 0x40000000, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
        DENY(SYS_socket),
        DENY(SYS_socketpair),
        DENY(SYS_connect),
        DENY(SYS_bind),
        DENY(SYS_sendto),
        DENY(SYS_sendmsg),
        DENY(SYS_sendmmsg),
        DENY(SYS_kill),
        DENY(SYS_tkill),
        DENY(SYS_ptrace),
        DENY(SYS_process_vm_readv),
        DENY(SYS_process_vm_writev),
        DENY(SYS_pidfd_open),
        DENY(SYS_pidfd_send_signal),
        DENY(SYS_io_uring_setup),
        DENY(SYS_unshare),
        DENY(SYS_setns),
        DENY(SYS_mount),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_tgkill, 0, 3),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[0])),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (uint32_t)getpid(), 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    struct sock_fprog program = {sizeof filter / sizeof *filter, filter};
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
        return unavailable("no_new_privs");
    if (syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &program) != 0)
        return unavailable("seccomp");
    return 0;
}

static int bind_at(const char *root, const char *path, bool read_only) {
    char target[1024];
    struct stat st;
    if (lstat(path, &st) != 0)
        return 0;
    snprintf(target, sizeof target, "%s%s", root, path);
    for (char *slash = target + strlen(root) + 1; (slash = strchr(slash, '/')) != NULL; slash++) {
        *slash = '\0';
        (void)mkdir(target, 0755);
        *slash = '/';
    }
    if (S_ISLNK(st.st_mode)) {
        char link[512] = "";
        if (readlink(path, link, sizeof link - 1) < 0 || symlink(link, target) != 0)
            return unavailable("symlink");
        return 0;
    }
    if (S_ISDIR(st.st_mode) ? mkdir(target, 0755) != 0 && errno != EEXIST
                            : close(open(target, O_WRONLY | O_CREAT, 0600)) != 0)
        return unavailable(target);
    if (mount(path, target, NULL, MS_BIND | MS_REC, NULL) != 0)
        return unavailable("bind mount");
    if (read_only &&
        mount(NULL, target, NULL, MS_BIND | MS_REMOUNT | MS_RDONLY | MS_REC, NULL) != 0)
        return unavailable("read-only remount");
    return 0;
}

static int namespaces(void) {
    char map[64], root[512], path[512];
    unsigned uid = getuid(), gid = getgid();
    if (unshare(CLONE_NEWUSER | CLONE_NEWNS | CLONE_NEWNET | CLONE_NEWIPC | CLONE_NEWPID) != 0)
        return unavailable("unshare");
    int fd = open("/proc/self/setgroups", O_WRONLY);
    if (fd < 0 || write(fd, "deny", 4) != 4 || close(fd) != 0)
        return unavailable("setgroups");
    snprintf(map, sizeof map, "%u %u 1", uid, uid);
    fd = open("/proc/self/uid_map", O_WRONLY);
    if (fd < 0 || write(fd, map, strlen(map)) < 0 || close(fd) != 0)
        return unavailable("uid_map");
    snprintf(map, sizeof map, "%u %u 1", gid, gid);
    fd = open("/proc/self/gid_map", O_WRONLY);
    if (fd < 0 || write(fd, map, strlen(map)) < 0 || close(fd) != 0)
        return unavailable("gid_map");
    fflush(stdout);
    pid_t first = fork();
    if (first < 0)
        return unavailable("fork");
    if (first != 0) {
        int status;
        (void)waitpid(first, &status, 0);
        _exit(WIFEXITED(status) ? WEXITSTATUS(status) : 1);
    }
    snprintf(root, sizeof root, "%s/root", base);
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0)
        return unavailable("private mounts");
    if (mkdir(root, 0755) != 0 || mount("tmpfs", root, "tmpfs", 0, "mode=755") != 0)
        return unavailable("tmpfs root");
    const char *shared[] = {"/usr", "/lib", "/lib64", "/etc/ld.so.cache", self};
    for (size_t i = 0; i < sizeof shared / sizeof *shared; i++)
        if (bind_at(root, shared[i], true) != 0)
            return -1;
    const char *mine[] = {"lib", "earlier", "scratch"};
    for (size_t i = 0; i < 3; i++) {
        snprintf(path, sizeof path, "%s/%s", base, mine[i]);
        if (bind_at(root, path, i < 2) != 0)
            return -1;
    }
    if (bind_at(root, "/dev/null", false) != 0)
        return -1;
    snprintf(path, sizeof path, "%s/proc", root);
    if (mkdir(path, 0555) != 0 || mount("proc", path, "proc", 0, NULL) != 0)
        return unavailable("proc");
    snprintf(path, sizeof path, "%s/old", root);
    if (mkdir(path, 0700) != 0 || syscall(SYS_pivot_root, root, path) != 0 || chdir("/") != 0 ||
        umount2("/old", MNT_DETACH) != 0 || rmdir("/old") != 0)
        return unavailable("pivot_root");
    return 0;
}

static int failures;

static void need(const char *what, bool worked) {
    printf("need  %-34s %s\n", what, worked ? "ok" : strerror(errno));
    failures += worked ? 0 : 1;
}

static void attempt(const char *what, bool worked) {
    if (worked)
        printf("try   %-34s ALLOWED\n", what);
    else
        printf("try   %-34s refused (%s)\n", what, strerror(errno));
    failures += worked ? 1 : 0;
}

static bool can_open(const char *folder, const char *name, int flags) {
    char path[512];
    snprintf(path, sizeof path, "%s%s%s", folder, *name ? "/" : "", name);
    int fd = open(path, flags, 0600);
    if (fd < 0)
        return false;
    char byte = 'x';
    bool worked = (flags & O_ACCMODE) == O_RDONLY ? read(fd, &byte, 1) >= 0
                                                 : write(fd, &byte, 1) == 1;
    int number = errno;
    (void)close(fd);
    errno = number;
    return worked;
}

static bool can_connect(int family, int type, const struct sockaddr *address, socklen_t size) {
    int fd = socket(family, type, 0);
    if (fd < 0)
        return false;
    bool worked = type == SOCK_DGRAM ? sendto(fd, "x", 1, 0, address, size) == 1
                                     : connect(fd, address, size) == 0;
    int number = errno;
    (void)close(fd);
    errno = number;
    return worked;
}

static int probe(char **argv) {
    char path[512], name[64];
    snprintf(base, sizeof base, "%s", argv[2]);
    pid_t parent = (pid_t)atoi(argv[5]);

    need("execute and load the C library", true);
    need("read the executable itself", can_open("/proc/self/exe", "", O_RDONLY));
    snprintf(path, sizeof path, "%s/lib", base);
    need("read the library folder", can_open(path, "list.mo", O_RDONLY));
    snprintf(path, sizeof path, "%s/earlier", base);
    need("read an earlier child's output", can_open(path, "image.bc", O_RDONLY));
    attempt("change an earlier child's output", can_open(path, "image.bc", O_WRONLY));
    snprintf(path, sizeof path, "%s/scratch", base);
    need("create a file in its scratch folder", can_open(path, "out", O_WRONLY | O_CREAT));
    need("read it back", can_open(path, "out", O_RDONLY));
    need("open /dev/null", can_open("/dev/null", "", O_RDWR));

    snprintf(path, sizeof path, "%s/outside", base);
    attempt("open a file outside", can_open(path, "secret", O_RDONLY));
    attempt("create a file outside", can_open(path, "new", O_WRONLY | O_CREAT));
    snprintf(path, sizeof path, "%s/data", base);
    attempt("read the service's data folder", can_open(path, "store.sqlite", O_RDONLY));
    attempt("read /etc/passwd", can_open("/etc/passwd", "", O_RDONLY));

    struct sockaddr_in loopback = {.sin_family = AF_INET,
                                   .sin_port = htons((uint16_t)atoi(argv[3])),
                                   .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    attempt("connect over TCP to loopback",
            can_connect(AF_INET, SOCK_STREAM, (struct sockaddr *)&loopback, sizeof loopback));
    loopback.sin_port = htons((uint16_t)atoi(argv[4]));
    attempt("send a UDP datagram",
            can_connect(AF_INET, SOCK_DGRAM, (struct sockaddr *)&loopback, sizeof loopback));
    struct sockaddr_un local = {.sun_family = AF_UNIX};
    snprintf(local.sun_path, sizeof local.sun_path, "%s/socket", base);
    attempt("connect to a Unix socket by path",
            can_connect(AF_UNIX, SOCK_STREAM, (struct sockaddr *)&local, sizeof local));
    memset(local.sun_path, 0, sizeof local.sun_path);
    snprintf(name, sizeof name, "mo-confine-%d", (int)parent);
    memcpy(local.sun_path + 1, name, strlen(name));
    attempt("connect to an abstract Unix socket",
            can_connect(AF_UNIX, SOCK_STREAM, (struct sockaddr *)&local,
                        (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + strlen(name))));

    attempt("signal its parent", kill(parent, 0) == 0);
    char word[8];
    struct iovec to = {word, sizeof word}, from = {(void *)(uintptr_t)strtoull(argv[6], NULL, 16),
                                                   sizeof word};
    attempt("read its parent's memory", process_vm_readv(parent, &to, 1, &from, 1, 0) > 0);
    snprintf(path, sizeof path, "/proc/%d/mem", (int)parent);
    int memory = open(path, O_RDONLY);
    attempt("open its parent's memory file", memory >= 0);
    printf("result: %s\n", failures == 0 ? "confined" : "NOT confined");
    return failures == 0 ? 0 : 1;
}

int main(int argc, char **argv) {
    static char marker[8] = "parent!";
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc == 7 && strcmp(argv[1], "--probe") == 0)
        return probe(argv);
    if (argc != 2) {
        fprintf(stderr, "usage: confine none|landlock|seccomp|landlock+seccomp|namespaces\n");
        return 3;
    }
    ssize_t length = readlink("/proc/self/exe", self, sizeof self - 1);
    snprintf(base, sizeof base, "/tmp/mo-confine-XXXXXX");
    if (length < 0 || mkdtemp(base) == NULL)
        return 3;
    char path[512];
    const char *folders[] = {"scratch", "earlier", "lib", "outside", "data"};
    const char *files[] = {NULL, "image.bc", "list.mo", "secret", "store.sqlite"};
    for (size_t i = 0; i < 5; i++) {
        snprintf(path, sizeof path, "%s/%s", base, folders[i]);
        if (mkdir(path, 0700) != 0)
            return 3;
        if (files[i] != NULL) {
            snprintf(path, sizeof path, "%s/%s/%s", base, folders[i], files[i]);
            put(path, "contents\n");
        }
    }
    struct sockaddr_in any = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t size = sizeof any;
    int tcp = socket(AF_INET, SOCK_STREAM, 0), udp = socket(AF_INET, SOCK_DGRAM, 0);
    if (bind(tcp, (struct sockaddr *)&any, sizeof any) != 0 || listen(tcp, 8) != 0 ||
        getsockname(tcp, (struct sockaddr *)&any, &size) != 0)
        return 3;
    int tcp_port = ntohs(any.sin_port);
    any.sin_port = 0;
    if (bind(udp, (struct sockaddr *)&any, sizeof any) != 0 ||
        getsockname(udp, (struct sockaddr *)&any, &size) != 0)
        return 3;
    int udp_port = ntohs(any.sin_port);
    struct sockaddr_un local = {.sun_family = AF_UNIX};
    snprintf(local.sun_path, sizeof local.sun_path, "%s/socket", base);
    int by_path = socket(AF_UNIX, SOCK_STREAM, 0), abstract = socket(AF_UNIX, SOCK_STREAM, 0);
    if (bind(by_path, (struct sockaddr *)&local, sizeof local) != 0 || listen(by_path, 8) != 0)
        return 3;
    char name[64];
    snprintf(name, sizeof name, "mo-confine-%d", (int)getpid());
    memset(local.sun_path, 0, sizeof local.sun_path);
    memcpy(local.sun_path + 1, name, strlen(name));
    if (bind(abstract, (struct sockaddr *)&local,
             (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + strlen(name))) != 0 ||
        listen(abstract, 8) != 0)
        return 3;

    printf("mechanism: %s\n", argv[1]);
    char tcp_text[16], udp_text[16], pid_text[16], address[32];
    snprintf(tcp_text, sizeof tcp_text, "%d", tcp_port);
    snprintf(udp_text, sizeof udp_text, "%d", udp_port);
    snprintf(pid_text, sizeof pid_text, "%d", (int)getpid());
    snprintf(address, sizeof address, "%llx", (unsigned long long)(uintptr_t)marker);
    fflush(stdout);
    pid_t child = fork();
    if (child == 0) {
        for (int fd = 3; fd < 64; fd++)
            (void)close(fd);
        int applied = 0;
        if (strcmp(argv[1], "namespaces") == 0)
            applied = namespaces();
        if (applied == 0 && strstr(argv[1], "landlock") != NULL)
            applied = landlock();
        if (applied == 0 && strstr(argv[1], "seccomp") != NULL)
            applied = seccomp();
        if (applied != 0)
            _exit(2);
        char *arguments[] = {"confine", "--probe", base, tcp_text, udp_text, pid_text, address,
                             NULL};
        char *environment[] = {NULL};
        execve("/proc/self/exe", arguments, environment);
        printf("unavailable: execve: %s\n", strerror(errno));
        _exit(2);
    }
    int status;
    if (child < 0 || waitpid(child, &status, 0) != child)
        return 3;
    snprintf(path, sizeof path, "rm -rf %s", base);
    if (system(path) != 0)
        return 3;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
