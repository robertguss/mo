#!/usr/bin/env python3
"""Concrete task-queue schema synthesis, not a production untrusted-input reader."""
import hashlib
import json
import pathlib
import subprocess

BUILTINS = ['Unit', 'Bool', 'Byte', 'Int', 'Float', 'Char', 'Time', 'Duration',
            'Decimal', 'Range', 'Text', 'Bytes', 'BigInt', '', 'List', 'Set', 'Map']


class Schema:
    def __init__(self, data):
        self.data, self.at, self.nodes = data, 0, []
        self.root = self.read()
        assert self.at == len(data)

    def take(self, count):
        assert count <= len(self.data) - self.at
        result = self.data[self.at:self.at + count]
        self.at += count
        return result

    def number(self, width=4):
        return int.from_bytes(self.take(width), 'little')

    def text(self):
        return self.take(self.number()).decode()

    def read(self):
        tag = self.number(1)
        if tag == 255:
            return self.nodes[self.number()]
        node = {'tag': tag}
        self.nodes.append(node)
        node['name'] = self.text() if tag in (17, 18) else ''
        node['parameters'] = [self.read() for _ in range(self.number())]
        groups = []
        for _ in range(self.number()):
            name, unnamed = (self.text(), self.number(1)) if tag == 18 else ('', False)
            fields = [(self.text(), self.read()) for _ in range(self.number())]
            groups.append((name, unnamed, fields))
        node['groups'] = groups
        return node

    def typename(self, node):
        tag = node['tag']
        if tag in (17, 18):
            return node['name'].rsplit('.', 1)[1]
        parameters = ', '.join(self.typename(n) for n in node['parameters'])
        return (BUILTINS[tag] + ('(' + parameters + ')' if parameters else ''))

    def source(self, module='Main'):
        out = [f'module {module}']
        nominal = [n for n in self.nodes if n['tag'] in (17, 18)]
        if module != 'Main':
            out.append('expose ' + ', '.join(self.typename(n) + '(..)' for n in nominal))
        for node in nominal:
            name = self.typename(node)
            # Struct and one-constructor enum are indistinguishable in MOSV.
            # For this fixture the constructor equals the nominal's short name.
            groups = node['groups']
            record = len(groups) == 1 and groups[0][0] == name
            out.append(('struct ' if record else 'enum ') + name)
            for case, unnamed, fields in groups:
                assert not unnamed, 'fixture uses only named fields'
                text = [f'{key}: {self.typename(value)}' for key, value in fields]
                if record:
                    out.extend(text)
                else:
                    out.append(case + ('(' + ', '.join(text) + ')' if text else ''))
            out.append('end')
        return '\n'.join(out) + '\n'


def verify(executable, folder, binary):
    schema = folder / 'original.schema'
    run = subprocess.run([str(executable), str(folder / 'synthesis.sqlite'), str(schema)],
                         capture_output=True, text=True, check=True)
    original = schema.read_bytes()
    parsed = Schema(original)
    suffix = '''service
state Queue = Queue(jobs: {}, next_id: 1)
tools read
token from env "TOKEN"
end
fn read(state: Queue, _now: Time) : Int
state.next_id
end
'''
    source = folder / 'main.mo'
    source.write_text(parsed.source() + suffix)
    reconstructed = folder / 'reconstructed.schema'
    subprocess.run([str(executable), '--schema', str(source), str(reconstructed)], check=True)
    assert reconstructed.read_bytes() == original
    source.write_text((parsed.source() + suffix).replace('next_id: Int\n', 'next_id: Int\nrevision: Int\n')
                      .replace('next_id: 1)', 'next_id: 1, revision: 1)'))
    changed = folder / 'changed.schema'
    subprocess.run([str(executable), '--schema', str(source), str(changed)], check=True)
    assert changed.read_bytes() != original
    (folder / 'upgrade-old.mo').write_text(parsed.source('UpgradeOld'))
    migration = '''module Main
use UpgradeOld{Queue}
struct NewQueue
jobs: Map(Int, UpgradeOld.Job)
next_id: Int
revision: Int
end
fn migrate(old: Queue) : NewQueue
NewQueue(jobs: old.jobs, next_id: old.next_id, revision: 1)
end
fn main() : Int
job = UpgradeOld.Job(id: 7, payload: "preserved", attempts: 2, state: UpgradeOld.Ready)
old = UpgradeOld.Queue(jobs: {7: job}, next_id: 8)
new = migrate(old)
if new.jobs == old.jobs and new.next_id == 8 and new.revision == 1: 1 else: 2
end
'''
    source.write_text(migration)
    emitted = folder / 'migration.bc'
    result = subprocess.run([str(binary), 'emit', str(source), '-o', str(emitted)],
                            capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    output = subprocess.run([str(binary), 'run', str(source)], capture_output=True, text=True)
    assert output.returncode == 1 and not output.stderr, (output.returncode, output.stderr)
    return {'same_schema_sha256': hashlib.sha256(original).hexdigest(),
            'added_field_schema_sha256': hashlib.sha256(changed.read_bytes()).hexdigest(),
            'roundtrip_equal': True, 'added_field_differs': True,
            'migration_emitted': True, 'migration_exit': output.returncode,
            'transfer': json.loads(run.stdout)}
