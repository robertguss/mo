/* Mo's limits: every bound on what the toolchain reads and on what a running program uses, by
 * name, so that a test can reach each one (architecture section 2).
 *
 * There are three kinds, and each limit's comment says which kind it is:
 *
 * - A diagnostic. A limit on what the compiler reads: a program past it is refused with an error,
 *   whose code src/base/codes.h registers.
 * - A load error. A limit on a bytecode file: the loader refuses a file past it (bytecode.md
 *   section 3). Where the compiler could write such a file, it checks the same limit first and
 *   reports a diagnostic instead, so it never writes a file the loader refuses.
 * - A bug. A limit on a running program, in struct mo_limits: passing it stops the program with a
 *   report (reference 13.2), never a crash.
 *
 * Limits on what the compiler reads are set well beyond what people write, so that only generated
 * or hostile input meets them. The others follow from those, or from the bytecode's encoding.
 *
 * The limits are integer constants, which compare and convert without a cast to the int, uint32_t
 * and size_t values they bound. */
#ifndef MO_LIMITS_H
#define MO_LIMITS_H

#include <stddef.h>
#include <stdint.h>

/* What the compiler reads. Each limit here is a diagnostic. */

/* Bytes in one source file. Spans hold byte offsets in 32 bits, so this must stay below 2^32.
 * mo_sources_add refuses a longer file: E1001. */
#define MO_LIMIT_FILE_SIZE (8 << 20)

/* Bytes of source in one program, all its files together. mo_sources_add refuses a file that
 * would pass it: E1002. */
#define MO_LIMIT_PROGRAM_SIZE (64 << 20)

/* Source files in one program, the library's modules included: one module each. mo_sources_add
 * refuses one more: E1003. */
#define MO_LIMIT_FILES 4096

/* Directory entries inspected while finding the toolchain's library modules, including
 * non-source files and folders but not . and ..: E1059. This bounds a source-free directory tree.
 */
#define MO_LIMIT_LIBRARY_ENTRIES 65536

/* Unqualified names imported by one module, including exposed choice cases: E1060. This
 * bounds expansion when a short use line brings in a large choice interface. */
#define MO_LIMIT_MODULE_IMPORTS (1 << 20)

/* All imported name groups and exposed choice cases across the program: E1100. Cases and
 * names borrow their declaring module's storage; this bounds the per-import index entries. */
#define MO_LIMIT_PROGRAM_IMPORTS (4 << 20)

/* Bytes in a source file's path, as the toolchain names it to the system. mo_sources_add refuses a
 * longer one: E1004. */
#define MO_LIMIT_PATH_SIZE 4096

/* Characters in one line, its line ending not counted. They are counted as columns are
 * (src/base/diag.h). The lexer refuses a longer line: E1005. It is far above any line a person
 * writes, so that a long text literal on one line still fits; MO_LIMIT_FILE_SIZE bounds lines
 * anyway. */
#define MO_LIMIT_LINE_LENGTH 65536

/* How deeply code may nest: blocks, brackets, and expressions and patterns inside each other. It
 * bounds the recursion of the parser, and of every stage that walks the syntax tree. The parser
 * refuses deeper code: E1006. */
#define MO_LIMIT_NESTING_DEPTH 256

/* Core interchange, independent of the surface parser. Nesting counts every wire list,
 * including list wrappers and the document. The lexer uses an explicit stack; codec walks
 * recurse only up to this bound. E1006 for depth, E1016 for bytes/nodes/scratch, E1017 for arena
 * exhaustion. The byte limit covers both input and canonical output. Nodes also bound wide
 * lists and the printer's work on caller-built trees. */
#define MO_LIMIT_CORE_DEPTH 4096
#define MO_LIMIT_CORE_TEXT_SIZE (8 << 20)
#define MO_LIMIT_CORE_NODES (1 << 20)
#define MO_LIMIT_CORE_SCRATCH_SIZE (128 << 20)

/* Core checking keeps interned types, prepared expressions, lexical indices and memo answers.
 * Size its two arenas from the interchange node budget, not the text codec's scratch budget.
 * E1053 for exceeding this scratch limit or when the system cannot allocate it. */
#define MO_LIMIT_CORE_CHECK_SCRATCH_SIZE (UINT64_C(512) * MO_LIMIT_CORE_NODES)

