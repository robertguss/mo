/* Loading, intermediate checks and the checked-program contract.
 * Only mo_check certifies a program for lowering; intermediate stages do not.
 * Arrays and names belong to arena; interface declaration and origin pointers borrow the loaded
 * syntax, which must outlive the interfaces. Syntax owns its token text
 * independently of sources. */
#ifndef MO_CHECK_H
#define MO_CHECK_H

#include "syntax/syntax.h"

struct mo_module_set {
    const char *root;
    uint32_t count;
    struct mo_module_syntax **modules; /* entry first, then discovery order */
    uint32_t *order;                   /* dependencies before their users; indices into modules */
    const char **names;                /* dotted module names */
    bool *library;
    uint32_t **uses; /* one target index per use line, in header order */
    /* Input to mo_check for exact spec text; need only outlive checking.
     * May be NULL for a synthetic set without specifications. */
    const struct mo_sources *sources;
};

/* Loads the entry and its transitive uses. sources must be initialized and empty.
 * A NULL library means no library folder; a non-NULL folder must exist. Every
 * .mo library file is included, recursively, without entering linked folders.
 * Errors return NULL, retaining sources and diagnostics. An unreadable entry
 * has an unavailable primary file (UINT32_MAX).
 * The entry's spelling must end in its reference 2.1 module path. */
MO_MUST_USE struct mo_module_set *mo_load_program(struct mo_arena *arena,
                                                  struct mo_sources *sources, const char *entry,
                                                  const char *library, struct mo_diags *diags);

MO_MUST_USE struct mo_module_set *mo_load_program_sources(struct mo_arena *arena,
                                                          struct mo_sources *sources,
                                                          const bool *library,
                                                          struct mo_diags *diags);

struct mo_symbol {
    const char *name;
    const struct mo_syntax *declaration;
    uint32_t owner;    /* owning choice's symbol index, otherwise UINT32_MAX */
    uint32_t receiver; /* first parameter's local nominal type, otherwise UINT32_MAX */
    uint32_t case_count;
    uint32_t overload_count; /* name-group size on its first member; one on later members */
    uint32_t name_length;
    const uint32_t *cases; /* choice's symbol indices in declaration order; shared by imports */
    uint32_t module;
    bool exported;
    bool insides; /* type fields/cases are exposed */
};

struct mo_imported_name {
    const char *name;
    const struct mo_symbol *symbols; /* all overloads travel together */
    uint32_t count;
    const struct mo_syntax *origin;
};

struct mo_module_alias {
    const char *name;
    uint32_t module;
    const struct mo_syntax *origin;
};

struct mo_module_scope {
    const char *name; /* arena-owned dotted name; NULL for an unnamed synthetic module set */
    bool library;
    uint32_t count;
    struct mo_symbol *symbols; /* sorted by name, then source position */
    uint32_t imported_count;
    struct mo_imported_name *imported; /* sorted by name */
    uint32_t alias_count;
    struct mo_module_alias *aliases; /* sorted by name; separate from type names */
};

struct mo_module_interfaces {
    uint32_t count;
    struct mo_module_scope *scopes; /* same indices as the loaded set */
};

/* Looks up a declaration without copying or requiring a terminating NUL. The
 * first overload is returned; later overloads are adjacent in symbols. */
MO_MUST_USE const struct mo_symbol *mo_find_symbol(const struct mo_module_scope *scope,
                                                   const uint8_t *name, size_t length);

/* Indexes declarations, validates overload ownership, exposes public interfaces
 * and resolves explicit imports and their choice cases. NULL on error. Does not
 * check body names, types, uses of hidden insides or unused imports; those need
 * body checking. set must be a successful mo_load_program result, or its equivalent:
 * complete syntax, valid target indices and an acyclic dependency order. */
MO_MUST_USE struct mo_module_interfaces *
mo_resolve_modules(struct mo_arena *arena, const struct mo_module_set *set, struct mo_diags *diags);

