"""Private callable grader. Inputs and expected values must remain operator-owned."""
import json
import subprocess
import fixture
import proxy


def stored_jobs(fields):
    """Normalize trusted operator state; author tools cannot attest saved jobs."""
    jobs = []
    seen = set()
    try:
        for key, value in fields['jobs']:
            job = value['fields']
            if key != job['id'] or key in seen:
                return None
            seen.add(key)
            state = job['state']
            variant = {'case': state['case']}
            if 'fields' in state:
                variant['fields'] = state['fields']
            jobs.append({'id': key, 'payload': job['payload'],
                         'attempts': job['attempts'], 'state': variant})
        return sorted(jobs, key=lambda job: int(job['id']))
    except (KeyError, TypeError, ValueError):
        return None


def observe(client):
    result = {'jobs': client.every_job()}
    try:
        result['revision'] = client.ok('revision')
    except (fixture.base.Refused, KeyError):
        result['revision'] = None
    state = client.ok('mo-state')
    fields = state.get('state', {}).get('fields', {})
    result.update(state_complete=state.get('complete') is True,
                  stored_revision=fields.get('revision'), next_id=fields.get('next_id'),
                  stored_jobs=stored_jobs(fields))
    return result


def history(mo, folder):
    result = subprocess.run([str(mo), 'history', '--json', str(folder)],
                            capture_output=True, text=True, timeout=120, check=True)
    records = [json.loads(line) for line in result.stdout.splitlines()]
    return next(r for r in records if r.get('record') == 'history')


def attestation(client, stream, log):
    """Require reconciled proxy traffic and a successful recorded push command.

    The runner supplies protected original stream/log paths, never client summaries.
    """
    matched = proxy.match(client, stream, log)
    events = [json.loads(line) for line in stream.read_text().splitlines()]
    successful = any(e.get('type') == 'item.completed' and
                     e.get('item', {}).get('type') == 'command_execution' and
                     e['item'].get('exit_code') == 0 and
                     proxy.is_push(e['item'].get('command', '')) for e in events)
    commands = set()
    for event in events:
        for block in proxy.base.content_blocks(event):
            if not isinstance(block, dict):
                continue
            if block.get('type') == 'tool_use' and block.get('name') == 'Bash' and proxy.is_push(block.get('input', {}).get('command', '')):
                commands.add(block.get('id'))
            if block.get('type') == 'tool_result' and block.get('tool_use_id') in commands and not block.get('is_error', False):
                successful = True
    if not successful or not matched['match'] or not matched['command_origin']:
        return None
    rows = [json.loads(line) for line in log.read_text().splitlines()]
    for row in reversed(rows):
        if row.get('direction') == 'response' and row.get('tool') == 'mo-upgrade-commit' and row.get('status') == 200:
            body = row.get('body') or {}
            report = body.get('result', {}).get('structuredContent', {})
            if report.get('status') == 'accepted' and report.get('ordinal'):
                return {'origin': 'command', 'report': report}
    return None


def compare(expected, live, restarted, recorded, pushed, development=False, answer=None):
    reasons = []
    for label, observed in [('live', live), ('restart', restarted)]:
        if observed.get('state_complete') is not True:
            reasons.append(label + ': incomplete persisted state observation')
        if observed.get('stored_jobs') != expected['jobs']:
            reasons.append(label + ': wrong persisted jobs')
        for field in ('jobs', 'revision', 'stored_revision', 'next_id'):
            if observed.get(field) != expected[field]:
                reasons.append(label + ': wrong ' + field)
    report = (pushed or {}).get('report', {})
    origin = (pushed or {}).get('origin')
    if origin != 'command' and not (development and origin == 'development-reference'):
        reasons.append('no successful recorded command-origin push')
    upgrades = recorded.get('upgrades', [])
    preparation = expected.get('preparation_upgrades', [])
    session_upgrades = upgrades[len(preparation):]
    if (report.get('status') != 'accepted' or report.get('candidate_source') != expected['source_digest']
            or report.get('active_source') != expected['source_digest'] or
            recorded.get('program', {}).get('source') != expected['source_digest'] or
            recorded.get('program', {}).get('ordinal') != report.get('ordinal') or
            upgrades[:len(preparation)] != preparation or len(session_upgrades) != 1 or recorded.get('upgrades_truncated') or
            session_upgrades[0].get('new_source') != expected['source_digest'] or
            session_upgrades[0].get('old_source') != expected['base_digest'] or
            session_upgrades[0].get('ordinal') != report.get('ordinal')):
        reasons.append('activation identity mismatch')
    if answer != expected['revision']:
        reasons.append('wrong or missing answer')
    return {'correct': not reasons, 'reasons': reasons,
            'evidence_class': 'DEVELOPMENT' if development else 'SESSION'}


def stopped_grade(mo, folder, token, upgrade, log, expected, live, pushed,
                  development=False, answer=None):
    before = history(mo, folder)
    service = fixture.Service(mo, folder, token, upgrade, log)
    try:
        restarted = observe(fixture.Client(service.url, token))
    finally:
        service.stop()
    after = history(mo, folder)
    result = compare(expected, live, restarted, after, pushed, development, answer)
    if before.get('upgrades') != after.get('upgrades'):
        result['reasons'].append('restart changed activation history')
        result['correct'] = False
    return result


precheck = fixture.precheck
live_grade = observe


def source_precheck(mo, candidate, work, original=None):
    """Restore immutable queue tests in a private grading copy and execute them."""
    import pathlib
    import shutil
    work = pathlib.Path(work)
    shutil.copytree(candidate, work)
    # UpgradeOld is compiler-synthesized during upgrade checking. Ordinary tests
    # need the historical module too, exclusively from operator-retained source.
    if original is not None:
        historical = pathlib.Path(original) / 'job-queue.mo'
        source = historical.read_text()
        if 'module JobQueue' not in source or 'expose Queue,' not in source:
            raise ValueError('unsupported historical queue module')
        old = work / 'upgrade-old.mo'
        if old.exists() or old.is_symlink():
            old.unlink()
        # Only historical types belong to the synthetic upgrade namespace.
        # Copying functions also runs old promises with newly exposed internals.
        import re
        declarations = re.findall(r'^(?:struct|enum) [^\n]+\n.*?^end$', source,
                                  re.MULTILINE | re.DOTALL)
        names = [declaration.split()[1] for declaration in declarations]
        if 'Queue' not in names:
            raise ValueError('historical Queue type missing')
        old.write_text('module UpgradeOld\nexpose ' +
                       ', '.join(name + '(..)' for name in names) + '\n\n' +
                       '\n\n'.join(declarations) + '\n')
    target = work / 'queue-tests.mo'
    if target.exists() or target.is_symlink():
        target.unlink()
    shutil.copyfile(fixture.ROOT / 'services/task-queue/queue-tests.mo', target)
    return subprocess.run([str(mo), 'test', '--json', str(target)],
                          capture_output=True, text=True, timeout=240)
