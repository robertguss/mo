//! Mutation operator enumeration and source splicing (`src/tools/mutants.{h,c}`).

use mo_base::{LineCol, SourceMap, Span};
use mo_check::{CheckedProgram, ModuleId};
use mo_def::limits::MO_LIMIT_MUTANTS;
use mo_syntax::{NodeId, SyntaxKind, SyntaxTree, TokenKind};

struct Swap {
    from: TokenKind,
    to: Option<&'static str>,
}

struct Row {
    kind: &'static str,
    swaps: &'static [Swap],
}

const ROWS: &[Row] = &[
    Row {
        kind: "boundary",
        swaps: &[
            Swap {
                from: TokenKind::Lt,
                to: Some("<="),
            },
            Swap {
                from: TokenKind::Le,
                to: Some("<"),
            },
            Swap {
                from: TokenKind::Gt,
                to: Some(">="),
            },
            Swap {
                from: TokenKind::Ge,
                to: Some(">"),
            },
        ],
    },
    Row {
        kind: "negate",
        swaps: &[
            Swap {
                from: TokenKind::Eq,
                to: Some("!="),
            },
            Swap {
                from: TokenKind::Ne,
                to: Some("=="),
            },
            Swap {
                from: TokenKind::Lt,
                to: Some(">="),
            },
            Swap {
                from: TokenKind::Le,
                to: Some(">"),
            },
            Swap {
                from: TokenKind::Gt,
                to: Some("<="),
            },
            Swap {
                from: TokenKind::Ge,
                to: Some("<"),
            },
        ],
    },
    Row {
        kind: "logic",
        swaps: &[
            Swap {
                from: TokenKind::And,
                to: Some("or"),
            },
            Swap {
                from: TokenKind::Or,
                to: Some("and"),
            },
        ],
    },
    Row {
        kind: "not",
        swaps: &[Swap {
            from: TokenKind::Not,
            to: Some(""),
        }],
    },
    Row {
        kind: "arithmetic",
        swaps: &[
            Swap {
                from: TokenKind::Plus,
                to: Some("-"),
            },
            Swap {
                from: TokenKind::Minus,
                to: Some("+"),
            },
            Swap {
                from: TokenKind::Star,
                to: Some("/"),
            },
            Swap {
                from: TokenKind::Slash,
                to: Some("*"),
            },
        ],
    },
    Row {
        kind: "bool",
        swaps: &[
            Swap {
                from: TokenKind::True,
                to: Some("false"),
            },
            Swap {
                from: TokenKind::False,
                to: Some("true"),
            },
        ],
    },
    Row {
        kind: "int",
        swaps: &[Swap {
            from: TokenKind::Int,
            to: None,
        }],
    },
];

fn operator_spelling(token: TokenKind) -> Option<&'static str> {
    match token {
        TokenKind::Eq => Some("=="),
        TokenKind::Ne => Some("!="),
        TokenKind::Lt => Some("<"),
        TokenKind::Le => Some("<="),
        TokenKind::Gt => Some(">"),
        TokenKind::Ge => Some(">="),
        TokenKind::And => Some("and"),
        TokenKind::Or => Some("or"),
        TokenKind::Plus => Some("+"),
        TokenKind::Minus => Some("-"),
        TokenKind::Star => Some("*"),
        TokenKind::Slash => Some("/"),
        _ => None,
    }
}

/// One candidate source mutation (`struct mo_mutant`).
#[derive(Debug, Clone)]
pub struct Mutant {
    pub function: usize,
    pub module: usize,
    pub span: Span,
    pub position: LineCol,
    pub kind: &'static str,
    pub original: String,
    pub replacement: String,
}

fn value_of(c: u8) -> u8 {
    if c <= b'9' {
        c - b'0'
    } else if c >= b'a' {
        c - b'a' + 10
    } else {
        c - b'A' + 10
    }
}