enum mo_source_type_kind {
    MO_SOURCE_NAMED,
    MO_SOURCE_PARAMETER,
    MO_SOURCE_FUNCTION,
    MO_SOURCE_TUPLE,
};

/* Canonical annotation types: equal types have the same pointer within one
 * preparation. Named types use module UINT32_MAX for built-ins; type parameters
 * belong to their declaring function or data type. Function arguments include the result in
 * the last slot. Declaration pointers borrow the module interfaces, except
 * immutable native parameter owners, which have no source declaration. */
struct mo_source_type {
    enum mo_source_type_kind kind;
    const char *name;
    uint32_t module;
    const struct mo_symbol *declaration;
    uint32_t count;
    const struct mo_source_type *const *arguments;
    uint32_t height;     /* longest path to a leaf; a leaf has height zero */
    uint32_t parameters; /* mask of abstract single-letter parameters A through Z */
};

struct mo_function_signature {
    const struct mo_symbol *function;
    const struct mo_source_type *type;
    uint8_t requirements[26]; /* ability mask per scoped A-Z parameter */
};

struct mo_field_signature {
    const struct mo_syntax *name;       /* NULL for a case's single unnamed value */
    const struct mo_syntax *annotation; /* the written field type */
    const struct mo_source_type *type;
    uint32_t index; /* declaration order */
};

/* One record or choice case. Generic fields refer to parameters owned by the
 * declaring type, not an implicit function parameter. Recursive fields retain
 * nominal types; they never expand the record's layout. */
struct mo_data_signature {
    const struct mo_symbol *symbol;
    const struct mo_source_type *type; /* the record or owning choice */
    const struct mo_syntax *parameters;
    uint32_t count;
    struct mo_field_signature *fields;          /* sorted by name; indices keep declaration order */
    const struct mo_field_signature **by_index; /* declaration order */
    uint32_t hash_parameters;                   /* scoped parameters needed for structural Hash */
    uint32_t key_parameters; /* implicit Hash requirements imposed by Map/Set fields */
    bool unnamed;
};

struct mo_signatures {
    uint32_t count;
    struct mo_function_signature *items; /* module order, then interface symbol order */
    uint64_t *work; /* shared program budget, initialized by preparation; single-threaded */
    uint32_t data_count;
    struct mo_data_signature *data; /* module order, then interface symbol order */
    uint32_t native_count;
    struct mo_native_signature *native; /* private prepared table interfaces */
    uint32_t nominal_count, customizer_count;
    struct mo_nominal_signature *nominals;            /* private owner index and dependency masks */
    const struct mo_function_signature **customizers; /* private receiver index */
    uint32_t import_count;
    struct mo_import_usage *imports; /* private written-origin usage, independent of symbols */
    bool recovering; /* private diagnostic recovery; never true in a published preparation */
    const struct mo_program *service_program;
};

/* Prepares all named function signatures, including abstract generic parameters
 * and default Unit results, and record/case field types. NULL with diagnostics
 * on failure. This does not check function bodies or abilities; it is not a checked program.
 * Result types belong to arena and borrow interfaces, which must outlive them. */
MO_MUST_USE struct mo_signatures *
mo_prepare_signatures(struct mo_arena *arena, const struct mo_module_set *set,
                      const struct mo_module_interfaces *interfaces, struct mo_diags *diags);

struct mo_inferred_expression {
    const struct mo_syntax *syntax;
    const struct mo_source_type *type;
    const struct mo_syntax *binding;        /* binder/parameter; assignment's original var binder */
    const struct mo_symbol *symbol;         /* resolved function or constructor, otherwise NULL */
    const struct mo_field_signature *field; /* selected field or supplied named argument */
    const char *builtin_case;               /* built-in constructor, otherwise NULL */
    bool never; /* every path leaves the current function or loop, or stops */
};

