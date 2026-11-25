//! Synthesizes `UpgradeOld*.mo` source modules directly from a binary `MOSV\x01` schema
//! (`src/tools/upgrade-schema.c` parity) for empty-tail fallback upgrades when recorded source
//! cannot be compiled on the current build.

use mo_def::limits::{
    MO_LIMIT_FIELDS, MO_LIMIT_SAVED_BYTES, MO_LIMIT_SAVED_DEPTH, MO_LIMIT_STRING_SIZE,
    MO_LIMIT_TYPES,
};

use crate::binary::compile_context::CompileFile;
use crate::binary::read_u32_le;

#[derive(Debug, Clone)]
struct SchemaField {
    name: String,
    ty: usize,
}

#[derive(Debug, Clone)]
struct SchemaCase {
    name: String,
    unnamed: bool,
    fields: Vec<SchemaField>,
}

#[derive(Debug, Clone)]
struct SchemaNode {
    tag: u8,
    name: Option<String>,
    parameters: Vec<usize>,
    cases: Vec<SchemaCase>,
}

struct SchemaReader<'a> {
    bytes: &'a [u8],
    pos: usize,
    nodes: Vec<SchemaNode>,
    capacity: usize,
}

impl<'a> SchemaReader<'a> {
    fn read_u8(&mut self) -> Option<u8> {
        let b = *self.bytes.get(self.pos)?;
        self.pos += 1;
        Some(b)
    }

    fn read_u32(&mut self) -> Option<u32> {
        read_u32_le(self.bytes, &mut self.pos)
    }

    fn read_text(&mut self) -> Option<String> {
        let len = self.read_u32()? as usize;
        if len as u64 > MO_LIMIT_STRING_SIZE {
            return None;
        }
        let end = self.pos.checked_add(len)?;
        let slice = self.bytes.get(self.pos..end)?;
        self.pos = end;
        if slice.contains(&0) {
            return None;
        }
        let s = std::str::from_utf8(slice).ok()?;
        Some(s.to_string())
    }

    fn read_node(&mut self, depth: u32) -> Option<usize> {
        if u64::from(depth) >= MO_LIMIT_SAVED_DEPTH {
            return None;
        }
        let tag = self.read_u8()?;
        if tag == 255 {
            let idx = self.read_u32()? as usize;
            return (idx < self.nodes.len()).then_some(idx);
        }
        // Source structs use choice layout (tag 18). Legacy record layout (17) cannot be
        // reproduced by a source declaration without changing its saved shape.
        if tag > 18 || tag == 17 || self.nodes.len() >= self.capacity {
            return None;
        }
        let idx = self.nodes.len();
        self.nodes.push(SchemaNode {
            tag,
            name: None,
            parameters: Vec::new(),
            cases: Vec::new(),
        });
        if tag >= 17 {
            let name = self.read_text()?;
            self.nodes[idx].name = Some(name);
        }
        let param_count = self.read_u32()? as usize;
        if param_count as u64 > MO_LIMIT_TYPES {
            return None;
        }
        let mut params = Vec::with_capacity(param_count);
        for _ in 0..param_count {
            params.push(self.read_node(depth + 1)?);
        }
        self.nodes[idx].parameters = params;

        let group_count = self.read_u32()? as usize;
        if group_count as u64 > MO_LIMIT_FIELDS {
            return None;
        }
        let mut cases = Vec::with_capacity(group_count);
        for _ in 0..group_count {
            let mut case_name = String::new();
            let mut unnamed = false;
            if tag == 18 {
                case_name = self.read_text()?;
                let u = self.read_u8()?;
                if u > 1 {
                    return None;
                }
                unnamed = u != 0;
            }
            let field_count = self.read_u32()? as usize;
            if field_count as u64 > MO_LIMIT_FIELDS {
                return None;
            }
            let mut fields = Vec::with_capacity(field_count);
            for _ in 0..field_count {
                let fname = self.read_text()?;
                let fty = self.read_node(depth + 1)?;
                fields.push(SchemaField {
                    name: fname,
                    ty: fty,
                });
            }
            cases.push(SchemaCase {
                name: case_name,
                unnamed,
                fields,
            });
        }
        self.nodes[idx].cases = cases;
        Some(idx)
    }
}

fn owner_length(name: &str) -> usize {
    let mut owner = 0usize;
    for (i, b) in name.bytes().enumerate() {
        if b == b'(' {
            break;
        }
        if b == b'.' {
            owner = i;
        }
    }
    owner
}

fn same_owner(a: &str, b: &str) -> bool {
    let na = owner_length(a);
    let nb = owner_length(b);
    na == nb && (na == 0 || a.as_bytes()[..na] == b.as_bytes()[..nb])
}

fn push_module_text(out: &mut String, name: &str, root: &str) {
    out.push_str("UpgradeOld");
    if !same_owner(name, root) {
        out.push('.');
        out.push_str(&name[..owner_length(name)]);
    }
}

