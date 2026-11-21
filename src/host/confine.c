#define _GNU_SOURCE
#include "host/internal.h"

#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__)) &&                         \
    __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#include <errno.h>
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <linux/sockios.h>
#include <sched.h>
#include <stddef.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#if defined(__x86_64__)
#define NATIVE_ARCH AUDIT_ARCH_X86_64
#define X32_CHECK 2
#else
#define NATIVE_ARCH AUDIT_ARCH_AARCH64
#define X32_CHECK 0
#endif
#ifndef SYS_pidfd_send_signal
#define SYS_pidfd_send_signal 424
#endif
#ifndef SYS_io_uring_setup
#define SYS_io_uring_setup 425
#define SYS_io_uring_enter 426
#define SYS_io_uring_register 427
#endif
#ifndef SYS_open_tree
#define SYS_open_tree 428
#define SYS_move_mount 429
#define SYS_fsopen 430
#define SYS_fsconfig 431
#define SYS_fsmount 432
#define SYS_fspick 433
#endif
#ifndef SYS_pidfd_open
#define SYS_pidfd_open 434
#endif
#ifndef SYS_clone3
#define SYS_clone3 435
#endif
#ifndef SYS_pidfd_getfd
#define SYS_pidfd_getfd 438
#endif
#ifndef SYS_process_madvise
#define SYS_process_madvise 440
#endif
#ifndef SYS_mount_setattr
#define SYS_mount_setattr 442
#endif
#ifndef SYS_landlock_create_ruleset
#define SYS_landlock_create_ruleset 444
#define SYS_landlock_add_rule 445
#define SYS_landlock_restrict_self 446
#endif
#ifndef F_SETOWN_EX
#define F_SETOWN_EX 15
#endif

enum {
    FS_EXECUTE = 1 << 0,
    FS_WRITE_FILE = 1 << 1,
    FS_READ_FILE = 1 << 2,
    FS_READ_DIR = 1 << 3,
    FS_REMOVE_DIR = 1 << 4,
    FS_REMOVE_FILE = 1 << 5,
    FS_MAKE_DIR = 1 << 7,
    FS_MAKE_REG = 1 << 8,
    FS_MAKE_SYM = 1 << 11,
    FS_REFER = 1 << 13,
    FS_TRUNCATE = 1 << 14,
    FS_IOCTL_DEV = 1 << 15,
    FS_FILE = FS_EXECUTE | FS_WRITE_FILE | FS_READ_FILE | FS_TRUNCATE | FS_IOCTL_DEV,
    SYSTEM = FS_EXECUTE | FS_READ_FILE | FS_READ_DIR,
    OWN = FS_READ_FILE | FS_READ_DIR | FS_WRITE_FILE | FS_MAKE_REG | FS_REMOVE_FILE | FS_TRUNCATE,
    COMMAND_OWN = OWN | FS_REMOVE_DIR | FS_MAKE_DIR | FS_MAKE_SYM | FS_REFER,
    NET_TCP = (1 << 0) | (1 << 1),
    SCOPES = (1 << 0) | (1 << 1),
    RULE_PATH_BENEATH = 1,
    RULESET_VERSION = 1 << 0,
    NEWEST_ABI = 6,
};
struct ruleset_attr {
    uint64_t handled_fs, handled_net, scoped;
};
struct path_beneath_attr {
    uint64_t allowed;
    int32_t parent;
} __attribute__((packed));