struct mo_inferred_body {
    const struct mo_function_signature *function;
    uint32_t count;
    struct mo_inferred_expression *items; /* visited expressions/statements, in postorder */
};

/* Infers one named function's body against its prepared signature. Local names,
 * positional calls, lambdas, scalar/collection expressions, variables and
 * control flow are supported. E1103 explicitly refuses unfinished forms.
 * Records, cases, fields and typed patterns check construction, hidden insides,
 * exhaustiveness and the no-whole-arm choice catch-all rule. Dot dispatch and
 * delayed overload selection finish in this walk. This is not mo_check: ability
 * proofs and polymorphic recursion belong to that walk. Capability/capture flow,
 * specs, constants, unused imports and main's contract still need their checking
 * stages. Equal scalar literals and repeated bare names in map literals are rejected.
 * A success is not suitable for lowering.
 * NULL with the earliest retained independent source error in this body; a resource
 * failure stops immediately and takes priority.
 * Output belongs to arena and borrows syntax, interfaces and signatures. Equal
 * inferred types are structurally equal; pointer identity is not required.
 * Types are shared DAGs: consumers must memoize walks, not expand shared paths.
 * E1104 bounds inference work per function; E1107 bounds its sum across calls
 * using these signatures. Prepare new signatures to start a new checking run.
 * E1105 bounds inferred type paths independently of source nesting; E1106
 * bounds recursive inference frames. The limits are in base/limits.h. All
 * temporary and result storage uses the caller arena, with E1058 on exhaustion.
 * Syntax and type walks use depth-bounded C recursion; run source stages via
 * base/stack.h with MO_SOURCE_STACK_SIZE, not the platform's default stack.
 * No scratch allocation bypasses the caller arena. */
MO_MUST_USE struct mo_inferred_body *
mo_infer_function(struct mo_arena *arena, const struct mo_module_interfaces *interfaces,
                  const struct mo_signatures *signatures, uint32_t function,
                  struct mo_diags *diags);

/* Checked-program contract for lower. mo_check fills these records;
 * unfinished stages return E1103. None of the intermediate APIs above
 * certifies these records.
 * This is an in-memory compiler interface, not a serialized or stable ABI. */

struct mo_type_argument {
    const struct mo_source_type *parameter; /* scoped MO_SOURCE_PARAMETER, not just its letter */
    const struct mo_source_type *type;      /* may use the enclosing generic's rigid parameters */
};

/* Function uses list implicit parameters in A–Z order; data uses list explicit
 * parameters in declaration order. Empty for a nongeneric use. Every generic
 * function value, call, constructor and pattern retains its substitution. */
struct mo_type_arguments {
    uint32_t count;
    const struct mo_type_argument *items;
};

enum mo_source_ability {
    MO_SOURCE_EQUAL,
    MO_SOURCE_HASH,
    MO_SOURCE_ORDER,
    MO_SOURCE_TEXT,
    MO_SOURCE_NUMBER,
};

enum mo_ability_implementation_kind {
    MO_ABILITY_NATIVE,
    MO_ABILITY_DERIVED,
    MO_ABILITY_FUNCTION,
    MO_ABILITY_PARAMETER,
};

