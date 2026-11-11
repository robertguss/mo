"""Post-activation observation over protected HTTP, pipe and client event records."""
import hashlib
import json

def validate(evidence, rows, origins, stream):
    try:
        if evidence['stdout'] != stream: return False
        trace = evidence['trace']
        writes = [w for w in trace['stdout_events'] if evidence['client_path'] == w['sender']['exec_path']]
        if any(w['data'] is None for w in writes): return False
        if ''.join(w['data'] for w in writes).encode('utf-8', 'surrogateescape') != stream: return False
        events = [json.loads(line) for line in stream.splitlines()]
        final = events[-2]
        if events[-1].get('type') == 'turn.completed':
            if final.get('type') != 'item.completed' or final['item']['type'] != 'agent_message': return False
            value = final['item']['text'].strip()
        elif events[-1].get('type') == 'result':
            terminal = events[-1]
            if terminal.get('subtype') != 'success' or terminal.get('is_error') is not False: return False
            if final.get('type') != 'assistant': return False
            content = final['message']['content']
            if len(content) != 1 or content[0]['type'] != 'text': return False
            value = content[0]['text'].strip()
            if terminal.get('result', '').strip() != value: return False
        else: return False
        offset = sum(len(line) for line in stream.splitlines(keepends=True)[:-2])
        cursor = 0
        final_writes = []
        for w in writes:
            end = cursor + len(w['data'].encode('utf-8', 'surrogateescape'))
            if end > offset: final_writes.append(w)
            cursor = end
        owner = final_writes[0]['sender']
        keys = ('process', 'generation', 'exec_path', 'exec_argv', 'session_id')
        same = lambda a,b: all(a[k] == b[k] for k in keys)
        initial = next(e for e in trace['exec_events'] if e['exec_path'] == evidence['client_path'])
        if not same(initial, owner): return False
        if evidence['client_path'] != owner['exec_path']: return False
        if any(not same(w['sender'], owner) for w in final_writes): return False
        start = min(w['started'] for w in final_writes)
        requests = [r for r in rows if r['direction'] == 'request']
        # RPC IDs are not occurrence identities; protected request ordinals are.
        commits = [r for r in requests if r.get('tool') == 'mo-upgrade-commit']
        if len(commits) != 1: return False
        commit = commits[0]
        if not origins[commit['seq']]['verified_push_identity']: return False
        replies = {}
        for r in rows:
            if r['direction'] != 'response': continue
            if r['request'] in replies: return False
            replies[r['request']] = r
        accepted = replies[commit['seq']]
        report = accepted['body']['result']['structuredContent']
        if accepted['status'] != 200 or report['status'] != 'accepted' or not report.get('ordinal'): return False
        def response(req):
            return next(r for r in trace['responses'] if (r['connection_ordinal'], r['request_ordinal']) == (req['connection_ordinal'], req['request_ordinal']))
        committed = max(e['completed'] for e in response(origins[commit['seq']])['occurrences'])
        for row in requests:
            if row.get('tool') != 'revision': continue
            req = origins[row['seq']]
            reply = replies[row['seq']]
            body = reply['body']
            if reply['status'] != 200 or body.get('error') or body['result'].get('isError'): continue
            if str(body['result']['structuredContent']['value']) != value: continue
            received = response(req)
            if not same(req['sender'], received['sender']): continue
            native = same(req['sender'], owner)
            if committed < min(e['started'] for e in req['occurrences']) and max(e['completed'] for e in received['occurrences']) < start:
                received_at = max(e['completed'] for e in received['occurrences'])
                candidates = typed_results(events[:-2]) if native else shell_results(events[:-2])
                matches = []
                lines = stream.splitlines(keepends=True)
                for ordinal, result in candidates:
                    exact = False
                    if native:
                        if result != body['result']['structuredContent']: continue
                    else:
                        try: parsed = json.loads(result)
                        except (ValueError, TypeError): parsed = None
                        exact = (type(parsed) in (str, int) and str(parsed) == value
                                 or parsed in (body, body['result']['structuredContent']))
                        if not exact and not isinstance(result, str): continue
                    lo = sum(len(line) for line in lines[:ordinal])
                    hi = lo + len(lines[ordinal])
                    cursor = 0
                    emission = []
                    for write in writes:
                        end = cursor + len(write['data'].encode('utf-8', 'surrogateescape'))
                        if end > lo and cursor < hi: emission.append(write)
                        cursor = end
                    if (emission and all(same(w['sender'], owner) for w in emission)
                            and received_at < min(w['started'] for w in emission)
                            and max(w['completed'] for w in emission) < start
                            and delivered(native, exact, trace, req['sender'], owner, same, received,
                                          min(w['started'] for w in emission), result)):
                        matches.append(ordinal)
                if len(matches) == 1: return True
        return False
    except (KeyError, IndexError, TypeError, ValueError, StopIteration):
        return False


def delivered(native, exact, trace, child, owner, same, received, emitted_at, output):
    if native: return True
    received_at = max(e['completed'] for e in received['occurrences'])
    if exact and pipe_delivery(trace, child, owner, same, received_at, emitted_at, output): return True
    return mixed_delivery(trace, child, owner, same, received, emitted_at, output)


