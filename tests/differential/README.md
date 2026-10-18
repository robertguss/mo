Programs the nightly VM-against-Lean run (`ci/check_core_differential.py`) found to differ, after
minimizing and fixing. Each `NAME.core` is a bare `(core 0.8 (Program ...))` with a zero-argument
entry; every run of the script compares it first, so the fix stays fixed.
