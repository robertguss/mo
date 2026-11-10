"""Private callable grader. Inputs and expected values must remain operator-owned."""
import json
import re
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


def attestation(client, stream, log, *, matched=None):
    """Require reconciled proxy traffic and a successful recorded push command.

    The runner supplies protected original stream/log paths, never client summaries.
    """
    if matched is None:
        matched = proxy.match(client, stream, log)
    # The exact executable/argv, process generation, transfers and accepted
    # commit are established by protected capture. Shell display text is not
    # evidence of the invocation and wrappers need not contain "mo push".
    if not matched['match'] or not matched['command_origin']:
        return None
    rows = proxy.base.entries(log)
    commits = [r for r in rows if r.get('direction') == 'request'
               and r.get('tool') == 'mo-upgrade-commit']
    proven = {r['request_seq'] for r in matched['command_origin']}
    if len(commits) != 1 or commits[0]['seq'] not in proven:
        return None
    if any(r.get('direction') == 'request' and r.get('tool') in proxy.UPLOAD
           and r['seq'] not in proven for r in rows):
        return None
    accepted = accepted_report(log)
    return dict(accepted, origin='command') if accepted else None


def accepted_report(log):
    """Recorded report for compilation diagnostics; this alone is not attestation."""
    rows = [json.loads(line) for line in log.read_text().splitlines()]
    for row in reversed(rows):
        if row.get('direction') == 'response' and row.get('tool') == 'mo-upgrade-commit' and row.get('status') == 200:
            body = row.get('body') or {}
            report = body.get('result', {}).get('structuredContent', {})
            if report.get('status') == 'accepted' and report.get('ordinal'):
                return {'report': report}
    return None


def migration_selector(pushed):
    selector = (pushed or {}).get('report', {}).get('migration')
    if not isinstance(selector, str) or re.fullmatch(r'(?:[A-Za-z_][A-Za-z_0-9]*\.)*migrate', selector) is None:
        raise ValueError('recorded migration must have function name migrate')
    return selector


def compare(expected, live, restarted, recorded, pushed, development=False, answer=None,
            identity_report=None, revision_observed=False):
    reasons = []
    for label, observed in [('live', live), ('restart', restarted)]:
        if observed.get('state_complete') is not True:
            reasons.append(label + ': incomplete persisted state observation')
        if observed.get('stored_jobs') != expected['jobs']:
            reasons.append(label + ': wrong persisted jobs')
        for field in ('jobs', 'revision', 'stored_revision', 'next_id'):
            if observed.get(field) != expected[field]:
                reasons.append(label + ': wrong ' + field)
    state_ok = not reasons
    report = (identity_report or pushed or {}).get('report', {})
    origin = (pushed or {}).get('origin')
    command_ok = origin == 'command' or (development and origin == 'development-reference')
    if not command_ok:
        reasons.append('no successful recorded command-origin push')
    upgrades = recorded.get('upgrades', [])
    preparation = expected.get('preparation_upgrades', [])
    session_upgrades = upgrades[len(preparation):]
    identity_ok = not (report.get('status') != 'accepted' or report.get('candidate_source') != expected['source_digest']
            or report.get('active_source') != expected['source_digest'] or
            recorded.get('program', {}).get('source') != expected['source_digest'] or
            recorded.get('program', {}).get('ordinal') != report.get('ordinal') or
            upgrades[:len(preparation)] != preparation or len(session_upgrades) != 1 or recorded.get('upgrades_truncated') or
            session_upgrades[0].get('new_source') != expected['source_digest'] or
            session_upgrades[0].get('old_source') != expected['base_digest'] or
            session_upgrades[0].get('ordinal') != report.get('ordinal'))
    if not identity_ok:
        reasons.append('activation identity mismatch')
    if not revision_observed:
        reasons.append('post-activation revision observation not proven')
    answer_ok = isinstance(answer, str) and answer.strip() == expected['revision']
    if not answer_ok:
        reasons.append('wrong or missing answer')
    return {'correct': not reasons, 'reasons': reasons,
            'dimensions': {'task_state': state_ok, 'activation_identity': identity_ok,
                           'command_origin': command_ok,
                           'final_answer': revision_observed is True and answer_ok},
            'evidence_class': 'DEVELOPMENT' if development else 'SESSION'}


def stopped_grade(mo, folder, token, upgrade, log, expected, live, pushed,
                  development=False, answer=None, identity_report=None, revision_observed=False):
    before = history(mo, folder)
    service = fixture.Service(mo, folder, token, upgrade, log)
    try:
        restarted = observe(fixture.Client(service.url, token))
    finally:
        service.stop()
    after = history(mo, folder)
    result = compare(expected, live, restarted, after, pushed, development, answer,
                     identity_report, revision_observed)
    if before.get('upgrades') != after.get('upgrades'):
        result['reasons'].append('restart changed activation history')
        result['correct'] = False
        result['dimensions']['activation_identity'] = False
    return result


precheck = fixture.precheck
live_grade = observe


def source_precheck(mo, candidate, work, original=None, *, immutable_tests=None):
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
    shutil.copyfile(immutable_tests if immutable_tests is not None else
                    fixture.ROOT / 'services/task-queue/queue-tests.mo', target)
    command = [str(mo), 'test', '--json', str(target)]
    try:
        return subprocess.run(command, capture_output=True, text=True, timeout=240)
    except subprocess.TimeoutExpired:
        result = subprocess.CompletedProcess(command, 124, stdout='', stderr='source tests timed out')
        result.timed_out = True
        return result
