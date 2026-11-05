#!/usr/bin/env python3
"""Measure mo serve's resident memory and the mo-state reply at the largest saved state.

  python3 learnability/phase4/live-views/sizing.py MO > learnability/phase4/live-views/results/sizing.json

Serves one state holding a 16,000,000-byte Text, reads /proc before and after a whole-state
mo-state call, and prints one JSON object with the reply size and the mo-memory view. Resident
memory includes the compiler and runtime, so it describes this run, not a universal bound.
"""

import hashlib
import json
import pathlib
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / 'preflight'))
from sizing import Server  # noqa: E402

TEXT_BYTES = 16_000_000
SOURCE = f'''module Main
struct Large
  text: Text
  answer: Int
end
service
  state Large = Large(text: "x".repeat({TEXT_BYTES}), answer: 42)
  tools read
  token from env "SIZING_TOKEN"
end
fn read(state: Large, _now: Time) : Int
  state.answer
end
'''


def kib(server):
    peak, resident = server.memory()
    return {'VmHWM': peak, 'VmRSS': resident}


def main(binary):
    binary = pathlib.Path(binary).resolve()
    with tempfile.TemporaryDirectory(prefix='mo-live-sizing-') as directory:
        folder = pathlib.Path(directory)
        program = folder / 'main.mo'
        program.write_text(SOURCE, encoding='utf-8')
        data = folder / 'data'
        data.mkdir()
        server = Server(binary, program, 'SIZING_TOKEN', data)
        before = kib(server)
        content, failed, _elapsed, size = server.call('mo-state')
        assert not failed, content
        after = kib(server)
        memory, failed, _elapsed, _size = server.call('mo-memory')
        assert not failed, memory
        status, stderr, _final = server.stop()
        assert status == 0, stderr
    value = content['value']
    print(json.dumps({
        'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
        'encoded_bytes': memory['value']['encoded_state_bytes'],
        'response_bytes': size,
        'complete': value['complete'],
        'before_kib': before,
        'after_kib': after,
        'memory_view': memory['value'],
        'note': f'One synthetic {TEXT_BYTES:,}-byte Text run; process RSS includes '
                'compiler/runtime, not a universal bound.',
    }, indent=2))


if __name__ == '__main__':
    main(sys.argv[1])
