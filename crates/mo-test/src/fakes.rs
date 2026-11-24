//! Fake capability generator for property-based promises (`src/tools/fakes.{h,c}`).

use std::cell::RefCell;
use std::collections::VecDeque;
use std::rc::Rc;

use mo_bytecode::types::TypeTable;
use mo_bytecode::{Capability, TypeKind};
use mo_def::limits::{MO_CLI_FAKE_RECIPES, MO_TIME_MAX_US, MO_TIME_MIN_US};
use mo_heap::table::MoMap;
use mo_heap::{Heap, Value};
use mo_host::{FakeCapInit, HostContext, HostError};

use crate::inputs::{
    InputFactory, InputKind, InputLeafRequest, InputLimits, InputResult, input_random,
};

const PATHS: &[&str] = &[
    "a.txt",
    "b.md",
    "notes/c.txt",
    "notes/d.md",
    "notes/deep/e.txt",
];
const FILE_TEXTS: &[&str] = &[
    "",
    "x",
    "one\ntwo\n",
    "x marks the spot\nbakery\n",
    "caf\u{e9} \u{20ac}\n",
    "no trailing newline",
    "\n\n",
    "a, b\r\nc\r\n",
];
const LINES: &[&str] = &["", "x", "hello world", "caf\u{e9}", "x y z", "  padded  "];
const ARGUMENTS: &[&str] = &[
    "",
    "a",
    "--help",
    "-x",
    "file.txt",
    "caf\u{e9}",
    "two words",
];
const ENV_NAMES: &[&str] = &["HOME", "PATH", "USER", "EMPTY", "LANG"];
const ENV_VALUES: &[&str] = &["/home/mo", "/usr/bin", "mo", "", "en_US.UTF-8"];

const MAX_ITEMS: usize = 8;
const BOUNDARIES: u32 = 3;

#[derive(Default)]
struct Contents {
    keys: Vec<&'static str>,
    values: Vec<&'static str>,
}

impl Contents {
    fn charge(&self, remaining: &mut InputLimits) -> bool {
        let mut bytes = 0usize;
        for k in &self.keys {
            bytes += k.len();
        }
        for v in &self.values {
            bytes += v.len();
        }
        let nodes = 1 + self.values.len() as u64;
        let work = nodes + bytes as u64;
        if nodes > u64::from(remaining.nodes) || bytes > remaining.bytes || work > remaining.work {
            return false;
        }
        remaining.nodes -= nodes as u32;
        remaining.bytes -= bytes;
        remaining.work -= work;
        true
    }
}

fn pick(request: &InputLeafRequest, random: &mut u64, n: usize, k: usize, out: &mut [usize]) {
    let mut all = [0usize; MAX_ITEMS];
    debug_assert!(n <= MAX_ITEMS && k <= n);
    for (i, slot) in all.iter_mut().enumerate().take(n) {
        *slot = i;
    }
    if request.kind == InputKind::Boundary {
        out[..k].copy_from_slice(&all[..k]);
    } else {
        for i in 0..k {
            let j = i + (input_random(random) % (n - i) as u64) as usize;
            all.swap(i, j);
            out[i] = all[i];
        }
    }
}

fn amount(request: &InputLeafRequest, random: &mut u64, most: usize) -> usize {
    if request.kind == InputKind::Boundary {
        match request.boundary {
            0 => 0,
            1 => most.min(1),
            _ => most,
        }
    } else {
        let limit = (request.size as usize).min(most);
        (input_random(random) % (limit as u64 + 1)) as usize
    }
}

fn file_contents(request: &InputLeafRequest) -> Contents {
    let mut random = request.seed;
    let mut chosen = [0usize; MAX_ITEMS];
    let count = amount(request, &mut random, PATHS.len());
    pick(request, &mut random, PATHS.len(), count, &mut chosen);
    let mut out = Contents::default();
    for &idx in &chosen[..count] {
        out.keys.push(PATHS[idx]);
    }
    for i in 0..count {
        let val = if request.kind == InputKind::Boundary {
            FILE_TEXTS[(i + usize::from(count == 1)) % FILE_TEXTS.len()]
        } else {
            FILE_TEXTS[(input_random(&mut random) % FILE_TEXTS.len() as u64) as usize]
        };
        out.values.push(val);
    }
    out
}

