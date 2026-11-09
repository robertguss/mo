"""One-shot development HTTP characterization; never used by scored collection."""
import sys,os,json,subprocess
from pathlib import Path
sys.path.insert(0,'/home/exedev/code/mo-m6/learnability/phase5/evaluation')
import harness as h
p=h.p4;l=h.layout();out=Path('/home/exedev/.mo-phase5-harness/http-development-20261109-one')
os.umask(0o077)
p.check_private(l,out,l.state,Path('/home/exedev/code/mo-m6'))
h.check(l)
import hashlib
assert hashlib.sha256(Path('/opt/mo-phase5/bin/codex').read_bytes()).hexdigest() == 'b8a11fedf24b2dfb59a70c9e52b030f9a508dcd49cd2dd0d41956f4c785156b8'
out.mkdir(mode=0o700, exist_ok=False)
(out/'declaration.json').write_text(json.dumps({'identity':out.name,'codex':1,'claude':0,'scored':0,'retry_or_replacement':False,'timeout_seconds':180,'model':'gpt-6.1-sol','effort':'xhigh','provider':'development_http','qualified':False}))
summary={}
for client in ('codex',):
 p.reset(l);p.sudo('mkdir','-p',l.work);p.sudo('chown',l.user,l.work)
 env={'HOME':str(l.home()),'USER':l.user,'LOGNAME':l.user,'PATH':f'{l.bin}:/usr/bin:/bin','LANG':'C.UTF-8','DISABLE_AUTOUPDATER':'1','RUST_LOG':'codex_api=trace,codex_core=debug'}
 prompt='Development metadata check. Execute /usr/bin/printf qualification-ok using your shell tool, then report its output. Do not inspect any files or use network tools.'
 if client=='codex':
  cmd=[str(l.bin/client),'exec','--json','--ephemeral','--skip-git-repo-check','-m','gpt-6.1-sol','-c','model_reasoning_effort="xhigh"','-c','model_provider="development_http"','-c','model_providers.development_http={name="OpenAI",requires_openai_auth=true,supports_websockets=false}', '--sandbox','workspace-write','-c','approval_policy="never"','-c','check_for_update_on_startup=false',prompt]
 summary[client]={'attempted':1,'completed':0,'failed':0}
 (out/'counts.json').write_text(json.dumps(summary))
 try:
  with open(out/(client+'.jsonl'),'wb') as stdout,open(out/(client+'.stderr'),'wb') as stderr:
   child=subprocess.Popen(['sudo','-n','-u',l.user,'--','/usr/bin/python3','-I','-c','import os,json,sys; os.chdir(sys.argv[2]); os.execve(sys.argv[3],sys.argv[3:],json.loads(sys.argv[1]))',json.dumps(env),str(l.work),*cmd],stdout=stdout,stderr=stderr,start_new_session=True)
   try: rc=child.wait(timeout=180)
   except subprocess.TimeoutExpired: p.end_all(l.user);rc=child.wait(timeout=30)
  summary[client].update(exit_status=rc,completed=int(rc==0),failed=int(rc!=0))
 finally:
  p.reset(l)
  (out/'counts.json').write_text(json.dumps(summary))
print(json.dumps(summary))
