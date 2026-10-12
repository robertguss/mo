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

/* How deeply interpolations may nest inside each other's text, as the executable grammar allows
 * (grammar/lexer.c uses this limit). The lexer refuses deeper ones: E1007. */
#define MO_LIMIT_INTERPOLATION_DEPTH 16

/* Names a module declares: its functions, types and constants. The checker refuses more: E1008. */
#define MO_LIMIT_MODULE_NAMES 65535

/* Parameters of one function, and so arguments of one call. The checker refuses more: E1009. The
 * loader refuses a function with more. */
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

/* Registers of one function, its parameters included. The code generator refuses a function that
 * needs more: E1013. The loader refuses a function with more. */
#define MO_LIMIT_FUNCTION_REGISTERS 65535

/* Instructions in one function's code. The code generator refuses a function that compiles to
 * more, or to more than MO_LIMIT_FUNCTION_CODE_WORDS words: E1014. */
#define MO_LIMIT_FUNCTION_INSTRUCTIONS (1 << 18)

/* What a bytecode file holds (bytecode.md section 3). Each limit here is a load error: the loader
 * checks every count and index before using it. The code generator reports a program whose file
 * would pass one as too big for the format: E1015. */

/* Bytes in one bytecode file. */
#define MO_LIMIT_BYTECODE_SIZE (256 << 20)

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

/* Entries in the debug section (bytecode.md section 3.4). */
#define MO_LIMIT_DEBUG_ENTRIES (1 << 24)

/* 32-bit words of one function's code. The largest instruction, a switch over
 * MO_LIMIT_CHOICE_CASES cases, must fit. */
#define MO_LIMIT_FUNCTION_CODE_WORDS (1 << 20)

/* Variable operands of one instruction: a call's arguments, a switch's targets, the fields
 * construct takes (bytecode.md section 4.1). The head word holds their number in 16 bits. */
#define MO_LIMIT_OPERANDS 65535

/* A running program. Each limit here is a bug. */

/* Real Files and ReadFiles handles open at once, each holding its folder open: within past it
 * stops the program (bytecode.md section 7). It stays well below the 1,024 descriptors a process
 * usually gets, leaving room for the files the program reads and writes. */
#define MO_LIMIT_OPEN_FOLDERS 256

/* The limits a run sets on its processes: mo_image_init and mo_process_new take them (architecture
 * section 12). Passing one is a bug with a report: "out of stack" for the first two (bytecode.md
 * section 5.1), and running out of memory for the heap (reference 13.2). */
struct mo_limits {
    /* Bytes of the VM's stack, which holds the frames: each frame's registers, 16 bytes each. A
     * tail call reuses its frame (bytecode.md section 5.2). */
    size_t stack_size;
    /* Frames on the stack at once. */
    uint32_t call_depth;
    /* Bytes a process's heap may hold. */
    size_t heap_size;
};

/* The defaults. Architecture section 13.1 leaves them open until Phase 2 measures them, and
 * Phase 3's budgets will replace them, so they are conservative: the stack a C program gets from
 * Linux by default, a call depth that a recursion over a large collection reaches and a recursive
 * walk of a tree doesn't, and a heap that fits on a small machine. A run that needs more sets
 * more. */
#define MO_DEFAULT_STACK_SIZE (8 << 20)
#define MO_DEFAULT_CALL_DEPTH 10000
#define MO_DEFAULT_HEAP_SIZE (1 << 30)

static inline struct mo_limits mo_limits_default(void) {
    return (struct mo_limits){
        .stack_size = MO_DEFAULT_STACK_SIZE,
        .call_depth = MO_DEFAULT_CALL_DEPTH,
        .heap_size = MO_DEFAULT_HEAP_SIZE,
    };
}

/* What the toolchain keeps of its own. */

/* Diagnostics one run keeps. mo_diags_add refuses more, and counts them, so the run can say that
 * it stopped early (src/base/diag.h). */
#define MO_LIMIT_DIAGNOSTICS 1000

/* Bytes the diagnostics may hold: their labels, messages and fixes. mo_diags_add refuses a
 * diagnostic that doesn't fit, as it does past MO_LIMIT_DIAGNOSTICS. */
#define MO_LIMIT_DIAGNOSTICS_SIZE (64 << 20)

/* Edits in one fix. mo_diags_add checks every pair of a fix's edits for overlap, and this bounds
 * that work. A stage gives no fix rather than one with more: mo_diags_add asserts it. */
#define MO_LIMIT_FIX_EDITS 1024

/* How the limits depend on each other and on the formats. */
_Static_assert(MO_LIMIT_FILE_SIZE < UINT32_MAX, "spans hold byte offsets in 32 bits");
_Static_assert(MO_LIMIT_PROGRAM_SIZE >= MO_LIMIT_FILE_SIZE, "a program can be one large file");
_Static_assert(MO_LIMIT_FIELDS <= MO_LIMIT_OPERANDS, "construct takes every field as an operand");
_Static_assert(MO_LIMIT_CHOICE_CASES <= MO_LIMIT_OPERANDS, "switch takes a target per case");
_Static_assert(MO_LIMIT_PARAMETERS <= MO_LIMIT_OPERANDS,
               "a call takes each argument as an operand");
_Static_assert(MO_LIMIT_PARAMETERS <= MO_LIMIT_FUNCTION_REGISTERS, "parameters are registers");
_Static_assert(MO_LIMIT_OPERANDS <= UINT16_MAX, "the head word holds the operand count in 16 bits");
_Static_assert(MO_LIMIT_FUNCTION_CODE_WORDS >= 2 + MO_LIMIT_CHOICE_CASES,
               "the largest switch fits in a function");
_Static_assert(MO_LIMIT_FUNCTION_INSTRUCTIONS <= MO_LIMIT_FUNCTION_CODE_WORDS,
               "every instruction takes at least one word");
_Static_assert(MO_LIMIT_STRING_SIZE >= MO_LIMIT_FILE_SIZE, "a text literal can fill its file");
_Static_assert(MO_LIMIT_MODULES >= MO_LIMIT_FILES, "every source file is a module");
_Static_assert(MO_LIMIT_BYTECODE_SIZE >= MO_LIMIT_STRING_SIZE, "the longest string fits in a file");

#endif
