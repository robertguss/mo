#!/usr/bin/env python3
"""Check the reference-rule inventory and its links to executable conformance fixtures."""

import argparse
import csv
import pathlib
import re

ROOT = pathlib.Path(__file__).resolve().parent.parent
FIELDS = ["rule", "accepts", "refuses", "stops", "gap", "drivers", "cost", "untestable"]
EVIDENCE = ("accepts", "gap", "drivers", "cost", "untestable")


def has_test(path, name):
    """Whether the driver at path defines, and for Python also calls, the test called name."""
    text = path.read_text()
    if path.name == "Makefile":
        return re.search(rf"^{name}:", text, re.M) is not None
    if path.suffix in (".c", ".h"):
        return re.search(rf"^[A-Za-z_][\w *]*\b{name}\(", text, re.M) is not None
    if path.suffix == ".mo":
        # A library function in lib/, whose examples the drivers group runs with mo test.
        return path.parent.name == "lib" and re.search(rf"^fn {name}\(", text, re.M) is not None
    if path.suffix == ".py":
        defined = re.search(rf"^\s*def {name}\(", text, re.M) is not None
        return defined and len(re.findall(rf"\b{name}\(", text)) > 1
    return False


MEASURED = "measured:"


def cost_entries(entries):
    """Split a cost column into (semantics-only entries, entries whose test measures the cost)."""
    entries = list(filter(None, entries.split(",")))
    measured = [entry.removeprefix(MEASURED) for entry in entries if entry.startswith(MEASURED)]
    return [entry for entry in entries if not entry.startswith(MEASURED)], measured


def check_tests(root, rule, field, entries):
    for entry in filter(None, entries.split(",")):
        path, _, name = entry.partition(":")
        assert name or field == "cost", f"{rule}: driver evidence must name a test: {entry}"
        assert re.fullmatch(r"[\w./-]+", path) and re.fullmatch(r"\w*", name), (rule, field, entry)
        assert field != "cost" or re.fullmatch(r"tests/test_\w+\.c", path), \
            f"{rule}: cost evidence must be a C unit test: {entry}"
        assert (root / path).is_file(), f"{rule}: missing driver {path}"
        assert not name or has_test(root / path, name), f"{rule}: no test {name} in {path}"


def inventory(source):
    section = None
    counts = {}
    block = []
    fenced = False
    text_fence = False
    rules = {}

    def flush():
        if block and section:
            counts[section] = counts.get(section, 0) + 1
            rules[f"{section}/{counts[section]}"] = " ".join(block)
        block.clear()

    for line in source.splitlines():
        if line.startswith("```"):
            flush()
            text_fence = not fenced and line == "```text"
            fenced = not fenced
            continue
        if fenced:
            if text_fence:
                block.append(line.strip())
            continue
        heading = re.match(r"^#{2,3} (\d+(?:\.\d+)?)[.]? |^## Appendix ([ABC])[.]", line)
        if heading:
            flush()
            section = heading[1] or heading[2]
        elif not line.strip() or line.startswith("#"):
            flush()
        elif line.startswith("| "):
            flush()
            if not re.fullmatch(r"[| :\-]+", line) and not line.startswith(
                    ("| Type |", "| Pattern |", "| Ability |", "| Capability |",
                     "| Operation |", "| Level |", "| Elsewhere |")):
                block.append(line.strip())
                flush()
        else:
            if line.startswith("- "):
                flush()
            block.append(line.strip())
    flush()
    return rules