/* Prepared code, ordered indices and decoded bytecode tables; E1056 on exhaustion. */
#define MO_LIMIT_CODEGEN_SCRATCH_SIZE (UINT64_C(512) * MO_LIMIT_CORE_NODES)

/* Decimal BigInt conversion work across one compilation. Balanced radix conversion counts
 * digit reads, limb operations and coefficient products; E1202, not a compiler bug. */
#define MO_LIMIT_CODEGEN_BIGINT_WORK (1u << 26)

/* Backward reference liveness, including join fixed points: node/operand visits and
 * register-set cells copied or merged per function. E1203 on exhaustion. */
#define MO_LIMIT_CODEGEN_LIVE_WORK (1u << 26)
/* Sparse reference-set storage, including retained growth blocks, per function. E1203. */
#define MO_LIMIT_CODEGEN_LIVE_CELLS (1u << 22)

/* Register recycling scans and relocates bounded emitted code; E1204 on exhaustion. */
#define MO_LIMIT_CODEGEN_REGISTER_WORK (1u << 26)

/* Borrow inference slots and dependency edges, also E1056. They grow with prepared registers
 * and Core operands; each owned slot is propagated once, never by rescanning the call graph. */
#define MO_LIMIT_CODEGEN_OWNERSHIP_SLOTS (4u * MO_LIMIT_CORE_NODES)
#define MO_LIMIT_CODEGEN_OWNERSHIP_EDGES (8u * MO_LIMIT_CORE_NODES)

/* How deeply interpolations may nest inside each other's text, as the executable grammar allows
 * (grammar/lexer.c uses this limit). The lexer refuses deeper ones: E1007. */
#define MO_LIMIT_INTERPOLATION_DEPTH 16

/* Names a module declares: its functions, types and constants. The checker refuses more: E1008. */
#define MO_LIMIT_MODULE_NAMES 65535

/* Parameters of one source function, and arguments of one source call. The checker refuses
 * more: E1009. Generated promise results and closure environments are not source parameters. */
#define MO_LIMIT_PARAMETERS 255

/* Fields of one record or case, elements of one tuple, and values one closure captures.
 * construct and make_closure take each as an operand, so this is at most MO_LIMIT_OPERANDS. The
 * checker refuses more: E1010. The loader refuses a type or layout with more. */
#define MO_LIMIT_FIELDS 65535

/* Cases of one choice type. switch takes a target for each, so this is at most MO_LIMIT_OPERANDS
 * (bytecode.md section 9). The checker refuses more: E1011. The loader refuses a type with more. */
#define MO_LIMIT_CHOICE_CASES 65535

/* How deeply instantiations of generic functions may nest: an instantiation that asks for
 * another, as f at Int asking for g at List(Int), nests one deeper than it. The checker refuses
 * polymorphic recursion (architecture section 3.3), so this is a second line of defense.
 * Lowering stops at it: E1012. */
#define MO_LIMIT_INSTANTIATION_DEPTH 64

/* Nodes along one path in an inferred surface type, including instantiated
 * function values. This independently bounds implicit growth on flat source:
 * E1105. A shared subtree keeps its height, not just its first traversal depth. */
#define MO_LIMIT_SOURCE_TYPE_DEPTH 256

/* Edges on an ability implementation path, including nominal field layouts.
 * These are not nested type arguments. E1108 bounds proof construction and
 * callback-dependency walks independently of type height. */
#define MO_LIMIT_SOURCE_ABILITY_DEPTH 4096

/* Each source nesting can retain the precedence ladder (implies, or, and, not,
 * comparison/is, range, sum, product, negate, postfix) and statement/sequence
 * wrappers. Sixteen frames per level plus the function wrappers bound inference
 * independently of parser bookkeeping. E1106, never an assertion on input. */
#define MO_LIMIT_SOURCE_INFERENCE_DEPTH (16 * MO_LIMIT_NESTING_DEPTH + 8)

/* Type construction, unification, occurs checks and expression visits in one
 * surface function. E1104 bounds generated constraints and shared type DAGs
 * independently of the caller's arena limit. Coverage charges algorithm steps
 * and one unit per allocated byte, so retained matrices share this bound. */
#define MO_LIMIT_SOURCE_CHECK_WORK (1 << 24)

/* Inference work shared by every body checked against one preparation, E1107.
 * A hostile program cannot multiply the per-function budget by its functions. */
#define MO_LIMIT_PROGRAM_CHECK_WORK (UINT64_C(1) << 28)

