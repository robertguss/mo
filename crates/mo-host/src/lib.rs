#![forbid(unsafe_code)]

//! Capability abstraction and real/fake implementations (`Args`, `Env`, `Console`,
//! `Files`, `ReadFiles`, `Files.File`, `Clock`, `Random`, `SecureRandom`,
//! `Network`, `Connection`, `Sqlite`, `Sqlite.Database`, `Command`).

mod clock_random;
mod command;
mod fake_files;
mod files_common;
mod network;
#[cfg(unix)]
mod real_files;
#[cfg(test)]
mod slice1d_tests;
mod sqlite;

use std::cell::Cell;
use std::cell::RefCell;
use std::collections::VecDeque;
use std::io::{self, BufRead, BufReader, Read, Write};
#[cfg(unix)]
use std::os::fd::{AsFd, OwnedFd};
use std::path::Path;
use std::rc::Rc;

use mo_bytecode::types::TypeTable;
use mo_bytecode::{Capability, TypeKind};
use mo_def::HostOpId;
use mo_def::limits::MO_LIMIT_VALUE_BYTES;
use mo_heap::table::MoMap;
use mo_heap::{Heap, HeapError, Value};

use fake_files::{FakeFolderCap, FakeOpenFileCap};
#[cfg(unix)]
use real_files::{RealFolder, RealOpenFile, ResolverState};

#[expect(
    clippy::expect_used,
    reason = "verified bytecode guarantees capability value matches host operation"
)]
pub(crate) fn downcast_cap<'a, T: 'static>(v: &'a Value, expected: &'static str) -> &'a T {
    v.as_capability().inner.downcast_ref::<T>().expect(expected)
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum HostError {
    Bug {
        reason: &'static str,
        detail: Option<String>,
    },
    Internal {
        reason: &'static str,
    },
    OutOfMemory {
        value_bytes_limit: bool,
    },
    UnimplementedCapability(Capability),
}

impl From<HeapError> for HostError {
    fn from(_: HeapError) -> Self {
        Self::OutOfMemory {
            value_bytes_limit: false,
        }
    }
}

struct ArgsState {
    list: Value,
}

struct EnvState {
    map: Value,
}

struct FakeConsoleState {
    input: Vec<String>,
    cursor: usize,
    printed: Vec<String>,
    printed_errors: Vec<String>,
}

enum ConsoleState {
    Real,
    Fake(RefCell<FakeConsoleState>),
}

pub(crate) enum FolderCap {
    #[cfg(unix)]
    Real(Rc<RealFolder>),
    Fake(FakeFolderCap),
}

pub(crate) enum OpenFileCap {
    #[cfg(unix)]
    Real(RealOpenFile),
    Fake(FakeOpenFileCap),
}

/// Real I/O handles and OS context for a process.
pub struct HostContext {
    pub args: Vec<String>,
    pub env: Vec<(String, String)>,
    input: Box<dyn BufRead>,
    output: Box<dyn Write>,
    errors: Box<dyn Write>,
    #[cfg(unix)]
    root_fd: Option<OwnedFd>,
    #[cfg(unix)]
    state_fd: Option<OwnedFd>,
    #[cfg(unix)]
    resolver: ResolverState,
    open_folders: Rc<Cell<u32>>,
    open_files: Rc<Cell<u32>>,
    open_connections: Rc<Cell<u32>>,
    open_databases: Rc<Cell<u32>>,
    started: bool,
}

impl HostContext {
    pub fn stdio(args: Vec<String>, env: Vec<(String, String)>) -> Self {
        Self {
            args,
            env,
            input: Box::new(BufReader::new(io::stdin())),
            output: Box::new(io::stdout()),
            errors: Box::new(io::stderr()),
            #[cfg(unix)]
            root_fd: None,
            #[cfg(unix)]
            state_fd: None,
            #[cfg(unix)]
            resolver: ResolverState::new(),
            open_folders: Rc::new(Cell::new(0)),
            open_files: Rc::new(Cell::new(0)),
            open_connections: Rc::new(Cell::new(0)),
            open_databases: Rc::new(Cell::new(0)),
            started: false,
        }
    }

    /// Creates a stdio host context rooted at `root_dir` for real `Files` / `ReadFiles` /
    /// `Command` / `Sqlite`.
    #[cfg(unix)]
    pub fn stdio_with_root(
        args: Vec<String>,
        env: Vec<(String, String)>,
        root_dir: Option<&Path>,
    ) -> Result<Self, String> {
        Self::stdio_with_roots(args, env, root_dir, None)
    }

    /// Creates a stdio host context with optional `--root` and `--state-dir` folders.
    #[cfg(unix)]
    pub fn stdio_with_roots(
        args: Vec<String>,
        env: Vec<(String, String)>,
        root_dir: Option<&Path>,
        state_dir: Option<&Path>,
    ) -> Result<Self, String> {
        let root_fd = match root_dir {
            Some(p) => Some(real_files::open_root_dir(p)?),
            None => None,
        };
        let state_fd = match state_dir {
            Some(p) => Some(real_files::open_root_dir(p)?),
            None => None,
        };
        Ok(Self {
            root_fd,
            state_fd,
            ..Self::stdio(args, env)
        })
    }

    pub fn with_io<R, W1, W2>(
        args: Vec<String>,
        env: Vec<(String, String)>,
        input: R,
        output: W1,
        errors: W2,
    ) -> Self
    where
        R: Read + 'static,
        W1: Write + 'static,
        W2: Write + 'static,
    {
        Self {
            args,
            env,
            input: Box::new(BufReader::new(input)),
            output: Box::new(output),
            errors: Box::new(errors),
            #[cfg(unix)]
            root_fd: None,
            #[cfg(unix)]
            state_fd: None,
            #[cfg(unix)]
            resolver: ResolverState::new(),
            open_folders: Rc::new(Cell::new(0)),
            open_files: Rc::new(Cell::new(0)),
            open_connections: Rc::new(Cell::new(0)),
            open_databases: Rc::new(Cell::new(0)),
            started: false,
        }
    }

    #[cfg(unix)]
    pub fn set_root_dir(&mut self, root_dir: &Path) -> Result<(), String> {
        self.root_fd = Some(real_files::open_root_dir(root_dir)?);
        Ok(())
    }

    #[cfg(unix)]
    pub fn set_state_dir(&mut self, state_dir: &Path) -> Result<(), String> {
        self.state_fd = Some(real_files::open_root_dir(state_dir)?);
        Ok(())
    }