def validate(root, rows, rules):
    indexed = {}
    for row in rows:
        rule = row["rule"]
        assert rule not in indexed, f"duplicate rule: {rule}"
        indexed[rule] = row
        assert rule in rules, f"unknown rule: {rule}"
        assert None not in row, f"{rule}: more columns than {FIELDS}"
        assert any(row[field].strip() for field in FIELDS[1:]), \
            f"{rule}: no program or gap explanation"
        assert any(row[field].strip() for field in EVIDENCE), \
            f"{rule}: no accepting program, driver, untestable reason or gap explanation"
        check_tests(root, rule, "drivers", row["drivers"])
        semantics, measured = cost_entries(row["cost"])
        check_tests(root, rule, "cost", ",".join(semantics + measured))
        for entry in measured:
            assert ":" in entry, f"{rule}: measured cost evidence must name a test: {entry}"
        for field in ("accepts", "refuses", "stops"):
            for name in filter(None, row[field].split(",")):
                assert re.fullmatch(r"[a-z0-9]+(-[a-z0-9]+)*", name), (rule, field, name)
                corpus = "errors" if field == "refuses" and re.match(r"e\d{4}", name) else "golden"
                case = root / "tests" / corpus / name
                assert case.is_dir(), f"{rule}: missing {corpus} program {name}"
                if corpus == "errors":
                    assert (case / "main.mo").is_file(), f"{rule}: missing source for {name}"
                    assert (case / "expected").is_file(), f"{rule}: missing exact error for {name}"
                    assert not (case / "library-dependent").exists(), \
                        f"{rule}: {name} does not check an exact error"
                else:
                    source = case / "main.mo"
                    entry = case / "entry"
                    assert source.is_file() != entry.is_file(), f"{rule}: missing source for {name}"
                    if entry.is_file():
                        assert (root / entry.read_text().strip()).is_file(), \
                            f"{rule}: missing entry for {name}"
                    assert (case / "stdout").is_file(), f"{rule}: missing exact output for {name}"
                    status_file = case / "status"
                    status = int(status_file.read_text()) if status_file.exists() else 0
                    wanted = status not in (0, 70) if field == "refuses" else \
                        status == (70 if field == "stops" else 0)
                    assert wanted, \
                        f"{rule}: {name} has the wrong status for {field}"
    missing = rules.keys() - indexed.keys()
    assert not missing, f"reference rules with no entry: {', '.join(sorted(missing))}"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--inventory", action="store_true", help="print reference rule text")
    parser.add_argument("--section", help="show a section and its subsections, e.g. 5 or 5.6")
    args = parser.parse_args()
    rules = inventory((ROOT / "docs/reference/README.md").read_text())
    if args.inventory:
        for rule, source in rules.items():
            print(f"{rule}\t{source}")
        return
    with (ROOT / "tests/conformance/reference.tsv").open(newline="") as stream:
        snapshot = list(csv.reader(stream, delimiter="\t", quoting=csv.QUOTE_NONE))
    assert snapshot == [list(item) for item in rules.items()], \
        "reference changed; review rules.tsv, then refresh reference.tsv with --inventory"
    with (ROOT / "tests/conformance/rules.tsv").open(newline="") as stream:
        # Trailing empty columns may be left off, so a row without other-driver
        # evidence keeps the original five columns.
        reader = csv.DictReader(stream, delimiter="\t", restval="")
        assert reader.fieldnames == FIELDS, f"expected columns: {FIELDS}"
        rows = list(reader)
    validate(ROOT, rows, rules)
    if args.section:
        selected = [row for row in rows if row["rule"].split("/")[0] == args.section or
                    row["rule"].startswith(args.section + ".")]
        assert selected, f"unknown section: {args.section}"
        for row in selected:
            print(f"\n{row['rule']} {rules[row['rule']]}")
            for field in FIELDS[1:]:
                if field == "cost":
                    semantics, measured = cost_entries(row[field])
                    if semantics:
                        print(f"  cost (semantics checked, cost not measured): "
                              f"{','.join(semantics)}")
                    if measured:
                        print(f"  cost measured: {','.join(measured)}")
                elif row[field]:
                    print(f"  {field}: {row[field]}")
            diagnostics = [f"tests/check/diagnostics/{name}.diag"
                           for name in row["refuses"].split(",")
                           if name and (ROOT / f"tests/check/diagnostics/{name}.diag").is_file()]
            if diagnostics:
                print(f"  diagnostic fields: {', '.join(diagnostics)}")
    count = {field: sum(bool(row[field]) for row in rows) for field in FIELDS[4:]}
    measured = sum(bool(cost_entries(row["cost"])[1]) for row in rows)
    print(f"{len(rows)} reference rules indexed; {count['gap']} have explicit gaps; "
          f"{count['drivers']} name other drivers; {count['cost'] - measured} have cost claims "
          f"checked for semantics only; {measured} have cost measured; "
          f"{count['untestable']} untestable")


if __name__ == "__main__":
    main()
