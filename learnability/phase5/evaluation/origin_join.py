"""Private collector correlation. Inputs must remain collector owned.

No attestation issuer: executable authentication and capture completeness remain
required. Reused endpoint tuples are rejected, never ordered by timestamps.
"""
import hashlib
import json


def digests(data):
    result = {'body_byte_count': len(data),
              'body_sha256': hashlib.sha256(data).hexdigest()}
    try:
        value = json.loads(data)
        result['canonical_json_sha256'] = hashlib.sha256(json.dumps(
            value, sort_keys=True, separators=(',', ':'), ensure_ascii=True
        ).encode('ascii')).hexdigest()
    except (ValueError, UnicodeError):
        pass
    return result


class JoinError(ValueError):
    pass


def join(trace, rows):
    """Join a complete private capture once; return sender observations, never push credit.

    Caller must isolate each invocation/capture. Duplicate sequences, connections,
    missing endpoints and response receive mismatches reject the entire join.
    """
    def reject():
        raise JoinError('missing, ambiguous or inconsistent collector evidence')
    try:
        seqs = [r['seq'] for r in rows]
        if len(set(seqs)) != len(seqs): reject()
        requests = [r for r in rows if r['direction'] == 'request']
        replies = [r for r in rows if r['direction'] == 'response']
        used = set()
        result = []
        for row in requests:
            evidence = row['transport']
            endpoint = evidence['endpoint']
            if not endpoint or '->' not in endpoint: reject()
            # A tuple may name only one incarnation on each side of this capture.
            for records, field in ((requests, 'connection'),
                                   (trace['requests'], 'connection_ordinal')):
                twins = [r for r in records if (r.get('transport', {}).get('endpoint')
                         if field == 'connection' else r.get('endpoint')) == endpoint]
                if len({r[field] for r in twins}) != 1: reject()
            candidates = [r for r in trace['requests'] if r.get('endpoint') == endpoint
                          and r['request_ordinal'] == evidence['request_ordinal']]
            if len(candidates) != 1: reject()
            request = candidates[0]
            key = (request['connection_ordinal'], request['request_ordinal'])
            if key in used: reject()
            used.add(key)
            response = [r for r in trace['responses'] if
                        (r['connection_ordinal'], r['request_ordinal']) == key]
            reply = [r for r in replies if r['request'] == row['seq']]
            if len(response) != 1 or len(reply) != 1: reject()
            if request['method'] != row['http']: reject()
            if response[0]['status'] != reply[0]['status']: reject()
            for actual, expected in ((request, evidence),
                                     (response[0], reply[0]['transport'])):
                if not actual['occurrences']: reject()
                for name in ('body_byte_count', 'body_sha256', 'canonical_json_sha256'):
                    if actual.get(name) != expected.get(name): reject()
            result.append({'request_seq': row['seq'], 'sender': request['sender'],
                           'connection_ordinal': key[0], 'request_ordinal': key[1]})
        if len(used) != len(trace['requests']) or len(replies) != len(requests): reject()
        return {'qualified': False, 'observations': result, 'command_origin': []}
    except (KeyError, TypeError, AttributeError):
        reject()
