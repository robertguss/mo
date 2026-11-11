from pathlib import Path
import json,hashlib,subprocess,datetime
root=Path.cwd();out=root/'build/freeze-3'
def digest(p): return hashlib.sha256(p.read_bytes()).hexdigest()
def files(paths):
 result={}
 for p in paths:
  if not p.exists(): raise FileNotFoundError(str(p))
  for q in ([p] if p.is_file() else p.rglob('*')):
   if q.is_file(): result[str(q.resolve())]=digest(q)
 return result
spec=json.loads((out/'spec.json').read_text())
inputs=json.loads((out/'build-inputs.json').read_text())
groups={
 'protocol':files([root/'learnability/phase5/protocol.md',root/'learnability/phase5/collection.md',out/'declaration.json']),
 'prompt':files([Path(spec['prompt'])]),
 'harness':files(list((root/'learnability/phase5/evaluation').glob('*.py'))+list((root/'learnability/phase4').glob('*.py'))),
 'proxy':files([root/'learnability/phase5/evaluation/proxy.py',root/'learnability/phase4/proxy.py']),
 'scanner':files([root/'learnability/phase4/scan.py',root/'learnability/phase5/evaluation/publish.py']),
 'grader':files([root/'learnability/phase5/evaluation/grade.py',root/'learnability/phase5/evaluation/protected_regrade.py',root/'learnability/phase5/evidence.py',out/'publish_experiment.py']),
 'fixtures':files([Path(spec[k]) for k in ('source','context','data','expected')]+[out/'spec.json',out/'fixture-continuity.json',root/'services/task-queue',root/'learnability/phase5/evaluation/fixtures']),
 'controls':files([out/'controls.json',out/'harness-controls.txt',out/'focused-controls.txt',out/'collector-controls.txt',out/'lock-collector-controls.txt']),
 'build_inputs':files([root/n for n in inputs]+[Path('/opt/mo-phase5/build-e53a822e')/n for n in inputs]+[out/'build-inputs.json',out/'m6-comparison.json',out/'installed-source-comparison.json']),
 'build_commands':files([out/'build-commands.json',out/'build-history-receipt.json',out/'eval-build-e53a822e.txt',out/'setup-eval-build.txt']),
 'compiler_versions':files([out/'compiler-versions.json']),
 'verification':files([out/'verification.json',out/'gcc.txt',out/'prior-results-hashes.json',out/'resource-summary.json']+list(out.glob('resource-*.json'))+list(out.glob('measure_*.py'))),
}
f=dict(created_at=datetime.datetime.now(datetime.timezone.utc).isoformat(),repository=str(root),experiment='experiment-3',repair_commit=subprocess.check_output(['git','rev-parse','origin/main']).decode().strip(),main_commit=subprocess.check_output(['git','rev-parse','origin/main']).decode().strip(),candidate_commit=subprocess.check_output(['git','rev-parse','HEAD']).decode().strip(),slots=['codex','claude','codex','claude'],verification_evidence=str(out/'verification.json'),controls_evidence=str(out/'controls.json'),binaries={n:digest(Path('/opt/mo-phase5/bin')/n) for n in ('mo','codex','claude')},artifacts=groups)
(out/'manifest.json').write_text(json.dumps(f,indent=2)+'\n')
print('Freeze artifact count:',sum(map(len,groups.values())))
