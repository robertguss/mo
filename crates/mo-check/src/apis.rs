//! HTTP `api` block checking: ports, CORS, SQLite bindings, actors, static mounts, routes,
//! scopes, auth, jobs, status mappings, and capability remapping (`src/check/apis.c` parity).

use mo_base::{DiagBag, Diagnostic, Span};
use mo_def::ErrorCode;
use mo_def::limits::{
    MO_LIMIT_API_ACTORS, MO_LIMIT_API_JOBS, MO_LIMIT_API_PATH_SEGMENTS, MO_LIMIT_API_ROUTES,
    MO_LIMIT_API_SCOPE_DEPTH, MO_LIMIT_API_SQLITES, MO_LIMIT_API_STATICS, MO_LIMIT_API_STATUSES,
    MO_LIMIT_PROGRAM_CHECK_WORK, MO_LIMIT_SOURCE_TYPE_DEPTH,
};
use mo_syntax::{NodeId, SyntaxKind};

use crate::abilities::{AbilityArena, AbilityContext, SourceAbility};
use crate::checked::{
    ApiAuthMode, ApiJobReplyKind, ApiMethod, ApiParamKind, ApiReplyKind, CheckedApi,
    CheckedApiActor, CheckedApiAuth, CheckedApiJob, CheckedApiParam, CheckedApiRoute,
    CheckedApiSqlite, CheckedApiStatic, CheckedApiStatus, CheckedService,
};
use crate::ids::{
    CheckedFuncId, CheckedServiceId, FuncSigId, ModuleId, ProofId, SymIdx, SymRef, TypeId,
};
use crate::load::ModuleSet;
use crate::map_keys::{MapKeyBudget, MapKeyCategory, MapLiteralKey, canonicalize_map_literal_key};
use crate::modules::ModuleInterfaces;
use crate::signatures::{Signatures, resolve_service_type};
use crate::types::SourceTypeKind;

const API_CAP_ORDER: [&str; 10] = [
    "Sqlite",
    "Network",
    "Clock",
    "Random",
    "Files",
    "ReadFiles",
    "Env",
    "Console",
    "Command",
    "SecureRandom",
];

struct ApiCheck<'a> {
    modules: &'a ModuleSet,
    interfaces: &'a ModuleInterfaces,
    signatures: &'a mut Signatures,
    abilities_arena: &'a mut AbilityArena,
    services: &'a [CheckedService],
    sig_to_func: &'a [CheckedFuncId],
    diags: &'a mut DiagBag,
    arena_remaining: &'a mut usize,
    span: Span,
    equal: Vec<(TypeId, TypeId)>,
    req_type: Option<TypeId>,
    res_type: Option<TypeId>,
    json_type: Option<TypeId>,
    cap_used: [bool; API_CAP_ORDER.len()],
    failed: bool,
}

struct ApiMapBudget<'a, 'b> {
    state: &'a mut ApiCheck<'b>,
}

impl MapKeyBudget for ApiMapBudget<'_, '_> {
    fn charge_work(&mut self, amount: usize) -> bool {
        self.state.api_work(amount)
    }

    fn charge_bytes(&mut self, bytes: usize) -> bool {
        self.state.api_alloc(bytes, 1)
    }
}

struct RoutePathInfo {
    clean_path: String,
    shape: String,
    param_names: Vec<String>,
}

impl<'a> ApiCheck<'a> {
    fn api_report(
        &mut self,
        code: ErrorCode,
        message: impl Into<String>,
        reason: Option<&str>,
    ) -> bool {
        let code_num = code.number();
        let final_reason = if (108..=110).contains(&code_num) {
            match reason {
                None => Some("See the api block rules (reference 11.6).".to_string()),
                Some(r) if r.contains("(reference 11.6)") => Some(r.to_string()),
                Some(r) => {
                    let trimmed = r.strip_suffix('.').unwrap_or(r);
                    Some(format!("{trimmed} (reference 11.6)."))
                }
            }
        } else {
            reason.map(str::to_string)
        };
        if !self.failed {
            let msg = message.into();
            let mut diag = Diagnostic::new(code, self.span, msg.clone()).with_primary_text(msg);
            if let Some(r) = final_reason {
                diag = diag.with_reason(r);
            }
            let _ = self.diags.push(diag);
        }
        self.failed = true;
        false
    }

    fn api_work(&mut self, amount: usize) -> bool {
        if self.failed {
            return false;
        }
        let amount_u64 = amount as u64;
        if amount_u64 > MO_LIMIT_PROGRAM_CHECK_WORK.saturating_sub(self.signatures.work) {
            return self.api_report(
                ErrorCode::CheckProgramWork,
                "the program needs too much api-checking work",
                None,
            );
        }
        self.signatures.work += amount_u64;
        true
    }

    fn api_alloc(&mut self, count: usize, size: usize) -> bool {
        if self.failed {
            return false;
        }
        let bytes = count.saturating_mul(size);
        let aligned = bytes.checked_add(7).map_or(usize::MAX, |n| n & !7usize);
        if aligned > *self.arena_remaining {
            return self.api_report(
                ErrorCode::CheckNoMemory,
                "the checking arena has no room",
                None,
            );
        }
        *self.arena_remaining -= aligned;
        true
    }

    fn syntax_string(&mut self, module: ModuleId, node: NodeId) -> Option<String> {
        let bytes = self.modules.module(module).syntax.tree.node_text(node)?;
        if !self.api_alloc(bytes.len() + 1, 1) {
            return None;
        }
        Some(String::from_utf8_lossy(bytes).into_owned())
    }

    fn api_same_type(&mut self, left: TypeId, right: TypeId, depth: u32) -> bool {
        if !self.api_work(1) {
            return false;
        }
        if u64::from(depth) > MO_LIMIT_SOURCE_TYPE_DEPTH {
            return self.api_report(
                ErrorCode::CheckTypeDepth,
                "the type is nested too deeply",
                None,
            );
        }
        if left == right {
            return true;
        }
        let same_head = {
            let l = self.signatures.types.get(left);
            let r = self.signatures.types.get(right);
            l.kind == r.kind
                && l.module == r.module
                && l.owner == r.owner
                && l.arguments.len() == r.arguments.len()
                && l.name == r.name
        };
        if !same_head {
            return false;
        }
        for i in 0..self.equal.len() {
            if !self.api_work(1) {
                return false;
            }
            if self.equal[i] == (left, right) {
                return true;
            }
        }
        let arity = self.signatures.types.get(left).arguments.len();
        for i in 0..arity {
            let la = self.signatures.types.get(left).arguments[i];
            let ra = self.signatures.types.get(right).arguments[i];
            if !self.api_same_type(la, ra, depth + 1) {
                return false;
            }
        }
        if !self.api_alloc(1, 24) {
            return false;
        }
        self.equal.push((left, right));
        true
    }

    fn native_type(&self, ty: TypeId, name: &str) -> bool {
        self.signatures.types.get(ty).is_builtin_named(name)
    }

    fn is_path_scalar(&self, ty: TypeId) -> bool {
        let t = self.signatures.types.get(ty);
        t.kind == SourceTypeKind::Named
            && t.module.is_none()
            && matches!(
                t.name.as_str(),
                "Int" | "Byte" | "BigInt" | "Decimal" | "Bool" | "Text"
            )
    }

    fn is_query_scalar(&self, ty: TypeId) -> bool {
        self.is_path_scalar(ty) || self.native_type(ty, "Float")
    }

    fn cap_slot(&self, ty: TypeId) -> Option<usize> {
        let t = self.signatures.types.get(ty);
        if t.kind != SourceTypeKind::Named || t.module.is_some() {
            return None;
        }
        API_CAP_ORDER.iter().position(|&c| c == t.name)
    }

    fn is_actor_type(&self, ty: TypeId) -> bool {
        let t = self.signatures.types.get(ty);
        t.kind == SourceTypeKind::Named
            && t.owner
                .as_source()
                .is_some_and(|s| self.interfaces.symbol(s).decl_kind == SyntaxKind::Actor)
    }

    fn prove_ability_with_fallback(
        &mut self,
        ty: TypeId,
        ability: SourceAbility,
        fallback_code: ErrorCode,
        fallback_msg: &'static str,
        fallback_reason: &'static str,
    ) -> Option<ProofId> {
        let arena = std::mem::take(self.abilities_arena);
        let mut ctx = AbilityContext::with_arena(arena, self.interfaces, self.signatures, 0);
        let proof = ctx.prove_ability(ty, ability);
        let (arena, _cache, _work, err) = ctx.into_parts();
        *self.abilities_arena = arena;
        if let Some(pid) = proof {
            return Some(pid);
        }
        if let Some(fail) = err
            && fail.code.number() >= 1000
        {
            let _ = self.api_report(fail.code, fail.message, fail.reason);
        } else {
            let _ = self.api_report(fallback_code, fallback_msg, Some(fallback_reason));
        }
        None
    }

    fn check_type_abilities_in_api(&mut self, ty: TypeId) -> bool {
        let arena = std::mem::take(self.abilities_arena);
        let mut ctx = AbilityContext::with_arena(arena, self.interfaces, self.signatures, 0);
        let ok = ctx.check_type_abilities(ty);
        let (arena, _cache, _work, err) = ctx.into_parts();
        *self.abilities_arena = arena;
        if ok {
            return true;
        }
        if let Some(fail) = err {
            let _ = self.api_report(fail.code, fail.message, fail.reason);
        } else {
            let _ = self.api_report(
                ErrorCode::CheckWork,
                "the function needs too much ability-checking work",
                None,
            );
        }
        false
    }

    fn parse_u16_literal(&mut self, module: ModuleId, lit: NodeId) -> Option<u16> {
        let bytes = self
            .modules
            .module(module)
            .syntax
            .tree
            .node_text(lit)?
            .to_vec();
        let (base, mut pos) =
            if bytes.len() > 2 && bytes[0] == b'0' && matches!(bytes[1], b'x' | b'b') {
                (if bytes[1] == b'x' { 16u64 } else { 2u64 }, 2usize)
            } else {
                (10u64, 0usize)
            };
        let mut val = 0u64;
        let mut any_digit = false;
        while pos < bytes.len() {
            if !self.api_work(1) {
                return None;
            }
            let c = bytes[pos];
            pos += 1;
            if c == b'_' {
                continue;
            }
            let d = match c {
                b'0'..=b'9' => u64::from(c - b'0'),
                b'a'..=b'f' => u64::from(c - b'a') + 10,
                b'A'..=b'F' => u64::from(c - b'A') + 10,
                _ => return None,
            };
            if d >= base {
                return None;
            }
            any_digit = true;
            val = val * base + d;
            if val > u64::from(u16::MAX) {
                return None;
            }
        }
        if !any_digit {
            return None;
        }
        Some(val as u16)
    }

