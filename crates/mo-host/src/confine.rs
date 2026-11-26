//! Process confinement and resource limits for child processes (`Command.run_raw`,
//! `mo emit --bundle`, and `mo test --bundle`).
//!
//! Pure safe Rust (`#![forbid(unsafe_code)]`) using `rustix` for `setpgid`/`setrlimit`/
//! `kill_process_group`/`no_new_privs`, `landlock` (ABI V3–V6: filesystem, TCP net, scopes),
//! and `seccompiler` (Linux Seccomp-BPF).

use std::ffi::OsString;
use std::path::{Path, PathBuf};
use std::process::ExitCode;
use std::sync::OnceLock;

/// Hidden `argv[1]` sentinel triggering the safe self-exec confinement trampoline for `Command`.
pub const INTERNAL_COMMAND_TRAMPOLINE_ARG: &str = "--mo-internal-confine-command";

static TRAMPOLINE_EXE: OnceLock<PathBuf> = OnceLock::new();

/// Registers the currently running binary as the `Command.run_raw` confinement trampoline.
///
/// Must be called from `main()` of `mo` (after [`maybe_run_command_trampoline`]).
pub fn register_self_as_trampoline() {
    if let Ok(exe) = std::env::current_exe() {
        let resolved = std::fs::canonicalize(&exe).unwrap_or(exe);
        let _ = TRAMPOLINE_EXE.set(resolved);
    }
}

/// Explicitly registers a trampoline binary path (useful in integration tests).
pub fn register_trampoline_exe(path: PathBuf) {
    let _ = TRAMPOLINE_EXE.set(path);
}

/// Returns the registered confinement trampoline executable, if any.
#[must_use]
pub fn trampoline_exe() -> Option<&'static Path> {
    TRAMPOLINE_EXE.get().map(PathBuf::as_path)
}

/// Places the current process in its own process group (`setpgid(0, 0)`).
pub fn enter_own_process_group() {
    #[cfg(unix)]
    {
        let _ = rustix::process::setpgid(None, None);
    }
}

/// Best-effort parent-side `setpgid(pid, pid)` to close the fork/exec process-group race.
pub fn set_child_process_group(pid: u32) {
    #[cfg(unix)]
    {
        if let Ok(raw) = i32::try_from(pid)
            && let Some(p) = rustix::process::Pid::from_raw(raw)
        {
            let _ = rustix::process::setpgid(Some(p), Some(p));
        }
    }
    #[cfg(not(unix))]
    {
        let _ = pid;
    }
}

/// Sends `SIGKILL` to the child's entire process group and to the direct child PID.
pub fn kill_child_tree(pid: u32) {
    #[cfg(unix)]
    {
        if let Ok(raw) = i32::try_from(pid)
            && let Some(p) = rustix::process::Pid::from_raw(raw)
        {
            let _ = rustix::process::kill_process_group(p, rustix::process::Signal::KILL);
            let _ = rustix::process::kill_process(p, rustix::process::Signal::KILL);
        }
    }
    #[cfg(not(unix))]
    {
        let _ = pid;
    }
}

/// Applies `RLIMIT_CORE = 0`, `RLIMIT_CPU = cpu_seconds`, and `RLIMIT_AS = memory_bytes`.
///
/// Each target is clamped to the process's existing hard limit so containers with tighter
/// pre-existing hard limits do not fail with `EPERM`.
pub fn apply_child_rlimits(memory_bytes: u64, cpu_seconds: u64) -> Result<(), &'static str> {
    #[cfg(unix)]
    {
        use rustix::process::{Resource, Rlimit, getrlimit, setrlimit};

        fn set_bound(
            resource: Resource,
            wanted: u64,
            label: &'static str,
        ) -> Result<(), &'static str> {
            let existing = getrlimit(resource);
            let effective = match existing.maximum {
                Some(max) => wanted.min(max),
                None => wanted,
            };
            let new_lim = Rlimit {
                current: Some(effective),
                maximum: Some(effective),
            };
            setrlimit(resource, new_lim).map_err(|_| label)
        }

        set_bound(Resource::Core, 0, "setrlimit(RLIMIT_CORE)")?;
        set_bound(Resource::Cpu, cpu_seconds, "setrlimit(RLIMIT_CPU)")?;
        set_bound(Resource::As, memory_bytes, "setrlimit(RLIMIT_AS)")?;
        Ok(())
    }
    #[cfg(not(unix))]
    {
        let _ = (memory_bytes, cpu_seconds);
        Ok(())
    }
}

/// Returns `true` if the running kernel supports both Linux Landlock ABI >= 3 and Seccomp
/// `SECCOMP_RET_KILL_PROCESS` on a supported little-endian architecture (`x86_64` or `aarch64`).
#[must_use]
pub fn can_confine() -> bool {
    #[cfg(all(
        target_os = "linux",
        target_endian = "little",
        any(target_arch = "x86_64", target_arch = "aarch64")
    ))]
    {
        linux::can_confine_linux()
    }
    #[cfg(not(all(
        target_os = "linux",
        target_endian = "little",
        any(target_arch = "x86_64", target_arch = "aarch64")
    )))]
    {
        false
    }
}