    #[cfg(unix)]
    pub fn set_force_fallback(&mut self, force: bool) {
        self.resolver.force_fallback = force;
    }

    pub fn open_folders(&self) -> u32 {
        self.open_folders.get()
    }

    pub fn open_files(&self) -> u32 {
        self.open_files.get()
    }

    pub fn open_connections(&self) -> u32 {
        self.open_connections.get()
    }

    pub fn open_databases(&self) -> u32 {
        self.open_databases.get()
    }

    fn ensure_started(&mut self) {
        if !self.started {
            #[cfg(unix)]
            real_files::raise_nofile_limit_best_effort();
            self.started = true;
        }
    }

    #[cfg(unix)]
    fn ensure_root_fd(&mut self) -> Result<&OwnedFd, HostError> {
        if let Some(ref fd) = self.root_fd {
            return Ok(fd);
        }
        let fd = real_files::open_root_dir(Path::new(".")).map_err(|_| HostError::Internal {
            reason: "could not open the starting folder",
        })?;
        Ok(self.root_fd.insert(fd))
    }

    #[cfg(unix)]
    fn ensure_sqlite_fd(&mut self) -> Result<&OwnedFd, HostError> {
        if let Some(ref fd) = self.state_fd {
            return Ok(fd);
        }
        self.ensure_root_fd()
    }