/* A shared graph, possibly recursive for nominal data; lower memoizes it.
 * Checking bounds dependency paths by MO_LIMIT_SOURCE_ABILITY_DEPTH (E1108),
 * independently of the height of a surface type's arguments.
 * FUNCTION is the selected declaring-module to_text/compare, even if hidden.
 * PARAMETER is an abstract requirement of a generic body; substitution selects
 * its concrete implementation before emitting Core. DERIVED parts follow
 * tuple/field declaration order. Choices flatten every case's fields in
 * case/field declaration order, not one proof per case: Option has only Some's
 * payload; Result has Ok's payload then Error's. Empty cases contribute zero.
 * List/Set have one element part; Map has key then value. NATIVE is determined
 * by ability and type.
 * Equal/Hash never select a user function.
 *
 * Every generic use's type-only abilities (operand UINT32_MAX) identify the
 * callee's scoped parameter in mo_checked_ability_use.parameter; the pair
 * (parameter, implementation->ability), never the concrete type alone, selects
 * the replacement for a PARAMETER proof during monomorphization. FUNCTION
 * parts carry the customizer's own requirement proofs, with corresponding
 * scoped keys in part_parameters. For to_text(Box(T)) where T: Text, selecting
 * Box(Int)'s customizer supplies its T -> Int substitution and a Text(Int)
 * part keyed by that customizer's T. This also applies to entry.error_text.
 * Hash/Order signatures include their implied Equal requirement. Graphs may
 * recurse through nominal data; lowering memoizes them. DERIVED has no
 * part_parameters: its parts are structural, not signature requirements.
 * Proofs may be shared for structurally equal actual types: implementation.type
 * need not be the same pointer as a use's substituted actual type. The scoped
 * parameter keys, not concrete-type pointers, retain canonical identity. */
struct mo_ability_implementation {
    enum mo_source_ability ability;
    enum mo_ability_implementation_kind kind;
    const struct mo_source_type *type;
    const struct mo_symbol *function; /* non-NULL only for FUNCTION */
    struct mo_type_arguments arguments;
    uint32_t part_count;
    const struct mo_ability_implementation *const *parts;
    const struct mo_source_type *const *part_parameters; /* FUNCTION requirement keys */
};

struct mo_checked_binding {
    const struct mo_syntax *syntax; /* canonical name; NULL only for promise's implicit result */
    const char *name;
    struct mo_span span;
    const struct mo_source_type *type;
    bool variable;
};

/* Exactly one member for a resolved constructor; both NULL means absent. A
 * native constructor uses its reference spelling, e.g. Some, Error or Entry.
 * Its instantiated nominal type supplies its arguments. */
struct mo_constructor_ref {
    const struct mo_symbol *symbol;
    const char *builtin;
};

enum mo_checked_pattern_kind {
    MO_PATTERN_ANY,
    MO_PATTERN_BIND,
    MO_PATTERN_LITERAL,
    MO_PATTERN_TUPLE,
    MO_PATTERN_LIST,
    MO_PATTERN_CONSTRUCTOR,
};

struct mo_checked_pattern;
struct mo_checked_pattern_part {
    uint32_t index; /* tuple slot or declared field; list cons: 0=head, 1=tail */
    const struct mo_checked_pattern *pattern;
};

/* Named fields are in declaration order; omitted fields become ANY. A list is
 * normalized to empty (zero parts) or cons (two parts), including its rest.
 * Alternatives share canonical binding pointers, not just matching spellings.
 * Omitted-field ANY nodes are immutable and may be shared; their syntax names
 * the declaring field. Ignored _ has no binding entry. LITERAL retains its
 * source spelling and already range-checked contextual type, with no expression
 * rows emitted for its spelling or unary minus. */
struct mo_checked_pattern {
    enum mo_checked_pattern_kind kind;
    const struct mo_syntax *syntax;
    const struct mo_source_type *type;
    const struct mo_checked_binding *binding; /* BIND only */
    struct mo_constructor_ref constructor;
    struct mo_type_arguments arguments;
    uint32_t ability_count;
    const struct mo_checked_ability_use *abilities; /* keyed type-only constructor requirements */
    uint32_t part_count;
    const struct mo_checked_pattern_part *parts;
};

struct mo_checked_arm {
    const struct mo_syntax *syntax;
    uint32_t pattern_count;
    const struct mo_checked_pattern *const *patterns; /* alternatives in written order */
    uint32_t guard; /* body expression index, UINT32_MAX when absent */
    uint32_t body;  /* body expression index */
};

