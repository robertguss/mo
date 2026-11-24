//! `mo install` implementation (`src/tools/install.c` parity).

use std::env;
use std::fs;
use std::io::Write;

use mo_base::{DiagBag, Diagnostic, SourceMap, Span};
use mo_def::ErrorCode;
use mo_def::limits::{
    MO_LIMIT_HOST_PATH_SIZE, MO_LIMIT_INSTALL_NAME, MO_LIMIT_INSTALL_SCRIPT, MO_LIMIT_PATH_SIZE,
};

use crate::cli::{Cli, valid_install_name};
use crate::fs_util::{
    bytes_to_path, io_error_message, names_a_source, os_str_to_bytes, path_to_bytes,
    write_atomic_file,
};
use crate::identity::executable_path;

fn install_error(diags: &mut DiagBag, message: &str, detail: Option<&[u8]>) {
    let mut d = Diagnostic::new(ErrorCode::ToolPipeline, Span::NONE, message);
    if let Some(lbl) = detail {
        d = d.with_primary_text(lbl);
    }
    let _ = diags.push(d);
}

/// Derives a valid tool name from `source_path` (`src/tools/install.c:derive_tool_name`).
///
/// For `main.mo`, uses the canonical parent directory name; otherwise strips `.mo`.
#[must_use]
pub fn derive_tool_name(source_path: &[u8]) -> Option<String> {
    let slash = source_path.iter().rposition(|&b| b == b'/');
    let base = match slash {
        Some(pos) => &source_path[pos + 1..],
        None => source_path,
    };
    if base == b"main.mo" {
        let parent_bytes: Vec<u8> = match slash {
            None => b".".to_vec(),
            Some(0) => b"/".to_vec(),
            Some(pos) => {
                if (pos as u64) >= MO_LIMIT_PATH_SIZE {
                    return None;
                }
                source_path[..pos].to_vec()
            }
        };
        let resolved_bytes = fs::canonicalize(bytes_to_path(&parent_bytes))
            .map(|p| path_to_bytes(&p))
            .unwrap_or(parent_bytes);
        let dir_name = match resolved_bytes.iter().rposition(|&b| b == b'/') {
            Some(pos) => &resolved_bytes[pos + 1..],
            None => resolved_bytes.as_slice(),
        };
        if !valid_install_name(dir_name) {
            return None;
        }
        return std::str::from_utf8(dir_name).ok().map(str::to_string);
    }
    let stem = base.strip_suffix(b".mo").unwrap_or(base);
    if stem.is_empty() || (stem.len() as u64) > MO_LIMIT_INSTALL_NAME || !valid_install_name(stem) {
        return None;
    }
    std::str::from_utf8(stem).ok().map(str::to_string)
}

fn mkdir_parents(dir: &[u8]) -> Result<(), String> {
    let mut len = dir.len();
    if len == 0 || (len as u64) >= MO_LIMIT_HOST_PATH_SIZE {
        return Err("File name too long".to_string());
    }
    while len > 1 && dir[len - 1] == b'/' {
        len -= 1;
    }
    let trimmed = &dir[..len];
    let start = usize::from(trimmed[0] == b'/');
    for i in start..=trimmed.len() {
        if i < trimmed.len() && trimmed[i] != b'/' {
            continue;
        }
        let prefix = &trimmed[..i];
        let p = bytes_to_path(prefix);
        match fs::metadata(p.as_ref()) {
            Ok(meta) => {
                if !meta.is_dir() {
                    return Err("Not a directory".to_string());
                }
            }
            Err(_) => {
                if let Err(err) = fs::create_dir(p.as_ref())
                    && err.kind() != std::io::ErrorKind::AlreadyExists
                {
                    return Err(io_error_message(&err));
                }
            }
        }
    }
    Ok(())
}

fn resolve_existing_dir(path: &[u8]) -> Result<Vec<u8>, String> {
    let resolved = fs::canonicalize(bytes_to_path(path)).map_err(|e| io_error_message(&e))?;
    let meta = fs::metadata(&resolved).map_err(|e| io_error_message(&e))?;
    if !meta.is_dir() {
        return Err("Not a directory".to_string());
    }
    let bytes = path_to_bytes(&resolved);
    if bytes.is_empty() || (bytes.len() as u64) >= MO_LIMIT_HOST_PATH_SIZE {
        return Err("File name too long".to_string());
    }
    Ok(bytes)
}

fn append_bytes(buf: &mut Vec<u8>, limit: usize, data: &[u8]) -> bool {
    if data.len() > limit.saturating_sub(buf.len()) {
        return false;
    }
    buf.extend_from_slice(data);
    true
}

