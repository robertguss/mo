/* CBMC proofs of the bytecode reader, src/bytecode/read.c (V4, MO-54).
 *
 * Each function the reader is built from has a specification here: what it requires of the
 * reader, and what it guarantees. Each specification is used twice:
 *
 * - harness_<name> gives the real function any state its requirements allow, calls it, and
 *   asserts its guarantees, including that it changed nothing it is not allowed to change;
 * - model_<name> asserts the requirements, then forgets everything the function may change and
 *   assumes only its guarantees. cbmc/prove.sh replaces each callee by its model, so every proof
 *   covers one function and stays small.
 *
 * Together the proofs show that the reader, on any input of up to MO_CBMC_INPUT bytes, reads and
 * writes only inside its objects, never overflows a signed integer, never loses a value in a
 * conversion, never fails an MO_ASSERT, and refuses with an error inside the input.
 * cbmc/README.md says what the proofs assume.
 *
 * read.c is included rather than linked, so that its static functions can be called here.
 * utf8.c is included so the reader's UTF-8 check is the shared decoder. */
#include "base/utf8.c"
#include "bytecode/read.c"

#include <stdlib.h>

#ifndef MO_CBMC_INPUT
#define MO_CBMC_INPUT 32
#endif
#define N MO_CBMC_INPUT

/* The most entries a table can have in N bytes (each entry takes at least 4), and the largest
 * native entry the reader allocates. */
#define MOST_ENTRIES (N / 4)
#define LARGEST_ENTRY sizeof(struct mo_bytecode)

size_t nondet_size(void);
bool nondet_bool(void);
uint32_t nondet_u32(void);
uint8_t nondet_u8(void);
enum mo_case_shape nondet_shape(void);
struct mo_load_error nondet_error(void);
struct mo_type nondet_type(void);
struct mo_layout nondet_layout(void);
struct mo_bytecode_module nondet_module(void);
struct mo_bytecode_debug nondet_debug(void);
/* A pointer to no object: a model returns one for a table its callers store but never read, so a
 * caller that did read it would fail its proof. */
void *nondet_pointer(void);

/* ---- Specifications ----------------------------------------------------------------------- */

/* A reader: offset and error position inside an input of at most N readable bytes, and a
 * refusal, once made, with its reason and its position inside the input. */
#define READER(r)                                                                                  \
    ((r)->offset <= (r)->length && (r)->length <= N && (r)->field <= (r)->length &&                \
     __CPROVER_r_ok((r)->bytes, (r)->length) &&                                                    \
     (!(r)->failed || ((r)->error->reason != NULL && (r)->error->offset <= (r)->length)))

/* What every reading function guarantees: still a reader, offset only moved forward, and a
 * failed reader stays failed. */
#define READS(r, old) (READER(r) && (r)->offset >= (old).offset && (!(old).failed || (r)->failed))

/* A table of n entries of size bytes: absent when empty, otherwise n * size bytes that end where
 * their object ends, so a read past the table is a bounds failure. */
#define TABLE(p, n, size)                                                                          \
    ((n) == 0 ? (p) == NULL                                                                        \
              : (__CPROVER_r_ok((p), (size_t)(n) * (size)) &&                                      \
                 __CPROVER_OBJECT_SIZE(p) - __CPROVER_POINTER_OFFSET(p) == (size_t)(n) * (size)))

/* A new table for a model. Its object has a fixed size, the most the input allows, which CBMC
 * handles far better than a size it must solve for, and the entry type the reader stores in it,
 * which spares CBMC from treating the object as bytes. The table ends where the object ends. */
#define NEW_TABLE_OF(T)                                                                            \
    if (size == sizeof(T)) {                                                                       \
        T *p = malloc(n * sizeof(T));                                                              \
        __CPROVER_assume(p != NULL);                                                               \
        return p;                                                                                  \
    }

static void *new_table(size_t n, size_t size) {
    size_t most = size >= 4 ? MOST_ENTRIES + 1 : N + 1; /* entries of 4 bytes or more, or bytes */
    __CPROVER_assert(n <= most, "a model's table is bounded by the input");
    NEW_TABLE_OF(uint8_t)
    NEW_TABLE_OF(uint32_t)
    NEW_TABLE_OF(struct mo_bytecode)
    NEW_TABLE_OF(struct mo_bytecode_string)
    NEW_TABLE_OF(struct mo_type)
    NEW_TABLE_OF(struct mo_type_field)
    NEW_TABLE_OF(struct mo_type_case)
    NEW_TABLE_OF(struct mo_layout)
    NEW_TABLE_OF(struct mo_bytecode_constant)
    NEW_TABLE_OF(struct mo_bytecode_global)
    NEW_TABLE_OF(struct mo_bytecode_function)
    NEW_TABLE_OF(struct mo_parameter)
    NEW_TABLE_OF(struct mo_bytecode_module)
    NEW_TABLE_OF(struct mo_bytecode_type_export)
    NEW_TABLE_OF(struct mo_bytecode_bytes)
    NEW_TABLE_OF(struct mo_bytecode_debug)
    NEW_TABLE_OF(struct mo_bytecode_debug_positions)
    NEW_TABLE_OF(struct mo_bytecode_span)
    NEW_TABLE_OF(struct mo_bytecode_register_name)
    NEW_TABLE_OF(struct mo_bytecode_spec_line)
    uint8_t *p = malloc(most * size);
    __CPROVER_assume(p != NULL);
    return p + (most - n) * size;
}