struct mo_checked_match {
    uint32_t scrutinee; /* body expression index */
    uint32_t arm_count;
    const struct mo_checked_arm *arms; /* written order; coverage already proved */
};

enum mo_checked_or { MO_CHECK_OR_ABSENT, MO_CHECK_OR_BOOL, MO_CHECK_OR_OPTION };

/* The source type decides Option versus Result. failure is None or Error in
 * the enclosing result; wrapper is absent for a direct propagation, or the
 * unique single-unnamed-field case selected by reference 8.3. Neither lower nor
 * codegen searches for a wrapper or chooses among candidates. Successful input
 * and enclosing result payloads need not have the same type. A generic wrapper
 * carries its substitution and keyed type-only requirements, just like an
 * explicit constructor; direct propagation has neither. */
struct mo_checked_try {
    const struct mo_source_type *result; /* enclosing function/lambda result */
    struct mo_constructor_ref failure;
    struct mo_constructor_ref wrapper;
    struct mo_type_arguments arguments; /* wrapper substitution */
    uint32_t ability_count;
    const struct mo_checked_ability_use *abilities; /* keyed wrapper requirements, type-only */
};

struct mo_checked_lambda {
    uint32_t number; /* one-based source order within the containing function/constant/spec */
    uint32_t parameter_count;
    /* NULL for _; its type is in the lambda expression's function type. */
    const struct mo_checked_binding *const *parameters;
    uint32_t body; /* expression index in the containing checked body */
    uint32_t capture_count;
    const struct mo_checked_binding *const *captures; /* first-use order, capture by value */
};

struct mo_checked_ability_use {
    uint32_t operand; /* body row, or UINT32_MAX for a type-only generic-use requirement */
    const struct mo_ability_implementation *implementation;
    const struct mo_source_type *parameter; /* scoped callee parameter; NULL for an operation */
};

/* Typed expression graph in written postorder. operands refer to earlier rows
 * of this body, including separate rows for every normalized operator/postfix
 * step. The syntax supplies its kind, literals, text and responsible span;
 * operation is the resolved operator token, or MO_T_EOF for a non-operator.
 * Operands retain source evaluation order, including guards/branches/lambda
 * bodies, which lower must keep lazy according to their syntax kind.
 * No name lookup, overload choice, field lookup or inference remains for lower.
 * All types contain only concrete types and scoped rigid parameters, never
 * inference variables. Bound names, including parameters/patterns, have types
 * in the binding table even when they need no expression row. Generic uses
 * include every required ability implementation, including function values;
 * lower substitutes those proofs for PARAMETER uses in the generic body.
 *
 * SEQUENCE operands are statement roots in order; the last supplies its value,
 * or Unit when empty. GROUP/EXPR_STMT forward their single operand. IF/LINE_IF
 * operands are condition, body, then each else-if condition/body, then an
 * optional else body. RETURN has an optional guard first, then an optional
 * value; syntax distinguishes bare return from guarded bare return. Guards
 * run before the lazy value, even though the value is written first.
 * OR has left/default operands, with a lazy right operand for both Bool and
 * Option. An Option OR returns its payload type; its never flag comes from
 * the mandatory left only. TRY has one input operand and try_ metadata; its
 * failure returns from the nearest function/lambda, not the containing named
 * function when inside a lambda. Neither form introduces constructor rows
 * for its implicit failure/default paths.
 *
 * WHILE operands are condition/body; FOR operands are iterable/body. Both
 * return Unit. The body is lazy, and WHILE re-evaluates its condition each
 * round. FOR binding is its canonical element name, or NULL for _ or a tuple;
 * pattern is the normalized element binder, including ignored/tuple slots.
 * BREAK/CONTINUE have zero operands or one guard, and target the nearest loop
 * in the same function/lambda. Match arms have their own binding scopes;
 * pattern alternatives share canonical binders. IS has one scrutinee operand
 * and its normalized pattern; its bindings follow reference 7.4's true path.
 *
 * A new binding or whole-var assignment has one initializer/value operand.
 * Tuple binding has a normalized binder pattern instead of a single binding.
 * Compound assignment's value is an explicit operator row that borrows the
 * assignment syntax; operation is authoritative and that row is not a write.
 * A field assignment has its target projection then its value, with binding
 * pointing at the base var. Projection operands recursively retain the field
 * path; do not mutate the projected value independently of that base var.
 * For a compound field write, the same projection row is operand zero of both
 * the write and its RHS operator. Evaluate it once, not once per reference.
 * Constructors have payload operands only, in written order. A NAMED_ARG
 * operand wraps its value and supplies its declaration-order field index.
 * An unnamed payload is the sole operand, at field zero. Field/projection
 * rows have one base operand and field's resolved index.
 *
 * Ordinary calls have callee first, then positional arguments. Source dot
 * calls have selected callee, receiver, then explicit arguments; a function
 * field is projected first and called as an ordinary callee, with no inserted
 * receiver. Native dot calls omit a callee row; native names the operation,
 * and operands begin with its receiver. receiver is that row index, not a slot.
 * Ability dot calls use native "to_text" or "compare", receiver first and no
 * callee row; their Text/Order use identifies the receiver. Interpolation has
 * only hole operands, in written order, each with a Text use keyed by its row.
 * Evaluate and convert each hole before evaluating the next. Literal fragments
 * remain in syntax.children (HEAD/hole/MID/.../TAIL), not executable rows;
 * lowering decodes them using the whole triple-quoted text's indentation.
 * Lambdas have one lazy body operand, also named by lambda->body.
 *
 * Intermediate flat operator rows borrow the operator TOKEN's syntax. A
 * final chain/postfix row borrows the whole owner node; its last syntax step
 * determines the kind, while operation/field/symbol/native resolve the use.
 * Numeric leaves keep their spelling: lower must fold a literal's immediate
 * unary minus before constructing a scalar, including Int's signed minimum. */
