"""Collector descriptor facts; path annotations never establish file type.

Shared descriptor tables cannot grant regular-file transfer credit. A successful
exec creates an independent descriptor context, so unrelated client threads do
not disqualify an ordinary subprocess copy.
"""
import re

POSITIONED = ('preadv2', 'pwritev2')
KERNEL = ('sendfile', 'splice', 'tee', 'vmsplice', 'copy_file_range')
ASYNC = ('io_uring_setup', 'io_uring_enter', 'io_uring_register', 'io_setup', 'io_submit')
MUTATIONS = ('open', 'openat', 'openat2', 'creat', 'memfd_create', 'close',
             'close_range', 'dup', 'dup2', 'dup3', 'fcntl', 'execve', 'execveat',
             'clone', 'clone3', 'fork', 'vfork', 'unshare', 'socket', 'socketpair',
             'accept', 'accept4', 'pipe', 'pipe2', 'recvmsg', 'recvmmsg')
TRACE_FACTS = 'ioctl,pidfd_getfd,open,openat,openat2,creat,memfd_create,fstat,newfstatat,close_range'


class DescriptorFacts:
    def __init__(self):
        self.tables = {}
        self.ambiguous = set()
        self.exhausted = False

    def observe(self, pid, op, args, result, *, unfinished=False):
        table = self.tables.setdefault(pid, set())
        number = re.fullmatch(r'(\d+)(?:<.*>)?(?:\(deleted\))?', result)
        failed = re.match(r'-1 [A-Z][A-Z0-9_]*(?: |$)', result)
        if failed:
            return False
        if op == 'pidfd_getfd':
            return True
        if op == 'fcntl' and 'F_DUPFD' not in args:
            return False
        if op in KERNEL + POSITIONED + ASYNC:
            if not number:
                return True
            n = int(number[1])
            if op in ASYNC:
                return True
            if n == 0:
                return False
            operands = re.findall(r'(?:^|, )(\d+)(?:<[^>]*>)?', args)
            needed = 2 if op == 'sendfile' else 1
            # Since Linux 5.3 a successful copy_file_range requires two regular files
            # (fs/read_write.c generic_file_rw_checks), so it cannot carry socket or pipe
            # bytes. Channel files still fail closed: their writer reconstruction would
            # miss these bytes. Virtual filesystems stay rejected.
            if op == 'copy_file_range':
                ends = [re.sub(r'\\x([0-9a-fA-F]{2})', lambda m: chr(int(m[1], 16)), e)
                        for e in re.findall(r'(?:^|, )\d+<([^>]*)>', args)]
                if len(ends) == 2 and all(e.startswith('/') and not e.startswith(('/proc/', '/sys/', '/dev/'))
                                          for e in ends):
                    return False
                match = re.match(r'(\d+)(?:<[^>]*>)?(?:\(deleted\))?, [^,]+, (\d+)', args)
                operands = list(match.groups()) if match else []
                needed = 2
            return (op in ('splice', 'tee', 'vmsplice') or unfinished or pid in self.ambiguous or self.exhausted
                    or len(operands) < needed
                    or not all(int(fd) in table for fd in operands[:needed]))
        if op in MUTATIONS and not number:
            self.ambiguous.add(pid)
            return False
        if not number:
            return False
        n = int(number[1])
        first = re.match(r'(\d+)', args)
        fd = int(first[1]) if first else None
        if op in ('execve', 'execveat'):
            self.tables[pid] = set()
            self.ambiguous.discard(pid)
        elif op in ('close_range', 'unshare'):
            table.clear()
        elif op == 'close':
            table.discard(fd)
        elif op in ('clone', 'clone3', 'fork', 'vfork'):
            self.tables[n] = table.copy()
            if 'CLONE_FILES' in args:
                self.ambiguous.update((pid, n))
            elif pid in self.ambiguous:
                self.ambiguous.add(n)
        elif op in ('dup', 'dup2', 'dup3') or op == 'fcntl' and 'F_DUPFD' in args:
            regular = fd in table
            table.discard(n)
            if regular:
                table.add(n)
        elif op in ('open', 'openat', 'openat2', 'creat', 'memfd_create', 'socket', 'accept', 'accept4'):
            table.discard(n)
            if op == 'memfd_create' or ('O_CREAT' in args and 'O_EXCL' in args) or 'O_TMPFILE' in args:
                table.add(n)
        elif op in ('pipe', 'pipe2', 'socketpair', 'recvmsg', 'recvmmsg'):
            # Array/ancillary descriptor acquisition can reuse any closed slot.
            table.clear()
        elif op == 'fstat' or (op == 'newfstatat' and re.match(r'\d+(?:<[^>]*>)?, "", ', args) and 'AT_EMPTY_PATH' in args):
            table.discard(fd)
            if 'st_mode=S_IFREG|' in args:
                table.add(fd)
        if len(table) > 65536 or len(self.tables) > 65536:
            self.exhausted = True
            self.tables.clear()
        return False
