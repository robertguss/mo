import unittest
from origin_observer import process_integrity
from origin_trace import parse_trace, TraceError


class DescriptorTests(unittest.TestCase):
    def check_trace(self, body, accepted):
        text = '1 execve("/bin/true", [], []) = 0\n' + body + '\n1 +++ exited with 0 +++\n'
        self.assertEqual(process_integrity(text)['process_completeness_verified'], accepted)
        if accepted:
            parse_trace(text)
        else:
            with self.assertRaises(TraceError):
                parse_trace(text)

    def test_regular_sendfile(self):
        self.check_trace('1 fstat(3</tmp/in>, {st_mode=S_IFREG|0600}) = 0\n1 fstat(4</tmp/out>, {st_mode=S_IFREG|0600}) = 0\n1 sendfile(4</tmp/out>, 3</tmp/in>, NULL, 4) = 4', True)

    def test_memfd_dup(self):
        self.check_trace('1 memfd_create("a", 0) = 3\n1 dup(3) = 4\n1 sendfile(4, 3, NULL, 4) = 4', True)

    def test_unknown_named_fifo_and_results(self):
        for call in ('sendfile(4</tmp/fifo>, 3</tmp/in>, NULL, 4) = 4',
                     'sendfile(4, 3, NULL, 4) = ?',
                     'pwritev2(4<pipe:[1]>, [], 1, -1, 0) = 3',
                     'preadv2(4<TCP:[1]>, [], 1, -1, 0) = ?'):
            self.check_trace('1 ' + call, False)

    def test_reuse(self):
        self.check_trace('1 memfd_create("a", 0) = 3\n1 close(3) = 0\n1 openat(AT_FDCWD, "/tmp/fifo", O_RDWR) = 3\n1 pwritev2(3, [], 1, -1, 0) = 3', False)

    def test_unfinished_transfer(self):
        self.check_trace('1 memfd_create("a", 0) = 3\n1 pwritev2(3, [], 1, -1, 0 <unfinished ...>\n1 <... pwritev2 resumed>) = 3', False)

    def test_shared_parent_does_not_poison_exec_child(self):
        self.check_trace('1 clone(flags=CLONE_FILES) = 2\n1 fork() = 3\n'
            '3 execve("/bin/copy", [], []) = 0\n3 memfd_create("a", 0) = 3\n'
            '3 memfd_create("b", 0) = 4\n3 sendfile(4, 3, NULL, 3) = 3\n'
            '3 +++ exited with 0 +++\n2 +++ exited with 0 +++', True)

    def test_shared_descriptor_table_rejects_transfer(self):
        self.check_trace('1 memfd_create("a", 0) = 3\n1 memfd_create("b", 0) = 4\n'
            '1 clone(flags=CLONE_FILES) = 2\n2 sendfile(4, 3, NULL, 3) = 3\n'
            '2 +++ exited with 0 +++', False)
