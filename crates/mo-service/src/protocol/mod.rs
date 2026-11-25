//! Embedded Mo protocol runtime (`src/tools/protocol*.c` parity).
//!
//! Compiles the 18 embedded `runtime/*.mo` + `lib/*.mo` protocol sources into a validated
//! `mo_vm::Image` (cached as bytecode wire in a `OnceLock`), and drives HTTP/1.1, JSON-RPC,
//! MCP tool calls, operator views, paged exports, SSE sessions, and live upgrades.

pub mod descriptor;
pub mod server;
pub mod value;

use std::sync::OnceLock;
use std::thread;

use mo_base::{DiagBag, SourceMap};
use mo_bytecode::{BytecodeFile, CaseShape, load as load_bytecode};
use mo_check::{check_program, load_program_sources};
use mo_codegen::compile;
use mo_core::check_program as check_core_program;
use mo_def::limits::{MO_LIMIT_MCP_BODY_BYTES, MO_SOURCE_STACK_SIZE};
use mo_heap::Value;
use mo_lower::lower_program;
use mo_vm::{Image, Limits};

include!(concat!(env!("OUT_DIR"), "/embedded_protocol.rs"));

/// Indices into [`ProtocolFunctions`] matching `enum protocol_function` in `protocol-server.h`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(usize)]
pub enum Pf {
    /// `HttpServer.scan_headers`
    Scan = 0,
    /// `HttpServer.parse_headers`
    Headers = 1,
    /// `HttpAuthority.check`
    Authority = 2,
    /// `HttpPolicy.inspect`
    Inspect = 3,
    /// `HttpPolicy.body_plan`
    Plan = 4,
    /// `HttpBody.fixed`
    Fixed = 5,
    /// `HttpBody.chunked`
    Chunked = 6,
    /// `HttpBody.feed`
    Feed = 7,
    /// `Mcp.prepare`
    Prepare = 8,
    /// `McpTransport.receive`
    Receive = 9,
    /// `McpValue.arguments`
    Arguments = 10,
    /// `McpValue.encode`
    Encode = 11,
    /// `Mcp.tool_reply`
    ToolReply = 12,
    /// `McpTransport.fallback`
    Fallback = 13,
    /// `McpTransport.http`
    Http = 14,
    /// `McpTransport.http_head`
    HttpHead = 15,
    /// `McpTransport.http100`
    Http100 = 16,
    /// `McpViews.render`
    View = 17,
    /// `McpViews.export_id`
    ExportId = 18,
    /// `McpViews.failure_id`
    FailureId = 19,
    /// `McpViews.failure_after`
    FailureAfter = 20,
    /// `McpViews.failure_reply`
    FailureView = 21,
    /// `McpViews.export_reply`
    Export = 22,
    /// `Mcp.operator_reply`
    OperatorReply = 23,
    /// `HttpPolicy.stream_plan`
    StreamPlan = 24,
    /// `McpTransport.http_session`
    HttpSession = 25,
    /// `McpTransport.stream_head`
    StreamHead = 26,
    /// `McpTransport.list_changed`
    ListChanged = 27,
    /// `McpViews.upgrade_arguments`
    UpgradeArguments = 28,
    /// `Mcp.upgrade_reply`
    UpgradeReply = 29,
    /// `McpViews.upgrade_kind`
    UpgradeKind = 30,
    /// `McpFetch.read`
    FetchRead = 31,
    /// `McpFetch.upgrade`
    FetchUpgrade = 32,
    /// `McpFetch.overview_source`
    FetchOverviewSource = 33,
    /// `McpViews.offline_value`
    OfflineValue = 34,
    /// `Mcp.catalog`
    Catalog = 35,
}

const FUNCTION_NAMES: &[&str] = &[
    "HttpServer.scan_headers",
    "HttpServer.parse_headers",
    "HttpAuthority.check",
    "HttpPolicy.inspect",
    "HttpPolicy.body_plan",
    "HttpBody.fixed",
    "HttpBody.chunked",
    "HttpBody.feed",
    "Mcp.prepare",
    "McpTransport.receive",
    "McpValue.arguments",
    "McpValue.encode",
    "Mcp.tool_reply",
    "McpTransport.fallback",
    "McpTransport.http",
    "McpTransport.http_head",
    "McpTransport.http100",
    "McpViews.render",
    "McpViews.export_id",
    "McpViews.failure_id",
    "McpViews.failure_after",
    "McpViews.failure_reply",
    "McpViews.export_reply",
    "Mcp.operator_reply",
    "HttpPolicy.stream_plan",
    "McpTransport.http_session",
    "McpTransport.stream_head",
    "McpTransport.list_changed",
    "McpViews.upgrade_arguments",
    "Mcp.upgrade_reply",
    "McpViews.upgrade_kind",
    "McpFetch.read",
    "McpFetch.upgrade",
    "McpFetch.overview_source",
    "McpViews.offline_value",
    "Mcp.catalog",
];

/// Resolved bytecode function indices for the embedded protocol image.
#[derive(Debug, Clone)]
pub struct ProtocolFunctions {
    indices: Vec<u32>,
}

impl ProtocolFunctions {
    /// Resolves all required protocol function indices in `file`.
    #[must_use]
    pub fn resolve(file: &BytecodeFile) -> Option<Self> {
        let mut indices = Vec::with_capacity(FUNCTION_NAMES.len());
        for &target in FUNCTION_NAMES {
            let idx = file
                .functions
                .iter()
                .position(|f| file.string(f.name) == target)?;
            indices.push(idx as u32);
        }
        Some(Self { indices })
    }