/// Applies fail-closed Landlock (ABI V3–V6) + Seccomp-BPF confinement to a compile or door-check
/// child process (`mo emit --bundle` / `mo test --bundle`).
pub fn confine_compile_child(
    executable: &Path,
    work_dir: &Path,
    library: Option<&Path>,
    read_files: &[&Path],
) -> Result<(), String> {
    #[cfg(all(
        target_os = "linux",
        target_endian = "little",
        any(target_arch = "x86_64", target_arch = "aarch64")
    ))]
    {
        linux::confine_compile_child_linux(executable, work_dir, library, read_files)
    }
    #[cfg(not(all(
        target_os = "linux",
        target_endian = "little",
        any(target_arch = "x86_64", target_arch = "aarch64")
    )))]
    {
        let _ = (executable, work_dir, library, read_files);
        Err("confinement is unsupported on this host".to_string())
    }
}

/// Applies fail-closed Landlock (ABI V3–V6) + Seccomp-BPF confinement to a `Command.run_raw`
/// or `Command.spawn_raw` child process.
pub fn confine_command_child(
    executable: &Path,
    work_dir: &Path,
    tmp_dir: &Path,
    parent_pid: u32,
    allow_loopback: bool,
    loopback_ports_filter: Option<&[u16]>,
) -> Result<(), String> {
    #[cfg(all(
        target_os = "linux",
        target_endian = "little",
        any(target_arch = "x86_64", target_arch = "aarch64")
    ))]
    {
        linux::confine_command_child_linux(
            executable,
            work_dir,
            tmp_dir,
            parent_pid,
            allow_loopback,
            loopback_ports_filter,
        )
    }
    #[cfg(not(all(
        target_os = "linux",
        target_endian = "little",
        any(target_arch = "x86_64", target_arch = "aarch64")
    )))]
    {
        let _ = (
            executable,
            work_dir,
            tmp_dir,
            parent_pid,
            allow_loopback,
            loopback_ports_filter,
        );
        Err("confinement is unsupported on this host".to_string())
    }
}

/// Safe self-exec trampoline for `Command.run_raw`.
///
/// Intercepts `argv[1] == "--mo-internal-confine-command"` at the start of `main()`, applies
/// `setpgid(0, 0)`, verifies the working directory identity (`dev`, `ino`), enforces
/// `RLIMIT_CORE`/`RLIMIT_CPU`/`RLIMIT_AS`, optionally installs Landlock + Seccomp-BPF, and
/// `exec`s the target executable via safe `CommandExt::exec`.
#[cfg(unix)]
pub fn maybe_run_command_trampoline(args: impl IntoIterator<Item = OsString>) -> Option<ExitCode> {
    let mut iter = args.into_iter();
    let _argv0 = iter.next()?;
    let first = iter.next()?;
    if first != INTERNAL_COMMAND_TRAMPOLINE_ARG {
        return None;
    }

    let code = run_trampoline_inner(iter.collect());
    Some(ExitCode::from(code))
}

#[cfg(unix)]
fn fail_trampoline(step: &str) -> u8 {
    use std::io::Write;
    let _ = writeln!(std::io::stderr(), "mo: cannot confine the child: {step}");
    let _ = std::io::stderr().flush();
    125
}