/* Lowering charges allocation bytes and algorithm steps to this shared budget,
 * independently of the caller arena (E1152). Emitted Core nodes are bounded by
 * MO_LIMIT_CORE_NODES; paths by MO_LIMIT_CORE_DEPTH (E1153). Generic discovery
 * also retains MO_LIMIT_INSTANTIATION_DEPTH (E1153). */
#define MO_LIMIT_LOWER_WORK (UINT64_C(1) << 28)

/* Reserved POSIX stack for source-stage execution, not an input restriction.
 * Tests, fuzzing and source tools use base/stack.h. */
#define MO_SOURCE_STACK_SIZE (48 << 20)

/* Registers of one function, its parameters included. The code generator refuses a function that
 * needs more: E1013. The loader refuses a function with more. */
#define MO_LIMIT_FUNCTION_REGISTERS 65535

/* Instructions in one function's code. The code generator refuses a function that compiles to
 * more, or to more than MO_LIMIT_FUNCTION_CODE_WORDS words: E1014. */
#define MO_LIMIT_FUNCTION_INSTRUCTIONS (1 << 18)

/* What a bytecode file holds (bytecode.md section 3). Each limit here is a load error: the loader
 * checks every count and index before using it. The code generator reports a program whose file
 * would pass one as too big for the format: E1015. */

/* Symbolic links one path may pass through before it is refused with "too many links"
 * (architecture 7, D236): Linux's own limit. */
#define MO_LIMIT_PATH_LINKS 40

/* Native filesystem resolver bytes, including NUL. Paths or expanded link
 * targets past this produce Failed(path, "path too long"). Fake paths use
 * charged dynamic storage instead, and are not constrained by OS path buffers. */
#define MO_LIMIT_HOST_PATH_SIZE 4096

/* Bytes in one bytecode file. */
#define MO_LIMIT_BYTECODE_SIZE (256 << 20)

/* Bytes of bytecode text input or output (bytecode.md section 10). The text tools refuse more;
 * this does not change the binary file limit. Canonical text fits within eight times its file's
 * bytes, including quoting and instruction names. Use wide unsigned arithmetic: 2 GiB exceeds
 * a signed 32-bit int, but text error offsets must still fit u32. */
#define MO_LIMIT_BYTECODE_TEXT_SIZE (UINT64_C(8) * MO_LIMIT_BYTECODE_SIZE)

/* Decimal spelling of a shorthand BigInt. Larger magnitudes use the linear hex spelling;
 * this bounds the repeated multiply-by-ten conversion independently of the binary limit. */
#define MO_LIMIT_BYTECODE_BIGINT_DIGITS 4096

/* Parameters of one bytecode function, including a closure's implicit env. One extra slot
 * holds a promise's result or a closure's env after the largest source signature. Runtime
 * function types may also have this many parameters, for generated promise signatures. */
#define MO_LIMIT_BYTECODE_PARAMETERS (MO_LIMIT_PARAMETERS + 1)

/* Entries in the strings section. */
#define MO_LIMIT_STRINGS (1 << 22)

/* Bytes in one string. A text literal can fill its file, so this is at least MO_LIMIT_FILE_SIZE.
 * It bounds a constant's bytes, too. */
#define MO_LIMIT_STRING_SIZE MO_LIMIT_FILE_SIZE

/* Entries in the types section. */
#define MO_LIMIT_TYPES (1 << 20)

/* Entries in the layouts section. */
#define MO_LIMIT_LAYOUTS (1 << 20)

/* Entries in the constants section. */
#define MO_LIMIT_CONSTANTS (1 << 20)

/* Entries in the globals section: the module constants. */
#define MO_LIMIT_GLOBALS (1 << 20)

/* Entries in the init section. Each init function sets one global, and each global is set by one
 * (bytecode.md section 9), so there are as many as there are globals. */
#define MO_LIMIT_INITS MO_LIMIT_GLOBALS

/* Entries in the functions section. */
#define MO_LIMIT_FUNCTIONS (1 << 20)

/* Entries in the modules section: one per source file. */
#define MO_LIMIT_MODULES MO_LIMIT_FILES

/* One service per module; distinct local handler names share the module bound. */
#define MO_LIMIT_SERVICES MO_LIMIT_MODULES
#define MO_LIMIT_SERVICE_HANDLERS MO_LIMIT_MODULE_NAMES

/* Entries in the debug section (bytecode.md section 3.4). */
#define MO_LIMIT_DEBUG_ENTRIES (1 << 24)

