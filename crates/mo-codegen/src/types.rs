//! Type translation, `.def` signature instantiation, constant interning, and literal
//! encoding matching `src/codegen/types.c`.

use std::collections::HashMap;

use mo_bytecode::{
    ABILITY_EQUAL, ABILITY_HASH, Capability, ConstantDef, ConstantValue, NO_INDEX, TypeCase,
    TypeDef, TypeField, TypeKind,
};
use mo_core::{Cap, CoreInteger, CoreNatural, Lit, SimOp, Ty, TyId};
use mo_def::limits::{
    MO_LIMIT_BYTECODE_PARAMETERS, MO_LIMIT_CONSTANTS, MO_LIMIT_CORE_DEPTH,
    MO_LIMIT_DEF_SIGNATURE_DEPTH, MO_LIMIT_FIELDS, MO_LIMIT_PARAMETERS, MO_LIMIT_STRING_SIZE,
    MO_LIMIT_TYPES,
};
use mo_def::{ErrorCode, Opcode};

use crate::bigint::convert_bigint;
use crate::compiler::{Compiler, ConstantKey};
use crate::operations::Operation;

const BUILTIN_OPTION: u8 = 0;
const BUILTIN_RESULT: u8 = 1;
const BUILTIN_ORDERING: u8 = 2;
const BUILTIN_FILE_ERROR: u8 = 3;
const BUILTIN_SIMULATION_ERROR: u8 = 4;
const BUILTIN_NETWORK_ERROR: u8 = 5;
const BUILTIN_SQLITE_VALUE: u8 = 6;
const BUILTIN_SQLITE_ERROR: u8 = 7;
const BUILTIN_ENTRY_KIND: u8 = 8;
const BUILTIN_ENTRY: u8 = 9;
const BUILTIN_SQLITE_ROWS: u8 = 10;

pub(crate) fn bare_type(kind: TypeKind) -> TypeDef {
    TypeDef {
        kind,
        parameters: Vec::new(),
        result: 0,
        function: 0,
        capability: None,
        name_idx: None,
        name: None,
        fields: Vec::new(),
        cases: Vec::new(),
        is_reference: false,
        has_fields: false,
    }
}

fn core_cap_to_bytecode(cap: Cap) -> Capability {
    match cap {
        Cap::Console => Capability::Console,
        Cap::Files => Capability::Files,
        Cap::ReadFiles => Capability::ReadFiles,
        Cap::Clock => Capability::Clock,
        Cap::Random => Capability::Random,
        Cap::Args => Capability::Args,
        Cap::Env => Capability::Env,
        Cap::Network => Capability::Network,
        Cap::Connection => Capability::Connection,
        Cap::Sqlite => Capability::Sqlite,
        Cap::Database => Capability::Database,
        Cap::Command => Capability::Command,
        Cap::SecureRandom => Capability::SecureRandom,
        Cap::File => Capability::File,
    }
}

#[derive(Debug, Clone)]
enum SigTy {
    Resolved(u32),
    Scalar(TypeKind),
    Tuple(Vec<SigTy>),
    List(Box<SigTy>),
    Set(Box<SigTy>),
    Map(Box<SigTy>, Box<SigTy>),
    Option(Box<SigTy>),
    Result(Box<SigTy>, Box<SigTy>),
    Ordering,
    Entry,
    FileError,
    NetworkError,
    SqliteValue,
    SqliteRows,
    SqliteError,
    Cap(Cap),
}

fn take_char(cursor: &mut &str, ch: char) -> bool {
    *cursor = cursor.trim_start_matches(' ');
    if let Some(rest) = cursor.strip_prefix(ch) {
        *cursor = rest;
        true
    } else {
        false
    }
}