fn raise_int_literal(text: &[u8]) -> String {
    let prefix = if text.len() > 2 && text[0] == b'0' && (text[1] == b'x' || text[1] == b'b') {
        2usize
    } else {
        0usize
    };
    let base: u8 = if prefix == 0 {
        10
    } else if text[1] == b'x' {
        16
    } else {
        2
    };
    let lower = text[prefix..].iter().any(|&b| b.is_ascii_lowercase());
    let mut out = vec![0u8; text.len() + 1];
    out[1..].copy_from_slice(text);
    let mut carry = true;
    let mut i = text.len();
    while carry && i > prefix {
        if out[i] == b'_' {
            i -= 1;
            continue;
        }
        let mut value = value_of(out[i]) + 1;
        carry = value == base;
        if carry {
            value = 0;
        }
        out[i] = if value < 10 {
            b'0' + value
        } else {
            (if lower { b'a' } else { b'A' }) + value - 10
        };
        i -= 1;
    }
    let slice = if !carry {
        &out[1..]
    } else {
        for k in 0..prefix {
            out[k] = out[k + 1];
        }
        out[prefix] = b'1';
        &out[..]
    };
    String::from_utf8_lossy(slice).into_owned()
}

struct Walker<'a> {
    sources: &'a SourceMap,
    tree: &'a SyntaxTree,
    bytes: &'a [u8],
    function: usize,
    module: usize,
    items: Vec<Mutant>,
    limit_exceeded: bool,
}

impl Walker<'_> {
    fn site(&mut self, token: TokenKind, span: Span) {
        let orig_bytes = &self.bytes[span.start as usize..span.end as usize];
        let original = String::from_utf8_lossy(orig_bytes).into_owned();
        let position = self
            .sources
            .position(span.file, span.start)
            .unwrap_or(LineCol { line: 1, column: 1 });
        for row in ROWS {
            for swap in row.swaps {
                if swap.from != token {
                    continue;
                }
                if (self.items.len() as u64) >= MO_LIMIT_MUTANTS {
                    self.limit_exceeded = true;
                    return;
                }
                let replacement = match swap.to {
                    Some(s) => s.to_string(),
                    None => raise_int_literal(orig_bytes),
                };
                self.items.push(Mutant {
                    function: self.function,
                    module: self.module,
                    span,
                    position,
                    kind: row.kind,
                    original: original.clone(),
                    replacement,
                });
            }
        }
    }

    fn operator_span(&self, node_id: NodeId) -> Span {
        let node = self.tree.node(node_id);
        let left_end =
            node.children[0].map_or(node.span.start, |id| self.tree.node(id).span.end) as usize;
        let right_start =
            node.children[1].map_or(node.span.end, |id| self.tree.node(id).span.start) as usize;
        let mut at = left_end;
        while at < right_start && matches!(self.bytes[at], b' ' | b'\t' | b'\n' | b'\r' | b'#') {
            if self.bytes[at] == b'#' {
                at += 1;
                while at < right_start && self.bytes[at] != b'\n' && self.bytes[at] != b'\r' {
                    at += 1;
                }
            } else {
                at += 1;
            }
        }
        #[expect(
            clippy::expect_used,
            reason = "operator_span is only called after operator_spelling succeeds"
        )]
        let spelling = operator_spelling(node.token).expect("listed binary operator");
        debug_assert!(
            at + spelling.len() <= right_start
                && &self.bytes[at..at + spelling.len()] == spelling.as_bytes()
        );
        Span::new(node.span.file, at as u32, (at + spelling.len()) as u32)
    }

    fn walk(&mut self, node_opt: Option<NodeId>) {
        if self.limit_exceeded {
            return;
        }
        let Some(node_id) = node_opt else {
            return;
        };
        let node = self.tree.node(node_id);
        let kind = node.kind;
        let token = node.token;
        let span = node.span;
        if matches!(kind, SyntaxKind::True | SyntaxKind::False | SyntaxKind::Int) {
            self.site(token, span);
            return;
        }
        let children: Vec<Option<NodeId>> = node.children.clone();
        if kind == SyntaxKind::Binary {
            self.walk(children[0]);
            if operator_spelling(token).is_some() {
                let op_span = self.operator_span(node_id);
                self.site(token, op_span);
            }
            self.walk(children[1]);
            return;
        }
        if kind == SyntaxKind::BinaryChain {
            for (i, &child) in children.iter().enumerate() {
                if i % 2 == 0 {
                    self.walk(child);
                } else if let Some(cid) = child {
                    let cnode = self.tree.node(cid);
                    self.site(cnode.token, cnode.span);
                }
            }
            return;
        }
        let from = usize::from(kind == SyntaxKind::Arm);
        let to = if matches!(kind, SyntaxKind::Is | SyntaxKind::Position) {
            children.len().saturating_sub(1)
        } else {
            children.len()
        };
        if kind == SyntaxKind::Not {
            debug_assert_eq!(
                &self.bytes[span.start as usize..span.start as usize + 3],
                b"not"
            );
            self.site(
                TokenKind::Not,
                Span::new(span.file, span.start, span.start + 3),
            );
        }
        for &child in &children[from..to] {
            self.walk(child);
        }
    }
}

