#!/usr/bin/env python3
"""Pinned finite TLC instances, including counterexamples that must be found."""
import hashlib
import os
import pathlib
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SHA256 = "936a262061c914694dfd669a543be24573c45d5aa0ff20a8b96b23d01e050e88"
URL = "https://github.com/tlaplus/tlaplus/releases/download/v1.7.4/tla2tools.jar"


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
    shutil.copyfile(ROOT / "models/Swap.tla", out / "Swap.tla")
    for config, violation in [("Capture", None), ("CaptureBadPublish", "PairComplete"),
                              ("CaptureBadCount", "CountedOnce"),
                              ("CaptureBadRecovery", "RecoveryUntouched"),
                              ("CaptureBadUnsafe", "NoUnsafeContinue"),
                              ("CaptureBadRetention", "Retention"),
                              ("CaptureBadRepeat", "RepeatKeepsOthers"),
                              ("Export", None), ("ExportProgress", None),
                              ("ExportBadGeneration", "NoMixedGeneration"),
                              ("ExportBadRelease", "NoReleasedRead"),
                              ("Swap", None), ("SwapBadAuthority", "SingleAuthority"),
                              ("SwapBadConservation", "MessageConservation"),
                              ("SwapBadCrossRecord", "NoCrossUpgradeRecord"), ("SwapDoor", None),
                              ("SwapBadVersion", "NoUnreviewedIntent"),
                              ("SwapBadReview", "YesNamesCandidate")]:
        module = next(m for m in ("Capture", "Export", "Swap") if config.startswith(m))
        shutil.copyfile(ROOT / "models" / (config + ".cfg"), out / (config + ".cfg"))
        command = ["java", "-XX:+UseParallelGC", "-Xmx512m", "-cp", str(jar.resolve()),
                   "tlc2.TLC", "-workers", "1", "-seed", "1", "-metadir", str(out / config),
                   "-config", str(out / (config + ".cfg")),
                   str(out / (module + ".tla"))]
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
