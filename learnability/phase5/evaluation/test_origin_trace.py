import unittest
from origin_trace import parse_trace, TraceError


def hx(b):
    return '"' + ''.join('\\x%02x' % x for x in b) + '"'


def line(data, pid=10, fd=3, op='write'):
    return f'{pid} {op}({fd}<TCP:[127.0.0.1:4000->127.0.0.1:80]>, {hx(data)}, {len(data)}) = {len(data)}'


class TraceTests(unittest.TestCase):
    def test_split_repeated_and_response(self):
        body=b'{"id":1,"method":"tools/call","params":{"name":"echo","arguments":{"x":1}}}'
        req=b'POST / HTTP/1.1\r\nAuthorization: secret\r\nContent-Length: '+str(len(body)).encode()+b'\r\n\r\n'+body
        trace=['10 execve("\\x2f\\x63", [], []) = 0',line(req[:20]),line(req[20:]),line(req),line(b'HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n{}',op='read')]
        result=parse_trace('\n'.join(trace))
        self.assertEqual(len(result['requests']),2)
        self.assertEqual(result['requests'][1]['request_ordinal'],2)
        self.assertEqual(result['requests'][0]['sender']['exec_path'],'/c')
        self.assertEqual(len(result['responses']),1)
        self.assertNotIn('secret',str(result))
    def test_fail_closed(self):
        for trace in [line(b'HTTP/1.1 200 OK\r\n\r\n'), '10 write(3<TCP:[a->b]>, "abc"..., 20) = 20',line(b'x')]:
            with self.assertRaises(TraceError): parse_trace(trace)
    def test_unfinished_and_partial(self):
        req=b'POST / HTTP/1.1\r\nContent-Length: 0\r\n\r\n'
        full=line(req)
        prefix,result=full.split(' = ')
        resumed=prefix+' <unfinished ...>\n10 <... write resumed> = '+result
        self.assertEqual(len(parse_trace(resumed)['requests']),1)
        partial=line(req+b'ignored').rsplit(' = ',1)[0]+' = '+str(len(req))
        self.assertEqual(len(parse_trace(partial)['requests']),1)
        with self.assertRaises(TraceError): parse_trace(prefix+' <unfinished ...>')

    def test_exec_and_lifecycle(self):
        req=b'POST / HTTP/1.1\r\nContent-Length: 0\r\n\r\n'
        for middle in ['10 execve("\\x2f\\x64", [], []) = 0',
                       '10 close(3<TCP:[127.0.0.1:4000->127.0.0.1:80]>) = 0']:
            with self.assertRaises(TraceError): parse_trace(line(req)+'\n'+middle+'\n'+line(req))
        concurrent=line(req)+'\n'+line(req,pid=11).replace(':4000', ':4001')
        self.assertEqual(len(parse_trace(concurrent)['requests']),2)

    def test_shared_sender(self):
        with self.assertRaises(TraceError): parse_trace(line(b'POST ')+ '\n'+line(b'/',pid=11))


class ActualSyntaxTests(unittest.TestCase):
    def test_inode_lifecycle_ascii(self):
        trace = '\n'.join(['10 execve("/usr/bin/curl", [], []) = 0',
            '10 socket(AF_INET, SOCK_STREAM, IPPROTO_TCP) = 3<TCP:[123]>',
            '10 getsockopt(3<TCP:[123]>, SOL_SOCKET, SO_ERROR, [0], [4]) = 0',
            line(b'GET / HTTP/1.1\r\n\r\n').replace('127.0.0.1:4000->127.0.0.1:80','123'),
            line(b'HTTP/1.0 200 OK\r\nContent-Length: 2\r\n\r\n{}',op='read').replace('127.0.0.1:4000->127.0.0.1:80','123')])
        r=parse_trace(trace)
        self.assertEqual((len(r['requests']),len(r['responses'])),(1,1))
        self.assertEqual(r['requests'][0]['method'],'GET')
        self.assertEqual(r['responses'][0]['status'],200)
    def test_escaped_ascii(self):
        from origin_trace import decode
        self.assertEqual(decode(r'a\r\n\t\"\\\101\x42'), b'a\r\n\t"\\AB')