/* 32-bit words of one function's code. The largest instruction, a switch over
 * MO_LIMIT_CHOICE_CASES cases, must fit. */
#define MO_LIMIT_FUNCTION_CODE_WORDS (1 << 20)

/* Registers times basic blocks in one function. Lowering splits a function before crossing
 * this bound, which keeps ownership joins and defect-report replay within a small budget. */
#define MO_LIMIT_FUNCTION_REGISTER_BLOCKS (1 << 22)

/* Verification steps and scratch bytes per encoded register/code word, plus 32 header words.
 * These are load-error ceilings, also applied to defect-report replay. */
#define MO_LIMIT_VERIFY_WORK_PER_WORD 128
#define MO_LIMIT_VERIFY_BYTES_PER_WORD 2048

/* Variable operands of one instruction: a call's arguments, a switch's targets, the fields
 * construct takes (bytecode.md section 4.1). The head word holds their number in 16 bits. */
#define MO_LIMIT_OPERANDS 65535

/* A running program. Each limit here is a bug. */

/* Real Files and ReadFiles handles open at once, each holding its folder open: within past it
 * stops the program (bytecode.md section 7). It stays well below the 1,024 descriptors a process
 * usually gets, leaving room for the files the program reads and writes. */
#define MO_LIMIT_OPEN_FOLDERS 256

/* Lookup-wide component attempts, including portable ancestor reconstruction.
 * A refusal is Failed naming this limit, not a language bug. Test builds may
 * lower it to exercise exhaustion without millions of system calls. */
#ifndef MO_LIMIT_PATH_STEPS
#define MO_LIMIT_PATH_STEPS (1U << 18)
#endif

/* Type-ability preparation has a work cap. Runtime printing uses its caller's
 * output budget; equality, ordering and hashing have no work cap. Scratch uses the remaining heap.
 * Insertion grows its index after this probe bound; searches probe to empty.
 * Collection lengths keep doubled cell indices in u32. */
#define MO_LIMIT_HEAP_WORK (64 << 20)
#define MO_LIMIT_HEAP_PROBES 128
#define MO_LIMIT_HEAP_INDEX_SLOTS (1U << 30)
#define MO_LIMIT_COLLECTION_SIZE (1 << 26)

/* Runtime Text, Bytes and BigInt lengths are u32. This is a representation
 * ceiling, not the source/constant size limit; allocation uses --max-heap. */
#define MO_LIMIT_VALUE_BYTES UINT32_MAX

/* Inclusive UTC microseconds of Instant: years 1 through 9999 (reference 3.9).
 * A time outside this range is a bug. */
#define MO_TIME_MIN_US INT64_C(-62135596800000000)
#define MO_TIME_MAX_US INT64_C(253402300799999999)

/* Best-effort diagnostic output, outside an exhausted process heap. */
#define MO_LIMIT_DEFECT_TEXT_SIZE (64 << 10)

/* The limits a run sets on its processes: mo_image_init and mo_process_new take them (architecture
 * section 12). Passing one is a bug with a report: "out of stack" for the first two (bytecode.md
 * section 5.1), and running out of memory for the heap (reference 13.2). */
struct mo_limits {
    /* Bytes of the VM's stack: frame headers, 16-byte cells, one state byte per
     * register and alignment. A tail call replaces its frame, possibly growing
     * it within this limit (bytecode.md section 5.2). */
    size_t stack_size;
    /* Frames on the stack at once. */
    uint32_t call_depth;
    /* Bytes a process's heap may hold. */
    size_t heap_size;
};

/* The defaults of mo run's and mo test's --max-stack, --max-depth and --max-heap (reference 13.4,
 * D233), until Phase 3's budgets replace them. They are conservative: the stack a C program gets
 * from Linux by default, a call depth that a recursion over a large collection reaches and a
 * recursive walk of a tree doesn't, and a heap that fits on a small machine. A run that needs more
 * sets more with those flags. */
#define MO_DEFAULT_STACK_SIZE (8 << 20)
#define MO_DEFAULT_CALL_DEPTH 10000
#define MO_DEFAULT_HEAP_SIZE (1 << 30)

/* D304/D308. Service calls share these fixed limits, including initialization.
 * Memory includes retained state and queued values; stack and heap partitions
 * together fit MESSAGE_MEMORY. Mailbox/timer metadata has a separate bound. */