/* The file's strings and the name table over them, as name needs them. */
#define STRINGS(r)                                                                                 \
    ((r)->file->string_count <= MOST_ENTRIES &&                                                    \
     ((r)->file->string_count == 0 ||                                                              \
      (__CPROVER_r_ok((r)->file->strings,                                                          \
                      (size_t)(r)->file->string_count * sizeof(struct mo_bytecode_string)) &&      \
       __CPROVER_r_ok((r)->names.canonical, (size_t)(r)->file->string_count * sizeof(uint32_t)))))

/* A decoded type table, shared by the file and the reader. */
#define TYPES(r)                                                                                   \
    ((r)->file->type_count <= MOST_ENTRIES && (r)->types == (const void *)(r)->file->types &&      \
     TABLE((r)->types, (r)->file->type_count, sizeof(struct mo_type)))

/* Every choice's cases are bounded, so a loop over them ends. */
static bool cases_bounded(const struct reader *r) {
    for (uint32_t i = 0; i < r->file->type_count; i++) {
        if (r->types[i].kind == MO_TYPE_CHOICE && r->types[i].case_count > MOST_ENTRIES) {
            return false;
        }
    }
    return true;
}

/* A function may change only the fields it names: after restoring those in before, every
 * field of before and after must be the same. */
static bool same_reader(const struct reader *a, const struct reader *b) {
    return a->arena == b->arena && a->scratch.chunk == b->scratch.chunk &&
           a->scratch.used == b->scratch.used && a->scratch.limit == b->scratch.limit &&
           a->scratch.next_chunk_size == b->scratch.next_chunk_size &&
           a->names.items == b->names.items && a->names.pointers == b->names.pointers &&
           a->names.canonical == b->names.canonical && a->names.count == b->names.count &&
           a->bytes == b->bytes && a->length == b->length && a->offset == b->offset &&
           a->field == b->field && a->code_positions == b->code_positions &&
           a->debug_positions == b->debug_positions && a->file == b->file && a->types == b->types &&
           a->layouts == b->layouts && a->error == b->error && a->failed == b->failed;
}

static bool same_file(const struct mo_bytecode *a, const struct mo_bytecode *b) {
    return a->header.major == b->header.major && a->header.minor == b->header.minor &&
           a->header.flags == b->header.flags && a->header.checksum == b->header.checksum &&
           a->string_count == b->string_count && a->strings == b->strings &&
           a->type_count == b->type_count && a->types == b->types &&
           a->layout_count == b->layout_count && a->layouts == b->layouts &&
           a->constant_count == b->constant_count && a->constants == b->constants &&
           a->global_count == b->global_count && a->globals == b->globals &&
           a->init_count == b->init_count && a->init == b->init &&
           a->function_count == b->function_count && a->functions == b->functions &&
           a->module_count == b->module_count && a->modules == b->modules &&
           a->file_count == b->file_count && a->files == b->files &&
           a->debug_count == b->debug_count && a->debug == b->debug;
}

#define UNCHANGED(before, after) same_reader(&(before), &(after))

/* Forgets what a reading function may change and assumes it is still a reader. */
static void havoc_reading(struct reader *r) {
    r->offset = nondet_size();
    r->field = nondet_u32();
    r->failed = nondet_bool();
    *r->error = nondet_error();
}

/* ---- Models ------------------------------------------------------------------------------- */

