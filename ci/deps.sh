#!/bin/sh
# Checks the module boundaries of docs/design/architecture.md, section 2: which headers each
# module in src/ may include. Prints every include that breaks them, with its file and line,
# and fails if there is one. ci/test_deps.sh tests this check.
#
#   sh ci/deps.sh [directory]    the directory that holds the modules: src by default
#
# It reads the #include lines of every C file in the directory: .c, .h and .def. Mo's headers
# are included by their path in src/, which starts with their module, such as "base/base.h". A
# module may include any of its own files, and the public headers of the modules it uses.
set -eu

# The rules, as section 2 gives them. Every module uses base as well. A module named under
# "uses" may be included through any of its public headers; a header named there, such as
# heap/types.h, is the only one of its module that may be. In public headers, * stands for any
# name.
rules='
# module | public headers           | uses
base     | base/*.h base/*.def      |
syntax   | syntax/syntax.h          |
check    | check/check.h            | syntax
lower    | lower/lower.h            | check core
core     | core/core.h              |
codegen  | codegen/codegen.h        | core bytecode heap/types.h
bytecode | bytecode/bytecode.h      | heap/types.h
heap     | heap/heap.h heap/types.h |
prim     | prim/prim.h              | heap
host     | host/host.h              | heap
vm       | vm/vm.h                  | bytecode heap prim host
tools    | tools/tools.h            | syntax check lower core codegen bytecode heap prim host vm
'

if [ $# -gt 1 ]; then
	echo "usage: sh ci/deps.sh [directory]" >&2
	exit 2
fi
root=${1:-src}
if [ ! -d "$root" ]; then
	echo "ci/deps.sh: $root is not a directory" >&2
	exit 2
fi

find "$root" -type f \( -name '*.c' -o -name '*.h' -o -name '*.def' \) | LC_ALL=C sort |
	MO_DEPS_RULES=$rules MO_DEPS_ROOT=$root awk '
function trim(text) {
	sub(/^[ \t]+/, "", text)
	sub(/[ \t]+$/, "", text)
	return text
}

# The first directory of a path in src/, which names a module; "" if the path has none, or
# goes through . or ..
function first_directory(path) {
	if (index(path, "/") == 0 || ("/" path "/") ~ /\/\.\.?\//)
		return ""
	sub(/\/.*/, "", path)
	return path
}

# Whether path is one of the public headers of module. In a pattern, * stands for any name.
function is_public(path, module,    count, patterns, i, pattern) {
	count = split(public[module], patterns, " ")
	for (i = 1; i <= count; i++) {
		pattern = patterns[i]
		gsub(/\./, "[.]", pattern)
		gsub(/\*/, "[^/]*", pattern)
		if (path ~ ("^" pattern "$"))
			return 1
	}
	return 0
}

# Why a file of module may not include path, or "" if it may.
function refusal(module, path,    target, count, used, i, only) {
	target = first_directory(path)
	if (target == "")
		return "not a module path, such as \"base/base.h\""
	if (!(target in public))
		return target " is not a module of architecture section 2"
	if (target == module)
		return ""
	if (target != "base") {
		only = ""
		count = split(uses[module], used, " ")
		for (i = 1; i <= count && used[i] != target; i++) {
			if (used[i] == path)
				return ""
			if (index(used[i], target "/") == 1)
				only = only " " used[i]
		}
		if (i > count && only != "")
			return module " uses only" only " of " target
		if (i > count)
			return module " does not use " target
	}
	if (!is_public(path, target))
		return "private to " target "; other modules may include only " public[target]
	return ""
}

function problem(message) {
	print message > "/dev/stderr"
	problems++
}

BEGIN {
	count = split(ENVIRON["MO_DEPS_RULES"], rows, "\n")
	for (i = 1; i <= count; i++) {
		if (rows[i] ~ /^[ \t]*#/ || rows[i] ~ /^[ \t]*$/)
			continue
		if (split(rows[i], cells, "|") != 3) {
			print "ci/deps.sh: a rule needs three columns: " rows[i] > "/dev/stderr"
			bad_rules = 1
			exit 2
		}
		public[trim(cells[1])] = trim(cells[2])
		uses[trim(cells[1])] = trim(cells[3])
	}
	root = ENVIRON["MO_DEPS_ROOT"]
}

{
	file = $0
	files++
	path = substr(file, length(root) + 1)
	sub(/^\/+/, "", path)
	module = first_directory(path)
	if (module == "") {
		problem(file ": not in the directory of a module, such as " root "/base/")
		next
	}
	if (!(module in public)) {
		problem(file ": " module " is not a module of architecture section 2")
		next
	}
	number = 0
	while ((status = (getline line < file)) > 0) {
		number++
		if (line !~ /^[ \t]*#[ \t]*include[ \t]*["<]/)
			continue
		sub(/^[ \t]*#[ \t]*include[ \t]*/, "", line)
		opener = substr(line, 1, 1)
		closer = (opener == "<") ? ">" : "\""
		line = substr(line, 2)
		if (index(line, closer) == 0)
			continue # the compiler reports it
		path = substr(line, 1, index(line, closer) - 1)
		# A header in angle brackets is a system header, unless its path starts with a module.
		if (opener == "<" && !(first_directory(path) in public))
			continue
		includes++
		reason = refusal(module, path)
		if (reason != "")
			problem(file ":" number ": #include " opener path closer ": " reason)
	}
	if (status < 0)
		problem(file ": cannot be read")
	close(file)
}

END {
	if (bad_rules)
		exit 2
	if (files == 0) {
		print "ci/deps.sh: no C files in " root > "/dev/stderr"
		exit 1
	}
	if (problems > 0) {
		print "ci/deps.sh: " problems " error(s); the rules are in the table at the top of ci/deps.sh" > "/dev/stderr"
		exit 1
	}
	print "ci/deps.sh: every include in " root " follows architecture section 2 (" files " files, " includes + 0 " includes of Mo headers)"
}
'
