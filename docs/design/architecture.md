# Mo Architecture v0.1

**Draft, 12 October 2026, revised after two independent reviews.** This is how
Phase 2 builds the `mo` toolchain in C (decision D207): the compiler, the
virtual machine, and the contracts between them. It is written against the
Language Reference v0.1 (`docs/reference/`), whose rules win wherever the two
disagree. It also builds on Mo Core (`formal/`) and the bytecode
specification (`bytecode.md`). It fixes what Phase 2's work streams build
against, so they can work in parallel. Section 13 lists what is still open.

## 1. The shape of the system

One program, `mo`, with the commands `mo check`, `mo run`, `mo test`,
`mo format` and `mo fix`. It compiles a whole program at once, from source to
bytecode, and runs the bytecode on its own virtual machine (D195).

```text
┌────────┐  ┌────────┐  ┌──────────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────┐
│ source ├─►│ parser ├─►│ names, types ├─►│ lowering ├─►│ bytecode ├─►│ loader,  ├─►│  VM  │
│ (.mo)  │  │        │  │  and checks  │  │ to Core  │  │ compiler │  │ verifier │  │      │
└────────┘  └────────┘  └──────────────┘  └────┬─────┘  └──────────┘  └──────────┘  └──┬───┘
                                               │ Core, as text                         │ outcomes
                                               ▼                                       ▼
                                        ┌──────────────┐                        ┌──────────────┐
                                        │ Lean         │◄──── compared ────────►│ value, stop  │
                                        │ evaluator    │                        │ or failure   │
                                        └──────────────┘                        └──────────────┘
```

- **The front end** turns source into Mo Core. It parses, resolves every name,
  types every expression and checks every rule of the reference, and then
  lowers the checked program to Core and instantiates its generic functions.
- **Mo Core** is the small language every feature lowers to. It is defined in
  Lean, which gives it an evaluator to test against (section 4). It has a text
  form, so the C toolchain and Lean can exchange programs.
- **The bytecode compiler** turns Core into bytecode. It places the reference
  counting, the borrowing and the in-place reuse.
- **The VM** loads bytecode, verifies it (condition 5 of D207), and runs it. It
  never sees source. Everything it does to the outside world goes through
  capability objects (section 7).

Four contracts connect the parts, and each one is a file that every side reads:

| Contract | Defined in | Between |
| --- | --- | --- |
| Mo Core | `formal/` (Lean) and `src/core/core.h`, with a text form | the front end, the bytecode compiler and the Lean model |
| The bytecode format | `bytecode.md` and `src/bytecode/bytecode.h` | the bytecode compiler and the VM |
| The primitives | `src/base/prims.def` | the checker, the compiler, the verifier and the VM |
| The capability operations | `src/base/hostops.def` | the same four, and the host |

## 2. C modules and their boundaries

```text
src/base/      assertions, arenas, limits, checked arithmetic (64 and 128 bits,
               and Decimal's 256-bit products), UTF-8, hashing, interned names,
               diagnostics, source files, and the tables prims.def and hostops.def
src/syntax/    lexer, parser, syntax tree, formatter
src/check/     program loading, names, types, abilities, capabilities,
               exhaustiveness, the rules of reference section 6
src/lower/     lowering the checked program to Core; monomorphization
src/core/      Core's data structures, its text form, and the Core checker
src/codegen/   borrow inference, reference counting, reuse, registers; writes bytecode
src/bytecode/  the format: writer, reader, verifier, assembler and disassembler
src/heap/      the runtime type table and layouts (types.h); values, cells, heap
               objects, reference counts, and the generic equality, hashing,
               dropping and printing walks
src/prim/      the primitives of prims.def
src/host/      capabilities, real and fake
src/vm/        processes, the interpreter, calls and frames, stops and defect reports
src/tools/     the mo command: check, run, test, format and fix; diagnostics as text and JSON
lib/           the standard library written in Mo (condition 4 of D207)
```

Every module uses `base`. Beyond that, each uses only the modules listed here:

| Module | Uses |
| --- | --- |
| `syntax` | none |
| `check` | `syntax` |
| `lower` | `check`, `core` |
| `core` | none |
| `codegen` | `core`, `bytecode`, and `heap`'s types |
| `bytecode` | `heap`'s types |
| `heap` | none |
| `prim`, `host` | `heap` |
| `vm` | `bytecode`, `heap`, `prim`, `host` |
| `tools` | all of them |

The VM never sees source, syntax trees or Core. Each module has one public
header, named after it, such as `src/check/check.h`. There are two
exceptions. `base`'s headers are each a separate facility: `base.h`,
`arena.h`, `buffer.h`, `limits.h`, `diag.h`, `codes.h`, `path.h`, `prims.def` and
`hostops.def`. And `heap` has
a second header, `types.h`, with the runtime type table, the layouts and
`mo_case_shape`; the compiler, the bytecode format, the primitives and the
host use it without the rest of the heap. Nothing else is included across
modules.

Every module follows the five conditions of D207: sanitizers on every test,
fuzzing in CI, Tiger Style (`MO_ASSERT` always on, explicit limits, arenas,
every result checked, `-Wconversion`), a small trusted C core, and bytecode
verified on load.

**Memory.** Compiler stages allocate from arenas that live as long as the
stage's output and are freed at once. The VM allocates Mo values from a
process's heap, with reference counts (section 6). No module calls `malloc`
directly outside `base` and `heap`.

**Limits.** Every input has a named limit in `src/base/limits.h`, so a test can
reach each one. Limits on what the compiler reads are **diagnostics**: file
size, line length, nesting depth, names per module, registers per function,
instructions per function. Limits on a running program are **bugs**, with a
report (reference 13.2): the stack's size, the call depth and the heap's size.

**128-bit integers.** `base.h` defines `mo_i128` and `mo_u128` as
`__extension__ typedef __int128`. GCC and Clang both have the type. The
`__extension__` keeps `-Wpedantic` quiet, and a static assertion fails the
build on a compiler without it. Decimal's 256-bit intermediate products are
written once, in `base/decimal.c`, shared by the VM and primitives. Division
takes exactly 256 steps and rounds the exact result once, with ties to even.
The current gate checks boundary and generated results against exact Python
rationals through the public VM; CBMC proofs remain to be added.

## 3. The front end

### 3.1 Parsing and formatting

The parser is hand-written recursive descent, because Mo's error messages need
to know which block is unfinished and where to resume (D33, D223). It produces
a syntax tree whose every node has a span: a file, and start and end byte
offsets. Lines and columns are computed when a message is printed. Columns
count characters, as the executable grammar does.

The executable grammar (`grammar/mo.y`) is the specification. The parser and
the grammar must agree on every input within the limits of
`src/base/limits.h`: they accept the same inputs and reject the same ones. The
grammar enforces none of those limits but the depth of interpolations, so the
fuzz target that checks this continuously, by running both on the same bytes,
compares them only on inputs within the limits. After an error, the parser
resumes at the next line that can start a declaration or a statement, so one
mistake gives one error.

The lexers distinguish a lambda's parameter-list `)` from a call or group
closer (D237). A following `[` can begin the lambda's one-line list body,
but remains refused as indexing after calls and groups. Lambda parameters
contain only names, `_` and commas, so this needs two flags and no stack.

`mo format` prints the syntax tree back. The tree keeps every comment: a
comment on its own line is attached to the next node at its indentation, or
to the end of its scope if nothing follows. Blocks and multi-line lists,
maps, tuples, groups and calls are scopes; a comment before a closing bracket
stays inside that scope. These comments use `leading` or `ending` with the
`OWN_LINE` anchor. A comment at the end of a line uses `trailing` on the
owner of the last token on that physical line, including tokens such as
`not`, `(`, a parameter-list `)`, `else`, and `end`. Its `TOKEN` anchor
records the token kind and its zero-based occurrence among tokens of that
kind owned directly by the node, excluding child tokens. Leaves own their
raw token; single operators own their operator; flattened chains use their
operator-token or postfix-step nodes. A construct owns its keywords and
delimiters. Collection sequences, binders and expression-statement wrappers
own no lexical tokens. The final `end` of an `if` belongs to that `if`, not
to its last else part. No source location is needed to print an anchor.

Optional trailing-comma comments use an `ITEM` boundary anchor on the item
collection, with the zero-based index of its final item. A comment immediately
after that item's final token, when the next token is the collection's closing
delimiter, uses the same anchor. In particular, `[1, # c` followed by `]`
and `[1 # c` followed by `]` have identical trees: optional trailing commas
are not part of the tree. This applies to parameters, arguments, named
fields, tuples, lists and maps wherever the grammar allows a trailing
comma. Other commas, type-argument and header collections, lambda parameters
and single unnamed case fields retain their exact `TOKEN` anchors. The printer
places the comment after the item and after any comma it chooses to print.
Non-token anchors have token kind `EOF`; own-line anchors have index zero.

Statements and declarations have `blank_before`: one bit recording whether
an empty physical line occurred since the previous statement or declaration.
Spaces and tabs on an otherwise empty line count; comment-only lines do not.
Runs collapse to one blank line, placed before the attached leading comments.
The first statement of a block has no initial blank line; specification
blocks and the ordinary function body form one statement sequence for this
purpose. A module header can precede the first declaration's blank line.
No end-of-block blank lines are retained. No intra-statement break bits are
recorded: D238 chooses those breaks from the tree at 100 display columns.

Formatting is a function of the tree.
Formatting twice changes nothing, and the formatted program's tree equals the
original's, except for the rewrites the reference names: `x = x + y` becomes
`x += y` (reference 6.2).

**The printer (D238).** `syntax/format.h` prints a successful
tree through an arena-owned Wadler-style document: text, groups, nesting,
optional breaks and mandatory lines. Flat group widths are cached; rendering
does not repeatedly scan document suffixes. Syntax takes a display-width
callback; tools supply `mo_unicode16_display_width` from prim, without adding
a syntax-to-prim dependency or copying Unicode tables.

Tools select width-based layout at 100 display columns. A statement that fits
stays on one line; longer statements break at outer brackets, dots or operators
before their nested groups. Layout ignores source storage, including source
spans used only for locations. Indivisible names,
literals and comments can exceed the target width. The document budget is
128 MiB, with 8,224 nested groups or alignment scopes; the CLI's output is
bounded by `MO_LIMIT_FILE_SIZE` (8 MiB) and 65,536 scalars per physical line.
A refusal returns no partial output and never replaces the source file.

Indentation is two spaces per level. Bracket contents and block bodies indent
from the physical indentation of their opening line, not a dormant dot or
operator continuation. Operators have one space on either side, except for
`..`; field dots and bracket edges have none. There is one space after commas
and colons; function result annotations use ` : `. Broken bracket groups put
each item on its own line and put the closer at the opener's indentation.
Module and use paths have their own fit groups; long paths break before dots
with two-space continuation indentation, independent of other header lines.
An operator stays beside a bracketed operand's opener. A call with one
bracketed value or block callback shares that inner scope's opening and
closing lines, without another pair of call-enclosing lines, as the existing
examples do. Repeated prefix minuses are separated so their token boundary
stays clear; interpolation edges have no extra padding.
There are no trailing commas. An inline comment follows its exact token or
item boundary and forces the next token onto another line. Consecutive rendered
code/comment rows align at one column, two spaces after the longest code row
(D238); a blank row, own-line comment or code-only row ends that alignment run.
Alignment uses the supplied display-width callback and does not feed padding
back into statement breaks. Indivisible comments may extend beyond 100 columns.
Alignment scratch fits `MO_LIMIT_FILE_SIZE` and added padding still observes the lexer's
65,536-scalar physical-line limit; refusal leaves output empty.
Own-line comments retain their text and attachments. Leading/ending blank
lines disappear, statement blank runs collapse to one, and nonempty output
ends in one LF. Literal and comment spellings are not rewritten. Case arms
with one non-block statement can share the colon's line; nested control blocks
start on the next line. The `+=` rewrite applies to equal names and field
paths, but is deferred when it would discard or move a token comment.

The corpus test reparses every accepted input, compares the tree (including
comment anchors and blank bits, modulo `+=`), and formats again. Its strict
fixed-point mode covers all reference `mo` blocks, examples, library samples
and good grammar fixtures except four named syntax tests:
`grammar/tests/good/layout.mo` deliberately varies layout;
`declarations.mo` includes a trailing comma; `lambda-list.mo` uses short
authored parameter breaks; `literals.mo` varies interpolation and unary
spacing. All four remain in the parse, tree-equality and idempotence gates.
Reference section 2.4 alone is exempt from byte equality: its three continuation
examples demonstrate allowed breaks that the formatter would join. The gate
identifies that section by its heading and verifies those exact three examples;
they still pass the parse, tree-equality and idempotence gates. No other reference
blocks, examples or library samples are exempt.

**The source tree contract.** `src/syntax/syntax.h` defines a tagged node
with ordered children, an operator token, and half-open spans. Absent
optional children are `NULL`; an empty sequence is a node with no children.
Leaves and comments copy their raw source spellings into the caller's
arena, including literal quotes, escapes and interpolation delimiters. The
tree does not borrow source storage. Grouping parentheses remain nodes;
lists, maps, tuples and argument sequences remain distinct. No typing or
name resolution is done here.

Trivia annotation uses the caller's bounded arena budget for temporary
indexes. Only files with inline comments need an additional token scan;
that scan skips allocating comments, indexes token occurrences, and
normalizes item boundaries. It does not change the parser's acceptance or
the semantic tree, and all existing checker consumers may ignore trivia.

Children follow the executable grammar's production order with these
normalizations: a function type has its argument sequence and optional
result, with token `FN`; a tuple type has one sequence, with token `LPAREN`;
other types have their name and optional type arguments. Interpolated text
has alternating raw text parts and expressions in one flat sequence. A
case has arms, each with alternatives, an optional guard, and a statement
sequence which includes the statement on its header line. An arm's span
extends to the next arm or the case's `end`, so comments ending an arm have
an owner. Block and one-line `if` nodes have the first condition and body,
then a sequence of `IF_PART` (condition, body) and optional `ELSE` (body)
nodes. Else-if parts are flat, not nested `IF` nodes. Same-precedence binary
chains with at least two operators are `BINARY_CHAIN` nodes containing
operand, operator token, operand, and so on; a single operator remains
`BINARY`. Postfix chains with at least two steps are `POSTFIX` nodes: base,
then call or field steps, each with only its arguments or field child, not
a repeated base. A single postfix keeps its two-child shape. Repeated
prefix operators are `PREFIX` nodes containing operator tokens then the
operand; a single prefix keeps its one-child shape. Consumers walk these
chains iteratively, preserving operator order and left associativity.

Erroneous items are omitted. Their containers have `incomplete` set, so the
checker can inspect retained children without checking the container as a
whole. A damaged declaration header can leave even a required child `NULL`,
but its body is retained. A damaged control-block header has the same
boundary. A failed case-arm header leaves an incomplete placeholder arm,
so its body cannot become part of the previous arm. Recovery identifies an
arm's colon outside quotes and brackets through the failing physical line;
a failed ordinary statement stays in its existing arm. Valid retained
children are still validated; `incomplete` suppresses only whole-container
checks. Line recovery has boundaries inside declaration and control headers,
statement blocks, case arms, record and choice declarations,
specification lines and anchors. Only allocation failure or inability to
keep a diagnostic returns `NULL`.
`mo_read_header` stops before the body's first token and leaves its output
unchanged on failure. `mo_parse_fragment` uses the grammar's fragment mode
for reference snippets, fixtures and differential fuzzing.

Nesting counts enclosing source constructs: declaration, specification and
control blocks, brackets and parentheses, lambdas, call arguments,
parenthesized types and interpolated text. Function parameter lists and
case-field lists add one around their types. Leaves, tree wrappers,
operator and postfix chains, and else-if parts add no depth. The same
nesting bound applies in a fragment, binding or function; a function's block
adds one. An `else if`
is a continuation, but `else: if ...` contains a nested value. E1006 points
to the innermost opener that exceeds the bound. Each source limit is
reported at most once per parse. The parser also checks the
source counts already visible in syntax: module declarations, parameters
and calls (including function types), record and case fields, tuples
(including types and binders), and choice cases. The module-name limit
counts declarations, not distinct spellings; overloads count separately.
Capture counts and limits which need types, lowering or bytecode remain
with their owning stages.
The differential target includes Bison's lexer and AST validation, not
just `yyparse`, and compares exact normalized trees, including operators
and leaf positions. It excludes a mismatch only when every parser
diagnostic is a source-limit code and an independent walk of the grammar's
tree or source proves each reported limit was exceeded. It also compares
header reads with accepted full parses and exercises input-derived arena
limits. A token dictionary and regression seeds run in CI, including
generated long binary and prefix chains. The grammar's AST validator walks
ordinary expressions with an explicit stack, preserving its first-error
order. Bison's maximum stack is four times the source-byte limit rather
than its default 10,000 states: flat prefix chains need LR stack space but
add no source nesting. These are instrument resource changes, not grammar
rule changes. The independent limit walk precedes AST validation, so a
proved out-of-domain input need not validate an over-deep pattern.

**Resources.** The caller chooses an arena budget, not just a source-size
budget. Dense generated source can need roughly 100 bytes of arena per
source byte (about 800 MiB for an 8 MiB source), and some flat chains need
more; this is an estimate, not a bound. Failure to allocate is E1054, even
during recovery. Comment attachment uses temporary arena storage for a
node-start index and sparse inline-token counters, bounded by the caller
arena's unused budget and freed before returning: caller arena plus scratch
never exceeds that arena's limit.
Source storage and diagnostics have their own base-module budgets.
Reserve at least 2 MiB of C stack for the parser and tree walks. Deep
255-lambda fixtures need about 256–512 KiB in optimized builds and about
1 MiB with AddressSanitizer; actual requirements depend on compiler flags.

### 3.2 Loading a program

`mo` is given one file: the program's **entry module**, whose `main` `mo run`
runs. The entry module's name, such as `Shop.Cli`, together with its path,
such as `app/shop/cli.mo`, gives the **root folder** (`app/`). Every other module is found
from its name under the root (reference 2.1). The loader reads each file's
header, the `module` line and the `use` lines, with `mo_read_header`, without
parsing the rest. It follows `use` lines to the files they name, and builds
the module graph, refusing cycles. The library's modules (`lib/`) are found
in the toolchain's own folder, and are always available.

**The loading boundary.** `mo_load_program` takes an explicit library folder from
the caller, rather than locating the running executable. The tools supply
the installed library's path; `NULL` permits a library-free fixture while
`lib/` is being built. A supplied folder must exist. All its `.mo` files,
including unused modules, are included. Discovery does not enter symlinked
folders. A dependency or library source file may be a link only when its
resolved path stays under the resolved path of its own root folder (D236).
This is checked before its bytes enter source storage or diagnostics; the
entry file may itself be a link outside the root. Source reading opens files
nonblocking and requires a regular file, so FIFOs and device links cannot
hang it. These checks assume the filesystem is not concurrently replaced.
Library files are sorted by path before their headers are read. The entry
is always module zero, followed by library files and uses in discovery
order; a separate dependency-first order names every module once.

