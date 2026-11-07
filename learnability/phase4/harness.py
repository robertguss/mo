#!/usr/bin/env python3
"""Runs Phase 4 evaluation sessions: one fresh agent session on one scenario, as a separate unix
user, behind the recording proxy, graded when it ends.

  python3 learnability/phase4/harness.py setup MO CLAUDE CODEX
        create the session user if absent and install the pinned mo and both clients for it
        under INSTALL/bin, owned by root; refuse to go on while the operator's home grants
        others any access. Once both clients are logged in as the session user, snapshot its
        home, list every folder anyone can write in, and copy the clients' stored credentials
        into STATE/secrets for the scanner; until then, say how to log in and exit 1. Run it
        again after logging in.
  python3 learnability/phase4/harness.py build MO ROOT OUT
        build every variant under ROOT/variants into OUT/NAME (fixture.py build), side by side
  python3 learnability/phase4/harness.py run VARIANT BUILT CLIENT OUT [--binary PATH]
        [--limit SECONDS]
        one session of VARIANT (a variant folder) on CLIENT (claude or codex), from BUILT (the
        variant's built folder, holding data/ and, for a question, key.json), recorded and
        graded into OUT. --binary runs another executable with that client's command line,
        such as a stub client for a dry run; --limit is the time limit, 3600 seconds unless
        given. Exit 0 for a success, 1 for a failure, 3 when the fixture check failed and the
        session was discarded before any client started.
  python3 learnability/phase4/harness.py order SEED VARIANT...
        print the round's order: each variant twice on each client, shuffled from SEED
  python3 learnability/phase4/harness.py round ORDER ROOT BUILT OUT
        run ORDER's sessions one at a time, ROOT/variants/NAME from BUILT/NAME into
        OUT/NN-NAME-CLIENT, skipping every slot already graded; stop at a discarded session
        (exit 3) or one whose client never answered (exit 4, its folder moved to
        OUT/unstarted), so that the rerun takes the same slot
  python3 learnability/phase4/harness.py reset
        end every process of the session user, restore its home and clear the session area,
        as after a session; for after a harness that did not finish

Every command takes --user NAME (mo-session), --install DIR (/opt/mo-phase4), --state DIR
(~/.mo-phase4-harness) and --private DIR (the operator's home), before or after its arguments.

What a session gets. Its working folder, INSTALL/session/task-queue, holds the variant's three
source files and nothing else. Beside it are the service's standard error log (service.log)
and read-only copies of the built data folder (start-data/), the bytes the service started
from, and of the three source files as the service runs them (deployed/), which `mo replay`
needs once the session has edited its own. The client runs as the session user with only HOME, USER, LOGNAME, SHELL, PATH (INSTALL/bin
first), LANG, the session's token in QUEUE_TOKEN and its own no-self-update setting, and reaches
the service through the proxy, as the MCP server task-queue. Everything else (the variants, the
built folders, the state folder, the records) must be where others cannot reach, which `run`
checks. The service runs a copy of the variant's program from a neutrally named folder, and
`run` starts again under a neutral command line before any session process exists, so `ps`
never shows the session a variant's name.

What a run does, in order: append a fresh token to STATE/tokens (the scanner's file); end every
process of the session user and restore its home; run the fixture check against a service on a
disposable copy of the built folder, called directly, and discard the session if it fails;
start the session's service on a second copy and place the third, read-only, as start-data/,
beside deployed/;
start the proxy; launch the client. At the limit every process of the session user gets
SIGTERM, and SIGKILL 10 seconds later; when the client ends sooner its leftovers get SIGKILL.
Then the proxy and service stop, the working folder (without symlinks), the log and the data
copy move into OUT, the session user's home is restored, keeping the clients' current logins
(whose secrets join STATE/secrets), and its files in every folder anyone can write in are
deleted. SIGTERM or SIGHUP to the harness still ends the session's processes.
OUT/manifest.json records each artifact's SHA-256, the exit statuses, whether the service
stopped by itself, whether the client's first assistant event arrived, and any host other than
the loopback address the client's commands addressed; OUT/grade.json the grade: grade.py's
verdict, the record match, and those hosts. A session succeeds only when all three pass. Beside
them, and never part of success, grade.json records the session's coverage (the operator tools
and offline commands it used) and, for a defect variant, a fix check: mo replay --fix of the
session's folder against the retained failure in start-data/, with its verdict, whether that
verdict broke the original-first rule, and whether the grader judged the folder correct. The
service is never restarted for the session.
"""

import concurrent.futures
import hashlib
import json
import os
import pathlib
import grp
import pwd
import random
import re
import secrets
import shutil
import signal
import stat
import subprocess
import sys
import tempfile
import time

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import fixture  # noqa: E402
import grade  # noqa: E402
import proxy  # noqa: E402