fn parse_sig_type(cursor: &mut &str, vars: &[Option<u32>; 4], depth: u32) -> Option<SigTy> {
    if depth >= MO_LIMIT_DEF_SIGNATURE_DEPTH as u32 {
        return None;
    }
    if take_char(cursor, '(') {
        let mut args = Vec::new();
        loop {
            args.push(parse_sig_type(cursor, vars, depth + 1)?);
            if !take_char(cursor, ',') {
                break;
            }
        }
        if !take_char(cursor, ')') {
            return None;
        }
        return Some(SigTy::Tuple(args));
    }

    *cursor = cursor.trim_start_matches(' ');
    let start = *cursor;
    let end = start
        .find(|c: char| !(c.is_ascii_alphabetic() || c == '.'))
        .unwrap_or(start.len());
    let name = &start[..end];
    *cursor = &start[end..];

    if name.len() == 1 {
        let slot = match name {
            "T" => 0,
            "U" => 1,
            "K" => 2,
            "V" => 3,
            _ => return None,
        };
        return Some(SigTy::Resolved(vars[slot]?));
    }

    match name {
        "Unit" => return Some(SigTy::Scalar(TypeKind::Unit)),
        "Bool" => return Some(SigTy::Scalar(TypeKind::Bool)),
        "Byte" => return Some(SigTy::Scalar(TypeKind::Byte)),
        "Int" => return Some(SigTy::Scalar(TypeKind::Int)),
        "Float" => return Some(SigTy::Scalar(TypeKind::Float)),
        "Char" => return Some(SigTy::Scalar(TypeKind::Char)),
        "Time" => return Some(SigTy::Scalar(TypeKind::Time)),
        "Duration" => return Some(SigTy::Scalar(TypeKind::Duration)),
        "Decimal" => return Some(SigTy::Scalar(TypeKind::Decimal)),
        "Range" => return Some(SigTy::Scalar(TypeKind::Range)),
        "Text" => return Some(SigTy::Scalar(TypeKind::Text)),
        "Bytes" => return Some(SigTy::Scalar(TypeKind::Bytes)),
        "BigInt" => return Some(SigTy::Scalar(TypeKind::BigInt)),
        "Ordering" => return Some(SigTy::Ordering),
        "Entry" => return Some(SigTy::Entry),
        "FileError" => return Some(SigTy::FileError),
        "NetworkError" => return Some(SigTy::NetworkError),
        "Sqlite.Value" => return Some(SigTy::SqliteValue),
        "Sqlite.Rows" => return Some(SigTy::SqliteRows),
        "SqliteError" => return Some(SigTy::SqliteError),
        "Console" => return Some(SigTy::Cap(Cap::Console)),
        "Files" => return Some(SigTy::Cap(Cap::Files)),
        "ReadFiles" => return Some(SigTy::Cap(Cap::ReadFiles)),
        "Clock" => return Some(SigTy::Cap(Cap::Clock)),
        "Random" => return Some(SigTy::Cap(Cap::Random)),
        "Args" => return Some(SigTy::Cap(Cap::Args)),
        "Env" => return Some(SigTy::Cap(Cap::Env)),
        "Network" => return Some(SigTy::Cap(Cap::Network)),
        "Connection" => return Some(SigTy::Cap(Cap::Connection)),
        "Sqlite" => return Some(SigTy::Cap(Cap::Sqlite)),
        "Sqlite.Database" => return Some(SigTy::Cap(Cap::Database)),
        "Command" => return Some(SigTy::Cap(Cap::Command)),
        "SecureRandom" => return Some(SigTy::Cap(Cap::SecureRandom)),
        "Files.File" => return Some(SigTy::Cap(Cap::File)),
        "List" | "Set" | "Map" | "Option" | "Result" => {}
        _ => return None,
    }

    if !take_char(cursor, '(') {
        return None;
    }
    let mut args = Vec::new();
    loop {
        args.push(parse_sig_type(cursor, vars, depth + 1)?);
        if !take_char(cursor, ',') {
            break;
        }
    }
    if !take_char(cursor, ')') {
        return None;
    }

    match name {
        "List" if args.len() == 1 => Some(SigTy::List(Box::new(args.remove(0)))),
        "Set" if args.len() == 1 => Some(SigTy::Set(Box::new(args.remove(0)))),
        "Option" if args.len() == 1 => Some(SigTy::Option(Box::new(args.remove(0)))),
        "Map" if args.len() == 2 => {
            let v = args.remove(1);
            let k = args.remove(0);
            Some(SigTy::Map(Box::new(k), Box::new(v)))
        }
        "Result" if args.len() == 2 => {
            let e = args.remove(1);
            let ok = args.remove(0);
            Some(SigTy::Result(Box::new(ok), Box::new(e)))
        }
        _ => None,
    }
}

pub(crate) fn selector_u32(num: &CoreNatural) -> u32 {
    let mut out = 0u32;
    for &b in num.digits.as_bytes() {
        let digit = u32::from(b - b'0');
        out = out * 10 + digit;
    }
    out
}

impl<'a> Compiler<'a> {
    pub fn type_result<T>(&mut self, res: Result<T, ErrorCode>) -> Option<T> {
        match res {
            Ok(v) => Some(v),
            Err(code) => {
                self.fail(
                    code,
                    "The generated type or layout was refused by the runtime type builder.",
                );
                None
            }
        }
    }

    pub fn intern_type(&mut self, def: TypeDef) -> Option<u32> {
        let res = self.types.intern(def);
        self.type_result(res)
    }

    pub fn scalar_type(&mut self, kind: TypeKind) -> Option<u32> {
        self.intern_type(bare_type(kind))
    }

    fn append_type_text(&mut self, out: &mut String, s: &str) -> bool {
        if (s.len() as u64) > MO_LIMIT_STRING_SIZE.saturating_sub(out.len() as u64) {
            return self.fail(
                ErrorCode::BytecodeTooBig,
                "Instantiated type name exceeds MO_LIMIT_STRING_SIZE.",
            );
        }
        out.push_str(s);
        true
    }