fn push_generated_name(out: &mut String, name: &str) {
    let owner = owner_length(name);
    let short_name = &name[owner + 1..];
    if !short_name.contains('(') {
        out.push_str(short_name);
        return;
    }
    const HEX: &[u8; 16] = b"0123456789abcdef";
    out.push_str("Saved_");
    for &c in name.as_bytes() {
        out.push(HEX[(c >> 4) as usize] as char);
        out.push(HEX[(c & 15) as usize] as char);
    }
}

fn push_nominal_text(out: &mut String, name: &str, root: &str, module: &str) {
    let bytes = name.as_bytes();
    let mut at = 0usize;
    while at < bytes.len() {
        let b = bytes[at];
        if matches!(b, b'(' | b')' | b',' | b' ') {
            out.push(b as char);
            at += 1;
            continue;
        }
        let start = at;
        let mut dot: Option<usize> = None;
        while at < bytes.len() && !matches!(bytes[at], b'(' | b')' | b',' | b' ') {
            if bytes[at] == b'.' {
                dot = Some(at);
            }
            at += 1;
        }
        match dot {
            None => out.push_str(&name[start..at]),
            Some(d) => {
                let owner = d - start;
                let mod_owner = owner_length(module);
                let root_owner = owner_length(root);
                let local = owner == mod_owner
                    && name.as_bytes()[start..d] == module.as_bytes()[..mod_owner];
                let is_main = owner == root_owner
                    && name.as_bytes()[start..d] == root.as_bytes()[..root_owner];
                if !local {
                    out.push_str("UpgradeOld.");
                    if !is_main {
                        out.push_str(&name[start..d]);
                        out.push('.');
                    }
                }
                out.push_str(&name[d + 1..at]);
            }
        }
    }
}

fn uses_module(nodes: &[SchemaNode], idx: usize, module: &str, depth: u32) -> bool {
    if u64::from(depth) >= MO_LIMIT_SAVED_DEPTH {
        return false;
    }
    let node = &nodes[idx];
    if let Some(ref name) = node.name
        && owner_length(name) != 0
    {
        return same_owner(name, module);
    }
    for &p in &node.parameters {
        if uses_module(nodes, p, module, depth + 1) {
            return true;
        }
    }
    for c in &node.cases {
        for f in &c.fields {
            if uses_module(nodes, f.ty, module, depth + 1) {
                return true;
            }
        }
    }
    false
}

const BUILTINS: [&str; 17] = [
    "Unit", "Bool", "Byte", "Int", "Float", "Char", "Time", "Duration", "Decimal", "Range", "Text",
    "Bytes", "BigInt", "", "List", "Set", "Map",
];

fn push_node_text(
    out: &mut String,
    nodes: &[SchemaNode],
    idx: usize,
    root: &str,
    module: &str,
    depth: u32,
) -> bool {
    if u64::from(depth) >= MO_LIMIT_SAVED_DEPTH {
        return false;
    }
    let node = &nodes[idx];
    if node.tag >= 17 {
        let Some(ref name) = node.name else {
            return false;
        };
        if owner_length(name) != 0 {
            if !same_owner(name, module) {
                push_module_text(out, name, root);
                out.push('.');
            }
            push_generated_name(out, name);
            return true;
        }
        if name.starts_with("Option(") && node.cases.len() == 2 && node.cases[1].fields.len() == 1 {
            out.push_str("Option(");
            if !push_node_text(
                out,
                nodes,
                node.cases[1].fields[0].ty,
                root,
                module,
                depth + 1,
            ) {
                return false;
            }
            out.push(')');
            return true;
        }
        push_nominal_text(out, name, root, module);
        return true;
    }
    let Some(bname) = BUILTINS.get(node.tag as usize) else {
        return false;
    };
    out.push_str(bname);
    if !node.parameters.is_empty() || node.tag == 13 {
        out.push('(');
        for (i, &p) in node.parameters.iter().enumerate() {
            if i != 0 {
                out.push_str(", ");
            }
            if !push_node_text(out, nodes, p, root, module, depth + 1) {
                return false;
            }
        }
        out.push(')');
    }
    true
}

/// Extracts the root nominal state type name from a `MOSV\x01` schema prefix (used when
/// compiling a migration without an old bytecode image).
#[must_use]
pub fn schema_root_name(saved: &[u8]) -> Option<String> {
    if saved.len() < 9 || saved.len() as u64 > MO_LIMIT_SAVED_BYTES || &saved[..5] != b"MOSV\x01" {
        return None;
    }
    let mut pos = 5usize;
    let size = read_u32_le(saved, &mut pos)? as usize;
    let schema = saved.get(pos..pos.checked_add(size)?)?;
    if schema.first().copied()? < 17 {
        return None;
    }
    let mut reader = SchemaReader {
        bytes: schema,
        pos: 1,
        nodes: Vec::new(),
        capacity: 1,
    };
    reader.read_text()
}