SERVER = proxy.SERVER
CLIENTS = proxy.CLIENTS
SESSIONS_PER_CLIENT = 2
LIMIT = 3600
GRACE = 10  # seconds between SIGTERM and SIGKILL at the limit
LEFTOVERS = ('/tmp', '/var/tmp', '/dev/shm', '/var/spool')
CREDENTIALS = ('.claude/.credentials.json', '.codex/auth.json')
SECRET = re.compile(r'^[^\s]{20,}$')
DEFECT_CLOSE = 'When you are done, stop: your folder will be collected and graded as it stands.'
QUESTION_CLOSE = ('Change no code. When you have the answer, end your last message with the '
                  'ANSWER: line in exactly the form the report gives, and stop.')
ENTER = ('import json, os, sys; env = json.loads(sys.argv[1]); '
         'env["QUEUE_TOKEN"] = os.environ["QUEUE_TOKEN"]; os.chdir(sys.argv[2]); '
         'os.execve(sys.argv[3], sys.argv[3:], env)')


class HarnessError(Exception):
    """The harness cannot go on."""


class Layout:
    def __init__(self, user='mo-session', install='/opt/mo-phase4', state=None, private=None):
        self.user = user
        self.install = pathlib.Path(install)
        self.bin = self.install / 'bin'
        self.session = self.install / 'session'
        self.work = self.session / 'task-queue'
        self.start_data = self.session / 'start-data'
        self.deployed = self.session / 'deployed'
        self.service_log = self.session / 'service.log'
        self.mcp_config = self.session / 'mcp.json'
        self.private = pathlib.Path(private or pathlib.Path.home())
        self.state = pathlib.Path(state or self.private / '.mo-phase4-harness')
        self.tokens = self.state / 'tokens'
        self.secrets = self.state / 'secrets'
        self.snapshot = self.state / 'home.tar'
        self.binaries = self.state / 'binaries.json'

    def account(self):
        try:
            entry = pwd.getpwnam(self.user)
        except KeyError as error:
            raise HarnessError(f'there is no user {self.user}: run setup') from error
        if entry.pw_uid in (0, os.getuid()):
            raise HarnessError(f'{self.user} must be a user of its own, not root or the operator')
        return entry

    def home(self):
        return pathlib.Path(self.account().pw_dir)


def sudo(*command, check=True, stdin=None, stdout=None):
    result = subprocess.run(['sudo', '-n', *map(str, command)], check=False, stdin=stdin,
                            stdout=stdout if stdout is not None else subprocess.PIPE,
                            stderr=subprocess.PIPE, timeout=1800)
    if check and result.returncode != 0:
        raise HarnessError(f'sudo {" ".join(map(str, command))[:200]} exited '
                           f'{result.returncode}: {result.stderr.decode(errors="replace")[-500:]}')
    return result