fn append_shell_quoted(buf: &mut Vec<u8>, limit: usize, s: &[u8]) -> bool {
    if !append_bytes(buf, limit, b"'") {
        return false;
    }
    for &b in s {
        if b == b'\'' {
            if !append_bytes(buf, limit, b"'\\''") {
                return false;
            }
        } else if !append_bytes(buf, limit, &[b]) {
            return false;
        }
    }
    append_bytes(buf, limit, b"'")
}

/// Builds the `/bin/sh` launcher script matching `src/tools/install.c:build_launcher_script`.
#[must_use]
pub fn build_launcher_script(
    tool_name: &str,
    fallback_mo: &[u8],
    baked_root: Option<&[u8]>,
    baked_state: Option<&[u8]>,
) -> Option<Vec<u8>> {
    let limit = MO_LIMIT_INSTALL_SCRIPT as usize;
    let compiler_mo = if fallback_mo.is_empty() {
        b"mo".as_slice()
    } else {
        fallback_mo
    };
    let mut script = Vec::with_capacity(512);
    let ok = append_bytes(
        &mut script,
        limit,
        b"#!/bin/sh\n\
          dir=$(CDPATH= cd -- \"$(dirname -- \"$0\")\" && pwd)\n\
          if [ -x \"$dir/mo\" ]; then\n\
            mo_bin=\"$dir/mo\"\n\
          elif [ -x ",
    ) && append_shell_quoted(&mut script, limit, compiler_mo)
        && append_bytes(&mut script, limit, b" ]; then\n  mo_bin=")
        && append_shell_quoted(&mut script, limit, compiler_mo)
        && append_bytes(&mut script, limit, b"\nelse\n  mo_bin=mo\nfi\nroot_arg=")
        && append_shell_quoted(&mut script, limit, baked_root.unwrap_or(b""))
        && append_bytes(&mut script, limit, b"\nstate_arg=")
        && append_shell_quoted(&mut script, limit, baked_state.unwrap_or(b""))
        && append_bytes(
            &mut script,
            limit,
            b"\nwhile [ $# -gt 0 ]; do\n\
              case \"$1\" in\n\
                --mo-root)\n\
                  [ $# -ge 2 ] || exec \"$mo_bin\" run --root\n\
                  root_arg=$2\n\
                  shift 2\n\
                  ;;\n\
                --mo-state-dir)\n\
                  [ $# -ge 2 ] || exec \"$mo_bin\" run --state-dir\n\
                  state_arg=$2\n\
                  shift 2\n\
                  ;;\n\
                *)\n\
                  break\n\
                  ;;\n\
              esac\n\
            done\n\
            set -- \"$dir/.mo-libexec/",
        )
        && append_bytes(&mut script, limit, tool_name.as_bytes())
        && append_bytes(
            &mut script,
            limit,
            b".bc\" \"$@\"\n\
              if [ -n \"$state_arg\" ]; then\n\
                set -- --state-dir \"$state_arg\" \"$@\"\n\
              fi\n\
              if [ -n \"$root_arg\" ]; then\n\
                set -- --root \"$root_arg\" \"$@\"\n\
              fi\n\
              exec \"$mo_bin\" run \"$@\"\n",
        );
    if ok { Some(script) } else { None }
}