def typed_results(events):
    """Bounded native adapters from original slots 1–4; ambiguous IDs reject.

    The returned ordinal is the actual client JSONL event, never a child line.
    """
    results = []
    uses = {}
    completed = set()
    for ordinal, event in enumerate(events):
        if event.get('type') == 'item.completed':
            item = event['item']
            if item.get('type') != 'mcp_tool_call': continue
            key = item['id']
            if key in completed: raise ValueError('ambiguous completed native ID')
            completed.add(key)
            if (item.get('server') == 'task-queue' and item.get('tool') == 'revision'
                    and item.get('arguments') == {} and item.get('status') == 'completed'
                    and item.get('error') is None and isinstance(item.get('result'), dict)
                    and not item['result'].get('isError')):
                result = item['result']['structured_content']
                for block in item['result'].get('content', []):
                    if block.get('type') != 'text' or json.loads(block['text']) != result:
                        raise ValueError('inconsistent native result')
                results.append((ordinal, result))
        elif event.get('type') == 'assistant':
            for block in event['message']['content']:
                if block['type'] != 'tool_use': continue
                if block['id'] in uses: raise ValueError('ambiguous tool use ID')
                uses[block['id']] = block
        elif event.get('type') == 'user':
            for block in event['message']['content']:
                if block['type'] != 'tool_result': continue
                key = block['tool_use_id']
                if key in completed: raise ValueError('ambiguous tool result ID')
                completed.add(key)
                use = uses.get(key)
                if (use and use['name'] == 'mcp__task-queue__revision'
                        and use['input'] == {} and not block.get('is_error')):
                    results.append((ordinal, json.loads(block['content'])))
    return results


def shell_results(events):
    """Typed completed shell results; command text never grants attribution."""
    results, uses, completed = [], {}, set()
    for ordinal, event in enumerate(events):
        if event.get('type') == 'item.completed':
            item = event['item']
            if item.get('type') != 'command_execution': continue
            key = item['id']
            if key in completed: raise ValueError('duplicate command result')
            completed.add(key)
            if item.get('status') == 'completed' and item.get('exit_code') == 0:
                results.append((ordinal, item['aggregated_output']))
        elif event.get('type') == 'assistant':
            for block in event['message']['content']:
                if block['type'] != 'tool_use': continue
                if block['id'] in uses: raise ValueError('duplicate command use')
                uses[block['id']] = block
        elif event.get('type') == 'user':
            for block in event['message']['content']:
                if block['type'] != 'tool_result': continue
                key = block['tool_use_id']
                if key in completed: raise ValueError('duplicate command result')
                completed.add(key)
                if uses.get(key, {}).get('name') == 'Bash' and not block.get('is_error'):
                    results.append((ordinal, block['content']))
    return results


def pipe_delivery(trace, child, owner, same, received_at, emitted_at, output):
    """Require one captured pipe, exact bytes, one writer and one client reader.

    Pipe creation must have been observed. Reused inodes and shared writers are
    rejected rather than inferred from byte equality or nearby timestamps.
    Raw pipe bytes stay inside the protected capture.
    """
    events = trace.get('pipe_events', [])
    matches = 0
    for pipe in {e['pipe'] for e in events}:
        transfers = [e for e in events if e['pipe'] == pipe]
        writes = [e for e in transfers if e['operation'] == 'write']
        reads = [e for e in transfers if e['operation'] == 'read']
        if not writes or not reads: continue
        if any(e['data_hex'] is None for e in transfers): continue
        if any(e['created'] is None or e['created'] >= e['started'] for e in transfers): continue
        if any(not same(e['sender'], child) or e['fd'] != 1 for e in writes): continue
        if any(not same(e['sender'], owner) for e in reads): continue
        if min(e['started'] for e in writes) <= received_at: continue
        if max(e['completed'] for e in reads) >= emitted_at: continue
        sent = b''.join(bytes.fromhex(e['data_hex']) for e in writes)
        got = b''.join(bytes.fromhex(e['data_hex']) for e in reads)
        if sent != got or got != output.encode(): continue
        # A read may start before the child writes, but cannot finish before the
        # corresponding bytes were written. Check each prefix independently.
        consumed = 0
        for read in reads:
            consumed += len(bytes.fromhex(read['data_hex']))
            available = sum(len(bytes.fromhex(w['data_hex'])) for w in writes
                            if w['completed'] < read['completed'])
            if consumed > available: break
        else:
            matches += 1
    return matches == 1


def mixed_delivery(trace, child, owner, same, received, emitted_at, output):
    """Accept the response inside larger shell output only through an exact chain.

    The process that received the response must have written exactly that body to
    its standard output, and the client must have read it from the same pipe or
    file before emitting the typed result that contains it. Only client reads that
    finish after the receiver's first write count, so bytes another writer placed
    there earlier cannot stand in for the response.
    """
    received_at = max(e['completed'] for e in received['occurrences'])
    objects = {}
    for event in trace.get('pipe_events', []):
        objects.setdefault(('pipe', event['pipe']), []).append(event)
    for event in trace.get('file_events', []):
        objects.setdefault(('file', event['path']), []).append(event)
    matches = 0
    for transfers in objects.values():
        sent = [e for e in transfers if e['operation'].startswith('write') and e['fd'] == 1 and same(e['sender'], child)]
        reads = [e for e in transfers if e['operation'].startswith('read') and same(e['sender'], owner)]
        if not sent or not reads or any(e['data_hex'] is None for e in sent + reads): continue
        if any('pipe' in e and (e['created'] is None or e['created'] >= e['started']) for e in transfers): continue
        body = b''.join(bytes.fromhex(e['data_hex']) for e in sent)
        if hashlib.sha256(body).hexdigest() != received['body_sha256']: continue
        if min(e['started'] for e in sent) <= received_at: continue
        wrote_at = min(e['completed'] for e in sent)
        counted = [e for e in reads if wrote_at < e['completed'] < emitted_at]
        if body not in b''.join(bytes.fromhex(e['data_hex']) for e in counted): continue
        if body.decode() not in output: continue
        matches += 1
    return matches == 1
