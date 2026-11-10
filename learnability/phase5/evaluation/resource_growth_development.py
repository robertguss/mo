"""Opt-in deterministic parser growth probe; never launches a collector/client."""
import json
import resource
import sys
import time
from resource_probe_development import memory
from origin_trace import parse_trace, decode


class Fixture:
    def __init__(self, kind, count):
        self.kind, self.count = kind, count
        self.decoded_bytes = 0

    def splitlines(self):
        total = 0
        if self.kind == 'payload':
            body = 'a' * 16384
            request = 'POST / HTTP/1.1\\r\\nContent-Length: 16384\\r\\n\\r\\n' + body
            size = len(decode(request))
            rows = (f'10 write(3<TCP:[127.0.0.1:4000->127.0.0.1:80]>, "{request}", {size}) = {size}'
                    for _ in range(self.count * 64))
        else:
            rows = (f'{pid + 10} execve("/usr/bin/true", [], []) = 0' for pid in range(self.count))
        for row in rows:
            total += len(row) + 1
            assert total <= 32 * 1024 * 1024
            yield row
        self.decoded_bytes = total


def main():
    kind, count = sys.argv[1], int(sys.argv[2])
    assert (kind == 'payload' and count in (1, 2, 4)) or (kind == 'process' and count in (100, 1000, 10000))
    initial = memory()
    started = time.monotonic()
    fixture = Fixture(kind, count)
    result = parse_trace(fixture)
    print(json.dumps(dict(kind=kind, count=count, initial=initial, final=memory(),
                         ru_maxrss=resource.getrusage(0).ru_maxrss,
                         elapsed_seconds=time.monotonic()-started,
                         decoded_bytes=fixture.decoded_bytes,
                         requests=len(result['requests']), exec_events=len(result['exec_events']))))


if __name__ == '__main__':
    main()
