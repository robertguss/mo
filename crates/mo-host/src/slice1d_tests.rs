use std::collections::VecDeque;
use std::io::{Read, Write};
use std::net::TcpListener;
use std::time::Duration;

use mo_bytecode::types::TypeTable;
use mo_bytecode::{
    Capability, LayoutDef, LayoutKind, NO_INDEX, TypeCase, TypeDef, TypeField, TypeKind,
};
use mo_def::HostOpId;
use mo_def::limits::{
    MO_LIMIT_COMMAND_ARGS, MO_LIMIT_COMMAND_DURATION_US, MO_LIMIT_OPEN_CONNECTIONS,
    MO_LIMIT_OPEN_DATABASES, MO_LIMIT_RECEIVE_BYTES,
};
use mo_heap::print::value_print;
use mo_heap::table::MoMap;
use mo_heap::{Heap, Value};

use crate::{HostContext, HostError};

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

fn make_result_type(ok_ty: u32, err_ty: u32) -> TypeDef {
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
                ty: err_ty,
            }],
            unnamed: true,
        },
    ];
    r
}

struct Slice1dFixture {
    types: TypeTable,
    text: u32,
    bytes: u32,
    list_text: u32,
    map_bb: u32,
    map_t_map_bb: u32,
    net_cap: u32,
    res_conn: u32,
    res_net_unit: u32,
    res_net_bytes: u32,
    sqlite_cap: u32,
    sqlite_val: u32,
    res_db: u32,
    res_sql_unit: u32,
    res_sql_raw: u32,
    res_sql_rows: u32,
    cmd_cap: u32,
    res_cmd_raw: u32,
}