    fn decode_plain_text(&mut self, module: ModuleId, text_node: NodeId) -> Option<MapLiteralKey> {
        let tree = &self.modules.module(module).syntax.tree;
        let mut budget = ApiMapBudget { state: self };
        let key = canonicalize_map_literal_key(&mut budget, tree, text_node)?;
        debug_assert_eq!(key.category, MapKeyCategory::Text);
        Some(key)
    }

    fn parse_static_prefix(&mut self, module: ModuleId, lit: NodeId) -> Option<String> {
        self.span = self.modules.module(module).syntax.tree.node(lit).span;
        let raw = self.decode_plain_text(module, lit)?;
        if raw.bytes.len() < 2 || raw.bytes[0] != b'/' {
            let _ = self.api_report(
                ErrorCode::ApiBlock,
                "an api static prefix must be a non-root URL path starting with /",
                Some("Write a static mount prefix such as \"/assets\" or \"/static\"."),
            );
            return None;
        }
        let mut clean_len = raw.bytes.len();
        if clean_len > 1 && raw.bytes[clean_len - 1] == b'/' {
            clean_len -= 1;
        }
        if clean_len < 2 {
            let _ = self.api_report(
                ErrorCode::ApiBlock,
                "an api static prefix must be a non-root URL path starting with /",
                Some("Write a static mount prefix such as \"/assets\" or \"/static\"."),
            );
            return None;
        }
        for &c in &raw.bytes[..clean_len] {
            if c <= 0x20 || c == 0x7f || matches!(c, b'?' | b'#' | b':' | b'\\') {
                let _ = self.api_report(
                    ErrorCode::ApiBlock,
                    "an api static prefix contains an invalid character",
                    Some("Static URL prefixes cannot contain whitespace, ?, #, : or \\."),
                );
                return None;
            }
        }
        let mut pos = 1usize;
        while pos <= clean_len {
            let start = pos;
            while pos < clean_len && raw.bytes[pos] != b'/' {
                pos += 1;
            }
            let seg = &raw.bytes[start..pos];
            if seg.is_empty() || seg == b"." || seg == b".." {
                let _ = self.api_report(
                    ErrorCode::ApiBlock,
                    "an api static prefix has an empty or relative segment",
                    Some("Use clean URL path segments in static mount prefixes."),
                );
                return None;
            }
            pos += 1;
        }
        let clean = String::from_utf8_lossy(&raw.bytes[..clean_len]).into_owned();
        if url_prefixes_overlap(&clean, "/openapi.json") || url_prefixes_overlap(&clean, "/docs") {
            let _ = self.api_report(
                ErrorCode::ApiBlock,
                "an api static prefix conflicts with built-in /openapi.json or /docs",
                Some("Mount static assets on a distinct prefix such as \"/assets\"."),
            );
            return None;
        }
        Some(clean)
    }

    fn parse_scope_prefix(
        &mut self,
        module: ModuleId,
        lit: NodeId,
        parent_prefix: Option<&str>,
    ) -> Option<String> {
        self.span = self.modules.module(module).syntax.tree.node(lit).span;
        let raw = self.decode_plain_text(module, lit)?;
        if raw.bytes.is_empty() || raw.bytes[0] != b'/' {
            let _ = self.api_report(
                ErrorCode::ApiBlock,
                "an api scope prefix must start with /",
                Some("Write a scope prefix starting with /, such as \"/bills\"."),
            );
            return None;
        }
        for &c in &raw.bytes {
            if c <= 0x20 || c == 0x7f || matches!(c, b'?' | b'#') {
                let _ = self.api_report(
                    ErrorCode::ApiBlock,
                    "an api scope prefix contains an invalid character",
                    Some("Scope prefixes cannot contain whitespace, control bytes, ? or #."),
                );
                return None;
            }
        }
        let mut clean_len = raw.bytes.len();
        if clean_len > 1 && raw.bytes[clean_len - 1] == b'/' {
            clean_len -= 1;
        }
        if clean_len > 1 {
            let mut pos = 1usize;
            while pos <= clean_len {
                let start = pos;
                while pos < clean_len && raw.bytes[pos] != b'/' {
                    pos += 1;
                }
                if pos == start {
                    let _ = self.api_report(
                        ErrorCode::ApiBlock,
                        "an api scope prefix has an empty segment",
                        Some("Avoid repeated slashes in scope prefixes."),
                    );
                    return None;
                }
                pos += 1;
            }
        }
        let sub = String::from_utf8_lossy(&raw.bytes[..clean_len]).into_owned();
        let Some(parent) = parent_prefix else {
            return Some(sub);
        };
        if parent == "/" {
            return Some(sub);
        }
        if sub == "/" {
            return Some(parent.to_string());
        }
        Some(format!("{parent}{sub}"))
    }

    fn parse_route_path(
        &mut self,
        module: ModuleId,
        lit: NodeId,
        scope_prefix: Option<&str>,
    ) -> Option<RoutePathInfo> {
        self.span = self.modules.module(module).syntax.tree.node(lit).span;
        let raw = self.decode_plain_text(module, lit)?;
        let full_bytes: Vec<u8> = if let Some(prefix) = scope_prefix {
            if !raw.bytes.is_empty() && raw.bytes[0] != b'/' {
                let _ = self.api_report(
                    ErrorCode::ApiRoute,
                    "an api route path inside scope must be \"\" or start with /",
                    Some(
                        "Write \"\" for the scope root or a suffix starting with /, such as \
                         \"/:id\".",
                    ),
                );
                return None;
            }
            if raw.bytes.is_empty() || raw.bytes == b"/" {
                prefix.as_bytes().to_vec()
            } else if prefix == "/" {
                raw.bytes
            } else {
                let mut buf = Vec::with_capacity(prefix.len() + raw.bytes.len());
                buf.extend_from_slice(prefix.as_bytes());
                buf.extend_from_slice(&raw.bytes);
                buf
            }
        } else {
            if raw.bytes.is_empty() || raw.bytes[0] != b'/' {
                let _ = self.api_report(
                    ErrorCode::ApiRoute,
                    "an api route path must start with /",
                    Some("Write a path template starting with /, such as \"/users/:id\"."),
                );
                return None;
            }
            raw.bytes
        };
        for &c in &full_bytes {
            if c <= 0x20 || c == 0x7f || matches!(c, b'?' | b'#') {
                let _ = self.api_report(
                    ErrorCode::ApiRoute,
                    "an api route path contains an invalid character",
                    Some("Route templates cannot contain whitespace, control bytes, ? or #."),
                );
                return None;
            }
        }
        let mut clean_len = full_bytes.len();
        if clean_len > 1 && full_bytes[clean_len - 1] == b'/' {
            clean_len -= 1;
        }
        let clean_bytes = &full_bytes[..clean_len];
        let clean_path = String::from_utf8_lossy(clean_bytes).into_owned();
        if clean_len == 1 {
            return Some(RoutePathInfo {
                clean_path,
                shape: "/".to_string(),
                param_names: Vec::new(),
            });
        }
        let mut shape = String::with_capacity(clean_len + 2);
        let mut param_names: Vec<String> = Vec::new();
        let mut seg_count = 0u64;
        let mut pos = 1usize;
        while pos <= clean_len {
            let start = pos;
            while pos < clean_len && clean_bytes[pos] != b'/' {
                pos += 1;
            }
            let seg = &clean_bytes[start..pos];
            if seg.is_empty() {
                let _ = self.api_report(
                    ErrorCode::ApiRoute,
                    "an api route path has an empty segment",
                    Some("Avoid repeated slashes in route paths."),
                );
                return None;
            }
            seg_count += 1;
            if seg_count > MO_LIMIT_API_PATH_SEGMENTS {
                let _ = self.api_report(
                    ErrorCode::ApiRoute,
                    "an api route path has too many segments",
                    Some("An api route path has at most 64 segments."),
                );
                return None;
            }
            shape.push('/');
            if seg[0] == b':' {
                if seg.len() < 2 {
                    let _ = self.api_report(
                        ErrorCode::ApiRoute,
                        "an api path parameter has no name",
                        Some("Name path parameters after :, such as :id."),
                    );
                    return None;
                }
                let ok_first = seg[1].is_ascii_lowercase() || seg[1] == b'_';
                let mut ok_name = ok_first;
                for (k, &ch) in seg.iter().enumerate().skip(2) {
                    let ok_ch = ch.is_ascii_lowercase()
                        || ch.is_ascii_digit()
                        || ch == b'_'
                        || (ch == b'?' && k + 1 == seg.len());
                    ok_name = ok_name && ok_ch;
                }
                if !ok_name {
                    let _ = self.api_report(
                        ErrorCode::ApiRoute,
                        "an api path parameter is not a valid parameter name",
                        Some("Use snake_case parameter names matching the handler's parameter."),
                    );
                    return None;
                }
                let pname = String::from_utf8_lossy(&seg[1..]).into_owned();
                if param_names.iter().any(|p| p == &pname) {
                    let _ = self.api_report(
                        ErrorCode::ApiRoute,
                        "an api route repeats a path parameter name",
                        Some("Each :parameter in a route path must have a distinct name."),
                    );
                    return None;
                }
                param_names.push(pname);
                shape.push(':');
            } else {
                shape.push_str(&String::from_utf8_lossy(seg));
            }
            pos += 1;
        }
        Some(RoutePathInfo {
            clean_path,
            shape,
            param_names,
        })
    }

