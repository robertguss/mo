"""Tabulates which item digests each variant changes against the unchanged task queue."""
import json
import os
import sys

out = sys.argv[1]
modes = ["raw", "formatted", "canonical"]
flavours = ["with_signature", "without_signature"]


def load(name):
    with open(os.path.join(out, name + ".json")) as f:
        return {item["name"]: item for item in json.load(f)["items"]}


def checks(name):
    with open(os.path.join(out, name + ".test.txt")) as f:
        return f.read().strip().splitlines()[-1]


def short(name):
    return name.removeprefix("JobQueue.")


base = load("base")
variants = sorted(f[:-5] for f in os.listdir(out) if f.endswith(".json") and f != "base.json")
changed = {}
for v in variants:
    items = load(v)
    assert items.keys() == base.keys(), v
    changed[v] = {
        (m, fl): [n for n in base if items[n]["digests"][m][fl] != base[n]["digests"][m][fl]]
        for m in modes for fl in flavours
    }
    changed[v]["reach"] = [n for n in base if items[n]["reach"] != base[n]["reach"]]

print(f"Items: {len(base)}. Digest rule: canonical text (formatter output with comments and "
      "blank-line bits removed).\n")
print("| Variant | `mo test queue-tests.mo` | Changed, signature included | "
      "Changed, signature left out |")
print("| --- | --- | --- | --- |")
for v in variants:
    cells = []
    for fl in flavours:
        names = changed[v][("canonical", fl)]
        cells.append(f"{len(names)}: " + ", ".join(short(n) for n in names) if names else "0")
    print(f"| {v} | {checks(v)} | {cells[0]} | {cells[1]} |")

print("\nChanged item count by text rule (with signature / without signature):\n")
print("| Variant | raw bytes | formatted, comments kept | canonical | reach lists changed |")
print("| --- | --- | --- | --- | --- |")
for v in variants:
    cells = [f"{len(changed[v][(m, flavours[0])])} / {len(changed[v][(m, flavours[1])])}"
             for m in modes]
    print(f"| {v} | " + " | ".join(cells) + f" | {len(changed[v]['reach'])} |")
