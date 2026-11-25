//! Comment/trivia attachment and `blank_before` computation (`src/syntax/comments.c` parity).

use std::cmp::Reverse;
use std::collections::HashMap;

use mo_base::{DiagBag, SourceFile};

use crate::ast::{CommentAnchor, NodeId, SyntaxKind, SyntaxTree, TokenKind};
use crate::lexer::{Lexer, RawComment, Token, decode_utf8};

const CANDIDATE_SCRATCH_COST: usize = 16;
const COUNTER_SCRATCH_COST: usize = 16;

struct Candidate {
    node: NodeId,
    indentation: u32,
    start: u32,
    end: u32,
    kind: SyntaxKind,
}

fn count_nodes(tree: &SyntaxTree, root: NodeId) -> usize {
    let mut total = 0usize;
    let mut stack = vec![root];
    while let Some(nid) = stack.pop() {
        total += 1;
        for child in &tree.node(nid).children {
            if let Some(cid) = *child {
                stack.push(cid);
            }
        }
    }
    total
}

fn collect_candidates(tree: &SyntaxTree, root: NodeId, out: &mut Vec<Candidate>) {
    let mut stack = vec![root];
    while let Some(nid) = stack.pop() {
        let n = tree.node(nid);
        if n.kind != SyntaxKind::Sequence && n.kind != SyntaxKind::File {
            out.push(Candidate {
                node: nid,
                indentation: 0,
                start: n.span.start,
                end: n.span.end,
                kind: n.kind,
            });
        }
        for child in n.children.iter().rev() {
            if let Some(cid) = *child {
                stack.push(cid);
            }
        }
    }
}

const fn is_block_scope(kind: SyntaxKind) -> bool {
    matches!(
        kind,
        SyntaxKind::File
            | SyntaxKind::Module
            | SyntaxKind::Fn
            | SyntaxKind::Struct
            | SyntaxKind::Enum
            | SyntaxKind::Test
            | SyntaxKind::Service
            | SyntaxKind::Actor
            | SyntaxKind::Api
            | SyntaxKind::ApiScope
            | SyntaxKind::If
            | SyntaxKind::Else
            | SyntaxKind::Case
            | SyntaxKind::For
            | SyntaxKind::While
            | SyntaxKind::Lambda
            | SyntaxKind::Promise
            | SyntaxKind::Examples
            | SyntaxKind::Arm
            | SyntaxKind::IfPart
            | SyntaxKind::List
            | SyntaxKind::Map
            | SyntaxKind::Tuple
            | SyntaxKind::Group
            | SyntaxKind::Call
            | SyntaxKind::TypeArgs
    )
}

fn find_owner(
    tree: &SyntaxTree,
    nid: NodeId,
    offset: u32,
    mut enclosing: NodeId,
    end: &mut u32,
) -> NodeId {
    let n = tree.node(nid);
    if offset < n.span.start || offset > n.span.end {
        return enclosing;
    }
    if is_block_scope(n.kind) {
        enclosing = nid;
        *end = n.span.end;
    }
    if n.kind == SyntaxKind::If
        && let Some(parts_id) = n.children.get(2).copied().flatten()
    {
        let parts = tree.node(parts_id);
        if !parts.children.is_empty() && offset < parts.span.start {
            *end = parts.span.start;
        }
    }
    if n.children.len() <= 6 {
        for child in &n.children {
            if let Some(cid) = *child {
                let c = tree.node(cid);
                if c.span.start <= offset && c.span.end >= offset {
                    enclosing = find_owner(tree, cid, offset, enclosing, end);
                }
            }
        }
    } else {
        let mut low = 0usize;
        let mut high = n.children.len();
        while low < high {
            let mid = low + (high - low) / 2;
            let starts_before =
                n.children[mid].is_some_and(|cid| tree.node(cid).span.start <= offset);
            if starts_before {
                low = mid + 1;
            } else {
                high = mid;
            }
        }
        if low > 0
            && let Some(cid) = n.children[low - 1]
        {
            enclosing = find_owner(tree, cid, offset, enclosing, end);
        }
    }
    enclosing
}

