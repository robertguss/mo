#!/usr/bin/env python3
"""Pinned finite TLC instances, including counterexamples that must be found."""
import hashlib
import os
import pathlib
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SHA256 = "e20447644bfabd29d6bd0fc2c0eb1473fb301c1a0e4dfe1961938ce5a191edbf"
URL = "https://github.com/tlaplus/tlaplus/releases/download/v1.8.0/tla2tools.jar"


def main():
    jar = pathlib.Path(os.environ.get("TLA2TOOLS_JAR", ROOT / "build/models/tla2tools.jar"))
    if not shutil.which("java") or not jar.is_file():
        print(f"missing Java or pinned TLC jar; place {URL} at {jar}; SHA-256 {SHA256}")
        return 1 if os.environ.get("CI") == "1" else 77
    if hashlib.sha256(jar.read_bytes()).hexdigest() != SHA256:
        raise SystemExit("TLC jar digest does not match the pinned release")
    out = ROOT / "build/models"
    out.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(ROOT / "models/Export.tla", out / "Export.tla")
    shutil.copyfile(ROOT / "models/Capture.tla", out / "Capture.tla")
    for config, violation in [("Capture", None), ("CaptureBadPublish", "PairComplete"),
                              ("CaptureBadCount", "CountedOnce"), ("Export", None), ("ExportProgress", None),
                              ("ExportBadGeneration", "NoMixedGeneration"),
                              ("ExportBadRelease", "NoReleasedRead")]:
        command = ["java", "-XX:+UseParallelGC", "-Xmx512m", "-cp", str(jar.resolve()),
                   "tlc2.TLC", "-workers", "1", "-seed", "1", "-metadir", str(out / config),
                   "-config", str(ROOT / "models" / (config + ".cfg")),
                   str(out / ("Capture.tla" if config.startswith("Capture") else "Export.tla"))]
        result = subprocess.run(command, cwd=ROOT, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=120)
        (out / (config + ".log")).write_bytes(result.stdout)
        text = result.stdout.decode()
        if violation:
            assert result.returncode == 12 and f"Invariant {violation} is violated" in text, text
            print(f"{config}: expected {violation} counterexample detected")
        else:
            assert result.returncode == 0 and "No error has been found" in text, text
            print(f"{config}: passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