    fn resolve_api_fn(
        &mut self,
        api_module: ModuleId,
        handler_node: NodeId,
        err_code: ErrorCode,
        role: &str,
    ) -> Option<(FuncSigId, CheckedFuncId)> {
        let tree = &self.modules.module(api_module).syntax.tree;
        let hnode = tree.node(handler_node);
        self.span = hnode.span;
        let scope = self.interfaces.scope(api_module);

        let group_ref = if hnode.kind == SyntaxKind::Qualified {
            let prefix = hnode.children.first().copied().flatten()?;
            let fname = hnode.children.get(1).copied().flatten()?;
            let pbytes = tree.node_text(prefix)?;
            let alias = scope.aliases.iter().find(|a| a.name.as_bytes() == pbytes);
            let Some(al) = alias else {
                self.span = tree.node(prefix).span;
                let _ = self.api_report(
                    ErrorCode::ImportMissing,
                    "a module prefix isn't in scope",
                    Some("Use names the modules available to this file (reference 10.3)."),
                );
                return None;
            };
            let target_mod = al.module;
            let origin = al.origin;
            let origin_span = al.origin_span;
            if !self.signatures.use_import(api_module, origin, origin_span) {
                let _ = self.api_report(
                    ErrorCode::CheckProgramWork,
                    "the program needs too much import-checking work",
                    None,
                );
                return None;
            }
            let target_scope = self.interfaces.scope(target_mod);
            let fbytes = tree.node_text(fname)?;
            let found = target_scope
                .find_symbol_index(fbytes)
                .map(|idx| SymRef::new(target_mod, idx));
            let Some(sym_ref) =
                found.filter(|&s| self.interfaces.symbol(s).decl_kind == SyntaxKind::Fn)
            else {
                self.span = tree.node(fname).span;
                let _ = self.api_report(
                    err_code,
                    format!("an api {role} names an unknown function"),
                    Some("Name an exposed function declared in the imported module."),
                );
                return None;
            };
            let sym = self.interfaces.symbol(sym_ref);
            if !sym.exported && sym.module != api_module {
                self.span = tree.node(fname).span;
                let _ = self.api_report(
                    ErrorCode::ImportPrivate,
                    "everything not on expose is private to the module",
                    Some("Everything not on expose is private to the module (reference 10.1)."),
                );
                return None;
            }
            sym_ref
        } else {
            let fbytes = tree.node_text(handler_node)?;
            let found = scope
                .find_symbol_index(fbytes)
                .map(|idx| SymRef::new(api_module, idx));
            let Some(sym_ref) =
                found.filter(|&s| self.interfaces.symbol(s).decl_kind == SyntaxKind::Fn)
            else {
                let _ = self.api_report(
                    err_code,
                    format!("an api {role} names an unknown local function"),
                    Some(
                        "Name a function declared in the same module as the api block, or \
                         Module.fn for an imported module.",
                    ),
                );
                return None;
            };
            sym_ref
        };

        if self.interfaces.symbol(group_ref).overload_count > 1 {
            let _ = self.api_report(
                err_code,
                format!("an api {role} function cannot be overloaded"),
                Some("Give each api route or auth handler a distinct function name."),
            );
            return None;
        }
        let (sig_id, sig) = self.signatures.function_signature_by_symbol(group_ref)?;
        let fn_ty = sig.ty?;
        if self.signatures.types.get(fn_ty).parameters != 0 {
            let _ = self.api_report(
                err_code,
                format!("an api {role} function cannot be generic"),
                Some(
                    "Api handlers have concrete types; wrap generic helpers in a concrete \
                     function.",
                ),
            );
            return None;
        }
        Some((sig_id, self.sig_to_func[sig_id.index()]))
    }

    fn check_error_type(
        &mut self,
        api_module: ModuleId,
        err_ty: TypeId,
        span: Span,
    ) -> Option<Option<ProofId>> {
        self.span = span;
        let res_type = self.res_type?;
        if self.api_same_type(err_ty, res_type, 0) {
            return Some(None);
        }
        if self.failed {
            return None;
        }
        let (kind, owner, params, arg_count, err_mod) = {
            let t = self.signatures.types.get(err_ty);
            (t.kind, t.owner, t.parameters, t.arguments.len(), t.module)
        };
        let Some(owner_sym) = owner.as_source().filter(|&s| {
            kind == SourceTypeKind::Named
                && self.interfaces.symbol(s).decl_kind == SyntaxKind::Enum
                && params == 0
                && arg_count == 0
        }) else {
            let _ = self.api_report(
                ErrorCode::ApiStatus,
                "an api error type must be a concrete enum or Response",
                Some(
                    "Return Result(T, AppError) where AppError is a non-generic enum with status \
                     clauses, or Result(T, Response).",
                ),
            );
            return None;
        };
        if err_mod != Some(api_module) && !self.interfaces.symbol(owner_sym).insides {
            let _ = self.api_report(
                ErrorCode::ApiStatus,
                "an api error enum's cases are hidden from this module",
                Some("Expose the error enum with (..) so the api block can match its cases."),
            );
            return None;
        }
        let proof = self.prove_ability_with_fallback(
            err_ty,
            SourceAbility::Json,
            ErrorCode::ApiStatus,
            "an api error enum lacks the Json ability",
            "Every case field of an api error enum must support Json encoding.",
        )?;
        Some(Some(proof))
    }

    fn find_sqlite_slot(sqlites: &[CheckedApiSqlite], name: &str) -> Option<u32> {
        let stripped = name
            .strip_prefix('_')
            .filter(|s| !s.is_empty())
            .unwrap_or(name);
        sqlites
            .iter()
            .position(|s| s.name == name || s.name == stripped)
            .map(|i| i as u32)
    }

    fn find_actor_slot(
        &mut self,
        actors: &[CheckedApiActor],
        name: &str,
        ty: TypeId,
    ) -> Option<u32> {
        let stripped = name
            .strip_prefix('_')
            .filter(|s| !s.is_empty())
            .unwrap_or(name);
        for (i, actor) in actors.iter().enumerate() {
            if (actor.name == name || actor.name == stripped) && self.api_same_type(ty, actor.ty, 0)
            {
                return Some(i as u32);
            }
        }
        None
    }

    fn check_auth_clause(&mut self, api: &mut CheckedApi, clause: NodeId) -> bool {
        let api_module = api.module;
        let tree = &self.modules.module(api_module).syntax.tree;
        self.span = tree.node(clause).span;
        let Some(principal) = resolve_service_type(
            self.modules,
            self.interfaces,
            self.signatures,
            api_module,
            clause,
            self.diags,
        ) else {
            self.failed = true;
            return false;
        };
        if !self.check_type_abilities_in_api(principal) {
            return false;
        }
        let Some(req_ty) = self.req_type else {
            return false;
        };
        let Some(res_ty) = self.res_type else {
            return false;
        };
        let (p_kind, p_owner, p_params) = {
            let t = self.signatures.types.get(principal);
            (t.kind, t.owner, t.parameters)
        };
        let valid_nominal = p_params == 0
            && p_kind == SourceTypeKind::Named
            && p_owner.as_source().is_some_and(|s| {
                matches!(
                    self.interfaces.symbol(s).decl_kind,
                    SyntaxKind::Struct | SyntaxKind::Enum
                )
            });
        if !valid_nominal
            || self.api_same_type(principal, req_ty, 0)
            || self.api_same_type(principal, res_ty, 0)
        {
            return self.api_report(
                ErrorCode::ApiBlock,
                "an api auth principal must be a concrete struct or enum",
                Some("Declare a nominal principal type such as struct CurrentUser."),
            );
        }
        let Some(fn_node) = tree.node(clause).children.get(1).copied().flatten() else {
            return false;
        };
        let Some((sig_id, fn_idx)) =
            self.resolve_api_fn(api_module, fn_node, ErrorCode::ApiBlock, "auth")
        else {
            return false;
        };
        let sig = &self.signatures.functions[sig_id.index()];
        let fn_sym = sig.function;
        let Some(sig_ty) = sig.ty else {
            return false;
        };
        let fn_args = self.signatures.types.get(sig_ty).arguments.clone();
        let fn_mod = fn_sym.module;
        let fn_tree = &self.modules.module(fn_mod).syntax.tree;
        let fn_decl = self.interfaces.symbol(fn_sym).declaration;
        let Some(param_nodes_id) = fn_tree.node(fn_decl).children.get(1).copied().flatten() else {
            return false;
        };
        let param_children = fn_tree.node(param_nodes_id).children.clone();
        let param_count = param_children.len();
        let mut params = Vec::with_capacity(param_count);
        let mut seen_req = false;
        let mut seen_caps = 0u32;
        let mut seen_dbs = 0u32;
        let mut seen_actors = 0u32;

        for (i, ch) in param_children.iter().enumerate() {
            let Some(pnode) = *ch else {
                return false;
            };
            self.span = fn_tree.node(pnode).span;
            let Some(pname_node) = fn_tree.node(pnode).children.first().copied().flatten() else {
                return false;
            };
            let Some(pname) = self.syntax_string(fn_mod, pname_node) else {
                return false;
            };
            let ptype = fn_args[i];
            if self.api_same_type(ptype, req_ty, 0) {
                if seen_req {
                    return self.api_report(
                        ErrorCode::ApiBlock,
                        "an api auth function takes Request twice",
                        None,
                    );
                }
                seen_req = true;
                params.push(CheckedApiParam {
                    kind: ApiParamKind::Request,
                    name: pname,
                    ty: ptype,
                    index: 0,
                    json: None,
                });
                continue;
            }
            if self.native_type(ptype, "Sqlite.Database") {
                let Some(db_slot) = Self::find_sqlite_slot(&api.sqlites, &pname) else {
                    return self.api_report(
                        ErrorCode::ApiBlock,
                        "an api auth Sqlite.Database parameter has no matching sqlite clause",
                        Some("Declare sqlite <name> from \"<path>\" in the api block with the same name."),
                    );
                };
                if (seen_dbs & (1u32 << db_slot)) != 0 {
                    return self.api_report(
                        ErrorCode::ApiBlock,
                        "an api auth function takes the same sqlite database twice",
                        None,
                    );
                }
                seen_dbs |= 1u32 << db_slot;
                params.push(CheckedApiParam {
                    kind: ApiParamKind::SqliteDb,
                    name: pname,
                    ty: ptype,
                    index: db_slot,
                    json: None,
                });
                continue;
            }
            if self.is_actor_type(ptype) {
                let Some(actor_slot) = self.find_actor_slot(&api.actors, &pname, ptype) else {
                    return if self.failed {
                        false
                    } else {
                        self.api_report(
                            ErrorCode::ApiBlock,
                            "an api auth actor parameter has no matching actor clause",
                            Some(
                                "Declare actor <name>: <ActorType> in the api block with the same \
                                 name and type.",
                            ),
                        )
                    };
                };
                if (seen_actors & (1u32 << actor_slot)) != 0 {
                    return self.api_report(
                        ErrorCode::ApiBlock,
                        "an api auth function takes the same actor twice",
                        None,
                    );
                }
                seen_actors |= 1u32 << actor_slot;
                params.push(CheckedApiParam {
                    kind: ApiParamKind::Actor,
                    name: pname,
                    ty: ptype,
                    index: actor_slot,
                    json: None,
                });
                continue;
            }
            if let Some(cap_slot) = self.cap_slot(ptype) {
                if (seen_caps & (1u32 << cap_slot)) != 0 {
                    return self.api_report(
                        ErrorCode::ApiBlock,
                        "an api auth function repeats a capability type",
                        None,
                    );
                }
                seen_caps |= 1u32 << cap_slot;
                self.cap_used[cap_slot] = true;
                params.push(CheckedApiParam {
                    kind: ApiParamKind::Capability,
                    name: pname,
                    ty: ptype,
                    index: cap_slot as u32,
                    json: None,
                });
                continue;
            }
            return self.api_report(
                ErrorCode::ApiBlock,
                "an api auth function has an unsupported parameter",
                Some("An auth function takes Request, declared sqlite databases, or capabilities."),
            );
        }

        let ret = fn_args[fn_args.len() - 1];
        let ret_span = fn_tree
            .node(fn_decl)
            .children
            .get(2)
            .copied()
            .flatten()
            .map_or_else(|| tree.node(clause).span, |n| fn_tree.node(n).span);
        self.span = ret_span;
        let ret_args = self.signatures.types.get(ret).arguments.clone();
        if !self.native_type(ret, "Result")
            || ret_args.len() < 2
            || !self.api_same_type(ret_args[0], principal, 0)
        {
            return self.api_report(
                ErrorCode::ApiBlock,
                "an api auth function must return Result(Principal, E)",
                Some("Return Result(<Type>, <ErrorEnum>) matching the auth <Type> clause."),
            );
        }
        let Some(err_json) = self.check_error_type(api_module, ret_args[1], ret_span) else {
            return false;
        };
        api.auth = Some(CheckedApiAuth {
            syntax: clause,
            principal,
            function: fn_idx,
            params,
            error_type: ret_args[1],
            error_json: err_json,
        });
        true
    }

