import collections
import json
import pathlib
import sys

ROWS = ["boundary", "negate", "logic", "not", "arithmetic", "bool", "int"]


def records(path):
    return [json.loads(line) for line in path.read_text().splitlines() if line.startswith("{")]


def program(directory, name):
    listed = records(directory / f"{name}.list.json")
    gated = records(directory / f"{name}.gates.json")
    functions = [r for r in listed if r["record"] == "function"]
    made = collections.Counter(r["kind"] for r in listed if r["record"] == "mutant")
    tried = [r for r in gated if r["record"] == "mutant"]
    totals = gated[-1]
    return {
        "functions": len(functions),
        "without": sum(r["mutants"] == 0 for r in functions),
        "made": made,
        "killed": collections.Counter(r["kind"] for r in tried if r["outcome"] == "killed"),
        "survived": collections.Counter(r["kind"] for r in tried if r["outcome"] == "survived"),
        "dropped": collections.Counter(r["kind"] for r in tried if r["outcome"] == "dropped"),
        "by": collections.Counter(r["by"] for r in tried if r["outcome"] == "killed"),
        "gates": totals.get("gates"),
        "status": totals["status"],
        "codes": collections.Counter(r["code"] for r in gated if r["record"] == "diagnostic"),
    }


def main():
    directory = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else
                             "learnability/trust-gates/results/dry-run")
    seconds = {}
    for line in (directory / "times.tsv").read_text().splitlines()[1:]:
        cells = line.split("\t")
        if len(cells) == 4:
            seconds[cells[0]] = float(cells[3])
    total = collections.defaultdict(collections.Counter)
    print("program\tfunctions\twithout\tmade\ttried\tkilled\tsurvived\tdropped\tgates\tstatus\t"
          "codes\tseconds")
    for path, time in seconds.items():
        name = ("task-queue" if path.startswith("services/") else
                path.removesuffix(".mo").replace("/", "-"))
        p = program(directory, name)
        tried = sum(p["killed"].values()) + sum(p["survived"].values()) + sum(p["dropped"].values())
        print(f"{path}\t{p['functions']}\t{p['without']}\t{sum(p['made'].values())}\t{tried}\t"
              f"{sum(p['killed'].values())}\t{sum(p['survived'].values())}\t"
              f"{sum(p['dropped'].values())}\t{p['gates']}\t{p['status']}\t"
              f"{dict(p['codes'])}\t{time:.1f}")
        for key in ("made", "killed", "survived", "dropped", "by"):
            total[key].update(p[key])
        total["functions"]["all"] += p["functions"]
        total["functions"]["without"] += p["without"]
    print()
    print("row\tmade\tkilled\tsurvived\tdropped\tsurvived_share")
    for row in ROWS:
        ran = total["killed"][row] + total["survived"][row]
        share = f"{total['survived'][row] / ran:.2f}" if ran else "-"
        print(f"{row}\t{total['made'][row]}\t{total['killed'][row]}\t{total['survived'][row]}\t"
              f"{total['dropped'][row]}\t{share}")
    print()
    print("kills by kind of check:", dict(total["by"]))
    print("functions:", total["functions"]["all"], "without a mutant:",
          total["functions"]["without"])


if __name__ == "__main__":
    main()