    fn format_type_name(&mut self, id: u32, out: &mut String, depth: u32) -> bool {
        if u64::from(depth) >= MO_LIMIT_CORE_DEPTH {
            return self.fail(
                ErrorCode::NestedTooDeeply,
                "Instantiated type name nesting exceeds MO_LIMIT_CORE_DEPTH.",
            );
        }
        let kind = self.types.items[id as usize].kind;
        match kind {
            TypeKind::Unit => self.append_type_text(out, "Unit"),
            TypeKind::Bool => self.append_type_text(out, "Bool"),
            TypeKind::Byte => self.append_type_text(out, "Byte"),
            TypeKind::Int => self.append_type_text(out, "Int"),
            TypeKind::Float => self.append_type_text(out, "Float"),
            TypeKind::Char => self.append_type_text(out, "Char"),
            TypeKind::Time => self.append_type_text(out, "Time"),
            TypeKind::Duration => self.append_type_text(out, "Duration"),
            TypeKind::Decimal => self.append_type_text(out, "Decimal"),
            TypeKind::Range => self.append_type_text(out, "Range"),
            TypeKind::Text => self.append_type_text(out, "Text"),
            TypeKind::Bytes => self.append_type_text(out, "Bytes"),
            TypeKind::BigInt => self.append_type_text(out, "BigInt"),
            TypeKind::Capability => {
                let cap_name = self.types.items[id as usize]
                    .capability
                    .map_or("", Capability::name);
                self.append_type_text(out, cap_name)
            }
            TypeKind::Record | TypeKind::Choice | TypeKind::Env => {
                let owned = self.types.items[id as usize]
                    .name
                    .clone()
                    .unwrap_or_default();
                self.append_type_text(out, &owned)
            }
            TypeKind::Tuple
            | TypeKind::Function
            | TypeKind::List
            | TypeKind::Set
            | TypeKind::Map => {
                let start = match kind {
                    TypeKind::Function => "fn(",
                    TypeKind::List => "List(",
                    TypeKind::Set => "Set(",
                    TypeKind::Map => "Map(",
                    _ => "(",
                };
                if !self.append_type_text(out, start) {
                    return false;
                }
                let params = self.types.items[id as usize].parameters.clone();
                let res = self.types.items[id as usize].result;
                for (i, &p) in params.iter().enumerate() {
                    if (i != 0 && !self.append_type_text(out, ", "))
                        || !self.format_type_name(p, out, depth + 1)
                    {
                        return false;
                    }
                }
                if !self.append_type_text(out, ")") {
                    return false;
                }
                if kind == TypeKind::Function {
                    self.append_type_text(out, " : ") && self.format_type_name(res, out, depth + 1)
                } else {
                    true
                }
            }
        }
    }

    fn builtin_choice_type(&mut self, tag: u8, a: u32, b: u32) -> Option<u32> {
        let key = (tag, a, b);
        if let Some(&old) = self.builtin_index.get(&key) {
            return Some(old);
        }
        let base = match tag {
            BUILTIN_OPTION => "Option",
            BUILTIN_RESULT => "Result",
            BUILTIN_ORDERING => "Ordering",
            BUILTIN_FILE_ERROR => "FileError",
            BUILTIN_SIMULATION_ERROR => "SimulationError",
            BUILTIN_NETWORK_ERROR => "NetworkError",
            BUILTIN_SQLITE_VALUE => "Sqlite.Value",
            BUILTIN_SQLITE_ERROR => "SqliteError",
            BUILTIN_ENTRY_KIND => "EntryKind",
            _ => return None,
        };

        let mut buf = String::new();
        if !self.append_type_text(&mut buf, base) {
            return None;
        }
        if a != NO_INDEX {
            if !self.append_type_text(&mut buf, "(") || !self.format_type_name(a, &mut buf, 0) {
                return None;
            }
            if b != NO_INDEX
                && (!self.append_type_text(&mut buf, ", ")
                    || !self.format_type_name(b, &mut buf, 0))
            {
                return None;
            }
            if !self.append_type_text(&mut buf, ")") {
                return None;
            }
        }
        let nominal = self.intern_name(&buf)?;
        let decl = self.types.declare(TypeKind::Choice, nominal);
        let id = self.type_result(decl)?;

        let case_names: &[&str] = match tag {
            BUILTIN_OPTION => &["None", "Some"],
            BUILTIN_RESULT => &["Ok", "Error"],
            BUILTIN_ORDERING => &["Less", "Equal", "Greater"],
            BUILTIN_ENTRY_KIND => &["File", "Folder", "Link", "Other"],
            BUILTIN_SIMULATION_ERROR => &["ServiceDown", "ToolFailed"],
            BUILTIN_NETWORK_ERROR => &[
                "UnknownHost",
                "Refused",
                "TimedOut",
                "Closed",
                "Broken",
                "Certificate",
            ],
            BUILTIN_SQLITE_VALUE => &["Null", "Integer", "Real", "String", "Blob"],
            BUILTIN_SQLITE_ERROR => &["OutsideFolder", "NotPermitted", "SqlFailed"],
            BUILTIN_FILE_ERROR => &["NotFound", "NotAllowed", "NotText", "Failed"],
            _ => return None,
        };

        let mut file_err_text = NO_INDEX;
        if tag == BUILTIN_FILE_ERROR {
            file_err_text = self.scalar_type(TypeKind::Text)?;
            self.intern_string("path")?;
            self.intern_string("reason")?;
        }
        let mut net_text = NO_INDEX;
        let mut net_int = NO_INDEX;
        if tag == BUILTIN_NETWORK_ERROR {
            net_text = self.scalar_type(TypeKind::Text)?;
            net_int = self.scalar_type(TypeKind::Int)?;
            self.intern_string("host")?;
            self.intern_string("port")?;
            self.intern_string("reason")?;
        }
        let mut sqlite_val_tys = [NO_INDEX; 4];
        if tag == BUILTIN_SQLITE_VALUE {
            for (slot, kind) in sqlite_val_tys.iter_mut().zip([
                TypeKind::Int,
                TypeKind::Float,
                TypeKind::Text,
                TypeKind::Bytes,
            ]) {
                *slot = self.scalar_type(kind)?;
            }
        }
        let mut sqlite_err_text = NO_INDEX;
        if tag == BUILTIN_SQLITE_ERROR {
            sqlite_err_text = self.scalar_type(TypeKind::Text)?;
            self.intern_string("path")?;
            self.intern_string("reason")?;
            self.intern_string("message")?;
        }

        let mut cases = Vec::with_capacity(case_names.len());
        let mut unnamed_flags = Vec::with_capacity(case_names.len());
        for (i, &cname) in case_names.iter().enumerate() {
            let name_idx = self.intern_string(cname)?;
            let mut fields = Vec::new();
            let mut unnamed = false;
            match tag {
                BUILTIN_OPTION if i == 1 => {
                    fields.push(TypeField {
                        name_idx: None,
                        name: None,
                        ty: a,
                    });
                    unnamed = true;
                }
                BUILTIN_RESULT => {
                    fields.push(TypeField {
                        name_idx: None,
                        name: None,
                        ty: if i == 0 { a } else { b },
                    });
                    unnamed = true;
                }
                BUILTIN_FILE_ERROR => {
                    fields.push(TypeField {
                        name_idx: None,
                        name: Some("path".to_string()),
                        ty: file_err_text,
                    });
                    if i == 3 {
                        fields.push(TypeField {
                            name_idx: None,
                            name: Some("reason".to_string()),
                            ty: file_err_text,
                        });
                    }
                }
                BUILTIN_NETWORK_ERROR => match i {
                    0 => fields.push(TypeField {
                        name_idx: None,
                        name: Some("host".to_string()),
                        ty: net_text,
                    }),
                    1 => {
                        fields.push(TypeField {
                            name_idx: None,
                            name: Some("host".to_string()),
                            ty: net_text,
                        });
                        fields.push(TypeField {
                            name_idx: None,
                            name: Some("port".to_string()),
                            ty: net_int,
                        });
                    }
                    4 | 5 => fields.push(TypeField {
                        name_idx: None,
                        name: Some("reason".to_string()),
                        ty: net_text,
                    }),
                    _ => {}
                },
                BUILTIN_SQLITE_VALUE if i != 0 => {
                    fields.push(TypeField {
                        name_idx: None,
                        name: None,
                        ty: sqlite_val_tys[i - 1],
                    });
                    unnamed = true;
                }
                BUILTIN_SQLITE_ERROR => {
                    let fname = ["path", "reason", "message"][i];
                    fields.push(TypeField {
                        name_idx: None,
                        name: Some(fname.to_string()),
                        ty: sqlite_err_text,
                    });
                }
                _ => {}
            }
            unnamed_flags.push(unnamed);
            cases.push(TypeCase {
                name_idx,
                name: cname.to_string(),
                fields,
                unnamed,
            });
        }

        let def_res = self.types.define_choice(id, cases);
        self.type_result(def_res)?;
        if !self.record_service_shape(id, false, unnamed_flags) {
            return None;
        }
        self.builtin_index.insert(key, id);
        Some(id)
    }

