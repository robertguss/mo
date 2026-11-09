"""Development metadata characterization; never certifies complete serving coverage.

Inputs must come from collector-owned client/provider channels, never tool text.
Codex header parsing requires an already authenticated provider event envelope.
This module does not establish that trust or invent a request binding for it.
"""
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
    coverage_limit: str = 'Retries, auxiliary requests and transport completeness unproven'


def claude_metadata(events: list[dict[str, Any]], session: str) -> Metadata:
    """Extract only observed SDK assistant envelopes and reconcile terminal model keys.

    complete means this narrow envelope accounting passed, not R4 qualification.
    Duplicate request IDs may represent split assistant content; differing models fail.
    Terminal extra models are retained and fail closed, never silently discarded.
    """
    requests: dict[str, str] = {}
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
        model = message.get('model') if isinstance(message, dict) else None
        request = event.get('request_id')
        if not isinstance(request, str) or not request or not isinstance(model, str) or not model:
            valid = False
            continue
        if request in requests and requests[request] != model:
            valid = False
        requests[request] = model
        if (request, model) not in observed:
            observed.append((request, model))
    models: tuple[str, ...] = ()
    if len(terminal) != 1:
        valid = False
    else:
        end = terminal[0]
        usage = end.get('modelUsage')
        if isinstance(usage, dict):
            models = tuple(sorted(usage))
        valid = (valid and end.get('subtype') == 'success' and end.get('is_error') is False
                 and bool(models) and set(models) == set(requests.values()))
    valid = valid and bool(requests) and set(requests.values()) == {'claude-opus-5-5'}
    return Metadata(tuple(observed), models, valid)