impl Slice1dFixture {
    fn new() -> Self {
        let mut items = Vec::new();
        let mut layouts = Vec::new();
        let mut push = |t: TypeDef| -> u32 {
            let id = items.len() as u32;
            items.push(t);
            id
        };

        let unit = push(base_type(TypeKind::Unit));
        let int = push(base_type(TypeKind::Int));
        let float = push(base_type(TypeKind::Float));
        let text = push(base_type(TypeKind::Text));
        let bytes = push(base_type(TypeKind::Bytes));

        let mut list_text_def = base_type(TypeKind::List);
        list_text_def.parameters = vec![text];
        let list_text = push(list_text_def);

        let mut map_bb_def = base_type(TypeKind::Map);
        map_bb_def.parameters = vec![bytes, bytes];
        let map_bb = push(map_bb_def);

        let mut map_t_map_bb_def = base_type(TypeKind::Map);
        map_t_map_bb_def.parameters = vec![text, map_bb];
        let map_t_map_bb = push(map_t_map_bb_def);

        // Network types
        let net_cap = push(cap_type(Capability::Network));
        let conn_cap = push(cap_type(Capability::Connection));

        let mut net_err_def = base_type(TypeKind::Choice);
        net_err_def.name = Some("NetworkError".into());
        net_err_def.has_fields = true;
        net_err_def.cases = vec![
            TypeCase {
                name_idx: 0,
                name: "TimedOut".into(),
                fields: vec![],
                unnamed: false,
            },
            TypeCase {
                name_idx: 1,
                name: "Closed".into(),
                fields: vec![],
                unnamed: false,
            },
            TypeCase {
                name_idx: 2,
                name: "Refused".into(),
                fields: vec![
                    TypeField {
                        name_idx: None,
                        name: Some("host".into()),
                        ty: text,
                    },
                    TypeField {
                        name_idx: None,
                        name: Some("port".into()),
                        ty: int,
                    },
                ],
                unnamed: false,
            },
            TypeCase {
                name_idx: 3,
                name: "UnknownHost".into(),
                fields: vec![TypeField {
                    name_idx: None,
                    name: Some("host".into()),
                    ty: text,
                }],
                unnamed: false,
            },
            TypeCase {
                name_idx: 4,
                name: "Broken".into(),
                fields: vec![TypeField {
                    name_idx: None,
                    name: Some("reason".into()),
                    ty: text,
                }],
                unnamed: false,
            },
            TypeCase {
                name_idx: 5,
                name: "Certificate".into(),
                fields: vec![TypeField {
                    name_idx: None,
                    name: Some("reason".into()),
                    ty: text,
                }],
                unnamed: false,
            },
        ];
        let net_err = push(net_err_def);
        layouts.push(LayoutDef {
            kind: LayoutKind::Case,
            ty: net_err,
            case_index: 2,
            function: NO_INDEX,
            fields: vec![text, int],
        });

        let res_conn = push(make_result_type(conn_cap, net_err));
        let res_net_unit = push(make_result_type(unit, net_err));
        let res_net_bytes = push(make_result_type(bytes, net_err));
        for rty in [res_conn, res_net_unit, res_net_bytes] {
            layouts.push(LayoutDef {
                kind: LayoutKind::Case,
                ty: rty,
                case_index: 1,
                function: NO_INDEX,
                fields: vec![net_err],
            });
        }

        // Sqlite types
        let sqlite_cap = push(cap_type(Capability::Sqlite));
        let db_cap = push(cap_type(Capability::Database));

        let mut sql_err_def = base_type(TypeKind::Choice);
        sql_err_def.name = Some("SqliteError".into());
        sql_err_def.has_fields = true;
        sql_err_def.cases = vec![
            TypeCase {
                name_idx: 0,
                name: "OutsideFolder".into(),
                fields: vec![TypeField {
                    name_idx: None,
                    name: Some("path".into()),
                    ty: text,
                }],
                unnamed: false,
            },
            TypeCase {
                name_idx: 1,
                name: "NotPermitted".into(),
                fields: vec![TypeField {
                    name_idx: None,
                    name: Some("reason".into()),
                    ty: text,
                }],
                unnamed: false,
            },
            TypeCase {
                name_idx: 2,
                name: "SqlFailed".into(),
                fields: vec![TypeField {
                    name_idx: None,
                    name: Some("reason".into()),
                    ty: text,
                }],
                unnamed: false,
            },
        ];
        let sql_err = push(sql_err_def);

        let mut sql_val_def = base_type(TypeKind::Choice);
        sql_val_def.name = Some("Sqlite.Value".into());
        sql_val_def.has_fields = true;
        sql_val_def.cases = vec![
            TypeCase {
                name_idx: 0,
                name: "Null".into(),
                fields: vec![],
                unnamed: false,
            },
            TypeCase {
                name_idx: 1,
                name: "Integer".into(),
                fields: vec![TypeField {
                    name_idx: None,
                    name: None,
                    ty: int,
                }],
                unnamed: true,
            },
            TypeCase {
                name_idx: 2,
                name: "Real".into(),
                fields: vec![TypeField {
                    name_idx: None,
                    name: None,
                    ty: float,
                }],
                unnamed: true,
            },
            TypeCase {
                name_idx: 3,
                name: "String".into(),
                fields: vec![TypeField {
                    name_idx: None,
                    name: None,
                    ty: text,
                }],
                unnamed: true,
            },
            TypeCase {
                name_idx: 4,
                name: "Blob".into(),
                fields: vec![TypeField {
                    name_idx: None,
                    name: None,
                    ty: bytes,
                }],
                unnamed: true,
            },
        ];
        let sqlite_val = push(sql_val_def);

        let mut list_val_def = base_type(TypeKind::List);
        list_val_def.parameters = vec![sqlite_val];
        let list_val = push(list_val_def);

        let mut list_list_val_def = base_type(TypeKind::List);
        list_list_val_def.parameters = vec![list_val];
        let list_list_val = push(list_list_val_def);

        let mut sql_rows_def = base_type(TypeKind::Record);
        sql_rows_def.name = Some("Sqlite.Rows".into());
        sql_rows_def.fields = vec![
            TypeField {
                name_idx: None,
                name: Some("columns".into()),
                ty: list_text,
            },
            TypeField {
                name_idx: None,
                name: Some("rows".into()),
                ty: list_list_val,
            },
        ];
        let sql_rows = push(sql_rows_def);
        layouts.push(LayoutDef {
            kind: LayoutKind::Record,
            ty: sql_rows,
            case_index: 0,
            function: NO_INDEX,
            fields: vec![list_text, list_list_val],
        });

        let mut raw_tup_def = base_type(TypeKind::Tuple);
        raw_tup_def.parameters = vec![int, int];
        let raw_tup = push(raw_tup_def);
        layouts.push(LayoutDef {
            kind: LayoutKind::Tuple,
            ty: raw_tup,
            case_index: 0,
            function: NO_INDEX,
            fields: vec![int, int],
        });

        let res_db = push(make_result_type(db_cap, sql_err));
        let res_sql_unit = push(make_result_type(unit, sql_err));
        let res_sql_raw = push(make_result_type(raw_tup, sql_err));
        let res_sql_rows = push(make_result_type(sql_rows, sql_err));
        for rty in [res_db, res_sql_unit, res_sql_raw, res_sql_rows] {
            layouts.push(LayoutDef {
                kind: LayoutKind::Case,
                ty: rty,
                case_index: 1,
                function: NO_INDEX,
                fields: vec![sql_err],
            });
        }

        // Command types
        let cmd_cap = push(cap_type(Capability::Command));
        let mut cmd_tup_def = base_type(TypeKind::Tuple);
        cmd_tup_def.parameters = vec![int, bytes, bytes];
        let cmd_tup = push(cmd_tup_def);
        layouts.push(LayoutDef {
            kind: LayoutKind::Tuple,
            ty: cmd_tup,
            case_index: 0,
            function: NO_INDEX,
            fields: vec![int, bytes, bytes],
        });
        let res_cmd_raw = push(make_result_type(cmd_tup, text));

        let types = TypeTable::finish(items, layouts).unwrap();
        Self {
            types,
            text,
            bytes,
            list_text,
            map_bb,
            map_t_map_bb,
            net_cap,
            res_conn,
            res_net_unit,
            res_net_bytes,
            sqlite_cap,
            sqlite_val,
            res_db,
            res_sql_unit,
            res_sql_raw,
            res_sql_rows,
            cmd_cap,
            res_cmd_raw,
        }
    }
}

fn text_list(heap: &Heap, items: &[&str]) -> Value {
    let mut dq = VecDeque::new();
    for s in items {
        dq.push_back(heap.alloc_text((*s).into()).unwrap());
    }
    heap.alloc_list(dq).unwrap()
}