The root is derived from the entry path's spelling, after collapsing `.`
components and repeated slashes, not its `realpath`:
the path must end at a folder boundary in the declared name's file path.
This preserves a caller's chosen module root through source-file links.
CapCase words are split before an uppercase letter following a lowercase
letter or digit, or at the last uppercase letter of a run followed by a
lowercase letter (D237). Digits stay with what comes before: `XMLReader`
becomes `xml-reader.mo`, `HTTP2Server` becomes `http2-server.mo`, and
`Unicode16Tool` becomes `unicode16-tool.mo`. `mo_module_path` in `base/path.h`
owns this conversion; the loader and library-map check use it, tested against
the same name/path table. Names and paths must agree. A missing module's
diagnostic names the file looked for; a mismatched header labels both the
requesting use and the declaration. A cycle labels every use in the cycle.
`Json` and `Unicode16` are reserved even in library-free fixtures; every
discovered library name is reserved too. A library use resolves only inside
the library, never by falling back to the program's root.

An AVL name index shares diamond dependencies without repeated file reads.
An explicit DFS stack bounds graph traversal by `MO_LIMIT_FILES`, not the
C stack. The loader stops at the first loading error, and never hands a
partial graph or erroneous syntax to checking. A header read that adds a
diagnostic fails loading even if it returned a usable header. Full parsing
starts only after the complete graph passes. All stage storage, including
discovery scratch, belongs to the caller's arena; source storage and
diagnostics retain their base-module budgets. E1058 reports allocation
failure. `MO_LIMIT_LIBRARY_ENTRIES` (65,536, E1059) bounds discovery even
when no source files are present. Source bytes, file count and path lengths
use the existing base limits and their codes. E1101 refuses a module outside
its root; E1102 reports an unreadable library folder without a language
section citation.

**Source-stage stack.** Checker tests and the `fuzz_check` and `fuzz_bodies`
targets run source work through `mo_with_stack` in `base/stack.h`. Tools run
the whole compiler pipeline, including parsing and future lowering, Core
checking and codegen, in one call with `MO_SOURCE_STACK_SIZE` (10).
It rounds the requested allowance
up to the POSIX page size, sets a new thread's stack explicitly and joins it
before returning; arenas, diagnostics and the shared work budget remain
single-threaded. It returns a POSIX error number without running the callback
if thread setup fails. It does not change the caller's stack limit or assume
the platform's main-thread or secondary-thread default.
The 255-call fixture containing the full precedence ladder, now producing a
checked graph through `mo_check`, first passed at 16.25 MiB in clang's `-O0`
debug build and 22.25 MiB with `-O1` ASan/UBSan on Linux x86-64, in a 256 KiB
sweep; 16 MiB and 22 MiB respectively failed. Graph emission enlarges inference
frames; the earlier inference-only minima were 7.75 MiB and 10.5 MiB.
`MO_SOURCE_STACK_SIZE` reserves 48 MiB, more than twice the larger measured
minimum. Unused stack is reserved address space, not touched memory. Compiler
flags and platform instrumentation can change these measurements; the deep
checks run under the same explicit allowance in CI and fuzzing.

### 3.3 Names and types

**Module interfaces precede bodies.** `mo_resolve_modules` indexes complete
loaded syntax, checks top-level name clashes and overload ownership, marks
exposure, and resolves explicit imports. Its result is not a checked
program: body checking still owns lexical names, signature types, uses of
hidden insides, unused imports and every flow rule below. Tests, which have
textual labels rather than callable names, are not module symbols.

Symbols are sorted by name and source position; lookup is binary search.
`mo_find_symbol` accepts bytes and length, without copying syntax spellings.
Each case retains its owning type's symbol index. Each overloaded function
retains its first parameter's local nominal type index for call resolution.
All overloads travel together in one import. Their first parameter must
have a distinct locally declared record or choice constructor; two
applications of the same generic constructor do not distinguish overloads.
Module prefixes live separately from value/type names, so importing module
`Money` and its type `Money` does not clash. This is not a user-defined alias.
Names always in scope (reference 10.5) cannot be declared, imported, or used
as a module prefix's last segment, by D224's one meaning per name and
reference 10.3's clash rule.
Exposing `T(..)` exposes its cases; importing `T` brings those exposed
cases. Directly exposing or importing a case is refused: cases travel
through their owning type, not independently. A direct case import has a
fix to import its exposed type, or to remove the redundant case when the
type is already listed. No fix is offered for hidden cases. Repeated expose
entries merge, with `(..)` taking precedence. Repeated explicit imports are
clashes, detected before case expansion; a repeated name in one use list
has a deletion fix. Other clashes suggest qualification without rewriting
uses whose intended binding is unknown.

Interface work is bounded by source declarations and imported names, not
pairwise name scans. Each choice's case indices are computed once in its
declaring module, in declaration order. Imports borrow the existing names
and symbols rather than re-spelling, looking up and copying cases.
`MO_LIMIT_MODULE_IMPORTS` (1,048,576, E1060) counts
one per imported name group plus the choice cases it brings, before
expansion; `MO_LIMIT_PROGRAM_IMPORTS` (4,194,304, E1100) caps their sum across
all modules, reported once. Independent errors across modules and name
groups are retained and sorted by source position before returning `NULL`.
An invalid interface suppresses dependent import errors, not independent
errors in other modules. Allocation or diagnostic-storage failure stops
checking immediately. Temporary request indices and all interfaces use the
caller's arena, not unaccounted scratch. Interface declaration and origin pointers
borrow the loaded syntax, which must outlive them. Loading and interface
checking are the first independently usable slice; `mo_check` and its
typed program follow.

**Signatures.** `mo_prepare_signatures` resolves the parameter and result
annotations of every named function before its body is visited. Omitted
results are `Unit`. Tuples and function types keep ordered arguments; a
function type's last argument is its result. An AVL index interns types,
so repeated annotation types have one identity within a preparation.
Nominal types retain their module symbol, not just their printable name.
Locally declared and explicitly imported types take precedence over implicit
single-letter parameters; qualification always selects a nominal type.
Implicit parameters belong to their function, and are abstract there, not
shared with another function's same-spelled parameter. All storage uses the
caller arena, with E1058 on exhaustion. This intermediate result checks no
bodies or abilities and is not a checked program. It also prepares record and
choice-case field types. Each case retains its owning choice's nominal type;
recursive fields retain that identity rather than expanding a layout.
Explicit single-letter data parameters are scoped to their declaring type.
Fields are indexed by name for binary lookup, with declaration-order indices
for lowering. Constructor lambda fields receive constraints from ordinary
arguments first, just as positional lambda arguments do. Output metadata keeps
written evaluation order, not the order in which constraints were solved.
Duplicate declared or supplied fields are refused without pairwise scans.
Record/case construction and field reads check the owning type's `(..)`
exposure before revealing its layout. Field metadata borrows syntax and is
stored once in the declaring module, not copied into every importing module.

**Local inference is a second intermediate result.** `mo_infer_function`
checks one named body against prepared signatures, returning postorder
expression metadata: types, local binder or module symbol, and whether all
paths leave the function or loop or stop. An AVL lexical index restores
visible bindings on scope exit, while retaining local/module name uses for
the one-meaning rule. Union-find constraints preserve numeric literal choices
until all uses are known. Named generic parameters remain abstract in their
own body; each reference to a generic function gets fresh parameters.

This stage supports positional and module-qualified function calls, contextual
anonymous functions, scalar/list/map/tuple expressions, tuple projections,
bindings, variables, conditionals, loops and early exits. It reports one
independent error per body, choosing the earliest retained source span, so a
dependent error cannot cascade. End-of-scope and final type checks retain the earliest candidate;
after an inference failure only already-retained, earlier expressions are
finalized, without inventing unresolved-type errors from the missing uses.
Resource errors stop immediately and take priority. The caller can
continue with independent bodies. Signature preparation currently stops at
its first error too. E1103 explicitly refuses unfinished body forms; it does
not silently accept them. Builtin/dot
dispatch and delayed overload selection, abilities, polymorphic recursion,
failure propagation, capture/capability flow, specs, constants, unused imports
and the entry contract still need their stages. Compile-time duplicate map
keys (equal literals or repeated names, reference 3.8) are also still pending.
Nonempty examples or promises are refused with E1103, never silently skipped.
A successful intermediate body is not a `mo_program`, is not lowering-ready and does not certify those
rules. `mo_check` composes implemented stages for a certified subset; section 12
records its walking skeleton and the forms still refused with E1103.

**Typed data and patterns.** Construction names every declared field once, or
supplies a case's single unnamed value. Reads retain the selected field's
declaration index; field writes retain their original base `var` binder and
cannot cross a function boundary. Compound assignment uses the same binary
operand rules as its expanded assignment.
Patterns resolve constructors and fields, share bindings across alternatives,
and retain the scopes of `is`, `and` and `implies`. A whole-arm wildcard or name
over a choice is refused even when guarded. Guarded rows never prove coverage:
their conditions may be false. An iterative Maranget matrix checks tuples,
records, choices, lists and literal domains, and reports a bounded missing-value
preview, not a speculative edit. A missing choice partition lists up to eight
absent cases in declaration order, then `and N more`; names longer than 128
bytes are elided so the omitted count remains visible. Previews also have a
4,000-byte ceiling. All-guarded cases explain that guards do not count toward
coverage, listing the choice's cases or the missing domain. Qualified constructor
patterns use the same resolution and binding rules as unqualified patterns.
The literal fallback accepts only literals and negated numbers, never construction
expressions. `Entry` patterns report E1103 until native record interfaces arrive.
Byte patterns normalize numeric spelling after contextual typing and range
checks, so all 256 values establish coverage.
Matrix cells, partitions, sorting and witness rendering charge the existing
body and program work budgets. Each allocated coverage byte also costs one work
unit, bounding retained matrix storage independently of the caller's arena;
arena exhaustion retains E1058. Missing-case previews are built only for a hole,
using the choice's case index rather than its module's unrelated symbols.
Explicit frames handle wide products and lists without increasing C stack depth. Coverage of
completed matches before a later independent error participates in the same
earliest-source-error selection as inference and unused-value checks, but a
scrutinee left unresolved or defaulted during a failed walk cannot prove coverage.
An attempted read outside a local's scope replaces that local's unused-name
error, not an earlier independent error. Unused-name fixes are withheld for shared
alternative binders; a single unnamed match of a single named field has a certain
field-name insertion fix. Missing construction fields are named. Coverage errors
mark the case header and its scrutinee, not the full arm block.
E0062–E0071 cover duplicate fields, constructor arguments, hidden insides, missing
fields, data parameters and pattern errors; each has a diagnostic corpus input.

Inferred types are shared DAGs, not expanded trees. Consumers must memoize
walks. Prepared annotations retain parameter masks and heights; monomorphic
subterms, and rigid parameters in a function's own signature, are borrowed
unchanged. Only generic subterms are instantiated, and frozen children are
materialized lazily when a constraint needs them. Occurs checks skip frozen
terms and visit each other shared term once per walk; output conversion
caches each term and its longest leaf path. Source nesting cannot bound the
types built by flat calls: `duplicate(T) : (T, T)` can double a type's apparent
tree on every line. `MO_LIMIT_SOURCE_TYPE_DEPTH` caps paths at 256 nodes,
including function values (E1105); cached subtrees retain their height when
reused at greater depth. `MO_LIMIT_SOURCE_CHECK_WORK` caps term construction,
unification, occurs checks, expression visits and output conversion at
16,777,216 operations per function (E1104). `MO_LIMIT_PROGRAM_CHECK_WORK`
caps their total at 268,435,456 (E1107), shared by all inference calls against
one signature preparation. Checking is single-threaded; new preparation starts
a new run. These errors are implementation limits, not language restrictions.
All temporary and output storage is
charged to the caller arena (E1058), with no additional scratch arena.
`MO_LIMIT_SOURCE_INFERENCE_DEPTH` allows sixteen inference frames per source
nesting level plus eight function wrappers (E1106). A source level can retain
the entire precedence ladder, not just a call and its argument. Flat chains
remain iterative. Callers use the explicit source-stage stack of section 3.2;
the input depth bound is not a guarantee about a platform's default C stack.

Non-lambda positional arguments constrain the callee before contextual lambdas,
regardless of written order; output notes retain written postorder. Binder notes
carry types, and assignment notes point to the original var binder, including
compound assignments. `+=` and `-=` use the same operand rules as their expanded
forms. Unit contexts infer discarded tails without imposing Unit on their
values, so unused values and ignored Results keep their specific diagnostics.
Unused vars have no underscore fix: changing only the declaration could turn a
later assignment into a new binding. Float literal validation compares source
digits with the exact nearest-even overflow boundary, independently of host
locale and rounding state; value conversion belongs to lowering.

Type checking is **bidirectional**, one function body at a time. A signature
is given, and the body is checked against it: an expression is either
checked against an expected type or has its type inferred, and inferred types
are unified. Inference never crosses a function boundary, so errors stay
local. Names are resolved during checking, not before it, because Mo resolves
some names by type:

- **A call** checks its callee first. It then checks each argument against the
  callee's parameter type. So an anonymous function passed as an argument
  learns its parameter types before its body is checked, as in
  `customers.filter(fn(c) c.active? end)`.
- **A dot call** `x.f(...)` is resolved once `x`'s type is known: in the module
  that declares that type first, then among the names in scope (reference
  5.4). A field of `x`'s record wins where it's visible. When the field holds a
  function, `x.f(a)` calls it. It's enough to know the type's constructor:
  `var items = []` followed by `items.append(x)` finds `append` in `List`,
  even before the element type is known. If `x`'s type is still unknown when
  the body's other constraints have been solved, that's an error: give `x` a
  type, or write the call another way.
- **Two functions with one name** in a module differ in their first
  parameter's type (reference 5.1). A call picks the one whose first parameter
  matches the first argument's type. If that type isn't known yet, the call
  waits, as a dot call does.
- **Literals** take the type their context needs (reference 3.3). An integer
  literal can be an `Int`, `BigInt`, `Byte`, `Decimal` or `Float`. A literal
  with a decimal point can be a `Decimal` or a `Float`. A literal out of its
  type's range is an error: an `Int` beyond 64 bits, a `Byte` beyond 255, a
  `Decimal` with more than 18 places or beyond its range. So is a `Float`
  literal that doesn't round to a finite value. A literal with nothing to
  decide its type is an `Int`, or a `Decimal`.
- **`is` patterns** bind names where the test is known to be true: in the
  block it guards, after `and`, after `implies`, and after `assert`
  (reference 7.4). The checker tracks this through `and`, `implies` and
  `assert`.
- **Generic functions** are checked once, with their type parameters abstract.
  Every instantiation is compiled separately (section 4.2), so a generic
  function can't reach itself again at a bigger type, which is called
  polymorphic recursion: it would need infinitely many copies. The rule is
  checked over each group of generic functions that call each other, directly,
  through other functions, or through anonymous functions. Within such a group, every
  call to a member must pass type arguments that are the caller's own type
  parameters, never types built from them, such as `List(T)`. Breaking it is
  an error whose message names the calls in the cycle (reference 5.5). As a
  second line of defense, lowering stops with a diagnostic if instantiation
  ever nests deeper than a limit.

### 3.4 Abilities, capabilities and the other rules

- **Abilities** are constraints on type parameters, checked at each call. The
  checker knows which built-in types have each of the five abilities (the
  table in reference 9.2), how records and choice types derive them, and the
  customizing functions `compare` and `to_text`.
- **Anonymous functions** may read a `var` only if nothing assigns it after
  the function is made, on any path, including a later turn of a loop around
  the function (reference 5.6). The checker finds the assignments that can
  follow the function's creation in the function's control flow.
- **Capabilities.** A type *holds a capability* when it is a capability or
  contains one. The checker refuses anonymous functions that capture such
  values, and fakes, `printed` and `printed_errors` outside tests, examples
  and promises (reference 11.4, 12.4).
- **Map literals** whose keys the checker can see are equal, such as two equal
  literals or the same constant twice, are refused (reference 3.8).
