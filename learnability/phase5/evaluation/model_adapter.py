"""Typed serving-model metadata from collector-owned client channels, never tool text.

codex_header is the earlier header-only characterization (the provider sent no such
header). codex_responses binds provider response objects to the HTTP attempt ledger;
claude_metadata binds provider message envelopes to the client's terminal accounting.
Neither establishes that the channel's writer is the client; the collector checks that.
"""
import json
import re
from dataclasses import dataclass
from typing import Any


def codex_header(event: dict[str, Any]) -> str | None:
    """Exact-tag response_model precedence; payload model is deliberately ignored."""
    response = event.get('response')
    for headers in (response.get('headers') if isinstance(response, dict) else None,
                    event.get('headers')):
        if isinstance(headers, dict):
            for key, value in sorted(headers.items()):
                while isinstance(value, list) and value:
                    value = value[0]
                if key.lower() in ('openai-model', 'x-openai-model') and isinstance(value, str):
                    return value
    return None


@dataclass(frozen=True)
class Metadata:
    requests: tuple[tuple[str, str], ...]
    terminal_models: tuple[str, ...]
    complete: bool
    qualified: bool = False


_INPUTS = (('input_tokens', 'inputTokens'), ('cache_read_input_tokens', 'cacheReadInputTokens'),
           ('cache_creation_input_tokens', 'cacheCreationInputTokens'))


def claude_metadata(events: list[dict[str, Any]], session: str, model: str = 'claude-opus-5-5') -> Metadata:
    """Bind provider message.model/request IDs and reconcile them with terminal accounting.

    complete: every envelope names the expected model and terminal modelUsage has no other.
    qualified additionally requires per-request provider input usage to sum exactly to the
    client's terminal modelUsage. Every request bills nonzero input tokens, so a served request
    without an envelope (retry, auxiliary, fallback) breaks the equality. Extra terminal
    models are retained and fail closed, never silently discarded.
    """
    requests: dict[str, tuple] = {}
    observed = []
    terminal = []
    valid = bool(session)
    seen_terminal = False
    for event in events:
        kind = event.get('type')
        if kind not in ('assistant', 'result'):
            continue
        if event.get('session_id') != session or seen_terminal:
            valid = False
        if kind == 'result':
            seen_terminal = True
            terminal.append(event)
            continue
        message = event.get('message')
        message = message if isinstance(message, dict) else {}
        served, request, use = message.get('model'), event.get('request_id'), message.get('usage')
        if not isinstance(request, str) or not request or not isinstance(served, str) or not served:
            valid = False
            continue
        key = (served, message.get('id'),
               tuple(use.get(k) for k, _ in _INPUTS) if isinstance(use, dict) else None)
        if requests.setdefault(request, key) != key:
            valid = False
        if (request, served) not in observed:
            observed.append((request, served))
    models: tuple[str, ...] = ()
    reconciled = False
    if len(terminal) != 1:
        valid = False
    else:
        end = terminal[0]
        usage = end.get('modelUsage')
        if isinstance(usage, dict):
            models = tuple(sorted(usage))
        valid = (valid and end.get('subtype') == 'success' and end.get('is_error') is False
                 and bool(models) and set(models) == {k[0] for k in requests.values()})
        totals = usage.get(model) if isinstance(usage, dict) else None
        uses = [k[2] for k in requests.values()]
        reconciled = (isinstance(totals, dict) and all(u is not None and None not in u for u in uses)
                      and [sum(u[i] for u in uses) for i in range(3)] == [totals.get(t) for _, t in _INPUTS])
    valid = valid and bool(requests) and {k[0] for k in requests.values()} == {model}
    return Metadata(tuple(observed), models, valid, valid and reconciled)


_SSE = ' TRACE codex_api::sse::responses: SSE event: '
_HTTP = re.compile(r'codex_http_client::client: Request (completed|failed) '
                   r'method=(\S+) url=(\S+) status=(\S+)')
_THREAD = re.compile(r'session_loop\{thread_id=([^}\s]+)\}')
_PROVIDER_HOSTS = ('chatgpt.com', 'openai.com')
_NON_SERVING = (('GET', '/models'), ('POST', '/analytics-events/events'))
_TERMINAL = ('response.completed', 'response.failed', 'response.incomplete')
_AUXILIARY = ('memories/trace_summarize', 'realtime', 'alpha/search', 'images')