static const unsigned refused[] = {
    SYS_socket,
    SYS_socketpair,
    SYS_connect,
    SYS_bind,
    SYS_sendto,
    SYS_sendmsg,
    SYS_sendmmsg,
    SYS_tkill,
    SYS_rt_sigqueueinfo,
    SYS_rt_tgsigqueueinfo,
    SYS_pidfd_open,
    SYS_pidfd_send_signal,
    SYS_pidfd_getfd,
    SYS_process_vm_readv,
    SYS_process_vm_writev,
    SYS_process_madvise,
    SYS_kcmp,
    SYS_perf_event_open,
    SYS_io_uring_setup,
    SYS_io_uring_enter,
    SYS_io_uring_register,
    SYS_mount,
    SYS_umount2,
    SYS_pivot_root,
    SYS_open_tree,
    SYS_move_mount,
    SYS_fsopen,
    SYS_fsconfig,
    SYS_fsmount,
    SYS_fspick,
    SYS_mount_setattr,
    SYS_unshare,
    SYS_setns,
    SYS_setpriority,
    SYS_ioprio_set,
    SYS_sched_setaffinity,
    SYS_sched_setscheduler,
    SYS_sched_setparam,
    SYS_sched_setattr,
    SYS_add_key,
    SYS_request_key,
    SYS_keyctl,
    SYS_shmget,
    SYS_shmat,
    SYS_shmctl,
    SYS_msgget,
    SYS_msgsnd,
    SYS_msgrcv,
    SYS_msgctl,
    SYS_semget,
    SYS_semop,
    SYS_semtimedop,
    SYS_semctl,
    SYS_mq_open,
    SYS_mq_unlink,
    SYS_mq_timedsend,
    SYS_mq_timedreceive,
    SYS_mq_notify,
    SYS_mq_getsetattr,
#if !MO_UPGRADE_COMPILE_ASAN
    SYS_ptrace,
#endif
};
enum { REFUSED = sizeof refused / sizeof *refused };

static bool fail(const char *step) {
    static const char prefix[] = "mo: cannot confine the child: ";
    (void)!write(STDERR_FILENO, prefix, sizeof prefix - 1);
    (void)!write(STDERR_FILENO, step, strlen(step));
    (void)!write(STDERR_FILENO, "\n", 1);
    return false;
}
static long landlock_abi(void) {
    return syscall(SYS_landlock_create_ruleset, NULL, 0, RULESET_VERSION);
}
bool mo_compile_can_confine(void) {
    uint32_t action = SECCOMP_RET_KILL_PROCESS;
    return landlock_abi() >= 3 && syscall(SYS_seccomp, SECCOMP_GET_ACTION_AVAIL, 0, &action) == 0;
}