- **Exhaustiveness** uses the pattern-matrix method (Maranget, "Warnings for
  pattern matching", 2007). Reference 7.2's rule against whole-arm catch-alls
  over a choice type is checked on top of it.
- **Constants** may refer to each other in any order within the program, and
  are computed in the order of their dependencies. A cycle is an error
  (reference 10.4).
- **Promises and examples** are checked like functions returning `Bool`. An
  example line or a promise body becomes a spec function (`bytecode.md`
  section 3.3). The `mo check` command in tools evaluates the anchors by
  compiling them and calling the promise's body on the VM, each in a fresh
  process, with `mo_call` (section 8). Anchors may make fakes, as
  `Files.in_memory({}).read_only`.

### 3.5 Diagnostics

Every stage reports into one structure: a stable code (`E0412`), a headline in
the reference's words, a reason, the section the reason cites, a primary span
with a label, secondary spans with labels, and fixes (D223). The reason is one
sentence that says why, citing the rule's section, as appendix A's "Mo never
drops a failure silently (reference 8.3)." Implementation codes for compiler
bugs, resource limits and unfinished forms may cite no section. A fix is one
or more text edits, applied together or not at all, each against the original
text, so they don't overlap; a
diagnostic's fixes are alternatives. `src/tools` renders it
as the text of reference appendix A, or as JSON for `mo check --json`. Codes
are registered in one table, `src/base/codes.h`, with the reference section
each cites. A code is never reused for a different meaning. Diagnostics are
sorted by position. A stage that reports an error passes nothing it can't
vouch for to the next, so one mistake doesn't cascade. `mo fix` applies a
diagnostic's fix when it has exactly one (reference appendix A).

New implementation codes use blocks per stream: front end (check/lower)
E1100–E1199, code generation E1200–E1299, bytecode/heap/VM E1300–E1399,
and tools E1400–E1499. Existing codes keep their numbers.

## 4. Mo Core

### 4.1 What Core is

Core is a small, typed, monomorphic, strict functional language. Its syntax,
typing rules and evaluator are defined in Lean in `formal/`, which is the
authority. Its proofs say that well-typed Core programs don't go wrong: they
return a value of their type, stop with a bug, or run out of fuel. This
section summarizes it; `formal/README.md` lists what is proven.

- **Types:** `Int`, `BigInt`, `Byte`, `Decimal`, `Float`, `Bool`, `Char`, `Text`,
  `Bytes`, `Unit`, `Range`, `Time`, `Duration`, tuples, `List`, `Map`, `Set`,
  declared data types (records and choice types share one form: constructors
  with fields), function types and capability types. There are no type
  variables.
- **Expressions:** literals; variables; `let`; anonymous functions and their
  application; calls to top-level functions; tuples; constructor
  application; `case` on one level of a data type's constructors; `if`;
  primitive operations, each checking its own bugs, including the capability
  operations and the fake constructors; `stop`; `fail e`, an assertion's
  failure with the detail `e`, a `Text`, allowed only in spec code; and join
  points: local continuations, called only in tail position, which carry loops
  and early exits.
- **Programs:** the data types; the module constants, each `const name : T =
  e`; and the functions. Each function has its module and a kind: `function`,
  `test`, `example`, `promise` or `anchor`, with the signatures of
  `bytecode.md` section 3.3. The last four are spec code, and so are the
  anonymous functions inside them. One function is the entry (4.3).

This program layer is what the bytecode compiler needs beyond expressions:
kinds become the bytecode's kinds, constants become `init` functions, and the
compiler never has to look back at the checked program. The model now has
these types, `fail`, fakes, Unicode16's trusted interface and the program layer.

The program document also carries **compiler metadata beside Core's
meaning**, defined in `formal/TEXT.md`: a source-file table in `mo_sources`
index order; module ownership/exports and `T` versus `T(..)` exposure;
declaration spans/full names; spec wording, subject links, anchor expectations
and promise conditions; and body tables keyed by expression preorder and
binder slots. Functions and constants have separate tables aligned with
their declarations. This keeps every expression constructor and Lean's
semantics/proofs unchanged. Lean preserves and shape-checks metadata but
never uses it to type or evaluate Core. Changes of position, wording or
exposure can therefore change emitted bytecode without changing an outcome.
The explicit `noSource` mode is for handwritten fixtures; lowering from Mo
must emit the full envelope, not silently drop source information.

### 4.2 Monomorphization

Lowering instantiates every generic function at each list of types it is used
with, so Core has no type variables and no abilities. An ability becomes the
specific function for its type: a primitive, a library function, or a
function lowering generates (4.3). Polymorphic recursion is refused by the
checker (3.3), so instantiation always ends. An instantiation belongs to the
module that asked for it, which matters for Phase 5's upgrades (section 9).

### 4.3 Lowering

Each rule here has a written lowering in `formal/TRANSLATION.md`, with
examples. The ones a compiler is most likely to get wrong are:

- **`var`, loops, `break`, `continue` and `return`** become join points whose
  parameters are the `var`s the loop changes. An anonymous function captures
  the value a name has when the function is made. A function can't assign a
  `var` (reference 5.6), so a captured value can't change behind it.
- **`try`** becomes a `case`. On the failure path, the error value is wrapped
  in the enclosing function's error type when the reference says it is
  (reference 8.3): through the failure type's one case with a single unnamed
  value of that type. The checker reports an error when there is no such
  case, or more than one.
- **Patterns** become decision trees of one-level `case`s. `is` becomes a
  `case` whose bindings are visible where 3.3 says.
- **`and`, `or` and `implies`** on `Bool` become `if`. **`or` on an `Option`**
  is lazy too: `x or default` becomes `case x of Some(v) → v | None →
  default`, so `Time.parse(t) or stop "..."` stops only on `None`. The
  checker picks the meaning of `or` from its left side's type.
- **Text interpolation** becomes calls to the values' `to_text` functions and
  concatenation. Lowering generates a `to_text` for each type that has the
  `Text` ability, following reference 9.2: a type's own `to_text` where it has
  one, and otherwise the form that looks like code. A `Text` or `Char` inside
  another value is quoted and escaped, and at the top level it isn't.
  Lowering also generates `compare` for each type with `Order`, using the
  type's own `compare` where it has one. Equality and hashing are structural
  and done by the VM (`bytecode.md` section 8).
- **Ranges** `a..b` are values of type `Range`: two `Int`s. `for i in r`
  becomes a join point that counts from `r`'s start while below its end, so
  the counter never overflows.
- **List and map literals** compute their elements, or every key and value,
  in order, then build the value one entry at a time: a list from
  `List.%empty` with `append`, and a map from `Map.%empty` with
  `Map.%literal_insert`, which stops if its key is already there (reference
  3.8). `to_map` is a different primitive, in which the later value wins.
  A name that starts with `%` is a primitive only lowering emits
  (`src/base/prims.def`).
- **Constants** become one init function each, run in the order of 3.4.
- **`main`** is wrapped in an entry function, which receives the capabilities
  `main` declares and turns `main`'s result into an exit status: `Unit` gives
  0; an `Int` is the status, and one outside 0 to 255 is a bug; `Error(e)` of a
  `Result(Unit, E)` prints `e`'s text form to standard error and gives 1
  (reference 11.2). Only a Result entry without a declared Console gets an
  additional, generated Console parameter. Wrapper parameters retain main's
  written capability order, followed by that generated Console. It is never
  passed to source main; Unit/Int entries get no additional authority.
- **`assert`** in a test, and each example line, become spec code that calls
  the `fail` instruction with the details: both sides of a failed `==`, or
  the value that didn't match a failed `is`.

**Part 1 implementation.** `src/lower` consumes the normalized checked graph,
not syntax inference. Explicit arena-backed task and continuation lists perform
expression/pattern lowering; an explicit, memoized stack walks type DAGs.
Queued function/data instances have no recursive C walk. E1150 refuses later
forms (unsupported ability/native
operations, map literals, constants and spec code); E1151 is arena exhaustion, E1152 the
shared allocation-byte/step or Core-node bound, and E1153 the source-type,
instantiation or generated Core path bound. Compilation is independent of the
text codec: `mo_lower` does not print Core or apply the codec's smaller schema,
metadata, byte or scratch bounds. Callers emitting Core text report the printer's
own refusal at that boundary; it does not make a compilable program invalid.
`MO_LIMIT_LOWER_WORK` bounds all
allocation bytes and work independently of the caller's arena allowance.
Structural Core type heights are checked after generic substitution, not only
at the template's source depth; an over-deep concrete result refuses with E1153.
E1152 also bounds canonical types plus unfolded expression occurrences across
all bodies together, matching Core's whole-document node allowance.
Numeric immediates fold unary minus before building Core scalars, including
Int's minimum. Float literals convert in a private C numeric locale with
nearest-even rounding, then restore the caller's rounding state. Text literals
decode escapes and triple-text indentation/CRLFs into independently owned bytes.
Arguments and list elements evaluate in written order. Atoms pass directly;
mutable reads are snapshotted before later non-atomic arguments. Discarded
Unit/atom statements need no additional let. List elements then pass to Core's
`%list.lit` primitive. Map literal graphs remain staged with the checker.
Hash indices memoize concrete types and function instances; concrete source
types do not depend on an instance's substitution pointer. Source-function
lookup uses a sorted index, and metadata traverses per-declaration instance
lists rather than a module/declaration/instance cross product.

Core lookup/display names retain the declaring module and source name;
overloads add `@Receiver`. Generic function code is keyed by (requesting
module, defining declaration, concrete type list), with names such as
`Main::Lib.identity(Int)`. Each requester owns its copy; nested requests belong
to that instance's owner. Names, declaration spans and module ownership retain
the types, origin and requester without relying on discovery numbers.
Generic data instead has one canonical declaration per type list, owned by
the defining module. Nongeneric data is retained even without an expression use.
`Lib.make()` returning `Lib.Box(Int)` must return the same
nominal type a requester-specialized function accepts; separate requester data
names would make those values incompatible in Core. Anonymous names retain their
source ordinal as `.fn.N`; generated function-value adapters use `.ref.ROW`.

Capturing lambdas prefix their body with aliases of the checked captures in
first-use order. Each alias's bound expression refers to the outer binding;
its body sees the local copy. This gives Core/codegen a first-use-ordered free
environment, captured by value at closure creation, independent of later CPS
rewrites. The aliases retain the captured source names and the lambda span.
The checker owns capture legality, including capabilities and later var writes.

Option `or` evaluates its left operand once and emits a one-level None/Some
case; only None evaluates the fallback. `try` uses the same decision, or Ok/Error
for Result, with the remaining expression confined to the success arm. Failure
jumps to the containing function or lambda's return join. Lowering uses exactly
the checked failure and wrapper constructors; the substituted enclosing error
type supplies a wrapper's concrete nominal identity. No wrapper search remains.

Interpolation decodes HEAD/MID/TAIL fragments from the original syntax; only
hole expressions have checked operands. Triple text uses the closing delimiter's
indentation for every fragment. Each hole is evaluated and converted through its
selected Text proof before the next hole is evaluated. The same conversion emits
`to_text` calls and Result entry errors; unresolved generic PARAMETER proofs refuse
E1150 until the checker publishes scoped requirement keys.
Derived text helpers reserve a typed function before following the proof's
parts, memoize concrete type/module pairs with a bounded hash index and drain
a FIFO body queue. Tuple and record fields convert in declaration order; choice
helpers use a one-level case, indexing proofs by flattened case/field order.
Empty constructors print their bare name; other constructors print their name
and fields, retaining declared field labels but not the positional `0` sentinel.
Native FileError and Entry display their path/reason and path/kind labels.
Nested native
Text/Char fields are quoted and escape slashes before newly inserted escapes,
including controls and interpolation markers. Generated parameter metadata
retains arity with absent source names.
Derived Order uses the same reservation queue, keyed additionally by ability.
Tuple/record comparisons inspect parts left to right and continue only on Equal;
choice comparisons dispatch on both constructors, with different cases ordered
by declaration position and matching cases comparing their flattened fields.
Selected custom compare functions replace structural ordering, not equality.
List Order uses a join over the remaining two lists: compare heads once, advance
both tails only on Equal, and order an exhausted list before a nonempty one.
Native Float `compare` orders NaNs after numbers and returns Equal for two NaNs,
while direct scalar comparison operators retain IEEE behavior.

The initial CPS implementation conservatively passes every live outer var,
not only changed vars, across a continuation join; expression joins also pass
their result. Function/lambda return joins are omitted when no return is
reachable outside nested lambdas. Immutable field updates evaluate the new
value first and reconstruct the checked projection path from its original
var. Pattern tests run in written order, with constructors in declaration
order. Each arm has one shared success join taking canonical pattern binders:
alternatives do not duplicate its body or renumber closures. Pattern failure
and false guards both try the next alternative, including the same arm's
remaining alternatives. Guarded groups share one guard join and resume an
index-taking dispatch join after failure. Conditions
compile is/and/implies directly around the guarded continuation, keeping true-
path bindings in scope. Shared nontrivial condition and pattern continuations
become joins; only bounded, atomic leaves are copied. A branch chain therefore
grows linearly at fixed pattern width and never duplicates a lambda body.
Range loops increment only below the saved end;
list/map/set iteration uses uncons after one iterable evaluation/conversion.

Lowering receives the original `mo_sources` as well as the checked program.
It copies filenames, spans, original binding names, spec text and module
exposure into the Core arena, including constants, wrappers, library
instances and lifted-name provenance. Spec/anonymous-function numbers follow
source order before rewrites. Every rewritten body gets exactly one optional
span per Expr in preorder; synthesized nodes inherit the responsible source
construct's span. Generated binders have no source name. Ignored `_` lambda parameter
slots retain their signature type and argument position but have no source
binding or binder name. Repeated SSA/loop
binders retain their original name at distinct node/slot identities.
Lowering supplies correct promise signatures/line correspondence and links
anchors to promises and promises to their ordinary subjects. Module ownership
lists preserve source order even though constants are dependency-ordered.

Compilation needs **only this Core document**. It validates metadata shape,
retains provenance through lambda lifting/optimization, maps binder identities
to register live ranges, and maps expression spans to instruction offsets.
It interns filenames and remaps source-file indices to bytecode string
indices. The image keeps declaration spans, condition text/spans and spec
links as well as exposure, so tools never reconstruct them from checked Mo
syntax. `noSource` compiles with empty source debug tables/no exports; tools
that need absent links report them unavailable (`formal/TEXT.md`).

### 4.4 The text form

Core has a text form, an S-expression for each Lean constructor, defined in
`formal/` and read and printed by both Lean and `src/core`. `mo check
--emit=core` prints a program's Core. This is how a Core program goes from the
C toolchain to the Lean evaluator, and back, and how stream 2 is tested
without stream 1 (section 11). Values have a text form too, for comparing
results: the structural form of reference 9.2, except that a `Float` is
written as its 64 bits in hexadecimal, so the comparison doesn't depend on
how each side spells floats.

### 4.5 What the Lean model covers

The Lean evaluator implements the primitives whose meaning is simple to state
exactly: integers with overflow, `BigInt`, `Decimal` with its rounding,
booleans, characters, text as a sequence of characters, tuples,
constructors, lists, insertion-ordered maps and sets, time and duration
arithmetic. Their results are compared with the VM's in differential tests
(section 11).

The rest are **trusted primitives**, each checked against its own oracle
instead:

| Primitive | Oracle |
| --- | --- |
| `Float` arithmetic and `sqrt` | IEEE 754 itself: the C library's correctly rounded operations |
| `Float`'s shortest text form and parsing | a reference implementation of shortest round-trip printing (Ryu), and `strtod` |
| RFC 3339 times | a table of cases, and a second implementation in Python run in tests |
| UTF-8 decoding of `Bytes` | an exhaustive test over every 1- to 4-byte sequence |
| `Random` | the published reference code of xoshiro256** and SplitMix64, and Lemire's method (section 7) |
| `Unicode16` | the Unicode Character Database 16.0's own test files |

## 5. Bytecode

`bytecode.md` specifies the format, and is the contract between the bytecode
compiler and the VM. In short:

- Every register and field is a 16-byte cell with a static type. Every
  instruction says which of its operands it consumes, and choice values keep
  small payloads inline.
- Functions use register frames on the VM's own stack, with tail calls. A
  closure is an object holding its function and its captured values.
- The verifier proves that a file is well-typed and that its reference counts
  balance. So verified bytecode can't corrupt memory, even if the compiler
  that wrote it is wrong.

## 6. Values, reference counting and reuse

This follows the research brief on reference counting and reuse: precise,
compiler-placed reference counting as in Perceus, reuse as in Lean 4 and
frame-limited reuse, and borrowed parameters as in Lean 4. `bytecode.md`
gives the exact representation; this section gives the reasons.

- **Immediates** are stored in cells directly: numbers, `Bool`, `Char`,
  `Byte`, `Unit`, `Time`, `Duration`, `Range`, `Decimal` (a 128-bit count of
  10⁻¹⁸), and choice values whose cases have no fields.
- **Choice values** with fields keep a tag and an 8-byte payload. A single
  small field is stored inline, so `Some(3)` and `Ok(Unit)` allocate nothing.
- **Heap objects** have an 8-byte header, a 32-bit reference count and a
  32-bit layout. Counts saturate: an object whose count would overflow
  becomes immortal and is never freed.
- **Lists** are arrays with a live start, length and capacity, so `get` takes
  constant time, and `append` on a list with one holder grows it in place, in
  amortized constant time. Owned `concat` reserves the whole suffix before
  copying its elements; prepend, slices, reverse and removal reuse unique
  storage. A shared copy reserves exactly the requested elements, not the
  source's slack.
  Below one quarter capacity, unique shrinking tries an exact buffer and
  transfers retained cells; on refusal it keeps its buffer and still succeeds.
  Prefix drop advances the live start and compacts past half capacity;
  repeated single-holder `drop(1)` or `remove_at(0)` is linear, plus element
  destruction. Prepend reuses a dead prefix cell when available; otherwise it
  moves the live cells. Middle removal still moves the following cells, so
  repeated middle removal or prepending without a dead prefix is quadratic.
- **Maps and sets** are insertion-ordered hash tables: an array of entries in
  insertion order, and an index table of positions, as in CPython's compact
  dictionaries. A removal leaves a tombstone, and the table is compacted when
  tombstones reach a quarter of the entries. Both keep insertion order
  (reference 3.8). Like lists, a map with one holder changes in place, and a
  shared one is copied first (reference 13.3).
- **Text** is UTF-8 bytes with a byte length and capacity, always valid. The
  character count is computed when asked. Owned concatenation grows unique
  storage geometrically and copies just the new suffix between growths; two
  valid texts concatenate without another UTF-8 check. Slack is charged to the
  heap. Unique growth requests twice the needed size (at least 32 bytes for
  Text or four cells for List), bounded by the representation limit. If that
  does not fit the remaining heap after the allocation header, it requests the
  needed size plus half the remaining spare bytes, rounded down to whole cells
  for List. A native `ENOMEM` refusal retries once at the exact needed size.
  Shared Text copies have exactly the needed bytes, regardless of old capacity.
  Unique Text shrinking uses the same best-effort quarter-capacity rule as List.
  Trims and prefix/suffix removal reuse unique storage, and unchanged trims
  keep the original object even when shared or immortal.
- **Reference counting.** The compiler places `dup` and `drop` so that every
  object is freed as soon as nothing can reach it. Parameters are borrowed
  where inference shows the caller keeps them alive (`bytecode.md` 5.4), which
  removes most count updates.
- **Reuse.** When a value is taken apart and a value of the same layout is
  built, the compiler emits `reset` and `reuse`: with one holder, the old
  memory is reused in place; otherwise a new object is allocated. So `var`
  field assignment and `append` in a loop cost what they would with mutable
  data (reference 13.3).
- **No cycles.** Values are immutable; an anonymous function captures values,
  not variables, and can't refer to itself; and the only mutable objects,
  fakes' state, hold only plain data. So no value can reach itself, reference
  counting reclaims everything, and no cycle collector is needed.
- **No recursion in C.** Dropping, equality, hashing and printing use explicit
  work lists. A legal program that builds a million-deep value can't crash
  the VM by dropping it.
- **One thread.** Phase 2's VM runs one process at a time on one thread, so
  counts are never atomic. Phase 3's processes each get their own heap
  (section 9).

## 7. The outside world: capabilities

A capability is a heap object that carries a table of its operations, real or
fake (`bytecode.md` section 7). The VM calls an operation through the object
it is given. It never calls the operating system itself.

- **Real capabilities** are made once, by the runtime, for `main`: exactly
  the ones `main` declares, from the process's arguments, environment and
  start folder (reference 11.2). `mo_call_main` caches each declared kind in
  the process, so a later call keeps the same state and open directory object.
  Its C caller supplies borrowed argument/environment values and a directory
  descriptor; construction retains them before that caller releases them.
- **Fakes** are made by the fake constructors, which only spec code may call:
  `Console.fake()`, `Files.in_memory(...)` and the rest (reference 12.4). A
  fake's state is a mutable object shared by every handle on that fake. So a
  file `main` writes can be read back, and two fakes in one test stay
  separate. That state holds only texts, bytes, lists and maps, which change
  only as programs change values: a test that still holds the map it gave
  `Files.in_memory` keeps seeing the old map (`bytecode.md` section 7).
- **Files.** A real `Files` or `ReadFiles` holds an open handle on its folder.
  Before anything is opened, paths are normalized lexically: `.` is skipped,
  and `..` removes the preceding component. Absolute paths, empty components
  and paths whose `..` would leave the capability's root are refused;
  `a/../b` resolves to `b`. The normalized path is then resolved beneath the
  root, following symbolic links only while they stay inside (D236). Where
  the system has usable `openat2`, `RESOLVE_BENEATH` does exactly that. A
  feature probe treats `ENOSYS`, `EINVAL` and seccomp's `EPERM` as unavailable;
  builds without the kernel headers or syscall number also use the fallback.
  Elsewhere the VM resolves one component at a time with `openat` and `O_NOFOLLOW`;
  when a component is a link, it reads it with `readlinkat`, refuses an
  absolute target, and resolves the target from the link's folder in the same
  way, refusing any step that would leave the root. Either way a path passes
  through at most `MO_LIMIT_PATH_LINKS` links. The capability's folder is the
  directory object held open, not its pathname. Each directory opened during
  a lookup is authorized for that operation; `..`, including in a link target,
  never climbs above that root object. In the portable resolver, popping `..`
  re-resolves the parent from the capability's folder; after another process
  replaces an ancestor, the lookup may reach the replacement, which is still
  inside the folder. This is fresh authorization from the retained root, not
  a walk through an OS parent. Saved component device/inode identities are
  checked during replay; a mismatch is a concurrent change reported transiently
  as `Failed(path, "changed while being used")`, not `NotAllowed`. Inode reuse
  under concurrent replacement cannot portably preserve exact ancestor identity.
  The lookup-wide `MO_LIMIT_PATH_STEPS` budget (262,144 component steps,
  including replay) bounds the extra work; exhaustion gives
  `Failed(path, "MO_LIMIT_PATH_STEPS")`, a limit refusal, not a bug.
  Mo's own resolution never leaves its directory-object authority through
  links or `..`. Another process may rename an intermediate directory during
  the lookup, which may then follow that object to its new location. Preventing
  those concurrent reparentings is outside the portable guarantee; no POSIX
  ancestry precheck or postcheck can make the namespace an atomic snapshot.
  This is directory-object authority, as in Capsicum, WASI, cap-std and Go's
  `os.Root`. `remove` unlinks its last component without following it, so it
  removes a link itself. A refused link gives `NotAllowed`, like any path that
  would leave the folder. Errors map to `FileError`: a missing file to
  `NotFound`, a refused path or permission to `NotAllowed`, invalid UTF-8 in
  `read_text` to `NotText`, and the rest to `Failed`. Three reasons are Mo's
  own, the same on every system and in fakes: "not a folder" where a folder
  was needed, "is a folder" where a file was, and "too many links"; the VM
  checks a path's kind itself rather than trusting `errno`, since systems
  differ, as macOS's `unlink` of a folder gives `EPERM`. Real reads and writes
  refuse a non-regular, non-folder object with "not a regular file" rather
  than reading a device or blocking on a pipe. Linux pins the resolved object
  with `O_PATH` and classifies it before opening it for I/O, then reopens the
  retained descriptor through `/proc/self/fd` with `O_NOCTTY`. It checks the
  reopened descriptor's device/inode against the pinned object before I/O;
  a mismatch gives `Failed(path, "changed while being used")`, including when
  a privileged process has substituted a different `/proc` mount. Without procfs
  it uses the portable resolver. Portable traversal uses search-only directory
  descriptors where the system supplies them (`O_SEARCH`, `O_EXEC` or `O_PATH`).
  macOS headers older than 13 lack `O_SEARCH` and fall back to `O_RDONLY`, so
  traversing a directory there also needs read permission.
  A trailing-slash link target is checked before creating a missing file.
  Outside these kind and native representation guards, failures carry the
  system's message.
  Native resolution uses at most `MO_LIMIT_HOST_PATH_SIZE` bytes, including
  the terminator; an expanded path past it gives `Failed(path, "path too long")`.
  The Linux fast path first resolves without links; if links are present it
  applies Mo's expansion/link limits with the portable walk before pinning the
  result with `openat2`. This is a path-semantics check, not an ancestry check.
  POSIX cannot name a file with NUL, so real paths containing it are refused
  with `NotAllowed`. Fakes keep exact Text bytes, including NUL, as Core does,
  and use charged dynamic path storage rather than the native path bound.
  `list` sorts names by their bytes, which is the order of their characters;
  `entries` classifies children without following their links. An invalid
  UTF-8 name gives `Failed(path, "folder contains a name that is not text")`,
  rather than a lossy name that cannot identify that entry.
  A missing narrowed root retains its parent capability and binds when that
  parent later makes the folder. Every retained parent is older than its
  child, preserving acyclicity. Permission refusal is retryable; only a
  containment refusal permanently makes a narrowed capability reach nothing.
  Once bound, the handle keeps that directory object. Persistent real handles,
  including deferred ones, count against `MO_LIMIT_OPEN_FOLDERS`; portable
  traversal keeps at most three temporary
  descriptors regardless of depth, and closes them when the operation ends.
  At main's startup, `mo_host_start` tries to raise the process's soft descriptor
  limit towards `MO_LIMIT_OPEN_FOLDERS + 32`, never past its hard limit. The raise
  is best-effort: a sandbox may deny it without preventing startup. The existing
  soft limit or a lower hard limit can still refuse a later handle or temporary
  descriptor through the ordinary file error.
- **Console.** Main's startup calls `mo_host_start` once per context, before
  starting threads, to ignore process-wide `SIGPIPE`. C embedders calling real
  Console directly must call it, or arrange their own signal handling and
  descriptor budget. Printing never changes a signal disposition. Writing to
  a closed output returns `EPIPE`, which is a bug with status 70: "printing to
  an output that is gone" (reference 13.2). A real print appends a line feed;
  a fake records the unchanged argument as one list entry. Real output uses
  `writev` for the argument and LF together, so lines no larger than `PIPE_BUF` are atomic on
  a pipe. Non-blocking input and output wait with `poll` on `EAGAIN` and retry;
  a closed stdin is end of input. Real input replaces each maximal
  invalid UTF-8 subpart with U+FFFD and removes LF or CRLF, not a bare CR.
  Reads and line input use the runtime `MO_LIMIT_VALUE_BYTES` representation
  bound, not a source-file size bound. Exceeding it names that limit; charged
  scratch or value allocations exhausting the process heap name `--max-heap`.
- **Random.** `Random.seeded(n)` and the real `Random` use xoshiro256**,
  seeded through SplitMix64. `int(low, high)` uses Lemire's method, which is
  unbiased. So a seeded sequence is the same on every machine, and stays so
  within a major version.
- **Clock.** The real clock reads the system's real-time clock, in
  microseconds, and stops if it is outside the years 1 to 9999.
  `Clock.fixed(t)` always returns `t`.

Host refusal leaves operand cells and ownership unchanged; only a successful
operation consumes the owned arguments in `hostops.def`. Fake updates commit
only after all allocations succeed. Real I/O cannot be rolled back: partial
writes, input consumption, directory binding and earlier `make_folder` steps
may remain after a refusal. The capability still exists for the defect report.

The library is written in Mo wherever it can be (condition 4 of D207). C
implements only what Mo can't express or must be fast: arithmetic and
conversions, text and byte operations, the basic list, map and set
operations, and the capability operations. Every library function that takes
a function, from `map` and `filter` to `sort_by` and `group_by`, is Mo code
in `lib/`. So C never calls back into Mo.

## 8. Running: processes, outcomes and bugs

An **image** is a loaded, verified program. Its **initialization**,
`mo_image_init`, runs the init functions once, in order, before any process
is made. The image keeps the memory they allocate, and every module constant
is immortal and shared by every process (`bytecode.md` section 3.2). A stop
during initialization stops the program before it starts (reference 10.4).
For `mo test` and `mo check`, it fails the file with its defect report.

A **process** is a stack, a heap and the image it runs. `mo_call` runs one
function in a process and returns its **outcome**:

- **returned**, with the result;
- **stopped**, with a defect report;
- or **failed**, from spec code's `fail`, with the failure's details.

A defect report is data: the reason, the span of the instruction that
stopped, the call stack with each frame's function and span, and the stopping
frame's variables in their structural text form: those still readable at that
instruction (`bytecode.md` section 9). `mo_render_defect` turns it
into the text `mo run` prints. When a process ends, its heap is freed all at
once, whatever the counts say. So a stop or a failure in the middle of a
function leaks nothing. Constants live in the image, made immortal, and
every process shares them.

An initialized process can make another `mo_call` after a return. A stop or
failure is terminal: retain the frames and values until rendering finishes,
then free the process. Reports borrow this storage rather than duplicating
every variable at exhaustion. C-owned borrowed arguments must remain alive
while their frames or report can read them. An initialization failure instead
belongs to the image and lasts until `mo_image_free`; a failed image cannot
create processes. Initialization is serial, and its attached runtime owns the
literal cells, global cells, set flags and initializer process. Once attached,
repeated initialization returns the recorded outcome. A failure allocating
that initial runtime or preparing its heap image has no attached state and
may be retried. System entropy failure is a separate internal stop, not a
heap-limit report. Attachment precedes building literals and running init
functions; a later failure remains permanent. Successful initialization frees
its native VM stack; failed initialization retains it for the defect report.

`heap` supplies a separate `struct mo_heap` owned by the VM's process, created
with `mo_heap_new(image, limits)`. Execution frames, loading and outcomes
remain `vm`'s work. A shared `struct mo_heap_image`, prepared once per run,
borrows finished types and holds abilities, the sorted layout index, secret
hash/index domains and its initialization heap. Layouts may be appended after
types finish; refresh before publishing the image for parallel execution.
New process heaps perform no entropy reads, type walks or layout sorting.
Making a module constant immortal computes reachable structural hash caches
before freezing the whole immutable graph, even for a saturated root. It can
fail for memory without changing counts; published image caches never change.
Processes never free image allocations or cache captureless closures locally.

Each process heap limit counts its process and fixed heap contexts, native allocation
prefixes, value storage and temporary walk arenas, including chunk overhead.
The image budget counts runtime cells/flags, its initializer context, shared
indices and its initialization heap together. Host preparation copies the
sealed type table and adds any absent private Int, Time, Text, List(Text),
Set(Text) and Map(Text, Text) types for capability storage; original indices
and layouts never change. That stable table is charged to the image budget
and shared by its heaps, not allocated once per process. Native stack reservation is
charged separately to the stack limit, including growth slack.
Caller-owned types and output buffers are outside these limits, as is system
allocator bookkeeping. Scratch uses the remaining heap and is reclaimed after
each generic operation; equality, ordering and hashing have no separate work
cap. The current charged prefix and actual tuple cost are in bytecode 2.4.
Heap teardown releases native handles as well as values; release callbacks
cannot inspect children, which teardown may already have reclaimed. Printing
uses a small C-stack frame buffer before requesting scratch, so scalars and
shallow values remain printable at a full heap. A refusal preserves output
and lets the defect renderer use `<value unavailable>` instead of another stop.

- **`mo run`** initializes the image, makes a process and calls the entry
  function (4.3). It exits with `main`'s status. On a stop, it prints the
  defect report to standard error and exits with status 70. The verifier
  proves that `main` can't reach spec code (`bytecode.md` section 9), so a
  failed assertion can't happen there. If one ever did, it would be treated
  as a stop.
- **`mo test`** and `mo check` run each test, each example line and each
  call of a promise's body in a fresh process. A stop or a failure is
  reported, and the run goes on to the next.

Frames live on the VM's own stack. The stack's size, the call depth and the
heap's size have limits, and exceeding one is a bug with a report, not a
crash. A tail call reuses its frame, so a loop written as tail calls runs in
constant stack (reference 5.3); a callee with more registers grows that frame
(bytecode 5.2).

## 9. Hooks for later phases

- **Processes (Phase 3):** the VM's state, its heap, stack and current
  function, lives in one `mo_process` structure from the start, so a scheduler
  can run many. Messages between processes copy values, or share them when
  the value is deeply immutable and its count is atomic, as in Lean 4's
  multi-threaded objects.
- **Introspection (Phase 4):** debug information maps every instruction to its
  span and every register to its source name, and every value has a
  structural text form, so a running process can be shown in source terms.
  Every capability operation passes through one dispatch point, where Phase 4
  adds recording for replay.
- **Live upgrades (Phase 5):** the module table lists each module's exposed
  functions and types, and the type table gives what the upgrade checker
  compares. An instantiation of a generic function belongs to the module that
  asked for it (4.2), so swapping a module knows which instantiations to
  rebuild. Calls between modules are direct in Phase 2; Phase 5 adds the
  indirection it needs.
- **In-place demands (Phase 7):** the in-place check of D26 runs on Core, where
  the full-3c checker's rules apply.

## 10. The tools

- **`mo check`** loads, checks and lowers the program, and evaluates the
  promises' anchors. It prints diagnostics as text, or as JSON with `--json`
  (10.2). `--emit=core` and `--emit=bytecode` print the program's Core or
  bytecode in their text forms.
- **`mo run FILE [ARGS...]`** checks the program whose entry module is
  `FILE`, including the anchors, as `mo check` does, then runs its `main`
  with the arguments after the file name. The image it runs is the one the
  anchors ran in. Its spec code is there, but `main` can't reach it.
  Bytecode has no entry field: the tool knows the entry module from loading
  source and finds `<Module>.main` by full name in the function table.
  Lowering always produces that name. `mo_compile` requires Core's entry to
  exist, be an ordinary function, and have a unique full name in the image
  (from compiler metadata, or `module + "." + name` for `noSource`).
  It accepts other entry names for Core tools and differential tests.
  Core's checker establishes the first-match ordinary entry; E1200 reports a
  lowering bug if any other function, before or after it, has the same full name.
- **`mo test`** runs every example, every test, and every promise against
  generated inputs, or those that `--only` and `--in` choose by the names of
  `bytecode.md` section 3.5. `--list` prints the names, and `--json` gives
  the results as 10.2 says.
- **`mo format`** and **`mo fix`** rewrite files in place (3.1, 3.5).
- **`mo guide`** prints `docs/agent-guide.md`, and `mo guide --reference`
  prints `docs/reference/README.md`, both built into the toolchain so their
  text matches its version (D235). The guide is short, about 2,000 tokens:
  what the reference can't say, such as the commands, the loop of checking,
  fixing and testing, and one small complete program with its tests.

**Limits.** `mo run` and `mo test` give each process the limits of
`struct mo_limits` in `src/base/limits.h`, whose defaults are the defaults of
`--max-stack`, `--max-depth` and `--max-heap` (reference 13.4). The VM counts
frames for the depth, so a tail call, which reuses its frame, never counts.
Going past a limit stops the process with a defect report that names the
limit and its value. A program can't set its own limits until Phase 3's
budgets subsume them.

**The `mo` command's own exit status** is 0 when everything passed, 1 when
the program has errors or a test failed, and 2 for a mistake in the command
itself. `mo run` exits with the program's own status once the program runs
(reference 13.4).

**Command grammar (stream 4, part 1).** No command is a command mistake.
`--version` (also `-V`) stands alone. Options may precede or follow files,
except in `run`: the first file ends option parsing and every remaining
argument, even an empty string or flag, belongs to the program. `--` ends
option parsing and permits file names starting with `-`. Options with values
take a separate nonempty argument; names cannot start with `-`, and numeric
values with signs are invalid limits. Only `--emit=core` and
`--emit=bytecode` use `=`. Unknown flags, flags on the wrong command,
duplicate flags (except `--only`), missing/extra files and invalid limits are
command mistakes. A mistake writes one newline-terminated message to standard
error, nothing to standard output, and exits 2, without extra usage text.
The message identifies the offending argument (both flag and value for an
invalid limit). Argument excerpts quote up to 256 bytes, escaping controls,
non-ASCII bytes, quotes and backslashes as `\xHH`, with `...` for longer
arguments; the whole message fits 4 KiB. Missing arguments name their command
or flag.

| Command | Files | Accepted flags |
| --- | --- | --- |
| `check` | one entry file | `--json`, `--emit=core`, `--emit=bytecode` |
| `run` | one entry file, then program arguments | `--max-stack`, `--max-depth`, `--max-heap`, before the file |
| `test` | one entry file | `--json`, repeatable `--only NAME`, `--in MODULE`, `--list`, `--max-stack`, `--max-depth`, `--max-heap`, `--calls`, `--seconds` |
| `format`, `fix` | one or more files | none |
| `guide` | none | `--reference` |

`--json` and `--emit` cannot be combined. Test selections may combine:
`--only`'s names are intersected with `--in`'s module. `--list` accepts
budgets but will not consume them. Matching is deferred until the test stage
exists; the current stub never claims that a selection matched. Limits are
positive ASCII decimal integers without signs, spaces or fractions; leading
zeros are accepted. Byte sizes accept bare bytes or the exact suffixes `B`,
`KiB`, `MiB`, `GiB` (powers of 1024); counts and seconds accept no suffix,
and seconds are whole seconds. Overflow is a command mistake: bytes fit
`size_t`, depth/calls/seconds fit `uint32_t`. Defaults are 8 MiB stack,
10,000 frames, 1 GiB heap, 200 calls and 2 seconds. `base/limits.h` bounds
Mo's own options and their operands to 256 words, 4,096 bytes per word and
64 KiB total including terminating NULs. The command name is also
length-bounded, but neither it nor the program name consumes the option
budgets. File lists and forwarded program arguments are borrowed from `argv`,
not copied or capped by Mo: the OS's argument limits bound them, and the
program's heap will bound arguments converted to Mo values. `mo_cli_file`
indexes borrowed file lists across an optional `--` separator.

**Current integration boundary.** `check` calls `mo_load_program`,
`mo_resolve_modules`, `mo_prepare_signatures`, then `mo_infer_function`
for every named function, retaining independent body errors. Each stage
stops before the next on errors, including dropped diagnostics; body iteration
stops on a resource error, but continues past independent source errors and
unfinished forms, even after the output retention cap. Each body reports into
its own bounded diagnostics, copied into the capped run's set and then freed;
this keeps counting dropped errors without making the checker stop because
the output is full. Its shared arena is bounded to 512 MiB (E1054 while parsing,
E1058 while checking on exhaustion).
Status 0 means only that
these available stages found no error: constants, specs and the other rules
listed in `check.h` are not whole-program checked. The seam in
`tools/execute.c` will replace staged inference with `mo_check`, then
connect `mo_lower`, `mo_compile` and anchor evaluation; it never passes
inference results to lowering. `run`, `test` (including `--list`), `fix`
and `--emit` currently validate arguments then return status 1 with
E1400, "this build cannot ... yet", without reading or rewriting files.
E1400 is the tools' unavailable-stage code, not a language error. Source work
runs synchronously through `mo_with_stack` with `MO_SOURCE_STACK_SIZE`, covering
loading, parsing and every checking stage in one pipeline callback. Lowering,
Core checking and code generation must also be wired inside that callback, not
after it returns: their accepted depths need more than a default C stack.
The callback joins before rendering or freeing its borrowed arenas and
diagnostics. It changes no
process-wide resource limits and follows changes to the shared stack allowance.
Failure to prepare that stack reports E1401 with an unavailable location and
the POSIX error as its label, in either text or JSON, and exits 1.

**Atomic formatting.** `format FILE...` handles files independently and in
argument order. Loading one file, parsing and printing it do not retain the
previous file's tree. Parsing and printing run synchronously through
`mo_with_stack(MO_SOURCE_STACK_SIZE)`. A parse error, printing refusal or file
error leaves that file unchanged, reports text diagnostics on standard error
and sets status 1; later files still run. Success is silent, with status 0.
The batch shows at most 1,000 diagnostics and counts all omissions.

Only ordinary regular files are accepted: symlinks, directories, FIFOs and
devices are refused before reading. Changed bytes are written to a sibling
`mkstemp` file, flushed, given the original ordinary permission bits, synced
and closed, then renamed over the requested path. Failed writes or renames
remove the temporary. A replacement inode discovered before rename is
refused, but this is not locking against concurrent in-place edits. Unchanged
files keep their inode and modification time; reading may advance access time.
Renamed files break hard-link sharing and do not preserve ownership, ACLs,
extended attributes or special mode bits.
The rename is atomic for readers, not a promise of directory durability after
a power failure. E1402 reports file access/replacement failures; E1403 reports
source-stack, document, recursion, output or source-line budget refusals.

**Source builds and documents.** `ci/embed_docs.py` (Python 3, portable to
macOS) generates byte arrays under `build/generated/tools/embedded.h`.
Ordinary Make prerequisites regenerate only when a document or the script
changes; mutually exclusive present/absent stamps also detect the optional
guide appearing or disappearing, regardless of its timestamp. A missing state
stamp forces generation and recompilation of the document consumer for that
invocation, even on coarse-timestamp filesystems; once created it is an ordinary
file prerequisite. After successful generation the header's timestamp advances
even if its bytes are unchanged,
so up-to-date builds don't invoke or require Python. Each document is bounded
to 4 MiB at generation. `guide --reference` writes the reference bytes exactly.
For `guide` only, a broken pipe (`EPIPE`) succeeds silently because the reader
chose to stop; other output errors exit 1. A missing or empty agent
guide returns 1 on standard error and points to `--reference`; no guide is
created. The generated header records the checkout's absolute `lib/` path.
A missing `lib/` means no library yet; an existing unreadable library is
reported by the loader. Rebuild after moving the checkout. There is no
working-directory or environment search and no installation layout yet.
Embedded documents remain usable after moving the executable.

**Text presentation.** Diagnostics and omission notices go to standard
error, with no success chatter. Headlines include the code and primary
`path:line:column`; secondary spans name their own locations. Source rows
use an ASCII `|` gutter. Locations count original characters as `diag.h`
specifies, not display cells. Tabs expand to four-cell stops in the excerpt.
The command sets only `LC_CTYPE` from the environment. When POSIX
`nl_langinfo(CODESET)` identifies UTF-8, valid UTF-8 stays as written,
including in paths, messages and fixed lines. Other encodings, or an
unavailable requested locale, use ASCII escapes for every non-ASCII scalar
in text diagnostics. Numeric parsing remains independent of the locale.
Only controls, DEL, invalid bytes and the invisible/direction-changing scalars
refused by reference 2.1 are escaped in UTF-8 locales: ASCII controls and
invalid bytes use `\xHH`, other refused scalars use `\u{hex}`. Tabs remain
expanded spaces.
Carets and clipping use `mo_unicode16_display_width` and
`mo_unicode16_next_grapheme` from `prim/prim.h`,
the same grapheme-aware Unicode 16 estimate as the library primitive;
tools copy no Unicode tables and use no locale-dependent width APIs.
Validated safe UTF-8 runs are traversed once by complete grapheme clusters.
For drawing, both span ends snap outward to cluster boundaries; a zero-width
span still receives at least one caret. JSON's byte offsets and original
character columns do not change. Clips never split a cluster or re-pair flags.
Long source rows show a window of at most 160 cells and 4,096 excerpt bytes,
including `...` elision markers but excluding gutter and label. A cluster
that does not fit is omitted whole. Left context retains up to twelve complete
clusters and 1,024 source bytes; a giant preceding cluster cannot hide the
focus. Scanning and measuring take linear work, including zero-width runs;
there is no prefix-width binary search or repeated whole-suffix scan.
Long multi-line spans show the first two and last two rows. EOF insertions get
one caret, even after a final newline. Reasons already contain the section
citation; codes without a section print no reason. Fixes show their titles
and fixed lines after applying all original-text edits simultaneously,
sorted by file and offset, with insertions before replacements at a shared
start. Rendering never changes files. Unavailable locations display
`<unavailable>` once in their header; a label, if any, follows on its own row.

### 10.1 Generating inputs for promises

`mo test` checks a promise's body on many generated inputs, boundaries first
(reference 12.2). These are the rules:

- **Boundaries.** Each type has a list of values to try first: for `Int`, 0,
  1, −1 and the two extremes; for `Decimal`, 0, the smallest step, ±1 and the
  extremes; for `Text`, the empty text, one ASCII character, one character
  outside ASCII, and a long text; for lists, maps and sets, empty, one
  element, and several; every case of a choice type; for `Time`, the first and
  last instants of the years 1 and 9999, and the epoch; for `Duration`, 0 and
  the extremes. The boundary inputs are the combinations of the parameters'
  boundaries, up to a limit.
- **Random inputs** follow, from a generator seeded by the promise's full
  name, not its subject's, so every run of `mo test` tries the same inputs.
  Sizes grow as the run goes on. The generator hashes with its own fixed key,
  not the VM's, which changes from run to run (`bytecode.md` section 8).
- **Hidden types.** A type exposed without its insides is built only through
  calls to its module's exposed functions that return it, in sequences of up
  to four calls from generated arguments. A builder that returns
  `Result(T, E)` or `Option(T)` counts when it succeeds.
- **Functions** are generated as constant functions, the identity where the
  types allow it, and functions that derive their result from their
  argument's structural hash, so equal arguments give equal results. In
  `mo test`, the compiler emits these three as `closure` functions for every
  function type a promise's parameters use, and the generator makes closures
  of them.
- **Capabilities** are fresh fakes with generated contents.
- **The budget** is 200 calls of the function per promise, boundaries
  included, and 2 seconds; both can be changed with `mo test --calls` and
  `--seconds`.
- **When the body is false**, the input is shrunk: numbers toward 0, texts and
  collections toward fewer elements, records field by field, for at most 200
  more calls. The report shows the smallest failing input, the result, and
  which line of the body was false. A call that stops is reported as a bug,
  with its input (reference 12.2). When the input holds one of its type's
  extremes, the report names the parameter and the extreme, as in "stops with
  overflow when `amount` is `Decimal`'s largest value", and shows the two
  fixes in words, since only the program's author knows which is right:
  return a failure for such inputs, or take a type whose module keeps its
  values in range (D234).

**The input library, part 1.** `tools/inputs.h` takes the sealed image type
table and parameter indices. It constructs owned cells through `heap`'s public
builders, without calls or timing. The caller retains inputs while testing
and supplies borrowed current inputs to the shrinker. Shrink candidates are
deep copies in the destination heap, which may be a fresh process's heap;
no reference into the old process is retained. The caller tests a candidate,
then accepts it as the new failing input or drops it. The shrink call budget
counts emitted candidates. Numeric shrinking tries zero first. Decimal then
halves the coefficient's decimal digit count; BigInt shifts away half its
magnitude bytes; Float reduces its exponent before its ordered magnitude bits.
Integer, Decimal and BigInt subtract distances `abs(x) / 2^k` toward zero
for increasing `k`, ending at one unit; Float uses the same distances on its
exponent and then magnitude bits, ending at one ULP. Acceptance resumes at
that node and distance within a pass; structural edits restart at the accepted
node. A pass that accepted anything starts again at node zero with fresh
distances, because changes to later nodes can make earlier rejections obsolete.
Shrinking stops when a pass accepts nothing or the call budget runs out.
Equal scalar candidates are skipped.
The driver, not this library, decides whether a stopped call is a bug.
Extreme detection walks a parameter and returns its first Int, Decimal, Time
or Duration minimum/maximum, including Range's Int endpoints, start before
end; overrides are opaque.

Boundary order starts with the base tuple, then varies each parameter alone
in declaration order, so a capped product does not hide later boundaries of
early parameters. The remaining Cartesian tuples follow, last parameter
fastest, without repeating the base or one-parameter tuples. A cap below
`1 + sum(boundaries_per_parameter - 1)` truncates even this initial coverage.
Records and tuples start with minimum fields,
then vary one field at a time. Choices try every case with minimum fields,
then field variations. Minimum inhabitants use the shallowest terminating
choice case. Recursive back-edges add one non-minimum shell rather than an
infinite expansion. Collections try zero, one and three boundary-derived
elements; map/set deduplication can leave fewer distinct entries. Long Text
is 1,024 ASCII `x` bytes; its short non-ASCII boundary is `é`. Time starts
with the epoch, then the first and last microseconds of year 1 and year 9999.
Other scalar boundaries include Bool's two values, Byte's 0/1/255, Char's
surrogate-adjacent endpoints, Float's signed zeros, finite extremes,
infinities, NaN and smallest subnormals, Range's empty/forward/reversed bounds,
Bytes' empty/one-byte/1,024-byte values, and BigInt's 0/±1/±128-byte magnitudes.

SipHash-1-3 in `base/hash.h` hashes the full name with the public fixed key
`6d6f2d70726f6d69:73652d696e707574`. The same key hashes the little-endian
16-byte pair of name seed and ordinal into each SplitMix64 starting state;
adjacent inputs do not share shifted portions of a stream. Random sizes
increase every four inputs, up to the collection-size budget. Shape and blob
size divide by the number of children plus one on each descent, bounding
nested branching. Immediate integer magnitudes still grow with the trial's
global size. Random Decimals choose decimal digits and a fractional scale
from 0 through 18, with overflow-checked growth into the representation's
range; they do not stay near zero as raw small coefficients would.
An allocation or generation-limit refusal preserves outputs and the ordinal;
the caller may retry or explicitly skip it without changing later streams.
Empty parameter lists have one input. Explicit-stack walks are bounded by
`MO_LIMIT_INPUT_*`, with smaller caller budgets: defaults are depth 64,
4,096 nodes, 1,024 entries per collection, 64 KiB total blob payload and
1 Mi work units per input. Preparation has a separate 64 MiB arena and
64 Mi work budget. Generic table construction reserves conservative work
proportional to this table's child nodes and bytes for hashing and bounded
paired collision/equality walks before entering the heap builder, not the
whole input's cumulative size once per entry. Uninhabited root types have
their own refusal. Uninhabitable or too-deep choice cases are excluded;
collections with uninhabitable elements still generate empty values.
Functions/env/capabilities without
overrides, missing layouts and non-hashable keys are unsupported. Exact-index
factory overrides are the future hidden-builder, closure and fake-capability seam; their trusted
callbacks subtract child nodes, bytes and work from the same remaining budget
and rebuild foreign cells instead of retaining them. Hidden visibility and
the choice of factories remain with the driver. None of these later builders
or the `mo_call` driver is implemented in part 1.

### 10.2 JSON output

`mo check --json` and `mo test --json` write JSON Lines to standard output:
one object per line, written as soon as it is complete, so a run cut short
leaves what it found. Each object's `record` field says what it is. Nothing
else goes to standard output: anything else, such as progress, goes to
standard error. A mistake in the command writes only its message, to standard
error, and exits with status 2. The format keeps reference 13.4's promise: within a major version, fields
keep their names, types and meanings, and codes their meanings; fields and
records may be added, and readers ignore what they don't know.

- **`version`**, always first: `format`, the format's version, `"1.0"`; `mo`,
  Mo's version; and `command`, `"check"` or `"test"`.
- **`diagnostic`**, one per error, sorted by position (3.5): `code`, such as
  `"E0412"`; `headline`; `reason`, and `section`, such as `"8.3"`, both
  `null` for the codes of limits, which cite no section; `primary`, a labeled
  span; `secondary`, a list of them; and `fixes`, the alternatives, each a
  `title` and its `edits`, applied together, each edit a `span` and the `text`
  that replaces it.
- **`test`**, one per example, promise and test, written by `--list`: `name`,
  `kind` and `span`, as in `result`.
- **`result`**, one per example, promise and test run: `name`, its full name
  (`bytecode.md` section 3.5); `kind`, `"example"`, `"promise"` or `"test"`;
  `span`; `outcome`, `"passed"`, `"failed"` or `"stopped"`; and
  `microseconds`, how long it took. A failed example, or a failed test whose
  `assert` compares with `==`, adds `left` and `right`, its two sides' values;
  a failed test adds `line`, the span of the `assert`. A failed promise adds
  `input`, the smallest failing input, as each parameter's `name` and
  `value`; `result`, the call's result; `line`, the span of the line of the
  body that was false; and `calls`, how many calls were tried. A stopped one
  adds `defect`, the defect report of section 8: `reason`, `span`, `stack`,
  each frame's `function` and `span`, and `variables`, each a `name` and
  `value`; and for a promise, the `input` it stopped on, and `extreme`, when
  that input holds one of its type's extremes: the parameter's `name` and the
  extreme's `description`, such as `"Decimal's largest value"`.
- **`totals`**, always last: `errors`, the diagnostics written, and
  `dropped`, those found past the limits a run keeps
  (`MO_LIMIT_DIAGNOSTICS`); for `mo test`, `passed`, `failed` and `stopped`;
  and `status`, the status `mo` exits with.

A **span** is `file`, the path of the file; `start` and `end`, byte offsets
into it, `end` excluded; and `line` and `column` where it starts, and
`end_line` and `end_column` where it ends, counted from 1, a column counting
characters as messages do (`src/base/diag.h`). A labeled span adds `label`. A
**value** is a string holding its structural text form (`bytecode.md` section
8), since JSON's numbers can't hold every `Int`, `Decimal` or `Time` exactly.

**Bounded records and unavailable spans.** Runs retain at most 1,000
diagnostics and 64 MiB of diagnostic storage (`base/limits.h`). Each rendered
diagnostic including escaping and newline fits 1 MiB
(`MO_CLI_DIAGNOSTIC_SIZE`); text fix-preview scratch also fits 1 MiB. A
diagnostic that cannot fit is omitted whole and added to `dropped`, in
either presentation. Fields are never truncated to manufacture a record.
Errors or drops imply status 1; `errors` counts records actually written.
Output failure exits 1 too; an output that refuses bytes cannot be promised
a totals record. SIGPIPE is ignored so broken pipes take this failure path.
Each complete record is written and flushed immediately. Version precedes
loading; diagnostics follow the stages in one globally position-sorted
batch; totals come last. Text is likewise flushed per diagnostic.

An unavailable span (`file == UINT32_MAX`, or no retained source) keeps
its `start` and `end`, but uses `null` for `file`, `line`, `column`,
`end_line` and `end_column`; absent labels and fix titles are `null`.
JSON strings preserve valid UTF-8 scalars. Quotes and backslashes are
escaped; controls, DEL, C1 and the reference 2.1 refusal set use JSON Unicode
escapes (`\uXXXX`, or a surrogate pair for supplementary scalars), so decoded
content is unchanged but the encoded record cannot carry terminal controls.
Each invalid UTF-8 byte uses `\u00HH`. JSON is UTF-8 regardless of locale.
Invalid bytes thus become the corresponding U+00HH characters rather than malformed
UTF-8; offsets still refer to unmodified source bytes. This is not a
lossless path-byte codec when valid U+00HH also occurs; consumers must check
decoded paths before rewriting files. Limit/implementation codes have both
`reason` and `section` null, even when the stage supplies explanatory reason
text; their explanation belongs in headline/label. Test stubs produce
version, E1400 diagnostic and totals with all test counts zero.

If `count` in `examples/wc.mo` miscounted words, `mo test --json
examples/wc.mo` would write these lines, with the four results that pass
among them:

```json
{"record":"version","format":"1.0","mo":"0.1.0","command":"test"}
{"record":"result","name":"Wc.count.example.3","kind":"example","span":{"file":"examples/wc.mo","start":453,"end":513,"line":16,"column":5,"end_line":16,"end_column":65},"outcome":"failed","microseconds":41,"left":"Counts(lines: 0, words: 1, bytes: 10)","right":"Counts(lines: 0, words: 2, bytes: 10)"}
{"record":"totals","errors":0,"dropped":0,"passed":4,"failed":1,"stopped":0,"status":1}
```

## 11. Testing strategy

| Layer | How it is tested |
| --- | --- |
| Every C module | unit tests in `tests/`, run plain and under AddressSanitizer and UndefinedBehaviorSanitizer (condition 1) |
| Reference counting | a debug heap under `make sanitize` that records every allocation, poisons freed memory, and checks at each process's end that every object it didn't make immortal was freed |
| Parser | the executable grammar's tests, and a fuzz target that checks the parser and `grammar/mo.y` accept and reject the same inputs |
| Checker | a corpus of programs with the exact diagnostics they must produce, one file or more per error code |
| Lowering | the twelve samples and the corpus, lowered and compared with their Core text, which the Lean evaluator runs to the samples' expected results |
| Core and the VM | **differential testing**: Core programs, hand-written, from the samples, and generated well-typed, run on the Lean evaluator and through the C compiler and VM; any difference in outcome is a bug, and a minimized corpus of them runs in CI, as in AWS Cedar (research brief on verification-guided development) |
| Compiler | every Core program the Core checker accepts compiles to bytecode the verifier accepts: a property run on every Core program above |
| Verifier and loader | arbitrary and mutated files; disassembling then assembling gives the same bytes |
| Library | `mo test` on the library's own examples, promises and tests; the trusted primitives against their oracles (4.5) |
| Small critical C functions | CBMC proofs of memory safety and of no overflow: checked arithmetic, Decimal's products and quotients, UTF-8 decoding, path resolution, hash-table probing, reference count updates |
| The whole toolchain | a golden corpus of programs with their expected output and exit status, and Phase 2's agent benchmark |

**Fuzz targets** (condition 2), each with its invariant:

- the parser against the grammar: same verdict;
- the checker on mutated programs: diagnostics only, never a crash or a failed
  assertion;
- the Core checker on mutated Core text: the same;
- the verifier and loader on arbitrary and mutated bytecode: refuse, or load
  and run without a sanitizer error;
- the VM on generated valid bytecode, with a step budget: an outcome, never a
  crash;
- the text parsers `to_int`, `to_decimal`, `to_float`, `Time.parse` and
  `decode_utf8`: agree with their oracles;
- path resolution in the host: never resolves outside the folder;
- the formatter: formatting twice gives the same text, and keeps the tree.

Every check runs through `ci/check.sh`, in the CI orb on request (D214). The
Lean build (`make formal`) and CBMC need elan and CBMC in `.agents/setup`.

## 12. Phase 2's streams and their interfaces

Phase 2 starts with the contracts as code, then splits into four streams that
build in parallel.

| Stream | Builds | Consumes | Produces | Tests itself with |
| --- | --- | --- | --- | --- |
| 0. Contracts (first) | `bytecode.h`, `types.h`, `heap.h`, `prims.def`, `hostops.def`, `core.h`; Core's text form in C and Lean; in `formal/`, what 4.1 lists as missing, and `Random` as section 7 defines it | the design documents | the four contracts | round trips of the text forms; the Lean build |
| 1. Front end | `syntax`, `check`, `lower` | source files | Core, or diagnostics | the grammar, the diagnostics corpus, and Core text run by the Lean evaluator |
| 2. Compiler | `core`'s checker, `codegen` | Core | bytecode, through `src/bytecode`'s writer | the verifier, and the property that checked Core compiles to verified bytecode |
| 3. VM | all of `src/bytecode`: writer, reader, verifier, assembler and disassembler; `heap`, `prim`, `host`, `vm` | bytecode | outcomes | hand-written bytecode in the text form, with golden outcomes; then the differential tests |
| 4. Library and tools | `lib/`, `src/tools` | the other three | the `mo` command | `mo test` on `lib/`, and the golden corpus |

The headers each stream codes against, in outline. Every pointer a function
returns belongs to the arena it was given, unless its comment says otherwise.

The complete contracts in the headers win over this outline. Types and layouts
have per-kind wire encodings in `bytecode.md` section 3, never native C struct
bytes. Source signatures have at most 255 parameters (E1009); bytecode has
256 slots so a promise can add its result and closure code can add `env`.
Stream 3 implements the writer, text tools, image accessor and C value APIs
declared below. The image retains file indices in every decoded and runtime
table, so streams 2 and 4 use the same indices in calls and value builders.

`src/prim/prim.h` is the VM's primitive seam. `mo_prim_call(heap, types, id,
instantiation, arguments, out, reason_out)` takes the verified function-type
index, including `fn() : T` for a zero-argument constant. Entry sets and checks
`FE_TONEAREST`, also used by subsequent VM arithmetic. The id is the
unchanged zero-based row of `base/prims.def`, declared in `base/prims.h` so
`prim` need not depend on `bytecode`. A typed status distinguishes a result,
overflow, division by zero, an outside-domain call, a repeated map-literal key,
and allocation/size refusal. All primitive rows are implemented. The static
reason records the arithmetic type, outside-domain operation or representation
limit for the VM's defect report.
Only a returned result transfers ownership: the result is owned, the `o`
parameters are consumed, and the `b` parameters stay readable. All refusals
preserve every operand, its object and ownership, and the output cell, so the
VM can render pre-instruction registers without snapshots. Single updates use
the heap's transactional unique/shared paths. Text and list concatenation
reserve before mutation; shrinking or reordering a unique value cannot fail.
Lists track a live start in former private-header padding, with the same
56-byte prefix and unchanged object/wire layouts (bytecode 2.4). Slicing a
single-holder list advances that window, releasing discarded references;
compaction past half capacity and best-effort contraction below one quarter
make repeated `drop(1)` or `remove_at(0)` linear in the input size, plus element
destruction. Shared/immortal copies contain only the live window. Append uses
tail slack; prepend can reuse a dead prefix. All generic walks, printing and
VM/host list views use the same live-element accessors. The million-element
primitive and compiled uncons benchmarks run in the work gate. Compiled
single-holder traversal combines codegen's last-use transfer with the live
window; neither change alone removes repeated suffix copies or moves.
Set union plans missing keys and insertion slots before any mutation or count
change, using scratch proportional to the right set and a sparse overlay of
new slots rather than copying the left index. A singleton union uses the
existing transactional insertion. Shared/immortal values are copied before a
change; growth/rehashing builds an index privately, then commits ownership.
Other multi-step operations build a private result. Native walk/build storage is
charged to the process through `mo_scratch`; structural formatting uses the
heap printer's separately bounded caller-owned output buffer.

Primitive parts 1 and 2 implement every row. Time/Duration use microseconds,
the heap's formatter and the UTC instant's inclusive years 1 through 9999.
The parser follows `Support/Time.lean`, including offsets, lowercase `t`/`z`
and one to six fractional digits; an offset may move local year `0000` into
valid UTC year 1. `checked_add` returns `None` outside the time range; Time
arithmetic instructions stop with the bug required by reference 3.9.
The C/Lean differential gate covers every non-Unicode row, exact Float bits,
all four Number instances of sum, and exact stop details, with shared and unique
operands and multi-step collection updates. `%structural` uses the reference's
text form, including shortest decimal Float spellings and escaped nested
interpolation markers; Core's debug observation mode still exposes Float bits.
Model-only bridges in `formal/Main.lean` compose existing model operations for
lowering's order, literal insertion, sum and text-form helpers; they do not
change `Prim.lean`.
Long text matching uses KMP. BigInt multiplication uses explicit-stack
Karatsuba; division uses normalized base-2^32 long division (a single pass for
a one-limb divisor), with scratch bounded by operand/result sizes and the heap.
Reference 13.3 promises no constant or linear cost for BigInt arithmetic.

Unicode16 is trusted, not reimplemented in Lean. `ci/unicode16/` holds the
compressed unmodified UCD 16.0.0 sources, their SHA-256 hashes and license;
`ci/generate_unicode16.py` generates the compact range/mapping tables.
CI checks reproducibility and every grapheme/NFC test vector, full case data
and final-sigma contexts. Graphemes use the untailored extended rules of
UAX #29 revision 45. The width policy is explicit in `library.md`.
`prim.h` exposes `mo_unicode16_display_width(bytes, length, width_out)` for
diagnostics and formatting without constructing a heap Text. The primitive
calls this same implementation, so all three users share one policy. The
byte span is borrowed and readable; NULL is allowed only for an empty span,
and the output pointer is required. Malformed UTF-8 or `uint64_t` overflow
returns false without changing the output. Success writes the total width.
The call allocates nothing, needs no heap, uses constant stack space and
visits each scalar at most twice. It has no source-file or runtime value cap.
CI compares it with the primitive on every UCD runner input and every Text
generated by the primitive fuzzer; raw fuzz inputs also check refusal safety.
`mo_unicode16_next_grapheme(bytes, length, length_out)` exposes the same
boundary routine as `Unicode16.graphemes`, without allocation or tables in
tools. Its input must be valid UTF-8 starting at a cluster boundary. It
validates only the first cluster and one lookahead scalar, refusing an empty
span or malformed bytes it encounters without changing the output. It does
not validate the unvisited suffix. Repeated calls take linear work and
constant stack. CI compares every split with the primitive on all 1,093
GraphemeBreakTest vectors and generated fuzz Texts; the work gate also splits
a million ASCII characters under the existing 20-second ceiling.
Property, mapping and composition lookups are logarithmic in their table
counts, with direct ASCII properties and algorithmic Hangul handling. Each
scalar is visited a bounded number of times: segmentation keeps suffix
states, sigma lookahead runs do not overlap, and canonical ordering uses
stable counting sort on long combining runs (insertion sort only up to 32
scalars). Decompositions expand to at most four scalars, case mappings to
three. Scratch derives from input/output sizes and shares the process heap
budget. Output respects the runtime `MO_LIMIT_VALUE_BYTES` representation
ceiling and collection limits, not the source-file limit. Allocation refusals
name `--max-heap`; any refusal preserves operands and output just like part 1.

**Core's checker.** `mo_core_check` accepts complete parsed or arena-built
Core trees, not arbitrary pointers or invalid C enum tags. Its typing contract
is the executable `formal/MoCore/Check.lean` checker, including declaration
kinds, constant prefixes, spec code, capability and fake signatures, and the
ordinary entry. It also requires `mo_core_metadata_valid`; metadata has no
typing meaning, but its shape must be safe for code generation.

The executable checker is sound for the typing relation, not a complete
decision procedure for it. In particular, tuple parts, let bounds, callees
and scrutinees must infer their types even when the surrounding expression
has an expected type; `stop`, `fail` and `jump` alone cannot infer one.
Every function body is checked, including bodies hidden by a duplicate
function name; the relation's program rule only requires lookup-visible
bodies. C follows the checker in both cases. It preserves first-match
lookups for duplicate data, constructor, function and parameter names,
rejects duplicate constants, and does not add surface-language Equal or
capability-capture restrictions absent from the Core checker.

Typing refusal means a compiler bug. E1027–E1053 are registered implementation
diagnostics with precise reasons and no reference section; E1052 and E1053
report resource limits rather than compiler bugs. Checking appends the first
refusal without clearing earlier diagnostics or mutating Core. Its owned label
names the declaration, the relevant variable, call, constructor or enclosing
let binding, and expected/found types for type mismatches. Names are escaped
and elided at 128 bytes; types have a 16-node display budget. It uses compiler
expression spans, falling back to the declaration span. If neither exists,
`file` is `UINT32_MAX` and both offsets are zero, an unavailable location that
diagnostic renderers must not index into the source table. A missing entry has
no declaration to locate; its label names the entry instead.

Work is bounded by `MO_LIMIT_CORE_DEPTH`, `MO_LIMIT_CORE_NODES` and
`MO_LIMIT_CORE_CHECK_SCRATCH_SIZE` (512 bytes per permitted node, 512 MiB total).
This is separate from the text codec's scratch budget. The depth bound applies
to expression and type walks, including metadata; the node budget counts
expression occurrences and distinct annotation nodes across the program.
Scratch is divided equally between persistent declarations/types and one
body's temporary state, reset after each body. Ordered AVL indices avoid
quadratic name scans; interned types avoid repeated structural equality and
cache their constructor declarations, including absent declarations.
Each expression occurrence builds its binding
indices once and keeps two inference/checking answers inline; additional
expected types use overflow entries. Memoization avoids exponential branch
retries. Lambda and join parameter arrays belong to the body arena. Wide case
arms are traversed iteratively.
Metadata module names use merge sorting and exported types use indexed
ownership instead of nested membership scans. Literal list/map arities are
compared with their actual arguments before allocating any signature space.

**Core-stage stack.** `MO_LIMIT_CORE_DEPTH` is 4,096; the text-size and node
limits are unchanged. The wire limit counts list wrappers and the document;
the in-memory expression/type limit counts walker levels. A statement may lower
to several lets, so the depth limit is not a source statement-count allowance.
Core parsing, printing, metadata validation, checking and compilation must run
through `mo_with_stack(MO_SOURCE_STACK_SIZE)`, like source checking. Core and
codegen tests, their corpus drivers and fuzz targets, and the VM's `--core`
driver use this explicit stack, not the platform's default. Every caller that
accepts full-depth Core must use it. The `mo` command's pending source-stack
follow-up carries this contract when it connects Core stages. The existing
48 MiB source allowance is retained.

The following are minimum passing stack reservations, in MiB, from 256 KiB
sweeps on Linux x86-64 with Clang `-O0 -g` debug and ASan/UBSan builds using
`-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined`. Each boundary was
repeated twice; the reservation 256 KiB below each listed minimum failed with
stack exhaustion. Codec inputs reach the 4,096 wire-level bound: a 4,093-let
expression and 4,094 nested List types; the primitive expression has 2,046
nested negations under one let. Checker, metadata and compiler inputs have 4,096
levels (4,095 lets or negations plus a literal), with complete source metadata.
The type input is a 4,096-level List parameter on an otherwise shallow function.

| Public walker | Let debug / ASan | Negation debug / ASan | List type debug / ASan |
| --- | --- | --- | --- |
| Text parser, including its canonical-output printer | 2.25 / 3.25 | 1.75 / 2.50 | 2.25 / 3.25 |
| Text printer | 2.25 / 3.00 | 1.75 / 2.00 | 2.25 / 3.00 |
| Metadata validator | 0.75 / 0.75 | 1.00 / 1.00 | — |
| Core checker, including metadata validation | 5.25 / 9.25 | 5.25 / 9.25 | 1.25 / 2.00 |
| Codegen, including its Core recheck | 5.25 / 9.25 | 5.75 / 9.25 | 5.25 / 6.25 |

Codegen's boundaries were remeasured after stacking the part 7 primitive and
BigInt work: debug negation compilation rose from 5.25 to 5.75 MiB; the other
compiler boundaries and the 48 MiB allowance were unchanged.

`index.c`'s AVL insert/find was separately exercised with 4,096 sorted binder
names: both builds pass at the smallest tested reservation, 256 KiB. Its
recursion follows AVL height, not Core nesting. The checker exceeds half an
8 MiB main-thread stack, even without sanitizers; ASan exhausts 9 MiB.
The explicit allowance therefore covers all Core tools. These reservations are
measured bounds for these inputs, not portable frame-size constants; compiler
flags and instrumentation can change them. Depth-bound tests run with the
same 48 MiB allowance in CI. Lean's `mocore` also runs a 4,096-let chain to
`returned "7"` with fuel 100,000, independently of C's implementation limit.

`ci/check_core_check.py` compares every Core fixture and committed or smoke
`fuzz_core_text`/`fuzz_core_check` corpus file with `mocore`, plus fixture mutations,
type-directed trees and an independent table of all primitive/host signatures.
The version comes from `core.h`. All syntactically valid runs use zero evaluator
fuel: this compares acceptance, not evaluation. Oracle processes and C batches
run in parallel. Generated arity/type refusals must have their expected code
classes; a resource refusal never counts as typing agreement. Smoke fuzzing
resets only the disposable Core corpora before seeding them each run.
Lean's signature construction expands a list/map literal's declared `Nat`
count before checking its arity; enormous counts can exhaust the oracle
instead of giving a verdict. C must refuse these mismatches promptly; C
regressions cover them, but they are outside the runnable differential corpus.
No Lean rule is changed to accommodate this resource limitation.

**Core code generation, staged slices.** `mo_compile` rechecks the complete Core
document, then writes through `mo_bytecode_write`. Failure appends a diagnostic
and leaves output empty; success owns its bytes independently of Core. E1055
means an unfinished compiler form, not a source error; E1056 means scratch or
allocation exhaustion; E1057 means compiler-produced bytecode was refused.
Registers, instructions and format limits use E1013, E1014 and E1015.
E1200 means the ordinary entry's full runtime name is not unique; arbitrary
entry names are otherwise accepted. E1201 means a permanent representation
exclusion from source lowering, not an unfinished feature or a resource limit.
E1202 means decimal BigInt conversion exceeded its program-wide work budget.
E1203 means reference liveness exceeded its per-function work or storage budget.
E1204 means register recycling exceeded its per-function work budget.

The first slice compiles immediate scalar literals, variables, lets, direct
calls, if, stop, and choices with small immediate payloads. The second adds
Text and Bytes literals, Text/Bytes/BigInt references, borrow inference,
tuples of at least two elements and projections, multi-field/boxed and recursive
choices, Range make/start/end, and spec fail. Option, Result, Ordering and
EntryKind retain separate instantiated types. It retains all five Core
function kinds and direct tail calls in every tail position it supports.
The third adds globals and ordered initialization for these expression forms.
The fourth adds function types, closures and application, including closures
in constant initializers and spec code. The fifth adds native arithmetic and
comparisons, generic equality, primitive and host table calls, capability and
collection types, and list/map literals. The sixth adds joins, jumps and
reconverging reference-state normalization. The seventh adds List uncons and
Range toText expansions and BigInt decimal literals; all 15 Core fixtures now
compile. The eighth consumes owned operands at their last use. The ninth recycles
exact-type block-local registers. Cross-block recycling and reset/reuse remain
unfinished optimizations. The
sample fixtures and Core fuzz corpora are attempted in CI; unfinished forms,
permanently excluded representations, entry-name collisions, resource limits
and invalid compiler inputs are counted separately. E1201 inputs are skipped
from compiler outcome comparisons, not passed off as compilations. Core's
checker differential continues testing them against Lean's broader language.
Compiler-produced invalid bytecode still fails the corpus check.
Generated programs within the slices must all compile, never merely refuse
consistently.
The writer structurally checks and fully verifies every compiled file.
Writer refusals are classified by their error kind, never by reason text.
Outcome comparison with Lean waits for the VM.

Primitive and host names map to rows in `prims.def` and `hostops.def`; codegen
reads their signatures and ownership columns directly. Trusted signature
templates are instantiated once per opcode/row and canonical type arguments,
not reparsed per expression. All operand expressions run in Core order.
Greater-than comparisons swap the resulting registers, not evaluation order;
Float uses native less-than/less-or-equal, never negated comparisons, preserving
NaN behaviour. Text, Bytes and BigInt use their compare rows and compare the
Ordering result with Equal. Generic equality uses `equal` at the exact type.
Owned references transfer directly at their last use; a still-live or borrowed
source is duplicated when an instruction needs an owner. Repeated owned operands
duplicate all but the last occurrence. An operand also read by a borrowed
parameter stays live throughout the instruction and cannot transfer directly.
All operand expressions finish before any transfer. Temporary owners not
consumed by the instruction are dropped only afterward.
This also preserves pre-instruction operands for a stopped call's report.
Host tails include the borrowed capability; zero-argument fakes have no tail.
Core's spec restrictions are retained and checked again by the verifier.

Range toText reads the two endpoints once, converts each with Int.to_text and
concatenates `start + ".." + end`, retaining the borrowed endpoint text until
its last use. List uncons takes an owned input and switches on List.first. None
drops the input and returns None without constructing a tail; Some extracts the
head and transfers the input to List.drop with count 1, then constructs
Some((head, tail)). A still-live source gets a distinct copy before this expansion,
preserving the original list. Both expansions use only existing
instruction/primitive rows, with their declared ownership, and use the Core
operation's span throughout. Arm-local reference registers are normalized at
the uncons merge just like explicit Core branches.
Last-use transfer removes the compiler's extra holder, and the runtime's
single-holder List.drop advances its live window instead of shifting the
retained suffix. Walking a single-holder list of length \(n\) with repeated
consuming uncons therefore has \(O(n)\) storage work, plus destruction of
discarded references. Periodic compaction and contraction remain amortized.
A still-live source or another holder requires a distinct tail copy to
preserve its original contents; repeated such copies can still cost \(O(n^2)\).

BigInt decimal literals become canonical little-endian magnitudes and signs;
zero has no magnitude bytes. Canonical signed decimal digits are interned
before conversion, so repeated literals pay once. The emitted constant is
interned by type, sign and magnitude. Conversion groups nine decimal digits,
combines them with powers of 10^9 in a balanced tree, and multiplies with
Karatsuba using a 24-frame explicit stack. For n decimal digits, work is
subquadratic, \(O(n^{\log_2 3})\),
not repeated multiplication by ten. Scratch uses nine padded limb arrays of
i128 coefficients and is freed after each literal. The program-wide
MO_LIMIT_CODEGEN_BIGINT_WORK (2^26) charges digit reads, limb operations and
coefficient products. Fixed coefficient work is predicted before allocating
scratch, from the padded chunk count and multiplication costs
\(M(n)=2n+n^2\) for leaf sizes up to 16 and \(M(n)=5n+3M(n/2)\) above that.
The budget admits at most 147,456 decimal digits and 2.25 MiB of conversion
scratch plus an arena header, rather than allocating the wire limit's 144 MiB
for a literal certain to exceed the budget. Magnitude-dependent output work
remains exact. Exceeding the budget gives E1202 at the literal
span (or declaration fallback), leaves output empty and is a resource refusal,
not a compiler bug. CI compares generated magnitudes/signs with Python across
limb, nine-digit group and multiplication boundaries; it also requires every
fixture and supported generated program to compile deterministically and verify.

Core's metadata validator follows Lean's transport-shape contract. Codegen
additionally checks that a linked promise's parameters match its subject's
inputs followed by its result. Incompatible metadata cannot come from source
lowering and is refused with E1201 before the verifying writer, not reported
as compiler-produced invalid bytecode (E1057).

FileError has the exact ordered cases and named Text fields of bytecode 9.
Entry is the builtin record `(name: Text, kind: EntryKind)`, with its record
layout, separate from user choice declarations. Its Core constructor emits
`construct`; its sole case arm reads `field` without a switch or merge.
Option and Result containing these or collections retain their nominal
instantiation identities and names. Collections and capabilities use the
existing runtime type kinds, without a format or type-builder extension.

Variadic list/map literals first evaluate all operands, then start empty and
append elements or insert pairs in order. A duplicate map key must not suppress
a later operand's effects. Each insertion consumes the preceding builder and
distinct operand copies. This uses the existing List empty/append and Map
empty/literal-insert rows, with at most three tail operands regardless of Core
literal arity. Builder steps are linear; registers and instructions obey their
existing limits, including registers × blocks. Reconverging collection and
capability paths use the same state normalization as other reference values.

Core functions begin the bytecode function table in declaration order, preserving
call and spec link indices. First-match function/data/parameter lookup agrees with Core.
Every data declaration uses a nominal choice, including one-constructor
declarations; an empty name on its single field means an unnamed field.
Builtin instantiations have separate nominal identities from user types of
the same printable name. Instantiated names use Mo's type spelling, including
tuple arguments such as `Option((Int, Text))`, within `MO_LIMIT_STRING_SIZE`.
Their rendering also obeys `MO_LIMIT_CORE_DEPTH`, reporting E1006 if implicit
signature wrappers take a checked annotation past that recursion bound.
Compound Core annotations are translated once per node, preserving first-use
type indices even when lowering shares immutable subtrees. This prevents
exponential traversal of type DAGs.
Names/strings and constants use deterministic first use. Source-less function
names are module + "." + Core name; source-less data/constants use a first
synthetic `%Core` module, followed by function modules in first-use order, with no
invented exports. Source metadata supplies full names, module order/exports/globals,
file remapping, declaration and expression spans,
original binder names/ranges, spec text/links/expectations/condition lines.
Absent expression spans clear attribution, not inherit the previous span.

Globals and the init list follow Core's constant dependency order; module global
lists independently retain metadata's source declaration order. A constant's full
name comes from metadata, or its lookup name in `noSource`. After Core functions,
each constant appends an ordinary, non-spec value function named `name.init.%value`
and its non-spec `name.init` wrapper. The value function compiles the original body
in tail position with its expression spans and binding names. The wrapper calls
it, consumes the returned value with one `set_global`, then returns Unit. This
also compiles always-stopping bodies without unreachable stores after stop: the
verifier sees a store in every wrapper, while a stopped call never publishes.
Both functions retain the constant's declaration span; the wrapper call uses the
body's root span and publication/return use the declaration span. Helpers are
private, never in module exports or the Core call-name index. Entry-name uniqueness
includes these appended functions. Their counts, registers and blocks obey the
same limits as ordinary functions. Global references are borrowed, becoming owned
only through `dup` for a consuming operand or return; they never enter frame cleanup.

Lifted closures follow all original functions and initializer helpers. A sizing
walk reserves their slots; preparation appends them in expression preorder, first
through Core functions in declaration order, then constants in dependency order.
They remain private and absent from the Core call-name index and module exports.
Entry-name uniqueness includes them. Source metadata supplies their full names;
`noSource` uses the enclosing Core lookup name plus `.fn.N`, counted from 1 in
preorder across nested lambdas, as `formal/TEXT.md` specifies. A constant's lambdas
use its name, not the generated value helper's suffix.

Capture discovery resolves active lexical registers, not names after scopes have
been restored. Captures follow first use, deduplicate by register identity, and
retain values through intermediate environments when only a nested lambda uses
them. Globals are read in lifted code, not copied into the environment. Explicit
parameters precede one implicit owned `env` register; all reference parameters
and extracted reference captures are owned. The environment is released by the
ordinary return/tail-call cleanup. Capture-less lambdas have immortal constants
and a closure layout, even though they need no allocation. Other lambdas consume
distinct capture copies with `make_closure`. Applications evaluate the callee,
then arguments in Core order, and consume distinct owned copies, including the
closure itself; every tail application uses `tail_call_closure`. Named-function
values are Core lambdas wrapping direct calls, not a separate compiler form.
Closure signatures have at most `MO_LIMIT_PARAMETERS` explicit parameters, leaving
room for the implicit environment within `MO_LIMIT_BYTECODE_PARAMETERS`; captures
also obey `MO_LIMIT_FIELDS` and the function register bound. These are E1015/E1013
resource refusals, not E1057 writer failures.

Expected function types reach lambda bodies through function results, calls,
applications and constructor fields. Missing inference results from stopping
lambdas can be resolved once from a later if/case arm without repeating preparation
or lifting, preserving the checker's fallback rule with bounded work. Lifted
debug declarations use the lambda node's span, including absent spans. Body
preorder, binder slots and closure names retain the enclosing metadata's identities;
the indices are built once and shared. Explicit parameters use their lambda
binder names; capture names survive extraction and cover the extracted registers
from `field` through the function. Spec closures inherit the spec flag, not their
enclosing function's spec text, subject, anchor expectation or condition lines.

Borrow inference builds ownership dependencies between parameters, aliases,
merge results and call arguments. Returns, constructed fields, projections
and call results require owners. Tail calls add argument-to-parameter edges
as well as parameter-to-argument edges. A queue propagates each owned slot
once, including across recursive calls; non-function kinds always own their
reference parameters. This is conservative across merged aliases. Consuming
operands use backward reference liveness, independently of borrow inference.
Lets, branch results, captures, constructors, calls, host/primitive operands,
returns and jump staging move an existing owner when it dies at that transfer.
Other uses duplicate it. Join free-variable sets grow monotonically to a fixed
point, so a backedge keeps every register needed by the next iteration live.
Each join converges its body locally, solving inner joins against its current
entry set before updating its own. Growth of an outer entry set retries its
body; independent joins and the function's surrounding code need no retry.
The final stable walk records transfers for emission. Only the body adds
requirements to its join's entry set; the rest reads the converged set. Lexical
labels prevent other joins outside that body or rest from reading it.
Lexical aliases retain separate holders until their own last uses. Temporary
owners are dropped after use; remaining owners are dropped before returns/tail calls.
Stop and fail abandon the frame through the outcome path, not normal cleanup.
The VM differential gate keeps 1,024 deterministic nested-operand, alias,
capture, branch and simultaneous-backedge cases. It also requires 85 nested
counter loops and 16 nested loops around a 400-step branching body to compile
and return `"donevw"`, agreeing with Core rather than exhausting E1203.

With runtime single-holder reuse, the VM builder
`go(acc ++ "ab", n-1)` scales linearly. Clang `-O0 -g`, Linux x86-64, three
fresh processes per size, an owned initial `"z"`, and the same loaded image
gave these median call times in seconds (compile/load/init excluded):

| Steps | Before last-use transfer | After |
| --- | --- | --- |
| 10,000 | 0.0221 | 0.0100 |
| 25,000 | 0.0594 | 0.0235 |
| 50,000 | 0.2252 | 0.0483 |
| 100,000 | 0.6817 | 0.0937 |
| 250,000 | — | 0.2281 |
| 500,000 | — | 0.4615 |
| 1,000,000 | — | 0.9552 |

The same runtime was used on both sides; only the compiler changed. Every
returned byte was checked as `"z"` followed by exactly the requested number
of `"ab"` pairs. The recursive builder's bytecode has no `dup` or `drop`;
the permanent test checks that property and its VM result without timing gates.

Preparation assigns monotonically numbered virtual temporaries and binders;
the physical allocator recycles block-local lifetimes after pruning. Immediate
slots are initialized before control splits so incoming states agree. All
reference slots start undefined;
there are no dummy Text, collection or function inhabitants. A live merge copies
the result into its own slot, drops dead owners, and uses `clear` to normalize
dead borrowed and moved slots to undefined. The result is owned when ownership
inference requires it, borrowed otherwise, on every incoming edge. Outer lexical
values that are live afterward retain their state; dead ones are normalized too.
Stop, fail, returns and tail calls need no merge
cleanup; their bookkeeping is restored for generating sibling paths.

Joins have lexical label indices and distinct parameter slots. Parameters are
visible only in the body, labels in both body and rest, with first-match duplicate
parameter names and inner-label shadowing. Result inference prefers the rest,
then the body, following Core's checker without repeating preparation. An entry
jump skips the body. Every Core jump evaluates and saves all arguments in order
before changing any parameter, including immediate arguments, so swaps work.
Reference arguments move their owners or get owned copies; staging slots use the declared
parameter types even when an argument stops. The edge then normalizes locals to
the target declaration's lexical state, drops old parameters, transfers the saved
values and clears moved reference staging slots. The first entry and all backedges
thus agree exactly; reference join parameters are conservatively owned. Body and
rest results use the ordinary value merge, or ordinary returns in tail position.
Register names run from the binder's write through its body (parameters
through the function, join parameters through the join body), not through
temporary initialization.

Unused joins can have self-edges or nested bodies with no path from the entry.
A linear word-bounded graph walk removes unreachable instructions before the
writer verifies. It relocates every target, projects piecewise source attribution
onto surviving instructions, and relocates name ranges, discarding empty ranges.
Graph scratch is at most nine bytes per permitted function word plus arena
headers, released per function; surviving output uses the compiler's arena.

Preparation, borrow inference and emission use bounded walks. Reference liveness
charges node/operand visits and register-set searches, shifts, copies, unions and edge scans
to `MO_LIMIT_CODEGEN_LIVE_WORK` (2^26) per function, including fixed-point retries;
exhaustion gives E1203 with empty output. Ordered
AVL indices for strings, constants, declarations, lexical names and per-type constructors
give logarithmic lookup; case emission never searches a declaration by a long
type name. Recursion follows the checked Core depth bound; wide lists/arms
are iterative. `MO_LIMIT_CODEGEN_SCRATCH_SIZE` is 512 bytes per permitted Core
node (512 MiB), separate from checking and text parsing. Ownership slots/edges
have explicit node-proportional limits in `limits.h`. Cleanup walks only live
owners. A reversible log records only reference-state changes, bounded by twice
`MO_LIMIT_FUNCTION_CODE_WORDS` entries per function. Checkpoints and per-register
marks restore and normalize only changed slots, once per edge, including borrowed
loads and consuming instruction operands. Nested merges discard their local log
and publish only the result's state change; sibling arms reuse the log's storage.
Liveness keeps sorted sparse branch-after/work and join-entry sets containing
only reference registers. Clearing resets a count, copies visit present entries,
and unions merge sorted entries. Membership uses bounded binary search; insertion
and removal charge every shifted cell. `MO_LIMIT_CODEGEN_LIVE_CELLS` (2^22)
bounds all set allocations per function, including old growth blocks retained
by the arena: at most 16 MiB of register indices. Dead-reference normalization
merges each set with a prepared reference-only register index under the same
liveness work ceiling; immediate registers do not multiply branch costs.
Instruction expansion and Core's
depth bound also bound bookkeeping work on nested jumps.
Emission stops at instruction/code-word limits, even when cleanup expands code.
Virtual registers share the ownership graph's slot bound, not the physical
frame bound. After pruning, a linear operand scan identifies blocks and
first/last accesses. ABI parameters and the closure environment stay in their
original slots; cross-block registers, reset tokens, prologue read dependencies
and registers whose first real access reads an incoming value remain pinned.
Remaining intervals recycle slots of exactly the same type through per-type
free lists. An instruction reads every operand before its writes; retiring
intervals become available only after the whole instruction. A new result
therefore cannot overwrite an operand that instruction still needs.

Owned reference intervals recycle only after a consuming access or clear.
Known constant/global borrows and same-block borrowed moves can retire after
their last read. Other ownership stays conservative. Pooled references are
cleared at every normal block exit; references read by that exit's transfer
instruction are pinned. Thus pooled slots are undefined on all incoming edges,
including backedges. Returns, stop/fail and tail calls abandon the frame and
need no pool normalization. Scalar initialization remains before control splits,
so a reused scalar slot stays initialized on every edge. Unused prologue writes
are removed. Rewriting relocates targets and piecewise spans; local names begin
after their first definition and end no later than physical reassignment.

Register scanning, allocation, inserted clears and relocation charge
`MO_LIMIT_CODEGEN_REGISTER_WORK` (2^26) per function. Scratch is a separately
bounded arena freed after each function; rewritten code, including inserted
clears, stays within `MO_LIMIT_FUNCTION_INSTRUCTIONS` and
`MO_LIMIT_FUNCTION_CODE_WORDS`. Physical slot counts and their product with
block counts are checked before the writer runs, giving E1013 or E1015, not
E1057. Function splitting and cross-block recycling remain future work.
The 4,096-deep scalar chains use at most three physical slots and return the
same values; a 1,024-step owned Text chain uses at most eight, checks every
returned byte and leaves no process-heap objects after dropping the result.
The register-boundary test compiles a 65,534-element tuple at 65,535 registers
in both an ordinary function and a constant value function, and refuses one
more element with E1013. Its one basic block keeps the test
independent of verification's registers × blocks bound. Wide case/tail-call
programs that formerly exceeded the virtual product now fit in at most three
slots. A separate boundary keeps 255 ABI slots pinned: 16,447 arms load at
4,194,240 register-block cells; 16,448 arms exceed 2^22 and get E1015.

Source lowering has a narrower representation contract than Lean's typed
Core. The following forms are permanently refused with E1201 and a precise
reason, including in unused declarations or discarded tuple expressions.
They require no bytecode or type-builder extension. Mo's grammar prevents
empty enums and tuples with fewer than two elements; source checking and
lowering prevent the other forms. The Core checker still follows Lean and
does not reject a well-typed program merely for using one of these forms:

- **Empty choices and undeclared data types:** neither is produced by lowering.
  Do not insert dummy constructors, invent nominal declarations or alias Unit.
- **Duplicate constructor names:** never discard later constructors or remap
  their arms. Source checking requires unique labels within each declaration.
- **Zero/one-element tuples:** never erase tuple identity into Unit or the sole
  element. Tuples of at least two elements keep their arity and projections.
- **Empty/NUL-containing names:** declarations and lexical binders require
  nonempty, NUL-free names. Do not escape, rename or truncate them to fit the
  runtime. Compiler metadata already requires nonempty NUL-free display/file/module
  names. Text/Bytes payloads and stop messages still permit empty strings and NUL.
- **Empty or duplicate field labels:** named fields must have nonempty,
  NUL-free, unique labels. The one empty label on a single field denotes an
  unnamed field and remains valid; it is not a named-field exception.
- **Missing runtime abilities:** Map keys and Set elements need Hash; equality
  and the constrained primitive rows need Equal or Hash as their signatures say.
  Core and Lean allow broader instantiations, including equality on functions
  or capabilities and Float map keys, which source checking cannot lower.
  Codegen refuses these with E1201 before the writer, not E1057. Abilities are
  inferred once over the runtime type graph, removing bits through reverse
  edges to a greatest fixed point, including recursive nominal types. Each bit
  changes once; each instruction constraint then takes constant time. No Lean
  rule is changed and compiler comparisons skip these permanent exclusions.

**Checked-program contract, provisional.** `src/check/check.h` now defines the
input to lowering. Its
`mo_checked_*` records and `mo_program` are an in-memory interface; their layout
may change while checking is completed. `mo_check` is an implemented walking
skeleton: it prepares interfaces and signatures and shares inference's walk to
emit typed graphs, rather than running a second name/type checker. It currently
certifies nongeneric functions, scalar expressions and native scalar operator
proofs, direct/indirect calls, record/choice construction and fields, normalized
patterns, tuples/lists, conditionals, loops, early exits/asserts and noncapturing
lambdas. `main` returns Unit or Int and
takes no parameters or one Console. Direct `Console.print` reads its operation
and named input type from `base/hostops.def`, with the receiver explicit.
Native method values, other host operations, generic functions, map expressions,
imports, constants/specs and derived abilities
still report E1103. Any `where` clause and any declared `compare` or `to_text`
customizer are refused until ability checking exists, even on nongeneric
functions. Written Map/Set types are refused in every annotation, including
unused declarations and nested fields, until Hash requirements are checked.
Invalid entry signatures report E0079: parameters must be capabilities without
duplicate types, and the result must be Unit, Int or Result(Unit, E). Valid
unfinished entry conventions still report E1103. Checking stops at the first
failed stage or body; no staged inference result or partly filled program is
returned as certified output.
The whole-program corpus asserts resolved graphs and canonical binders, checks
unfinished-stage rejection, sweeps arena limits and reaches maximum nesting.
`fuzz_program` walks graph invariants and varies allocation budgets. Generated
arithmetic controls also compare their graph's value with an independent
calculation from input bytes, and require a type-changing control to be refused.

Each named function has a typed expression graph in written postorder, explicit
operand indices, a root, and typed canonical binders, including nested lambdas
and shared pattern-alternative names. Operator and postfix steps are explicit;
assignment links its original `var`, not a new binding. Resolved symbols,
native reference spellings, field indices and dot receivers remove lookup and
dispatch from lowering. Scoped generic substitutions accompany every generic
use. The selected ability implementation is native, derived, a declaring-module
custom function, or an abstract requirement resolved during monomorphization.
Types remain shared DAGs; ability graphs can be recursive through nominal
data. Lowering memoizes them rather than expanding them.

The graph's operand conventions are part of the contract. Sequence operands
are statement roots in order, with the last value or Unit for an empty block.
Group and expression-statement rows forward one operand. If rows alternate
condition/body operands, followed by an optional else body. A guarded return
has its guard before its lazy value, matching evaluation rather than spelling.
While rows have condition/body operands; for rows have iterable/body operands,
with a canonical element binding for a single name and a normalized binder
pattern for every name/ignored/tuple form. Both loops return Unit. Conditions
are re-evaluated on each while round; bodies remain lazy. Break and continue
have zero operands or one guard and target the nearest loop in the same function.
Case arms have separate scopes; alternatives share canonical binding pointers.
An is row has one scrutinee operand and a normalized pattern, retaining 7.4's
true-path scopes. Omitted record fields become explicit typed ANY patterns.
These nodes are immutable and shared per declaring field and nominal
instantiation; their syntax points at the field declaration. Checked pattern
nodes and parts use ordinary node/arena budgets, not coverage's byte charging.
The 150-arm, 1,000-field regression retains all fields without E1104.
Pattern literals retain contextual range checks and normalized LITERAL metadata,
but emit no executable expression rows, including for their unary minus.
Ignored `_` has no binding entry. A lambda's ignored parameter slot is NULL,
with positional arity and its type retained in the function type; an ignored
for binder has NULL binding and an ANY pattern.
Native case substitutions borrow immutable Option/Result parameter templates;
their parameter owners have no source declaration. Parameter pointer identity,
not its spelling, selects a substitution. Native payloads and declared payloads
use the same operand and field-index conventions. Tuple/list operands keep
element order; tuple destructuring uses a normalized binder pattern instead of
a single binding pointer. Compound writes consume an explicit operator row,
which borrows the assignment's syntax but isn't itself a write.
A whole-var assignment has one value operand; a field update has its resolved
target projection followed by its value, and retains the original base var.
Projection rows retain the field path. A compound field write and its RHS
operator both reference the same target projection row, which runs only once.
Constructor operands keep written order:
named-argument wrapper rows carry the declaration-order field index and forward
one value; a single unnamed payload needs no wrapper. Ordinary calls have callee
then argument operands. Native dot calls omit the callee, retaining its qualified
native spelling and an explicit receiver operand/row index. Lambda bodies remain
lazy operands. Intermediate flat operators borrow their TOKEN syntax; final
chain/postfix rows borrow the owner, whose final step supplies the syntax kind.
This avoids copying normalized syntax solely for lowering. Numeric leaves retain
source spelling; lowering folds an immediately negated numeric literal before
constructing a scalar, including Int's signed minimum.

Checked patterns name their cases and declaration-order fields. Case arms keep
source order and guards; coverage and the no-catch-all rule are already checked.
`or` explicitly selects Bool or Option. `try` identifies its enclosing result,
failure constructor and any unique wrapping case. Lambda records retain typed
parameters, captures in first-use order and source-order numbers before lifting.
Constants alone are dependency-ordered, with source-order ties. The entry names
the entry module's `main`, its ordered capability parameters, result convention
and the Text implementation for a returned failure.

Examples, tests, promises and anchors keep their typed bodies, subject/promise
links, source ordinals, decoded sentences and exact source text/spans. Promise
conditions remain separate Bool rows, which lowering turns into the numbered
Int answer of bytecode 3.3. An anchor has argument-value rows, not a tuple:
`values` maps them to promise-parameter order while their graph retains written
evaluation order. Lowering makes the promise call and returns its Int answer.
`mo_check` types and links anchors but executes no source code; tools compile
and run them before accepting their allows/forbids expectations. This keeps
`check` dependent only on `syntax` and `base`.

The result arena owns interfaces, signatures, types, binders, graphs and spec
byte copies; native spellings may borrow immutable compiler tables.
The program borrows its loaded set and syntax, which must outlive
it and remain unchanged. It borrows no source-store bytes. Original sources
remain a separate argument to `mo_lower` for filenames and provenance; Core
copies all metadata and borrows neither input. Counts, graph work, arena storage
and source execution retain the base limits and explicit-stack policy.

```c
/* src/base/diag.h: diagnostics, rendered as text or JSON by src/tools. */
struct mo_span { uint32_t file; uint32_t start; uint32_t end; };     /* byte offsets */
struct mo_label { struct mo_span span; const char *text; };
struct mo_edit { struct mo_span span; const char *replacement; };   /* an empty span inserts */
/* Edits apply together or not at all, each against the original text, and don't overlap. */
struct mo_fix { const char *title; const struct mo_edit *edits; uint32_t edit_count; };
struct mo_diag {
    uint16_t code;                      /* E0412, registered in src/base/codes.h */
    struct mo_label primary;
    const struct mo_label *secondary;   /* secondary_count of them */
    uint32_t secondary_count;
    const char *message;                /* the headline, in the reference's words */
    const char *reason;                 /* why, citing the section; NULL only for a limit's code */
    const char *section;                /* the reference section it cites, such as "8.3" */
    const struct mo_fix *fixes;         /* fix_count alternatives */
    uint32_t fix_count;
};
/* Diagnostics own their strings, in their own arena. */
MO_MUST_USE bool mo_diags_init(struct mo_diags *diags);
void mo_diags_free(struct mo_diags *diags);
/* Source files, indexed by mo_span.file, for printing snippets. A file that can't be added
 * adds nothing, and error_out says why: unreadable, past a limit, or no memory. */