@dataclass(frozen=True)
class CodexProvenance:
    thread: str | None
    threads: tuple[str, ...]
    responses: tuple[tuple[str, str | None, str | None, str | None], ...]
    substituted: tuple[tuple[str, str | None, str | None], ...]
    attempts: int
    failed_attempts: int
    problems: tuple[str, ...]
    complete: bool


def codex_responses(lines, events, model, effort) -> CodexProvenance:
    """Bind provider-authored response.model/reasoning.effort to every /responses attempt.

    lines: collector-owned client stderr with RUST_LOG=codex_api=trace,codex_core=debug,
    codex_http_client=debug over HTTP transport; events: the client's own --json stdout.
    Every provider request must be a /responses attempt or a known non-serving route.
    Sub-agent threads are allowed: the attested process already binds the session, and
    their responses must still name the expected model.
    """
    problems = []
    thread = next((e.get('thread_id') for e in events if e.get('type') == 'thread.started'), None)
    if not isinstance(thread, str) or not thread:
        problems.append('no session thread')
    responses: dict[str, list] = {}
    pending = attempts = failed = 0
    threads = []
    totals = [0, 0, 0, 0]
    for line in lines:
        if _SSE in line:
            try:
                event = json.loads(line.split(_SSE, 1)[1])
            except ValueError:
                problems.append('malformed provider event')
                continue
            kind = event.get('type')
            if kind != 'response.created' and kind not in _TERMINAL:
                continue
            body = event.get('response')
            rid = body.get('id') if isinstance(body, dict) else None
            if not isinstance(rid, str) or not rid:
                problems.append('provider event without response id')
                continue
            reasoning = body.get('reasoning')
            seen = (body.get('model'), reasoning.get('effort') if isinstance(reasoning, dict) else None)
            if kind == 'response.created':
                if rid in responses:
                    problems.append('duplicate response ' + rid)
                    continue
                if pending == 0:
                    problems.append('response without a logged attempt ' + rid)
                else:
                    pending -= 1
                responses[rid] = [*seen, None, seen != (model, effort)]
            elif rid not in responses or responses[rid][2] is not None:
                problems.append('terminal event without one open response ' + rid)
                continue
            else:
                responses[rid][2] = kind
                responses[rid][3] |= seen != (model, effort)
                use = body.get('usage')
                if isinstance(use, dict):
                    totals[0] += use.get('input_tokens', 0)
                    totals[1] += (use.get('input_tokens_details') or {}).get('cached_tokens', 0)
                    totals[2] += use.get('output_tokens', 0)
                    totals[3] += (use.get('output_tokens_details') or {}).get('reasoning_tokens', 0)
            continue
        match = _HTTP.search(line)
        if not match:
            continue
        outcome, method, url, status = match.groups()
        scheme, _, rest = url.partition('://')
        host, _, path = rest.partition('/')
        path = '/' + path.split('?', 1)[0]
        host = host.rsplit('@', 1)[-1].split(':', 1)[0].lower()
        provider = any(host == h or host.endswith('.' + h) for h in _PROVIDER_HOSTS)
        if any(part in path for part in _AUXILIARY):
            problems.append('unaccounted model route ' + path)
        elif method == 'POST' and path.endswith('/responses'):
            attempts += 1
            if scheme != 'https' or status == '101':
                problems.append('unaccounted transport ' + scheme + ' ' + status)
            seen_thread = _THREAD.search(line)
            if seen_thread and seen_thread[1] not in threads:
                threads.append(seen_thread[1])
            if outcome == 'completed' and status.startswith('2'):
                pending += 1
            else:
                failed += 1
        elif provider and not any(method == m and path.endswith(p) for m, p in _NON_SERVING):
            problems.append('unclassified provider request ' + method + ' ' + path)
    if pending:
        problems.append('attempt without provider response')
    if thread not in threads:
        problems.append('no attempt in the session thread')
    if attempts == 0 or not responses:
        problems.append('no logged serving attempt')
    turns = [e.get('usage') for e in events if e.get('type') == 'turn.completed']
    client = [sum(u.get(k, 0) for u in turns if isinstance(u, dict)) for k in
              ('input_tokens', 'cached_input_tokens', 'output_tokens', 'reasoning_output_tokens')]
    if not turns or client != totals:
        problems.append('provider usage does not reconcile with client turns')
    substituted = tuple((rid, m, e) for rid, (m, e, _, changed) in responses.items() if changed)
    if substituted:
        problems.append('substituted or changed serving model')
    flat = tuple((rid, m, e, end) for rid, (m, e, end, _) in responses.items())
    return CodexProvenance(thread, tuple(threads), flat, substituted, attempts, failed,
                           tuple(problems), not problems)