#[cfg(unix)]
fn run_trampoline_inner(args: Vec<OsString>) -> u8 {
    use std::os::unix::process::CommandExt;
    use std::process::Command;

    let mut expected_dev: Option<String> = None;
    let mut expected_ino: Option<String> = None;
    let mut workdir: Option<PathBuf> = None;
    let mut tmpdir: Option<PathBuf> = None;
    let mut cpu_seconds: Option<u64> = None;
    let mut mem_bytes: Option<u64> = None;
    let mut parent_pid: Option<u32> = None;
    let mut confine = false;
    let mut allow_loopback = false;
    let mut loopback_ports_filter: Option<Vec<u16>> = None;
    let mut exe_path: Option<PathBuf> = None;
    let mut cmd_args: Vec<OsString> = Vec::new();

    let mut i = 0usize;
    while i < args.len() {
        let a = &args[i];
        if a == "--" {
            cmd_args.extend(args[i + 1..].iter().cloned());
            break;
        }
        let s = a.to_string_lossy();
        match s.as_ref() {
            "--dev" => {
                i += 1;
                expected_dev = args.get(i).map(|v| v.to_string_lossy().into_owned());
            }
            "--ino" => {
                i += 1;
                expected_ino = args.get(i).map(|v| v.to_string_lossy().into_owned());
            }
            "--workdir" => {
                i += 1;
                workdir = args.get(i).map(PathBuf::from);
            }
            "--tmpdir" => {
                i += 1;
                tmpdir = args.get(i).map(PathBuf::from);
            }
            "--cpu" => {
                i += 1;
                cpu_seconds = args.get(i).and_then(|v| v.to_string_lossy().parse().ok());
            }
            "--mem" => {
                i += 1;
                mem_bytes = args.get(i).and_then(|v| v.to_string_lossy().parse().ok());
            }
            "--ppid" => {
                i += 1;
                parent_pid = args.get(i).and_then(|v| v.to_string_lossy().parse().ok());
            }
            "--confine" => confine = true,
            "--no-confine" => confine = false,
            "--allow-loopback" => allow_loopback = true,
            "--loopback-ports" => {
                i += 1;
                let Some(raw) = args.get(i).and_then(|v| v.to_str()) else {
                    return fail_trampoline("invalid --loopback-ports argument");
                };
                if raw.is_empty() {
                    return fail_trampoline("empty --loopback-ports argument");
                }
                let mut parsed = Vec::new();
                for tok in raw.split(',') {
                    let Ok(p) = tok.parse::<u16>() else {
                        return fail_trampoline("invalid port in --loopback-ports");
                    };
                    if p == 0 {
                        return fail_trampoline("zero port in --loopback-ports");
                    }
                    parsed.push(p);
                }
                loopback_ports_filter = Some(parsed);
            }
            "--exe" => {
                i += 1;
                exe_path = args.get(i).map(PathBuf::from);
            }
            _ => return fail_trampoline("invalid trampoline argument"),
        }
        i += 1;
    }

    let (Some(dev), Some(ino), Some(wd), Some(td), Some(cpu), Some(mem), Some(ppid), Some(exe)) = (
        expected_dev,
        expected_ino,
        workdir,
        tmpdir,
        cpu_seconds,
        mem_bytes,
        parent_pid,
        exe_path,
    ) else {
        return fail_trampoline("incomplete trampoline arguments");
    };

    enter_own_process_group();

    // Verify cwd identity matches the capability-resolved directory fd (eliminates TOCTOU).
    match rustix::fs::statat(rustix::fs::CWD, ".", rustix::fs::AtFlags::empty()) {
        Ok(st) if st.st_dev.to_string() == dev && st.st_ino.to_string() == ino => {}
        _ => return fail_trampoline("working directory changed"),
    }

    if let Err(step) = apply_child_rlimits(mem, cpu) {
        return fail_trampoline(step);
    }

    if confine
        && let Err(step) = confine_command_child(
            &exe,
            &wd,
            &td,
            ppid,
            allow_loopback,
            loopback_ports_filter.as_deref(),
        )
    {
        return fail_trampoline(&step);
    }

    let err = Command::new(&exe).args(&cmd_args).exec();
    fail_trampoline(&format!("exec failed: {err}"))
}

#[cfg(all(
    target_os = "linux",
    target_endian = "little",
    any(target_arch = "x86_64", target_arch = "aarch64")
))]
mod linux {
    use std::collections::BTreeMap;
    use std::io::ErrorKind;
    use std::path::Path;

    use landlock::{
        ABI, Access, AccessFs, AccessNet, BitFlags, CompatLevel, Compatible, NetPort, PathBeneath,
        PathFd, PathFdError, Ruleset, RulesetAttr, RulesetCreated, RulesetCreatedAttr,
        RulesetStatus, Scope,
    };
    use seccompiler::{
        BpfProgram, SeccompAction, SeccompCmpArgLen, SeccompCmpOp, SeccompCondition, SeccompFilter,
        SeccompRule, TargetArch, sock_filter,
    };

    const SYSTEM_READ: BitFlags<AccessFs> = landlock::make_bitflags!(AccessFs::{
        Execute | ReadFile | ReadDir
    });
    const COMPILE_OWN: BitFlags<AccessFs> = landlock::make_bitflags!(AccessFs::{
        ReadFile | ReadDir | WriteFile | MakeReg | RemoveFile | Truncate
    });
    const COMMAND_OWN: BitFlags<AccessFs> = landlock::make_bitflags!(AccessFs::{
        ReadFile | ReadDir | WriteFile | MakeReg | RemoveFile | Truncate
            | RemoveDir | MakeDir | MakeSym | Refer
    });
    const FILE_ONLY_MASK: BitFlags<AccessFs> = landlock::make_bitflags!(AccessFs::{
        Execute | WriteFile | ReadFile | Truncate | IoctlDev
    });

    const F_SETOWN_EX: u64 = 15;
    const FIOSETOWN: u64 = 0x8901;
    const SIOCSPGRP: u64 = 0x8902;

    const NAMESPACE_FLAGS: [u64; 7] = [
        libc::CLONE_NEWNS as u64,
        libc::CLONE_NEWCGROUP as u64,
        libc::CLONE_NEWUTS as u64,
        libc::CLONE_NEWIPC as u64,
        libc::CLONE_NEWUSER as u64,
        libc::CLONE_NEWPID as u64,
        libc::CLONE_NEWNET as u64,
    ];