MO_MUST_USE bool mo_sources_init(struct mo_sources *sources);
void mo_sources_free(struct mo_sources *sources);
MO_MUST_USE bool mo_sources_add(struct mo_sources *sources, const char *path, uint32_t *file_out,
                                struct mo_source_error *error_out);

/* src/syntax/syntax.h: on an error, a partial tree for the rest of the file, and diagnostics. */
MO_MUST_USE struct mo_module_syntax *mo_parse(struct mo_arena *arena, const struct mo_sources *sources,
                                              uint32_t file, struct mo_diags *diags);
MO_MUST_USE bool mo_read_header(struct mo_arena *arena, const struct mo_sources *sources, uint32_t file,
                                struct mo_header *header_out, struct mo_diags *diags);

/* src/check/check.h: implemented intermediate stages; NULL with diagnostics.
 * None of their successes is a lowering-ready checked program. */
MO_MUST_USE struct mo_module_set *mo_load_program(struct mo_arena *arena, struct mo_sources *sources,
                                                  const char *entry, const char *library,
                                                  struct mo_diags *diags);
MO_MUST_USE struct mo_module_interfaces *mo_resolve_modules(struct mo_arena *arena,
                                                            const struct mo_module_set *set,
                                                            struct mo_diags *diags);
MO_MUST_USE struct mo_signatures *mo_prepare_signatures(struct mo_arena *arena,
                                                        const struct mo_module_set *set,
                                                        const struct mo_module_interfaces *interfaces,
                                                        struct mo_diags *diags);