EXPECTED = {'codex': ('gpt-6.1-sol', 'xhigh'), 'claude': ('claude-opus-5-5', None)}


def reported_models(client, stream, stderr):
    """The model the client requested and every model its own output reports; never graded."""
    events = []
    for line in stream.split(b'\n'):
        try:
            event = json.loads(line)
        except ValueError:
            continue
        if isinstance(event, dict):
            events.append(event)
    model, effort = EXPECTED[client]
    if client == 'codex':
        r = codex_responses(stderr.decode('utf-8', 'replace').split('\n'), events, model, effort)
        reported = {m for _, m, _, _ in r.responses}
    else:
        session = next((e.get('session_id') for e in events if e.get('type') == 'system'), '')
        r = claude_metadata(events, session, model)
        reported = {m for _, m in r.requests} | set(r.terminal_models)
    return dict(requested_model=model, requested_effort=effort,
                reported_models=sorted(m for m in reported if isinstance(m, str) and m))


def _written_by_client(writes, data, initial, decode):
    """True only if the client's root process wrote every byte of this channel file."""
    own = [w for w in writes if w['sender'].get('tgid') == initial['tgid']
           and w['sender'].get('exec_path') == initial['exec_path']
           and w['sender'].get('exec_argv') == initial['exec_argv']]
    if data is None or any(w.get('data_hex', '') is None or w.get('data', '') is None for w in own):
        return False
    return b''.join(decode(w) for w in own) == data


def session_provenance(client, evidence, stream):
    """Model provenance from a consumed capture; any unattested channel fails closed."""
    trace = evidence['trace']
    initial = next((e for e in trace['exec_events'] if e['exec_path'] == evidence['client_path']), None)
    stdout_ok = (initial is not None and evidence['stdout'] == stream and _written_by_client(
        trace['stdout_events'], stream, initial, lambda w: w['data'].encode('utf-8', 'surrogateescape')))
    stderr_ok = (initial is not None and _written_by_client(
        trace.get('stderr_events', []), evidence.get('stderr'), initial, lambda w: bytes.fromhex(w['data_hex'])))
    events = []
    for line in stream.split(b'\n'):
        if not line:
            continue
        try:
            events.append(json.loads(line))
        except ValueError:
            stdout_ok = False
    model, effort = EXPECTED[client]
    if client == 'codex':
        lines = (evidence.get('stderr') or b'').decode('utf-8', 'replace').split('\n')
        r = codex_responses(lines, events, model, effort)
        detail = dict(responses=r.responses, substituted=r.substituted, attempts=r.attempts,
                      failed_attempts=r.failed_attempts, problems=r.problems, thread=r.thread,
                      threads=r.threads)
        qualified, reason = stdout_ok and stderr_ok and r.complete, None
    else:
        session = next((e.get('session_id') for e in events if e.get('type') == 'system'), '')
        r = claude_metadata(events, session, model)
        detail = dict(requests=r.requests, terminal_models=r.terminal_models, envelope_complete=r.complete)
        stderr_ok, qualified, reason = True, stdout_ok and r.qualified, None
    if not (stdout_ok and stderr_ok):
        reason = 'client channels not attested to the client process'
    elif not qualified and reason is None:
        reason = 'serving responses incomplete or substituted'
    return dict(qualified=qualified, reason=reason, stdout_attested=stdout_ok,
                stderr_attested=stderr_ok, expected=dict(model=model, effort=effort), **detail)