    /// Syscalls unconditionally denied with `EPERM` in compile/check children (exact parity with
    /// C `src/host/confine.c:refused[]`).
    const REFUSED_SYSCALLS: &[i64] = &[
        libc::SYS_socket,
        libc::SYS_socketpair,
        libc::SYS_connect,
        libc::SYS_bind,
        libc::SYS_sendto,
        libc::SYS_sendmsg,
        libc::SYS_sendmmsg,
        libc::SYS_tkill,
        libc::SYS_rt_sigqueueinfo,
        libc::SYS_rt_tgsigqueueinfo,
        libc::SYS_pidfd_open,
        libc::SYS_pidfd_send_signal,
        libc::SYS_pidfd_getfd,
        libc::SYS_process_vm_readv,
        libc::SYS_process_vm_writev,
        libc::SYS_process_madvise,
        libc::SYS_kcmp,
        libc::SYS_perf_event_open,
        libc::SYS_io_uring_setup,
        libc::SYS_io_uring_enter,
        libc::SYS_io_uring_register,
        libc::SYS_mount,
        libc::SYS_umount2,
        libc::SYS_pivot_root,
        libc::SYS_open_tree,
        libc::SYS_move_mount,
        libc::SYS_fsopen,
        libc::SYS_fsconfig,
        libc::SYS_fsmount,
        libc::SYS_fspick,
        libc::SYS_mount_setattr,
        libc::SYS_unshare,
        libc::SYS_setns,
        libc::SYS_setpriority,
        libc::SYS_ioprio_set,
        libc::SYS_sched_setaffinity,
        libc::SYS_sched_setscheduler,
        libc::SYS_sched_setparam,
        libc::SYS_sched_setattr,
        libc::SYS_add_key,
        libc::SYS_request_key,
        libc::SYS_keyctl,
        libc::SYS_shmget,
        libc::SYS_shmat,
        libc::SYS_shmctl,
        libc::SYS_msgget,
        libc::SYS_msgsnd,
        libc::SYS_msgrcv,
        libc::SYS_msgctl,
        libc::SYS_semget,
        libc::SYS_semop,
        libc::SYS_semtimedop,
        libc::SYS_semctl,
        libc::SYS_mq_open,
        libc::SYS_mq_unlink,
        libc::SYS_mq_timedsend,
        libc::SYS_mq_timedreceive,
        libc::SYS_mq_notify,
        libc::SYS_mq_getsetattr,
        libc::SYS_ptrace,
        libc::SYS_setsid,
        libc::SYS_setpgid,
    ];

    pub(super) fn can_confine_linux() -> bool {
        let landlock_ok = Ruleset::default()
            .set_compatibility(CompatLevel::HardRequirement)
            .handle_access(AccessFs::from_all(ABI::V3))
            .and_then(Ruleset::create)
            .is_ok();
        if !landlock_ok {
            return false;
        }
        std::fs::read_to_string("/proc/sys/kernel/seccomp/actions_avail")
            .is_ok_and(|s| s.split_whitespace().any(|tok| tok == "kill_process"))
    }

    fn create_ruleset_v3_to_v6(require_net_v4: bool) -> Result<RulesetCreated, String> {
        // First confirm Landlock ABI >= 3 is supported.
        Ruleset::default()
            .set_compatibility(CompatLevel::HardRequirement)
            .handle_access(AccessFs::from_all(ABI::V3))
            .and_then(Ruleset::create)
            .map_err(|e| format!("landlock_create_ruleset(v3 probe): {e}"))?;

        let net_access = AccessNet::from_all(ABI::V4);
        if require_net_v4 {
            Ruleset::default()
                .set_compatibility(CompatLevel::HardRequirement)
                .handle_access(net_access)
                .and_then(Ruleset::create)
                .map_err(|e| format!("landlock_create_ruleset(v4 net probe): {e}"))?;
        }

        // Build best-effort ABI V6 ruleset (FS + TCP net + signal/abstract-unix scopes);
        // `restrict_self` verifies enforcement is not NotEnforced.
        Ruleset::default()
            .handle_access(AccessFs::from_all(ABI::V6))
            .map_err(|e| format!("landlock handle_access(fs): {e}"))?
            .handle_access(net_access)
            .map_err(|e| format!("landlock handle_access(net): {e}"))?
            .scope(Scope::from_all(ABI::V6))
            .map_err(|e| format!("landlock scope(v6): {e}"))?
            .create()
            .map_err(|e| format!("landlock_create_ruleset: {e}"))
    }