fn find_token_owner(tree: &SyntaxTree, nid: NodeId, token: Token, mut found: NodeId) -> NodeId {
    let n = tree.node(nid);
    if token.span.start < n.span.start || token.span.end > n.span.end {
        return found;
    }
    if n.kind != SyntaxKind::ExprStmt
        && n.kind != SyntaxKind::Binder
        && n.kind != SyntaxKind::Sequence
        && !((n.kind == SyntaxKind::Else || n.kind == SyntaxKind::IfPart)
            && token.kind == TokenKind::End)
    {
        found = nid;
    }
    if n.children.len() <= 6 {
        for child in &n.children {
            if let Some(cid) = *child {
                found = find_token_owner(tree, cid, token, found);
            }
        }
    } else {
        let mut low = 0usize;
        let mut high = n.children.len();
        while low < high {
            let mid = low + (high - low) / 2;
            let starts_before =
                n.children[mid].is_some_and(|cid| tree.node(cid).span.start <= token.span.start);
            if starts_before {
                low = mid + 1;
            } else {
                high = mid;
            }
        }
        if low > 0
            && let Some(cid) = n.children[low - 1]
        {
            found = find_token_owner(tree, cid, token, found);
        }
    }
    found
}

struct ItemList {
    owner: NodeId,
    closer: TokenKind,
}

fn bracket_items(tree: &SyntaxTree, nid: NodeId) -> Option<ItemList> {
    let n = tree.node(nid);
    let mut closer = TokenKind::RParen;
    let seq = match n.kind {
        SyntaxKind::List => {
            closer = TokenKind::RBrack;
            Some(nid)
        }
        SyntaxKind::Map => {
            closer = TokenKind::RBrace;
            Some(nid)
        }
        SyntaxKind::Tuple => Some(nid),
        SyntaxKind::Fn => n.children.get(1).copied().flatten(),
        SyntaxKind::Call => n.children.last().copied().flatten(),
        SyntaxKind::TypeArgs => {
            closer = TokenKind::RBrack;
            n.children.last().copied().flatten()
        }
        SyntaxKind::CaseDecl | SyntaxKind::ApiStatus => {
            let cand = n.children.get(1).copied().flatten()?;
            if tree.node(cand).kind != SyntaxKind::Sequence {
                return None;
            }
            Some(cand)
        }
        SyntaxKind::File
        | SyntaxKind::Module
        | SyntaxKind::Expose
        | SyntaxKind::ExposeAll
        | SyntaxKind::Use
        | SyntaxKind::Param
        | SyntaxKind::Where
        | SyntaxKind::Struct
        | SyntaxKind::Field
        | SyntaxKind::Enum
        | SyntaxKind::Test
        | SyntaxKind::Const
        | SyntaxKind::Service
        | SyntaxKind::ServiceState
        | SyntaxKind::ServiceTools
        | SyntaxKind::ServiceEvery
        | SyntaxKind::ServiceToken
        | SyntaxKind::Actor
        | SyntaxKind::ActorCalls
        | SyntaxKind::Api
        | SyntaxKind::ApiPort
        | SyntaxKind::ApiCors
        | SyntaxKind::ApiSqlite
        | SyntaxKind::ApiActor
        | SyntaxKind::ApiAuth
        | SyntaxKind::ApiRoute
        | SyntaxKind::ApiStatic
        | SyntaxKind::ApiJob
        | SyntaxKind::ApiScope
        | SyntaxKind::Promise
        | SyntaxKind::Anchor
        | SyntaxKind::Examples
        | SyntaxKind::Type
        | SyntaxKind::Sequence
        | SyntaxKind::ExprStmt
        | SyntaxKind::Bind
        | SyntaxKind::Var
        | SyntaxKind::AddAssign
        | SyntaxKind::SubAssign
        | SyntaxKind::Return
        | SyntaxKind::Break
        | SyntaxKind::Continue
        | SyntaxKind::Assert
        | SyntaxKind::For
        | SyntaxKind::While
        | SyntaxKind::Binder
        | SyntaxKind::If
        | SyntaxKind::Else
        | SyntaxKind::LineIf
        | SyntaxKind::Case
        | SyntaxKind::Arm
        | SyntaxKind::Lambda
        | SyntaxKind::Binary
        | SyntaxKind::Negate
        | SyntaxKind::Not
        | SyntaxKind::Try
        | SyntaxKind::Is
        | SyntaxKind::Range
        | SyntaxKind::NamedArg
        | SyntaxKind::FieldAccess
        | SyntaxKind::Qualified
        | SyntaxKind::Position
        | SyntaxKind::Int
        | SyntaxKind::Decimal
        | SyntaxKind::Char
        | SyntaxKind::Text
        | SyntaxKind::TextInterpolated
        | SyntaxKind::True
        | SyntaxKind::False
        | SyntaxKind::Name
        | SyntaxKind::CapName
        | SyntaxKind::Underscore
        | SyntaxKind::Group
        | SyntaxKind::Rest
        | SyntaxKind::MapEntry
        | SyntaxKind::Stop
        | SyntaxKind::Token
        | SyntaxKind::BinaryChain
        | SyntaxKind::Postfix
        | SyntaxKind::Prefix
        | SyntaxKind::IfPart => None,
    }?;
    Some(ItemList { owner: seq, closer })
}