    pub fn make_real_capability(
        &mut self,
        heap: &Heap,
        types: &TypeTable,
        ty: u32,
    ) -> Result<Value, HostError> {
        self.ensure_started();
        #[expect(
            clippy::expect_used,
            reason = "verified bytecode guarantees capability type specifies kind"
        )]
        let cap = types
            .get(ty)
            .capability
            .expect("capability type must specify capability kind");
        match cap {
            Capability::Args => {
                let mut items = VecDeque::with_capacity(self.args.len());
                for a in &self.args {
                    items.push_back(heap.alloc_text(a.clone())?);
                }
                let list = heap.alloc_list(items)?;
                Ok(heap.alloc_capability(cap, ty, Rc::new(ArgsState { list }))?)
            }
            Capability::Env => {
                let text_ty = find_text_type(types).unwrap_or(0);
                let mut map = MoMap::new(text_ty, text_ty);
                for (k, v) in &self.env {
                    let kv = heap.alloc_text(k.clone())?;
                    let vv = heap.alloc_text(v.clone())?;
                    map.set(types, heap.hash_key(), kv, vv);
                }
                let map_val = heap.alloc_map(map)?;
                Ok(heap.alloc_capability(cap, ty, Rc::new(EnvState { map: map_val }))?)
            }
            Capability::Console => {
                Ok(heap.alloc_capability(cap, ty, Rc::new(ConsoleState::Real))?)
            }
            #[cfg(unix)]
            Capability::Files | Capability::ReadFiles => {
                let open_folders = Rc::clone(&self.open_folders);
                let root_fd = self.ensure_root_fd()?;
                real_files::make_real_folder_cap(heap, &open_folders, root_fd.as_fd(), cap, ty)
            }
            Capability::Clock => clock_random::make_real_clock(heap, ty),
            Capability::Random => clock_random::make_real_random(heap, ty),
            Capability::SecureRandom => clock_random::make_real_secure_random(heap, ty),
            Capability::Network => network::make_real_network(heap, ty),
            #[cfg(unix)]
            Capability::Sqlite => {
                let open_folders = Rc::clone(&self.open_folders);
                let sqlite_fd = self.ensure_sqlite_fd()?;
                sqlite::make_real_sqlite(heap, &open_folders, sqlite_fd.as_fd(), ty)
            }
            #[cfg(unix)]
            Capability::Command => {
                let open_folders = Rc::clone(&self.open_folders);
                let root_fd = self.ensure_root_fd()?;
                command::make_real_command(heap, &open_folders, root_fd.as_fd(), ty)
            }
            other => Err(HostError::UnimplementedCapability(other)),
        }
    }

    pub fn call(
        &mut self,
        heap: &Heap,
        types: &TypeTable,
        op: HostOpId,
        result_ty: u32,
        args: &[Value],
    ) -> Result<Value, HostError> {
        match op {
            HostOpId::ArgsList => {
                let state = downcast_cap::<ArgsState>(&args[0], "Args capability");
                Ok(state.list.clone())
            }
            HostOpId::ArgsOf => {
                let state = ArgsState {
                    list: args[0].clone(),
                };
                Ok(heap.alloc_capability(Capability::Args, result_ty, Rc::new(state))?)
            }
            HostOpId::EnvGet => {
                let state = downcast_cap::<EnvState>(&args[0], "Env capability");
                let found = state
                    .map
                    .as_map()
                    .get(types, heap.hash_key(), &args[1])
                    .cloned();
                Ok(heap.make_option(types, result_ty, found)?)
            }
            HostOpId::EnvOf => {
                let state = EnvState {
                    map: args[0].clone(),
                };
                Ok(heap.alloc_capability(Capability::Env, result_ty, Rc::new(state))?)
            }
            HostOpId::ConsolePrint | HostOpId::ConsoleError => {
                let is_err = op == HostOpId::ConsoleError;
                let state = downcast_cap::<ConsoleState>(&args[0], "Console capability");
                let text = args[1].as_text();
                match state {
                    ConsoleState::Fake(fake) => {
                        let mut f = fake.borrow_mut();
                        if is_err {
                            f.printed_errors.push(text.to_string());
                        } else {
                            f.printed.push(text.to_string());
                        }
                    }
                    ConsoleState::Real => {
                        let writer: &mut dyn Write = if is_err {
                            self.errors.as_mut()
                        } else {
                            self.output.as_mut()
                        };
                        let write_ok = write_line_atomic(writer, text.as_bytes());
                        if write_ok.is_err() {
                            return Err(HostError::Bug {
                                reason: "printing to an output that is gone",
                                detail: None,
                            });
                        }
                    }
                }
                Ok(Value::Unit)
            }
            HostOpId::ConsoleReadLine => {
                let state = downcast_cap::<ConsoleState>(&args[0], "Console capability");
                match state {
                    ConsoleState::Fake(fake) => {
                        let mut f = fake.borrow_mut();
                        if f.cursor < f.input.len() {
                            let line = f.input[f.cursor].clone();
                            f.cursor += 1;
                            let v = heap.alloc_text(line)?;
                            Ok(heap.make_option(types, result_ty, Some(v))?)
                        } else {
                            Ok(heap.make_option(types, result_ty, None)?)
                        }
                    }
                    ConsoleState::Real => self.read_real_line(heap, types, result_ty),
                }
            }
            HostOpId::ConsolePrinted | HostOpId::ConsolePrintedErrors => {
                let is_err = op == HostOpId::ConsolePrintedErrors;
                let state = downcast_cap::<ConsoleState>(&args[0], "Console capability");
                match state {
                    ConsoleState::Real => Err(HostError::Bug {
                        reason: "console observation needs a fake",
                        detail: None,
                    }),
                    ConsoleState::Fake(fake) => {
                        let f = fake.borrow();
                        let src = if is_err {
                            &f.printed_errors
                        } else {
                            &f.printed
                        };
                        let mut items = VecDeque::with_capacity(src.len());
                        for line in src {
                            items.push_back(heap.alloc_text(line.clone())?);
                        }
                        Ok(heap.alloc_list(items)?)
                    }
                }
            }
            HostOpId::ConsoleFake => {
                let state = ConsoleState::Fake(RefCell::new(FakeConsoleState {
                    input: Vec::new(),
                    cursor: 0,
                    printed: Vec::new(),
                    printed_errors: Vec::new(),
                }));
                Ok(heap.alloc_capability(Capability::Console, result_ty, Rc::new(state))?)
            }
            HostOpId::ConsoleFakeInput => {
                let input: Vec<String> = args[0]
                    .as_list()
                    .iter()
                    .map(|v| v.as_text().to_string())
                    .collect();
                let state = ConsoleState::Fake(RefCell::new(FakeConsoleState {
                    input,
                    cursor: 0,
                    printed: Vec::new(),
                    printed_errors: Vec::new(),
                }));
                Ok(heap.alloc_capability(Capability::Console, result_ty, Rc::new(state))?)
            }

            // Clock (3 ops)
            HostOpId::ClockFixed => {
                clock_random::make_fixed_clock(heap, result_ty, args[0].as_time())
            }
            HostOpId::ClockNow => clock_random::clock_now(&args[0]),
            HostOpId::ClockSleep => clock_random::clock_sleep(&args[0], args[1].as_duration()),

            // Random & SecureRandom (4 ops)
            HostOpId::RandomSeeded => {
                clock_random::make_seeded_random(heap, result_ty, args[0].as_int())
            }
            HostOpId::RandomInt => {
                clock_random::random_int(&args[0], args[1].as_int(), args[2].as_int())
            }
            HostOpId::SecurerandomSeeded => {
                clock_random::make_seeded_secure_random(heap, result_ty, args[0].as_int())
            }
            HostOpId::SecurerandomBytes => {
                clock_random::secure_random_bytes(heap, &args[0], args[1].as_int())
            }

            // Files, ReadFiles, Files.File (26 ops)
            HostOpId::FilesInMemory => {
                fake_files::files_in_memory(heap, types, result_ty, &args[0])
            }
            HostOpId::FilesReadText
            | HostOpId::FilesReadBytes
            | HostOpId::FilesWriteText
            | HostOpId::FilesAppendText
            | HostOpId::FilesRemove
            | HostOpId::FilesMakeFolder
            | HostOpId::FilesList
            | HostOpId::FilesEntries
            | HostOpId::FilesExists
            | HostOpId::FilesWithin
            | HostOpId::FilesReadOnly
            | HostOpId::FilesWriteBytes
            | HostOpId::FilesRename
            | HostOpId::FilesOpen
            | HostOpId::ReadfilesReadText
            | HostOpId::ReadfilesReadBytes
            | HostOpId::ReadfilesList
            | HostOpId::ReadfilesEntries
            | HostOpId::ReadfilesExists
            | HostOpId::ReadfilesWithin => {
                let folder = downcast_cap::<FolderCap>(&args[0], "Files/ReadFiles capability");
                match folder {
                    FolderCap::Fake(fake) => fake_files::call_fake_folder(
                        heap,
                        types,
                        &self.open_files,
                        fake,
                        op,
                        result_ty,
                        args,
                    ),
                    #[cfg(unix)]
                    FolderCap::Real(real) => real_files::call_real_folder(
                        heap,
                        types,
                        &mut self.resolver,
                        &self.open_folders,
                        &self.open_files,
                        real,
                        op,
                        result_ty,
                        args,
                    ),
                }
            }
            HostOpId::FilesReadAt
            | HostOpId::FilesWriteAt
            | HostOpId::FilesSize
            | HostOpId::FilesTruncate
            | HostOpId::FilesSync => {
                let fh = downcast_cap::<OpenFileCap>(&args[0], "Files.File capability");
                match fh {
                    OpenFileCap::Fake(fake) => {
                        fake_files::call_fake_file(heap, types, fake, op, result_ty, args)
                    }
                    #[cfg(unix)]
                    OpenFileCap::Real(real) => {
                        real_files::call_real_file(heap, types, real, op, result_ty, args)
                    }
                }
            }

            // Network & Connection (6 ops)
            HostOpId::NetworkFake => network::network_fake(heap, result_ty, args[0].clone()),
            HostOpId::NetworkConnect
            | HostOpId::NetworkConnectSecure
            | HostOpId::NetworkAllowHosts
            | HostOpId::ConnectionSend
            | HostOpId::ConnectionReceive => {
                network::call_network(heap, types, &self.open_connections, op, result_ty, args)
            }

            // Sqlite & Sqlite.Database (5 ops)
            HostOpId::SqliteFake => sqlite::sqlite_fake(heap, result_ty, args.first()),
            HostOpId::SqliteOpen
            | HostOpId::SqliteRun
            | HostOpId::SqliteQuery
            | HostOpId::SqliteExecuteRaw => sqlite::call_sqlite(
                heap,
                types,
                #[cfg(unix)]
                &mut self.resolver,
                &self.open_databases,
                op,
                result_ty,
                args,
            ),

            // Command (4 ops)
            HostOpId::CommandFake => command::command_fake(heap, result_ty, args[0].clone()),
            HostOpId::CommandRunRaw | HostOpId::CommandWithin | HostOpId::CommandAllowPrograms => {
                command::call_command(
                    heap,
                    types,
                    #[cfg(unix)]
                    &mut self.resolver,
                    &self.open_folders,
                    op,
                    result_ty,
                    args,
                )
            }
        }
    }

    fn read_real_line(
        &mut self,
        heap: &Heap,
        types: &TypeTable,
        result_ty: u32,
    ) -> Result<Value, HostError> {
        let mut raw = Vec::new();
        let n = match self.input.read_until(b'\n', &mut raw) {
            Ok(n) => n,
            Err(e) if e.kind() == io::ErrorKind::Interrupted => loop {
                match self.input.read_until(b'\n', &mut raw) {
                    Ok(n) => break n,
                    Err(err) if err.kind() == io::ErrorKind::Interrupted => continue,
                    Err(_) => {
                        return Err(HostError::Internal {
                            reason: "internal error: console input unavailable",
                        });
                    }
                }
            },
            Err(_) => {
                return Err(HostError::Internal {
                    reason: "internal error: console input unavailable",
                });
            }
        };
        if n == 0 {
            return Ok(heap.make_option(types, result_ty, None)?);
        }
        let had_newline = raw.last() == Some(&b'\n');
        if had_newline {
            raw.pop();
            if raw.last() == Some(&b'\r') {
                raw.pop();
            }
        }
        let decoded = String::from_utf8_lossy(&raw).into_owned();
        if (decoded.len() as u64) > MO_LIMIT_VALUE_BYTES {
            return Err(HostError::OutOfMemory {
                value_bytes_limit: true,
            });
        }
        let text = heap.alloc_text(decoded)?;
        Ok(heap.make_option(types, result_ty, Some(text))?)
    }
}