    fn parse_method_word(&self, module: ModuleId, word: NodeId) -> ApiMethod {
        match self.modules.module(module).syntax.tree.node_text(word) {
            Some(b"get") => ApiMethod::Get,
            Some(b"post") => ApiMethod::Post,
            Some(b"put") => ApiMethod::Put,
            Some(b"patch") => ApiMethod::Patch,
            _ => ApiMethod::Delete,
        }
    }

    fn check_route_clause(
        &mut self,
        api: &CheckedApi,
        clause: NodeId,
        scope_prefix: Option<&str>,
        shapes: &mut Vec<String>,
        routes: &[CheckedApiRoute],
    ) -> Option<CheckedApiRoute> {
        let api_module = api.module;
        let tree = &self.modules.module(api_module).syntax.tree;
        let c0 = tree.node(clause).children.first().copied().flatten()?;
        let c1 = tree.node(clause).children.get(1).copied().flatten()?;
        let c2 = tree.node(clause).children.get(2).copied().flatten()?;
        let method = self.parse_method_word(api_module, c0);
        let path_info = self.parse_route_path(api_module, c1, scope_prefix)?;
        for (i, prev_shape) in shapes.iter().enumerate() {
            if !self.api_work(1) {
                return None;
            }
            if routes[i].method == method && prev_shape == &path_info.shape {
                self.span = tree.node(c1).span;
                let _ = self.api_report(
                    ErrorCode::ApiRoute,
                    "two api routes have the same HTTP method and path shape",
                    Some("Each (method, path) pair in an api block must be distinct."),
                );
                return None;
            }
        }
        shapes.push(path_info.shape);

        let (sig_id, fn_idx) = self.resolve_api_fn(api_module, c2, ErrorCode::ApiRoute, "route")?;
        let sig = &self.signatures.functions[sig_id.index()];
        let fn_sym = sig.function;
        let sig_ty = sig.ty?;
        let fn_args = self.signatures.types.get(sig_ty).arguments.clone();
        let fn_mod = fn_sym.module;
        let fn_tree = &self.modules.module(fn_mod).syntax.tree;
        let fn_decl = self.interfaces.symbol(fn_sym).declaration;
        let param_nodes_id = fn_tree.node(fn_decl).children.get(1).copied().flatten()?;
        let param_children = fn_tree.node(param_nodes_id).children.clone();
        let param_count = param_children.len();
        let mut params = Vec::with_capacity(param_count);
        let mut matched_path = vec![false; path_info.param_names.len()];

        let req_ty = self.req_type?;
        let res_ty = self.res_type?;
        let mut auth_mode = ApiAuthMode::None;
        let mut seen_req = false;
        let mut seen_body = false;
        let mut seen_caps = 0u32;
        let mut seen_dbs = 0u32;
        let mut seen_actors = 0u32;
        let allows_body = matches!(method, ApiMethod::Post | ApiMethod::Put | ApiMethod::Patch);

        for (i, ch) in param_children.iter().enumerate() {
            let pnode = (*ch)?;
            self.span = fn_tree.node(pnode).span;
            let pname_node = fn_tree.node(pnode).children.first().copied().flatten()?;
            let pname = self.syntax_string(fn_mod, pname_node)?;
            let ptype = fn_args[i];

            if let Some(ref auth) = api.auth {
                if self.api_same_type(ptype, auth.principal, 0) {
                    if auth_mode != ApiAuthMode::None {
                        let _ = self.api_report(
                            ErrorCode::ApiRoute,
                            "an api route handler takes the auth principal twice",
                            None,
                        );
                        return None;
                    }
                    auth_mode = ApiAuthMode::Required;
                    params.push(CheckedApiParam {
                        kind: ApiParamKind::Auth,
                        name: pname,
                        ty: ptype,
                        index: 0,
                        json: None,
                    });
                    continue;
                }
                if self.native_type(ptype, "Option") {
                    let opt_inner = self.signatures.types.get(ptype).arguments[0];
                    if self.api_same_type(opt_inner, auth.principal, 0) {
                        if auth_mode != ApiAuthMode::None {
                            let _ = self.api_report(
                                ErrorCode::ApiRoute,
                                "an api route handler takes the auth principal twice",
                                None,
                            );
                            return None;
                        }
                        auth_mode = ApiAuthMode::Optional;
                        params.push(CheckedApiParam {
                            kind: ApiParamKind::AuthOptional,
                            name: pname,
                            ty: ptype,
                            index: 0,
                            json: None,
                        });
                        continue;
                    }
                }
            }
            if self.api_same_type(ptype, req_ty, 0) {
                if seen_req {
                    let _ = self.api_report(
                        ErrorCode::ApiRoute,
                        "an api route handler takes Request twice",
                        None,
                    );
                    return None;
                }
                seen_req = true;
                params.push(CheckedApiParam {
                    kind: ApiParamKind::Request,
                    name: pname,
                    ty: ptype,
                    index: 0,
                    json: None,
                });
                continue;
            }
            if self.native_type(ptype, "Sqlite.Database") {
                let Some(db_slot) = Self::find_sqlite_slot(&api.sqlites, &pname) else {
                    let _ = self.api_report(
                        ErrorCode::ApiRoute,
                        "an api route Sqlite.Database parameter has no matching sqlite clause",
                        Some("Declare sqlite <name> from \"<path>\" in the api block with the same name."),
                    );
                    return None;
                };
                if (seen_dbs & (1u32 << db_slot)) != 0 {
                    let _ = self.api_report(
                        ErrorCode::ApiRoute,
                        "an api route handler takes the same sqlite database twice",
                        None,
                    );
                    return None;
                }
                seen_dbs |= 1u32 << db_slot;
                params.push(CheckedApiParam {
                    kind: ApiParamKind::SqliteDb,
                    name: pname,
                    ty: ptype,
                    index: db_slot,
                    json: None,
                });
                continue;
            }
            if self.is_actor_type(ptype) {
                let Some(actor_slot) = self.find_actor_slot(&api.actors, &pname, ptype) else {
                    if !self.failed {
                        let _ = self.api_report(
                            ErrorCode::ApiRoute,
                            "an api route actor parameter has no matching actor clause",
                            Some(
                                "Declare actor <name>: <ActorType> in the api block with the same \
                                 name and type.",
                            ),
                        );
                    }
                    return None;
                };
                if (seen_actors & (1u32 << actor_slot)) != 0 {
                    let _ = self.api_report(
                        ErrorCode::ApiRoute,
                        "an api route handler takes the same actor twice",
                        None,
                    );
                    return None;
                }
                seen_actors |= 1u32 << actor_slot;
                params.push(CheckedApiParam {
                    kind: ApiParamKind::Actor,
                    name: pname,
                    ty: ptype,
                    index: actor_slot,
                    json: None,
                });
                continue;
            }
            if let Some(cap_slot) = self.cap_slot(ptype) {
                if (seen_caps & (1u32 << cap_slot)) != 0 {
                    let _ = self.api_report(
                        ErrorCode::ApiRoute,
                        "an api route handler repeats a capability type",
                        None,
                    );
                    return None;
                }
                seen_caps |= 1u32 << cap_slot;
                self.cap_used[cap_slot] = true;
                params.push(CheckedApiParam {
                    kind: ApiParamKind::Capability,
                    name: pname,
                    ty: ptype,
                    index: cap_slot as u32,
                    json: None,
                });
                continue;
            }
            if let Some(path_slot) = path_info.param_names.iter().position(|p| p == &pname) {
                if !self.is_path_scalar(ptype) {
                    let _ = self.api_report(
                        ErrorCode::ApiRoute,
                        "an api path parameter must be Int, Byte, BigInt, Decimal, Bool or Text",
                        Some("Path segments are decoded into scalar values."),
                    );
                    return None;
                }
                matched_path[path_slot] = true;
                params.push(CheckedApiParam {
                    kind: ApiParamKind::Path,
                    name: pname,
                    ty: ptype,
                    index: path_slot as u32,
                    json: None,
                });
                continue;
            }
            if self.native_type(ptype, "Option") {
                let opt_inner = self.signatures.types.get(ptype).arguments[0];
                if self.is_query_scalar(opt_inner) {
                    params.push(CheckedApiParam {
                        kind: ApiParamKind::QueryOptional,
                        name: pname,
                        ty: ptype,
                        index: 0,
                        json: None,
                    });
                    continue;
                }
            }
            if !allows_body {
                if self.is_query_scalar(ptype) {
                    params.push(CheckedApiParam {
                        kind: ApiParamKind::QueryRequired,
                        name: pname,
                        ty: ptype,
                        index: 0,
                        json: None,
                    });
                    continue;
                }
                let _ = self.api_report(
                    ErrorCode::ApiRoute,
                    "a GET or DELETE route parameter must be a path or scalar query parameter",
                    Some(
                        "Use scalar or Option(Scalar) query parameters on GET/DELETE, or \
                         POST/PUT/PATCH for a JSON request body.",
                    ),
                );
                return None;
            }
            if seen_body {
                let _ = self.api_report(
                    ErrorCode::ApiRoute,
                    "an api route handler takes more than one JSON body",
                    Some(
                        "Pass one struct or enum body parameter (and Option(Scalar) for optional \
                         query parameters).",
                    ),
                );
                return None;
            }
            let body_json = self.prove_ability_with_fallback(
                ptype,
                SourceAbility::Json,
                ErrorCode::ApiRoute,
                "an api request body parameter lacks the Json ability",
                "Request body types must support Json decoding.",
            )?;
            seen_body = true;
            params.push(CheckedApiParam {
                kind: ApiParamKind::Body,
                name: pname,
                ty: ptype,
                index: 0,
                json: Some(body_json),
            });
        }

        if matched_path.iter().any(|&m| !m) {
            self.span = tree.node(c1).span;
            let _ = self.api_report(
                ErrorCode::ApiRoute,
                "an api route path parameter has no matching parameter on the handler",
                Some("Add a parameter with the same name to the route handler."),
            );
            return None;
        }

        let ret = fn_args[fn_args.len() - 1];
        let ret_span = fn_tree
            .node(fn_decl)
            .children
            .get(2)
            .copied()
            .flatten()
            .map_or_else(|| tree.node(clause).span, |n| fn_tree.node(n).span);
        self.span = ret_span;
        let reply;
        let mut ok_ty = None;
        let mut err_ty = None;
        let mut ok_json = None;
        let mut err_json = None;

        if self.api_same_type(ret, res_ty, 0) {
            reply = ApiReplyKind::Response;
        } else if self.native_type(ret, "Unit") {
            reply = ApiReplyKind::Unit;
        } else if self.native_type(ret, "Result") {
            let rargs = self.signatures.types.get(ret).arguments.clone();
            let oty = rargs[0];
            let ety = rargs[1];
            ok_ty = Some(oty);
            err_ty = Some(ety);
            if self.api_same_type(oty, res_ty, 0) {
                reply = ApiReplyKind::ResultResponse;
            } else if self.native_type(oty, "Unit") {
                reply = ApiReplyKind::ResultUnit;
            } else {
                reply = ApiReplyKind::ResultJson;
                ok_json = Some(self.prove_ability_with_fallback(
                    oty,
                    SourceAbility::Json,
                    ErrorCode::ApiRoute,
                    "an api route return type lacks the Json ability",
                    "Route return types must be Response, Unit, or a Json type.",
                )?);
            }
            err_json = self.check_error_type(api_module, ety, ret_span)?;
        } else {
            if self.failed {
                return None;
            }
            ok_ty = Some(ret);
            reply = ApiReplyKind::Json;
            ok_json = Some(self.prove_ability_with_fallback(
                ret,
                SourceAbility::Json,
                ErrorCode::ApiRoute,
                "an api route return type lacks the Json ability",
                "Route return types must be Response, Unit, or a Json type.",
            )?);
        }

        Some(CheckedApiRoute {
            syntax: clause,
            method,
            path: path_info.clean_path,
            function: fn_idx,
            auth_mode,
            params,
            reply,
            ok_type: ok_ty,
            ok_json,
            error_type: err_ty,
            error_json: err_json,
        })
    }