    /// Reads `/proc/net/tcp` and `/proc/net/tcp6` before Landlock restricts filesystem access,
    /// returning all TCP ports currently listening strictly on loopback (`127.0.0.0/8`, `::1`, or
    /// `::ffff:127.0.0.0/104`). Listeners on `0.0.0.0`, `::`, or external NICs are excluded so
    /// Landlock `AccessNet::ConnectTcp` blocks every non-loopback destination port with `EACCES`.
    fn collect_loopback_listen_ports() -> Vec<u16> {
        let mut ports = Vec::new();
        if let Ok(tcp4) = std::fs::read_to_string("/proc/net/tcp") {
            for line in tcp4.lines().skip(1) {
                let mut cols = line.split_whitespace();
                let _slot = cols.next();
                let Some(local) = cols.next() else { continue };
                let _rem = cols.next();
                let Some(state) = cols.next() else { continue };
                if !state.eq_ignore_ascii_case("0A") {
                    continue;
                }
                let Some((ip_hex, port_hex)) = local.split_once(':') else {
                    continue;
                };
                if ip_hex.len() == 8
                    && ip_hex[6..8].eq_ignore_ascii_case("7F")
                    && let Ok(port) = u16::from_str_radix(port_hex, 16)
                    && port > 0
                {
                    ports.push(port);
                }
            }
        }
        if let Ok(tcp6) = std::fs::read_to_string("/proc/net/tcp6") {
            for line in tcp6.lines().skip(1) {
                let mut cols = line.split_whitespace();
                let _slot = cols.next();
                let Some(local) = cols.next() else { continue };
                let _rem = cols.next();
                let Some(state) = cols.next() else { continue };
                if !state.eq_ignore_ascii_case("0A") {
                    continue;
                }
                let Some((ip6_hex, port_hex)) = local.split_once(':') else {
                    continue;
                };
                let is_loopback6 = ip6_hex.len() == 32
                    && (ip6_hex.eq_ignore_ascii_case("00000000000000000000000001000000")
                        || (ip6_hex[..24].eq_ignore_ascii_case("0000000000000000FFFF0000")
                            && ip6_hex[30..32].eq_ignore_ascii_case("7F")));
                if is_loopback6
                    && let Ok(port) = u16::from_str_radix(port_hex, 16)
                    && port > 0
                {
                    ports.push(port);
                }
            }
        }
        ports.sort_unstable();
        ports.dedup();
        ports
    }

    fn allow_path(
        mut ruleset: RulesetCreated,
        path: &Path,
        rights: BitFlags<AccessFs>,
    ) -> Result<RulesetCreated, String> {
        let meta = match std::fs::metadata(path) {
            Ok(m) => m,
            Err(e) if e.kind() == ErrorKind::NotFound => return Ok(ruleset),
            Err(e) => return Err(format!("{}: {e}", path.display())),
        };
        let effective = if meta.is_dir() {
            rights
        } else {
            rights & FILE_ONLY_MASK
        };
        if effective.is_empty() {
            return Ok(ruleset);
        }
        let fd = match PathFd::new(path) {
            Ok(fd) => fd,
            Err(PathFdError::OpenCall { ref source, .. })
                if source.kind() == ErrorKind::NotFound =>
            {
                return Ok(ruleset);
            }
            Err(e) => return Err(format!("{}: {e}", path.display())),
        };
        ruleset = ruleset
            .add_rule(PathBeneath::new(fd, effective))
            .map_err(|e| format!("{}: {e}", path.display()))?;
        Ok(ruleset)
    }

    fn finish_landlock(ruleset: RulesetCreated) -> Result<(), String> {
        let status = ruleset
            .restrict_self()
            .map_err(|e| format!("landlock_restrict_self: {e}"))?;
        if status.ruleset == RulesetStatus::NotEnforced {
            return Err("landlock_restrict_self: ruleset not enforced".to_string());
        }
        if !status.no_new_privs {
            return Err("no_new_privs not enforced".to_string());
        }
        Ok(())
    }

    fn target_arch() -> Result<TargetArch, String> {
        TargetArch::try_from(std::env::consts::ARCH)
            .map_err(|e| format!("unsupported seccomp arch: {e}"))
    }

    fn rule(conds: Vec<SeccompCondition>) -> Result<SeccompRule, String> {
        SeccompRule::new(conds).map_err(|e| format!("seccomp rule: {e}"))
    }

    fn cond(
        idx: u8,
        len: SeccompCmpArgLen,
        op: SeccompCmpOp,
        val: u64,
    ) -> Result<SeccompCondition, String> {
        SeccompCondition::new(idx, len, op, val).map_err(|e| format!("seccomp condition: {e}"))
    }