fn list_contents(palette: &[&'static str], request: &InputLeafRequest) -> Contents {
    let mut random = request.seed;
    let mut chosen = [0usize; MAX_ITEMS];
    let count = amount(request, &mut random, palette.len());
    pick(request, &mut random, palette.len(), count, &mut chosen);
    let mut out = Contents::default();
    for &idx in &chosen[..count] {
        let val = if request.kind == InputKind::Boundary && count == 1 {
            palette[1]
        } else {
            palette[idx]
        };
        out.values.push(val);
    }
    out
}

fn env_contents(request: &InputLeafRequest) -> Contents {
    let mut random = request.seed;
    let mut chosen = [0usize; MAX_ITEMS];
    let count = amount(request, &mut random, ENV_NAMES.len());
    pick(request, &mut random, ENV_NAMES.len(), count, &mut chosen);
    let mut out = Contents::default();
    for &idx in &chosen[..count] {
        out.keys.push(ENV_NAMES[idx]);
        out.values.push(ENV_VALUES[idx]);
    }
    out
}

fn value_identity(val: &Value) -> usize {
    match val {
        Value::Ref(r) => std::ptr::from_ref(r.obj()) as usize,
        _ => 0,
    }
}

#[derive(Clone, Copy)]
struct FakeRecipe {
    identity: usize,
    request: InputLeafRequest,
}

#[derive(Default)]
struct FakesShared {
    pending: Vec<FakeRecipe>,
    kept: Vec<FakeRecipe>,
}

/// Manages fake capability recipes across trials and shrinking (`struct mo_fakes`).
pub struct Fakes {
    cap_types: Vec<u32>,
    shared: Rc<RefCell<FakesShared>>,
}

impl Fakes {
    /// Discovers all fakeable capability types present in `types` (`mo_fakes_init`).
    #[must_use]
    pub fn new(types: &TypeTable) -> Self {
        let mut cap_types = Vec::new();
        for (i, item) in types.items.iter().enumerate() {
            if item.kind != TypeKind::Capability {
                continue;
            }
            let Some(cap) = item.capability else {
                continue;
            };
            if matches!(
                cap,
                Capability::Simulation
                    | Capability::Connection
                    | Capability::Database
                    | Capability::File
            ) {
                continue;
            }
            cap_types.push(i as u32);
        }
        Self {
            cap_types,
            shared: Rc::new(RefCell::new(FakesShared::default())),
        }
    }

    /// Returns the list of fakeable capability type indices.
    #[must_use]
    pub fn types(&self) -> &[u32] {
        &self.cap_types
    }

    /// Clears pending recipes before starting a new trial (`mo_fakes_bind`).
    pub fn reset_pending(&self) {
        self.shared.borrow_mut().pending.clear();
    }

    /// Promotes pending recipes to kept recipes for subsequent shrinking copies (`mo_fakes_keep`).
    pub fn keep(&self) {
        let mut shared = self.shared.borrow_mut();
        shared.kept = shared.pending.clone();
    }

    /// Creates one [`FakeFactory`] per fakeable capability type bound to `types` and `host`.
    #[must_use]
    pub fn factories<'a>(&self, types: &'a TypeTable) -> Vec<FakeFactory<'a>> {
        self.cap_types
            .iter()
            .map(|&ty| FakeFactory {
                types,
                ty,
                shared: Rc::clone(&self.shared),
                host: HostContext::stdio(Vec::new(), Vec::new()),
            })
            .collect()
    }
}

/// Per-type [`InputFactory`] producing fake capabilities (`struct mo_fake_context`).
pub struct FakeFactory<'a> {
    types: &'a TypeTable,
    ty: u32,
    shared: Rc<RefCell<FakesShared>>,
    host: HostContext,
}

