# Editor Support for Mo (Zed & Cursor / VS Code)

First install the `mo` binary onto `$PATH` (`~/.local/bin/mo`) so editor formatting can invoke
`mo format --stdin`:

```sh
make install
```

---

## Cursor & VS Code (`editors/vscode/`)

Provides TextMate syntax highlighting for `.mo` files and ```` ```mo ```` Markdown blocks, comment
toggling (`Cmd+/` / `Ctrl+/`), block auto-indentation, snippets, and document formatting via
`mo format --stdin`.

### Install

```sh
sh editors/vscode/install.sh
```

By default, `install.sh` installs into `~/.cursor/extensions/` and/or `~/.vscode/extensions/`
(pass `--cursor`, `--vscode`, or `--target DIR` to pick explicitly), then reload the window
(`Developer: Reload Window`).

To format on save in Cursor / VS Code, add to your user `settings.json`:

```json
{
  "[mo]": {
    "editor.formatOnSave": true,
    "editor.defaultFormatter": "mo-lang.mo-language"
  }
}
```

---

## Zed (`editors/zed/` & `editors/tree-sitter-mo/`)

Provides a full Tree-sitter grammar derived from [`grammar/mo.y`](../grammar/mo.y) with syntax
highlighting (`highlights.scm`), symbol outline (`outline.scm`), bracket matching (`brackets.scm`),
and structural indentation (`indents.scm`).

### Install

1. Open Zed and run **`zed: extensions`** from the Command Palette (`Cmd+Shift+P` / `Ctrl+Shift+P`).
2. Click **Install Dev Extension** and select the **`editors/zed`** directory in this repository.
3. Add `mo format --stdin` to your Zed `settings.json` (`zed: open settings`):

```json
{
  "languages": {
    "Mo": {
      "tab_size": 2,
      "format_on_save": "on",
      "formatter": {
        "external": {
          "command": "mo",
          "arguments": ["format", "--stdin"]
        }
      }
    }
  }
}
```