fn find_item_owner(
    tree: &SyntaxTree,
    nid: NodeId,
    preceding: Token,
    next: TokenKind,
) -> Option<(NodeId, u32)> {
    let n = tree.node(nid);
    if preceding.span.start < n.span.start || preceding.span.end > n.span.end {
        return None;
    }
    if n.children.len() <= 6 {
        for child in &n.children {
            if let Some(cid) = *child
                && let Some(found) = find_item_owner(tree, cid, preceding, next)
            {
                return Some(found);
            }
        }
    } else {
        let mut low = 0usize;
        let mut high = n.children.len();
        while low < high {
            let mid = low + (high - low) / 2;
            let starts_before = n.children[mid]
                .is_some_and(|cid| tree.node(cid).span.start <= preceding.span.start);
            if starts_before {
                low = mid + 1;
            } else {
                high = mid;
            }
        }
        if low > 0
            && let Some(cid) = n.children[low - 1]
            && let Some(found) = find_item_owner(tree, cid, preceding, next)
        {
            return Some(found);
        }
    }
    let list = bracket_items(tree, nid)?;
    let items = &tree.node(list.owner).children;
    if items.is_empty() || next != list.closer {
        return None;
    }
    let target = if preceding.kind == TokenKind::Comma {
        preceding.span.start
    } else {
        preceding.span.end
    };
    let mut low = 0usize;
    let mut high = items.len();
    while low < high {
        let mid = low + (high - low) / 2;
        let ends_before = items[mid].is_some_and(|cid| tree.node(cid).span.end <= target);
        if ends_before {
            low = mid + 1;
        } else {
            high = mid;
        }
    }
    if low != items.len() {
        return None;
    }
    let last_end = items[low - 1].map(|cid| tree.node(cid).span.end)?;
    if preceding.kind != TokenKind::Comma && last_end != target {
        return None;
    }
    Some((list.owner, (low - 1) as u32))
}

