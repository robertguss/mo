"""Development-only preload control. Run with sudo; no model or service credentials."""
import json
import os
from pathlib import Path
import pwd
import shutil
import socket
import subprocess
import tempfile
import threading

from origin_identity import Capture
from origin_observer import TraceRun


SOURCE = r'''
#include <arpa/inet.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>
__attribute__((constructor)) static void injected(void) {
    const char *port = getenv("CONTROL_PORT");
    if (!port) _exit(80);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_port = htons((unsigned short)atoi(port));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (fd < 0 || connect(fd, (struct sockaddr *)&address, sizeof(address))) _exit(81);
    const char request[] = "GET /injected HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    if (write(fd, request, sizeof(request)-1) != sizeof(request)-1) _exit(82);
    char buffer[256];
    while (read(fd, buffer, sizeof(buffer)) > 0) {}
    if (close(fd)) _exit(83);
    _exit(0);
}
'''


def run_control():
    assert os.geteuid() == 0
    os.umask(0o077)
    account = pwd.getpwnam('mo-phase5-session')
    root = Path(tempfile.mkdtemp(prefix='mo-execution-identity-', dir='/root'))
    install = Path(tempfile.mkdtemp(prefix='mo-execution-install-', dir='/opt'))
    install.chmod(0o755)
    mo = Path(__file__).resolve().parents[3] / 'build/gcc/debug/mo'
    binary = install / 'mo'
    shutil.copyfile(mo, binary)
    binary.chmod(0o555)
    source = root / 'preload.c'
    source.write_text(SOURCE)
    library = install / 'preload.so'
    subprocess.run(['gcc', '-shared', '-fPIC', '-Wall', '-Wextra', '-Werror',
                    str(source), '-o', str(library)], check=True)
    library.chmod(0o444)
    listener = socket.socket()
    listener.bind(('127.0.0.1', 0))
    listener.listen()
    listener.settimeout(10)
    requests = []
    errors = []
    def serve():
        try:
            with listener.accept()[0] as peer:
                peer.settimeout(5)
                request = b''
                while b'\r\n\r\n' not in request:
                    chunk = peer.recv(4096)
                    if not chunk: raise RuntimeError('truncated request')
                    request += chunk
                requests.append(request)
                peer.sendall(b'HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n{}')
        except Exception as error:
            errors.append(type(error).__name__)
    thread = threading.Thread(target=serve)
    capture = Capture(root / 'identity', 'preload-control', account.pw_uid, binary)
    thread.start()
    try:
        result = TraceRun(root / 'raw').run(
            ['/usr/bin/env', 'LD_PRELOAD=' + str(library),
             'CONTROL_PORT=' + str(listener.getsockname()[1]), str(binary), 'push'],
            timeout=15, env={'PATH': '/usr/bin:/bin'}, session_user=account.pw_name,
            cwd=install)
        thread.join(12)
        assert not thread.is_alive() and not errors
        assert len(requests) == 1 and result['capture_finished']
        log = root / 'proxy.jsonl'
        log.write_text('')
        capture.seal(root / 'raw/trace', log)
        evidence = capture.consume('preload-control')
        receipts = evidence['trace']['requests']
        assert len(receipts) == 1
        receipt = receipts[0]
        assert receipt['sender']['exec_path'] == str(binary)
        assert receipt['sender']['exec_argv'][1] == 'push'
        assert receipt['sender']['loader_context_clean'] is False
        assert receipt['verified_push_identity'] is False
        summary = dict(requests=1, within_pinned_mo_process=True,
                       verified_push=0, qualified=False, private_root=str(root))
        (root / 'result.json').write_text(json.dumps(summary))
        print(json.dumps(summary))
    finally:
        listener.close()
        thread.join(12)
        shutil.rmtree(install)


if __name__ == '__main__':
    run_control()