    fn parse_job_interval_us(&mut self, module: ModuleId, node: NodeId) -> Option<u64> {
        let tree = &self.modules.module(module).syntax.tree;
        let n = tree.node(node);
        self.span = n.span;
        let (lit, unit_node) = if n.kind == SyntaxKind::FieldAccess
            && n.children.len() == 2
            && let Some(c0) = n.children[0]
            && let Some(c1) = n.children[1]
            && tree.node(c0).kind == SyntaxKind::Int
            && tree.node(c1).kind == SyntaxKind::Name
        {
            (c0, c1)
        } else {
            let _ = self.api_report(
                ErrorCode::ApiBlock,
                "an api job schedule must be a positive integer duration such as 15.minutes",
                Some("Write every <N>.seconds, <N>.minutes, <N>.hours or <N>.days."),
            );
            return None;
        };
        let unit_us: u64 = match tree.node_text(unit_node) {
            Some(b"second" | b"seconds") => 1_000_000,
            Some(b"minute" | b"minutes") => 60_000_000,
            Some(b"hour" | b"hours") => 3_600_000_000,
            Some(b"day" | b"days") => 86_400_000_000,
            _ => {
                self.span = tree.node(unit_node).span;
                let _ = self.api_report(
                    ErrorCode::ApiBlock,
                    "an api job schedule unit must be seconds, minutes, hours or days",
                    Some("Use .second(s), .minute(s), .hour(s) or .day(s) on a positive integer."),
                );
                return None;
            }
        };
        let bytes = tree.node_text(lit)?.to_vec();
        let (base, mut pos) =
            if bytes.len() > 2 && bytes[0] == b'0' && matches!(bytes[1], b'x' | b'b') {
                (if bytes[1] == b'x' { 16u64 } else { 2u64 }, 2usize)
            } else {
                (10u64, 0usize)
            };
        let max_count = (i64::MAX as u64) / unit_us;
        let mut val = 0u64;
        let mut any_digit = false;
        while pos < bytes.len() {
            if !self.api_work(1) {
                return None;
            }
            let c = bytes[pos];
            pos += 1;
            if c == b'_' {
                continue;
            }
            let d = match c {
                b'0'..=b'9' => u64::from(c - b'0'),
                b'a'..=b'f' => u64::from(c - b'a') + 10,
                b'A'..=b'F' => u64::from(c - b'A') + 10,
                _ => 99,
            };
            if d >= base || val > (max_count - d) / base {
                self.span = tree.node(lit).span;
                let _ = self.api_report(
                    ErrorCode::ApiBlock,
                    "an api job interval must be a positive integer duration in range",
                    Some("Use an integer duration of at least 1 second."),
                );
                return None;
            }
            any_digit = true;
            val = val * base + d;
        }
        if !any_digit || val == 0 {
            self.span = tree.node(lit).span;
            let _ = self.api_report(
                ErrorCode::ApiBlock,
                "an api job interval must be at least 1",
                Some("Use a positive integer duration such as 1.second or 15.minutes."),
            );
            return None;
        }
        Some(val * unit_us)
    }

    fn check_job_clause(
        &mut self,
        api: &CheckedApi,
        clause: NodeId,
        jobs: &[CheckedApiJob],
    ) -> Option<CheckedApiJob> {
        let api_module = api.module;
        let tree = &self.modules.module(api_module).syntax.tree;
        let c0 = tree.node(clause).children.first().copied().flatten()?;
        let c1 = tree.node(clause).children.get(1).copied().flatten()?;
        let c2 = tree.node(clause).children.get(2).copied().flatten();
        self.span = tree.node(c0).span;
        let jkey = self.decode_plain_text(api_module, c0)?;
        if !valid_job_name(&jkey.bytes) {
            let _ = self.api_report(
                ErrorCode::ApiBlock,
                "an api job name must be a nonempty ASCII identifier without whitespace",
                Some("Use a concise job name such as \"mercury.sync\" or \"csv.import\"."),
            );
            return None;
        }
        let jname = String::from_utf8_lossy(&jkey.bytes).into_owned();
        if jobs.iter().any(|j| j.name == jname) {
            let _ = self.api_report(
                ErrorCode::ApiBlock,
                "an api block declares two jobs with the same name",
                Some("Give each job clause in an api block a distinct name."),
            );
            return None;
        }
        let has_schedule = c2.is_some();
        let interval_us = match c2 {
            Some(sched) => self.parse_job_interval_us(api_module, sched)?,
            None => 0,
        };
        let (sig_id, fn_idx) = self.resolve_api_fn(api_module, c1, ErrorCode::ApiBlock, "job")?;
        let sig = &self.signatures.functions[sig_id.index()];
        let fn_sym = sig.function;
        let sig_ty = sig.ty?;
        let fn_args = self.signatures.types.get(sig_ty).arguments.clone();
        let fn_mod = fn_sym.module;
        let fn_tree = &self.modules.module(fn_mod).syntax.tree;
        let fn_decl = self.interfaces.symbol(fn_sym).declaration;
        let param_nodes_id = fn_tree.node(fn_decl).children.get(1).copied().flatten()?;
        let param_children = fn_tree.node(param_nodes_id).children.clone();
        let param_count = param_children.len();
        let json_ty = self.json_type?;
        if param_count == 0 || !self.api_same_type(fn_args[0], json_ty, 0) {
            self.span = param_children
                .first()
                .copied()
                .flatten()
                .map_or_else(|| tree.node(c1).span, |n| fn_tree.node(n).span);
            let _ = self.api_report(
                ErrorCode::ApiBlock,
                "an api job handler's first parameter must be payload: Json",
                Some("Write fn run(payload: Json, ...) : Result(Unit, E) or : Unit."),
            );
            return None;
        }
        let mut params = Vec::with_capacity(param_count);
        let p0_node = param_children[0]?;
        let p0_name_node = fn_tree.node(p0_node).children.first().copied().flatten()?;
        let p0_name = self.syntax_string(fn_mod, p0_name_node)?;
        params.push(CheckedApiParam {
            kind: ApiParamKind::Body,
            name: p0_name,
            ty: fn_args[0],
            index: 0,
            json: None,
        });

        let mut seen_caps = 0u32;
        let mut seen_dbs = 0u32;
        let mut seen_actors = 0u32;
        for (i, ch) in param_children.iter().enumerate().skip(1) {
            let pnode = (*ch)?;
            self.span = fn_tree.node(pnode).span;
            let pname_node = fn_tree.node(pnode).children.first().copied().flatten()?;
            let pname = self.syntax_string(fn_mod, pname_node)?;
            let ptype = fn_args[i];
            if self.native_type(ptype, "Sqlite.Database") {
                let Some(db_slot) = Self::find_sqlite_slot(&api.sqlites, &pname) else {
                    let _ = self.api_report(
                        ErrorCode::ApiBlock,
                        "an api job Sqlite.Database parameter has no matching sqlite clause",
                        Some("Declare sqlite <name> from \"<path>\" in the api block with the same name."),
                    );
                    return None;
                };
                if (seen_dbs & (1u32 << db_slot)) != 0 {
                    let _ = self.api_report(
                        ErrorCode::ApiBlock,
                        "an api job handler takes the same sqlite database twice",
                        None,
                    );
                    return None;
                }
                seen_dbs |= 1u32 << db_slot;
                params.push(CheckedApiParam {
                    kind: ApiParamKind::SqliteDb,
                    name: pname,
                    ty: ptype,
                    index: db_slot,
                    json: None,
                });
                continue;
            }
            if self.is_actor_type(ptype) {
                let Some(actor_slot) = self.find_actor_slot(&api.actors, &pname, ptype) else {
                    if !self.failed {
                        let _ = self.api_report(
                            ErrorCode::ApiBlock,
                            "an api job actor parameter has no matching actor clause",
                            Some(
                                "Declare actor <name>: <ActorType> in the api block with the same \
                                 name and type.",
                            ),
                        );
                    }
                    return None;
                };
                if (seen_actors & (1u32 << actor_slot)) != 0 {
                    let _ = self.api_report(
                        ErrorCode::ApiBlock,
                        "an api job handler takes the same actor twice",
                        None,
                    );
                    return None;
                }
                seen_actors |= 1u32 << actor_slot;
                params.push(CheckedApiParam {
                    kind: ApiParamKind::Actor,
                    name: pname,
                    ty: ptype,
                    index: actor_slot,
                    json: None,
                });
                continue;
            }
            if let Some(cap_slot) = self.cap_slot(ptype) {
                if (seen_caps & (1u32 << cap_slot)) != 0 {
                    let _ = self.api_report(
                        ErrorCode::ApiBlock,
                        "an api job handler repeats a capability type",
                        None,
                    );
                    return None;
                }
                seen_caps |= 1u32 << cap_slot;
                self.cap_used[cap_slot] = true;
                params.push(CheckedApiParam {
                    kind: ApiParamKind::Capability,
                    name: pname,
                    ty: ptype,
                    index: cap_slot as u32,
                    json: None,
                });
                continue;
            }
            let _ = self.api_report(
                ErrorCode::ApiBlock,
                "an api job handler has an unsupported parameter",
                Some(
                    "After payload: Json, pass declared sqlite databases, actors, or capabilities.",
                ),
            );
            return None;
        }

        let ret = fn_args[fn_args.len() - 1];
        self.span = fn_tree
            .node(fn_decl)
            .children
            .get(2)
            .copied()
            .flatten()
            .map_or_else(|| tree.node(clause).span, |n| fn_tree.node(n).span);
        let reply;
        let mut err_ty = None;
        let mut err_text = None;
        if self.native_type(ret, "Unit") {
            reply = ApiJobReplyKind::Unit;
        } else if self.native_type(ret, "Result")
            && self.native_type(self.signatures.types.get(ret).arguments[0], "Unit")
        {
            reply = ApiJobReplyKind::ResultUnit;
            let ety = self.signatures.types.get(ret).arguments[1];
            err_ty = Some(ety);
            err_text = Some(self.prove_ability_with_fallback(
                ety,
                SourceAbility::Text,
                ErrorCode::ApiBlock,
                "an api job error type lacks the Text ability",
                "Job handlers return Unit or Result(Unit, E) where E supports Text formatting.",
            )?);
        } else {
            let _ = self.api_report(
                ErrorCode::ApiBlock,
                "an api job handler must return Unit or Result(Unit, E)",
                Some("Write fn run(payload: Json, ...) : Result(Unit, E) or : Unit."),
            );
            return None;
        }

        Some(CheckedApiJob {
            syntax: clause,
            name: jname,
            function: fn_idx,
            has_schedule,
            interval_us,
            params,
            reply,
            error_type: err_ty,
            error_text: err_text,
        })
    }