fn make_temp_dir(label: &str) -> std::path::PathBuf {
    let mut rng = [0u8; 8];
    getrandom::fill(&mut rng).unwrap();
    let dir =
        std::env::temp_dir().join(format!("mo-host-{label}-{:016x}", u64::from_le_bytes(rng)));
    std::fs::create_dir_all(&dir).unwrap();
    dir
}

#[test]
fn network_real_tcp_allowlist_limits_and_fake_parity() {
    let fx = Slice1dFixture::new();
    let heap = Heap::new(16 << 20);
    let mut ctx = HostContext::stdio(Vec::new(), Vec::new());

    let net = ctx
        .make_real_capability(&heap, &fx.types, fx.net_cap)
        .unwrap();

    // 1. Bug checks: port out of bounds.
    let h_local = heap.alloc_text("127.0.0.1".into()).unwrap();
    for bad_port in [0i64, -1, 65536] {
        assert_eq!(
            ctx.call(
                &heap,
                &fx.types,
                HostOpId::NetworkConnect,
                fx.res_conn,
                &[
                    net.clone(),
                    h_local.clone(),
                    Value::Int(bad_port),
                    Value::Duration(1_000_000)
                ],
            )
            .err(),
            Some(HostError::Bug {
                reason: "port outside 1 to 65535",
                detail: None,
            })
        );
    }

    // 2. Real loopback echo server + chunked receive + EOF + receive bounds.
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let port = i64::from(listener.local_addr().unwrap().port());
    let srv = std::thread::spawn(move || {
        let (mut sock, _) = listener.accept().unwrap();
        let mut buf = [0u8; 32];
        let n = sock.read(&mut buf).unwrap();
        sock.write_all(&buf[..n]).unwrap();
        sock.flush().unwrap();
    });

    let connected = ctx
        .call(
            &heap,
            &fx.types,
            HostOpId::NetworkConnect,
            fx.res_conn,
            &[
                net.clone(),
                h_local.clone(),
                Value::Int(port),
                Value::Duration(5_000_000),
            ],
        )
        .unwrap();
    assert_eq!(connected.as_tag(), 0);
    assert_eq!(ctx.open_connections(), 1);
    let conn = heap.extract_case_field(&fx.types, fx.res_conn, &connected, 0, 0);

    // Receive bug bounds.
    for bad_most in [0i64, -5, (MO_LIMIT_RECEIVE_BYTES as i64) + 1] {
        assert_eq!(
            ctx.call(
                &heap,
                &fx.types,
                HostOpId::ConnectionReceive,
                fx.res_net_bytes,
                &[conn.clone(), Value::Int(bad_most)],
            )
            .err(),
            Some(HostError::Bug {
                reason: "most below 1 or above the receive limit",
                detail: None,
            })
        );
    }

    let payload = heap.alloc_bytes(b"hello!".to_vec()).unwrap();
    let sent = ctx
        .call(
            &heap,
            &fx.types,
            HostOpId::ConnectionSend,
            fx.res_net_unit,
            &[conn.clone(), payload],
        )
        .unwrap();
    assert_eq!(value_print(&fx.types, fx.res_net_unit, &sent), "Ok(Unit)");

    let chunk1 = ctx
        .call(
            &heap,
            &fx.types,
            HostOpId::ConnectionReceive,
            fx.res_net_bytes,
            &[conn.clone(), Value::Int(4)],
        )
        .unwrap();
    assert_eq!(
        heap.extract_case_field(&fx.types, fx.res_net_bytes, &chunk1, 0, 0)
            .as_bytes(),
        b"hell"
    );
    let chunk2 = ctx
        .call(
            &heap,
            &fx.types,
            HostOpId::ConnectionReceive,
            fx.res_net_bytes,
            &[conn.clone(), Value::Int(16)],
        )
        .unwrap();
    assert_eq!(
        heap.extract_case_field(&fx.types, fx.res_net_bytes, &chunk2, 0, 0)
            .as_bytes(),
        b"o!"
    );
    let eof = ctx
        .call(
            &heap,
            &fx.types,
            HostOpId::ConnectionReceive,
            fx.res_net_bytes,
            &[conn.clone(), Value::Int(16)],
        )
        .unwrap();
    assert!(
        heap.extract_case_field(&fx.types, fx.res_net_bytes, &eof, 0, 0)
            .as_bytes()
            .is_empty()
    );
    drop(conn);
    drop(connected);
    assert_eq!(ctx.open_connections(), 0);
    srv.join().unwrap();

    // 3. Monotonic allow_hosts intersection.
    let net_a = ctx
        .call(
            &heap,
            &fx.types,
            HostOpId::NetworkAllowHosts,
            fx.net_cap,
            &[
                net.clone(),
                text_list(&heap, &["127.0.0.1:443", "Example.COM", "bad:99999"]),
            ],
        )
        .unwrap();
    let net_b = ctx
        .call(
            &heap,
            &fx.types,
            HostOpId::NetworkAllowHosts,
            fx.net_cap,
            &[
                net_a,
                text_list(&heap, &["127.0.0.1", "example.com:8443", "other.test"]),
            ],
        )
        .unwrap();
    // 127.0.0.1 was already pinned to port 443 -> port 80 must be Refused.
    let ref1 = ctx
        .call(
            &heap,
            &fx.types,
            HostOpId::NetworkConnect,
            fx.res_conn,
            &[
                net_b.clone(),
                h_local.clone(),
                Value::Int(80),
                Value::Duration(100_000),
            ],
        )
        .unwrap();
    assert_eq!(
        value_print(&fx.types, fx.res_conn, &ref1),
        "Error(Refused(host: \"127.0.0.1\", port: 80))"
    );
    // other.test was not in net_a -> Refused.
    let h_other = heap.alloc_text("other.test".into()).unwrap();
    let ref2 = ctx
        .call(
            &heap,
            &fx.types,
            HostOpId::NetworkConnect,
            fx.res_conn,
            &[net_b, h_other, Value::Int(443), Value::Duration(100_000)],
        )
        .unwrap();
    assert_eq!(
        value_print(&fx.types, fx.res_conn, &ref2),
        "Error(Refused(host: \"other.test\", port: 443))"
    );

    // 4. Open connections limit (64).
    let lim_listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let lim_port = i64::from(lim_listener.local_addr().unwrap().port());
    let lim_srv = std::thread::spawn(move || {
        let mut accepted = Vec::new();
        for _ in 0..MO_LIMIT_OPEN_CONNECTIONS {
            accepted.push(lim_listener.accept().unwrap().0);
        }
        accepted
    });
    let mut conns = Vec::new();
    for _ in 0..MO_LIMIT_OPEN_CONNECTIONS {
        let r = ctx
            .call(
                &heap,
                &fx.types,
                HostOpId::NetworkConnect,
                fx.res_conn,
                &[
                    net.clone(),
                    h_local.clone(),
                    Value::Int(lim_port),
                    Value::Duration(5_000_000),
                ],
            )
            .unwrap();
        assert_eq!(r.as_tag(), 0);
        conns.push(r);
    }
    assert_eq!(ctx.open_connections(), MO_LIMIT_OPEN_CONNECTIONS as u32);
    assert_eq!(
        ctx.call(
            &heap,
            &fx.types,
            HostOpId::NetworkConnect,
            fx.res_conn,
            &[
                net.clone(),
                h_local.clone(),
                Value::Int(lim_port),
                Value::Duration(5_000_000)
            ],
        )
        .err(),
        Some(HostError::Bug {
            reason: "too many connections open at once",
            detail: None,
        })
    );
    conns.clear();
    assert_eq!(ctx.open_connections(), 0);
    drop(lim_srv.join().unwrap());

    // 5. Fake network + mismatch description + send-after-receive Closed.
    let mut replies = MoMap::new(fx.bytes, fx.bytes);
    replies.set(
        &fx.types,
        heap.hash_key(),
        heap.alloc_bytes(b"GET / HTTP/1.1\r\nHost: right\r\n\r\n".to_vec())
            .unwrap(),
        heap.alloc_bytes(b"HTTP/1.1 200 OK\r\n\r\nok".to_vec())
            .unwrap(),
    );
    let mut peers = MoMap::new(fx.text, fx.map_bb);
    peers.set(
        &fx.types,
        heap.hash_key(),
        heap.alloc_text("api.test:80".into()).unwrap(),
        heap.alloc_map(replies).unwrap(),
    );
    let fake_net = ctx
        .call(
            &heap,
            &fx.types,
            HostOpId::NetworkFake,
            fx.net_cap,
            &[heap.alloc_map(peers).unwrap()],
        )
        .unwrap();
    let h_api = heap.alloc_text("api.test".into()).unwrap();
    let fc_res = ctx
        .call(
            &heap,
            &fx.types,
            HostOpId::NetworkConnect,
            fx.res_conn,
            &[
                fake_net.clone(),
                h_api.clone(),
                Value::Int(80),
                Value::Duration(1_000_000),
            ],
        )
        .unwrap();
    let fc = heap.extract_case_field(&fx.types, fx.res_conn, &fc_res, 0, 0);
    ctx.call(
        &heap,
        &fx.types,
        HostOpId::ConnectionSend,
        fx.res_net_unit,
        &[
            fc.clone(),
            heap.alloc_bytes(b"GET / HTTP/1.1\r\nHost: wrong\r\n\r\n".to_vec())
                .unwrap(),
        ],
    )
    .unwrap();
    let mismatch = ctx
        .call(
            &heap,
            &fx.types,
            HostOpId::ConnectionReceive,
            fx.res_net_bytes,
            &[fc.clone(), Value::Int(64)],
        )
        .unwrap();
    assert_eq!(
        value_print(&fx.types, fx.res_net_bytes, &mismatch),
        "Error(Broken(reason: \"no reply for \\\"GET / HTTP/1.1\\\": sent \\\"Host: wrong\\\\r\\\\n\\\", expected \\\"Host: right\\\\r\\\\n\\\"\"))"
    );
    // Send after receive returns Closed.
    let closed = ctx
        .call(
            &heap,
            &fx.types,
            HostOpId::ConnectionSend,
            fx.res_net_unit,
            &[fc, heap.alloc_bytes(b"x".to_vec()).unwrap()],
        )
        .unwrap();
    assert_eq!(
        value_print(&fx.types, fx.res_net_unit, &closed),
        "Error(Closed)"
    );
    let _ = (fx.map_t_map_bb, fx.list_text);
}