MO_MUST_USE struct mo_inferred_body *mo_infer_function(struct mo_arena *arena,
                                                       const struct mo_module_interfaces *interfaces,
                                                       const struct mo_signatures *signatures,
                                                       uint32_t function, struct mo_diags *diags);
/* Provisional checked-program contract, defined in check.h. mo_check produces
 * only its certified subset; unfinished forms report E1103. */
struct mo_program {
    const struct mo_module_set *set; /* borrows loaded syntax */
    const struct mo_module_interfaces *interfaces;
    const struct mo_signatures *signatures;
    uint32_t function_count;
    const struct mo_checked_function *functions; /* typed bodies, binders and use decisions */
    uint32_t constant_count;
    const struct mo_checked_constant *constants; /* dependency order */
    uint32_t spec_count;
    const struct mo_checked_spec *specs; /* text, spans, conditions and links */
    struct mo_checked_entry entry; /* main capabilities and result convention */
};
/* Lowering-ready program, or NULL with diagnostics; no partial success. */
MO_MUST_USE struct mo_program *mo_check(struct mo_arena *arena, const struct mo_module_set *set,
                                        struct mo_diags *diags);

/* src/lower/lower.h: lowering and monomorphization. Copies complete source metadata
 * (formal/TEXT.md) into arena; no pointer in Core borrows the program or sources. */