fn attach_inline_anchors(
    source: &SourceFile,
    file: u32,
    tree: &SyntaxTree,
    root: NodeId,
    raw_comments: &mut [RawComment],
    scratch_budget: &mut usize,
) -> bool {
    let token_comment_count = raw_comments
        .iter()
        .filter(|c| c.comment.anchor == CommentAnchor::Token)
        .count();
    if token_comment_count == 0 {
        return true;
    }
    let Some(cost) = token_comment_count.checked_mul(COUNTER_SCRATCH_COST) else {
        return false;
    };
    if cost > *scratch_budget {
        return false;
    }
    *scratch_budget -= cost;

    let mut counters: HashMap<(NodeId, TokenKind), u32> =
        HashMap::with_capacity(token_comment_count);
    for raw in raw_comments.iter_mut() {
        if raw.comment.anchor != CommentAnchor::Token {
            continue;
        }
        let prec = raw.preceding.unwrap_or_else(|| Token::empty(file));
        let owner = find_token_owner(tree, root, prec, root);
        raw.owner = Some(owner);
        counters.entry((owner, raw.comment.token)).or_insert(0);
    }

    let mut quiet = DiagBag::new();
    let mut scratch_tree = SyntaxTree::with_budget(0);
    let mut lx = Lexer::new(source, file);
    lx.ignore_comments = true;
    lx.recovering = true;

    let mut c_idx = 0usize;
    let mut previous = Token::empty(file);
    let mut previous_index = 0u32;

    loop {
        let token = lx.next_token(&mut scratch_tree, &mut quiet);
        if token.kind == TokenKind::Nl {
            continue;
        }
        if token.kind == TokenKind::Error {
            lx.resume();
            if lx.pos == source.len() {
                break;
            }
            continue;
        }
        while c_idx < raw_comments.len()
            && raw_comments[c_idx].comment.span.start < token.span.start
        {
            let raw = &mut raw_comments[c_idx];
            if raw.comment.anchor == CommentAnchor::Token
                && let Some(prec) = raw.preceding
                && prec.span.start == previous.span.start
                && prec.kind == previous.kind
            {
                raw.comment.token_index = previous_index;
                if let Some((boundary, idx)) = find_item_owner(tree, root, previous, token.kind) {
                    raw.comment.anchor = CommentAnchor::Item;
                    raw.comment.token = TokenKind::Eof;
                    raw.comment.token_index = idx;
                    raw.owner = Some(boundary);
                }
            }
            c_idx += 1;
        }
        if token.kind == TokenKind::Eof {
            break;
        }
        let owner = find_token_owner(tree, root, token, root);
        previous_index = if let Some(slot) = counters.get_mut(&(owner, token.kind)) {
            let idx = *slot;
            *slot += 1;
            idx
        } else {
            0
        };
        previous = token;
    }
    true
}

fn blank_between(source: &SourceFile, start: u32, end: u32) -> bool {
    let bytes = source.bytes();
    let mut whole_line = false;
    let mut empty = true;
    for &byte in &bytes[start as usize..end as usize] {
        if byte == b'\n' {
            if whole_line && empty {
                return true;
            }
            whole_line = true;
            empty = true;
        } else if !matches!(byte, b' ' | b'\t' | b'\r') {
            empty = false;
        }
    }
    false
}

fn content_end(tree: &SyntaxTree, nid: NodeId) -> u32 {
    let n = tree.node(nid);
    if matches!(
        n.kind,
        SyntaxKind::Arm | SyntaxKind::IfPart | SyntaxKind::Else
    ) {
        let body_idx = match n.kind {
            SyntaxKind::Arm => 2,
            SyntaxKind::IfPart => 1,
            _ => 0,
        };
        if let Some(body_id) = n.children.get(body_idx).copied().flatten() {
            let body = tree.node(body_id);
            if body.kind == SyntaxKind::Sequence {
                if let Some(last_id) = body.children.last().copied().flatten() {
                    return tree.node(last_id).span.end;
                }
                if n.kind != SyntaxKind::Else
                    && let Some(first_id) = n.children.first().copied().flatten()
                {
                    return tree.node(first_id).span.end;
                }
                return n.span.start;
            }
        }
    }
    n.span.end
}

fn statement_blanks(
    source: &SourceFile,
    tree: &mut SyntaxTree,
    seq_id: NodeId,
    mut previous: Option<NodeId>,
) {
    let count = tree.node(seq_id).children.len();
    for i in 0..count {
        let Some(nid) = tree.node(seq_id).children[i] else {
            continue;
        };
        let start = tree.node(nid).span.start;
        let end = match previous {
            None => start,
            Some(prev) => content_end(tree, prev),
        };
        let has_blank = end < start && blank_between(source, end, start);
        tree.node_mut(nid).blank_before = has_blank;
        previous = Some(nid);
    }
}