    /// Returns the bytecode function index for `pf`.
    #[must_use]
    pub fn get(&self, pf: Pf) -> u32 {
        self.indices[pf as usize]
    }
}

static PROTOCOL_BYTECODE: OnceLock<Result<Vec<u8>, String>> = OnceLock::new();

fn compile_protocol_bytecode_inner() -> Result<Vec<u8>, String> {
    let mut sources = SourceMap::new();
    let mut diags = DiagBag::new();
    let mut library_flags = Vec::with_capacity(PROTOCOL_SOURCES.len());
    for &(path, bytes) in PROTOCOL_SOURCES {
        sources
            .add_bytes(path.as_bytes().to_vec(), bytes.to_vec())
            .map_err(|e| format!("protocol source add {path}: {e:?}"))?;
        library_flags.push(path.starts_with("lib/"));
    }
    let set = load_program_sources(&mut sources, Some(&library_flags), &mut diags)
        .ok_or_else(|| format!("protocol load failed: {:?}", diags.items()))?;
    if diags.has_any() {
        return Err(format!("protocol load diags: {:?}", diags.items()));
    }
    let program = check_program(Some(&sources), set, &mut diags)
        .ok_or_else(|| format!("protocol check failed: {:?}", diags.items()))?;
    if diags.has_any() {
        return Err(format!("protocol check diags: {:?}", diags.items()));
    }
    let core = lower_program(&program, Some(&sources), &mut diags)
        .ok_or_else(|| format!("protocol lower failed: {:?}", diags.items()))?;
    if diags.has_any() || !check_core_program(&core.arena, &core.program, &mut diags) {
        return Err(format!("protocol core check failed: {:?}", diags.items()));
    }
    let wire = compile(&core.arena, &core.program, &mut diags)
        .ok_or_else(|| format!("protocol codegen failed: {:?}", diags.items()))?;
    if diags.has_any() {
        return Err(format!("protocol codegen diags: {:?}", diags.items()));
    }
    // Verify bytecode loads cleanly and resolves all ABI functions.
    let bc = load_bytecode(&wire).map_err(|e| format!("protocol bytecode verify: {e}"))?;
    ProtocolFunctions::resolve(&bc).ok_or_else(|| "protocol ABI resolution failed".to_string())?;
    Ok(wire)
}

/// Returns the cached compiled `.bc` bytes for the 18 embedded protocol sources.
pub fn protocol_bytecode() -> Result<&'static [u8], String> {
    let res = PROTOCOL_BYTECODE.get_or_init(|| {
        thread::scope(|s| {
            let handle = thread::Builder::new()
                .name("mo-protocol-compile".to_string())
                .stack_size(MO_SOURCE_STACK_SIZE as usize)
                .spawn_scoped(s, compile_protocol_bytecode_inner)
                .map_err(|e| format!("spawn protocol compile thread: {e}"))?;
            handle
                .join()
                .map_err(|_| "protocol compile thread panicked".to_string())?
        })
    });
    match res {
        Ok(bytes) => Ok(bytes.as_slice()),
        Err(msg) => Err(msg.clone()),
    }
}

/// Instantiates a fresh initialized protocol [`Image`] and its resolved [`ProtocolFunctions`].
pub fn build_protocol_image() -> Result<(Image, ProtocolFunctions), String> {
    let wire = protocol_bytecode()?;
    let bc = load_bytecode(wire).map_err(|e| format!("load protocol bytecode: {e}"))?;
    let fns = ProtocolFunctions::resolve(&bc)
        .ok_or_else(|| "missing protocol ABI function".to_string())?;
    let image = Image::init(bc, Limits::default())
        .map_err(|b| format!("protocol image init failed: {}", b.1.reason))?;
    Ok((image, fns))
}

/// Counts structural JSON tokens (`[`, `{`, `,`) outside quoted strings (`mo_protocol_body_marks`).
#[must_use]
pub fn body_marks(bytes: &[u8]) -> Option<usize> {
    if bytes.len() as u64 > MO_LIMIT_MCP_BODY_BYTES {
        return None;
    }
    let mut count = 0usize;
    let mut quoted = false;
    let mut escaped = false;
    for &c in bytes {
        if quoted {
            if escaped {
                escaped = false;
            } else if c == b'\\' {
                escaped = true;
            } else if c == b'"' {
                quoted = false;
            }
        } else if c == b'"' {
            quoted = true;
        } else if matches!(c, b'[' | b'{' | b',') {
            count += 1;
        }
    }
    Some(count)
}

/// Extracts field `field` from a protocol choice/struct `Value` (`mo_protocol_field`).
#[must_use]
pub fn choice_field(file: &BytecodeFile, ty: u32, val: &Value, field: usize) -> Option<Value> {
    let tag = val.as_tag();
    let case = file.types.get(ty).cases.get(tag as usize)?;
    match file.types.case_shape(ty, tag) {
        CaseShape::Empty => None,
        CaseShape::Inline => match val {
            Value::ChoiceInline { payload, .. } if field == 0 => {
                let fty = case.fields.first()?.ty;
                Some(mo_heap::hash::inline_payload_to_value(
                    *payload,
                    file.types.get(fty).kind,
                ))
            }
            _ => None,
        },
        CaseShape::Ref => match val {
            Value::ChoiceRef { payload, .. } if field == 0 => Some(Value::Ref(payload.clone())),
            _ => None,
        },
        CaseShape::Boxed => match val {
            Value::ChoiceRef { payload, .. } => match payload.obj() {
                mo_heap::HeapObj::CaseBlock { fields, .. } => fields.get(field).cloned(),
                _ => None,
            },
            _ => None,
        },
    }
}
