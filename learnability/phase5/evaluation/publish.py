#!/usr/bin/env python3
"""Stage a complete four-slot experiment; scan, archive, extract and scan again.

publish.py SESSIONS_JSON DEST --tokens SERVICE_TOKENS --provider-secrets PROVIDER_SECRETS
SESSIONS_JSON is an ordered list of four private session directories. DEST must
not exist. Nothing is uploaded. Private databases and original records stay put.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path, PurePosixPath
import shutil
import tarfile

HERE = Path(__file__).resolve().parent


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


scan = load('publication_scan', HERE.parents[1] / 'phase4/scan.py')
evidence = load('publication_gate', HERE.parent / 'evidence.py')
FILES = ('manifest.json', 'grade.json', 'stream.jsonl', 'proxy.jsonl', 'prompt.txt',
         'client-stderr.txt', 'service.log', 'restart.log', 'source-tests.json')
SOURCE_TREES = ('task-queue', 'serve')


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def regular(path):
    if path.is_symlink() or not path.is_file():
        raise ValueError('publication input must be a regular file, not a link')


def leaks(value, table):
    raw = value.encode('utf-8')
    decoded, _ = scan.decode(raw, True)
    return any(p in raw or p in decoded for p in table)


def clean(value, table):
    if isinstance(value, str):
        return '[redacted credential-bearing string]' if leaks(value, table) else value
    if isinstance(value, list):
        return [clean(v, table) for v in value]
    if isinstance(value, dict):
        if any(leaks(k, table) for k in value):
            raise ValueError('credential-bearing JSON key requires manual curation')
        return {k: clean(v, table) for k, v in value.items()}
    return value


def copy_text(source, target, table):
    regular(source)
    text = source.read_text(encoding='utf-8')
    if source.suffix == '.json':
        rendered = json.dumps(clean(json.loads(text), table), indent=2) + '\n'
    elif source.suffix == '.jsonl':
        rendered = ''.join(json.dumps(clean(json.loads(line), table)) + '\n'
                           for line in text.splitlines() if line.strip())
    else:
        rendered = ''.join('[redacted credential-bearing line]\n' if leaks(line, table)
                           else line for line in text.splitlines(keepends=True))
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(rendered)
    return {'original_sha256': sha(source), 'published_sha256': sha(target),
            'redacted': rendered != text}


def scan_tree(root, table):
    count = 0
    for path in sorted(root.rglob('*')):
        if path.is_symlink():
            raise ValueError('publication symlink refused')
        if path.is_dir():
            continue
        regular(path)
        if scan.path_leaks(table, str(path.relative_to(root))):
            raise ValueError('publication path contains a credential')
        if path.name.lower().endswith(scan.REFUSED_SUFFIXES):
            raise ValueError('opaque publication file refused')
        result = scan.scan_file(path, table, max(map(len, table)) - 1)
        if result:
            raise ValueError('publication scan found a leak or unreadable/opaque file')
        count += 1
    return count


def extract(archive, dest):
    """Only regular files and directories in this artifact format are admitted."""
    dest.mkdir(exist_ok=False)
    seen = set()
    with tarfile.open(archive, 'r:gz') as stream:
        for member in stream:
            name = PurePosixPath(member.name)
            if (name.is_absolute() or '..' in name.parts or not name.parts or
                    str(name) in seen or not (member.isfile() or member.isdir())):
                raise ValueError('unsafe archive member refused')
            seen.add(str(name))
            target = dest.joinpath(*name.parts)
            if member.isdir():
                target.mkdir(parents=True, exist_ok=True)
            else:
                target.parent.mkdir(parents=True, exist_ok=True)
                with stream.extractfile(member) as source, target.open('xb') as output:
                    shutil.copyfileobj(source, output)


def publish(sessions, destination, token_files, provider_files):
    if {Path(p).resolve() for p in token_files} & {Path(p).resolve() for p in provider_files}:
        raise ValueError('service and provider secret inventories must be separate')
    service = scan.read_tokens(token_files)
    providers = scan.read_tokens(provider_files)
    if len({token for _, token in service}) < 2 or not providers:
        raise ValueError('both distinct service credentials and provider secrets must be supplied')
    if not {token for _, token in providers} - {token for _, token in service}:
        raise ValueError('provider inventory contains only service credentials')
    tokens = service + providers
    table = scan.patterns(tokens)
    if len(sessions) != 4:
        raise ValueError('all four slots are required, including misses')
    rows = []
    for slot, directory in enumerate(sessions, 1):
        directory = Path(directory)
        if directory.is_symlink():
            raise ValueError('session symlink refused')
        regular(directory / 'manifest.json')
        manifest = json.loads((directory / 'manifest.json').read_text())
        if (manifest.get('slot') != slot or manifest.get('client') != evidence.ORDER[slot - 1]
                or manifest.get('development') is not False):
            raise ValueError('wrong slot/client or development record in final publication')
        if manifest.get('correct') is True:
            for required in ('stream.jsonl', 'proxy.jsonl'):
                regular(directory / required)
        rows.append({key: manifest.get(key) for key in ('slot', 'client', 'correct', 'development')})
    destination = Path(destination)
    destination.mkdir(parents=True, exist_ok=False)
    stage = destination / 'stage'
    stage.mkdir()
    inventory = {}
    missing = {}
    for slot, directory in enumerate(map(Path, sessions), 1):
        missing[str(slot)] = [name for name in FILES if not (directory / name).exists()]
        sources = [directory / name for name in FILES if (directory / name).exists() or
                   (directory / name).is_symlink()]
        for name in SOURCE_TREES:
            tree = directory / name
            if tree.is_symlink():
                raise ValueError('source tree symlink refused')
            if tree.exists():
                for path in tree.rglob('*'):
                    if path.is_symlink():
                        raise ValueError('source symlink refused')
                    if path.is_file() and path.suffix == '.mo':
                        sources.append(path)
        for source in sources:
            relative = Path(f'slot-{slot}') / source.relative_to(directory)
            if scan.path_leaks(table, str(relative)):
                raise ValueError('publication path contains a credential')
            inventory[str(relative)] = copy_text(source, stage / relative, table)
    (stage / 'inventory.json').write_text(json.dumps({'files': inventory, 'missing_artifacts': missing}, indent=2) + '\n')
    summary = {'sessions': rows, 'publication_scan_clean': True,
               'gate': evidence.evaluation_gate(rows, True)}
    (stage / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    scanned = scan_tree(stage, table)
    archive = destination / 'evidence.tar.gz'
    with tarfile.open(archive, 'w:gz') as output:
        for path in sorted(stage.rglob('*')):
            if path.is_file():
                output.add(path, arcname=str(path.relative_to(stage)), recursive=False)
    extracted = destination / 'extracted'
    extract(archive, extracted)
    rescanned = scan_tree(extracted, table)
    expected = {str(p.relative_to(stage)): sha(p) for p in stage.rglob('*') if p.is_file()}
    actual = {str(p.relative_to(extracted)): sha(p) for p in extracted.rglob('*') if p.is_file()}
    if expected != actual:
        raise ValueError('extracted archive differs from scanned stage')
    receipt = {'archive_sha256': sha(archive), 'stage_files_scanned': scanned,
               'extracted_files_scanned': rescanned, 'publication_scan_clean': True,
               'gate': summary['gate'], 'uploaded': False}
    (destination / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
    return receipt


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('sessions', type=Path)
    parser.add_argument('destination', type=Path)
    parser.add_argument('--tokens', nargs='+', required=True)
    parser.add_argument('--provider-secrets', nargs='+', required=True)
    args = parser.parse_args()
    result = publish(json.loads(args.sessions.read_text()), args.destination, args.tokens, args.provider_secrets)
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