/// Installs compiled bytecode `.mo-libexec/<name>.bc` and its `/bin/sh` launcher
/// (`src/tools/install.c:mo_install_program`).
pub fn install_program(
    cli: &Cli,
    sources: &SourceMap,
    wire: &[u8],
    output: &mut dyn Write,
    diags: &mut DiagBag,
) {
    let tool_name = match &cli.install_name {
        Some(raw) => match std::str::from_utf8(raw) {
            Ok(s) => s.to_string(),
            Err(_) => {
                install_error(
                    diags,
                    "could not derive a valid tool name; pass --name NAME",
                    Some(raw),
                );
                return;
            }
        },
        None => {
            let source_path = cli.file(0);
            match derive_tool_name(source_path) {
                Some(name) => name,
                None => {
                    install_error(
                        diags,
                        "could not derive a valid tool name; pass --name NAME",
                        Some(source_path),
                    );
                    return;
                }
            }
        }
    };

    let baked_root = match &cli.root {
        Some(r) => match resolve_existing_dir(r) {
            Ok(resolved) => Some(resolved),
            Err(err) => {
                install_error(diags, "could not open --root folder", Some(err.as_bytes()));
                return;
            }
        },
        None => None,
    };
    let baked_state = match &cli.state_dir {
        Some(s) => match resolve_existing_dir(s) {
            Ok(resolved) => Some(resolved),
            Err(err) => {
                install_error(
                    diags,
                    "could not open --state-dir folder",
                    Some(err.as_bytes()),
                );
                return;
            }
        },
        None => None,
    };

    let target_bin: Vec<u8> = match &cli.bin_dir {
        Some(dir) => {
            if dir.is_empty() || (dir.len() as u64) >= MO_LIMIT_HOST_PATH_SIZE {
                install_error(diags, "install directory path is too long", Some(dir));
                return;
            }
            dir.clone()
        }
        None => {
            let Some(home_os) = env::var_os("HOME").filter(|h| !h.is_empty()) else {
                install_error(diags, "HOME is not set; pass --bin-dir DIR", None);
                return;
            };
            let home = os_str_to_bytes(&home_os);
            let mut buf = Vec::with_capacity(home.len() + 11);
            buf.extend_from_slice(&home);
            buf.extend_from_slice(b"/.local/bin");
            if (buf.len() as u64) >= MO_LIMIT_HOST_PATH_SIZE {
                install_error(diags, "install directory path is too long", Some(&home));
                return;
            }
            buf
        }
    };

    if let Err(err) = mkdir_parents(&target_bin) {
        install_error(
            diags,
            "could not create install directory",
            Some(err.as_bytes()),
        );
        return;
    }
    let real_bin = match resolve_existing_dir(&target_bin) {
        Ok(r) => r,
        Err(err) => {
            install_error(
                diags,
                "could not resolve install directory",
                Some(err.as_bytes()),
            );
            return;
        }
    };

    let mut libexec_dir = Vec::with_capacity(real_bin.len() + 13);
    libexec_dir.extend_from_slice(&real_bin);
    libexec_dir.extend_from_slice(b"/.mo-libexec");
    if (libexec_dir.len() as u64) >= MO_LIMIT_HOST_PATH_SIZE {
        install_error(diags, "install directory path is too long", Some(&real_bin));
        return;
    }
    if let Err(err) = mkdir_parents(&libexec_dir) {
        install_error(
            diags,
            "could not create .mo-libexec directory",
            Some(err.as_bytes()),
        );
        return;
    }

    let mut bc_path = Vec::with_capacity(libexec_dir.len() + 1 + tool_name.len() + 3);
    bc_path.extend_from_slice(&libexec_dir);
    bc_path.push(b'/');
    bc_path.extend_from_slice(tool_name.as_bytes());
    bc_path.extend_from_slice(b".bc");

    let mut launcher_path = Vec::with_capacity(real_bin.len() + 1 + tool_name.len());
    launcher_path.extend_from_slice(&real_bin);
    launcher_path.push(b'/');
    launcher_path.extend_from_slice(tool_name.as_bytes());

    if (bc_path.len() as u64) >= MO_LIMIT_HOST_PATH_SIZE
        || (launcher_path.len() as u64) >= MO_LIMIT_HOST_PATH_SIZE
    {
        install_error(
            diags,
            "installed tool path is too long",
            Some(tool_name.as_bytes()),
        );
        return;
    }

    if names_a_source(&bc_path, sources) || names_a_source(&launcher_path, sources) {
        install_error(
            diags,
            "installed tool can't replace a source file",
            Some(&launcher_path),
        );
        return;
    }

    let self_mo = executable_path().unwrap_or_else(|| b"mo".to_vec());
    let Some(script) = build_launcher_script(
        &tool_name,
        &self_mo,
        baked_root.as_deref(),
        baked_state.as_deref(),
    ) else {
        install_error(
            diags,
            "launcher script exceeds output budget",
            Some(tool_name.as_bytes()),
        );
        return;
    };

    if let Err(err) = write_atomic_file(&libexec_dir, &tool_name, &bc_path, wire, 0o644) {
        install_error(
            diags,
            "bytecode could not be installed",
            Some(err.as_bytes()),
        );
        return;
    }
    if let Err(err) = write_atomic_file(&real_bin, &tool_name, &launcher_path, &script, 0o755) {
        install_error(
            diags,
            "launcher could not be installed",
            Some(err.as_bytes()),
        );
        return;
    }

    let mut line = Vec::with_capacity(10 + launcher_path.len() + 1);
    line.extend_from_slice(b"installed ");
    line.extend_from_slice(&launcher_path);
    line.push(b'\n');
    if let Err(err) = output.write_all(&line).and_then(|()| output.flush()) {
        install_error(
            diags,
            "install output could not be written",
            Some(io_error_message(&err).as_bytes()),
        );
    }
}