    #[cfg(target_arch = "x86_64")]
    fn apply_x32_kill_filter() -> Result<(), String> {
        // Reject x32 ABI syscalls (nr >= 0x4000_0000) with KILL_PROCESS (matching C confine.c).
        const BPF_LD: u16 = 0x00;
        const BPF_W: u16 = 0x00;
        const BPF_ABS: u16 = 0x20;
        const BPF_JMP: u16 = 0x05;
        const BPF_JGE: u16 = 0x30;
        const BPF_K: u16 = 0x00;
        const BPF_RET: u16 = 0x06;
        const X32_SYSCALL_BIT: u32 = 0x4000_0000;

        let prog: BpfProgram = vec![
            // Load `seccomp_data.nr` (offset 0).
            sock_filter {
                code: BPF_LD | BPF_W | BPF_ABS,
                jt: 0,
                jf: 0,
                k: 0,
            },
            // If nr >= 0x4000_0000, jump to KILL, else jump to ALLOW.
            sock_filter {
                code: BPF_JMP | BPF_JGE | BPF_K,
                jt: 0,
                jf: 1,
                k: X32_SYSCALL_BIT,
            },
            sock_filter {
                code: BPF_RET | BPF_K,
                jt: 0,
                jf: 0,
                k: libc::SECCOMP_RET_KILL_PROCESS,
            },
            sock_filter {
                code: BPF_RET | BPF_K,
                jt: 0,
                jf: 0,
                k: libc::SECCOMP_RET_ALLOW,
            },
        ];
        seccompiler::apply_filter(&prog).map_err(|e| format!("seccomp(x32): {e}"))
    }

    fn apply_clone3_enosys_filter(arch: TargetArch) -> Result<(), String> {
        let mut rules: BTreeMap<i64, Vec<SeccompRule>> = BTreeMap::new();
        rules.insert(libc::SYS_clone3, Vec::new());
        let filter = SeccompFilter::new(
            rules,
            SeccompAction::Allow,
            SeccompAction::Errno(libc::ENOSYS as u32),
            arch,
        )
        .map_err(|e| format!("seccomp(clone3): {e}"))?;
        let prog: BpfProgram = filter
            .try_into()
            .map_err(|e| format!("seccomp(clone3 bpf): {e}"))?;
        seccompiler::apply_filter(&prog).map_err(|e| format!("seccomp(clone3 apply): {e}"))
    }

    fn add_common_conditional_rules(
        rules: &mut BTreeMap<i64, Vec<SeccompRule>>,
    ) -> Result<(), String> {
        // Block any namespace creation flags in `clone(flags, ...)`.
        let mut clone_rules = Vec::with_capacity(NAMESPACE_FLAGS.len());
        for flag in NAMESPACE_FLAGS {
            clone_rules.push(rule(vec![cond(
                0,
                SeccompCmpArgLen::Dword,
                SeccompCmpOp::MaskedEq(flag),
                flag,
            )?])?);
        }
        rules.insert(libc::SYS_clone, clone_rules);

        // Block async I/O signal ownership via `fcntl` and `ioctl`.
        rules.insert(
            libc::SYS_fcntl,
            vec![
                rule(vec![cond(
                    1,
                    SeccompCmpArgLen::Dword,
                    SeccompCmpOp::Eq,
                    libc::F_SETOWN as u64,
                )?])?,
                rule(vec![cond(
                    1,
                    SeccompCmpArgLen::Dword,
                    SeccompCmpOp::Eq,
                    F_SETOWN_EX,
                )?])?,
            ],
        );
        rules.insert(
            libc::SYS_ioctl,
            vec![
                rule(vec![cond(
                    1,
                    SeccompCmpArgLen::Dword,
                    SeccompCmpOp::Eq,
                    FIOSETOWN,
                )?])?,
                rule(vec![cond(
                    1,
                    SeccompCmpArgLen::Dword,
                    SeccompCmpOp::Eq,
                    SIOCSPGRP,
                )?])?,
            ],
        );
        Ok(())
    }

    fn apply_compile_seccomp() -> Result<(), String> {
        let arch = target_arch()?;
        let self_pid = u64::from(std::process::id());

        let mut rules: BTreeMap<i64, Vec<SeccompRule>> = BTreeMap::new();
        for &nr in REFUSED_SYSCALLS {
            rules.insert(nr, Vec::new());
        }
        add_common_conditional_rules(&mut rules)?;

        // Compile/check child may only signal itself (`kill`/`tgkill` with arg0 == self_pid).
        let non_self = vec![rule(vec![cond(
            0,
            SeccompCmpArgLen::Dword,
            SeccompCmpOp::Ne,
            self_pid,
        )?])?];
        rules.insert(libc::SYS_kill, non_self.clone());
        rules.insert(libc::SYS_tgkill, non_self);

        // `prlimit64` only permitted for self (arg0 == 0 or arg0 == self_pid).
        rules.insert(
            libc::SYS_prlimit64,
            vec![rule(vec![
                cond(0, SeccompCmpArgLen::Dword, SeccompCmpOp::Ne, 0)?,
                cond(0, SeccompCmpArgLen::Dword, SeccompCmpOp::Ne, self_pid)?,
            ])?],
        );

        let filter = SeccompFilter::new(
            rules,
            SeccompAction::Allow,
            SeccompAction::Errno(libc::EPERM as u32),
            arch,
        )
        .map_err(|e| format!("seccomp(compile): {e}"))?;
        let prog: BpfProgram = filter
            .try_into()
            .map_err(|e| format!("seccomp(compile bpf): {e}"))?;

        #[cfg(target_arch = "x86_64")]
        apply_x32_kill_filter()?;
        apply_clone3_enosys_filter(arch)?;
        seccompiler::apply_filter(&prog).map_err(|e| format!("seccomp(compile apply): {e}"))
    }