static bool allow(int ruleset, const char *path, uint64_t rights) {
    int fd = open(path, O_PATH | O_CLOEXEC);
    if (fd < 0)
        return errno == ENOENT || fail(path);
    struct stat info;
    bool ok = fstat(fd, &info) == 0;
    if (ok) {
        struct path_beneath_attr rule = {S_ISDIR(info.st_mode) ? rights : rights & FS_FILE, fd};
        ok = syscall(SYS_landlock_add_rule, ruleset, RULE_PATH_BENEATH, &rule, 0) == 0;
    }
    ok = close(fd) == 0 && ok;
    return ok || fail(path);
}
static bool landlock(const struct mo_compiler *compiler, const char *folder,
                     const char *const reads[3]) {
    long abi = landlock_abi();
    if (abi < 3)
        return fail("landlock_create_ruleset");
    if (abi > NEWEST_ABI)
        abi = NEWEST_ABI;
    struct ruleset_attr attr = {(UINT64_C(1) << (abi >= 5 ? 16 : 15)) - 1, abi >= 4 ? NET_TCP : 0,
                                abi >= 6 ? SCOPES : 0};
    size_t size = abi >= 6 ? 24 : abi >= 4 ? 16 : 8;
    int ruleset = (int)syscall(SYS_landlock_create_ruleset, &attr, size, 0);
    if (ruleset < 0)
        return fail("landlock_create_ruleset");
    const struct {
        const char *path;
        uint64_t rights;
    } rules[] = {
        {"/usr", SYSTEM},
        {"/lib", SYSTEM},
        {"/lib64", SYSTEM},
        {"/etc/ld.so.cache", SYSTEM},
        {compiler->executable, FS_EXECUTE | FS_READ_FILE},
        {compiler->library, FS_READ_FILE | FS_READ_DIR},
        {reads[0], FS_READ_FILE},
        {reads[1], FS_READ_FILE},
        {reads[2], FS_READ_FILE},
        {folder, OWN},
        {"/dev/null", FS_READ_FILE | FS_WRITE_FILE},
        {MO_UPGRADE_COMPILE_ASAN ? "/proc/self" : NULL, FS_READ_FILE | FS_READ_DIR},
    };
    bool ok = true;
    for (size_t i = 0; ok && i < sizeof rules / sizeof *rules; i++)
        ok = rules[i].path == NULL || allow(ruleset, rules[i].path, rules[i].rights);
    if (ok && prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
        ok = fail("no_new_privs");
    if (ok && syscall(SYS_landlock_restrict_self, ruleset, 0) != 0)
        ok = fail("landlock_restrict_self");
    return close(ruleset) == 0 && ok;
}

#define LOAD(offset) BPF_STMT(BPF_LD | BPF_W | BPF_ABS, (offset))
#define RETURN(value) BPF_STMT(BPF_RET | BPF_K, (value))
#define ARGUMENT(n) (offsetof(struct seccomp_data, args) + (n) * sizeof(uint64_t))
#define ALLOW SECCOMP_RET_ALLOW
#define DENY (SECCOMP_RET_ERRNO | EPERM)
#define KILL SECCOMP_RET_KILL_PROCESS
enum {
    NAMESPACES = CLONE_NEWNS | CLONE_NEWCGROUP | CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWUSER |
                 CLONE_NEWPID | CLONE_NEWNET,
    PROGRAM = 4 + X32_CHECK + 2 * REFUSED + 2 + 5 + 2 * 5 + 3 * 6 + 1,
    COMMAND_PROGRAM = 4 + X32_CHECK + 2 * (REFUSED - 2) + 7 + 7 + 2 + 5 + 2 * 7 + 5 + 2 * 6 + 1,
};
struct program {
    struct sock_filter code[COMMAND_PROGRAM];
    unsigned short count;
};
static void put(struct program *p, struct sock_filter instruction) {
    MO_ASSERT(p->count < COMMAND_PROGRAM);
    p->code[p->count++] = instruction;
}
static void when(struct program *p, unsigned call, unsigned argument, const uint32_t *values,
                 unsigned count, uint32_t matched, uint32_t otherwise) {
    put(p, (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, call, 0, (uint8_t)(count + 3)));
    put(p, (struct sock_filter)LOAD((uint32_t)ARGUMENT(argument)));
    for (unsigned i = 0; i < count; i++)
        put(p, (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, values[i],
                                            (uint8_t)(count - 1 - i), (uint8_t)(i + 1 == count)));
    put(p, (struct sock_filter)RETURN(matched));
    put(p, (struct sock_filter)RETURN(otherwise));
}
static void when_arg64(struct program *p, unsigned call, unsigned argument, uint32_t low,
                       uint32_t high, uint32_t matched, uint32_t otherwise) {
    put(p, (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, call, 0, 6));
    put(p, (struct sock_filter)LOAD((uint32_t)ARGUMENT(argument)));
    put(p, (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, low, 0, 3));
    put(p, (struct sock_filter)LOAD((uint32_t)(ARGUMENT(argument) + 4)));
    put(p, (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, high, 0, 1));
    put(p, (struct sock_filter)RETURN(matched));
    put(p, (struct sock_filter)RETURN(otherwise));
}
static void allow_child_signal(struct program *p, unsigned call, uint32_t parent) {
    put(p, (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, call, 0, 6));
    put(p, (struct sock_filter)LOAD((uint32_t)ARGUMENT(0)));
    put(p, (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 1u, 3, 0));
    put(p, (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, parent, 2, 0));
    put(p, (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, 0x80000000u, 1, 0));
    put(p, (struct sock_filter)RETURN(ALLOW));
    put(p, (struct sock_filter)RETURN(DENY));
}
static bool seccomp(void) {
    struct program p = {.count = 0};
    put(&p, (struct sock_filter)LOAD(offsetof(struct seccomp_data, arch)));
    put(&p, (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, NATIVE_ARCH, 1, 0));
    put(&p, (struct sock_filter)RETURN(KILL));
    put(&p, (struct sock_filter)LOAD(offsetof(struct seccomp_data, nr)));
#if X32_CHECK
    put(&p, (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, 0x40000000u, 0, 1));
    put(&p, (struct sock_filter)RETURN(KILL));
#endif
    for (unsigned i = 0; i < REFUSED; i++) {
        put(&p, (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, refused[i], 0, 1));
        put(&p, (struct sock_filter)RETURN(DENY));
    }
    put(&p, (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_clone3, 0, 1));
    put(&p, (struct sock_filter)RETURN(SECCOMP_RET_ERRNO | ENOSYS));
    put(&p, (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_clone, 0, 4));
    put(&p, (struct sock_filter)LOAD((uint32_t)ARGUMENT(0)));
    put(&p, (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JSET | BPF_K, NAMESPACES, 0, 1));
    put(&p, (struct sock_filter)RETURN(DENY));
    put(&p, (struct sock_filter)RETURN(ALLOW));
    const uint32_t self[] = {(uint32_t)getpid(), 0};
    when(&p, SYS_kill, 0, self, 1, ALLOW, DENY);
    when(&p, SYS_tgkill, 0, self, 1, ALLOW, DENY);
    when(&p, SYS_prlimit64, 0, self, 2, ALLOW, DENY);
    const uint32_t owners[] = {F_SETOWN, F_SETOWN_EX}, devices[] = {FIOSETOWN, SIOCSPGRP};
    when(&p, SYS_fcntl, 1, owners, 2, DENY, ALLOW);
    when(&p, SYS_ioctl, 1, devices, 2, DENY, ALLOW);
    put(&p, (struct sock_filter)RETURN(ALLOW));
    MO_ASSERT(p.count == PROGRAM);
    struct sock_fprog program = {p.count, p.code};
    return syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &program) == 0 || fail("seccomp");
}
static bool command_seccomp(pid_t parent_pid) {
    struct program p = {.count = 0};
    put(&p, (struct sock_filter)LOAD(offsetof(struct seccomp_data, arch)));
    put(&p, (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, NATIVE_ARCH, 1, 0));
    put(&p, (struct sock_filter)RETURN(KILL));
    put(&p, (struct sock_filter)LOAD(offsetof(struct seccomp_data, nr)));
#if X32_CHECK
    put(&p, (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, 0x40000000u, 0, 1));
    put(&p, (struct sock_filter)RETURN(KILL));
#endif
    for (unsigned i = 0; i < REFUSED; i++) {
        if (refused[i] == SYS_socketpair || refused[i] == SYS_sendto)
            continue;
        put(&p, (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, refused[i], 0, 1));
        put(&p, (struct sock_filter)RETURN(DENY));
    }
    when_arg64(&p, SYS_socketpair, 0, AF_UNIX, 0, ALLOW, DENY);
    when_arg64(&p, SYS_sendto, 4, 0, 0, ALLOW, DENY);
    put(&p, (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_clone3, 0, 1));
    put(&p, (struct sock_filter)RETURN(SECCOMP_RET_ERRNO | ENOSYS));
    put(&p, (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_clone, 0, 4));
    put(&p, (struct sock_filter)LOAD((uint32_t)ARGUMENT(0)));
    put(&p, (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JSET | BPF_K, NAMESPACES, 0, 1));
    put(&p, (struct sock_filter)RETURN(DENY));
    put(&p, (struct sock_filter)RETURN(ALLOW));
    allow_child_signal(&p, SYS_kill, (uint32_t)parent_pid);
    allow_child_signal(&p, SYS_tgkill, (uint32_t)parent_pid);
    const uint32_t self_zero[] = {0};
    when(&p, SYS_prlimit64, 0, self_zero, 1, ALLOW, DENY);
    const uint32_t owners[] = {F_SETOWN, F_SETOWN_EX}, devices[] = {FIOSETOWN, SIOCSPGRP};
    when(&p, SYS_fcntl, 1, owners, 2, DENY, ALLOW);
    when(&p, SYS_ioctl, 1, devices, 2, DENY, ALLOW);
    put(&p, (struct sock_filter)RETURN(ALLOW));
    MO_ASSERT(p.count == COMMAND_PROGRAM);
    struct sock_fprog program = {p.count, p.code};
    return syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &program) == 0 || fail("seccomp");
}