def sha256(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as file:
        for block in iter(lambda: file.read(1 << 20), b''):
            digest.update(block)
    return digest.hexdigest()


def reachable_by(path, user):
    """Whether user can reach path: every folder above it, and path itself when it is a folder,
    lets user search it, by the owner's, the group's or others' bits as they apply to user."""
    entry = pwd.getpwnam(user)
    groups = {entry.pw_gid} | {g.gr_gid for g in grp.getgrall() if user in g.gr_mem}
    path = pathlib.Path(path).resolve()
    for folder in [*reversed(path.parents), path]:
        if not folder.is_dir():
            continue
        status = os.stat(folder)
        if status.st_uid == entry.pw_uid:
            bit = stat.S_IXUSR
        elif status.st_gid in groups:
            bit = stat.S_IXGRP
        else:
            bit = stat.S_IXOTH
        if not status.st_mode & bit:
            return False
    return True


def private_file(path):
    """Creates path, if absent, readable and writable by its owner alone."""
    path = pathlib.Path(path)
    path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    os.close(os.open(path, os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o600))
    os.chmod(path, 0o600)


def append_secret(path, value):
    private_file(path)
    with open(path, 'a', encoding='utf-8') as file:
        file.write(value + '\n')
        file.flush()
        os.fsync(file.fileno())


def processes(user):
    result = subprocess.run(['pgrep', '-U', user], capture_output=True, text=True, timeout=60)
    return result.stdout.split()


def signal_all(user, name):
    sudo('pkill', f'-{name}', '-U', user, check=False)


def end_all(user):
    """SIGKILLs every process of user until none is left; returns how many there were."""
    found = len(processes(user))
    for _ in range(80):
        if not processes(user):
            return found
        signal_all(user, 'KILL')
        time.sleep(0.25)
    raise HarnessError(f'processes of {user} survive SIGKILL: {processes(user)}')


def leftover_roots(layout):
    """The folders outside its home the session user can write in: the ones setup found, or
    the usual ones before setup has run."""
    path = layout.state / 'leftover-roots.json'
    return json.loads(path.read_text()) if path.exists() else list(LEFTOVERS)


def writable_folders():
    """Every folder anyone can write in, on every local file system."""
    mounts = []
    with open('/proc/mounts', encoding='utf-8') as file:
        for line in file:
            target, kind = line.split()[1], line.split()[2]
            if kind in ('ext4', 'xfs', 'btrfs', 'tmpfs', 'devtmpfs', 'mqueue', 'zfs'):
                mounts.append(target.replace('\\040', ' '))
    found = set(LEFTOVERS)
    for mount in mounts:
        result = sudo('find', mount, '-xdev', '-type', 'd', '-perm', '-0002', '-print',
                      '-prune', check=False)
        found.update(result.stdout.decode(errors='replace').split('\n')[:-1])
    return sorted(p for p in found if os.path.isdir(p))


def leftovers(layout):
    result = sudo('find', *leftover_roots(layout), '-xdev', '-user', layout.user, '-print',
                  check=False)
    return result.stdout.decode(errors='replace').split('\n')[:-1]


def keep_credentials(layout):
    """Adds every secret in the clients' stored credentials to STATE/secrets, and returns the
    credential files that are still well-formed, so that a login the client refreshed during a
    session outlives the restored home."""
    home, kept = layout.home(), {}
    known = set(layout.secrets.read_text().split()) if layout.secrets.exists() else set()
    for name in CREDENTIALS:
        result = sudo('cat', home / name, check=False)
        if result.returncode != 0:
            continue
        text = result.stdout.decode(errors='replace')
        for value in credential_values(text):
            if value not in known:
                append_secret(layout.secrets, value)
                known.add(value)
        try:
            json.loads(text)
            kept[name] = result.stdout
        except ValueError:
            pass
    return kept


def restore_credentials(layout, kept):
    """Writes the kept credential files back, as the session user, readable by it alone."""
    home = layout.home()
    for name, data in kept.items():
        sudo('-u', layout.user, 'mkdir', '-p', (home / name).parent)
        subprocess.run(['sudo', '-n', '-u', layout.user, 'sh', '-c', 'umask 077; cat > "$1"',
                        'sh', str(home / name)], input=data, check=True, capture_output=True,
                       timeout=60)


def reset(layout):
    """Ends the session user's processes, restores its home from the snapshot with the clients'
    current logins, deletes its files elsewhere, and empties the session area. Returns how many
    processes it ended."""
    ended = end_all(layout.user)
    for path in (layout.work, layout.start_data, layout.deployed, layout.service_log,
                 layout.mcp_config):
        sudo('rm', '-rf', path)
    if not layout.snapshot.exists():
        raise HarnessError(f'no snapshot of {layout.user}\'s home in {layout.snapshot}: run setup')
    kept = keep_credentials(layout)
    home = layout.home()
    sudo('find', home, '-mindepth', '1', '-delete')
    with open(layout.snapshot, 'rb') as snapshot:
        sudo('tar', '-C', home, '-xpf', '-', stdin=snapshot)
    restore_credentials(layout, kept)
    sudo('find', *leftover_roots(layout), '-xdev', '-user', layout.user, '-delete', check=False)
    for abandoned in layout.state.glob('session-*'):
        shutil.rmtree(abandoned, ignore_errors=True)
    left = leftovers(layout)
    if left:
        raise HarnessError(f'files of {layout.user} survive deletion: {left[:5]}')
    return ended


def install_binary(source, target):
    source = pathlib.Path(source).resolve()
    if not target.exists() or sha256(target) != sha256(source):
        sudo('install', '-m', '0755', '-o', 'root', '-g', 'root', source, target)


def install_client(layout, name, source):
    """Installs a client as INSTALL/bin/NAME. A client that ships as a package folder (Codex's
    standalone install, bin/ beside a codex-package.json) is copied whole."""
    source = pathlib.Path(source).resolve()
    package = source.parent.parent
    if not (package / 'codex-package.json').exists():
        install_binary(source, layout.bin / name)
        return layout.bin / name
    target = layout.install / name
    installed = target / source.relative_to(package)
    if not installed.exists() or sha256(installed) != sha256(source):
        sudo('rm', '-rf', target)
        sudo('cp', '-R', '--no-preserve=ownership', package, target)
        sudo('chown', '-R', 'root:root', target)
        sudo('chmod', '-R', 'go-w,a+rX', target)
    sudo('ln', '-sfn', installed, layout.bin / name)
    return installed


def as_session_user(layout, *command):
    environment = [f'HOME={layout.home()}', f'PATH={layout.bin}:/usr/bin:/bin']
    result = sudo('-u', layout.user, 'env', '-i', *environment, *command, check=False)
    return result.returncode, result.stdout.decode(errors='replace').strip()


def credential_values(text):
    """Every string in a client's stored credentials long enough to be a secret."""
    try:
        data = json.loads(text)
    except ValueError:
        return [line.strip() for line in text.splitlines() if SECRET.match(line.strip())]
    found = []

    def walk(value):
        if isinstance(value, dict):
            for inner in value.values():
                walk(inner)
        elif isinstance(value, list):
            for inner in value:
                walk(inner)
        elif isinstance(value, str) and SECRET.match(value):
            found.append(value)
    walk(data)
    return found


def earlier_binaries(layout, record):
    """The records of every earlier setup whose mo differs from this one, so that installing a
    new candidate keeps the baseline's."""
    try:
        old = json.loads(layout.binaries.read_text())
    except (OSError, ValueError):
        return []
    earlier = old.pop('previous', []) + [old]
    kept = []
    for entry in earlier:
        sha = entry.get('mo', {}).get('sha256')
        if sha != record['mo']['sha256'] and sha not in [e['mo']['sha256'] for e in kept]:
            kept.append(entry)
    return kept


def setup(layout, mo, claude, codex):
    if os.stat(layout.private).st_mode & 0o007:
        raise HarnessError(f'{layout.private} grants others access; make it 0750 or 0700 first')
    try:
        layout.account()
    except HarnessError:
        sudo('useradd', '--create-home', '--shell', '/usr/sbin/nologin', '--user-group',
             layout.user)
        layout.account()
    sudo('install', '-d', '-m', '0755', '-o', 'root', '-g', 'root', layout.install, layout.bin)
    sudo('install', '-d', '-m', '0755', '-o', str(os.getuid()), '-g', str(os.getgid()),
         layout.session)
    install_binary(mo, layout.bin / 'mo')
    installed = {'mo': layout.bin / 'mo', 'claude': install_client(layout, 'claude', claude),
                 'codex': install_client(layout, 'codex', codex)}
    record = {}
    for name, path in installed.items():
        status, version = as_session_user(layout, layout.bin / name, '--version')
        if status != 0:
            raise HarnessError(f'{name} does not run as {layout.user}: exit {status}')
        record[name] = {'path': str(layout.bin / name), 'sha256': sha256(path),
                        'version': version}
    layout.state.mkdir(mode=0o700, parents=True, exist_ok=True)
    os.chmod(layout.state, 0o700)
    private_file(layout.tokens)
    record['previous'] = earlier_binaries(layout, record)
    layout.binaries.write_text(json.dumps(record, indent=2) + '\n')
    print(json.dumps(record, indent=2))
    home = layout.home()
    missing = [name for name in CREDENTIALS
               if sudo('test', '-s', home / name, check=False).returncode != 0]
    if missing:
        print(f'\n{layout.user} is not logged in to every client ({", ".join(missing)} is '
              f'missing). Log in once, then run setup again:\n'
              f'  sudo -u {layout.user} env -i HOME={home} PATH={layout.bin}:/usr/bin:/bin '
              f'DISABLE_AUTOUPDATER=1 TERM=$TERM bash\n'
              f'  claude                 # log in, then /exit\n'
              f'  codex login --device-auth\n', file=sys.stderr)
        return 1
    if reachable_by(layout.private, layout.user):
        raise HarnessError(f'{layout.user} can reach {layout.private} through its groups')
    end_all(layout.user)
    private_file(layout.secrets)
    keep_credentials(layout)
    roots = [f for f in writable_folders() if reachable_by(f, layout.user)]
    (layout.state / 'leftover-roots.json').write_text(json.dumps(roots, indent=1) + '\n')
    snapshot = layout.snapshot.with_suffix('.partial')
    private_file(snapshot)
    with open(snapshot, 'wb') as file:
        sudo('tar', '-C', home, '-cpf', '-', '.', stdout=file)
    snapshot.replace(layout.snapshot)
    count = len(layout.secrets.read_text().split())
    print(f'snapshot of {home} taken; {count} stored credentials are in {layout.secrets}')
    return 0


def stream_events(stream):
    try:
        return list(proxy.events(stream))
    except OSError:
        return []


def first_assistant_event(client, stream):
    for event in stream_events(stream):
        kind = str(event.get('type'))
        if (client == 'claude' and kind == 'assistant') or \
                (client == 'codex' and kind.startswith('item.')):
            return True
    return False


def client_command(client, binary, prompt, url, config):
    if client == 'claude':
        return [str(binary), '-p', prompt, '--mcp-config', str(config), '--strict-mcp-config',
                '--output-format', 'stream-json', '--verbose', '--no-session-persistence',
                '--permission-mode', 'acceptEdits',
                '--allowedTools', 'Bash', f'mcp__{SERVER}__*',
                '--disallowedTools', 'WebFetch', 'WebSearch']
    return [str(binary), 'exec', '--json', '--ephemeral', '--skip-git-repo-check',
            '--sandbox', 'workspace-write',
            '-c', 'sandbox_workspace_write.network_access=true',
            '-c', 'web_search="disabled"', '-c', 'approval_policy="never"',
            '-c', 'check_for_update_on_startup=false',
            '-c', f'mcp_servers.{SERVER}.url="{url}"',
            '-c', f'mcp_servers.{SERVER}.bearer_token_env_var="QUEUE_TOKEN"',
            '-c', f'mcp_servers.{SERVER}.default_tools_approval_mode="approve"', prompt]


def prompt_text(layout, variant, limit, url):
    report = (variant / 'report.md').read_text(encoding='utf-8').strip()
    close = DEFECT_CLOSE if grade.defect(variant) else QUESTION_CLOSE
    text = (HERE / 'prompt.md').read_text(encoding='utf-8')
    for name, value in (('WORK', layout.work), ('LOG', layout.service_log),
                        ('DATA', layout.start_data), ('DEPLOYED', layout.deployed),
                        ('SERVICE_URL', url), ('MINUTES', limit // 60 or 1),
                        ('ANSWER_FORM', close), ('REPORT', report)):
        text = text.replace(name, str(value))
    return text.strip() + '\n'


def place_read_only(source, target):
    shutil.copytree(source, target)
    for folder, _, files in os.walk(target):
        for name in files:
            os.chmod(os.path.join(folder, name), 0o444)
    for folder, _, _ in sorted(os.walk(target), reverse=True):
        os.chmod(folder, 0o555)


def writable_tree(path):
    for folder, _, files in os.walk(path):
        os.chmod(folder, 0o700)
        for name in files:
            target = os.path.join(folder, name)
            if not os.path.islink(target):
                os.chmod(target, 0o600)


def collect_work(layout, out):
    """Moves the working folder into out, owned by the operator, with its symlinks removed.
    Returns the symlinks' names."""
    archive = sudo('tar', '-C', layout.session, '-cf', '-', layout.work.name).stdout
    subprocess.run(['tar', '-C', str(out), '-xf', '-', '--no-same-owner',
                    '--no-same-permissions'], input=archive, check=True, timeout=600)
    target = out / layout.work.name
    writable_tree(target)
    removed = []
    for folder, dirs, files in os.walk(target):
        for name in dirs + files:
            path = pathlib.Path(folder) / name
            if path.is_symlink():
                removed.append(str(path.relative_to(target)))
                path.unlink()
    sudo('rm', '-rf', layout.work)
    return sorted(removed)


def artifacts(out):
    return {str(p.relative_to(out)): sha256(p) for p in sorted(out.rglob('*'))
            if p.is_file() and not p.is_symlink() and p.name not in ('manifest.json',
                                                                     'grade.json')}


def check_private(layout, *paths):
    for path in paths:
        if reachable_by(path, layout.user):
            raise HarnessError(f'{path} can be reached by {layout.user}; keep it under a '
                               'folder only you can search')


def run(layout, variant, built, client, out, binary=None, limit=LIMIT):
    variant, built, out = (pathlib.Path(p).resolve() for p in (variant, built, out))
    if client not in CLIENTS:
        raise HarnessError(f'the client is claude or codex, not {client!r}')
    binary = pathlib.Path(binary).resolve() if binary else layout.bin / client
    mo = layout.bin / 'mo'
    try:
        builder = json.loads((built.parent / 'round.json').read_text())['mo_sha256']
    except (OSError, ValueError, KeyError) as error:
        raise HarnessError(f'{built.parent}/round.json does not say which mo built it') from error
    if builder != sha256(mo):
        raise HarnessError(f'{built} was built by another mo ({builder[:12]}), not the installed '
                           f'{sha256(mo)[:12]}; replay needs the mo that recorded its failures')
    out.mkdir(mode=0o700, parents=True, exist_ok=False)
    check_private(layout, variant, built, layout.state, out)
    token = secrets.token_urlsafe(32)
    append_secret(layout.tokens, token)
    fixed = fixture.load(variant)
    reset(layout)
    scratch = pathlib.Path(tempfile.mkdtemp(dir=layout.state, prefix='session-'))
    running = []  # what to stop if the session ends in an error
    previous = {name: signal.signal(name, stopped) for name in (signal.SIGTERM, signal.SIGHUP)}
    try:
        return session(layout, variant, built, client, out, binary, limit, mo, token, fixed,
                       scratch, running)
    finally:
        for name, handler in previous.items():
            signal.signal(name, handler)
        if running:
            for thing in reversed(running):
                try:
                    thing.stop()
                except Exception:
                    pass
            end_all(layout.user)
        shutil.rmtree(scratch, ignore_errors=True)


def stopped(number, _frame):
    """Turns SIGTERM and SIGHUP into an exit that still runs the session's cleanup."""
    raise SystemExit(f'harness: stopped by signal {number}')


def session(layout, variant, built, client, out, binary, limit, mo, token, fixed, scratch,
            running):
    program = scratch / 'serve' / 'task-queue'
    program.mkdir(parents=True)
    for source in variant.glob('*.mo'):
        shutil.copyfile(source, program / source.name)
    check_copy = scratch / 'check'
    shutil.copytree(built / 'data', check_copy)
    checking = None
    try:
        checking = fixture.Service(mo, program / 'main.mo', check_copy, token,
                                   scratch / 'check.log')
        running.append(checking)
        fixture.check(fixed, fixture.Client(checking.url, token), scratch / 'check.log')
    except (fixture.CheckFailed, fixture.Refused) as error:
        if checking is not None:
            running.remove(checking)
            checking.stop()
        (out / 'discarded.json').write_text(json.dumps(
            {'variant': variant.name, 'client': client, 'at': time.time(),
             'check_failed': str(error)}, indent=2) + '\n')
        print(f'discarded: the fixture check failed: {error}', file=sys.stderr)
        return 3
    running.remove(checking)
    checking.stop()
    shutil.rmtree(check_copy)

    shutil.copytree(built / 'data', scratch / 'data')
    service = fixture.Service(mo, program / 'main.mo', scratch / 'data', token,
                              layout.service_log)
    running.append(service)
    os.chmod(layout.service_log, 0o644)
    place_read_only(built / 'data', layout.start_data)
    deployed = scratch / 'deployed'  # the untouched source, which replay needs
    deployed.mkdir()
    for name in grade.SESSION_FILES:
        shutil.copyfile(variant / name, deployed / name)
    place_read_only(deployed, layout.deployed)
    record = proxy.Proxy(service.url, out / 'proxy.jsonl', token)
    running.append(record)
    layout.work.mkdir(mode=0o755)
    for name in grade.SESSION_FILES:
        shutil.copyfile(variant / name, layout.work / name)
        os.chmod(layout.work / name, 0o644)
    sudo('chown', '-R', f'{layout.user}:{layout.user}', layout.work)
    layout.mcp_config.write_text(json.dumps({'mcpServers': {SERVER: {
        'type': 'http', 'url': record.url,
        'headers': {'Authorization': 'Bearer ${QUEUE_TOKEN}'}}}}, indent=2) + '\n')
    os.chmod(layout.mcp_config, 0o644)
    prompt = prompt_text(layout, variant, limit, record.url)
    (out / 'prompt.txt').write_text(prompt)
    command = client_command(client, binary, prompt, record.url, layout.mcp_config)
    home = layout.home()
    environment = {'HOME': str(home), 'USER': layout.user, 'LOGNAME': layout.user,
                   'SHELL': '/bin/bash', 'PATH': f'{layout.bin}:/usr/local/bin:/usr/bin:/bin',
                   'LANG': 'C.UTF-8', 'DISABLE_AUTOUPDATER': '1'}
    check_copy_gone = not check_copy.exists()

    stream, client_log = out / 'stream.jsonl', out / 'client-stderr.txt'
    started, launched = time.time(), time.monotonic()
    print(f'session started: {client}, limit {limit} s', file=sys.stderr)
    with open(stream, 'wb') as stdout, open(client_log, 'wb') as stderr:
        child = subprocess.Popen(
            ['sudo', '-n', '-u', layout.user, '--preserve-env=QUEUE_TOKEN', '--',
             '/usr/bin/python3', '-I', '-c', ENTER, json.dumps(environment), str(layout.work),
             *command], stdin=subprocess.DEVNULL, stdout=stdout, stderr=stderr,
            env={'QUEUE_TOKEN': token, 'PATH': '/usr/bin:/bin'}, start_new_session=True)
        killed = False
        try:
            client_status = child.wait(timeout=limit)
        except subprocess.TimeoutExpired:
            killed = True
            signal_all(layout.user, 'TERM')
            try:
                child.wait(timeout=GRACE)
            except subprocess.TimeoutExpired:
                signal_all(layout.user, 'KILL')
            client_status = child.wait(timeout=60)
    seconds = round(time.monotonic() - launched, 1)
    left = end_all(layout.user)
    print(f'session ended after {seconds} s', file=sys.stderr)

    running.remove(record)
    record.stop()
    stopped_itself = service.process.poll() is not None
    running.remove(service)
    service_status = service.stop()
    symlinks = collect_work(layout, out)
    shutil.move(str(layout.service_log), str(out / 'service.log'))
    writable_tree(layout.start_data)
    shutil.move(str(layout.start_data), str(out / 'start-data'))
    writable_tree(layout.deployed)
    shutil.rmtree(layout.deployed)
    reset(layout)

    foreign = proxy.hosts(client, stream)
    failed = [f'addressed {f["host"]}: ' + next(v for k, v in f.items() if k != 'host')
              for f in foreign]
    manifest = {
        'variant': variant.name, 'client': client, 'binary': str(binary),
        'binary_sha256': sha256(binary), 'mo_sha256': sha256(mo), 'limit_seconds': limit,
        'started_at': started, 'seconds': seconds, 'killed_at_limit': killed,
        'client_exit_status': client_status, 'leftover_processes_ended': left,
        'service_exit_status': service_status, 'service_stopped_by_itself': stopped_itself,
        'first_assistant_event': first_assistant_event(client, stream),
        'check_copy_removed_before_launch': check_copy_gone,
        'symlinks_removed': symlinks, 'foreign_hosts': foreign, 'failed': failed,
        'command': [part if part != prompt else 'PROMPT' for part in command],
        'artifacts': artifacts(out)}
    (out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    return graded(variant, built, out, mo)


def graded(variant, built, out, mo):
    """Grades a session whose manifest is written: writes OUT/grade.json."""
    manifest = json.loads((out / 'manifest.json').read_text())
    stream = out / 'stream.jsonl'
    answers = None if grade.defect(variant) else \
        grade.answer_problems(variant, stream, built / 'key.json')
    success, reasons, notes = grade.grade_folder(variant, mo, out / 'task-queue', answers)
    records = proxy.match(manifest['client'], stream, out / 'proxy.jsonl', SERVER)
    checked = grade.fix_check(variant, mo, out / 'start-data', out / 'task-queue') \
        if grade.defect(variant) else None
    verdict = {'variant': variant.name, 'client': manifest['client'],
               'success': success and records['match'] and not manifest['failed'],
               'grader': {'success': success, 'reasons': reasons, 'notes': notes},
               'records': records, 'failed': manifest['failed'],
               'coverage': proxy.coverage(manifest['client'], stream, SERVER),
               'fix_check': checked and {**checked, 'graded_correct': success}}
    (out / 'grade.json').write_text(json.dumps(verdict, indent=2) + '\n')
    print(f'{"success" if verdict["success"] else "failure"}: {variant.name} on '
          f'{manifest["client"]}', file=sys.stderr)
    return 0 if verdict['success'] else 1


def order(seed, variants):
    slots = [(v, c) for v in variants for c in CLIENTS for _ in range(SESSIONS_PER_CLIENT)]
    random.Random(seed).shuffle(slots)
    return slots


def read_order(path):
    slots = []
    for line in pathlib.Path(path).read_text(encoding='utf-8').splitlines():
        if line.strip() and not line.startswith('#'):
            number, variant, client = line.split('\t')
            slots.append((int(number), variant, client))
    return slots


def round_(layout, order_file, root, built, out):
    out = pathlib.Path(out)
    out.mkdir(mode=0o700, parents=True, exist_ok=True)
    for number, name, client in read_order(order_file):
        folder = out / f'{number:02d}-{name}-{client}'
        variant = pathlib.Path(root) / 'variants' / name
        if (folder / 'grade.json').exists():
            continue
        if (folder / 'manifest.json').exists():
            graded(variant, pathlib.Path(built) / name, folder, layout.bin / 'mo')
            continue
        if folder.exists() and not (folder / 'discarded.json').exists():
            print(f'{folder} holds a session that did not finish; decide whether it saw its '
                  'task (protocol section 6), then remove or keep it by hand', file=sys.stderr)
            return 1
        if folder.exists():
            discarded = out / 'discarded'
            discarded.mkdir(mode=0o700, exist_ok=True)
            os.replace(folder / 'discarded.json',
                       discarded / f'{folder.name}-{int(time.time())}.json')
            folder.rmdir()
        print(f'slot {number}: {name} on {client}', file=sys.stderr)
        status = run(layout, variant, pathlib.Path(built) / name, client, folder)
        if status == 3:
            print(f'slot {number} was discarded; repair its fixture, then run the round again',
                  file=sys.stderr)
            return 3
        if not json.loads((folder / 'manifest.json').read_text())['first_assistant_event']:
            unstarted = out / 'unstarted'
            unstarted.mkdir(mode=0o700, exist_ok=True)
            os.replace(folder, unstarted / f'{folder.name}-{int(time.time())}')
            print(f'slot {number}: the client never answered, so the session did not see its '
                  'task and is run again (protocol section 6); say why in notes.md, then run '
                  'the round again', file=sys.stderr)
            return 4
    return 0


def build(mo, root, out):
    """Builds every variant's data folder side by side, each with a token of its own."""
    out = pathlib.Path(out)
    out.mkdir(mode=0o700, parents=True, exist_ok=False)
    variants = sorted(p for p in (pathlib.Path(root) / 'variants').iterdir()
                      if (p / 'fixture.json').exists())

    def one(variant):
        token = secrets.token_urlsafe(32)
        with open(out / f'{variant.name}.log', 'w') as log:
            return subprocess.run(
                [sys.executable, str(HERE / 'fixture.py'), 'build', str(variant), str(mo),
                 str(out / variant.name), 'BUILD_TOKEN'], stdout=log, stderr=subprocess.STDOUT,
                env={**os.environ, 'BUILD_TOKEN': token}, timeout=7200).returncode
    with concurrent.futures.ThreadPoolExecutor(max_workers=len(variants) or 1) as pool:
        statuses = list(pool.map(one, variants))
    (out / 'round.json').write_text(json.dumps(
        {'mo_sha256': sha256(mo), 'built_at': time.time(),
         'variants': {v.name: s for v, s in zip(variants, statuses)}}, indent=2) + '\n')
    for variant, status in zip(variants, statuses):
        print(f'{"ok  " if status == 0 else "FAIL"} {variant.name}')
    return 0 if all(s == 0 for s in statuses) else 1


OPTIONS = {'--user': 'user', '--install': 'install', '--state': 'state', '--private': 'private'}
RUN_OPTIONS = {'--binary': 'binary', '--limit': 'limit'}


def parse(args):
    positional, layout, extra = [], {}, {}
    args = list(args)
    while args:
        arg = args.pop(0)
        if arg in OPTIONS or arg in RUN_OPTIONS:
            if not args:
                raise HarnessError(f'{arg} needs a value')
            (layout if arg in OPTIONS else extra)[(OPTIONS | RUN_OPTIONS)[arg]] = args.pop(0)
        else:
            positional.append(arg)
    return positional, Layout(**layout), extra


def neutral_restart(args, positional, layout):
    """Starts this command again with its arguments in a private file, so that the command line
    every user can read names no variant. Returns the arguments when already restarted."""
    if positional[1:2] == ['--from'] and len(positional) == 3:
        path = pathlib.Path(positional[2])
        given = json.loads(path.read_text())
        path.unlink()
        return given
    layout.state.mkdir(mode=0o700, parents=True, exist_ok=True)
    handle, name = tempfile.mkstemp(dir=layout.state, prefix='run-', suffix='.json')
    with os.fdopen(handle, 'w') as file:
        json.dump(args, file)
    os.execv(sys.executable, [sys.executable, str(pathlib.Path(__file__).resolve()), 'run',
                              '--from', name])
    raise AssertionError('os.execv returned')


def main(args):
    try:
        positional, layout, extra = parse(args)
        if positional[:1] == ['run']:
            positional, layout, extra = parse(neutral_restart(args, positional, layout))
        command = positional[:1]
        if command == ['setup'] and len(positional) == 4:
            return setup(layout, *positional[1:])
        if command == ['build'] and len(positional) == 4:
            return build(*positional[1:])
        if command == ['run'] and len(positional) == 5:
            return run(layout, *positional[1:], binary=extra.get('binary'),
                       limit=int(extra.get('limit', LIMIT)))
        if command == ['order'] and len(positional) >= 3:
            print(f'# seed {positional[1]}\n# slot\tvariant\tclient')
            for number, (variant, client) in enumerate(order(positional[1], positional[2:]), 1):
                print(f'{number}\t{variant}\t{client}')
            return 0
        if command == ['round'] and len(positional) == 5:
            return round_(layout, *positional[1:])
        if command == ['reset'] and len(positional) == 1:
            print(f'ended {reset(layout)} processes of {layout.user}', file=sys.stderr)
            return 0
    except (HarnessError, fixture.FixtureError, fixture.CheckFailed) as error:
        print(f'harness: {error}', file=sys.stderr)
        return 2
    print(__doc__.split('\n\n')[1], file=sys.stderr)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
