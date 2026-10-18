Programs the nightly VM-against-Lean run (`ci/check_core_differential.py`) found to differ, after
minimizing and fixing. Each `NAME.core` is a bare `(core 0.8 (Program ...))` with a zero-argument
entry, run against the generator's empty world (`gen_core.WORLD`). Every run of the script compares
these first, so a fix stays fixed. `entry-returns-one.core` is a seed fixture that keeps the path
exercised.