#define MO_LIMIT_SERVICE_MAILBOX 64
#define MO_LIMIT_SERVICE_WORK (1 << 20)
#define MO_LIMIT_SERVICE_MESSAGE_MEMORY (64 << 20)
#define MO_LIMIT_SERVICE_METADATA (8 << 20)
#define MO_LIMIT_SERVICE_FAILURES 5
#define MO_LIMIT_SERVICE_FAILURE_WINDOW_US UINT64_C(60000000)
#define MO_LIMIT_SERVICE_RESTARTS 3
#define MO_LIMIT_SERVICE_RESTART_WINDOW_US UINT64_C(600000000)
#define MO_LIMIT_SIMULATION_TICKS 10000
#define MO_LIMIT_SIMULATION_REPORT 1024

/* Durable records and bounded value/schema walks, independent of native layout. */
#define MO_LIMIT_SAVED_BYTES (16 << 20)
#define MO_LIMIT_SAVED_DEPTH 128
#define MO_LIMIT_SAVED_NODES (1 << 20)
#define MO_LIMIT_SERVICE_SNAPSHOT_MESSAGES 100
/* Open stores per process. Their SQLite memory shares one budget sized for each
 * disk store's worst operation, one record plus a bounded page cache. */
#define MO_LIMIT_STORES 8
#define MO_LIMIT_STORE_CACHE_KIB 256
#define MO_LIMIT_STORE_MEMORY (MO_LIMIT_STORES * (24 << 20))
#define MO_LIMIT_STORE_SQL 4096
/* One SQLite row: a framed payload plus SQLite's record header. */
#define MO_LIMIT_STORE_ROW (MO_LIMIT_SAVED_BYTES + 4096)

static inline struct mo_limits mo_limits_default(void) {
    return (struct mo_limits){
        .stack_size = MO_DEFAULT_STACK_SIZE,
        .call_depth = MO_DEFAULT_CALL_DEPTH,
        .heap_size = MO_DEFAULT_HEAP_SIZE,
    };
}

/* What the toolchain keeps of its own. */

/* Compiler-generated identifiers (service fix adapters), including the terminating NUL.
 * Prefixes stay shorter so a decimal suffix still fits. */
#define MO_LIMIT_GENERATED_NAME 64
#define MO_LIMIT_GENERATED_PREFIX 48

/* Nesting of trusted prims.def and hostops.def signature templates. The parsers
 * assert this; they never walk a source type graph. */
#define MO_LIMIT_DEF_SIGNATURE_DEPTH 16

/* Mo's own option words and operands, not borrowed file lists or forwarded
 * program arguments (the OS bounds those). The command name is length-bounded
 * too, but doesn't count toward the option budgets. Mistakes exit 2. */
#define MO_CLI_ARGUMENTS 256
#define MO_CLI_ARGUMENT_SIZE 4096
#define MO_CLI_ARGUMENT_BYTES (64 << 10)
#define MO_CLI_ERROR_SIZE 4096 /* one escaped, newline-free command error */

/* One complete rendered diagnostic (including JSON escaping). Oversize records
 * are dropped whole and counted in totals; never emit a partial JSON object. */
#define MO_CLI_DIAGNOSTIC_SIZE (1 << 20)

/* The staged surface pipeline's one arena. E1054 while parsing, E1058 while checking. */
#define MO_CLI_CHECK_SIZE (512 << 20)

/* Compiler execution of each promise anchor, and initialization for that check.
 * Exhaustion refuses the check with E1405, not a conclusion about the promise.
 * This compiler work bound never limits the user's running main. */
#define MO_CLI_ANCHOR_INSTRUCTIONS (16 << 20)
#define MO_CLI_SERVE_TICK_US UINT64_C(200000)

/* Observation inventory/selection and image binding, not Mo execution limits.
 * Result records use MO_CLI_DIAGNOSTIC_SIZE and never truncate fields. */
#define MO_CLI_TEST_SIZE (64 << 20)
#define MO_CLI_TEST_WORK (64 << 20)
#define MO_CLI_TEST_VARIABLES 4096

/* Architecture 10.1: additional subject trials after a false promise.
 * --calls bounds the preceding search; both share the same deadline (D242). */
#define MO_CLI_PROMISE_SHRINK_CALLS 200

/* Generated fake capabilities one input tuple may hold (a promise's capability parameters, and
 * those inside its collections). A bigger tuple is refused like any oversized input. */
#define MO_CLI_FAKE_RECIPES 256

