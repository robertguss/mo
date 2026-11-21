#!/bin/sh
# Installs the Mo language extension into Cursor and/or VS Code user extension folders.
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ext_name="mo-lang.mo-language-0.3.0"

install_into() {
  target_root=$1
  mkdir -p "$target_root"
  dest="$target_root/$ext_name"
  rm -rf "$dest"
  mkdir -p "$dest/syntaxes" "$dest/snippets"
  cp "$script_dir/package.json" "$dest/"
  cp "$script_dir/language-configuration.json" "$dest/"
  cp "$script_dir/extension.js" "$dest/"
  cp "$script_dir/syntaxes/mo.tmLanguage.json" "$dest/syntaxes/"
  cp "$script_dir/syntaxes/mo.markdown.json" "$dest/syntaxes/"
  cp "$script_dir/snippets/mo.code-snippets" "$dest/snippets/"
  printf 'installed %s\n' "$dest"
}

if [ $# -gt 0 ]; then
  while [ $# -gt 0 ]; do
    case "$1" in
      --cursor)
        install_into "${HOME}/.cursor/extensions"
        shift
        ;;
      --vscode)
        install_into "${HOME}/.vscode/extensions"
        shift
        ;;
      --target)
        [ $# -ge 2 ] || { echo "usage: install.sh [--cursor] [--vscode] [--target DIR]" >&2; exit 2; }
        install_into "$2"
        shift 2
        ;;
      *)
        echo "usage: install.sh [--cursor] [--vscode] [--target DIR]" >&2
        exit 2
        ;;
    esac
  done
  exit 0
fi

installed_any=0
if [ -d "${HOME}/.cursor" ]; then
  install_into "${HOME}/.cursor/extensions"
  installed_any=1
fi
if [ -d "${HOME}/.vscode" ]; then
  install_into "${HOME}/.vscode/extensions"
  installed_any=1
fi
if [ "$installed_any" -eq 0 ]; then
  install_into "${HOME}/.cursor/extensions"
fi