bool mo_compile_confine(const struct mo_compiler *compiler, const char *folder,
                        const char *const reads[3]) {
    return landlock(compiler, folder, reads) && seccomp();
}
bool mo_command_confine(const char *executable, const char *folder, const char *tmpdir) {
    pid_t parent_pid = getppid();
    long abi = landlock_abi();
    if (abi < 3)
        return fail("landlock_create_ruleset");
    if (abi > NEWEST_ABI)
        abi = NEWEST_ABI;
    struct ruleset_attr attr = {(UINT64_C(1) << (abi >= 5 ? 16 : 15)) - 1, abi >= 4 ? NET_TCP : 0,
                                abi >= 6 ? SCOPES : 0};
    size_t size = abi >= 6 ? 24 : abi >= 4 ? 16 : 8;
    int ruleset = (int)syscall(SYS_landlock_create_ruleset, &attr, size, 0);
    if (ruleset < 0)
        return fail("landlock_create_ruleset");
    char local_bin[MO_LIMIT_HOST_PATH_SIZE];
    const char *local_bin_rule = NULL;
    static const char marker[] = "/.local/bin/";
    const char *found = strstr(executable, marker);
    if (found != NULL && strchr(found + sizeof marker - 1, '/') == NULL) {
        size_t dir_len = (size_t)(found - executable) + (sizeof marker - 2);
        if (dir_len < sizeof local_bin) {
            memcpy(local_bin, executable, dir_len);
            local_bin[dir_len] = '\0';
            local_bin_rule = local_bin;
        }
    }
    uint64_t own_rights = COMMAND_OWN & attr.handled_fs;
    const struct {
        const char *path;
        uint64_t rights;
    } rules[] = {
        {"/usr", SYSTEM},
        {"/bin", SYSTEM},
        {"/lib", SYSTEM},
        {"/lib64", SYSTEM},
        {"/opt/homebrew", SYSTEM},
        {local_bin_rule, SYSTEM},
        {"/etc/ld.so.cache", FS_READ_FILE},
        {executable, FS_EXECUTE | FS_READ_FILE},
        {folder, own_rights},
        {tmpdir, own_rights},
        {"/dev/null", FS_READ_FILE | FS_WRITE_FILE},
        {"/dev/urandom", FS_READ_FILE},
        {MO_UPGRADE_COMPILE_ASAN ? "/proc/self" : NULL, FS_READ_FILE | FS_READ_DIR},
    };
    bool ok = true;
    for (size_t i = 0; ok && i < sizeof rules / sizeof *rules; i++)
        ok = rules[i].path == NULL || allow(ruleset, rules[i].path, rules[i].rights);
    if (ok && prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
        ok = fail("no_new_privs");
    if (ok && syscall(SYS_landlock_restrict_self, ruleset, 0) != 0)
        ok = fail("landlock_restrict_self");
    return close(ruleset) == 0 && ok && command_seccomp(parent_pid);
}
#else
bool mo_compile_can_confine(void) {
    return false;
}
bool mo_compile_confine(const struct mo_compiler *compiler, const char *folder,
                        const char *const reads[3]) {
    (void)compiler;
    (void)folder;
    (void)reads;
    return false;
}
bool mo_command_confine(const char *executable, const char *folder, const char *tmpdir) {
    (void)executable;
    (void)folder;
    (void)tmpdir;
    return false;
}
#endif