MO_MUST_USE struct mo_core *mo_lower(struct mo_arena *arena, const struct mo_program *program,
                                     const struct mo_sources *sources,
                                     struct mo_diags *diags);

/* src/core/core.h: Core's checker and its text form. */
MO_MUST_USE bool mo_core_check(const struct mo_core *core, struct mo_diags *diags);
MO_MUST_USE bool mo_core_print(const struct mo_core *core, struct mo_buffer *out);
MO_MUST_USE struct mo_core *mo_core_parse(struct mo_arena *arena, const char *text, size_t length,
                                          struct mo_diags *diags);

/* src/codegen/codegen.h: the whole Core document, spec code and metadata included.
 * Validate metadata shape; derive debug ranges, remap file indices to filename strings,
 * preserve module exposure and spec links/lines. Never reopen sources or consult checked Mo. */
MO_MUST_USE bool mo_compile(const struct mo_core *core, struct mo_buffer *bytecode_out, struct mo_diags *diags);

/* src/bytecode/bytecode.h: writing and text tools borrow inputs. out is an initialized,
 * empty caller-owned buffer, never input storage. On false it is empty (capacity may have
 * grown), and required error_out gives static reason text. The writer verifies, preserves
 * indices and regenerates the header/checksum. Names must already be in the strings table. */