/* Architecture 10.1, hidden types: a value comes from at most four calls to its module, a start
 * and up to three steps. A start whose result doesn't count (None, an Error, a stop) is tried
 * again with other inputs, up to the attempts limit; a random value makes half as many random
 * attempts before those. A builder's result reaches the value through
 * at most MO_CLI_BUILDER_PATH tuple fields, Somes and Oks. */
#define MO_CLI_BUILDER_CALLS 4
#define MO_CLI_BUILDER_ATTEMPTS 64
#define MO_CLI_BUILDER_PATH 4
/* A builder may take other hidden types, each built first by its own factory. Levels make that
 * finite: a type is level 0 when a start needs no other hidden type, else one more than the
 * highest level its start needs, and a builder takes only hidden types of lower levels. Levels
 * stop at MO_CLI_BUILDER_LEVELS - 1, so one value nests at most this many builds, and a cycle
 * gets no level. */
#define MO_CLI_BUILDER_LEVELS 3
/* Companions one input tuple keeps (D284): for each hidden value built for it, the first other
 * hidden value its last call returned in the same tuple. Past this many, later ones keep none. */
#define MO_CLI_BUILDER_COMPANIONS 16
/* Optional replay records per trial. Exhaustion never changes ordinary input generation. */
#define MO_CLI_BUILDER_RECIPES 4096
/* Includes invalid replay edits which never reach a subject call. */
#define MO_CLI_BUILDER_SHRINK_ATTEMPTS 4096

/* Promise input generation, not limits on program results. The caller can
 * choose smaller budgets. Refusal leaves the sequence unchanged; tools may
 * skip an oversized candidate or retry it with more heap. */
#define MO_LIMIT_INPUT_DEPTH 256
#define MO_LIMIT_INPUT_NODES 65536
#define MO_LIMIT_INPUT_ELEMENTS 4096
#define MO_LIMIT_INPUT_BYTES (1 << 20)
#define MO_LIMIT_INPUT_WORK (64 << 20)
#define MO_LIMIT_INPUT_METADATA (128 << 20)

/* Formatter document storage and nested groups. Output is additionally bounded
 * by the caller's buffer and the lexer's per-line character limit. */
#define MO_FORMAT_DOCUMENT_SIZE (128 << 20)
#define MO_FORMAT_DOCUMENT_DEPTH (32 * MO_LIMIT_NESTING_DEPTH + 32)

/* Diagnostics one run keeps. mo_diags_add refuses more, and counts them, so the run can say that
 * it stopped early (src/base/diag.h). */
#define MO_LIMIT_DIAGNOSTICS 1000

/* Bytes the diagnostics may hold: their labels, messages and fixes. mo_diags_add refuses a
 * diagnostic that doesn't fit, as it does past MO_LIMIT_DIAGNOSTICS. */
#define MO_LIMIT_DIAGNOSTICS_SIZE (64 << 20)

/* Edits in one fix. mo_diags_add checks every pair of a fix's edits for overlap, and this bounds
 * that work. A stage gives no fix rather than one with more: mo_diags_add asserts it. */
#define MO_LIMIT_FIX_EDITS 1024

/* `mo fix` checks a file and applies the fixes it can, at most this many times, so a fix that
 * waited on an overlapping one, or one the first fixes uncovered, still lands. Each
 * round takes at most MO_LIMIT_FIX_ROUND_EDITS edits, which bounds its overlap checks. */
#define MO_LIMIT_FIX_ROUNDS 8
#define MO_LIMIT_FIX_ROUND_EDITS 4096