fn write_line_atomic(writer: &mut dyn Write, bytes: &[u8]) -> io::Result<()> {
    let mut line = Vec::with_capacity(bytes.len() + 1);
    line.extend_from_slice(bytes);
    line.push(b'\n');
    writer.write_all(&line)?;
    writer.flush()
}

fn find_text_type(types: &TypeTable) -> Option<u32> {
    types
        .items
        .iter()
        .position(|t| t.kind == TypeKind::Text)
        .map(|i| i as u32)
}

#[cfg(test)]
mod tests {
    use super::*;
    use mo_bytecode::{LayoutDef, LayoutKind, NO_INDEX, TypeCase, TypeDef, TypeField};
    use mo_def::limits::{
        MO_LIMIT_FILE_IO_BYTES, MO_LIMIT_OPEN_FILES, MO_LIMIT_OPEN_FOLDERS, MO_LIMIT_PATH_LINKS,
        MO_LIMIT_SECURE_RANDOM_BYTES, MO_TIME_MAX_US, MO_TIME_MIN_US,
    };
    use mo_heap::print::value_print;

    fn base_type(kind: TypeKind) -> TypeDef {
        TypeDef {
            kind,
            parameters: Vec::new(),
            result: NO_INDEX,
            function: NO_INDEX,
            capability: None,
            name_idx: None,
            name: None,
            fields: Vec::new(),
            cases: Vec::new(),
            is_reference: false,
            has_fields: false,
        }
    }

    fn cap_type(cap: Capability) -> TypeDef {
        let mut t = base_type(TypeKind::Capability);
        t.capability = Some(cap);
        t
    }

    struct HostFixture {
        types: TypeTable,
        text: u32,
        list_text: u32,
        opt_text: u32,
        map_tt: u32,
        args_cap: u32,
        env_cap: u32,
        console_cap: u32,
        files_cap: u32,
        readfiles_cap: u32,
        file_cap: u32,
        clock_cap: u32,
        random_cap: u32,
        securerandom_cap: u32,
        sqlite_cap: u32,
        res_unit: u32,
        res_text: u32,
        res_bytes: u32,
        res_int: u32,
        res_list_text: u32,
        res_entries: u32,
        res_file: u32,
    }