#[cfg(unix)]
#[test]
fn sqlite_real_confinement_authorizer_limits_and_fake_parity() {
    use std::os::unix::fs::symlink;

    let fx = Slice1dFixture::new();
    let heap = Heap::new(16 << 20);
    let root_dir = make_temp_dir("sqlite-root");
    let outside_dir = make_temp_dir("sqlite-outside");
    std::fs::write(outside_dir.join("ext.db"), b"").unwrap();
    symlink(outside_dir.join("ext.db"), root_dir.join("link.db")).unwrap();
    symlink(outside_dir.join("ext.db"), root_dir.join("bad-wal.db-wal")).unwrap();
    std::fs::write(root_dir.join("tiny.db"), b"not-a-db").unwrap();
    std::fs::create_dir(root_dir.join("adir")).unwrap();

    let mut ctx = HostContext::stdio_with_root(Vec::new(), Vec::new(), Some(&root_dir)).unwrap();
    let sql_cap = ctx
        .make_real_capability(&heap, &fx.types, fx.sqlite_cap)
        .unwrap();

    let open = |ctx: &mut HostContext, cap: &Value, p: &str| {
        ctx.call(
            &heap,
            &fx.types,
            HostOpId::SqliteOpen,
            fx.res_db,
            &[cap.clone(), heap.alloc_text(p.into()).unwrap()],
        )
    };
    let empty_params = heap.alloc_list(VecDeque::new()).unwrap();

    // 1. Confinement & invalid path checks.
    assert_eq!(
        value_print(
            &fx.types,
            fx.res_db,
            &open(&mut ctx, &sql_cap, "../escape.db").unwrap()
        ),
        "Error(OutsideFolder(path: \"../escape.db\"))"
    );
    assert_eq!(
        value_print(
            &fx.types,
            fx.res_db,
            &open(&mut ctx, &sql_cap, "/tmp/abs.db").unwrap()
        ),
        "Error(OutsideFolder(path: \"/tmp/abs.db\"))"
    );
    assert_eq!(
        value_print(
            &fx.types,
            fx.res_db,
            &open(&mut ctx, &sql_cap, "link.db").unwrap()
        ),
        "Error(OutsideFolder(path: \"link.db\"))"
    );
    assert_eq!(
        value_print(
            &fx.types,
            fx.res_db,
            &open(&mut ctx, &sql_cap, "bad-wal.db").unwrap()
        ),
        "Error(OutsideFolder(path: \"bad-wal.db\"))"
    );
    assert_eq!(
        value_print(
            &fx.types,
            fx.res_db,
            &open(&mut ctx, &sql_cap, "adir").unwrap()
        ),
        "Error(SqlFailed(reason: \"is a folder\"))"
    );
    assert_eq!(
        value_print(
            &fx.types,
            fx.res_db,
            &open(&mut ctx, &sql_cap, "tiny.db").unwrap()
        ),
        "Error(SqlFailed(reason: \"file is not a database\"))"
    );

    // 2. Real WAL database CRUD + all 5 Sqlite.Value variants + execute_raw + authorizer.
    let opened = open(&mut ctx, &sql_cap, "app.db").unwrap();
    assert_eq!(opened.as_tag(), 0);
    let db = heap.extract_case_field(&fx.types, fx.res_db, &opened, 0, 0);

    let run = |ctx: &mut HostContext, db: &Value, sql: &str, params: Value| {
        ctx.call(
            &heap,
            &fx.types,
            HostOpId::SqliteRun,
            fx.res_sql_unit,
            &[db.clone(), heap.alloc_text(sql.into()).unwrap(), params],
        )
        .unwrap()
    };
    let query = |ctx: &mut HostContext, db: &Value, sql: &str, params: Value| {
        ctx.call(
            &heap,
            &fx.types,
            HostOpId::SqliteQuery,
            fx.res_sql_rows,
            &[db.clone(), heap.alloc_text(sql.into()).unwrap(), params],
        )
        .unwrap()
    };

    assert_eq!(
        value_print(
            &fx.types,
            fx.res_sql_unit,
            &run(
                &mut ctx,
                &db,
                "CREATE TABLE items (id INTEGER PRIMARY KEY, a REAL, b TEXT, c BLOB, d TEXT)",
                empty_params.clone(),
            )
        ),
        "Ok(Unit)"
    );

    // Bind all 5 Sqlite.Value cases in execute_raw.
    let params = heap
        .alloc_list(VecDeque::from([
            heap.make_case(&fx.types, fx.sqlite_val, 1, vec![Value::Int(7)])
                .unwrap(),
            heap.make_case(&fx.types, fx.sqlite_val, 2, vec![Value::Float(1.5)])
                .unwrap(),
            heap.make_case(
                &fx.types,
                fx.sqlite_val,
                3,
                vec![heap.alloc_text("hi".into()).unwrap()],
            )
            .unwrap(),
            heap.make_case(
                &fx.types,
                fx.sqlite_val,
                4,
                vec![heap.alloc_bytes(vec![0xab, 0xcd]).unwrap()],
            )
            .unwrap(),
            heap.make_case(&fx.types, fx.sqlite_val, 0, vec![]).unwrap(),
        ]))
        .unwrap();
    let exec_res = ctx
        .call(
            &heap,
            &fx.types,
            HostOpId::SqliteExecuteRaw,
            fx.res_sql_raw,
            &[
                db.clone(),
                heap.alloc_text("INSERT INTO items VALUES (?, ?, ?, ?, ?)".into())
                    .unwrap(),
                params,
            ],
        )
        .unwrap();
    assert_eq!(
        value_print(&fx.types, fx.res_sql_raw, &exec_res),
        "Ok((1, 7))"
    );

    // Check WAL + foreign keys + query rows.
    let jm = query(&mut ctx, &db, "PRAGMA journal_mode", empty_params.clone());
    assert!(value_print(&fx.types, fx.res_sql_rows, &jm).contains("String(\"wal\")"));
    let fk = query(&mut ctx, &db, "PRAGMA foreign_keys", empty_params.clone());
    assert!(value_print(&fx.types, fx.res_sql_rows, &fk).contains("Integer(1)"));
    let rows = query(
        &mut ctx,
        &db,
        "SELECT id, a, b, c, d FROM items",
        empty_params.clone(),
    );
    assert!(value_print(&fx.types, fx.res_sql_rows, &rows).contains("Integer(7)"));
    assert!(value_print(&fx.types, fx.res_sql_rows, &rows).contains("String(\"hi\")"));
    assert!(value_print(&fx.types, fx.res_sql_rows, &rows).contains("Null"));

    // Invalid UTF-8 text column rejected.
    let bad_utf8 = query(
        &mut ctx,
        &db,
        "SELECT CAST(X'FF' AS TEXT)",
        empty_params.clone(),
    );
    assert_eq!(
        value_print(&fx.types, fx.res_sql_rows, &bad_utf8),
        "Error(SqlFailed(reason: \"result text is not valid UTF-8\"))"
    );

    // Empty / multi-statement / param mismatch rejected.
    assert_eq!(
        value_print(
            &fx.types,
            fx.res_sql_unit,
            &run(&mut ctx, &db, "  -- comment\n", empty_params.clone())
        ),
        "Error(SqlFailed(reason: \"empty SQL statement\"))"
    );
    assert_eq!(
        value_print(
            &fx.types,
            fx.res_sql_unit,
            &run(&mut ctx, &db, "SELECT 1; SELECT 2", empty_params.clone())
        ),
        "Error(SqlFailed(reason: \"multiple SQL statements are not permitted\"))"
    );
    assert_eq!(
        value_print(
            &fx.types,
            fx.res_sql_unit,
            &run(&mut ctx, &db, "SELECT 1;;", empty_params.clone())
        ),
        "Error(SqlFailed(reason: \"multiple SQL statements are not permitted\"))"
    );
    assert_eq!(
        value_print(
            &fx.types,
            fx.res_sql_unit,
            &run(&mut ctx, &db, "SELECT ?", empty_params.clone())
        ),
        "Error(SqlFailed(reason: \"SQL parameter count mismatch\"))"
    );

    // Authorizer denials vs plain VACUUM.
    assert_eq!(
        value_print(
            &fx.types,
            fx.res_sql_unit,
            &run(
                &mut ctx,
                &db,
                "ATTACH DATABASE ':memory:' AS x",
                empty_params.clone()
            )
        ),
        "Error(NotPermitted(reason: \"ATTACH is not permitted\"))"
    );
    assert_eq!(
        value_print(
            &fx.types,
            fx.res_sql_unit,
            &run(
                &mut ctx,
                &db,
                "PRAGMA temp_store = FILE",
                empty_params.clone()
            )
        ),
        "Error(NotPermitted(reason: \"PRAGMA temp_store is not permitted\"))"
    );
    assert_eq!(
        value_print(
            &fx.types,
            fx.res_sql_unit,
            &run(&mut ctx, &db, "VACUUM INTO 'out.db'", empty_params.clone())
        ),
        "Error(NotPermitted(reason: \"ATTACH is not permitted\"))"
    );
    assert_eq!(
        value_print(
            &fx.types,
            fx.res_sql_unit,
            &run(&mut ctx, &db, "VACUUM", empty_params.clone())
        ),
        "Ok(Unit)"
    );
    drop(db);
    drop(opened);
    assert_eq!(ctx.open_databases(), 0);

    // 3. MO_LIMIT_OPEN_DATABASES (16) limit + reclaim on drop.
    let mut dbs = Vec::new();
    for _ in 0..MO_LIMIT_OPEN_DATABASES {
        dbs.push(open(&mut ctx, &sql_cap, ":memory:").unwrap());
    }
    assert_eq!(ctx.open_databases(), MO_LIMIT_OPEN_DATABASES as u32);
    assert_eq!(
        open(&mut ctx, &sql_cap, ":memory:").err(),
        Some(HostError::Bug {
            reason: "too many databases open at once",
            detail: None,
        })
    );
    dbs.clear();
    assert_eq!(ctx.open_databases(), 0);

    // 4. Fake Sqlite shares state across normalized paths within one hub.
    let fake_sql = ctx
        .call(&heap, &fx.types, HostOpId::SqliteFake, fx.sqlite_cap, &[])
        .unwrap();
    let f1_res = open(&mut ctx, &fake_sql, "sub/test.db").unwrap();
    let f1 = heap.extract_case_field(&fx.types, fx.res_db, &f1_res, 0, 0);
    run(
        &mut ctx,
        &f1,
        "CREATE TABLE t(x INT); ",
        empty_params.clone(),
    );
    run(
        &mut ctx,
        &f1,
        "INSERT INTO t VALUES (99)",
        empty_params.clone(),
    );
    drop(f1);
    drop(f1_res);

    let f2_res = open(&mut ctx, &fake_sql, "./sub/test.db").unwrap();
    let f2 = heap.extract_case_field(&fx.types, fx.res_db, &f2_res, 0, 0);
    let q2 = query(&mut ctx, &f2, "SELECT x FROM t", empty_params.clone());
    assert!(value_print(&fx.types, fx.res_sql_rows, &q2).contains("Integer(99)"));
    assert_eq!(
        value_print(
            &fx.types,
            fx.res_db,
            &open(&mut ctx, &fake_sql, "sub").unwrap()
        ),
        "Error(SqlFailed(reason: \"is a folder\"))"
    );
    assert_eq!(
        value_print(
            &fx.types,
            fx.res_db,
            &open(&mut ctx, &fake_sql, "sub/test.db/more").unwrap()
        ),
        "Error(SqlFailed(reason: \"not a folder\"))"
    );
    drop(f2);
    drop(f2_res);

    let _ = std::fs::remove_dir_all(&root_dir);
    let _ = std::fs::remove_dir_all(&outside_dir);
}