fn blank_trivia(source: &SourceFile, tree: &mut SyntaxTree, root: NodeId) {
    // Iterative post-order walk so arbitrarily wide/nested sequences never overflow stack.
    let mut stack: Vec<(NodeId, bool)> = vec![(root, false)];
    while let Some((nid, expanded)) = stack.pop() {
        if !expanded {
            stack.push((nid, true));
            let count = tree.node(nid).children.len();
            for i in (0..count).rev() {
                if let Some(cid) = tree.node(nid).children[i] {
                    stack.push((cid, false));
                }
            }
            continue;
        }
        let kind = tree.node(nid).kind;
        let child = |t: &SyntaxTree, idx: usize| t.node(nid).children.get(idx).copied().flatten();
        match kind {
            SyntaxKind::File => {
                if let Some(items) = child(tree, 1) {
                    let prev = child(tree, 0);
                    statement_blanks(source, tree, items, prev);
                }
            }
            SyntaxKind::Fn => {
                let spec = child(tree, 4);
                let body = child(tree, 5);
                let mut last_spec = None;
                if let Some(spec_id) = spec {
                    statement_blanks(source, tree, spec_id, None);
                    last_spec = tree.node(spec_id).children.last().copied().flatten();
                }
                if let Some(body_id) = body {
                    statement_blanks(source, tree, body_id, last_spec);
                }
            }
            SyntaxKind::Promise => {
                let lines = child(tree, 1);
                let anchors = child(tree, 2);
                let mut last_line = None;
                if let Some(lines_id) = lines {
                    statement_blanks(source, tree, lines_id, None);
                    last_line = tree.node(lines_id).children.last().copied().flatten();
                }
                if let Some(anchors_id) = anchors {
                    statement_blanks(source, tree, anchors_id, last_line);
                }
            }
            SyntaxKind::Struct | SyntaxKind::Enum | SyntaxKind::For | SyntaxKind::Arm => {
                if let Some(body_id) = child(tree, 2)
                    && tree.node(body_id).kind == SyntaxKind::Sequence
                {
                    statement_blanks(source, tree, body_id, None);
                }
            }
            SyntaxKind::Test
            | SyntaxKind::Actor
            | SyntaxKind::ApiScope
            | SyntaxKind::While
            | SyntaxKind::If
            | SyntaxKind::IfPart
            | SyntaxKind::Case
            | SyntaxKind::Lambda => {
                if let Some(body_id) = child(tree, 1)
                    && tree.node(body_id).kind == SyntaxKind::Sequence
                {
                    statement_blanks(source, tree, body_id, None);
                }
            }
            SyntaxKind::Else | SyntaxKind::Examples | SyntaxKind::Service | SyntaxKind::Api => {
                if let Some(body_id) = child(tree, 0)
                    && tree.node(body_id).kind == SyntaxKind::Sequence
                {
                    statement_blanks(source, tree, body_id, None);
                }
            }
            SyntaxKind::Module
            | SyntaxKind::Expose
            | SyntaxKind::ExposeAll
            | SyntaxKind::Use
            | SyntaxKind::Param
            | SyntaxKind::Where
            | SyntaxKind::Field
            | SyntaxKind::CaseDecl
            | SyntaxKind::Const
            | SyntaxKind::ServiceState
            | SyntaxKind::ServiceTools
            | SyntaxKind::ServiceEvery
            | SyntaxKind::ServiceToken
            | SyntaxKind::ActorCalls
            | SyntaxKind::ApiPort
            | SyntaxKind::ApiCors
            | SyntaxKind::ApiSqlite
            | SyntaxKind::ApiActor
            | SyntaxKind::ApiAuth
            | SyntaxKind::ApiRoute
            | SyntaxKind::ApiStatic
            | SyntaxKind::ApiJob
            | SyntaxKind::ApiStatus
            | SyntaxKind::Anchor
            | SyntaxKind::Type
            | SyntaxKind::Sequence
            | SyntaxKind::ExprStmt
            | SyntaxKind::Bind
            | SyntaxKind::Var
            | SyntaxKind::AddAssign
            | SyntaxKind::SubAssign
            | SyntaxKind::Return
            | SyntaxKind::Break
            | SyntaxKind::Continue
            | SyntaxKind::Assert
            | SyntaxKind::Binder
            | SyntaxKind::LineIf
            | SyntaxKind::Binary
            | SyntaxKind::Negate
            | SyntaxKind::Not
            | SyntaxKind::Try
            | SyntaxKind::Is
            | SyntaxKind::Range
            | SyntaxKind::Call
            | SyntaxKind::TypeArgs
            | SyntaxKind::NamedArg
            | SyntaxKind::FieldAccess
            | SyntaxKind::Qualified
            | SyntaxKind::Position
            | SyntaxKind::Int
            | SyntaxKind::Decimal
            | SyntaxKind::Char
            | SyntaxKind::Text
            | SyntaxKind::TextInterpolated
            | SyntaxKind::True
            | SyntaxKind::False
            | SyntaxKind::Name
            | SyntaxKind::CapName
            | SyntaxKind::Underscore
            | SyntaxKind::Group
            | SyntaxKind::Tuple
            | SyntaxKind::List
            | SyntaxKind::Rest
            | SyntaxKind::Map
            | SyntaxKind::MapEntry
            | SyntaxKind::Stop
            | SyntaxKind::Token
            | SyntaxKind::BinaryChain
            | SyntaxKind::Postfix
            | SyntaxKind::Prefix => {}
        }
    }
}