/// Synthesizes `UpgradeOld*.mo` [`CompileFile`]s directly from a `MOSV\x01` binary schema
/// (`src/tools/upgrade-schema.c:mo_upgrade_schema_sources`).
#[must_use]
pub fn synthesize_schema_sources(saved: &[u8]) -> Option<Vec<CompileFile>> {
    if saved.len() < 9 || saved.len() as u64 > MO_LIMIT_SAVED_BYTES || &saved[..5] != b"MOSV\x01" {
        return None;
    }
    let mut pos = 5usize;
    let size = read_u32_le(saved, &mut pos)? as usize;
    let schema = saved.get(pos..pos.checked_add(size)?)?;

    let mut reader = SchemaReader {
        bytes: schema,
        pos: 0,
        nodes: Vec::new(),
        capacity: size.min(MO_LIMIT_TYPES as usize),
    };
    let root_idx = reader.read_node(0)?;
    if reader.pos != reader.bytes.len() {
        return None;
    }
    let nodes = &reader.nodes;
    let root_name = nodes[root_idx]
        .name
        .as_deref()
        .unwrap_or("Main.State")
        .to_string();

    let mut files = Vec::new();
    for m in 0..=nodes.len() {
        let module = if m == nodes.len() {
            root_name.as_str()
        } else {
            let Some(ref n) = nodes[m].name else {
                continue;
            };
            if owner_length(n) == 0 {
                continue;
            }
            n.as_str()
        };
        let mut seen = false;
        for node in &nodes[..m] {
            if let Some(ref n) = node.name
                && same_owner(n, module)
            {
                seen = true;
                break;
            }
        }
        if seen {
            continue;
        }

        let mut out = String::new();
        out.push_str("module ");
        push_module_text(&mut out, module, &root_name);
        out.push('\n');

        let mut exposed = false;
        for node in nodes {
            let Some(ref name) = node.name else {
                continue;
            };
            if owner_length(name) == 0 || !same_owner(name, module) {
                continue;
            }
            out.push_str(if exposed { ", " } else { "expose " });
            push_generated_name(&mut out, name);
            out.push_str("(..)");
            exposed = true;
        }
        if exposed {
            out.push('\n');
        }

        for (i, node_i) in nodes.iter().enumerate() {
            let Some(ref name) = node_i.name else {
                continue;
            };
            if owner_length(name) == 0 || same_owner(name, module) {
                continue;
            }
            let duplicate = nodes[..i]
                .iter()
                .any(|prev| prev.name.as_deref().is_some_and(|pn| same_owner(name, pn)));
            if duplicate {
                continue;
            }
            let needed = nodes.iter().any(|node| {
                node.name
                    .as_deref()
                    .is_some_and(|nn| same_owner(nn, module))
                    && node
                        .cases
                        .iter()
                        .any(|c| c.fields.iter().any(|f| uses_module(nodes, f.ty, name, 0)))
            });
            if needed {
                out.push_str("use ");
                push_module_text(&mut out, name, &root_name);
                out.push('\n');
            }
        }

        for node in nodes {
            let Some(ref name) = node.name else {
                continue;
            };
            if owner_length(name) == 0 || !same_owner(name, module) {
                continue;
            }
            let short_name = &name[owner_length(name) + 1..];
            let record = !short_name.contains('(')
                && (node.tag == 17
                    || (node.cases.len() == 1
                        && !node.cases[0].unnamed
                        && node.cases[0].name == short_name));
            out.push_str(if record { "struct " } else { "enum " });
            push_generated_name(&mut out, name);
            out.push('\n');
            for group in &node.cases {
                if !record {
                    out.push_str(&group.name);
                    if !group.fields.is_empty() {
                        out.push('(');
                    }
                }
                for (f_idx, field) in group.fields.iter().enumerate() {
                    if !record && f_idx != 0 {
                        out.push_str(", ");
                    }
                    if !group.unnamed {
                        out.push_str(&field.name);
                        out.push_str(": ");
                    }
                    if !push_node_text(&mut out, nodes, field.ty, &root_name, module, 0) {
                        return None;
                    }
                    if record {
                        out.push('\n');
                    }
                }
                if !record {
                    if !group.fields.is_empty() {
                        out.push(')');
                    }
                    out.push('\n');
                }
            }
            out.push_str("end\n");
        }

        if same_owner(module, &root_name) {
            out.push_str("fn upgrade_input(value: ");
            if !push_node_text(&mut out, nodes, root_idx, &root_name, module, 0) {
                return None;
            }
            out.push_str(") : ");
            if !push_node_text(&mut out, nodes, root_idx, &root_name, module, 0) {
                return None;
            }
            out.push_str("\nvalue\nend\n");
        }

        let mut path = String::new();
        push_module_text(&mut path, module, &root_name);
        path.push_str(".mo");
        files.push(CompileFile {
            path,
            source: out.into_bytes(),
        });
    }
    Some(files)
}