    fn builtin_entry_type(&mut self) -> Option<u32> {
        let key = (BUILTIN_ENTRY, NO_INDEX, NO_INDEX);
        if let Some(&old) = self.builtin_index.get(&key) {
            return Some(old);
        }
        let text_ty = self.scalar_type(TypeKind::Text)?;
        let kind_ty = self.builtin_choice_type(BUILTIN_ENTRY_KIND, NO_INDEX, NO_INDEX)?;
        self.intern_string("Entry")?;
        self.intern_string("name")?;
        self.intern_string("kind")?;
        let decl = self.types.declare(TypeKind::Record, "Entry".to_string());
        let id = self.type_result(decl)?;
        let def_res = self.types.define_record(
            id,
            vec![
                TypeField {
                    name_idx: None,
                    name: Some("name".to_string()),
                    ty: text_ty,
                },
                TypeField {
                    name_idx: None,
                    name: Some("kind".to_string()),
                    ty: kind_ty,
                },
            ],
        );
        self.type_result(def_res)?;
        if !self.record_service_shape(id, true, Vec::new()) {
            return None;
        }
        self.builtin_index.insert(key, id);
        Some(id)
    }

    fn builtin_sqlite_rows_type(&mut self) -> Option<u32> {
        let key = (BUILTIN_SQLITE_ROWS, NO_INDEX, NO_INDEX);
        if let Some(&old) = self.builtin_index.get(&key) {
            return Some(old);
        }
        let text_ty = self.scalar_type(TypeKind::Text)?;
        let mut list_text = bare_type(TypeKind::List);
        list_text.parameters = vec![text_ty];
        let columns_ty = self.intern_type(list_text)?;

        let val_ty = self.builtin_choice_type(BUILTIN_SQLITE_VALUE, NO_INDEX, NO_INDEX)?;
        let mut list_val = bare_type(TypeKind::List);
        list_val.parameters = vec![val_ty];
        let row_ty = self.intern_type(list_val)?;
        let mut list_rows = bare_type(TypeKind::List);
        list_rows.parameters = vec![row_ty];
        let rows_ty = self.intern_type(list_rows)?;

        self.intern_string("Sqlite.Rows")?;
        self.intern_string("columns")?;
        self.intern_string("rows")?;
        let decl = self
            .types
            .declare(TypeKind::Record, "Sqlite.Rows".to_string());
        let id = self.type_result(decl)?;
        let def_res = self.types.define_record(
            id,
            vec![
                TypeField {
                    name_idx: None,
                    name: Some("columns".to_string()),
                    ty: columns_ty,
                },
                TypeField {
                    name_idx: None,
                    name: Some("rows".to_string()),
                    ty: rows_ty,
                },
            ],
        );
        self.type_result(def_res)?;
        if !self.record_service_shape(id, true, Vec::new()) {
            return None;
        }
        self.builtin_index.insert(key, id);
        Some(id)
    }