    impl HostFixture {
        fn new() -> Self {
            let mut items = Vec::new();
            let mut layouts = Vec::new();
            let push = |items: &mut Vec<TypeDef>, t: TypeDef| -> u32 {
                let id = items.len() as u32;
                items.push(t);
                id
            };

            let unit = push(&mut items, base_type(TypeKind::Unit));
            let int = push(&mut items, base_type(TypeKind::Int));
            let text = push(&mut items, base_type(TypeKind::Text));
            let bytes = push(&mut items, base_type(TypeKind::Bytes));
            let mut list_text_def = base_type(TypeKind::List);
            list_text_def.parameters = vec![text];
            let list_text = push(&mut items, list_text_def);

            let mut opt_text_def = base_type(TypeKind::Choice);
            opt_text_def.name = Some("Option".into());
            opt_text_def.has_fields = true;
            opt_text_def.cases = vec![
                TypeCase {
                    name_idx: 0,
                    name: "None".into(),
                    fields: vec![],
                    unnamed: false,
                },
                TypeCase {
                    name_idx: 1,
                    name: "Some".into(),
                    fields: vec![TypeField {
                        name_idx: None,
                        name: None,
                        ty: text,
                    }],
                    unnamed: true,
                },
            ];
            let opt_text = push(&mut items, opt_text_def);

            let mut map_tt_def = base_type(TypeKind::Map);
            map_tt_def.parameters = vec![text, text];
            let map_tt = push(&mut items, map_tt_def);

            let args_cap = push(&mut items, cap_type(Capability::Args));
            let env_cap = push(&mut items, cap_type(Capability::Env));
            let console_cap = push(&mut items, cap_type(Capability::Console));
            let files_cap = push(&mut items, cap_type(Capability::Files));
            let readfiles_cap = push(&mut items, cap_type(Capability::ReadFiles));
            let file_cap = push(&mut items, cap_type(Capability::File));
            let clock_cap = push(&mut items, cap_type(Capability::Clock));
            let random_cap = push(&mut items, cap_type(Capability::Random));
            let securerandom_cap = push(&mut items, cap_type(Capability::SecureRandom));
            let sqlite_cap = push(&mut items, cap_type(Capability::Sqlite));

            let mut file_err_def = base_type(TypeKind::Choice);
            file_err_def.name = Some("FileError".into());
            file_err_def.has_fields = true;
            file_err_def.cases = vec![
                TypeCase {
                    name_idx: 0,
                    name: "NotFound".into(),
                    fields: vec![TypeField {
                        name_idx: None,
                        name: Some("path".into()),
                        ty: text,
                    }],
                    unnamed: false,
                },
                TypeCase {
                    name_idx: 1,
                    name: "NotAllowed".into(),
                    fields: vec![TypeField {
                        name_idx: None,
                        name: Some("path".into()),
                        ty: text,
                    }],
                    unnamed: false,
                },
                TypeCase {
                    name_idx: 2,
                    name: "NotText".into(),
                    fields: vec![TypeField {
                        name_idx: None,
                        name: Some("path".into()),
                        ty: text,
                    }],
                    unnamed: false,
                },
                TypeCase {
                    name_idx: 3,
                    name: "Failed".into(),
                    fields: vec![
                        TypeField {
                            name_idx: None,
                            name: Some("path".into()),
                            ty: text,
                        },
                        TypeField {
                            name_idx: None,
                            name: Some("reason".into()),
                            ty: text,
                        },
                    ],
                    unnamed: false,
                },
            ];
            let file_err = push(&mut items, file_err_def);
            layouts.push(LayoutDef {
                kind: LayoutKind::Case,
                ty: file_err,
                case_index: 3,
                function: NO_INDEX,
                fields: vec![text, text],
            });

            let mut entry_kind_def = base_type(TypeKind::Choice);
            entry_kind_def.name = Some("EntryKind".into());
            entry_kind_def.cases = ["File", "Folder", "Link", "Other"]
                .iter()
                .enumerate()
                .map(|(i, n)| TypeCase {
                    name_idx: i as u32,
                    name: (*n).into(),
                    fields: vec![],
                    unnamed: false,
                })
                .collect();
            let entry_kind = push(&mut items, entry_kind_def);

            let mut entry_def = base_type(TypeKind::Record);
            entry_def.name = Some("Entry".into());
            entry_def.fields = vec![
                TypeField {
                    name_idx: None,
                    name: Some("name".into()),
                    ty: text,
                },
                TypeField {
                    name_idx: None,
                    name: Some("kind".into()),
                    ty: entry_kind,
                },
            ];
            let entry = push(&mut items, entry_def);
            layouts.push(LayoutDef {
                kind: LayoutKind::Record,
                ty: entry,
                case_index: 0,
                function: NO_INDEX,
                fields: vec![text, entry_kind],
            });

            let mut list_entry_def = base_type(TypeKind::List);
            list_entry_def.parameters = vec![entry];
            let list_entry = push(&mut items, list_entry_def);

            let mut make_res = |ok_ty: u32| -> u32 {
                let mut r = base_type(TypeKind::Choice);
                r.name = Some("Result".into());
                r.has_fields = true;
                r.cases = vec![
                    TypeCase {
                        name_idx: 0,
                        name: "Ok".into(),
                        fields: vec![TypeField {
                            name_idx: None,
                            name: None,
                            ty: ok_ty,
                        }],
                        unnamed: true,
                    },
                    TypeCase {
                        name_idx: 1,
                        name: "Error".into(),
                        fields: vec![TypeField {
                            name_idx: None,
                            name: None,
                            ty: file_err,
                        }],
                        unnamed: true,
                    },
                ];
                let r_ty = push(&mut items, r);
                layouts.push(LayoutDef {
                    kind: LayoutKind::Case,
                    ty: r_ty,
                    case_index: 1,
                    function: NO_INDEX,
                    fields: vec![file_err],
                });
                r_ty
            };

            let res_unit = make_res(unit);
            let res_text = make_res(text);
            let res_bytes = make_res(bytes);
            let res_int = make_res(int);
            let res_list_text = make_res(list_text);
            let res_entries = make_res(list_entry);
            let res_file = make_res(file_cap);

            let types = TypeTable::finish(items, layouts).unwrap();
            Self {
                types,
                text,
                list_text,
                opt_text,
                map_tt,
                args_cap,
                env_cap,
                console_cap,
                files_cap,
                readfiles_cap,
                file_cap,
                clock_cap,
                random_cap,
                securerandom_cap,
                sqlite_cap,
                res_unit,
                res_text,
                res_bytes,
                res_int,
                res_list_text,
                res_entries,
                res_file,
            }
        }
    }

    #[test]
    fn real_and_fake_args_env_console() {
        let fx = HostFixture::new();
        let heap = Heap::new(1024 * 1024);
        let mut ctx = HostContext::with_io(
            vec!["alpha".to_string(), "beta".to_string()],
            vec![("KEY".to_string(), "VAL".to_string())],
            io::Cursor::new(b"first\r\nsecond".to_vec()),
            Vec::new(),
            Vec::new(),
        );

        let args_cap = ctx
            .make_real_capability(&heap, &fx.types, fx.args_cap)
            .unwrap();
        let all = ctx
            .call(
                &heap,
                &fx.types,
                HostOpId::ArgsList,
                fx.list_text,
                &[args_cap],
            )
            .unwrap();
        let elems: Vec<&str> = all.as_list().iter().map(Value::as_text).collect();
        assert_eq!(elems, vec!["alpha", "beta"]);

        let env_cap = ctx
            .make_real_capability(&heap, &fx.types, fx.env_cap)
            .unwrap();
        let k_hit = heap.alloc_text("KEY".into()).unwrap();
        let got = ctx
            .call(
                &heap,
                &fx.types,
                HostOpId::EnvGet,
                fx.opt_text,
                &[env_cap, k_hit],
            )
            .unwrap();
        assert_eq!(
            heap.extract_case_field(&fx.types, fx.opt_text, &got, 1, 0)
                .as_text(),
            "VAL"
        );

        let real_console = ctx
            .make_real_capability(&heap, &fx.types, fx.console_cap)
            .unwrap();
        let l1 = ctx
            .call(
                &heap,
                &fx.types,
                HostOpId::ConsoleReadLine,
                fx.opt_text,
                std::slice::from_ref(&real_console),
            )
            .unwrap();
        assert_eq!(
            heap.extract_case_field(&fx.types, fx.opt_text, &l1, 1, 0)
                .as_text(),
            "first"
        );
        let l2 = ctx
            .call(
                &heap,
                &fx.types,
                HostOpId::ConsoleReadLine,
                fx.opt_text,
                std::slice::from_ref(&real_console),
            )
            .unwrap();
        assert_eq!(
            heap.extract_case_field(&fx.types, fx.opt_text, &l2, 1, 0)
                .as_text(),
            "second"
        );
        let eof = ctx
            .call(
                &heap,
                &fx.types,
                HostOpId::ConsoleReadLine,
                fx.opt_text,
                &[real_console],
            )
            .unwrap();
        assert_eq!(eof.as_tag(), 0);

        // Verify real Sqlite capability can be constructed under default cwd root.
        let real_sqlite = ctx
            .make_real_capability(&heap, &fx.types, fx.sqlite_cap)
            .expect("real Sqlite capability");
        assert_eq!(real_sqlite.as_capability().kind, Capability::Sqlite);
    }

