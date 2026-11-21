#!/usr/bin/env python3
"""Validate VS Code/Cursor and Zed editor extensions, grammars, queries and C compilation."""
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import tomllib

ROOT = pathlib.Path(__file__).resolve().parent.parent


def check_vscode() -> None:
    vscode_dir = ROOT / "editors" / "vscode"
    pkg = json.loads((vscode_dir / "package.json").read_text(encoding="utf-8"))
    assert pkg["name"] == "mo-language"
    assert pkg["main"] == "./extension.js"
    assert (vscode_dir / "extension.js").is_file()
    assert (vscode_dir / "install.sh").is_file()

    lang_cfg = json.loads((vscode_dir / "language-configuration.json").read_text(encoding="utf-8"))
    assert lang_cfg["comments"]["lineComment"] == "#"
    re.compile(lang_cfg["wordPattern"])
    re.compile(lang_cfg["indentationRules"]["increaseIndentPattern"])
    re.compile(lang_cfg["indentationRules"]["decreaseIndentPattern"])
    re.compile(lang_cfg["folding"]["markers"]["start"])
    re.compile(lang_cfg["folding"]["markers"]["end"])

    tm = json.loads((vscode_dir / "syntaxes" / "mo.tmLanguage.json").read_text(encoding="utf-8"))
    assert tm["scopeName"] == "source.mo"
    md_tm = json.loads((vscode_dir / "syntaxes" / "mo.markdown.json").read_text(encoding="utf-8"))
    assert md_tm["scopeName"] == "markdown.mo.codeblock"
    snippets = json.loads((vscode_dir / "snippets" / "mo.code-snippets").read_text(encoding="utf-8"))
    assert len(snippets) >= 8

    def check_patterns(obj):
        if isinstance(obj, dict):
            for key in ("match", "begin", "end", "while"):
                if key in obj:
                    # Strip Oniguruma \G anchor; skip end patterns referencing begin captures (\1..\9)
                    pat = obj[key].replace("\\G", "")
                    if key == "end" and re.search(r"\\[1-9]", pat):
                        continue
                    re.compile(pat)
            for val in obj.values():
                check_patterns(val)
        elif isinstance(obj, list):
            for item in obj:
                check_patterns(item)

    check_patterns(tm)
    check_patterns(md_tm)

    # Verify installer script copies cleanly into a target directory
    with tempfile.TemporaryDirectory(prefix="mo-vscode-ext-") as tmp:
        res = subprocess.run(
            ["sh", str(vscode_dir / "install.sh"), "--target", tmp],
            capture_output=True, text=True, check=True, timeout=15,
        )
        installed_dir = pathlib.Path(tmp) / f"mo-lang.mo-language-{pkg['version']}"
        assert installed_dir.is_dir(), res.stdout
        assert (installed_dir / "syntaxes" / "mo.tmLanguage.json").is_file()
        assert (installed_dir / "extension.js").is_file()


def check_zed_and_treesitter() -> None:
    zed_dir = ROOT / "editors" / "zed"
    ts_dir = ROOT / "editors" / "tree-sitter-mo"

    ext = tomllib.loads((zed_dir / "extension.toml").read_text(encoding="utf-8"))
    assert ext["id"] == "mo"
    assert ext["grammars"]["mo"]["path"] == "editors/tree-sitter-mo"

    cfg = tomllib.loads((zed_dir / "languages" / "mo" / "config.toml").read_text(encoding="utf-8"))
    assert cfg["name"] == "Mo"
    assert cfg["grammar"] == "mo"
    assert cfg["path_suffixes"] == ["mo"]
    assert cfg["tab_size"] == 2

    node_types = json.loads((ts_dir / "src" / "node-types.json").read_text(encoding="utf-8"))
    named_nodes = {entry["type"] for entry in node_types if entry.get("named")}
    named_nodes.add("ERROR")

    for scm_name in ("highlights.scm", "brackets.scm", "indents.scm", "outline.scm"):
        scm_path = zed_dir / "languages" / "mo" / scm_name
        text = scm_path.read_text(encoding="utf-8")
        # Strip comments and strings to check S-expression paren/bracket balance and node names
        cleaned_lines = []
        for line in text.splitlines():
            in_str = False
            esc = False
            out = []
            for ch in line:
                if esc:
                    esc = False
                    continue
                if ch == "\\" and in_str:
                    esc = True
                    continue
                if ch == '"':
                    in_str = not in_str
                    continue
                if ch == ";" and not in_str:
                    break
                if not in_str:
                    out.append(ch)
            cleaned_lines.append("".join(out))
        cleaned = "\n".join(cleaned_lines)
        depth_paren = 0
        depth_brack = 0
        for ch in cleaned:
            if ch == "(":
                depth_paren += 1
            elif ch == ")":
                depth_paren -= 1
                assert depth_paren >= 0, f"unmatched ) in {scm_name}"
            elif ch == "[":
                depth_brack += 1
            elif ch == "]":
                depth_brack -= 1
                assert depth_brack >= 0, f"unmatched ] in {scm_name}"
        assert depth_paren == 0 and depth_brack == 0, f"unbalanced delimiters in {scm_name}"

        for match in re.finditer(r"\(([a-z_][a-z0-9_]*)\b", cleaned):
            node = match.group(1)
            if node.startswith("#"):
                continue
            assert node in named_nodes, f"{scm_name} references unknown Tree-sitter node ({node})"

    # Compile generated parser.c and scanner.c with a C compiler to ensure WASI/C portability
    cc = os.environ.get("CC") or shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    assert cc is not None, "no C compiler found"
    with tempfile.TemporaryDirectory(prefix="mo-ts-cc-") as tmp:
        parser_o = pathlib.Path(tmp) / "parser.o"
        scanner_o = pathlib.Path(tmp) / "scanner.o"
        subprocess.run(
            [cc, "-std=c11", "-O2", "-I", str(ts_dir / "src"),
             "-c", str(ts_dir / "src" / "parser.c"), "-o", str(parser_o)],
            check=True, capture_output=True, timeout=60,
        )
        subprocess.run(
            [cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-I", str(ts_dir / "src"),
             "-c", str(ts_dir / "src" / "scanner.c"), "-o", str(scanner_o)],
            check=True, capture_output=True, timeout=30,
        )


def main() -> int:
    check_vscode()
    check_zed_and_treesitter()
    print("editor grammars, queries, installer and Tree-sitter C sources passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