impl FakeFactory<'_> {
    fn make(
        &mut self,
        heap: &Heap,
        request: &InputLeafRequest,
        remaining: &mut InputLimits,
    ) -> Result<Value, InputResult> {
        let Some(kind) = self.types.get(self.ty).capability else {
            return Err(InputResult::Unsupported);
        };
        let mut contents = Contents::default();
        let mut number: i64 = 0;
        match kind {
            Capability::Simulation
            | Capability::Connection
            | Capability::Database
            | Capability::File => return Err(InputResult::Unsupported),
            Capability::Sqlite | Capability::Network | Capability::Command => {}
            Capability::Args => contents = list_contents(ARGUMENTS, request),
            Capability::Env => contents = env_contents(request),
            Capability::Console => contents = list_contents(LINES, request),
            Capability::Files | Capability::ReadFiles => contents = file_contents(request),
            Capability::Clock => {
                number = if request.kind == InputKind::Random {
                    let span = (i128::from(MO_TIME_MAX_US) + 1 - i128::from(MO_TIME_MIN_US)) as u64;
                    MO_TIME_MIN_US + (request.seed % span) as i64
                } else {
                    match request.boundary {
                        0 => 0,
                        1 => MO_TIME_MIN_US,
                        _ => MO_TIME_MAX_US,
                    }
                };
            }
            Capability::Random | Capability::SecureRandom => {
                number = if request.kind == InputKind::Random {
                    request.seed as i64
                } else {
                    match request.boundary {
                        0 => 0,
                        1 => 1,
                        _ => -1,
                    }
                };
            }
        }
        if !contents.charge(remaining) {
            return Err(InputResult::Limit);
        }
        let text_ty = self
            .types
            .items
            .iter()
            .position(|t| t.kind == TypeKind::Text)
            .unwrap_or(0) as u32;
        let bytes_ty = self
            .types
            .items
            .iter()
            .position(|t| t.kind == TypeKind::Bytes)
            .unwrap_or(0) as u32;

        let init = match kind {
            Capability::Sqlite => {
                let empty = heap
                    .alloc_map(MoMap::new(text_ty, bytes_ty))
                    .map_err(|_| InputResult::NoMemory)?;
                FakeCapInit::Sqlite(Some(empty))
            }
            Capability::Network => {
                let empty = heap
                    .alloc_map(MoMap::new(text_ty, text_ty))
                    .map_err(|_| InputResult::NoMemory)?;
                FakeCapInit::Network(empty)
            }
            Capability::Command => {
                let empty = heap
                    .alloc_map(MoMap::new(bytes_ty, bytes_ty))
                    .map_err(|_| InputResult::NoMemory)?;
                FakeCapInit::Command(empty)
            }
            Capability::Args => {
                let mut items = VecDeque::with_capacity(contents.values.len());
                for s in &contents.values {
                    items.push_back(
                        heap.alloc_text((*s).to_string())
                            .map_err(|_| InputResult::NoMemory)?,
                    );
                }
                let list = heap.alloc_list(items).map_err(|_| InputResult::NoMemory)?;
                FakeCapInit::Args(list)
            }
            Capability::Env => {
                let mut map = MoMap::new(text_ty, text_ty);
                for (k, v) in contents.keys.iter().zip(&contents.values) {
                    let kv = heap
                        .alloc_text((*k).to_string())
                        .map_err(|_| InputResult::NoMemory)?;
                    let vv = heap
                        .alloc_text((*v).to_string())
                        .map_err(|_| InputResult::NoMemory)?;
                    map.set(self.types, heap.hash_key(), kv, vv);
                }
                let mv = heap.alloc_map(map).map_err(|_| InputResult::NoMemory)?;
                FakeCapInit::Env(mv)
            }
            Capability::Console => FakeCapInit::Console {
                input: contents.values.iter().map(|s| (*s).to_string()).collect(),
            },
            Capability::Files => FakeCapInit::Files {
                files: contents
                    .keys
                    .iter()
                    .zip(&contents.values)
                    .map(|(k, v)| ((*k).to_string(), v.as_bytes().to_vec()))
                    .collect(),
                folders: Vec::new(),
            },
            Capability::ReadFiles => FakeCapInit::ReadFiles {
                files: contents
                    .keys
                    .iter()
                    .zip(&contents.values)
                    .map(|(k, v)| ((*k).to_string(), v.as_bytes().to_vec()))
                    .collect(),
                folders: Vec::new(),
            },
            Capability::Clock => FakeCapInit::Clock { time_us: number },
            Capability::Random => FakeCapInit::Random { seed: number },
            Capability::SecureRandom => FakeCapInit::SecureRandom { seed: number },
            _ => unreachable!(),
        };

        self.host
            .restore_fake_capability(heap, self.ty, init)
            .map_err(|err| match err {
                HostError::OutOfMemory { .. } => InputResult::NoMemory,
                _ => InputResult::Unsupported,
            })
    }

    fn register(&self, request: &InputLeafRequest, val: &Value) -> Result<(), InputResult> {
        let mut shared = self.shared.borrow_mut();
        if (shared.pending.len() as u64) >= MO_CLI_FAKE_RECIPES {
            return Err(InputResult::Limit);
        }
        shared.pending.push(FakeRecipe {
            identity: value_identity(val),
            request: *request,
        });
        Ok(())
    }
}

impl InputFactory for FakeFactory<'_> {
    fn ty(&self) -> u32 {
        self.ty
    }

    fn boundaries(&self) -> u32 {
        BOUNDARIES
    }

    fn hashable(&self) -> bool {
        false
    }

    fn build(
        &mut self,
        heap: &Heap,
        request: &InputLeafRequest,
        remaining: &mut InputLimits,
    ) -> Result<Value, InputResult> {
        let val = self.make(heap, request, remaining)?;
        self.register(request, &val)?;
        Ok(val)
    }

    fn copy(
        &mut self,
        heap: &Heap,
        source: &Value,
        remaining: &mut InputLimits,
    ) -> Result<Value, InputResult> {
        let id = value_identity(source);
        let found = {
            let shared = self.shared.borrow();
            shared
                .kept
                .iter()
                .rev()
                .find(|r| r.identity == id)
                .map(|r| r.request)
        };
        let Some(request) = found else {
            return Err(InputResult::Unsupported);
        };
        self.build(heap, &request, remaining)
    }
}
