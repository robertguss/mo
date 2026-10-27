#!/bin/sh
# Checks docs/design/library.md against the two tables it maps, src/base/prims.def and
# src/base/hostops.def: every row that says a primitive or a host operation implements a
# function names a line of the right table, and every line of both tables has such a row.
# Prints every disagreement, with its file and line, and fails if there is one.
# ci/test_library_map.sh tests this check.
#
#   sh ci/library_map.sh [root]    the repository's root folder: . by default
#
# The map's tables have four columns: Function, Where, Implemented by and Why. Each is in a
# section whose heading starts with a type, and the type's module is the section's: "## List(T)"
# is about List. Where is one of the six places below. A row whose Where is primitive or host
# names exactly one id in backticks in its third column, which must be a line of prims.def or
# hostops.def in the section's module; when the row's Function starts with a function's name
# in backticks, such as `pow(n)` or `Text.from_chars(chars)`, the line has that name. Other
# rows may name ids too, which must be lines of either table. No line has two rows, and no
# function has two rows in a section. A Mo row names its file in lib/, in backticks, named after
# the section's module as reference 2.1 names a module's file: FileError's is lib/file-error.mo.
# Other tables in the map, such as the one that lists the six places, aren't read.
set -eu

if [ $# -gt 1 ]; then
	echo "usage: sh ci/library_map.sh [root]" >&2
	exit 2
fi
root=${1:-.}
map=$root/docs/design/library.md
prims=$root/src/base/prims.def
hostops=$root/src/base/hostops.def
for file in "$map" "$prims" "$hostops"; do
	if [ ! -f "$file" ]; then
		echo "ci/library_map.sh: $file is not a file" >&2
		exit 2
	fi
done

if [ -z "${MO_MODULE_PATH:-}" ]; then
	repo=$(cd "$(dirname "$0")/.." && pwd)
	make -s -C "$repo" build/cc/debug/module-path CC=cc
	MO_MODULE_PATH=$repo/build/cc/debug/module-path
	export MO_MODULE_PATH
fi

awk -v prims="$prims" -v hostops="$hostops" -v map="$map" '
function trim(text) {
	sub(/^[ \t]+/, "", text)
	sub(/[ \t]+$/, "", text)
	return text
}

function unquote(text) {
	text = trim(text)
	sub(/^"/, "", text)
	sub(/"$/, "", text)
	return text
}

function problem(where, message) {
	print where ": " message > "/dev/stderr"
	problems++
}

# Splits the text between backticks into tokens[1..n] and returns n.
function backticked(text, tokens,    n, start, rest) {
	n = 0
	rest = text
	while ((start = index(rest, "`")) > 0) {
		rest = substr(rest, start + 1)
		if (index(rest, "`") == 0)
			break
		tokens[++n] = substr(rest, 1, index(rest, "`") - 1)
		rest = substr(rest, index(rest, "`") + 1)
	}
	return n
}

# The compiler owns module spelling; the map must not implement a second rule.
function module_file(name,    command, file, status) {
	if (name in module_paths)
		return module_paths[name]
	if (name !~ /^[A-Z][A-Za-z0-9]*([.][A-Z][A-Za-z0-9]*)*$/) {
		problem(map ":" FNR, "invalid module name " name)
		return ""
	}
	command = "\"$MO_MODULE_PATH\" \"" name "\""
	status = (command | getline file)
	if (close(command) != 0 || status != 1) {
		problem(map ":" FNR, "module path conversion failed for " name)
		return ""
	}
	module_paths[name] = "lib/" file
	return module_paths[name]
}

# The name of the function a row is about, from the first text in backticks in its Function
# column, as pow for `pow(n)` and Text.from_chars for `Text.from_chars(chars)`; "" for an
# operator, such as `a + b`.
function function_name(cell,    tokens, n, called) {
	n = backticked(cell, tokens)
	if (n == 0)
		return ""
	called = tokens[1]
	if (called !~ /^([A-Z][A-Za-z0-9]*[.])?%?[a-z][a-z0-9_]*[?]?([(].*[)])?$/)
		return ""
	sub(/[(].*$/, "", called)
	return called
}

# An id names a line of a table: capitals, digits and underscores, with at least one
# underscore, as LIST_APPEND.
function is_id(token) {
	return token ~ /^[A-Z][A-Z0-9]*_[A-Z0-9_]*$/
}

# A line of a table: PRIM(ID, "Module", "name", ...) or HOSTOP(ID, "Module", "name", ...). Ids,
# modules and names hold no commas.
FILENAME == prims || FILENAME == hostops {
	if ($0 !~ /^(PRIM|HOSTOP|SIMOP)\(/)
		next
	simulation = ($0 ~ /^SIMOP\(/)
	table = simulation ? "instruction" : (FILENAME == prims) ? "primitive" : "host"
	line = $0
	sub(/^[A-Z]+\(/, "", line)
	if (split(line, fields, ",") < 3) {
		problem(FILENAME ":" FNR, "a line needs an id, a module and a name")
		next
	}
	id = (simulation ? "SIM_" : "") trim(fields[1])
	if (id in kind) {
		problem(FILENAME ":" FNR, id " is a line of both tables, or twice of one")
		next
	}
	kind[id] = table
	module[id] = unquote(fields[2])
	name[id] = unquote(fields[3])
	place[id] = FILENAME ":" FNR
	order[++ids] = id
	next
}

# The map.
FILENAME == map && /^```/ {
	fenced = !fenced
	next
}
FILENAME == map && fenced {
	next
}
FILENAME == map && /^## / {
	section = $0
	sub(/^## +/, "", section)
	section_module = section
	sub(/[ (].*$/, "", section_module)
	next
}
FILENAME == map && !/^\|/ {
	in_table = 0
	next
}
FILENAME == map {
	if (!in_table) {
		# The first row of a table is its header.
		in_table = 1
		header = $0
		gsub(/[ \t]+/, " ", header)
		checked = (header == "| Function | Where | Implemented by | Why |")
		next
	}
	if (!checked || $0 ~ /^\|[- |]*$/)
		next
	where = map ":" FNR
	if (split($0, cells, "|") != 6) {
		problem(where, "a row needs four columns: Function, Where, Implemented by and Why")
		next
	}
	rows++
	function_cell = trim(cells[2])
	place_cell = trim(cells[3])
	implemented = trim(cells[4])
	if (place_cell !~ /^(instruction|primitive|host|generic|lowering|Mo)$/) {
		problem(where, "Where is \"" place_cell "\", which isn'"'"'t instruction, primitive, host, generic, lowering or Mo")
		next
	}
	called = function_name(function_cell)
	if (called != "") {
		if ((section "\t" called) in named)
			problem(where, called " has a row already in section \"" section "\", at " named[section "\t" called])
		else
			named[section "\t" called] = where
	}
	n = backticked(implemented, tokens)
	if (place_cell == "Mo") {
		files = 0
		for (i = 1; i <= n; i++) {
			if (tokens[i] !~ /^lib\//)
				continue
			files++
			if (tokens[i] != module_file(section_module))
				problem(where, "a Mo row of section \"" section "\" names " tokens[i] ", not " module_file(section_module))
		}
		if (files != 1)
			problem(where, "a Mo row names its file in lib/ in backticks, as " module_file(section_module))
	}
	found = 0
	for (i = 1; i <= n; i++) {
		if (!is_id(tokens[i]))
			continue
		found++
		id = tokens[i]
		if (!(id in kind))
			problem(where, id " is no line of " prims " or " hostops)
		else if (place_cell == "primitive" || place_cell == "host" || kind[id] == "instruction") {
			if (kind[id] != place_cell)
				problem(where, "a " place_cell " row names " id ", which is a line of " (kind[id] == "primitive" ? prims : hostops))
			else
				check_row(id, where, function_cell)
		}
	}
	if ((place_cell == "primitive" || place_cell == "host") && found != 1)
		problem(where, "a " place_cell " row names " found " ids in backticks, not exactly one")
}

# A row that names a line: the line is in the section'"'"'s module, has the name the row gives,
# and has no other row.
function check_row(id, where, function_cell,    called, called_module) {
	if (id in row)
		problem(where, id " has a row already, at " row[id])
	else
		row[id] = where
	if (module[id] != section_module)
		problem(where, id " is in module " module[id] ", not " section_module ", the module of section \"" section "\"")
	called = function_name(function_cell)
	if (called == "")
		return # an operator, such as `a + b`
	called_module = ""
	if (index(called, ".") > 0) {
		called_module = substr(called, 1, index(called, ".") - 1)
		called = substr(called, index(called, ".") + 1)
	}
	if (called != name[id])
		problem(where, "the row is about " called ", but " id " is named " name[id])
	if (called_module != "" && called_module != module[id])
		problem(where, "the row is about " called_module "." called ", but " id " is in module " module[id])
}

END {
	for (i = 1; i <= ids; i++) {
		id = order[i]
		if (!(id in row))
			problem(place[id], id " has no " kind[id] " row in " map)
		else if (kind[id] == "primitive")
			primitives++
		else
			operations++
	}
	if (problems > 0) {
		print "ci/library_map.sh: " problems " error(s); the rules are at the top of ci/library_map.sh" > "/dev/stderr"
		exit 1
	}
	print "ci/library_map.sh: " map " has a row for each of the " primitives + 0 " primitives and " operations + 0 " host operations (" rows + 0 " rows)"
}
' "$prims" "$hostops" "$map"

# Check the Core route as well; checking only runtime ids cannot find a missing
# Lean/C Core operation. This also checks immediate arithmetic such as REM_FLOAT.
python3 "$(dirname "$0")/core_library_map.py" "$root"