    #[test]
    fn clock_random_and_secure_random_parity() {
        let fx = HostFixture::new();
        let heap = Heap::new(4 * 1024 * 1024);
        let mut ctx = HostContext::stdio(Vec::new(), Vec::new());

        // Fake Clock.fixed + alias advance + bounds.
        let fixed = ctx
            .call(
                &heap,
                &fx.types,
                HostOpId::ClockFixed,
                fx.clock_cap,
                &[Value::Time(-1)],
            )
            .unwrap();
        let alias = fixed.clone();
        assert_eq!(
            ctx.call(
                &heap,
                &fx.types,
                HostOpId::ClockNow,
                0,
                std::slice::from_ref(&fixed),
            )
            .unwrap()
            .as_time(),
            -1
        );
        ctx.call(
            &heap,
            &fx.types,
            HostOpId::ClockSleep,
            0,
            &[fixed.clone(), Value::Duration(2_500_000)],
        )
        .unwrap();
        assert_eq!(
            ctx.call(&heap, &fx.types, HostOpId::ClockNow, 0, &[alias])
                .unwrap()
                .as_time(),
            2_499_999
        );
        assert_eq!(
            ctx.call(
                &heap,
                &fx.types,
                HostOpId::ClockSleep,
                0,
                &[fixed.clone(), Value::Duration(-1)],
            )
            .err(),
            Some(HostError::Bug {
                reason: "negative sleep duration",
                detail: None
            })
        );
        assert_eq!(
            ctx.call(
                &heap,
                &fx.types,
                HostOpId::ClockSleep,
                0,
                &[fixed.clone(), Value::Duration(MO_TIME_MAX_US)],
            )
            .err(),
            Some(HostError::Bug {
                reason: "time outside the years 1 to 9999",
                detail: None
            })
        );
        // Failed sleep must not mutate clock.
        assert_eq!(
            ctx.call(&heap, &fx.types, HostOpId::ClockNow, 0, &[fixed])
                .unwrap()
                .as_time(),
            2_499_999
        );

        // Real Clock.
        let real_clock = ctx
            .make_real_capability(&heap, &fx.types, fx.clock_cap)
            .unwrap();
        let now = ctx
            .call(
                &heap,
                &fx.types,
                HostOpId::ClockNow,
                0,
                std::slice::from_ref(&real_clock),
            )
            .unwrap()
            .as_time();
        assert!((MO_TIME_MIN_US..=MO_TIME_MAX_US).contains(&now));
        ctx.call(
            &heap,
            &fx.types,
            HostOpId::ClockSleep,
            0,
            &[real_clock.clone(), Value::Duration(0)],
        )
        .unwrap();
        assert_eq!(
            ctx.call(
                &heap,
                &fx.types,
                HostOpId::ClockSleep,
                0,
                &[
                    real_clock,
                    Value::Duration(MO_TIME_MAX_US - MO_TIME_MIN_US + 1)
                ],
            )
            .err(),
            Some(HostError::Bug {
                reason: "time outside the years 1 to 9999",
                detail: None
            })
        );

        // Random.seeded(0).int(0, 100) == 60 (pinned by C test_host.c:920).
        let rng = ctx
            .call(
                &heap,
                &fx.types,
                HostOpId::RandomSeeded,
                fx.random_cap,
                &[Value::Int(0)],
            )
            .unwrap();
        assert_eq!(
            ctx.call(
                &heap,
                &fx.types,
                HostOpId::RandomInt,
                1,
                &[rng.clone(), Value::Int(0), Value::Int(100)],
            )
            .unwrap()
            .as_int(),
            60
        );
        assert_eq!(
            ctx.call(
                &heap,
                &fx.types,
                HostOpId::RandomInt,
                1,
                &[rng, Value::Int(5), Value::Int(5)],
            )
            .err(),
            Some(HostError::Bug {
                reason: "high not greater than low",
                detail: None
            })
        );

        // Real Random full range.
        let real_rng = ctx
            .make_real_capability(&heap, &fx.types, fx.random_cap)
            .unwrap();
        for _ in 0..32 {
            let v = ctx
                .call(
                    &heap,
                    &fx.types,
                    HostOpId::RandomInt,
                    1,
                    &[real_rng.clone(), Value::Int(i64::MIN), Value::Int(i64::MAX)],
                )
                .unwrap()
                .as_int();
            assert!(v < i64::MAX);
        }

        // SecureRandom seeded determinism + bounds + real OS entropy.
        let sr1 = ctx
            .call(
                &heap,
                &fx.types,
                HostOpId::SecurerandomSeeded,
                fx.securerandom_cap,
                &[Value::Int(42)],
            )
            .unwrap();
        let sr2 = ctx
            .call(
                &heap,
                &fx.types,
                HostOpId::SecurerandomSeeded,
                fx.securerandom_cap,
                &[Value::Int(42)],
            )
            .unwrap();
        let b1 = ctx
            .call(
                &heap,
                &fx.types,
                HostOpId::SecurerandomBytes,
                3,
                &[sr1.clone(), Value::Int(48)],
            )
            .unwrap();
        let b2 = ctx
            .call(
                &heap,
                &fx.types,
                HostOpId::SecurerandomBytes,
                3,
                &[sr2, Value::Int(48)],
            )
            .unwrap();
        assert_eq!(b1.as_bytes(), b2.as_bytes());
        assert_eq!(b1.as_bytes().len(), 48);
        assert_eq!(
            ctx.call(
                &heap,
                &fx.types,
                HostOpId::SecurerandomBytes,
                3,
                &[sr1.clone(), Value::Int(-1)]
            )
            .err(),
            Some(HostError::Bug {
                reason: "count outside 0 to 65536",
                detail: None
            })
        );
        assert_eq!(
            ctx.call(
                &heap,
                &fx.types,
                HostOpId::SecurerandomBytes,
                3,
                &[sr1, Value::Int((MO_LIMIT_SECURE_RANDOM_BYTES + 1) as i64)],
            )
            .err(),
            Some(HostError::Bug {
                reason: "count outside 0 to 65536",
                detail: None
            })
        );

        let real_sr = ctx
            .make_real_capability(&heap, &fx.types, fx.securerandom_cap)
            .unwrap();
        let r1 = ctx
            .call(
                &heap,
                &fx.types,
                HostOpId::SecurerandomBytes,
                3,
                &[real_sr.clone(), Value::Int(32)],
            )
            .unwrap();
        let r2 = ctx
            .call(
                &heap,
                &fx.types,
                HostOpId::SecurerandomBytes,
                3,
                &[real_sr, Value::Int(32)],
            )
            .unwrap();
        assert_ne!(r1.as_bytes(), r2.as_bytes());
    }

