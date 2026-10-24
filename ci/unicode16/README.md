# Unicode 16.0.0 sources

The `.gz` files contain unmodified source files from the official
<https://www.unicode.org/Public/zipped/16.0.0/UCD.zip>. `sha256.json` records
the SHA-256 of each **decompressed** file. The generator also pins the whole
archive's hash. Unicode's permission notice is in `LICENSE.txt`.

Regenerate without network access:

```sh
python3 ci/generate_unicode16.py
python3 ci/generate_unicode16.py --check
```

To reproduce the imported sources, download that archive and pass its path
with `--archive`. Generation refuses an archive with a different hash. The
generator uses only this data, not Python's installed Unicode version.

`ci/check_unicode16.py` runs every `GraphemeBreakTest` vector and all five
NFC invariants of every `NormalizationTest` row, including identity for
assigned scalars outside Part 1. It checks full case mappings from
`UnicodeData` and `SpecialCasing`, with context from `DerivedCoreProperties`.
Locale-specific mappings are excluded; language-insensitive final sigma is
included. Width uses the policy in `docs/design/library.md`. Every valid
Unicode scalar is checked through both classification primitives against
General_Category L and Nd from `UnicodeData.txt`, including First/Last ranges
and the false results for unassigned code points. This oracle reads categories
independently of the generated runtime table.

The C tables coalesce property ranges, deduplicate mapping sequences and
compute Hangul decomposition/composition and LV/LVT classes algorithmically.
Lookups use binary search, with a generated direct table for ASCII. Canonical
decompositions are expanded during generation and have at most four scalars;
full case mappings have at most three. Runtime canonical ordering uses stable
counting sort on long combining runs, not quadratic insertion sort.