#[cfg(unix)]
#[test]
fn command_real_confinement_allowlist_timeout_output_cap_and_fake_parity() {
    let fx = Slice1dFixture::new();
    let heap = Heap::new(16 << 20);
    let root_dir = make_temp_dir("cmd-root");
    std::fs::create_dir(root_dir.join("sub")).unwrap();

    let mut ctx = HostContext::stdio_with_root(Vec::new(), Vec::new(), Some(&root_dir)).unwrap();
    let cmd = ctx
        .make_real_capability(&heap, &fx.types, fx.cmd_cap)
        .unwrap();

    // 1. Bug checks: timeout out of [1ms, 1h], > 64 args.
    let prog_echo = heap.alloc_text("echo".into()).unwrap();
    let empty_args = text_list(&heap, &[]);
    for bad_dur in [999i64, 0, -1, MO_LIMIT_COMMAND_DURATION_US + 1] {
        assert_eq!(
            ctx.call(
                &heap,
                &fx.types,
                HostOpId::CommandRunRaw,
                fx.res_cmd_raw,
                &[
                    cmd.clone(),
                    prog_echo.clone(),
                    empty_args.clone(),
                    Value::Duration(bad_dur)
                ],
            )
            .err(),
            Some(HostError::Bug {
                reason: "timeout below 1ms or above 1 hour",
                detail: None,
            })
        );
    }
    let too_many_args: Vec<&str> = vec!["x"; (MO_LIMIT_COMMAND_ARGS as usize) + 1];
    assert_eq!(
        ctx.call(
            &heap,
            &fx.types,
            HostOpId::CommandRunRaw,
            fx.res_cmd_raw,
            &[
                cmd.clone(),
                prog_echo.clone(),
                text_list(&heap, &too_many_args),
                Value::Duration(1_000_000),
            ],
        )
        .err(),
        Some(HostError::Bug {
            reason: "too many command arguments",
            detail: None,
        })
    );

    // 2. Real echo + within (existing + deferred + poisoned escape).
    let res = ctx
        .call(
            &heap,
            &fx.types,
            HostOpId::CommandRunRaw,
            fx.res_cmd_raw,
            &[
                cmd.clone(),
                prog_echo.clone(),
                text_list(&heap, &["hello", "world"]),
                Value::Duration(5_000_000),
            ],
        )
        .unwrap();
    let tup = heap.extract_case_field(&fx.types, fx.res_cmd_raw, &res, 0, 0);
    assert_eq!(heap.extract_field(&tup, 0).as_int(), 0);
    assert_eq!(heap.extract_field(&tup, 1).as_bytes(), b"hello world\n");
    assert_eq!(heap.extract_field(&tup, 2).as_bytes(), b"");

    let sub_cmd = ctx
        .call(
            &heap,
            &fx.types,
            HostOpId::CommandWithin,
            fx.cmd_cap,
            &[cmd.clone(), heap.alloc_text("deferred".into()).unwrap()],
        )
        .unwrap();
    std::fs::create_dir(root_dir.join("deferred")).unwrap();
    let pwd_res = ctx
        .call(
            &heap,
            &fx.types,
            HostOpId::CommandRunRaw,
            fx.res_cmd_raw,
            &[
                sub_cmd,
                heap.alloc_text("pwd".into()).unwrap(),
                empty_args.clone(),
                Value::Duration(5_000_000),
            ],
        )
        .unwrap();
    let pwd_tup = heap.extract_case_field(&fx.types, fx.res_cmd_raw, &pwd_res, 0, 0);
    assert!(
        std::str::from_utf8(heap.extract_field(&pwd_tup, 1).as_bytes())
            .unwrap()
            .trim()
            .ends_with("/deferred")
    );

    let esc_cmd = ctx
        .call(
            &heap,
            &fx.types,
            HostOpId::CommandWithin,
            fx.cmd_cap,
            &[cmd.clone(), heap.alloc_text("../escape".into()).unwrap()],
        )
        .unwrap();
    let esc_cmd2 = ctx
        .call(
            &heap,
            &fx.types,
            HostOpId::CommandWithin,
            fx.cmd_cap,
            &[esc_cmd, heap.alloc_text("sub".into()).unwrap()],
        )
        .unwrap();
    let esc_res = ctx
        .call(
            &heap,
            &fx.types,
            HostOpId::CommandRunRaw,
            fx.res_cmd_raw,
            &[
                esc_cmd2,
                prog_echo.clone(),
                empty_args.clone(),
                Value::Duration(5_000_000),
            ],
        )
        .unwrap();
    assert_eq!(
        value_print(&fx.types, fx.res_cmd_raw, &esc_res),
        "Error(\"outside_folder:../escape\")"
    );

    // 3. Monotonic allow_programs intersection.
    let c1 = ctx
        .call(
            &heap,
            &fx.types,
            HostOpId::CommandAllowPrograms,
            fx.cmd_cap,
            &[cmd.clone(), text_list(&heap, &["echo", "pwd", "/bin/sh"])],
        )
        .unwrap();
    let c2 = ctx
        .call(
            &heap,
            &fx.types,
            HostOpId::CommandAllowPrograms,
            fx.cmd_cap,
            &[c1, text_list(&heap, &["echo", "ls"])],
        )
        .unwrap();
    let blocked = ctx
        .call(
            &heap,
            &fx.types,
            HostOpId::CommandRunRaw,
            fx.res_cmd_raw,
            &[
                c2.clone(),
                heap.alloc_text("pwd".into()).unwrap(),
                empty_args.clone(),
                Value::Duration(5_000_000),
            ],
        )
        .unwrap();
    assert_eq!(
        value_print(&fx.types, fx.res_cmd_raw, &blocked),
        "Error(\"not_permitted:pwd\")"
    );

    // 4. Real timeout -> "timed_out".
    let to_res = ctx
        .call(
            &heap,
            &fx.types,
            HostOpId::CommandRunRaw,
            fx.res_cmd_raw,
            &[
                cmd.clone(),
                heap.alloc_text("sleep".into()).unwrap(),
                text_list(&heap, &["5"]),
                Value::Duration(Duration::from_millis(40).as_micros() as i64),
            ],
        )
        .unwrap();
    assert_eq!(
        value_print(&fx.types, fx.res_cmd_raw, &to_res),
        "Error(\"timed_out\")"
    );

    // 5. Real output cap (> 4 MiB) -> "output_too_large".
    let big_res = ctx
        .call(
            &heap,
            &fx.types,
            HostOpId::CommandRunRaw,
            fx.res_cmd_raw,
            &[
                cmd.clone(),
                heap.alloc_text("dd".into()).unwrap(),
                text_list(
                    &heap,
                    &["if=/dev/zero", "bs=1048576", "count=5", "status=none"],
                ),
                Value::Duration(5_000_000),
            ],
        )
        .unwrap();
    assert_eq!(
        value_print(&fx.types, fx.res_cmd_raw, &big_res),
        "Error(\"output_too_large\")"
    );

    // 6. Fake Command exact key/value round-trip.
    let mut replies = MoMap::new(fx.bytes, fx.bytes);
    replies.set(
        &fx.types,
        heap.hash_key(),
        heap.alloc_bytes(b"3:sub\n4:echo\n1\n2:hi\n".to_vec())
            .unwrap(),
        heap.alloc_bytes(b"0:3:2\nhi\nno".to_vec()).unwrap(),
    );
    let fcmd = ctx
        .call(
            &heap,
            &fx.types,
            HostOpId::CommandFake,
            fx.cmd_cap,
            &[heap.alloc_map(replies).unwrap()],
        )
        .unwrap();
    let fsub = ctx
        .call(
            &heap,
            &fx.types,
            HostOpId::CommandWithin,
            fx.cmd_cap,
            &[fcmd, heap.alloc_text("sub".into()).unwrap()],
        )
        .unwrap();
    let fres = ctx
        .call(
            &heap,
            &fx.types,
            HostOpId::CommandRunRaw,
            fx.res_cmd_raw,
            &[
                fsub,
                prog_echo,
                text_list(&heap, &["hi"]),
                Value::Duration(1_000_000),
            ],
        )
        .unwrap();
    let ftup = heap.extract_case_field(&fx.types, fx.res_cmd_raw, &fres, 0, 0);
    assert_eq!(heap.extract_field(&ftup, 0).as_int(), 0);
    assert_eq!(heap.extract_field(&ftup, 1).as_bytes(), b"hi\n");
    assert_eq!(heap.extract_field(&ftup, 2).as_bytes(), b"no");

    let _ = std::fs::remove_dir_all(&root_dir);
}