#define MO_LIMIT_MCP_TOKEN_BYTES 256
#define MO_LIMIT_MCP_HEADER_BYTES (1 << 10)
#define MO_LIMIT_MCP_LINE_BYTES (8 << 10)
#define MO_LIMIT_MCP_HEADER_FIELDS 64
#define MO_LIMIT_MCP_HEADER_HEAP (1 << 20)
#define MO_LIMIT_MCP_HEADER_WORK UINT64_C(1000000)
#define MO_LIMIT_MCP_BODY_BYTES (16 << 10)
#define MO_LIMIT_MCP_REQUEST_WORK UINT64_C(32000000)
#define MO_LIMIT_MCP_BIGINT_DIGITS 4096
#define MO_LIMIT_MCP_VALUE_DEPTH 60
#define MO_LIMIT_MCP_CONNECTIONS 8
#define MO_LIMIT_MCP_INGRESS_BYTES (24 << 10)
#define MO_LIMIT_MCP_RESPONSE_BYTES (32 << 10)
#define MO_LIMIT_MCP_RESPONSE_NODES (MO_LIMIT_MCP_RESPONSE_BYTES / 2 - 1)
#define MO_LIMIT_MCP_CHUNKS 1024
#define MO_LIMIT_MCP_TRAILER_BYTES (16 << 10)
#define MO_LIMIT_MCP_TRAILER_FIELDS 64
#define MO_LIMIT_MCP_REQUEST_MEMORY (16 << 20)
#define MO_LIMIT_MCP_CATALOG_MEMORY (64 << 20)
#define MO_LIMIT_MCP_TYPE_NODES 4096
#define MO_LIMIT_MCP_TYPE_EDGES 16384
#define MO_LIMIT_MCP_SCHEMA_BYTES (256 << 10)
#define MO_LIMIT_MCP_PAGE_TOOLS 32
#define MO_LIMIT_MCP_PAYLOAD_NODES 16384
#define MO_LIMIT_MCP_ENVELOPE_NODES 64
#define MO_LIMIT_MCP_ID_BYTES 128
#define MO_LIMIT_MCP_HEADER_US UINT64_C(5000000)
#define MO_LIMIT_MCP_BODY_US UINT64_C(10000000)
#define MO_LIMIT_MCP_WRITE_US UINT64_C(10000000)
#define MO_LIMIT_MCP_DRAIN_BYTES (512 << 10)
#define MO_LIMIT_MCP_DRAIN_US UINT64_C(250000)
#define MO_LIMIT_MCP_RATE 20
#define MO_LIMIT_MCP_BURST 40

/* How the limits depend on each other and on the formats. */
_Static_assert(MO_LIMIT_FILE_SIZE < UINT32_MAX, "spans hold byte offsets in 32 bits");
_Static_assert(MO_LIMIT_PROGRAM_SIZE >= MO_LIMIT_FILE_SIZE, "a program can be one large file");
_Static_assert(MO_LIMIT_FIX_ROUND_EDITS >= MO_LIMIT_FIX_EDITS, "a round can take any one fix");
_Static_assert(MO_LIMIT_FIELDS <= MO_LIMIT_OPERANDS, "construct takes every field as an operand");
_Static_assert(MO_LIMIT_CHOICE_CASES <= MO_LIMIT_OPERANDS, "switch takes a target per case");
_Static_assert(MO_LIMIT_BYTECODE_PARAMETERS <= MO_LIMIT_OPERANDS,
               "a call takes each argument as an operand");
_Static_assert(MO_LIMIT_BYTECODE_PARAMETERS <= MO_LIMIT_FUNCTION_REGISTERS,
               "parameters, including env, are registers");
_Static_assert(MO_LIMIT_OPERANDS <= UINT16_MAX, "the head word holds the operand count in 16 bits");
_Static_assert(MO_LIMIT_FUNCTION_CODE_WORDS >= 2 + MO_LIMIT_CHOICE_CASES,
               "the largest switch fits in a function");
_Static_assert(MO_LIMIT_FUNCTION_INSTRUCTIONS <= MO_LIMIT_FUNCTION_CODE_WORDS,
               "every instruction takes at least one word");
_Static_assert(MO_LIMIT_STRING_SIZE >= MO_LIMIT_FILE_SIZE, "a text literal can fill its file");
_Static_assert(MO_LIMIT_MODULES >= MO_LIMIT_FILES, "every source file is a module");
_Static_assert(MO_LIMIT_BYTECODE_SIZE >= MO_LIMIT_STRING_SIZE, "the longest string fits in a file");
_Static_assert(MO_LIMIT_BYTECODE_TEXT_SIZE == UINT64_C(8) * MO_LIMIT_BYTECODE_SIZE,
               "canonical bytecode text fits within eight times the binary limit");
_Static_assert(MO_LIMIT_BYTECODE_TEXT_SIZE <= UINT32_MAX, "bytecode text offsets fit u32");
_Static_assert(MO_LIMIT_GENERATED_PREFIX < MO_LIMIT_GENERATED_NAME,
               "generated prefixes are shorter than generated names");
_Static_assert(MO_TIME_MIN_US < 0, "year 1 is before the Unix epoch");
_Static_assert(MO_TIME_MAX_US > 0, "year 9999 is after the Unix epoch");

#endif