/// Whether `module` is tested (entry module 0, or neither library nor path dependency)
/// (`src/tools/mutants.c:mo_module_tested`).
#[must_use]
pub fn module_tested(program: &CheckedProgram, module: ModuleId) -> bool {
    let idx = module.index();
    idx == 0 || (!program.modules.modules[idx].library && !program.modules.modules[idx].dependency)
}

/// Lists all candidate mutants across tested functions in module+source order (`mo_list_mutants`).
pub fn list_mutants(
    program: &CheckedProgram,
    sources: &SourceMap,
) -> Result<Vec<Mutant>, &'static str> {
    let mut places: Vec<(usize, usize, u32)> = Vec::new();
    for (i, func) in program.functions.iter().enumerate() {
        let sym = program.interfaces.symbol(func.symbol);
        if !module_tested(program, sym.module) {
            continue;
        }
        let tree = &program.modules.module(sym.module).syntax.tree;
        let decl = tree.node(sym.declaration);
        if decl.kind == SyntaxKind::Fn {
            places.push((i, sym.module.index(), decl.span.start));
        }
    }
    places.sort_by_key(|&(_, m, start)| (m, start));

    let mut all = Vec::new();
    for (func_idx, mod_idx, _) in places {
        let sym = program
            .interfaces
            .symbol(program.functions[func_idx].symbol);
        let loaded = &program.modules.modules[mod_idx];
        let tree = &loaded.syntax.tree;
        let decl = tree.node(sym.declaration);
        let Some(src) = sources.get(decl.span.file) else {
            continue;
        };
        let body = decl.children.get(5).copied().flatten();
        let mut walker = Walker {
            sources,
            tree,
            bytes: src.bytes(),
            function: func_idx,
            module: mod_idx,
            items: std::mem::take(&mut all),
            limit_exceeded: false,
        };
        walker.walk(body);
        if walker.limit_exceeded {
            return Err("the program has more mutants than mo test makes");
        }
        all = walker.items;
    }
    Ok(all)
}

/// Builds a mutated [`SourceMap`] containing all modules with `mutant` spliced in (`mo_mutant_sources`).
pub fn mutant_sources(
    program: &CheckedProgram,
    sources: &SourceMap,
    mutant: &Mutant,
) -> Option<SourceMap> {
    let changed = sources.get(mutant.span.file)?;
    let orig = changed.bytes();
    let mut mutated = Vec::with_capacity(
        orig.len() - (mutant.span.end - mutant.span.start) as usize + mutant.replacement.len(),
    );
    mutated.extend_from_slice(&orig[..mutant.span.start as usize]);
    mutated.extend_from_slice(mutant.replacement.as_bytes());
    mutated.extend_from_slice(&orig[mutant.span.end as usize..]);

    let mut out = SourceMap::new();
    for (k, module) in program.modules.modules.iter().enumerate() {
        let file_id = module.syntax.tree.node(module.syntax.root).span.file;
        let f = sources.get(file_id)?;
        let bytes = if k == mutant.module {
            mutated.clone()
        } else {
            f.bytes().to_vec()
        };
        out.add_bytes(f.path_bytes().to_vec(), bytes).ok()?;
    }
    Some(out)
}