    fn count_scope_routes(
        &mut self,
        module: ModuleId,
        items: NodeId,
        depth: u64,
        route_count: &mut u64,
    ) -> bool {
        if depth > MO_LIMIT_API_SCOPE_DEPTH {
            return self.api_report(
                ErrorCode::ApiBlock,
                "api scope blocks nest more than 8 levels deep",
                Some("Keep scope blocks nested at most 8 levels deep."),
            );
        }
        let tree = &self.modules.module(module).syntax.tree;
        let children = tree.node(items).children.clone();
        for ch in children {
            if !self.api_work(1) {
                return false;
            }
            let Some(cid) = ch else {
                continue;
            };
            self.span = tree.node(cid).span;
            let ckind = tree.node(cid).kind;
            if ckind == SyntaxKind::ApiRoute {
                *route_count += 1;
                if *route_count > MO_LIMIT_API_ROUTES {
                    return self.api_report(
                        ErrorCode::ApiRoute,
                        "an api block has too many routes",
                        Some("An api block declares at most 1024 routes."),
                    );
                }
            } else if ckind == SyntaxKind::ApiScope
                && let Some(inner) = tree.node(cid).children.get(1).copied().flatten()
                && !self.count_scope_routes(module, inner, depth + 1, route_count)
            {
                return false;
            }
        }
        true
    }

    fn collect_routes_in(
        &mut self,
        api: &CheckedApi,
        items: NodeId,
        scope_prefix: Option<&str>,
        shapes: &mut Vec<String>,
        routes: &mut Vec<CheckedApiRoute>,
    ) -> bool {
        let module = api.module;
        let tree = &self.modules.module(module).syntax.tree;
        let children = tree.node(items).children.clone();
        for ch in children {
            if !self.api_work(1) {
                return false;
            }
            let Some(cid) = ch else {
                continue;
            };
            let ckind = tree.node(cid).kind;
            if ckind == SyntaxKind::ApiRoute {
                let Some(route) = self.check_route_clause(api, cid, scope_prefix, shapes, routes)
                else {
                    return false;
                };
                routes.push(route);
            } else if ckind == SyntaxKind::ApiScope {
                let Some(prefix_lit) = tree.node(cid).children.first().copied().flatten() else {
                    return false;
                };
                let Some(inner) = tree.node(cid).children.get(1).copied().flatten() else {
                    return false;
                };
                let Some(nested_prefix) = self.parse_scope_prefix(module, prefix_lit, scope_prefix)
                else {
                    return false;
                };
                if !self.collect_routes_in(api, inner, Some(&nested_prefix), shapes, routes) {
                    return false;
                }
            }
        }
        true
    }

    fn add_error_enum(&mut self, err_ty: Option<TypeId>, enums: &mut Vec<SymRef>) -> bool {
        let Some(ety) = err_ty else {
            return true;
        };
        let Some(res_ty) = self.res_type else {
            return false;
        };
        if self.api_same_type(ety, res_ty, 0) {
            return !self.failed;
        }
        if self.failed {
            return false;
        }
        let Some(sym) = self.signatures.types.get(ety).owner.as_source() else {
            return false;
        };
        debug_assert_eq!(self.interfaces.symbol(sym).decl_kind, SyntaxKind::Enum);
        if !enums.contains(&sym) {
            enums.push(sym);
        }
        true
    }

    fn check_statuses(&mut self, api: &mut CheckedApi, clauses: NodeId) -> bool {
        let mut enums: Vec<SymRef> = Vec::new();
        if let Some(ref auth) = api.auth
            && !self.add_error_enum(Some(auth.error_type), &mut enums)
        {
            return false;
        }
        for r in &api.routes {
            if !self.add_error_enum(r.error_type, &mut enums) {
                return false;
            }
        }

        let api_module = api.module;
        let tree = &self.modules.module(api_module).syntax.tree;
        let clause_children = tree.node(clauses).children.clone();
        let status_count = clause_children
            .iter()
            .filter(|c| c.is_some_and(|id| tree.node(id).kind == SyntaxKind::ApiStatus))
            .count() as u64;
        if status_count > MO_LIMIT_API_STATUSES {
            self.span = tree.node(api.syntax).span;
            return self.api_report(
                ErrorCode::ApiStatus,
                "an api block has too many status clauses",
                Some("An api block has at most 256 status clauses."),
            );
        }

        let mut statuses: Vec<CheckedApiStatus> = Vec::with_capacity(status_count as usize);
        let own_scope = self.interfaces.scope(api_module);

        for ch in clause_children {
            let Some(clause) = ch else {
                continue;
            };
            if tree.node(clause).kind != SyntaxKind::ApiStatus {
                continue;
            }
            let Some(variant_node) = tree.node(clause).children.first().copied().flatten() else {
                return false;
            };
            let wildcards = tree.node(clause).children.get(1).copied().flatten();
            let Some(code_node) = tree.node(clause).children.get(2).copied().flatten() else {
                return false;
            };
            self.span = tree.node(code_node).span;
            let Some(code) = self
                .parse_u16_literal(api_module, code_node)
                .filter(|&c| (100..=599).contains(&c))
            else {
                return self.api_report(
                    ErrorCode::ApiStatus,
                    "an api status code must be an HTTP status from 100 to 599",
                    Some("Use a standard HTTP status code such as 400, 401, 404 or 409."),
                );
            };

            self.span = tree.node(variant_node).span;
            let mut matched_enum: Option<SymRef> = None;
            let mut matched_case: Option<SymRef> = None;
            let mut alias_idx: Option<usize> = None;

            if tree.node(variant_node).kind == SyntaxKind::Qualified {
                let Some(prefix) = tree.node(variant_node).children.first().copied().flatten()
                else {
                    return false;
                };
                let Some(cname) = tree.node(variant_node).children.get(1).copied().flatten() else {
                    return false;
                };
                let pbytes = tree.node_text(prefix).unwrap_or(&[]);
                let cbytes = tree.node_text(cname).unwrap_or(&[]);
                let mut alias_module: Option<ModuleId> = None;
                for (a, al) in own_scope.aliases.iter().enumerate() {
                    if al.name.as_bytes() == pbytes {
                        alias_idx = Some(a);
                        alias_module = Some(al.module);
                    }
                }
                for &en_ref in &enums {
                    let en = self.interfaces.symbol(en_ref);
                    let prefix_ok = en.name.as_bytes() == pbytes || alias_module == Some(en.module);
                    if !prefix_ok {
                        continue;
                    }
                    let escope = self.interfaces.scope(en.module);
                    for &cidx in &en.cases {
                        let cs = escope.symbol(cidx);
                        if cs.name.as_bytes() == cbytes {
                            if matched_case.is_some() {
                                return self.api_report(
                                    ErrorCode::ApiStatus,
                                    "a status clause matches multiple error cases",
                                    None,
                                );
                            }
                            matched_enum = Some(en_ref);
                            matched_case = Some(SymRef::new(en.module, cidx));
                        }
                    }
                }
            } else {
                let cbytes = tree.node_text(variant_node).unwrap_or(&[]);
                for &en_ref in &enums {
                    let en = self.interfaces.symbol(en_ref);
                    let escope = self.interfaces.scope(en.module);
                    for &cidx in &en.cases {
                        let cs = escope.symbol(cidx);
                        if cs.name.as_bytes() == cbytes {
                            if matched_case.is_some() {
                                return self.api_report(
                                    ErrorCode::ApiStatus,
                                    "a status case name belongs to multiple api error enums",
                                    Some("Qualify the case name as EnumName.CaseName."),
                                );
                            }
                            matched_enum = Some(en_ref);
                            matched_case = Some(SymRef::new(en.module, cidx));
                        }
                    }
                }
            }

            let (Some(en_ref), Some(cs_ref)) = (matched_enum, matched_case) else {
                return self.api_report(
                    ErrorCode::ApiStatus,
                    "a status clause names an error case not returned by any route or auth function",
                    Some(
                        "Map only cases of error enums returned by this api's routes or auth \
                         function.",
                    ),
                );
            };

            if tree.node(variant_node).kind == SyntaxKind::Qualified {
                if let Some(aidx) = alias_idx
                    && en_ref.module == own_scope.aliases[aidx].module
                {
                    let origin = own_scope.aliases[aidx].origin;
                    let origin_span = own_scope.aliases[aidx].origin_span;
                    if !self.signatures.use_import(api_module, origin, origin_span) {
                        return self.api_report(
                            ErrorCode::CheckProgramWork,
                            "the program needs too much import-checking work",
                            None,
                        );
                    }
                } else if en_ref.module != api_module {
                    for imp in &own_scope.imported {
                        let hit = (0..imp.count).any(|m| {
                            SymRef::new(imp.symbol.module, SymIdx(imp.symbol.symbol.0 + m))
                                == en_ref
                        });
                        if hit {
                            if !self
                                .signatures
                                .use_import(api_module, imp.origin, imp.origin_span)
                            {
                                return self.api_report(
                                    ErrorCode::CheckProgramWork,
                                    "the program needs too much import-checking work",
                                    None,
                                );
                            }
                            break;
                        }
                    }
                }
            } else if en_ref.module != api_module {
                for imp in &own_scope.imported {
                    let hit = (0..imp.count).any(|m| {
                        let s = SymRef::new(imp.symbol.module, SymIdx(imp.symbol.symbol.0 + m));
                        s == cs_ref || s == en_ref
                    });
                    if hit {
                        if !self
                            .signatures
                            .use_import(api_module, imp.origin, imp.origin_span)
                        {
                            return self.api_report(
                                ErrorCode::CheckProgramWork,
                                "the program needs too much import-checking work",
                                None,
                            );
                        }
                        break;
                    }
                }
            }

            let cdata_count = self
                .signatures
                .data_signature_by_symbol(cs_ref)
                .map_or(0, |(_, d)| d.fields.len());
            if cdata_count == 0 && wildcards.is_some() {
                return self.api_report(
                    ErrorCode::ApiStatus,
                    "a payload-free error case cannot take (_) in status",
                    Some("Write status CaseName = code without parentheses."),
                );
            }
            if cdata_count != 0 {
                let wc_count = wildcards.map(|w| tree.node(w).children.len());
                if wc_count.is_none_or(|n| n != cdata_count && n != 1) {
                    return self.api_report(
                        ErrorCode::ApiStatus,
                        "an error case with fields needs (_) in status",
                        Some("Write status CaseName(_) = code for error cases carrying data."),
                    );
                }
            }

            if statuses
                .iter()
                .any(|s| s.error_enum == en_ref && s.variant == cs_ref)
            {
                return self.api_report(
                    ErrorCode::ApiStatus,
                    "a status clause maps the same case twice",
                    Some("Map each error enum case at most once."),
                );
            }

            statuses.push(CheckedApiStatus {
                syntax: clause,
                error_enum: en_ref,
                variant: cs_ref,
                status_code: code,
            });
        }

        self.span = tree.node(api.syntax).span;
        for &en_ref in &enums {
            let en = self.interfaces.symbol(en_ref);
            let escope = self.interfaces.scope(en.module);
            for &cidx in &en.cases {
                let cs_ref = SymRef::new(en.module, cidx);
                if !statuses
                    .iter()
                    .any(|s| s.error_enum == en_ref && s.variant == cs_ref)
                {
                    let cs = escope.symbol(cidx);
                    return self.api_report(
                        ErrorCode::ApiStatus,
                        format!(
                            "an api error case has no status clause: {}.{}",
                            en.name, cs.name
                        ),
                        Some(
                            "Every case of every error enum returned by an api must map to an \
                             HTTP status code.",
                        ),
                    );
                }
            }
        }

        api.statuses = statuses;
        true
    }
}