struct mo_checked_expression {
    const struct mo_syntax *syntax;
    const struct mo_source_type *type;
    uint32_t operand_count;
    const uint32_t *operands;
    enum mo_syntax_token operation;
    const struct mo_checked_binding *binding; /* read, new binding, or assignment target */
    const struct mo_symbol *symbol; /* selected function, constant or declared constructor */
    const char *native; /* native case spelling, or qualified native function/field spelling */
    uint32_t service, tool;
    uint32_t field;    /* declaration-order index, UINT32_MAX unless selecting/supplying a field */
    uint32_t receiver; /* operand row inserted by dot dispatch, UINT32_MAX otherwise */
    struct mo_type_arguments arguments;
    uint32_t ability_count;
    const struct mo_checked_ability_use *abilities;
    enum mo_checked_or or_kind;
    const struct mo_checked_try *try_;
    const struct mo_checked_pattern *pattern; /* is, for or tuple binder; case uses match */
    const struct mo_checked_match *match;
    const struct mo_checked_lambda *lambda;
    bool assignment; /* writes binding's original var; false for a new binding */
    bool never;      /* every path leaves the current function/loop or stops */
};

struct mo_checked_body {
    uint32_t count;
    const struct mo_checked_expression *items;
    uint32_t root; /* index in items; UINT32_MAX only for an anchor's argument-only body */
    uint32_t capture_free_parameters; /* letters of type parameters anonymous functions capture */
    uint32_t binding_count;
    const struct mo_checked_binding *bindings; /* includes lambdas and canonical alternatives */
};

struct mo_checked_function {
    const struct mo_function_signature *signature;
    uint32_t parameter_count;
    const struct mo_checked_binding *const *parameters; /* signature order, in body.bindings */
    struct mo_checked_body body; /* includes typed nested anonymous functions */
};

struct mo_checked_constant {
    const struct mo_symbol *symbol;
    const struct mo_source_type *type;
    struct mo_checked_body initializer;
};