    #[test]
    fn fake_files_and_file_handles_parity() {
        let fx = HostFixture::new();
        let heap = Heap::new(16 * 1024 * 1024);
        let mut ctx = HostContext::stdio(Vec::new(), Vec::new());

        // Invalid keys rejected with exact bug reason and detail.
        for bad in ["/a", "", ".", "..", "./a", "../a", "a/", "a//b"] {
            let mut m = MoMap::new(fx.text, fx.text);
            m.set(
                &fx.types,
                heap.hash_key(),
                heap.alloc_text(bad.into()).unwrap(),
                heap.alloc_text("x".into()).unwrap(),
            );
            let mv = heap.alloc_map(m).unwrap();
            assert_eq!(
                ctx.call(
                    &heap,
                    &fx.types,
                    HostOpId::FilesInMemory,
                    fx.files_cap,
                    &[mv]
                )
                .err(),
                Some(HostError::Bug {
                    reason: "fake has a key that is not a plain relative path",
                    detail: Some(bad.to_string())
                })
            );
        }

        // File-and-folder collision.
        let mut m = MoMap::new(fx.text, fx.text);
        m.set(
            &fx.types,
            heap.hash_key(),
            heap.alloc_text("a".into()).unwrap(),
            heap.alloc_text("x".into()).unwrap(),
        );
        m.set(
            &fx.types,
            heap.hash_key(),
            heap.alloc_text("a/b".into()).unwrap(),
            heap.alloc_text("y".into()).unwrap(),
        );
        let mv = heap.alloc_map(m).unwrap();
        assert_eq!(
            ctx.call(
                &heap,
                &fx.types,
                HostOpId::FilesInMemory,
                fx.files_cap,
                &[mv]
            )
            .err(),
            Some(HostError::Bug {
                reason: "fake has a file that is also a folder",
                detail: Some("a".to_string())
            })
        );

        // Full fake binary file lifecycle + open file cap (64).
        let empty_map = heap.alloc_map(MoMap::new(fx.text, fx.text)).unwrap();
        let files = ctx
            .call(
                &heap,
                &fx.types,
                HostOpId::FilesInMemory,
                fx.files_cap,
                &[empty_map],
            )
            .unwrap();
        let p = heap.alloc_text("db.bin".into()).unwrap();
        let res = ctx
            .call(
                &heap,
                &fx.types,
                HostOpId::FilesOpen,
                fx.res_file,
                &[files.clone(), p],
            )
            .unwrap();
        assert_eq!(res.as_tag(), 0);
        let fh = heap.extract_case_field(&fx.types, fx.res_file, &res, 0, 0);
        assert_eq!(ctx.open_files(), 1);

        let payload = heap.alloc_bytes(vec![0xaa, 0xbb]).unwrap();
        ctx.call(
            &heap,
            &fx.types,
            HostOpId::FilesWriteAt,
            fx.res_unit,
            &[fh.clone(), Value::Int(4), payload],
        )
        .unwrap();
        let rd = ctx
            .call(
                &heap,
                &fx.types,
                HostOpId::FilesReadAt,
                fx.res_bytes,
                &[fh.clone(), Value::Int(0), Value::Int(10)],
            )
            .unwrap();
        assert_eq!(
            heap.extract_case_field(&fx.types, fx.res_bytes, &rd, 0, 0)
                .as_bytes(),
            &[0, 0, 0, 0, 0xaa, 0xbb]
        );
        assert_eq!(
            ctx.call(
                &heap,
                &fx.types,
                HostOpId::FilesReadAt,
                fx.res_bytes,
                &[
                    fh.clone(),
                    Value::Int(0),
                    Value::Int((MO_LIMIT_FILE_IO_BYTES + 1) as i64)
                ],
            )
            .err(),
            Some(HostError::Bug {
                reason: "read count outside 0 to the read limit",
                detail: None
            })
        );
        drop(fh);
        drop(res);
        assert_eq!(ctx.open_files(), 0);

        // Enforce 64 open file handle limit on fake files too.
        let mut handles = Vec::new();
        for i in 0..MO_LIMIT_OPEN_FILES {
            let name = heap.alloc_text(format!("f{i}.bin")).unwrap();
            let r = ctx
                .call(
                    &heap,
                    &fx.types,
                    HostOpId::FilesOpen,
                    fx.res_file,
                    &[files.clone(), name],
                )
                .unwrap();
            handles.push(r);
        }
        let extra = heap.alloc_text("over.bin".into()).unwrap();
        assert_eq!(
            ctx.call(
                &heap,
                &fx.types,
                HostOpId::FilesOpen,
                fx.res_file,
                &[files.clone(), extra]
            )
            .err(),
            Some(HostError::Bug {
                reason: "too many files open at once",
                detail: None
            })
        );
        handles.clear();
        assert_eq!(ctx.open_files(), 0);
        let _ = fx.map_tt;
        let _ = fx.file_cap;
        let _ = fx.res_int;
        let _ = fx.res_entries;
        let _ = fx.res_list_text;
        let _ = fx.res_text;
        let _ = fx.readfiles_cap;
    }