fn valid_sqlite_path(bytes: &[u8]) -> bool {
    if bytes.is_empty() || matches!(bytes[0], b'/' | b'\\') {
        return false;
    }
    if bytes.iter().any(|&b| b == 0 || b == b'\\') {
        return false;
    }
    let mut i = 0usize;
    while i < bytes.len() {
        let start = i;
        while i < bytes.len() && bytes[i] != b'/' {
            i += 1;
        }
        let seg = &bytes[start..i];
        if seg.is_empty() || seg == b".." {
            return false;
        }
        if i < bytes.len() {
            i += 1;
        }
    }
    true
}

fn valid_static_dir(bytes: &[u8]) -> bool {
    if bytes.is_empty() || matches!(bytes[0], b'/' | b'\\') {
        return false;
    }
    if bytes.iter().any(|&b| b == 0 || b == b'\\') {
        return false;
    }
    let mut i = 0usize;
    while i < bytes.len() {
        let start = i;
        while i < bytes.len() && bytes[i] != b'/' {
            i += 1;
        }
        let seg = &bytes[start..i];
        if seg.is_empty() || seg == b"." || seg == b".." {
            return false;
        }
        if i < bytes.len() {
            i += 1;
        }
    }
    true
}

fn url_prefixes_overlap(a: &str, b: &str) -> bool {
    if a.len() == b.len() {
        a == b
    } else if a.len() < b.len() {
        b.as_bytes()[a.len()] == b'/' && b.starts_with(a)
    } else {
        a.as_bytes()[b.len()] == b'/' && a.starts_with(b)
    }
}

fn valid_job_name(bytes: &[u8]) -> bool {
    !bytes.is_empty() && bytes.len() <= 128 && bytes.iter().all(|&c| c > 0x20 && c < 0x7f)
}

fn find_library_data_type(
    interfaces: &ModuleInterfaces,
    signatures: &Signatures,
    mod_name: &str,
    type_name: &str,
) -> Option<TypeId> {
    for i in 0..interfaces.len() {
        let mid = ModuleId(i as u32);
        let scope = interfaces.scope(mid);
        if !scope.library || scope.name.as_deref() != Some(mod_name) {
            continue;
        }
        let idx = scope.find_symbol_index(type_name.as_bytes())?;
        return signatures
            .nominal_signature_by_symbol(SymRef::new(mid, idx))
            .map(|(_, n)| n.ty);
    }
    None
}