struct mo_write_error { const char *reason; };
struct mo_assemble_error { uint32_t offset; const char *reason; };  /* text byte offset */
struct mo_load_error { uint32_t offset; const char *reason; };       /* reason is static text */
MO_MUST_USE bool mo_bytecode_write(const struct mo_bytecode *file, struct mo_buffer *out,
                                   struct mo_write_error *error_out);
/* Assembly accepts bytecode.md section 10's indexed form or shorthand with optional pool seeds.
 * Disassembly verifies and prints only canonical indexed text, retaining every entry, index,
 * literal bit, exposure flag and debug field, so reassembly gives identical bytes.
 * Scalars use exact natural spellings, except float uses its bits. Text input/output fits
 * MO_LIMIT_BYTECODE_TEXT_SIZE; binary files fit MO_LIMIT_BYTECODE_SIZE.
 * Neither form changes the binary format. No terminating NUL is printed. */
MO_MUST_USE bool mo_bytecode_assemble(const char *text, size_t length, struct mo_buffer *out,
                                      struct mo_assemble_error *error_out);
MO_MUST_USE bool mo_bytecode_disassemble(const uint8_t *bytes, size_t length, struct mo_buffer *out,
                                         struct mo_load_error *error_out);
/* Loading always verifies and copies what it needs from bytes. The accessor's read-only
 * tables live until mo_image_free, retaining file indices and module contents_exposed. */