enum mo_service_reply { MO_SERVICE_READ_ONLY, MO_SERVICE_UPDATE, MO_SERVICE_RESULT_UPDATE };

struct mo_checked_service_tool {
    const struct mo_syntax *name;
    uint32_t function; /* index in program.functions */
    enum mo_service_reply reply;
};

struct mo_checked_service_timer {
    const struct mo_syntax *syntax;
    uint32_t function;
    struct mo_checked_body interval;
};

struct mo_checked_service {
    const struct mo_syntax *syntax;
    uint32_t module;
    const struct mo_source_type *state;
    struct mo_checked_body initializer;
    uint32_t tool_count;
    const struct mo_checked_service_tool *tools;
    uint32_t timer_count;
    const struct mo_checked_service_timer *timers;
    const struct mo_syntax *token_source; /* environment-name literal, never a secret value */
};

/* Exact original source bytes, not decoded/unescaped wording. May contain NUL;
 * length is authoritative. span names those bytes in the original sources. */
struct mo_spec_text {
    const uint8_t *bytes;
    size_t length;
    struct mo_span span;
};

struct mo_checked_condition {
    struct mo_spec_text text;
    uint32_t expression; /* Bool row in the spec body */
};

enum mo_checked_spec_kind { MO_SPEC_EXAMPLE, MO_SPEC_TEST, MO_SPEC_PROMISE, MO_SPEC_ANCHOR };

struct mo_checked_spec {
    enum mo_checked_spec_kind kind;
    const struct mo_syntax *syntax;
    uint32_t module;
    /* One-based source ordinal before rewrites: tests per module, examples/promises
     * per subject, anchors per promise and allows/forbids expectation. */
    uint32_t number;
    uint32_t subject;         /* index in program.functions, UINT32_MAX for test */
    uint32_t promise;         /* index in program.specs for anchor, UINT32_MAX otherwise */
    struct mo_spec_text text; /* original header/line */
    const uint8_t *wording;   /* decoded test/promise sentence, otherwise NULL */
    size_t wording_length;    /* may contain NUL; do not use strlen */
    uint32_t parameter_count;
    const struct mo_checked_binding *const *parameters; /* promise: subject params then result */
    struct mo_checked_body body; /* example/promise: Bool; test: Unit; anchor: value rows only */
    struct mo_type_arguments arguments; /* anchor's generic promise instantiation */
    /* Anchor requirements: type-only uses, keyed by the subject's scoped
     * parameters, just as a call of that subject. No Text proof for details. */
    uint32_t ability_count;
    const struct mo_checked_ability_use *abilities;
    uint32_t value_count;
    /* Anchor values are indexed by promise-parameter order. Rows themselves
     * retain written evaluation order, even when names are supplied out of order. */
    const uint32_t *values;
    uint32_t condition_count;
    const struct mo_checked_condition *conditions; /* promise Bool lines, in written order */
    bool allows; /* anchor expectation only: true=allows, false=forbids */
};

enum mo_checked_entry_result { MO_ENTRY_UNIT, MO_ENTRY_STATUS, MO_ENTRY_RESULT };

struct mo_checked_entry {
    uint32_t function; /* index in program.functions; main in set.modules[0], or UINT32_MAX */
    enum mo_checked_entry_result result;
    uint32_t capability_count;
    const struct mo_checked_binding *const *capabilities; /* main parameters in written order */
    const struct mo_ability_implementation *error_text;   /* Text for returned error, else NULL */
};