    fn intern_capability(&mut self, cap: Capability) -> Option<u32> {
        let mut td = bare_type(TypeKind::Capability);
        td.capability = Some(cap);
        let res = self.types.intern(td);
        self.type_result(res)
    }

    pub fn codegen_type(&mut self, ty_id: TyId, depth: u32) -> Option<u32> {
        if u64::from(depth) >= MO_LIMIT_CORE_DEPTH {
            self.fail(
                ErrorCode::NestedTooDeeply,
                "Core type nesting exceeds MO_LIMIT_CORE_DEPTH.",
            );
            return None;
        }
        let ty = self.arena.ty(ty_id);
        let is_scalar = matches!(
            ty,
            Ty::Int
                | Ty::BigInt
                | Ty::Byte
                | Ty::Decimal
                | Ty::Float
                | Ty::Bool
                | Ty::Char
                | Ty::Text
                | Ty::Bytes
                | Ty::Unit
                | Ty::Range
                | Ty::Time
                | Ty::Duration
        );
        if !is_scalar && let Some(&cached) = self.annotation_index.get(&ty_id) {
            return Some(cached);
        }
        let id = self.codegen_type_uncached(ty_id, depth)?;
        if !is_scalar {
            self.annotation_index.insert(ty_id, id);
        }
        Some(id)
    }

    fn codegen_type_uncached(&mut self, ty_id: TyId, depth: u32) -> Option<u32> {
        match self.arena.ty(ty_id) {
            Ty::Unit => self.scalar_type(TypeKind::Unit),
            Ty::Bool => self.scalar_type(TypeKind::Bool),
            Ty::Byte => self.scalar_type(TypeKind::Byte),
            Ty::Int => self.scalar_type(TypeKind::Int),
            Ty::Float => self.scalar_type(TypeKind::Float),
            Ty::Char => self.scalar_type(TypeKind::Char),
            Ty::Time => self.scalar_type(TypeKind::Time),
            Ty::Duration => self.scalar_type(TypeKind::Duration),
            Ty::Decimal => self.scalar_type(TypeKind::Decimal),
            Ty::Text => self.scalar_type(TypeKind::Text),
            Ty::Bytes => self.scalar_type(TypeKind::Bytes),
            Ty::BigInt => self.scalar_type(TypeKind::BigInt),
            Ty::Range => {
                self.range_int = self.scalar_type(TypeKind::Int)?;
                self.scalar_type(TypeKind::Range)
            }
            Ty::Tuple(items) => {
                if items.len() < 2 {
                    self.fail(
                        ErrorCode::CodegenRepresentation,
                        "Zero/one-element tuples cannot come from lowering.",
                    );
                    return None;
                }
                if (items.len() as u64) > MO_LIMIT_FIELDS {
                    self.fail(ErrorCode::BytecodeTooBig, "Tuple exceeds MO_LIMIT_FIELDS.");
                    return None;
                }
                let mut params = Vec::with_capacity(items.len());
                for &item in items {
                    params.push(self.codegen_type(item, depth + 1)?);
                }
                let mut td = bare_type(TypeKind::Tuple);
                td.parameters = params;
                let res = self.types.intern(td);
                self.type_result(res)
            }
            Ty::Data(name) => {
                if let Some(&id) = self.data_index.get(name.as_str()) {
                    Some(id)
                } else {
                    self.fail(
                        ErrorCode::CodegenRepresentation,
                        "Undeclared data types cannot come from lowering.",
                    );
                    None
                }
            }
            Ty::Option(inner) => {
                let a = self.codegen_type(*inner, depth + 1)?;
                self.builtin_choice_type(BUILTIN_OPTION, a, NO_INDEX)
            }
            Ty::Result { ok, err } => {
                let a = self.codegen_type(*ok, depth + 1)?;
                let b = self.codegen_type(*err, depth + 1)?;
                self.builtin_choice_type(BUILTIN_RESULT, a, b)
            }
            Ty::Ordering => self.builtin_choice_type(BUILTIN_ORDERING, NO_INDEX, NO_INDEX),
            Ty::FileError => self.builtin_choice_type(BUILTIN_FILE_ERROR, NO_INDEX, NO_INDEX),
            Ty::SimulationError => {
                self.builtin_choice_type(BUILTIN_SIMULATION_ERROR, NO_INDEX, NO_INDEX)
            }
            Ty::NetworkError => self.builtin_choice_type(BUILTIN_NETWORK_ERROR, NO_INDEX, NO_INDEX),
            Ty::SqliteValue => self.builtin_choice_type(BUILTIN_SQLITE_VALUE, NO_INDEX, NO_INDEX),
            Ty::SqliteError => self.builtin_choice_type(BUILTIN_SQLITE_ERROR, NO_INDEX, NO_INDEX),
            Ty::EntryKind => self.builtin_choice_type(BUILTIN_ENTRY_KIND, NO_INDEX, NO_INDEX),
            Ty::Entry => self.builtin_entry_type(),
            Ty::SqliteRows => self.builtin_sqlite_rows_type(),
            Ty::Fn { params, ret } => {
                if (params.len() as u64) > MO_LIMIT_PARAMETERS {
                    self.fail(
                        ErrorCode::BytecodeTooBig,
                        "Closure signature exceeds MO_LIMIT_PARAMETERS.",
                    );
                    return None;
                }
                let mut bc_params = Vec::with_capacity(params.len());
                for &p in params {
                    bc_params.push(self.codegen_type(p, depth + 1)?);
                }
                let result = self.codegen_type(*ret, depth + 1)?;
                let mut td = bare_type(TypeKind::Function);
                td.parameters = bc_params;
                td.result = result;
                let res = self.types.intern(td);
                self.type_result(res)
            }
            Ty::List(elem) => {
                let p = self.codegen_type(*elem, depth + 1)?;
                let mut td = bare_type(TypeKind::List);
                td.parameters = vec![p];
                let res = self.types.intern(td);
                self.type_result(res)
            }
            Ty::Set(elem) => {
                let p = self.codegen_type(*elem, depth + 1)?;
                let mut td = bare_type(TypeKind::Set);
                td.parameters = vec![p];
                let res = self.types.intern(td);
                self.type_result(res)
            }
            Ty::Map { key, value } => {
                let k = self.codegen_type(*key, depth + 1)?;
                let v = self.codegen_type(*value, depth + 1)?;
                let mut td = bare_type(TypeKind::Map);
                td.parameters = vec![k, v];
                let res = self.types.intern(td);
                self.type_result(res)
            }
            Ty::Simulation => self.intern_capability(Capability::Simulation),
            Ty::Cap(cap) => self.intern_capability(core_cap_to_bytecode(*cap)),
        }
    }