pub(crate) fn attach_trivia(
    source: &SourceFile,
    file: u32,
    tree: &mut SyntaxTree,
    root: NodeId,
    mut raw_comments: Vec<RawComment>,
) -> bool {
    if raw_comments.is_empty() {
        blank_trivia(source, tree, root);
        return true;
    }

    let nodes = count_nodes(tree, root);
    let mut scratch_budget = tree.remaining_budget();
    let Some(starts_cost) = nodes.checked_mul(CANDIDATE_SCRATCH_COST) else {
        return false;
    };
    if starts_cost > scratch_budget {
        return false;
    }
    scratch_budget -= starts_cost;

    if !attach_inline_anchors(
        source,
        file,
        tree,
        root,
        &mut raw_comments,
        &mut scratch_budget,
    ) {
        return false;
    }

    let mut starts = Vec::with_capacity(nodes);
    collect_candidates(tree, root, &mut starts);
    starts.sort_by_key(|c| (c.start, Reverse(c.end), c.kind));

    let bytes = source.bytes();
    let mut offset = 0u32;
    let mut column = 0u32;
    for cand in &mut starts {
        let target = cand.start;
        while offset < target {
            let pos = offset as usize;
            let (cp, width) = match decode_utf8(&bytes[pos..]) {
                Some((cp, w)) => (cp, w),
                None => (u32::from(bytes[pos]), 1),
            };
            if cp == u32::from(b'\n') {
                column = 0;
            } else if cp != u32::from(b'\r') || pos + 1 == bytes.len() || bytes[pos + 1] != b'\n' {
                column += 1;
            }
            offset += width;
        }
        cand.indentation = column;
    }

    // Attach in source order: an own-line comment attaches to the immediately following AST node
    // inside `enclosing`, rather than skipping ahead to an indentation-matched descendant or
    // falling to `enclosing.ending`. Only at a branch boundary (`Else`/`IfPart`/`Arm` starting at
    // `end`) does deeper indentation keep the comment inside the closing branch's `ending`.
    for raw in raw_comments {
        let anchor = raw.comment.anchor;
        let span = raw.comment.span;
        let indentation = raw.comment.indentation;
        let Some(cid) = tree.alloc_comment(raw.comment) else {
            return false;
        };
        if anchor != CommentAnchor::OwnLine {
            let owner = raw.owner.unwrap_or(root);
            tree.node_mut(owner).trailing.push(cid);
            continue;
        }
        let mut end = tree.node(root).span.end;
        let enclosing = find_owner(tree, root, span.start, root, &mut end);
        let low = starts.partition_point(|cand| cand.start < span.end);
        if low < starts.len() && starts[low].start <= end {
            let next = &starts[low];
            let at_branch_boundary = next.start == end
                && matches!(
                    next.kind,
                    SyntaxKind::Else | SyntaxKind::IfPart | SyntaxKind::Arm
                )
                && indentation > next.indentation;
            if at_branch_boundary {
                tree.node_mut(enclosing).ending.push(cid);
            } else {
                tree.node_mut(next.node).leading.push(cid);
            }
        } else {
            tree.node_mut(enclosing).ending.push(cid);
        }
    }

    blank_trivia(source, tree, root);
    true
}