    fn apply_command_seccomp(parent_pid: u32, allow_loopback: bool) -> Result<(), String> {
        let arch = target_arch()?;

        let mut rules: BTreeMap<i64, Vec<SeccompRule>> = BTreeMap::new();
        for &nr in REFUSED_SYSCALLS {
            if nr == libc::SYS_socketpair || nr == libc::SYS_sendto {
                continue;
            }
            if allow_loopback
                && (nr == libc::SYS_socket
                    || nr == libc::SYS_connect
                    || nr == libc::SYS_bind
                    || nr == libc::SYS_sendmsg)
            {
                continue;
            }
            rules.insert(nr, Vec::new());
        }

        if allow_loopback {
            // Permit strictly `socket(AF_INET | AF_INET6, SOCK_STREAM [| flags], ...)`.
            // `AF_UNIX` `socket()` is denied with `EPERM` (preventing connections to Docker/D-Bus
            // filesystem sockets), while `socketpair(AF_UNIX, ...)` remains permitted below.
            // Among 4-bit socket base types `0..=15`, `SOCK_STREAM == 1` is the unique value with
            // `(type & 1) == 1 && (type & 2) == 0 && (type & 4) == 0 && (type & 8) == 0`
            // (`SOCK_NONBLOCK = 0x800` and `SOCK_CLOEXEC = 0x80000` do not touch bits `0..=3`).
            let mut socket_deny = vec![rule(vec![
                cond(
                    0,
                    SeccompCmpArgLen::Dword,
                    SeccompCmpOp::Ne,
                    libc::AF_INET as u64,
                )?,
                cond(
                    0,
                    SeccompCmpArgLen::Dword,
                    SeccompCmpOp::Ne,
                    libc::AF_INET6 as u64,
                )?,
            ])?];
            for (mask, bad_val) in [(1u64, 0u64), (2, 2), (4, 4), (8, 8)] {
                socket_deny.push(rule(vec![cond(
                    1,
                    SeccompCmpArgLen::Dword,
                    SeccompCmpOp::MaskedEq(mask),
                    bad_val,
                )?])?);
            }
            // Also restrict protocol (arg2) to 0 (default TCP) or IPPROTO_TCP so non-TCP
            // stream protocols (e.g. SCTP 132, MPTCP 262) cannot slip past Landlock TCP rules.
            socket_deny.push(rule(vec![
                cond(2, SeccompCmpArgLen::Dword, SeccompCmpOp::Ne, 0)?,
                cond(
                    2,
                    SeccompCmpArgLen::Dword,
                    SeccompCmpOp::Ne,
                    libc::IPPROTO_TCP as u64,
                )?,
            ])?);
            rules.insert(libc::SYS_socket, socket_deny);
        }

        // Datagram socketpairs can change their peer when connect is permitted.
        let mut socketpair_deny = vec![rule(vec![cond(
            0,
            SeccompCmpArgLen::Dword,
            SeccompCmpOp::Ne,
            libc::AF_UNIX as u64,
        )?])?];
        if allow_loopback {
            socketpair_deny.push(rule(vec![cond(
                1,
                SeccompCmpArgLen::Dword,
                SeccompCmpOp::MaskedEq(0xf),
                libc::SOCK_DGRAM as u64,
            )?])?);
        }
        rules.insert(libc::SYS_socketpair, socketpair_deny);
        rules.insert(
            libc::SYS_sendto,
            vec![rule(vec![cond(
                4,
                SeccompCmpArgLen::Qword,
                SeccompCmpOp::Ne,
                0,
            )?])?],
        );

        add_common_conditional_rules(&mut rules)?;

        // Deny signaling PID 1, parent_pid, or negative/broadcast PIDs (arg0 >= 0x8000_0000).
        let bad_signal_targets = vec![
            rule(vec![cond(0, SeccompCmpArgLen::Dword, SeccompCmpOp::Eq, 1)?])?,
            rule(vec![cond(
                0,
                SeccompCmpArgLen::Dword,
                SeccompCmpOp::Eq,
                u64::from(parent_pid),
            )?])?,
            rule(vec![cond(
                0,
                SeccompCmpArgLen::Dword,
                SeccompCmpOp::Ge,
                0x8000_0000,
            )?])?,
        ];
        rules.insert(libc::SYS_kill, bad_signal_targets.clone());
        rules.insert(libc::SYS_tgkill, bad_signal_targets);

        // `prlimit64` only permitted for self (arg0 == 0).
        rules.insert(
            libc::SYS_prlimit64,
            vec![rule(vec![cond(
                0,
                SeccompCmpArgLen::Dword,
                SeccompCmpOp::Ne,
                0,
            )?])?],
        );

        let filter = SeccompFilter::new(
            rules,
            SeccompAction::Allow,
            SeccompAction::Errno(libc::EPERM as u32),
            arch,
        )
        .map_err(|e| format!("seccomp(command): {e}"))?;
        let prog: BpfProgram = filter
            .try_into()
            .map_err(|e| format!("seccomp(command bpf): {e}"))?;

        #[cfg(target_arch = "x86_64")]
        apply_x32_kill_filter()?;
        apply_clone3_enosys_filter(arch)?;
        seccompiler::apply_filter(&prog).map_err(|e| format!("seccomp(command apply): {e}"))
    }