    fn lower_sig_type(&mut self, st: &SigTy) -> Option<u32> {
        match st {
            SigTy::Resolved(id) => Some(*id),
            SigTy::Scalar(TypeKind::Range) => {
                self.range_int = self.scalar_type(TypeKind::Int)?;
                self.scalar_type(TypeKind::Range)
            }
            SigTy::Scalar(k) => self.scalar_type(*k),
            SigTy::Tuple(items) => {
                let mut params = Vec::with_capacity(items.len());
                for item in items {
                    params.push(self.lower_sig_type(item)?);
                }
                let mut td = bare_type(TypeKind::Tuple);
                td.parameters = params;
                let res = self.types.intern(td);
                self.type_result(res)
            }
            SigTy::List(inner) => {
                let p = self.lower_sig_type(inner)?;
                let mut td = bare_type(TypeKind::List);
                td.parameters = vec![p];
                let res = self.types.intern(td);
                self.type_result(res)
            }
            SigTy::Set(inner) => {
                let p = self.lower_sig_type(inner)?;
                let mut td = bare_type(TypeKind::Set);
                td.parameters = vec![p];
                let res = self.types.intern(td);
                self.type_result(res)
            }
            SigTy::Map(k, v) => {
                let kt = self.lower_sig_type(k)?;
                let vt = self.lower_sig_type(v)?;
                let mut td = bare_type(TypeKind::Map);
                td.parameters = vec![kt, vt];
                let res = self.types.intern(td);
                self.type_result(res)
            }
            SigTy::Option(inner) => {
                let a = self.lower_sig_type(inner)?;
                self.builtin_choice_type(BUILTIN_OPTION, a, NO_INDEX)
            }
            SigTy::Result(ok, err) => {
                let a = self.lower_sig_type(ok)?;
                let b = self.lower_sig_type(err)?;
                self.builtin_choice_type(BUILTIN_RESULT, a, b)
            }
            SigTy::Ordering => self.builtin_choice_type(BUILTIN_ORDERING, NO_INDEX, NO_INDEX),
            SigTy::Entry => self.builtin_entry_type(),
            SigTy::FileError => self.builtin_choice_type(BUILTIN_FILE_ERROR, NO_INDEX, NO_INDEX),
            SigTy::NetworkError => {
                self.builtin_choice_type(BUILTIN_NETWORK_ERROR, NO_INDEX, NO_INDEX)
            }
            SigTy::SqliteValue => {
                self.builtin_choice_type(BUILTIN_SQLITE_VALUE, NO_INDEX, NO_INDEX)
            }
            SigTy::SqliteRows => self.builtin_sqlite_rows_type(),
            SigTy::SqliteError => {
                self.builtin_choice_type(BUILTIN_SQLITE_ERROR, NO_INDEX, NO_INDEX)
            }
            SigTy::Cap(cap) => self.intern_capability(core_cap_to_bytecode(*cap)),
        }
    }