/// Checks every `api` block in the program (`mo_check_apis`).
#[expect(
    clippy::too_many_arguments,
    reason = "API checking mirrors C mo_check_apis and threads shared checker state"
)]
#[must_use]
pub(crate) fn check_apis(
    modules: &ModuleSet,
    interfaces: &ModuleInterfaces,
    signatures: &mut Signatures,
    abilities_arena: &mut AbilityArena,
    services: &[CheckedService],
    sig_to_func: &[CheckedFuncId],
    diags: &mut DiagBag,
    arena_remaining: &mut usize,
) -> Option<Vec<CheckedApi>> {
    let mut p = ApiCheck {
        modules,
        interfaces,
        signatures,
        abilities_arena,
        services,
        sig_to_func,
        diags,
        arena_remaining,
        span: modules.modules[0].syntax.span,
        equal: Vec::new(),
        req_type: None,
        res_type: None,
        json_type: None,
        cap_used: [false; API_CAP_ORDER.len()],
        failed: false,
    };

    let mut any_api = false;
    for m in 0..modules.len() {
        let tree = &modules.modules[m].syntax.tree;
        if let Some(items_nid) = tree
            .node(modules.modules[m].syntax.root)
            .children
            .get(1)
            .copied()
            .flatten()
        {
            for &ch in &tree.node(items_nid).children {
                if !p.api_work(1) {
                    return None;
                }
                if ch.is_some_and(|id| tree.node(id).kind == SyntaxKind::Api) {
                    any_api = true;
                }
            }
        }
    }
    if !any_api {
        return Some(Vec::new());
    }

    p.req_type = find_library_data_type(p.interfaces, p.signatures, "Api", "Request");
    p.res_type = find_library_data_type(p.interfaces, p.signatures, "Api", "Response");
    p.json_type = find_library_data_type(p.interfaces, p.signatures, "Json", "Json");
    if p.req_type.is_none() || p.res_type.is_none() || p.json_type.is_none() {
        let _ = p.api_report(
            ErrorCode::ImportMissing,
            "an api block needs the Api and Json standard library modules",
            Some("Load the program with the standard library enabled."),
        );
        return None;
    }

    let mut apis: Vec<CheckedApi> = Vec::new();
    for m in 0..modules.len() {
        let mid = ModuleId(m as u32);
        let tree = &modules.modules[m].syntax.tree;
        let Some(items_nid) = tree
            .node(modules.modules[m].syntax.root)
            .children
            .get(1)
            .copied()
            .flatten()
        else {
            continue;
        };
        let mut syntax: Option<NodeId> = None;
        let mut has_service = false;
        for &ch in &tree.node(items_nid).children {
            if !p.api_work(1) {
                return None;
            }
            let Some(item) = ch else {
                continue;
            };
            let kind = tree.node(item).kind;
            if kind == SyntaxKind::Service {
                has_service = true;
            }
            if kind != SyntaxKind::Api {
                continue;
            }
            p.span = tree.node(item).span;
            if syntax.is_some() {
                let _ = p.api_report(
                    ErrorCode::ApiBlock,
                    "a module declares two api blocks",
                    Some("A module has at most one api block."),
                );
                return None;
            }
            syntax = Some(item);
        }
        let Some(api_syntax) = syntax else {
            continue;
        };
        p.span = tree.node(api_syntax).span;
        if has_service {
            let _ = p.api_report(
                ErrorCode::ApiBlock,
                "a module cannot declare both service and api",
                Some("Put stateful actors/services in their own module and import them."),
            );
            return None;
        }

        p.cap_used = [false; API_CAP_ORDER.len()];
        let mut api = CheckedApi {
            syntax: api_syntax,
            module: mid,
            has_port: false,
            port: 8000,
            cors: None,
            sqlites: Vec::new(),
            actors: Vec::new(),
            statics: Vec::new(),
            jobs: Vec::new(),
            auth: None,
            routes: Vec::new(),
            statuses: Vec::new(),
            capabilities: Vec::new(),
        };

        let clauses_nid = tree.node(api_syntax).children.first().copied().flatten()?;
        let clause_children = tree.node(clauses_nid).children.clone();
        let mut sqlite_count = 0u64;
        let mut actor_count = 0u64;
        let mut static_count = 0u64;
        let mut job_count = 0u64;
        let mut route_count = 0u64;
        let mut auth_clause: Option<NodeId> = None;

        for &ch in &clause_children {
            if !p.api_work(1) {
                return None;
            }
            let Some(c) = ch else {
                continue;
            };
            p.span = tree.node(c).span;
            match tree.node(c).kind {
                SyntaxKind::ApiPort => {
                    if api.has_port {
                        let _ = p.api_report(
                            ErrorCode::ApiBlock,
                            "an api block repeats port",
                            Some("Write at most one port clause in an api block."),
                        );
                        return None;
                    }
                    let lit = tree.node(c).children.first().copied().flatten()?;
                    p.span = tree.node(lit).span;
                    let Some(port) = p.parse_u16_literal(mid, lit) else {
                        let _ = p.api_report(
                            ErrorCode::ApiBlock,
                            "an api port must be an integer from 0 to 65535",
                            Some("Use a valid TCP port number such as 8000."),
                        );
                        return None;
                    };
                    api.has_port = true;
                    api.port = port;
                }
                SyntaxKind::ApiCors => {
                    if api.cors.is_some() {
                        let _ = p.api_report(
                            ErrorCode::ApiBlock,
                            "an api block repeats cors",
                            Some("Write at most one cors clause in an api block."),
                        );
                        return None;
                    }
                    let lit = tree.node(c).children.first().copied().flatten()?;
                    p.span = tree.node(lit).span;
                    let cors_val = p.decode_plain_text(mid, lit)?;
                    let valid_cors = !cors_val.bytes.is_empty()
                        && !cors_val
                            .bytes
                            .iter()
                            .any(|&b| b == 0 || b == b'\r' || b == b'\n');
                    if !valid_cors {
                        let _ = p.api_report(
                            ErrorCode::ApiBlock,
                            "an api cors origin must be a nonempty single-line text",
                            Some(
                                "Write cors \"*\" or an origin URL such as \
                                 \"https://app.example.com\".",
                            ),
                        );
                        return None;
                    }
                    api.cors = Some(String::from_utf8_lossy(&cors_val.bytes).into_owned());
                }
                SyntaxKind::ApiSqlite => {
                    sqlite_count += 1;
                    if sqlite_count > MO_LIMIT_API_SQLITES {
                        let _ = p.api_report(
                            ErrorCode::ApiBlock,
                            "an api block declares too many sqlite databases",
                            Some("An api block declares at most 16 sqlite clauses."),
                        );
                        return None;
                    }
                }
                SyntaxKind::ApiActor => {
                    actor_count += 1;
                    if actor_count > MO_LIMIT_API_ACTORS {
                        let _ = p.api_report(
                            ErrorCode::ApiBlock,
                            "an api block declares too many actors",
                            Some("An api block declares at most 16 actor clauses."),
                        );
                        return None;
                    }
                }
                SyntaxKind::ApiStatic => {
                    static_count += 1;
                    if static_count > MO_LIMIT_API_STATICS {
                        let _ = p.api_report(
                            ErrorCode::ApiBlock,
                            "an api block declares too many static mounts",
                            Some("An api block declares at most 8 static clauses."),
                        );
                        return None;
                    }
                }
                SyntaxKind::ApiJob => {
                    job_count += 1;
                    if job_count > MO_LIMIT_API_JOBS {
                        let _ = p.api_report(
                            ErrorCode::ApiBlock,
                            "an api block declares too many jobs",
                            Some("An api block declares at most 16 job clauses."),
                        );
                        return None;
                    }
                }
                SyntaxKind::ApiAuth => {
                    if auth_clause.is_some() {
                        let _ = p.api_report(
                            ErrorCode::ApiBlock,
                            "an api block repeats auth",
                            Some("Write at most one auth <Type> via <fn> clause."),
                        );
                        return None;
                    }
                    auth_clause = Some(c);
                }
                SyntaxKind::ApiRoute => {
                    route_count += 1;
                    if route_count > MO_LIMIT_API_ROUTES {
                        let _ = p.api_report(
                            ErrorCode::ApiRoute,
                            "an api block has too many routes",
                            Some("An api block declares at most 1024 routes."),
                        );
                        return None;
                    }
                }
                SyntaxKind::ApiScope => {
                    let inner = tree.node(c).children.get(1).copied().flatten()?;
                    if !p.count_scope_routes(mid, inner, 1, &mut route_count) {
                        return None;
                    }
                }
                SyntaxKind::ApiStatus => {}
                _ => {
                    debug_assert!(false, "unexpected api clause kind");
                }
            }
        }

        if route_count == 0 && static_count == 0 && job_count == 0 {
            p.span = tree.node(api_syntax).span;
            let _ = p.api_report(
                ErrorCode::ApiBlock,
                "an api block has no routes",
                Some("Declare at least one get, post, put, patch or delete route."),
            );
            return None;
        }

        for &ch in &clause_children {
            let Some(c) = ch else {
                continue;
            };
            if tree.node(c).kind != SyntaxKind::ApiStatic {
                continue;
            }
            let c0 = tree.node(c).children.first().copied().flatten()?;
            let c1 = tree.node(c).children.get(1).copied().flatten()?;
            let sprefix = p.parse_static_prefix(mid, c0)?;
            if api
                .statics
                .iter()
                .any(|prev| url_prefixes_overlap(&prev.prefix, &sprefix))
            {
                p.span = tree.node(c0).span;
                let _ = p.api_report(
                    ErrorCode::ApiBlock,
                    "two api static clauses have duplicate or overlapping prefixes",
                    Some("Each static mount must serve a distinct, non-overlapping URL prefix."),
                );
                return None;
            }
            p.span = tree.node(c1).span;
            let sdir = p.decode_plain_text(mid, c1)?;
            if !valid_static_dir(&sdir.bytes) {
                let _ = p.api_report(
                    ErrorCode::ApiBlock,
                    "an api static folder must be a relative path without ..",
                    Some(
                        "Use a relative directory inside the data folder, such as \
                         \"public/assets\".",
                    ),
                );
                return None;
            }
            api.statics.push(CheckedApiStatic {
                syntax: c,
                prefix: sprefix,
                dir: String::from_utf8_lossy(&sdir.bytes).into_owned(),
            });
        }

        for &ch in &clause_children {
            let Some(c) = ch else {
                continue;
            };
            if tree.node(c).kind != SyntaxKind::ApiSqlite {
                continue;
            }
            let c0 = tree.node(c).children.first().copied().flatten()?;
            let c1 = tree.node(c).children.get(1).copied().flatten()?;
            p.span = tree.node(c0).span;
            let sname = p.syntax_string(mid, c0)?;
            if api.sqlites.iter().any(|prev| prev.name == sname) {
                let _ = p.api_report(
                    ErrorCode::ApiBlock,
                    "an api block declares two sqlite clauses with the same name",
                    Some("Give each sqlite database binding a distinct name."),
                );
                return None;
            }
            p.span = tree.node(c1).span;
            let spath = p.decode_plain_text(mid, c1)?;
            if !valid_sqlite_path(&spath.bytes) {
                let _ = p.api_report(
                    ErrorCode::ApiBlock,
                    "an api sqlite path must be a relative path without ..",
                    Some("Use a relative filename inside the data folder, such as \"app.db\"."),
                );
                return None;
            }
            api.sqlites.push(CheckedApiSqlite {
                syntax: c,
                name: sname,
                path: String::from_utf8_lossy(&spath.bytes).into_owned(),
            });
        }
        if !api.sqlites.is_empty() {
            p.cap_used[0] = true;
        }

        for &ch in &clause_children {
            let Some(c) = ch else {
                continue;
            };
            if tree.node(c).kind != SyntaxKind::ApiActor {
                continue;
            }
            let c0 = tree.node(c).children.first().copied().flatten()?;
            let c1 = tree.node(c).children.get(1).copied().flatten()?;
            p.span = tree.node(c0).span;
            let aname = p.syntax_string(mid, c0)?;
            if api.sqlites.iter().any(|prev| prev.name == aname) {
                let _ = p.api_report(
                    ErrorCode::ApiBlock,
                    "an api actor binding has the same name as a sqlite binding",
                    Some("Give each sqlite and actor clause in an api block a distinct name."),
                );
                return None;
            }
            if api.actors.iter().any(|prev| prev.name == aname) {
                let _ = p.api_report(
                    ErrorCode::ApiBlock,
                    "an api block declares two actor clauses with the same name",
                    Some("Give each actor binding in an api block a distinct name."),
                );
                return None;
            }
            p.span = tree.node(c1).span;
            let Some(atype) =
                resolve_service_type(p.modules, p.interfaces, p.signatures, mid, c, p.diags)
            else {
                p.failed = true;
                return None;
            };
            if !p.is_actor_type(atype) {
                let _ = p.api_report(
                    ErrorCode::ApiBlock,
                    "an api actor clause must name a declared actor type",
                    Some("Declare actor Name ... end and reference Name in actor <binding>: Name."),
                );
                return None;
            }
            let a_owner = p.signatures.types.get(atype).owner.as_source();
            if api
                .actors
                .iter()
                .any(|prev| p.signatures.types.get(prev.ty).owner.as_source() == a_owner)
            {
                let _ = p.api_report(
                    ErrorCode::ApiBlock,
                    "an api block declares two actor clauses of the same actor type",
                    Some("Declare at most one actor clause per actor type in an api block."),
                );
                return None;
            }
            let svc_idx = p
                .services
                .iter()
                .position(|s| s.is_actor && s.actor_symbol == a_owner)?;
            api.actors.push(CheckedApiActor {
                syntax: c,
                name: aname,
                ty: atype,
                service_index: CheckedServiceId(svc_idx as u32),
            });
        }

        if let Some(ac) = auth_clause
            && !p.check_auth_clause(&mut api, ac)
        {
            return None;
        }

        let mut shapes = Vec::with_capacity(route_count as usize);
        let mut routes = Vec::with_capacity(route_count as usize);
        if !p.collect_routes_in(&api, clauses_nid, None, &mut shapes, &mut routes) {
            return None;
        }
        api.routes = routes;

        let mut jobs = Vec::with_capacity(job_count as usize);
        for &ch in &clause_children {
            let Some(c) = ch else {
                continue;
            };
            if tree.node(c).kind != SyntaxKind::ApiJob {
                continue;
            }
            let job = p.check_job_clause(&api, c, &jobs)?;
            jobs.push(job);
        }
        api.jobs = jobs;

        if !p.check_statuses(&mut api, clauses_nid) {
            return None;
        }

        let mut cap_map = [u32::MAX; API_CAP_ORDER.len()];
        let mut cap_count = 0u32;
        for c in 0..API_CAP_ORDER.len() {
            if p.cap_used[c] {
                cap_map[c] = cap_count;
                cap_count += 1;
                api.capabilities.push(API_CAP_ORDER[c].to_string());
            }
        }
        if let Some(ref mut auth) = api.auth {
            for param in &mut auth.params {
                if param.kind == ApiParamKind::Capability {
                    param.index = cap_map[param.index as usize];
                }
            }
        }
        for route in &mut api.routes {
            for param in &mut route.params {
                if param.kind == ApiParamKind::Capability {
                    param.index = cap_map[param.index as usize];
                }
            }
        }
        for job in &mut api.jobs {
            for param in &mut job.params {
                if param.kind == ApiParamKind::Capability {
                    param.index = cap_map[param.index as usize];
                }
            }
        }

        apis.push(api);
    }

    Some(apis)
}