    pub(super) fn confine_compile_child_linux(
        executable: &Path,
        work_dir: &Path,
        library: Option<&Path>,
        read_files: &[&Path],
    ) -> Result<(), String> {
        rustix::thread::set_no_new_privs(true).map_err(|e| format!("no_new_privs: {e}"))?;

        let mut ruleset = create_ruleset_v3_to_v6(false)?;
        for sys in ["/usr", "/lib", "/lib64", "/etc/ld.so.cache"] {
            ruleset = allow_path(ruleset, Path::new(sys), SYSTEM_READ)?;
        }
        ruleset = allow_path(ruleset, executable, AccessFs::Execute | AccessFs::ReadFile)?;
        if let Some(lib_dir) = library {
            ruleset = allow_path(ruleset, lib_dir, AccessFs::ReadFile | AccessFs::ReadDir)?;
        }
        for &rf in read_files {
            ruleset = allow_path(ruleset, rf, BitFlags::from(AccessFs::ReadFile))?;
        }
        ruleset = allow_path(ruleset, work_dir, COMPILE_OWN)?;
        ruleset = allow_path(
            ruleset,
            Path::new("/dev/null"),
            AccessFs::ReadFile | AccessFs::WriteFile,
        )?;

        finish_landlock(ruleset)?;
        apply_compile_seccomp()
    }

    fn extract_local_bin_dir(executable: &Path) -> Option<std::path::PathBuf> {
        let s = executable.to_str()?;
        let marker = "/.local/bin/";
        let idx = s.find(marker)?;
        let tail = &s[idx + marker.len()..];
        if tail.contains('/') {
            return None;
        }
        Some(std::path::PathBuf::from(&s[..idx + marker.len() - 1]))
    }

    pub(super) fn confine_command_child_linux(
        executable: &Path,
        work_dir: &Path,
        tmp_dir: &Path,
        parent_pid: u32,
        allow_loopback: bool,
        loopback_ports_filter: Option<&[u16]>,
    ) -> Result<(), String> {
        rustix::thread::set_no_new_privs(true).map_err(|e| format!("no_new_privs: {e}"))?;

        let mut loopback_ports = if allow_loopback {
            collect_loopback_listen_ports()
        } else {
            Vec::new()
        };
        if let Some(allowed) = loopback_ports_filter {
            loopback_ports.retain(|p| allowed.contains(p));
        }

        let mut ruleset = create_ruleset_v3_to_v6(allow_loopback)?;
        if allow_loopback && loopback_ports_filter.is_none() {
            ruleset = ruleset
                .add_rule(NetPort::new(0, AccessNet::BindTcp))
                .map_err(|e| format!("landlock net_port(0, bind): {e}"))?;
        }
        for port in loopback_ports {
            ruleset = ruleset
                .add_rule(NetPort::new(port, AccessNet::ConnectTcp))
                .map_err(|e| format!("landlock net_port({port}): {e}"))?;
        }
        for sys in ["/usr", "/bin", "/lib", "/lib64", "/opt/homebrew"] {
            ruleset = allow_path(ruleset, Path::new(sys), SYSTEM_READ)?;
        }
        if let Some(local_bin) = extract_local_bin_dir(executable) {
            ruleset = allow_path(ruleset, &local_bin, SYSTEM_READ)?;
        }
        ruleset = allow_path(
            ruleset,
            Path::new("/etc/ld.so.cache"),
            BitFlags::from(AccessFs::ReadFile),
        )?;
        ruleset = allow_path(ruleset, executable, AccessFs::Execute | AccessFs::ReadFile)?;
        ruleset = allow_path(ruleset, work_dir, COMMAND_OWN)?;
        ruleset = allow_path(ruleset, tmp_dir, COMMAND_OWN)?;
        ruleset = allow_path(
            ruleset,
            Path::new("/dev/null"),
            AccessFs::ReadFile | AccessFs::WriteFile,
        )?;
        ruleset = allow_path(
            ruleset,
            Path::new("/dev/urandom"),
            BitFlags::from(AccessFs::ReadFile),
        )?;

        finish_landlock(ruleset)?;
        apply_command_seccomp(parent_pid, allow_loopback)
    }
}