class LifecycleTests(unittest.TestCase):
    request = b'POST / HTTP/1.1\r\nContent-Length: 2\r\n\r\n{}'
    def test_alias(self):
        r = parse_trace('\n'.join([
            '10 socket(AF_INET, SOCK_STREAM, IPPROTO_TCP) = 3<TCP:[123]>',
            '10 dup(3<TCP:[123]>) = 4<TCP:[123]>',
            '10 close(3<TCP:[123]>) = 0',
            line(self.request,fd=4).replace('127.0.0.1:4000->127.0.0.1:80','123')]))
        self.assertEqual(len(r['requests']),1)
        self.assertEqual(len(r['requests'][0]['body_sha256']),64)
        self.assertEqual(len(r['requests'][0]['canonical_json_sha256']),64)
    def test_inheritance_before_transfer(self):
        result = parse_trace('\n'.join([
                '10 socket(AF_INET, SOCK_STREAM, IPPROTO_TCP) = 3<TCP:[123]>',
                '10 clone(flags=0) = 11',
                line(self.request,pid=11).replace('127.0.0.1:4000->127.0.0.1:80','123')]))
        self.assertEqual(result['requests'][0]['sender']['pid'], 11)
    def test_direction_and_response_framing(self):
        for response in [b'POST / HTTP/1.1\r\nContent-Length: 0\r\n\r\n',
                         b'HTTP/1.1 200 OK\r\n\r\n']:
            with self.assertRaises(TraceError):
                parse_trace(line(self.request)+'\n'+line(response,op='read'))
    def test_unknown_inode_and_truncation(self):
        for trace in [line(self.request).replace('127.0.0.1:4000->127.0.0.1:80','123'),
                      '10 write(3<TCP:[a->b]>, "POST"..., 100) = 100']:
            with self.assertRaises(TraceError): parse_trace(trace)

if __name__=='__main__': unittest.main()


class OrderingIntervals(unittest.TestCase):
    def test_resumed_buffer_separator_spacing(self):
        request = b'GET / HTTP/1.1\r\n\r\n'
        tcp = line(request).replace('>, "', '>,  "')
        self.assertEqual(len(parse_trace(tcp)['requests']), 1)
        self.assertEqual(parse_trace('10 write(1,  "ok", 2) = 2')['stdout_events'][0]['data'], 'ok')

    def test_kernel_transfers_reject_receipts(self):
        for name in ('sendfile', 'splice', 'tee', 'vmsplice', 'copy_file_range'):
            with self.subTest(name=name), self.assertRaises(TraceError):
                parse_trace('10 '+name+'(1, 2, 3, 4) = 3')
        with self.assertRaises(TraceError):
            parse_trace('10 io_uring_setup(8, {}) = 3')
        with self.assertRaises(TraceError):
            parse_trace('10 io_setup(8, [123]) = 0')
        parse_trace('10 sendfile(1, 2, NULL, 3) = -1 EINVAL (Invalid argument)')
        parse_trace('10 sendfile(1, 2, NULL, 0) = 0')

    def test_pipe_transfer_bytes(self):
        trace='\n'.join(['10 pipe2([3<pipe:[123]>, 4<pipe:[123]>], O_CLOEXEC) = 0',
            '11 write(1<pipe:[123]>, "abc", 3) = 3',
            '10 read(3<pipe:[123]>, "abc", 10) = 3'])
        events=parse_trace(trace)['pipe_events']
        self.assertEqual([e['data_hex'] for e in events], ['616263','616263'])
        self.assertEqual([e['created'] for e in events], [1,1])
        self.assertEqual([e['operation'] for e in events], ['write','read'])
        escaped=''.join('\\x%02x' % b for b in b'pipe:[123]')
        encoded=parse_trace(trace.replace('pipe:[123]',escaped))['pipe_events']
        self.assertEqual(encoded,events)
        spaced=parse_trace(trace.replace('read(3<pipe:[123]>, ', 'read(3<pipe:[123]>,  '))['pipe_events']
        self.assertEqual(spaced,events)

    def test_unfinished_receive_keeps_entry_and_completion(self):
        request = b'POST / HTTP/1.1\r\nContent-Length: 0\r\n\r\n'
        response = b'HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n{}'
        prefix, result = line(response, op='read').split(' = ')
        trace = '\n'.join([line(request), prefix + ' <unfinished ...>',
                           '11 write(1, "final", 5) = 5',
                           '10 <... read resumed> = ' + result])
        receipt = parse_trace(trace)['responses'][0]['occurrences'][0]
        self.assertEqual((receipt['started'], receipt['completed']), (2, 4))
        self.assertLess(receipt['started'], 3)
        self.assertGreater(receipt['completed'], 3)

    def test_completed_request_precedes_receive(self):
        request = b'POST / HTTP/1.1\r\nContent-Length: 0\r\n\r\n'
        response = b'HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n{}'
        parsed = parse_trace(line(request) + '\n' + line(response, op='read'))
        self.assertEqual(parsed['requests'][0]['occurrences'][0]['completed'], 1)
        self.assertEqual(parsed['responses'][0]['occurrences'][0]['started'], 2)