    pub fn simulation_signature(&mut self, sim: &SimOp) -> Option<u32> {
        let sim_ty = self.intern_capability(Capability::Simulation)?;
        let (parameters, result) = match sim {
            SimOp::Start { .. } => {
                let time_ty = self.scalar_type(TypeKind::Time)?;
                (vec![time_ty], sim_ty)
            }
            SimOp::Send {
                arguments, result, ..
            } => {
                if (arguments.len() as u64) >= MO_LIMIT_BYTECODE_PARAMETERS {
                    self.fail(
                        ErrorCode::BytecodeTooBig,
                        "Simulation send exceeds MO_LIMIT_BYTECODE_PARAMETERS.",
                    );
                    return None;
                }
                let mut params = Vec::with_capacity(1 + arguments.len());
                params.push(sim_ty);
                for &arg in arguments {
                    params.push(self.codegen_type(arg, 0)?);
                }
                let ok_ty = self.codegen_type(*result, 0)?;
                let err_ty =
                    self.builtin_choice_type(BUILTIN_SIMULATION_ERROR, NO_INDEX, NO_INDEX)?;
                let res_ty = self.builtin_choice_type(BUILTIN_RESULT, ok_ty, err_ty)?;
                (params, res_ty)
            }
            SimOp::Advance => {
                let dur_ty = self.scalar_type(TypeKind::Duration)?;
                let unit_ty = self.scalar_type(TypeKind::Unit)?;
                (vec![sim_ty, dur_ty], unit_ty)
            }
            SimOp::Crash | SimOp::Restart => {
                let unit_ty = self.scalar_type(TypeKind::Unit)?;
                (vec![sim_ty], unit_ty)
            }
        };
        let mut td = bare_type(TypeKind::Function);
        td.parameters = parameters;
        td.result = result;
        let res = self.types.intern(td);
        self.type_result(res)
    }

    pub fn operation_signature(
        &mut self,
        op: &Operation,
        variables: [Option<TyId>; 4],
    ) -> Option<u32> {
        let mut resolved = [None; 4];
        let mut key = [
            op.opcode.map_or(0, |o| u32::from(o.number())),
            op.row,
            NO_INDEX,
            NO_INDEX,
            NO_INDEX,
            NO_INDEX,
            op.expansion as u32,
        ];
        for i in 0..4 {
            if let Some(ty_id) = variables[i] {
                let bc_ty = self.codegen_type(ty_id, 0)?;
                resolved[i] = Some(bc_ty);
                key[i + 2] = bc_ty;
            }
        }
        if let Some(&old) = self.operation_index.get(&key) {
            return Some(old);
        }

        let mut cursor = op.signature;
        let mut params = Vec::new();
        let is_fn = if let Some(rest) = cursor.strip_prefix("fn(") {
            cursor = rest;
            if !take_char(&mut cursor, ')') {
                loop {
                    let st = parse_sig_type(&mut cursor, &resolved, 0)?;
                    params.push(self.lower_sig_type(&st)?);
                    if !take_char(&mut cursor, ',') {
                        break;
                    }
                }
                if !take_char(&mut cursor, ')') {
                    return None;
                }
            }
            true
        } else {
            false
        };

        let result = if !is_fn || take_char(&mut cursor, ':') {
            let st = parse_sig_type(&mut cursor, &resolved, 0)?;
            self.lower_sig_type(&st)?
        } else {
            self.scalar_type(TypeKind::Unit)?
        };

        let mut td = bare_type(TypeKind::Function);
        td.parameters = params;
        td.result = result;
        let id = self.intern_type(td)?;
        self.operation_index.insert(key, id);

        cursor = cursor.trim_start_matches(' ');
        let mut constrained = NO_INDEX;
        let mut ability = 0u8;
        if op.opcode == Some(Opcode::Equal) {
            constrained = key[2];
            ability = ABILITY_EQUAL;
        } else if let Some(rest) = cursor.strip_prefix("where ") {
            let mut chars = rest.chars();
            let slot = match chars.next() {
                Some('T') => 0,
                Some('U') => 1,
                Some('K') => 2,
                Some('V') => 3,
                _ => {
                    self.fail(ErrorCode::CodegenInvalid, "malformed signature constraint");
                    return None;
                }
            };
            constrained = key[2 + slot];
            let tail = &rest[1..];
            ability = match tail {
                ": Equal" => ABILITY_EQUAL,
                ": Hash" => ABILITY_HASH,
                _ => {
                    self.fail(ErrorCode::CodegenInvalid, "malformed signature constraint");
                    return None;
                }
            };
        }

        if ability != 0 && !self.push_constraint(constrained, ability) {
            return None;
        }
        Some(id)
    }

    pub fn intern_constant(&mut self, value: ConstantDef) -> Option<u32> {
        let key = ConstantKey::from_def(&value);
        if let Some(&old) = self.constant_index.get(&key) {
            return Some(old);
        }
        if (self.constants.len() as u64) >= MO_LIMIT_CONSTANTS {
            self.fail(
                ErrorCode::BytecodeTooBig,
                "Code generation exceeds the corresponding bytecode table or function limit.",
            );
            return None;
        }
        let id = self.constants.len() as u32;
        self.constants.push(value);
        self.constant_index.insert(key, id);
        Some(id)
    }

    fn parse_integer_bits(int: &CoreInteger) -> u128 {
        let mut bits = 0u128;
        for &b in int.digits.as_bytes() {
            bits = bits.wrapping_mul(10).wrapping_add(u128::from(b - b'0'));
        }
        if int.negative {
            0u128.wrapping_sub(bits)
        } else {
            bits
        }
    }

