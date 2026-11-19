# Recount

A fresh agent (a Claude Code subagent on claude-opus-5-5, started 2026-11-19) recounted the
measurement from the six result files alone: the four `results/strength/*.json` and the two
`results/mutants/*.list.json`. Its brief told it to read no other file, including the record
and `count.py`. It counted with its own script. Its report, verbatim:

I counted with my own script, `/tmp/claude-1000/sc/count.py`, run as `python3 -I /tmp/claude-1000/sc/count.py <repo>/learnability/trust-gates/results`. It reads only lines starting with `{` in the six files. It tallies `outcome` over each strength record's `versions` and tallies the `mutant` and `function` records. A one-liner then compared per-function `mutants` between the two list files. I read no other repo files.

**frozen-working.json**
1. 12 versions, all names distinct.
2. promise 1, example 2, both 0, test 6, survived 3. invalid, changes_intent, defect, unfinished and not_counted are all 0.
3. Killed by promise or example: 3.
4. Killed by any check: 9.
5. Survived: w02.mo, w03.mo, w09.mo. Uncountable: none.
6. The record says killed=3, killed_any=9, scored=12, partial=false. All agree with my counts, and its `counts` object matches mine exactly.

**frozen-held-back.json**
1. 12 versions.
2. promise 2, example 0, both 0, test 8, survived 2. All uncountable outcomes are 0.
3. Killed by promise or example: 2.
4. Killed by any check: 10.
5. Survived: w16.mo, w18.mo. Uncountable: none.
6. The record says killed=2, killed_any=10, scored=12, partial=false. All agree, and `counts` matches.

**fitted-working.json**
1. 12 versions.
2. promise 0, example 3, both 1, test 5, survived 3. All uncountable outcomes are 0.
3. Killed by promise or example: 4.
4. Killed by any check: 9.
5. Survived: w02.mo, w03.mo, w09.mo. Uncountable: none.
6. The record says killed=4, killed_any=9, scored=12, partial=false. All agree, and `counts` matches.

**fitted-held-back.json**
1. 12 versions.
2. promise 1, example 3, both 1, test 5, survived 2. All uncountable outcomes are 0.
3. Killed by promise or example: 5.
4. Killed by any check: 10.
5. Survived: w16.mo, w18.mo. Uncountable: none.
6. The record says killed=5, killed_any=10, scored=12, partial=false. All agree, and `counts` matches.

**frozen.list.json**
7. 67 mutants: arithmetic 6, bool 1, boundary 11, int 24, logic 7, negate 18.
8. 35 functions, 17 with 0 mutants. The per-function `mutants` values add up to 67, which matches the mutant lines.

**fitted.list.json**
7. 66 mutants: arithmetic 6, bool 1, boundary 11, int 24, logic 6, negate 18.
8. 35 functions, 17 with 0 mutants. The per-function values add up to 66, which matches the mutant lines.

9. None. No function has mutants in frozen and 0 or no `function` line in fitted. Both files list the same 35 function names. The only per-function difference is `QueueTests.job`, with 4 mutants in frozen and 3 in fitted; that is where fitted's one fewer `logic` mutant comes from.

I took `partial=false` to be correct because no version has an uncountable outcome; I didn't check what the field actually means. I counted every `"record":"strength"` record, and each file holds exactly one.
