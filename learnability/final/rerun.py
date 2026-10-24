#!/usr/bin/env python3
"""Run the frozen corpus and labelled scratch probes without changing the recordings.

From the repository root: python3 learnability/final/rerun.py build/gcc/debug/mo
JSON on stdout contains every command, exit status, stdout and stderr.
"""

import argparse
import json
import pathlib
import re
import subprocess
import tempfile


def run(binary, path, *arguments):
    command = [str(binary), *arguments, str(path)]
    result = subprocess.run(command, capture_output=True, text=True, timeout=120)
    return dict(command=command, exit=result.returncode,
                stdout=result.stdout, stderr=result.stderr)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=pathlib.Path)
    args = parser.parse_args()
    binary = args.binary.resolve(strict=True)
    corpus = pathlib.Path(__file__).resolve().parent
    paths = sorted(corpus.glob("*/*.mo"))
    if len(paths) != 32:
        parser.error(f"expected 32 recorded programs, found {len(paths)}")
    results = {"originals": [], "probes": []}
    for path in paths:
        results["originals"].append(dict(
            program=str(path.relative_to(corpus)),
            check=run(binary, path, "check"), test=run(binary, path, "test")))
    with tempfile.TemporaryDirectory(prefix="mo121-") as temporary:
        for family, name in (
            ("claude-code", "job-queue"), ("claude-code", "json"),
            ("claude-code", "kv"), ("gpt-6.1-sol", "job-queue"),
            ("gpt-6.1-sol", "rate-limit"), ("gpt-6.1-sol", "json"),
            ("gpt-6.1-sol", "kv"),
        ):
            relative = pathlib.Path(family) / f"{name}.mo"
            source = (corpus / relative).read_text(encoding="utf-8")
            changes = []
            folder = pathlib.Path(temporary) / family
            if "module Answers." in source:
                folder /= "answers"
                changes.append("restore answers/ directory; source bytes unchanged")
            if name == "json":
                source = source.replace("module Json\n", "module JsonProbe\n")
                name = "json-probe"
                changes.append("module Json -> JsonProbe; filename json-probe.mo")
            if name == "kv":
                source = re.sub(r"\bEntry\b", "StoreEntry", source)
                changes.append("Entry -> StoreEntry")
            if family == "claude-code" and name == "job-queue":
                source = source.replace("BadLength(_)", "BadLength(length: _)")
                changes.append("BadLength(_) -> BadLength(length: _)")
            folder.mkdir(parents=True, exist_ok=True)
            path = folder / f"{name}.mo"
            path.write_text(source, encoding="utf-8")
            probe = dict(program=str(relative), changes=changes,
                         check=run(binary, path, "check"), test=run(binary, path, "test"))
            if family == "gpt-6.1-sol" and name == "job-queue":
                probe["selected_promises"] = [
                    run(binary, path, "test", "--only", f"Answers.JobQueue.{fn}.promise.1")
                    for fn in ("submit", "expire_leases", "lease", "acknowledge", "fail")
                ]
            results["probes"].append(probe)
    print(json.dumps(results, indent=2))


if __name__ == "__main__":
    main()