uint32_t model_word(struct reader *r) {
    __CPROVER_assert(READER(r), "word: requires");
    struct reader old = *r;
    havoc_reading(r);
    uint32_t value = nondet_u32();
    bool fails = old.failed || old.length - old.offset < 4;
    __CPROVER_assume(READS(r, old) && r->failed == fails &&
                     r->offset == (fails ? old.offset : old.offset + 4));
    if (fails) {
        __CPROVER_assume(value == 0);
    } else {
        const uint8_t *b = r->bytes + old.offset;
        __CPROVER_assume(value == ((uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 |
                                   (uint32_t)b[3] << 24));
    }
    return value;
}

bool model_boolean(struct reader *r) {
    __CPROVER_assert(READER(r), "boolean: requires");
    struct reader old = *r;
    havoc_reading(r);
    __CPROVER_assume(READS(r, old) && r->offset <= old.offset + 4);
    return nondet_bool();
}

/* allocate and scratch: nothing for a failed reader or an empty table, otherwise a new table or
 * a no-memory refusal. */
static void *model_block(struct reader *r, size_t n, size_t size, bool zero) {
    struct reader old = *r;
    if (old.failed || n == 0) {
        return NULL;
    }
    if (nondet_bool()) {
        r->failed = true;
        *r->error = nondet_error();
        __CPROVER_assume(READER(r));
        return NULL;
    }
    void *p = new_table(n, size);
    if (zero) {
        memset(p, 0, n * size);
    }
    return p;
}

void *model_allocate(struct reader *r, size_t n, size_t size, size_t alignment) {
    __CPROVER_assert(READER(r) && (r->failed || n <= N + 1) && size >= 1 && size <= LARGEST_ENTRY &&
                         alignment >= 1 && alignment <= 16 && (alignment & (alignment - 1)) == 0,
                     "allocate: requires");
    return model_block(r, n, size, true);
}

void *model_scratch(struct reader *r, uint32_t n, size_t size, size_t alignment) {
    __CPROVER_assert(READER(r) && (r->failed || n <= MOST_ENTRIES) && size >= 1 &&
                         size <= LARGEST_ENTRY && alignment >= 1 && alignment <= 16 &&
                         (alignment & (alignment - 1)) == 0,
                     "scratch: requires");
    return model_block(r, n, size, false);
}

/* array and indices: a count within its limit and the bytes left, then its table. */
static void *model_table(struct reader *r, uint32_t *n, uint32_t limit, size_t minimum, size_t size,
                         bool filled) {
    struct reader old = *r;
    havoc_reading(r);
    *n = nondet_u32();
    __CPROVER_assume(READS(r, old));
    if (r->failed) {
        __CPROVER_assume(*n == 0);
        return NULL;
    }
    __CPROVER_assume(r->offset == old.offset + 4 && *n <= limit &&
                     *n <= (r->length - r->offset) / minimum);
    if (filled) {
        r->offset += 4 * (size_t)*n;
    }
    if (*n == 0) {
        return NULL;
    }
    return new_table(*n, size);
}

void *model_array(struct reader *r, uint32_t *n, uint32_t limit, size_t minimum, size_t size,
                  size_t alignment) {
    __CPROVER_assert(READER(r) && minimum >= 4 && size >= 1 && size <= LARGEST_ENTRY &&
                         alignment >= 1 && alignment <= 16 && (alignment & (alignment - 1)) == 0,
                     "array: requires");
    return model_table(r, n, limit, minimum, size, false);
}

const uint32_t *model_indices(struct reader *r, uint32_t *n, uint32_t limit) {
    __CPROVER_assert(READER(r), "indices: requires");
    return model_table(r, n, limit, 4, sizeof(uint32_t), true);
}

bool model_utf8(const uint8_t *bytes, uint32_t length) {
    __CPROVER_assert(length <= N && __CPROVER_r_ok(bytes, length), "utf8: requires");
    return nondet_bool();
}

const uint8_t *model_blob(struct reader *r, uint32_t *n, bool terminate) {
    __CPROVER_assert(READER(r), "blob: requires");
    struct reader old = *r;
    havoc_reading(r);
    *n = nondet_u32();
    __CPROVER_assume(READS(r, old));
    if (r->failed) {
        __CPROVER_assume(*n == 0);
        return NULL;
    }
    __CPROVER_assume(r->offset == old.offset + 4 + *n && *n <= MO_LIMIT_STRING_SIZE);
    size_t size = *n + (terminate ? 1U : 0U);
    if (size == 0) {
        return NULL;
    }
    return new_table(size, 1);
}

const char *model_name(struct reader *r, bool optional) {
    (void)optional;
    __CPROVER_assert(READER(r) && STRINGS(r), "name: requires");
    struct reader old = *r;
    havoc_reading(r);
    __CPROVER_assume(READS(r, old) && r->offset <= old.offset + 4);
    return NULL; /* the reader stores names, never reads them */
}

const struct mo_type_field *model_fields(struct reader *r, uint32_t *n, bool optional) {
    (void)optional;
    __CPROVER_assert(READER(r) && STRINGS(r), "fields: requires");
    struct reader old = *r;
    havoc_reading(r);
    *n = nondet_u32();
    __CPROVER_assume(READS(r, old) && *n <= MOST_ENTRIES);
    if (*n == 0) {
        return NULL;
    }
    return new_table(*n, sizeof(struct mo_type_field));
}

void model_choice_cases(struct reader *r, struct mo_type *t) {
    __CPROVER_assert(READER(r) && STRINGS(r) && __CPROVER_w_ok(t, sizeof *t),
                     "choice_cases: requires");
    struct reader old = *r;
    havoc_reading(r);
    __CPROVER_assume(READS(r, old));
    t->name = nondet_pointer();
    t->case_count = nondet_u32();
    __CPROVER_assume(t->case_count <= MOST_ENTRIES);
    t->cases = nondet_pointer();
    t->has_fields = nondet_bool();
}

void model_type_entry(struct reader *r, struct mo_type *t) {
    __CPROVER_assert(READER(r) && STRINGS(r) && __CPROVER_w_ok(t, sizeof *t) &&
                         t->kind <= MO_TYPE_CAPABILITY,
                     "type_entry: requires");
    struct reader old = *r;
    havoc_reading(r);
    __CPROVER_assume(READS(r, old));
    struct mo_type entry = nondet_type();
    entry.kind = t->kind;
    entry.defined = t->defined;
    entry.has_fields = nondet_bool();
    entry.reference = nondet_bool();
    __CPROVER_assume(entry.kind != MO_TYPE_CHOICE || entry.case_count <= MOST_ENTRIES);
    *t = entry;
}

void model_parameters(struct reader *r, struct mo_bytecode_function *fn) {
    __CPROVER_assert(READER(r) && __CPROVER_w_ok(fn, sizeof *fn), "parameters: requires");
    struct reader old = *r;
    havoc_reading(r);
    __CPROVER_assume(READS(r, old));
    fn->parameter_count = nondet_u32();
    fn->parameters = nondet_pointer();
}

void model_module_entry(struct reader *r, struct mo_bytecode_module *m) {
    __CPROVER_assert(READER(r) && __CPROVER_w_ok(m, sizeof *m), "module_entry: requires");
    struct reader old = *r;
    havoc_reading(r);
    __CPROVER_assume(READS(r, old));
    *m = nondet_module();
}

void model_layout_entry(struct reader *r, struct mo_layout *l) {
    __CPROVER_assert(READER(r) && __CPROVER_w_ok(l, sizeof *l), "layout_entry: requires");
    struct reader old = *r;
    havoc_reading(r);
    __CPROVER_assume(READS(r, old));
    *l = nondet_layout();
}

void model_file_entry(struct reader *r, struct mo_bytecode_bytes *file) {
    __CPROVER_assert(READER(r) && __CPROVER_w_ok(file, sizeof *file), "file_entry: requires");
    struct reader old = *r;
    havoc_reading(r);
    __CPROVER_assume(READS(r, old));
    file->length = nondet_u32();
    file->bytes = nondet_pointer();
}

void model_debug_entry(struct reader *r, struct mo_bytecode_debug *d,
                       struct mo_bytecode_debug_positions *positions) {
    __CPROVER_assert(READER(r) && __CPROVER_w_ok(d, sizeof *d) &&
                         __CPROVER_w_ok(positions, sizeof *positions),
                     "debug_entry: requires");
    struct reader old = *r;
    havoc_reading(r);
    __CPROVER_assume(READS(r, old));
    *d = nondet_debug();
    positions->spans = nondet_u32();
    positions->names = nondet_u32();
    positions->metadata = nondet_u32();
    positions->lines = nondet_u32();
}

void model_types(struct reader *r) {
    __CPROVER_assert(READER(r) && STRINGS(r), "types: requires");
    struct reader old = *r;
    havoc_reading(r);
    __CPROVER_assume(READS(r, old));
    uint32_t count = nondet_u32();
    __CPROVER_assume(count <= MOST_ENTRIES);
    struct mo_type *p = count == 0 ? NULL : new_table(count, sizeof *p);
    for (uint32_t i = 0; i < count; i++) {
        p[i] = nondet_type();
        __CPROVER_assume(p[i].kind != MO_TYPE_CHOICE || p[i].case_count <= MOST_ENTRIES);
    }
    r->file->type_count = count;
    r->file->types = p;
    r->types = p;
}

void model_constants(struct reader *r) {
    __CPROVER_assert(READER(r) && (r->failed || TYPES(r)), "constants: requires");
    struct reader old = *r;
    havoc_reading(r);
    __CPROVER_assume(READS(r, old));
    r->file->constant_count = nondet_u32();
    r->file->constants = nondet_pointer();
}

void model_functions(struct reader *r) {
    __CPROVER_assert(READER(r), "functions: requires");
    struct reader old = *r;
    havoc_reading(r);
    __CPROVER_assume(READS(r, old));
    uint32_t count = nondet_u32();
    __CPROVER_assume(count <= MOST_ENTRIES);
    r->file->function_count = count;
    r->file->functions = nondet_pointer();
    r->code_positions = count == 0 ? NULL : new_table(count, sizeof(uint32_t));
}

void model_sections(struct reader *r) {
    __CPROVER_assert(READER(r) && !r->failed, "sections: requires");
    struct reader old = *r;
    havoc_reading(r);
    __CPROVER_assume(READS(r, old));
    if (r->failed) {
        return;
    }
    uint32_t count = nondet_u32();
    __CPROVER_assume(count <= MOST_ENTRIES);
    struct mo_type *types = count == 0 ? NULL : new_table(count, sizeof *types);
    for (uint32_t i = 0; i < count; i++) {
        types[i] = nondet_type();
        __CPROVER_assume(types[i].kind != MO_TYPE_CHOICE || types[i].case_count <= MOST_ENTRIES);
    }
    r->file->type_count = count;
    r->file->types = types;
    r->types = types;
    count = nondet_u32();
    __CPROVER_assume(count <= MOST_ENTRIES);
    r->file->function_count = count;
    r->code_positions = count == 0 ? NULL : new_table(count, sizeof(uint32_t));
}

/* ---- Neighbours ---------------------------------------------------------------------------- */

/* An MO_ASSERT that fails is a failed proof, not the end of a path. */
_Noreturn void mo_assert_failed(const char *expression, const char *file, int line) {
    (void)expression;
    (void)file;
    (void)line;
    __CPROVER_assert(0, "MO_ASSERT holds");
    __CPROVER_assume(0);
    abort();
}

/* The arena: each block is its own object of exactly the size asked, and any allocation may
 * fail. cbmc/arena.c proves that the real arena's blocks are that large and overlap no other. */
void mo_arena_init(struct mo_arena *arena, size_t limit) {
    *arena = (struct mo_arena){.limit = limit};
}

void mo_arena_free(struct mo_arena *arena) {
    (void)arena;
}

void *mo_arena_allocate_array(struct mo_arena *arena, size_t count, size_t element_size,
                              size_t alignment) {
    __CPROVER_assert(alignment != 0 && (alignment & (alignment - 1)) == 0, "a valid alignment");
    if (element_size != 0 && count > SIZE_MAX / element_size) {
        return NULL;
    }
    size_t size = count * element_size;
    if (nondet_bool() || size > arena->limit) {
        return NULL;
    }
    return malloc(size);
}

void *mo_arena_copy(struct mo_arena *arena, const void *bytes, size_t size, size_t alignment) {
    void *p = mo_arena_allocate_array(arena, size, 1, alignment);
    if (p != NULL && size != 0) {
        memcpy(p, bytes, size);
    }
    return p;
}

/* A byte-by-byte memcpy: CBMC's own copies a whole object at once, which is costly when the
 * source offset and the length are both unknown. */
void *memcpy(void *destination, const void *source, size_t n) {
    uint8_t *d = destination;
    const uint8_t *s = source;
    for (size_t i = 0; i < n; i++) {
        d[i] = s[i];
    }
    return destination;
}

/* CBMC's library has no memchr. */
void *memchr(const void *s, int c, size_t n) {
    const unsigned char *p = s;
    for (size_t i = 0; i < n; i++) {
        if (p[i] == (unsigned char)c) {
            return (void *)(p + i);
        }
    }
    return NULL;
}

/* The name table: each string's canonical index is absent or no later than its own, which is
 * all the reader relies on. */
bool mo_bytecode_names_init(struct mo_bytecode_names *names, struct mo_arena *arena,
                            const struct mo_bytecode *file) {
    (void)arena;
    *names = (struct mo_bytecode_names){.count = file->string_count};
    if (names->count == 0) {
        return true;
    }
    if (nondet_bool()) {
        return false;
    }
    names->canonical = malloc((size_t)names->count * sizeof(uint32_t));
    __CPROVER_assume(names->canonical != NULL);
    for (uint32_t i = 0; i < names->count; i++) {
        uint32_t c = nondet_u32();
        __CPROVER_assume(c == MO_BYTECODE_NO_INDEX || c <= i);
        names->canonical[i] = c;
    }
    return true;
}

/* The validator has its own tests and fuzzing; here it may accept or refuse anything read. */
bool mo_bytecode_validate(const struct mo_bytecode *file, const struct mo_bytecode_read_info *info,
                          struct mo_load_error *error) {
    (void)file;
    (void)info;
    if (nondet_bool()) {
        return true;
    }
    *error = nondet_error();
    __CPROVER_assume(error->reason != NULL && error->offset <= info->wire_size);
    return false;
}

enum mo_case_shape mo_case_shape(const struct mo_types *types, uint32_t type, uint32_t which) {
    __CPROVER_assert(type < types->count, "case shape of a decoded type");
    __CPROVER_assert(which < types->items[type].case_count, "case shape of a decoded case");
    return nondet_shape();
}

/* ---- Harnesses ---------------------------------------------------------------------------- */

/* Every harness ends here, so prove.sh --reach can check that some input gets to the end of it
 * (the blocks of the harness itself include assertions' unreachable failure branches). */
void reached_end(void) {
}

static uint8_t input[N > 0 ? N : 1];

static void *object(size_t size) {
    void *p = malloc(size);
    __CPROVER_assume(p != NULL);
    return p;
}

/* Any input of up to N bytes. It ends where its object ends, so reading past it fails. */
static const uint8_t *any_input(size_t *length) {
    for (size_t i = 0; i < N; i++) {
        input[i] = nondet_u8();
    }
    *length = nondet_size();
    __CPROVER_assume(*length <= N);
    return input + (N - *length);
}

/* Any reader the specifications allow, with a file whose strings and types hold up to
 * MOST_ENTRIES entries. */
static struct reader *any_reader(void) {
    struct reader *r = object(sizeof *r);
    r->bytes = any_input(&r->length);
    r->error = object(sizeof *r->error);
    r->arena = object(sizeof *r->arena);
    mo_arena_init(r->arena, mo_bytecode_work_budget(r->length));
    r->failed = nondet_bool(); /* a valid bool, not any byte */
    __CPROVER_assume(READER(r));
    struct mo_bytecode *f = object(sizeof *f);
    __CPROVER_assume(f->string_count <= MOST_ENTRIES && f->type_count <= MOST_ENTRIES);
    f->strings = object(MOST_ENTRIES * sizeof(struct mo_bytecode_string) + 1);
    r->names.canonical = object(MOST_ENTRIES * sizeof(uint32_t) + 1);
    r->types = f->type_count == 0 ? NULL : object(f->type_count * sizeof(struct mo_type));
    f->types = r->types;
    r->file = f;
    return r;
}

/* Restores what a reading function may change, so UNCHANGED compares the rest. */
static void reading(struct reader *before, const struct reader *after) {
    before->offset = after->offset;
    before->field = after->field;
    before->failed = after->failed;
}

void harness_word(void) {
    struct reader *r = any_reader();
    struct reader old = *r;
    uint32_t value = word(r);
    bool fails = old.failed || old.length - old.offset < 4;
    __CPROVER_assert(READS(r, old) && r->failed == fails &&
                         r->offset == (fails ? old.offset : old.offset + 4),
                     "word: offset and failure");
    const uint8_t *b = r->bytes + old.offset;
    __CPROVER_assert(fails ? value == 0
                           : value == ((uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 |
                                       (uint32_t)b[3] << 24),
                     "word: little-endian value");
    reading(&old, r);
    __CPROVER_assert(UNCHANGED(old, *r), "word: changes only the offset and failure");
    reached_end();
}

void harness_boolean(void) {
    struct reader *r = any_reader();
    struct reader old = *r;
    (void)boolean(r);
    __CPROVER_assert(READS(r, old) && r->offset <= old.offset + 4, "boolean: reads one word");
    reading(&old, r);
    __CPROVER_assert(UNCHANGED(old, *r), "boolean: changes only the offset and failure");
    reached_end();
}

static void check_block(struct reader *r, struct reader old, size_t n, size_t size, void *p) {
    __CPROVER_assert(READS(r, old) && r->offset == old.offset && r->field == old.field,
                     "allocation reads nothing");
    if (old.failed || n == 0) {
        __CPROVER_assert(p == NULL && r->failed == old.failed, "nothing to allocate");
    } else {
        __CPROVER_assert(r->failed ? p == NULL : TABLE(p, n, size), "a table, or no memory");
    }
    reading(&old, r);
    __CPROVER_assert(UNCHANGED(old, *r), "allocation changes only the failure");
}

void harness_allocate(void) {
    struct reader *r = any_reader();
    size_t n = nondet_size(), size = nondet_size(), alignment = nondet_size();
    __CPROVER_assume((r->failed || n <= N + 1) && size >= 1 && size <= LARGEST_ENTRY &&
                     alignment >= 1 && alignment <= 16 && (alignment & (alignment - 1)) == 0);
    struct reader old = *r;
    void *p = allocate(r, n, size, alignment);
    check_block(r, old, n, size, p);
    size_t i = nondet_size();
    __CPROVER_assume(p != NULL && i < n * size);
    __CPROVER_assert(((const uint8_t *)p)[i] == 0, "allocate: zeroed");
    reached_end();
}

void harness_scratch(void) {
    struct reader *r = any_reader();
    uint32_t n = nondet_u32();
    size_t size = nondet_size(), alignment = nondet_size();
    __CPROVER_assume((r->failed || n <= MOST_ENTRIES) && size >= 1 && size <= LARGEST_ENTRY &&
                     alignment >= 1 && alignment <= 16 && (alignment & (alignment - 1)) == 0);
    struct reader old = *r;
    void *p = scratch(r, n, size, alignment);
    check_block(r, old, n, size, p);
    reached_end();
}

static void check_table(struct reader *r, struct reader old, uint32_t n, uint32_t limit,
                        size_t minimum, size_t size, const void *p, bool filled) {
    __CPROVER_assert(READS(r, old), "table: reads");
    if (r->failed) {
        __CPROVER_assert(n == 0 && p == NULL, "table: none when failed");
    } else {
        __CPROVER_assert(n <= limit && TABLE(p, n, size), "table: bounded count and table");
        if (filled) {
            __CPROVER_assert(r->offset == old.offset + 4 + 4 * (size_t)n, "table: read whole");
        } else {
            __CPROVER_assert(r->offset == old.offset + 4 && n <= (r->length - r->offset) / minimum,
                             "table: count fits the bytes left");
        }
    }
    reading(&old, r);
    __CPROVER_assert(UNCHANGED(old, *r), "table: changes only the offset and failure");
}

void harness_array(void) {
    struct reader *r = any_reader();
    uint32_t n, limit = nondet_u32();
    size_t minimum = nondet_size(), size = nondet_size(), alignment = nondet_size();
    __CPROVER_assume(minimum >= 4 && size >= 1 && size <= LARGEST_ENTRY && alignment >= 1 &&
                     alignment <= 16 && (alignment & (alignment - 1)) == 0);
    struct reader old = *r;
    void *p = array(r, &n, limit, minimum, size, alignment);
    check_table(r, old, n, limit, minimum, size, p, false);
    reached_end();
}

void harness_indices(void) {
    struct reader *r = any_reader();
    uint32_t n, limit = nondet_u32();
    struct reader old = *r;
    const uint32_t *p = indices(r, &n, limit);
    check_table(r, old, n, limit, 4, sizeof *p, p, true);
    reached_end();
}

void harness_utf8(void) {
    size_t length;
    const uint8_t *bytes = any_input(&length);
    (void)mo_bytecode_utf8(bytes, (uint32_t)length);
    reached_end();
}

void harness_blob(void) {
    struct reader *r = any_reader();
    uint32_t n;
    bool terminate = nondet_bool();
    struct reader old = *r;
    const uint8_t *p = blob(r, &n, terminate);
    __CPROVER_assert(READS(r, old), "blob: reads");
    if (r->failed) {
        __CPROVER_assert(n == 0 && p == NULL, "blob: none when failed");
    } else {
        __CPROVER_assert(r->offset == old.offset + 4 + n && n <= MO_LIMIT_STRING_SIZE &&
                             TABLE(p, n + (terminate ? 1U : 0U), 1),
                         "blob: its bytes, copied");
        __CPROVER_assert(!terminate || p[n] == 0, "blob: NUL-terminated");
    }
    reading(&old, r);
    __CPROVER_assert(UNCHANGED(old, *r), "blob: changes only the offset and failure");
    reached_end();
}

void harness_name(void) {
    struct reader *r = any_reader();
    struct reader old = *r;
    (void)name(r, nondet_bool());
    __CPROVER_assert(READS(r, old) && r->offset <= old.offset + 4, "name: reads one word");
    reading(&old, r);
    __CPROVER_assert(UNCHANGED(old, *r), "name: changes only the offset and failure");
    reached_end();
}

void harness_fields(void) {
    struct reader *r = any_reader();
    uint32_t n;
    struct reader old = *r;
    const struct mo_type_field *p = fields(r, &n, nondet_bool());
    __CPROVER_assert(READS(r, old) && n <= MOST_ENTRIES && TABLE(p, n, sizeof *p),
                     "fields: a bounded table");
    reading(&old, r);
    __CPROVER_assert(UNCHANGED(old, *r), "fields: changes only the offset and failure");
    reached_end();
}

/* choice_cases and type_entry: one type's entry, setting only that entry. */
static struct mo_type *any_type(void) {
    struct mo_type *t = object(sizeof *t);
    *t = nondet_type();
    t->defined = nondet_bool();
    t->reference = nondet_bool();
    t->has_fields = nondet_bool();
    return t;
}

void harness_choice_cases(void) {
    struct reader *r = any_reader();
    struct mo_type *t = any_type();
    struct reader old = *r;
    struct mo_type before = *t;
    choice_cases(r, t);
    __CPROVER_assert(READS(r, old) && t->case_count <= MOST_ENTRIES, "choice_cases: bounded cases");
    reading(&old, r);
    __CPROVER_assert(UNCHANGED(old, *r), "choice_cases: changes only the offset and failure");
    __CPROVER_assert(t->kind == before.kind && t->defined == before.defined &&
                         t->parameters == before.parameters &&
                         t->parameter_count == before.parameter_count &&
                         t->result == before.result && t->function == before.function &&
                         t->capability == before.capability && t->fields == before.fields &&
                         t->field_count == before.field_count && t->reference == before.reference,
                     "choice_cases: sets only the choice's fields");
    reached_end();
}

void harness_type_entry(void) {
    struct reader *r = any_reader();
    struct mo_type *t = any_type();
    __CPROVER_assume(t->kind <= MO_TYPE_CAPABILITY);
    struct reader old = *r;
    struct mo_type before = *t;
    type_entry(r, t);
    __CPROVER_assert(READS(r, old), "type_entry: reads");
    __CPROVER_assert(t->kind == before.kind && t->defined == before.defined,
                     "type_entry: keeps the kind");
    __CPROVER_assert(t->kind != MO_TYPE_CHOICE || t->case_count <= MOST_ENTRIES,
                     "type_entry: bounded cases");
    reading(&old, r);
    __CPROVER_assert(UNCHANGED(old, *r), "type_entry: changes only the offset and failure");
    reached_end();
}

void harness_parameters(void) {
    struct reader *r = any_reader();
    struct mo_bytecode_function *fn = object(sizeof *fn);
    struct reader old = *r;
    parameters(r, fn);
    __CPROVER_assert(READS(r, old), "parameters: reads");
    reading(&old, r);
    __CPROVER_assert(UNCHANGED(old, *r), "parameters: changes only the offset and failure");
    reached_end();
}

void harness_module_entry(void) {
    struct reader *r = any_reader();
    struct mo_bytecode_module *m = object(sizeof *m);
    struct reader old = *r;
    module_entry(r, m);
    __CPROVER_assert(READS(r, old), "module_entry: reads");
    reading(&old, r);
    __CPROVER_assert(UNCHANGED(old, *r), "module_entry: changes only the offset and failure");
    reached_end();
}

void harness_layout_entry(void) {
    struct reader *r = any_reader();
    struct mo_layout *l = object(sizeof *l);
    struct reader old = *r;
    layout_entry(r, l);
    __CPROVER_assert(READS(r, old), "layout_entry: reads");
    reading(&old, r);
    __CPROVER_assert(UNCHANGED(old, *r), "layout_entry: changes only the offset and failure");
    reached_end();
}

void harness_file_entry(void) {
    struct reader *r = any_reader();
    struct mo_bytecode_bytes *file = object(sizeof *file);
    struct reader old = *r;
    file_entry(r, file);
    __CPROVER_assert(READS(r, old), "file_entry: reads");
    reading(&old, r);
    __CPROVER_assert(UNCHANGED(old, *r), "file_entry: changes only the offset and failure");
    reached_end();
}

void harness_debug_entry(void) {
    struct reader *r = any_reader();
    struct mo_bytecode_debug *d = object(sizeof *d);
    struct mo_bytecode_debug_positions *positions = object(sizeof *positions);
    struct reader old = *r;
    debug_entry(r, d, positions);
    __CPROVER_assert(READS(r, old), "debug_entry: reads");
    reading(&old, r);
    __CPROVER_assert(UNCHANGED(old, *r), "debug_entry: changes only the offset and failure");
    reached_end();
}

/* types, constants and functions: each reads its section and sets only its own tables. */
void harness_types(void) {
    struct reader *r = any_reader();
    struct reader old = *r;
    struct mo_bytecode file = *r->file;
    types(r);
    __CPROVER_assert(READS(r, old), "types: reads");
    __CPROVER_assert(r->failed || (TYPES(r) && cases_bounded(r)), "types: a bounded table");
    reading(&old, r);
    old.types = r->types;
    file.type_count = r->file->type_count;
    file.types = r->file->types;
    __CPROVER_assert(UNCHANGED(old, *r) && same_file(&file, r->file), "types: sets only types");
    reached_end();
}

void harness_constants(void) {
    struct reader *r = any_reader();
    __CPROVER_assume(r->failed || TYPES(r));
    struct reader old = *r;
    struct mo_bytecode file = *r->file;
    constants(r);
    __CPROVER_assert(READS(r, old), "constants: reads");
    reading(&old, r);
    file.constant_count = r->file->constant_count;
    file.constants = r->file->constants;
    __CPROVER_assert(UNCHANGED(old, *r) && same_file(&file, r->file),
                     "constants: sets only constants");
    reached_end();
}

void harness_functions(void) {
    struct reader *r = any_reader();
    struct reader old = *r;
    struct mo_bytecode file = *r->file;
    functions(r);
    __CPROVER_assert(READS(r, old), "functions: reads");
    __CPROVER_assert(r->failed || (r->file->function_count <= MOST_ENTRIES &&
                                   TABLE(r->code_positions, r->file->function_count, 4)),
                     "functions: a code position for each function");
    reading(&old, r);
    old.code_positions = r->code_positions;
    file.function_count = r->file->function_count;
    file.functions = r->file->functions;
    __CPROVER_assert(UNCHANGED(old, *r) && same_file(&file, r->file),
                     "functions: sets only functions");
    reached_end();
}

void harness_sections(void) {
    struct reader *r = any_reader();
    __CPROVER_assume(!r->failed);
    struct reader old = *r;
    sections(r);
    __CPROVER_assert(READS(r, old), "sections: reads");
    __CPROVER_assert(r->failed ||
                         (TYPES(r) && cases_bounded(r) && r->file->function_count <= MOST_ENTRIES &&
                          TABLE(r->code_positions, r->file->function_count, 4)),
                     "sections: bounded types and a code position for each function");
    __CPROVER_assert(r->bytes == old.bytes && r->length == old.length && r->error == old.error &&
                         r->arena == old.arena && r->file == old.file,
                     "sections: same input and objects");
    reached_end();
}

/* The whole decoder, with sections replaced by its model: any input of up to N bytes. */
void harness_decode(void) {
    size_t length;
    const uint8_t *bytes = any_input(&length);
    struct mo_arena arena;
    mo_arena_init(&arena, mo_bytecode_work_budget(length));
    const uint32_t *code_offsets = NULL;
    struct mo_types types_out;
    struct mo_bytecode_error_site site;
    struct mo_load_error error;
    bool details = nondet_bool();
    struct mo_bytecode *file =
        mo_bytecode_decode_info(&arena, bytes, length, details ? &code_offsets : NULL,
                                details ? &types_out : NULL, details ? &site : NULL, &error);
    __CPROVER_assert(file != NULL || error.reason != NULL, "a refusal has a reason");
    __CPROVER_assert(file != NULL || error.offset <= length, "a refusal points inside the input");
    reached_end();
}