    #[cfg(unix)]
    #[test]
    fn real_files_confinement_symlinks_and_limits() {
        use std::os::unix::fs::symlink;
        let fx = HostFixture::new();
        let heap = Heap::new(16 * 1024 * 1024);

        let base = std::env::temp_dir().join(format!(
            "mo-rust-host-test-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        let root = base.join("root");
        let outside = base.join("outside");
        std::fs::create_dir_all(&root).unwrap();
        std::fs::create_dir_all(&outside).unwrap();
        std::fs::write(outside.join("secret.txt"), "topsecret").unwrap();
        std::fs::write(root.join("ok.txt"), "hello").unwrap();
        symlink("../outside/secret.txt", root.join("escape_rel")).unwrap();
        symlink("/etc/hosts", root.join("escape_abs")).unwrap();
        symlink("ok.txt", root.join("link_inside")).unwrap();
        for i in 0..=MO_LIMIT_PATH_LINKS {
            let target = if i == MO_LIMIT_PATH_LINKS {
                "ok.txt".to_string()
            } else {
                format!("l{}", i + 1)
            };
            symlink(target, root.join(format!("l{i}"))).unwrap();
        }

        for force_fallback in [false, true] {
            let mut ctx =
                HostContext::stdio_with_root(Vec::new(), Vec::new(), Some(&root)).unwrap();
            ctx.set_force_fallback(force_fallback);
            let files = ctx
                .make_real_capability(&heap, &fx.types, fx.files_cap)
                .unwrap();
            assert_eq!(ctx.open_folders(), 1);

            let call_str = |ctx: &mut HostContext, op: HostOpId, ty: u32, path: &str| -> String {
                let p = heap.alloc_text(path.into()).unwrap();
                let res = ctx
                    .call(&heap, &fx.types, op, ty, &[files.clone(), p])
                    .unwrap();
                value_print(&fx.types, ty, &res)
            };

            assert_eq!(
                call_str(&mut ctx, HostOpId::FilesReadText, fx.res_text, "ok.txt"),
                "Ok(\"hello\")"
            );
            assert_eq!(
                call_str(
                    &mut ctx,
                    HostOpId::FilesReadText,
                    fx.res_text,
                    "link_inside"
                ),
                "Ok(\"hello\")"
            );
            assert_eq!(
                call_str(
                    &mut ctx,
                    HostOpId::FilesReadText,
                    fx.res_text,
                    "../outside/secret.txt"
                ),
                "Error(NotAllowed(path: \"../outside/secret.txt\"))"
            );
            assert_eq!(
                call_str(&mut ctx, HostOpId::FilesReadText, fx.res_text, "escape_rel"),
                "Error(NotAllowed(path: \"escape_rel\"))"
            );
            assert_eq!(
                call_str(&mut ctx, HostOpId::FilesReadText, fx.res_text, "escape_abs"),
                "Error(NotAllowed(path: \"escape_abs\"))"
            );
            assert_eq!(
                call_str(&mut ctx, HostOpId::FilesReadText, fx.res_text, "l1"),
                "Ok(\"hello\")"
            );
            assert_eq!(
                call_str(&mut ctx, HostOpId::FilesReadText, fx.res_text, "l0"),
                "Error(Failed(path: \"l0\", reason: \"too many links\"))"
            );
            // Strict no-symlink check on Files.open (even for internal symlinks).
            assert_eq!(
                call_str(&mut ctx, HostOpId::FilesOpen, fx.res_file, "link_inside"),
                "Error(NotAllowed(path: \"link_inside\"))"
            );
            assert_eq!(
                call_str(&mut ctx, HostOpId::FilesOpen, fx.res_file, "escape_rel"),
                "Error(NotAllowed(path: \"escape_rel\"))"
            );

            // Open folder limit (256).
            let mut ro_handles = Vec::new();
            for _ in 0..(MO_LIMIT_OPEN_FOLDERS - 1) {
                let h = ctx
                    .call(
                        &heap,
                        &fx.types,
                        HostOpId::FilesReadOnly,
                        fx.readfiles_cap,
                        std::slice::from_ref(&files),
                    )
                    .unwrap();
                ro_handles.push(h);
            }
            assert_eq!(ctx.open_folders(), MO_LIMIT_OPEN_FOLDERS as u32);
            assert_eq!(
                ctx.call(
                    &heap,
                    &fx.types,
                    HostOpId::FilesReadOnly,
                    fx.readfiles_cap,
                    std::slice::from_ref(&files),
                )
                .err(),
                Some(HostError::Bug {
                    reason: "too many folders open at once",
                    detail: None
                })
            );
            ro_handles.clear();
            assert_eq!(ctx.open_folders(), 1);

            // Binary file handle write_at / read_at / size / truncate / sync / rename.
            let p_db = heap.alloc_text("test.modb".into()).unwrap();
            let opened = ctx
                .call(
                    &heap,
                    &fx.types,
                    HostOpId::FilesOpen,
                    fx.res_file,
                    &[files.clone(), p_db],
                )
                .unwrap();
            let fh = heap.extract_case_field(&fx.types, fx.res_file, &opened, 0, 0);
            let data = heap.alloc_bytes(vec![1, 2, 3, 4]).unwrap();
            ctx.call(
                &heap,
                &fx.types,
                HostOpId::FilesWriteAt,
                fx.res_unit,
                &[fh.clone(), Value::Int(4), data],
            )
            .unwrap();
            ctx.call(
                &heap,
                &fx.types,
                HostOpId::FilesSync,
                fx.res_unit,
                std::slice::from_ref(&fh),
            )
            .unwrap();
            let sz = ctx
                .call(
                    &heap,
                    &fx.types,
                    HostOpId::FilesSize,
                    fx.res_int,
                    std::slice::from_ref(&fh),
                )
                .unwrap();
            assert_eq!(
                heap.extract_case_field(&fx.types, fx.res_int, &sz, 0, 0)
                    .as_int(),
                8
            );
            let rd = ctx
                .call(
                    &heap,
                    &fx.types,
                    HostOpId::FilesReadAt,
                    fx.res_bytes,
                    &[fh.clone(), Value::Int(0), Value::Int(8)],
                )
                .unwrap();
            assert_eq!(
                heap.extract_case_field(&fx.types, fx.res_bytes, &rd, 0, 0)
                    .as_bytes(),
                &[0, 0, 0, 0, 1, 2, 3, 4]
            );
            ctx.call(
                &heap,
                &fx.types,
                HostOpId::FilesTruncate,
                fx.res_unit,
                &[fh.clone(), Value::Int(6)],
            )
            .unwrap();
            drop(fh);
            drop(opened);
            assert_eq!(ctx.open_files(), 0);
            let p_old = heap.alloc_text("test.modb".into()).unwrap();
            let p_new = heap.alloc_text("renamed.modb".into()).unwrap();
            let ren = ctx
                .call(
                    &heap,
                    &fx.types,
                    HostOpId::FilesRename,
                    fx.res_unit,
                    &[files.clone(), p_old, p_new],
                )
                .unwrap();
            assert_eq!(value_print(&fx.types, fx.res_unit, &ren), "Ok(Unit)");
            let p_rm = heap.alloc_text("renamed.modb".into()).unwrap();
            ctx.call(
                &heap,
                &fx.types,
                HostOpId::FilesRemove,
                fx.res_unit,
                &[files.clone(), p_rm],
            )
            .unwrap();
            drop(files);
            assert_eq!(ctx.open_folders(), 0);
        }

        let _ = std::fs::remove_dir_all(&base);
    }
}