    pub fn intern_literal(&mut self, lit: &Lit) -> Option<(u32, u32)> {
        let kind = match lit {
            Lit::Int(_) => TypeKind::Int,
            Lit::BigInt(_) => TypeKind::BigInt,
            Lit::Byte(_) => TypeKind::Byte,
            Lit::Decimal(_) => TypeKind::Decimal,
            Lit::Float(_) => TypeKind::Float,
            Lit::Bool(_) => TypeKind::Bool,
            Lit::Char(_) => TypeKind::Char,
            Lit::Text(_) => TypeKind::Text,
            Lit::Bytes(_) => TypeKind::Bytes,
            Lit::Unit => TypeKind::Unit,
            Lit::Time(_) => TypeKind::Time,
            Lit::Duration(_) => TypeKind::Duration,
        };
        let ty = self.scalar_type(kind)?;

        match lit {
            Lit::BigInt(int) => {
                let sign_slot = usize::from(int.negative);
                if let Some(&old) = self.bigint_index[sign_slot].get(&int.digits) {
                    return Some((ty, old));
                }
                let Ok((sign, magnitude)) = convert_bigint(int, &mut self.bigint_work) else {
                    self.fail(
                        ErrorCode::CodegenBigintWork,
                        "BigInt conversion exceeds MO_LIMIT_CODEGEN_BIGINT_WORK.",
                    );
                    return None;
                };
                let id = self.intern_constant(ConstantDef {
                    ty,
                    value: ConstantValue::BigInt { sign, magnitude },
                })?;
                self.bigint_index[sign_slot].insert(int.digits.clone(), id);
                Some((ty, id))
            }
            Lit::Text(s) => {
                let sid = self.intern_string(s)?;
                let id = self.intern_constant(ConstantDef {
                    ty,
                    value: ConstantValue::Text(sid),
                })?;
                Some((ty, id))
            }
            Lit::Bytes(items) => {
                if (items.len() as u64) > MO_LIMIT_STRING_SIZE {
                    self.fail(
                        ErrorCode::BytecodeTooBig,
                        "Bytes literal exceeds MO_LIMIT_STRING_SIZE.",
                    );
                    return None;
                }
                let mut payload = Vec::with_capacity(items.len());
                for item in items {
                    let mut byte = 0u32;
                    for &b in item.digits.as_bytes() {
                        byte = byte * 10 + u32::from(b - b'0');
                    }
                    payload.push(byte as u8);
                }
                let id = self.intern_constant(ConstantDef {
                    ty,
                    value: ConstantValue::Bytes(payload),
                })?;
                Some((ty, id))
            }
            Lit::Decimal(int) => {
                let bits = Self::parse_integer_bits(int);
                let id = self.intern_constant(ConstantDef {
                    ty,
                    value: ConstantValue::Decimal128(bits.to_le_bytes()),
                })?;
                Some((ty, id))
            }
            Lit::Int(int) | Lit::Byte(int) | Lit::Time(int) | Lit::Duration(int) => {
                let bits = Self::parse_integer_bits(int) as u64;
                let id = self.intern_constant(ConstantDef {
                    ty,
                    value: ConstantValue::Immediate64(bits.to_le_bytes()),
                })?;
                Some((ty, id))
            }
            Lit::Float(bits) => {
                let id = self.intern_constant(ConstantDef {
                    ty,
                    value: ConstantValue::Immediate64(bits.to_le_bytes()),
                })?;
                Some((ty, id))
            }
            Lit::Bool(b) => {
                let id = self.intern_constant(ConstantDef {
                    ty,
                    value: ConstantValue::Immediate64(u64::from(*b).to_le_bytes()),
                })?;
                Some((ty, id))
            }
            Lit::Char(ch) => {
                let id = self.intern_constant(ConstantDef {
                    ty,
                    value: ConstantValue::Immediate64(u64::from(u32::from(*ch)).to_le_bytes()),
                })?;
                Some((ty, id))
            }
            Lit::Unit => {
                let id = self.intern_constant(ConstantDef {
                    ty,
                    value: ConstantValue::Immediate64([0u8; 8]),
                })?;
                Some((ty, id))
            }
        }
    }

    pub fn lookup_constructor(&mut self, ty: u32, key: &str) -> Option<u32> {
        if self.types.items[ty as usize].kind == TypeKind::Record {
            debug_assert!(key == "Entry" || key == "Rows");
            return Some(0);
        }
        if self.constructors.len() < self.types.items.len() {
            if (self.types.items.len() as u64) > MO_LIMIT_TYPES {
                self.fail(
                    ErrorCode::BytecodeTooBig,
                    "Code generation exceeds the corresponding bytecode table or function limit.",
                );
                return None;
            }
            self.constructors
                .resize_with(self.types.items.len(), HashMap::new);
        }
        if self.constructors[ty as usize].is_empty() {
            let map = &mut self.constructors[ty as usize];
            for (i, case) in self.types.items[ty as usize].cases.iter().enumerate() {
                map.entry(case.name.clone()).or_insert(i as u32);
            }
        }
        let idx = self.constructors[ty as usize].get(key).copied();
        debug_assert!(idx.is_some(), "missing constructor {key:?} on type {ty}");
        idx
    }
}
