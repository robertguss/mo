import copy
import json
import unittest
import observation
class Controls(unittest.TestCase):
    def fixture(self):
        owner=dict(pid=1,incarnation=1,generation=1,exec_path='/client',exec_argv=['/client'],session_id='s')
        def receipt(a,b): return dict(sender=owner,connection_ordinal=a,request_ordinal=1,occurrences=[dict(started=b,completed=b)])
        origins={1:dict(receipt(1,1),verified_push_identity=True),3:receipt(2,3)}
        rows=[dict(direction='request',seq=1,id=1,tool='mo-upgrade-commit'),dict(direction='response',seq=2,request=1,status=200,body={'result':{'structuredContent':{'status':'accepted','ordinal':'2'}}}),dict(direction='request',seq=3,id=2,tool='revision'),dict(direction='response',seq=4,request=3,status=200,body={'result':{'structuredContent':{'value':'1'}}})]
        stream=(json.dumps(dict(type='item.completed',item=dict(type='agent_message',text='1')))+'\n'+json.dumps(dict(type='turn.completed'))+'\n').encode()
        evidence=dict(stdout=stream,client_path='/client',trace=dict(exec_events=[owner],responses=[receipt(1,2),receipt(2,4)],stdout_events=[dict(data=stream.decode(),sender=owner,started=5,completed=5)]))
        return evidence,rows,origins,stream
    def test_untyped_receive_is_insufficient(self): self.assertFalse(observation.validate(*self.fixture()))
    def native_fixture(self):
        e,r,o,s=self.fixture()
        item=dict(id='item_1',type='mcp_tool_call',server='task-queue',tool='revision',arguments={},result={'content': [], 'structured_content': {'value':'1'}},error=None,status='completed')
        prefix=(json.dumps(dict(type='item.completed',item=item))+'\n').encode()
        e['trace']['stdout_events'].insert(0,dict(data=prefix.decode(),sender=e['trace']['exec_events'][0],started=4.5,completed=4.5))
        e['stdout']=prefix+s
        return e,r,o,prefix+s
    def test_native_typed_result(self):
        self.assertTrue(observation.validate(*self.native_fixture()))
    def shell_fixture(self):
        e,r,o,s=self.fixture()
        owner=e['trace']['exec_events'][0]
        child=dict(owner,pid=2,exec_path='/usr/bin/curl',exec_argv=['curl'])
        o[3]['sender']=child
        e['trace']['responses'][1]['sender']=child
        output=json.dumps(r[3]['body'])+'\n'
        item=dict(id='shell-1',type='command_execution',exit_code=0,status='completed',aggregated_output=output)
        prefix=(json.dumps(dict(type='item.completed',item=item))+'\n').encode()
        e['trace']['stdout_events'].insert(0,dict(data=prefix.decode(),sender=owner,started=4.8,completed=4.8))
        e['trace']['pipe_events']=[
            dict(pipe='12',created=2,operation='write',fd=1,data_hex=output.encode().hex(),sender=child,started=4.1,completed=4.2),
            dict(pipe='12',created=2,operation='read',fd=3,data_hex=output.encode().hex(),sender=owner,started=4,completed=4.3)]
        e['stdout']=prefix+s
        return e,r,o,prefix+s
    def test_shell_delivery(self):
        self.assertTrue(observation.validate(*self.shell_fixture()))
    def test_uncapturable_pipe_or_stdout_transfer_fails_closed(self):
        e,r,o,s=self.shell_fixture()
        e['trace']['pipe_events'][0]['data_hex']=None
        self.assertFalse(observation.validate(e,r,o,s))
        e,r,o,s=self.shell_fixture()
        e['trace']['stdout_events'][0]['data']=None
        self.assertFalse(observation.validate(e,r,o,s))
    def test_shell_printed_revision(self):
        e,r,o,s=self.shell_fixture()
        event=json.loads(s.splitlines()[0])
        event['item']['aggregated_output']='1\n'
        prefix=json.dumps(event)+'\n'
        e['trace']['stdout_events'][0]['data']=prefix
        e['stdout']=prefix.encode()+b'\n'.join(s.splitlines()[1:])+b'\n'
        for transfer in e['trace']['pipe_events']: transfer['data_hex']=b'1\n'.hex()
        self.assertTrue(observation.validate(e,r,o,e['stdout']))
    def test_claude_shell_delivery(self):
        e,r,o,s=self.shell_fixture()
        output=json.loads(s.splitlines()[0])['item']['aggregated_output']
        events=[
            dict(type='assistant',message={'content':[dict(type='tool_use',id='bash-1',name='Bash',input={'command':'curl'})]}),
            dict(type='user',message={'content':[dict(type='tool_result',tool_use_id='bash-1',content=output,is_error=False)]}),
            dict(type='assistant',message={'content':[dict(type='text',text='1')]}),
            dict(type='result',subtype='success',is_error=False,result='1')]
        prefix=''.join(json.dumps(v)+'\n' for v in events[:2])
        final=''.join(json.dumps(v)+'\n' for v in events[2:])
        e['trace']['stdout_events'][0]['data']=prefix
        e['trace']['stdout_events'][1]['data']=final
        e['stdout']=(prefix+final).encode()
        self.assertTrue(observation.validate(e,r,o,e['stdout']))
        events[0]['message']['content'][0]['name']='Read'
        prefix=''.join(json.dumps(v)+'\n' for v in events[:2])
        e['trace']['stdout_events'][0]['data']=prefix
        e['stdout']=(prefix+final).encode()
        self.assertFalse(observation.validate(e,r,o,e['stdout']))
    def test_shell_delivery_negatives(self):
        for case in ('no_read','wrong_reader','wrong_writer','different_bytes','late_read','early_write','uncreated','shared_writer'):
            e,r,o,s=self.shell_fixture()
            self.assertTrue(observation.validate(e,r,o,s))
            pipes=e['trace']['pipe_events']
            if case=='no_read': pipes.pop()
            if case=='wrong_reader': pipes[1]['sender']=dict(pipes[1]['sender'],pid=9)
            if case=='wrong_writer': pipes[0]['sender']=dict(pipes[0]['sender'],pid=9)
            if case=='different_bytes': pipes[1]['data_hex']='31'
            if case=='late_read': pipes[1]['completed']=6
            if case=='early_write': pipes[0]['started']=3
            if case=='uncreated': pipes[0]['created']=None
            if case=='shared_writer': pipes.append(dict(pipes[0],sender=dict(pipes[0]['sender'],pid=9)))
            with self.subTest(case=case): self.assertFalse(observation.validate(e,r,o,s))
    def test_native_controls(self):
        for case in ('before_receive', 'after_final', 'wrong_tool', 'fake_child', 'truncated', 'wrong_value'):
            e,r,o,s=self.native_fixture()
            if case == 'before_receive': e['trace']['stdout_events'][0]['started']=3.5
            if case == 'after_final': e['trace']['stdout_events'][0]['completed']=6
            if case == 'fake_child': e['trace']['stdout_events'][0]['sender']=dict(e['trace']['exec_events'][0],pid=99)
            if case == 'truncated': s=s[:-3]
            if case in ('wrong_tool','wrong_value'):
                event=json.loads(s.splitlines()[0])
                if case == 'wrong_tool': event['item']['tool']='stats'
                else: event['item']['result']['structured_content']['value']='11'
                first=json.dumps(event)+'\n'
                s=first.encode()+b'\n'.join(s.splitlines()[1:])+b'\n'
                e['stdout']=s
                e['trace']['stdout_events'][0]['data']=first
            with self.subTest(case=case): self.assertFalse(observation.validate(e,r,o,s))
    def test_reused_rpc_id_is_not_an_occurrence_identity(self):
        e,r,o,s=self.native_fixture()
        r[2]['id']=r[0]['id']
        self.assertTrue(observation.validate(e,r,o,s))
    def test_negative_controls(self):
        for case, name in enumerate(('guess-before-query', 'query-before-commit', 'query-after-final', 'refused', 'wrong-revision', 'nonclient-stdout', 'duplicate-response')):
            e,r,o,s=self.native_fixture()
            self.assertTrue(observation.validate(e,r,o,s), name)
            if case==0: r=r[:2]
            if case==1: o[3]['occurrences'][0]['started']=1
            if case==2: e['trace']['responses'][1]['occurrences'][0]['completed']=6
            if case==3: r[3]['body']['result']['isError']=True
            if case==4: r[3]['body']['result']['structuredContent']['value']='9'
            if case==5: e['client_path']='/other'
            if case==6: r.append(copy.deepcopy(r[3]))
            with self.subTest(case=name): self.assertFalse(observation.validate(e,r,o,s))
if __name__=='__main__': unittest.main()