class RealClientShapes(unittest.TestCase):
    request = b'POST / HTTP/1.1\r\nContent-Length: 2\r\n\r\n{}'
    response = b'HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n{}'
    clone = '10 clone(flags=CLONE_VM|CLONE_FILES|CLONE_THREAD) = 11'

    def test_threads_share_socket_and_exec_identity(self):
        parsed = parse_trace('\n'.join(['10 execve("/client", [], []) = 0',
            line(self.request), self.clone, line(self.response, pid=11, op='read')]))
        self.assertEqual(parsed['requests'][0]['sender']['pid'], 10)
        self.assertEqual(parsed['responses'][0]['sender']['pid'], 10)
        self.assertEqual(parsed['responses'][0]['occurrences'][0]['completed'], 4)

    def test_fork_after_handshake_does_not_steal_connection(self):
        parsed = parse_trace('\n'.join([line(self.request),
            '10 clone(flags=CLONE_VM|CLONE_VFORK) = 11',
            '11 execve("/bin/sh", [], []) = 0', line(self.response, op='read')]))
        self.assertEqual(len(parsed['responses']), 1)
        with self.assertRaisesRegex(TraceError, 'shared sender'):
            parse_trace('\n'.join([line(self.request), '10 fork() = 11',
                line(self.response, pid=11, op='read')]))

    def test_cloexec_does_not_leave_stale_socket_alias(self):
        trace = '\n'.join(['10 socket(AF_INET, SOCK_STREAM|SOCK_CLOEXEC, IPPROTO_TCP) = 3<TCP:[123]>',
            '10 fork() = 11', '11 execve("/bin/sh", [], []) = 0',
            '11 write(3</tmp/file>, "abc", 3) = 3',
            line(self.request).replace('127.0.0.1:4000->127.0.0.1:80','123')])
        self.assertEqual(len(parse_trace(trace)['requests']), 1)

    def vector(self, data, op='writev'):
        buffers = ', '.join('{iov_base=' + hx(piece) + ', iov_len=' + str(len(piece)) + '}'
                            for piece in (data[:13], data[13:]))
        prefix = '10 '+op+'(3<TCP:[127.0.0.1:4000->127.0.0.1:80]>, '
        if op in ('sendmsg', 'recvmsg'):
            return prefix + '{msg_name=NULL, msg_namelen=0, msg_iov=['+buffers+'], msg_iovlen=2, msg_controllen=0, msg_flags=0}, 0) = '+str(len(data))
        return prefix + '['+buffers+'], 2) = '+str(len(data))

    def test_vectors_and_partial_prefix(self):
        for write, read in [('writev','readv'), ('sendmsg','recvmsg')]:
            parsed = parse_trace(self.vector(self.request,write)+'\n'+self.vector(self.response,read))
            self.assertEqual(len(parsed['requests']), 1)
            self.assertEqual(len(parsed['responses']), 1)
        from origin_trace import transfer_bytes
        args='3, [{iov_base="abc", iov_len=3}, {iov_base="def", iov_len=3}], 2'
        self.assertEqual(transfer_bytes('writev',args,4), b'abcd')
        for bad in (args.replace('], 2', '], 3'), args.replace('"def"','"d"'),
                    args.replace('"abc"','"abc"...')):
            with self.assertRaises(TraceError): transfer_bytes('writev',bad,6)
        with self.assertRaises(TraceError):
            parse_trace(self.vector(self.request,'sendmsg').replace('msg_controllen=0','msg_controllen=8'))

class ReviewedBoundaries(unittest.TestCase):
    def test_duplicate_endpoint_rejected_in_both_creation_orders(self):
        req=b'GET / HTTP/1.1\r\n\r\n'
        # Deliberately model an unobserved descriptor acquisition/flag change.
        prefix=['10 socket(AF_INET, SOCK_STREAM|SOCK_CLOEXEC, IPPROTO_TCP) = 3<TCP:[123]>',
                '10 fork() = 11', '11 execve("/other", [], []) = 0']
        for writes in ([line(req,pid=11),line(req)], [line(req),line(req,pid=11)]):
            with self.subTest(writes=writes), self.assertRaises(TraceError):
                parse_trace('\n'.join(prefix+writes))

    def test_unix_ancillary_and_pidfd_delivery_refused(self):
        for trace in ('10 recvmsg(3<UNIX-STREAM:[123]>, {msg_controllen=24}, 0) = 1',
                      '10 sendmsg(3<UNIX:[123]>, {msg_controllen=24}, 0) = 1',
                      '10 pidfd_getfd(3, 4, 0) = 5'):
            with self.subTest(trace=trace), self.assertRaises(TraceError): parse_trace(trace)

    def test_cloned_vm_mappings_poison_native_identity(self):
        trace='\n'.join(['10 execve("/mo", ["mo", "push"], []) = 0',
            '10 clone(flags=CLONE_VM|CLONE_VFORK) = 11',
            '11 mprotect(0x1000, 4096, PROT_READ|PROT_EXEC) = 0',
            '11 execve("/other", [], []) = 0',line(b'GET / HTTP/1.1\r\n\r\n')])
        self.assertIn(None,parse_trace(trace)['requests'][0]['sender']['executable_mappings'])
