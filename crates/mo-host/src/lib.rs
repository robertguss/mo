#![forbid(unsafe_code)]

//! Capability abstraction and real/fake implementations for Slice 1A (`Args`, `Env`, `Console`).

use std::cell::RefCell;
use std::collections::VecDeque;
use std::io::{self, BufRead, BufReader, Read, Write};
use std::rc::Rc;

use mo_bytecode::types::TypeTable;
use mo_bytecode::{Capability, TypeKind};
use mo_def::HostOpId;
use mo_def::limits::MO_LIMIT_VALUE_BYTES;
use mo_heap::table::MoMap;
use mo_heap::{Heap, HeapError, Value};

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
    Unimplemented(HostOpId),
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

/// Real I/O handles and OS context for a process.
pub struct HostContext {
    pub args: Vec<String>,
    pub env: Vec<(String, String)>,
    input: Box<dyn BufRead>,
    output: Box<dyn Write>,
    errors: Box<dyn Write>,
}

impl HostContext {
    pub fn stdio(args: Vec<String>, env: Vec<(String, String)>) -> Self {
        Self {
            args,
            env,
            input: Box::new(BufReader::new(io::stdin())),
            output: Box::new(io::stdout()),
            errors: Box::new(io::stderr()),
        }
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
        }
    }

    pub fn make_real_capability(
        &mut self,
        heap: &Heap,
        types: &TypeTable,
        ty: u32,
    ) -> Result<Value, HostError> {
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
                let state = args[0]
                    .as_capability()
                    .inner
                    .downcast_ref::<ArgsState>()
                    .expect("Args capability");
                Ok(state.list.clone())
            }
            HostOpId::ArgsOf => {
                let state = ArgsState {
                    list: args[0].clone(),
                };
                Ok(heap.alloc_capability(Capability::Args, result_ty, Rc::new(state))?)
            }
            HostOpId::EnvGet => {
                let state = args[0]
                    .as_capability()
                    .inner
                    .downcast_ref::<EnvState>()
                    .expect("Env capability");
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
                let state = args[0]
                    .as_capability()
                    .inner
                    .downcast_ref::<ConsoleState>()
                    .expect("Console capability");
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
                let state = args[0]
                    .as_capability()
                    .inner
                    .downcast_ref::<ConsoleState>()
                    .expect("Console capability");
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
                let state = args[0]
                    .as_capability()
                    .inner
                    .downcast_ref::<ConsoleState>()
                    .expect("Console capability");
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
            other => Err(HostError::Unimplemented(other)),
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
            Err(e) if e.kind() == io::ErrorKind::Interrupted => {
                // Retry once on EINTR loop.
                loop {
                    match self.input.read_until(b'\n', &mut raw) {
                        Ok(n) => break n,
                        Err(err) if err.kind() == io::ErrorKind::Interrupted => continue,
                        Err(_) => {
                            return Err(HostError::Internal {
                                reason: "internal error: console input unavailable",
                            });
                        }
                    }
                }
            }
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
    use mo_bytecode::{NO_INDEX, TypeCase, TypeDef, TypeField};

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

    fn host_types() -> TypeTable {
        let text = base_type(TypeKind::Text); // 0
        let mut list_text = base_type(TypeKind::List); // 1
        list_text.parameters = vec![0];
        let mut opt_text = base_type(TypeKind::Choice); // 2
        opt_text.name = Some("Option".to_string());
        opt_text.cases = vec![
            TypeCase {
                name_idx: 0,
                name: "None".to_string(),
                fields: vec![],
                unnamed: false,
            },
            TypeCase {
                name_idx: 1,
                name: "Some".to_string(),
                fields: vec![TypeField {
                    name_idx: None,
                    name: None,
                    ty: 0,
                }],
                unnamed: true,
            },
        ];
        let mut map_tt = base_type(TypeKind::Map); // 3
        map_tt.parameters = vec![0, 0];
        let args_cap = cap_type(Capability::Args); // 4
        let env_cap = cap_type(Capability::Env); // 5
        let console_cap = cap_type(Capability::Console); // 6
        let files_cap = cap_type(Capability::Files); // 7
        TypeTable::finish(
            vec![
                text,
                list_text,
                opt_text,
                map_tt,
                args_cap,
                env_cap,
                console_cap,
                files_cap,
            ],
            Vec::new(),
        )
        .unwrap()
    }

    #[test]
    fn real_and_fake_args_and_env() {
        let types = host_types();
        let heap = Heap::new(1024 * 1024);
        let mut ctx = HostContext::with_io(
            vec!["alpha".to_string(), "beta".to_string()],
            vec![("KEY".to_string(), "VAL".to_string())],
            io::empty(),
            Vec::new(),
            Vec::new(),
        );

        let args_cap = ctx.make_real_capability(&heap, &types, 4).unwrap();
        let all = ctx
            .call(&heap, &types, HostOpId::ArgsList, 1, &[args_cap])
            .unwrap();
        let elems: Vec<&str> = all.as_list().iter().map(Value::as_text).collect();
        assert_eq!(elems, vec!["alpha", "beta"]);

        let fake_list = heap
            .alloc_list(VecDeque::from([heap.alloc_text("x".into()).unwrap()]))
            .unwrap();
        let fake_args = ctx
            .call(&heap, &types, HostOpId::ArgsOf, 4, &[fake_list])
            .unwrap();
        let fake_all = ctx
            .call(&heap, &types, HostOpId::ArgsList, 1, &[fake_args])
            .unwrap();
        assert_eq!(fake_all.as_list()[0].as_text(), "x");

        let env_cap = ctx.make_real_capability(&heap, &types, 5).unwrap();
        let k_hit = heap.alloc_text("KEY".into()).unwrap();
        let k_miss = heap.alloc_text("MISSING".into()).unwrap();
        let got = ctx
            .call(
                &heap,
                &types,
                HostOpId::EnvGet,
                2,
                &[env_cap.clone(), k_hit.clone()],
            )
            .unwrap();
        assert_eq!(got.as_tag(), 1);
        let inner = heap.extract_case_field(&types, 2, &got, 1, 0);
        assert_eq!(inner.as_text(), "VAL");

        let miss = ctx
            .call(&heap, &types, HostOpId::EnvGet, 2, &[env_cap, k_miss])
            .unwrap();
        assert_eq!(miss.as_tag(), 0);

        let mut fake_map = MoMap::new(0, 0);
        fake_map.set(
            &types,
            heap.hash_key(),
            k_hit.clone(),
            heap.alloc_text("FAKE".into()).unwrap(),
        );
        let fake_map_val = heap.alloc_map(fake_map).unwrap();
        let fake_env = ctx
            .call(&heap, &types, HostOpId::EnvOf, 5, &[fake_map_val])
            .unwrap();
        let got_fake = ctx
            .call(&heap, &types, HostOpId::EnvGet, 2, &[fake_env, k_hit])
            .unwrap();
        assert_eq!(
            heap.extract_case_field(&types, 2, &got_fake, 1, 0)
                .as_text(),
            "FAKE"
        );
    }

    #[test]
    fn real_and_fake_console_and_unimplemented_errors() {
        let types = host_types();
        let heap = Heap::new(1024 * 1024);
        let mut ctx = HostContext::with_io(
            Vec::new(),
            Vec::new(),
            io::Cursor::new(b"first\r\nsecond".to_vec()),
            Vec::new(),
            Vec::new(),
        );

        let real_console = ctx.make_real_capability(&heap, &types, 6).unwrap();
        let l1 = ctx
            .call(
                &heap,
                &types,
                HostOpId::ConsoleReadLine,
                2,
                std::slice::from_ref(&real_console),
            )
            .unwrap();
        assert_eq!(
            heap.extract_case_field(&types, 2, &l1, 1, 0).as_text(),
            "first"
        );
        let l2 = ctx
            .call(
                &heap,
                &types,
                HostOpId::ConsoleReadLine,
                2,
                std::slice::from_ref(&real_console),
            )
            .unwrap();
        assert_eq!(
            heap.extract_case_field(&types, 2, &l2, 1, 0).as_text(),
            "second"
        );
        let eof = ctx
            .call(
                &heap,
                &types,
                HostOpId::ConsoleReadLine,
                2,
                std::slice::from_ref(&real_console),
            )
            .unwrap();
        assert_eq!(eof.as_tag(), 0);

        let err = ctx
            .call(&heap, &types, HostOpId::ConsolePrinted, 1, &[real_console])
            .err()
            .unwrap();
        assert_eq!(
            err,
            HostError::Bug {
                reason: "console observation needs a fake",
                detail: None
            }
        );

        let in_list = heap
            .alloc_list(VecDeque::from([heap.alloc_text("hello".into()).unwrap()]))
            .unwrap();
        let fake_console = ctx
            .call(&heap, &types, HostOpId::ConsoleFakeInput, 6, &[in_list])
            .unwrap();
        let msg = heap.alloc_text("out".into()).unwrap();
        let errmsg = heap.alloc_text("err".into()).unwrap();
        ctx.call(
            &heap,
            &types,
            HostOpId::ConsolePrint,
            0,
            &[fake_console.clone(), msg],
        )
        .unwrap();
        ctx.call(
            &heap,
            &types,
            HostOpId::ConsoleError,
            0,
            &[fake_console.clone(), errmsg],
        )
        .unwrap();
        let printed = ctx
            .call(
                &heap,
                &types,
                HostOpId::ConsolePrinted,
                1,
                std::slice::from_ref(&fake_console),
            )
            .unwrap();
        assert_eq!(printed.as_list()[0].as_text(), "out");
        let printed_err = ctx
            .call(
                &heap,
                &types,
                HostOpId::ConsolePrintedErrors,
                1,
                std::slice::from_ref(&fake_console),
            )
            .unwrap();
        assert_eq!(printed_err.as_list()[0].as_text(), "err");
        let read = ctx
            .call(&heap, &types, HostOpId::ConsoleReadLine, 2, &[fake_console])
            .unwrap();
        assert_eq!(
            heap.extract_case_field(&types, 2, &read, 1, 0).as_text(),
            "hello"
        );

        assert_eq!(
            ctx.make_real_capability(&heap, &types, 7).err().unwrap(),
            HostError::UnimplementedCapability(Capability::Files)
        );
        assert_eq!(
            ctx.call(&heap, &types, HostOpId::ClockNow, 0, &[])
                .err()
                .unwrap(),
            HostError::Unimplemented(HostOpId::ClockNow)
        );
    }
}
