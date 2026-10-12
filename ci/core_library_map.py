#!/usr/bin/env python3
"""Check runtime primitives against both Core operation tables.

library_map.sh first checks every library row against the runtime tables. This
second check covers their Core route, including arithmetic bytecode operations.
Most C ids have the same suffix in Core; the explicit aliases/recipes below are
the existing lowering differences recorded in formal/TRANSLATION.md. No unknown
primitive is exempt. Lean builds check signatures, evaluators and safety proofs.
"""
import pathlib
import re
import sys

root = pathlib.Path(sys.argv[1])
c_path = root / "src/core/ops.def"
lean_path = root / "formal/MoCore/Text/Prim.lean"
c = c_path.read_text()
lean = lean_path.read_text()
operations = {}
errors = []

for family, body in re.findall(
    r"#define MO_CORE_(\w+)_OPS\(X\)(.*?)(?=\n#define|\n//|\Z)", c, re.S
):
    pairs = re.findall(r'X\((\w+), "(\w+)"\)', body)
    lean_family = family.lower()
    table = re.search(rf"def {lean_family}Ops\b.*?(?=\ndef |\Z)", lean, re.S)
    spellings = set(re.findall(r'\("(\w+)",', table[0])) if table else set()
    if family == "LIST":
        table = re.search(r"def listOut\b.*?(?=\ndef listIn)", lean, re.S)
        spellings = set(re.findall(r' => tag "(\w+)"(?: \||\n|$)', table[0])) if table else set()
    c_spellings = {wire for _, wire in pairs}
    if c_spellings != spellings:
        errors.append(f"{c_path}: {family} differs from {lean_path}: "
                      f"C only {sorted(c_spellings - spellings)}, "
                      f"Lean only {sorted(spellings - c_spellings)}")
    operations.update({(family, tag): wire for tag, wire in pairs if wire in spellings})

# Parameterized constructors are not in the nullary tables. Check their actual
# codec clauses too, rather than assuming that a known name still exists.
schema = (root / "src/core/text_schema.def").read_text()
for family, tag, wire in [("LIST", "LIT", "lit"), ("LIST", "ZIP", "zip"), ("MAP", "LIT", "lit")]:
    if f"MO_CORE_{family}_OP_{tag}" in schema and re.search(rf'\| \.{wire}\b', lean):
        operations[family, tag] = wire

families = {"BIGINT": "BIG", "DECIMAL": "DEC", "DURATION": "TIME", "UNICODE16": "UNICODE"}
aliases = {
    "TEXT_ASCII_LOWERCASE": ("TEXT", "LOWER"),
    "TEXT_ASCII_UPPERCASE": ("TEXT", "UPPER"),
    "CHAR_IS_ASCII_DIGIT": ("CHAR", "IS_DIGIT"),
    "CHAR_IS_ASCII_LETTER": ("CHAR", "IS_LETTER"),
    "CHAR_IS_ASCII_WHITE_SPACE": ("CHAR", "IS_WHITE_SPACE"),
    "CHAR_ASCII_LOWERCASE": ("CHAR", "LOWER"),
    "CHAR_ASCII_UPPERCASE": ("CHAR", "UPPER"),
    "LIST_EMPTY": ("LIST", "LIT"),
    "LIST_TO_SET": ("SET", "OF_LIST"),
    "LIST_TO_MAP": ("MAP", "OF_LIST"),
    "LIST_JOIN": ("TEXT", "JOIN"),
    "MAP_LITERAL_INSERT": ("MAP", "LIT"),
    "DURATION_TO_TEXT": ("TIME", "DUR_TO_TEXT"),
}
# These helpers have no direct primitive route in the current Lean model.
# They must remain explicit: adding a new unmodeled id fails, not silently skips.
unmodeled = {"TEXT_QUOTED", "CHAR_QUOTED", "TEXT_STRUCTURAL"}
recipes = {
    "LIST_SUM": [(f, "ADD") for f in ["INT", "BIG", "DEC", "FLOAT"]],
    "BIGINT_COMPARE": [("CMP", "LT"), ("CMP", "GT")],
    "TEXT_COMPARE": [("CMP", "LT"), ("CMP", "GT")],
    "BYTES_COMPARE": [("CMP", "LT"), ("CMP", "GT")],
}

count = 0
path = root / "src/base/prims.def"
for line_number, line in enumerate(path.read_text().splitlines(), 1):
    match = re.match(r"PRIM\((\w+),", line)
    if not match:
        continue
    identifier = match[1]
    if identifier in unmodeled:
        continue
    prefix, suffix = identifier.split("_", 1)
    route = recipes.get(identifier, [aliases.get(identifier, (families.get(prefix, prefix), suffix))])
    for op in route:
        if op not in operations:
            errors.append(f"{path}:{line_number}: {identifier} has no Core operation {op[0]}.{op[1]} in both codecs")
    count += 1

path = root / "src/bytecode/opcodes.def"
for line_number, line in enumerate(path.read_text().splitlines(), 1):
    match = re.match(r"OPCODE\((ADD|SUB|MUL|DIV|REM|NEG)_(INT|DECIMAL|FLOAT),", line)
    if match and (families.get(match[2], match[2]), match[1]) not in operations:
        errors.append(f"{path}:{line_number}: {match[1]}_{match[2]} has no Core operation in both codecs")

if errors:
    print("\n".join(errors), file=sys.stderr)
    sys.exit(1)
print(f"Core library map: {count} runtime primitives have routes in both codecs; arithmetic instructions covered; "
      "3 explicit text-form helpers remain unmodeled")
