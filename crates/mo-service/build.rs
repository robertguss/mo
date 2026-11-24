#![forbid(unsafe_code)]

//! Build script embedding the 18 protocol `.mo` sources (exact parity with `ci/embed_protocol.py`
//! and `Makefile`'s `PROTOCOL_SOURCES`) and generating `MO_BUILD_LABEL`.

use std::env;
use std::fs;
use std::path::PathBuf;

const PROTOCOL_SOURCES: &[&str] = &[
    "runtime/mcp-fetch.mo",
    "runtime/http-server.mo",
    "runtime/http-authority.mo",
    "runtime/http-policy.mo",
    "runtime/http-body.mo",
    "runtime/mcp.mo",
    "runtime/mcp-number.mo",
    "runtime/mcp-type.mo",
    "runtime/mcp-schema.mo",
    "runtime/mcp-scalar.mo",
    "runtime/mcp-value.mo",
    "runtime/mcp-rpc.mo",
    "runtime/mcp-transport.mo",
    "runtime/mcp-views.mo",
    "lib/json.mo",
    "lib/list.mo",
    "lib/option.mo",
    "lib/result.mo",
];

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let manifest_dir = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap_or_default());
    let repo_root = manifest_dir
        .parent()
        .and_then(|p| p.parent())
        .map(PathBuf::from)
        .unwrap_or_else(|| manifest_dir.clone());
    let out_dir = PathBuf::from(env::var("OUT_DIR").unwrap_or_default());

    let mut code = String::from(
        "/// Embedded protocol `.mo` sources in exact `Makefile` `PROTOCOL_SOURCES` order.\n",
    );
    code.push_str("pub const PROTOCOL_SOURCES: &[(&str, &[u8])] = &[\n");
    for rel in PROTOCOL_SOURCES {
        let abs = repo_root.join(rel);
        println!("cargo:rerun-if-changed={}", abs.display());
        let data = fs::read(&abs)?;
        code.push_str(&format!("    ({rel:?}, &[\n"));
        for chunk in data.chunks(24) {
            code.push_str("        ");
            for b in chunk {
                code.push_str(&format!("{b}, "));
            }
            code.push('\n');
        }
        code.push_str("    ]),\n");
    }
    code.push_str("];\n");

    let target = env::var("TARGET").unwrap_or_else(|_| "unknown".to_string());
    let profile = env::var("PROFILE").unwrap_or_else(|_| "debug".to_string());
    let label = format!("mo 0.3.0; rust {target} {profile}");
    code.push_str(&format!(
        "/// Build label embedded in `MOCS\\x01` compile metadata (<= 256 bytes).\npub const MO_BUILD_LABEL: &str = {label:?};\n"
    ));

    let out_path = out_dir.join("embedded_protocol.rs");
    let existing = fs::read(&out_path).ok();
    if existing.as_deref() != Some(code.as_bytes()) {
        fs::write(&out_path, code.as_bytes())?;
    }
    Ok(())
}