/* Only a successful mo_check certifies these invariants. interfaces and
 * signatures are prepared in the result arena and belong to this program;
 * set and syntax are borrowed and must outlive it. Names, type DAGs, typed
 * bodies, substitutions, ability graphs and spec byte copies belong to arena;
 * native spellings and scoped native parameter templates may borrow immutable compiler tables.
 * It borrows no mo_sources bytes: lower takes the original sources separately
 * to copy filenames/provenance into its own Core arena. All spans retain the
 * original file indices. Do not mutate syntax, interfaces or signatures after
 * checking. The current inference work counter isn't a lowering input.
 * Functions/specs follow module discovery order, then declaration/source order.
 * Constants alone are dependency-ordered (3.4); independent ties follow source
 * order. Module ownership/exposure remains in set/interfaces, not that order. */
struct mo_program {
    const struct mo_module_set *set;
    const struct mo_module_interfaces *interfaces;
    const struct mo_signatures *signatures;
    uint32_t function_count;
    const struct mo_checked_function *functions;
    uint32_t constant_count;
    const struct mo_checked_constant *constants;
    uint32_t spec_count;
    const struct mo_checked_spec *specs;
    uint32_t service_count;
    const struct mo_checked_service *services;
    struct mo_checked_entry entry;
};

/* Composes the implemented checks, returning a lowering-ready program or NULL
 * with diagnostics, never a partially checked result. set must satisfy
 * mo_resolve_modules' input contract. Use the explicit source-stage stack.
 * Checking covers generic functions with scoped requirement proofs, scalar
 * expressions/operators, direct/indirect calls, records/choices and fields, normalized
 * patterns, tuples/lists, conditionals, loops, early
 * exits/asserts, capturing lambdas, lazy Option defaults and nearest-function/lambda try with
 * unique error wrapping. Native signatures use the shared tables; operators,
 * ability dot calls and interpolation retain native/derived/custom/parameter proofs. Source
 * dot/overload dispatch and loaded library calls retain the selected symbols. Written Map/Set
 * types require Hash. main takes distinct capabilities in written order and returns Unit, Int
 * or Result(Unit, E) where E: Text. Invalid entry signatures report E0079 (or E0080 when E lacks
 * Text); a library module without main has entry.function == UINT32_MAX.
 * Source example lines are zero-parameter Bool bodies, with their subject index,
 * per-subject ordinal and an arena-owned copy of their original bytes/span.
 * set.sources must cover the syntax's original file indices for spec checking.
 * Promises bind subject parameters then implicit result, retain numbered Bool
 * condition rows and original spans, and combine them with short-circuit and.
 * Tests are zero-parameter Unit bodies in their module, with decoded wording.
 * Anchors check names, completeness, typing and generic requirements. Their
 * arguments/abilities specialize the promise's scoped parameters, not a new scope.
 * Constants infer closed types from their initializers, independently of their
 * uses, and are ordered through checked reads/calls/callbacks. Cycles are E0089;
 * unresolved indirect-call or abstract callback dependencies remain E1103.
 * Imports retain declaring symbol/type identities and count annotation/value references;
 * unused written module/name routes report E0093 after successful body/spec checking.
 * Imported generic functions use their declaring signature's parameter keys and
 * the caller's actual-type ability proofs, as local uses do. Imported generic data
 * annotations/construction/fields/patterns retain their declaring nominal identity
 * and declaration-ordered parameter keys; hidden insides remain private.
 * Foreign constant reads and initializer calls/values keep declaring symbols,
 * closed inferred types and the same checked dependency order as local uses.
 * Making a function value does not execute its body or its type-only proofs.
 * Existing implicit library routes are unchanged.
 * Spec-only capability operations are allowed only in spec bodies, including their lambdas;
 * constants and ordinary functions still refuse them with E1103. Unfinished body forms also
 * return E1103.
 * Spec checking checks anchor typing, completeness and links but executes no source code:
 * tools compile/run anchors to verify their allows/forbids expectations before accepting a
 * command-line mo check. Unfinished stages must report E1103, not succeed. */
MO_MUST_USE struct mo_program *mo_check(struct mo_arena *arena, const struct mo_module_set *set,
                                        struct mo_diags *diags);

#endif
