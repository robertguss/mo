"""Publish only the four reserved experiment-3 slots, with protected replay."""
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'learnability/phase5/evaluation'))
import harness
import protected_regrade
import publish

if __name__ == '__main__':
    layout = harness.layout()
    experiment = layout.state / 'experiment-3'
    sessions = [experiment / f'slot-{slot}' for slot in range(1, 5)]
    uid = layout.account().pw_uid
    bindings = {p.resolve(): (str(p.resolve()), uid) for p in sessions}
    work = experiment / 'publication-work'
    work.mkdir(mode=0o700, exist_ok=True)
    receipt = publish.publish(sessions, experiment / 'publication',
        [layout.tokens], [layout.secrets],
        protected_regrade=protected_regrade.callback(bindings, work))
    print(json.dumps(receipt, indent=2))