MO_MUST_USE struct mo_image *mo_load(const uint8_t *bytes, size_t length, struct mo_load_error *error_out);
MO_MUST_USE const struct mo_bytecode *mo_image_bytecode(const struct mo_image *image);
MO_MUST_USE const struct mo_types *mo_image_types(const struct mo_image *image);
/* The opaque VM run state attaches once; image_free releases it before decoded tables. */
MO_MUST_USE void *mo_image_runtime(const struct mo_image *image);
void mo_image_attach_runtime(struct mo_image *image, void *runtime, void (*release)(void *));
void mo_image_free(struct mo_image *image);

/* src/vm/vm.h: initialization, processes and calls. */
enum mo_outcome { MO_RETURNED, MO_STOPPED, MO_FAILED };
/* Runs the init functions once; on a stop, the image can't run and defect_out says why. */
MO_MUST_USE enum mo_outcome mo_image_init(struct mo_image *image, const struct mo_limits *limits,
                                          struct mo_defect *defect_out);
/* A process must not outlive its image, which must be initialized. */
MO_MUST_USE struct mo_process *mo_process_new(const struct mo_image *image, const struct mo_limits *limits);
void mo_process_free(struct mo_process *process);    /* frees its whole heap, never the image's */
/* Arguments passed to owned parameters are consumed on every outcome; those passed to borrowed
 * ones stay the caller's. Duplicate before the call to retain an owned argument for a promise.
 * Only MO_RETURNED writes an owned result_out, which C may drop early. On stop/failure it is
 * unchanged, and defect_out describes why, valid until mo_process_free. STOPPED/FAILED
 * are terminal; RETURNED permits another call. INIT and CLOSURE are not direct C calls. */
MO_MUST_USE enum mo_outcome mo_call(struct mo_process *process, uint32_t function,
                                    const struct mo_cell *arguments, uint32_t argument_count,
                                    struct mo_cell *result_out, struct mo_defect *defect_out);
MO_MUST_USE bool mo_render_defect(const struct mo_process *process, const struct mo_defect *defect,
                                  struct mo_buffer *out);
/* C harness hooks only, not Mo resource limits; internal stop on exhaustion. */
MO_MUST_USE enum mo_outcome mo_image_init_bounded(struct mo_image *image,
    const struct mo_limits *limits, uint64_t instructions, struct mo_defect *defect_out);
void mo_process_instruction_limit(struct mo_process *process, uint64_t instructions);
/* Initializes the image, then runs main with real capabilities; returns the exit status. */
MO_MUST_USE int mo_run(struct mo_image *image, const struct mo_run_options *options);

/* src/heap/heap.h: C ownership is type-directed. Both operations are infallible, copy/do nothing
 * for immediates, and follow bytecode 2.4's immortal and saturating counts. dup returns another
 * owned cell without consuming its input; drop consumes one owned cell without C recursion. */
struct mo_heap_image; /* Shared run metadata; its initialization heap owns image values. */
struct mo_heap;       /* A separate, fixed-size context owned by struct mo_process. */
MO_MUST_USE struct mo_heap_image *mo_heap_image_new(const struct mo_types *types, const struct mo_limits *limits);
MO_MUST_USE bool mo_heap_image_refresh(struct mo_heap_image *image);
MO_MUST_USE struct mo_heap *mo_heap_image_constants(struct mo_heap_image *image);
void mo_heap_image_free(struct mo_heap_image *image);
MO_MUST_USE struct mo_heap *mo_heap_new(struct mo_heap_image *image, const struct mo_limits *limits);
void mo_heap_free(struct mo_heap *heap);
MO_MUST_USE struct mo_cell mo_value_dup(struct mo_heap *heap, uint32_t type, struct mo_cell value);
void mo_value_drop(struct mo_heap *heap, uint32_t type, struct mo_cell value);
/* Building generated inputs borrows input cells and duplicates references kept. Successful
 * results are owned in the process heap; false leaves inputs and out unchanged. Byte builders
 * copy at most MO_LIMIT_VALUE_BYTES bytes, subject to --max-heap; NULL is allowed only for
 * zero length. Bytes accepts any sequence. BigInt uses base.h's sign enum and bytecode 3.1's canonical magnitude; invalid
 * signs/magnitudes, lengths past the limit and allocation/heap-limit failure return false. */
MO_MUST_USE bool mo_value_text(struct mo_heap *heap, const char *utf8, size_t length, struct mo_cell *out);
MO_MUST_USE bool mo_value_bytes(struct mo_heap *heap, const uint8_t *bytes, size_t length, struct mo_cell *out);
MO_MUST_USE bool mo_value_bigint(struct mo_heap *heap, enum mo_bigint_sign sign,
                                 const uint8_t *magnitude, size_t length, struct mo_cell *out);
MO_MUST_USE bool mo_value_construct(struct mo_heap *heap, uint32_t layout, const struct mo_cell *fields,
                                    uint32_t count, struct mo_cell *out);
MO_MUST_USE bool mo_value_case(struct mo_heap *heap, uint32_t type, uint32_t tag, const struct mo_cell *fields,
                               uint32_t count, struct mo_cell *out);
MO_MUST_USE bool mo_value_list(struct mo_heap *heap, uint32_t type, const struct mo_cell *elements,
                               uint32_t count, struct mo_cell *out);
MO_MUST_USE bool mo_value_map(struct mo_heap *heap, uint32_t type, const struct mo_cell *keys,
                              const struct mo_cell *values, uint32_t count, struct mo_cell *out);
MO_MUST_USE bool mo_value_set(struct mo_heap *heap, uint32_t type, const struct mo_cell *elements,
                              uint32_t count, struct mo_cell *out);
MO_MUST_USE bool mo_value_closure(struct mo_heap *heap, uint32_t function, const struct mo_cell *captures,
                                  uint32_t count, struct mo_cell *out);
/* The VM's _owned construct/case/closure/reuse variants transfer field ownership
 * on success, without a dup/drop pair; failure preserves fields, tokens and out. */
MO_MUST_USE bool mo_value_construct_owned(struct mo_heap *heap, uint32_t layout,
    const struct mo_cell *fields, uint32_t count, struct mo_cell *out);
MO_MUST_USE bool mo_value_make_immortal(struct mo_heap *heap, uint32_t type, struct mo_cell value);

/* src/host/host.h: a fake for mo test's generated inputs, as its constructor would make it
 * from contents, such as Files.in_memory's map. */
MO_MUST_USE bool mo_host_fake(struct mo_process *process, enum mo_capability which, const struct mo_cell *contents,
                              uint32_t count, struct mo_cell *out);
```

The order of work: stream 0 first, for about a week, reviewed against these
documents. Then streams 1, 2 and 3 in parallel, each tested on its own as
the table says, and stream 4 throughout. The differential tests join streams
1 to 3 as soon as each can run its part.

**Repository changes for Phase 2.** The Makefile builds `src/<module>/*.c` per
module; `grammar/` includes `src/base/base.h` by its new path; `.agents/setup`
adds elan, with Lean 4.34.0, and CBMC; `ci/check.sh` gains `make formal` and
the CBMC proofs, within a time budget for the CI orb.

## 13. Open questions

### 13.1 For engineering, measured in Phase 2

- **Monomorphization's cost.** Instantiating every generic function at every
  type it is used with keeps the VM simple and fast, but grows code. If it
  grows too much, generic code can share one instantiation for all
  reference types, whose cells look the same.
- **Packed lists.** Every element is a 16-byte cell, so a `List(Int)` uses
  twice the memory it needs. Packing lists of small immediates is decided
  with the benchmark.
- **Process-owned chunks with size classes.** Could chunk teardown replace
  per-object teardown links while retaining checked heap accounting and
  context-free value recovery? The current allocator charges a 56-byte prefix
  plus the 8-byte header: `build/clang/debug/tests/test_heap.test --bench` allocated one million
  `(Int, Int)` tuples for 96,000,000 charged bytes in 0.077 s (Clang `-O0`,
  Linux x64 orb). Compare charged bytes, throughput, fragmentation, reuse and
  teardown against that baseline before replacing the allocator. No uncharged
  allocation ledger or claim of a 40-byte allocation is part of this version.
- **Text's character count** is computed on each call. Caching it costs a word
  per text; decide with the benchmark.
- **Unicode16** now has generated UCD 16.0.0 tables, the generator, compressed
  unmodified sources and their hashes in the repository (section 12).
  CI checks regeneration and the UCD's conformance data; width follows the
  explicit policy in `library.md`, not the host terminal.
- **The bytecode format's stability.** It is internal in Phase 2. Phase 5's
  upgrades need a versioned format; decide then whether it is public.

### 13.2 Settled with Robert

Writing this architecture and Mo Core found places where the reference didn't
say what a program means. Robert decided all of them on 12 October 2026 (D227),
and the reference now says: when an anonymous function may read a `var`
(5.6); which built-in types have which abilities (9.2); where `printed` can be
used (12.4); repeated keys in map literals (3.8); paths that leave a folder,
line endings, invalid UTF-8 on standard input, and times as text (14); the
order of constants (10.4); polymorphic recursion (5.5); the limits of a few
library functions (14); and the `mo` command's exit status (13.4). The same
day he settled the `mo` command for agents (D233): JSON output with a written
promise, choosing tests by exact name or module, and limits as flags, with
`src/base/limits.h`'s defaults (reference 13.4; sections 10 and 10.2).
