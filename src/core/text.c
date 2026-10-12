#include "core/core.h"

#include <string.h>

#include "base/codes.h"
#include "base/limits.h"

/* Schema fields use object representations, not casts between unrelated C structs or enums. */
enum shape {
    SH_NODE,
    SH_LIST,
    SH_POINTER,
    SH_STRING,
    SH_INTEGER,
    SH_NATURAL,
    SH_BITS,
    SH_BOOLEAN,
    SH_CHARACTER
};
struct schema;
struct field {
    const struct schema *schema;
    size_t offset, size;
};
struct variant {
    const char *wire;
    const void *tag;
    const struct field *fields;
};
struct schema {
    enum shape kind;
    size_t size;
    size_t tag_offset, tag_size;
    const struct variant *variants;
    const struct schema *element;
    size_t items_offset, count_offset;
};
#include "core/text_schema.def"

static MO_MUST_USE const struct schema *document_schema(enum mo_core_text_kind kind) {
    switch (kind) {
    case MO_CORE_TEXT_TYPE:
        return &s_ty_object;
    case MO_CORE_TEXT_CAP:
        return &s_cap_object;
    case MO_CORE_TEXT_LITERAL:
        return &s_lit_object;
    case MO_CORE_TEXT_PRIMITIVE:
        return &s_prim;
    case MO_CORE_TEXT_EXPR:
        return &s_expr_object;
    case MO_CORE_TEXT_PROGRAM:
        return &s_program;
    case MO_CORE_TEXT_VALUE:
        return &s_value_object;
    case MO_CORE_TEXT_WORLD:
        return &s_world;
    case MO_CORE_TEXT_BUG:
        return &s_bug;
    case MO_CORE_TEXT_OUTCOME:
        return &s_outcome;
    case MO_CORE_TEXT_EFFECTS:
        return &s_effects;
    case MO_CORE_TEXT_REPORT:
        return &s_report;
    case MO_CORE_TEXT_SELECTION:
        return &s_selection;
    case MO_CORE_TEXT_RUN:
        return &s_run;
    }
    MO_UNREACHABLE();
}

enum sexp_kind { SX_NODE, SX_ATOM, SX_STRING };
struct sexp {
    enum sexp_kind kind;
    uint32_t start, end;
    mo_core_string text;
    struct sexp *children, *next;
    size_t count;
};
struct reader {
    const uint8_t *bytes;
    size_t length, pos, nodes;
    struct mo_arena scratch;
    struct mo_buffer string;
    struct mo_arena *arena;
    uint16_t code;
    uint32_t start, end;
    const char *message;
};

static MO_MUST_USE bool error_at(struct reader *r, uint16_t code, size_t start, size_t end,
                                 const char *message) {
    if (r->code == 0) {
        MO_ASSERT(start <= end && end <= r->length);
        r->code = code;
        r->start = (uint32_t)start;
        r->end = (uint32_t)end;
        r->message = message;
    }
    return false;
}
static MO_MUST_USE bool error_node(struct reader *r, uint16_t code, const struct sexp *sx,
                                   const char *message) {
    return error_at(r, code, sx->start, sx->end, message);
}

/* Strict UTF-8, including in comments and atoms. Advance only after a complete scalar. */
static MO_MUST_USE bool scalar(const uint8_t *bytes, size_t length, size_t *pos, uint32_t *out) {
    if (*pos >= length)
        return false;
    size_t at = *pos;
    uint32_t c = bytes[at++];
    unsigned extra = 0;
    uint32_t minimum = 0;
    if (c < 0x80) {
        *pos = at;
        *out = c;
        return true;
    }
    if (c >= 0xc2 && c <= 0xdf) {
        extra = 1;
        minimum = 0x80;
        c &= 0x1f;
    } else if (c >= 0xe0 && c <= 0xef) {
        extra = 2;
        minimum = 0x800;
        c &= 0x0f;
    } else if (c >= 0xf0 && c <= 0xf4) {
        extra = 3;
        minimum = 0x10000;
        c &= 7;
    } else
        return false;
    if (length - at < extra)
        return false;
    for (unsigned i = 0; i < extra; i++) {
        uint32_t b = bytes[at++];
        if ((b & 0xc0) != 0x80)
            return false;
        c = (c << 6) | (b & 0x3f);
    }
    if (c < minimum || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff))
        return false;
    *pos = at;
    *out = c;
    return true;
}
static MO_MUST_USE bool append_scalar(struct mo_buffer *out, uint32_t c) {
    uint8_t bytes[4];
    size_t n;
    if (c < 0x80) {
        bytes[0] = (uint8_t)c;
        n = 1;
    } else if (c < 0x800) {
        bytes[0] = (uint8_t)(0xc0 | (c >> 6));
        bytes[1] = (uint8_t)(0x80 | (c & 0x3f));
        n = 2;
    } else if (c < 0x10000 && !(c >= 0xd800 && c <= 0xdfff)) {
        bytes[0] = (uint8_t)(0xe0 | (c >> 12));
        bytes[1] = (uint8_t)(0x80 | ((c >> 6) & 0x3f));
        bytes[2] = (uint8_t)(0x80 | (c & 0x3f));
        n = 3;
    } else if (c >= 0x10000 && c <= 0x10ffff) {
        bytes[0] = (uint8_t)(0xf0 | (c >> 18));
        bytes[1] = (uint8_t)(0x80 | ((c >> 12) & 0x3f));
        bytes[2] = (uint8_t)(0x80 | ((c >> 6) & 0x3f));
        bytes[3] = (uint8_t)(0x80 | (c & 0x3f));
        n = 4;
    } else
        return false;
    return mo_buffer_append(out, bytes, n);
}
static MO_MUST_USE bool white(uint8_t c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}
static MO_MUST_USE bool boundary(uint8_t c) {
    return white(c) || c == '(' || c == ')' || c == '"' || c == ';';
}
static MO_MUST_USE int hex_digit(uint8_t c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}
static void skip(struct reader *r) {
    while (r->pos < r->length) {
        if (white(r->bytes[r->pos]))
            r->pos++;
        else if (r->bytes[r->pos] == ';') {
            while (r->pos < r->length && r->bytes[r->pos] != '\n')
                r->pos++;
        } else
            break;
    }
}
static MO_MUST_USE bool read_string(struct reader *r, mo_core_string *out) {
    size_t start = r->pos++;
    mo_buffer_clear(&r->string);
    while (r->pos < r->length) {
        uint8_t c = r->bytes[r->pos++];
        if (c == '"') {
            char *copy = mo_arena_allocate(&r->scratch, r->string.length + 1, 1);
            if (copy == NULL)
                return error_at(r, MO_CODE_CORE_TEXT_LIMIT, start, r->pos,
                                "Core scratch memory is exhausted");
            if (r->string.length != 0)
                memcpy(copy, r->string.bytes, r->string.length);
            copy[r->string.length] = '\0';
            *out = (mo_core_string){copy, r->string.length};
            return true;
        }
        if (c < 32 || c == 127)
            return error_at(r, MO_CODE_CORE_ESCAPE, r->pos - 1, r->pos, "raw control in string");
        if (c != '\\') {
            if (!mo_buffer_append_byte(&r->string, c))
                break;
            continue;
        }
        if (r->pos == r->length)
            return error_at(r, MO_CODE_CORE_ESCAPE, start, r->pos, "unterminated string escape");
        c = r->bytes[r->pos++];
        uint32_t decoded = c;
        switch (c) {
        case '"':
        case '\\':
            break;
        case 'n':
            decoded = '\n';
            break;
        case 'r':
            decoded = '\r';
            break;
        case 't':
            decoded = '\t';
            break;
        case 'u': {
            if (r->pos == r->length || r->bytes[r->pos++] != '{')
                return error_at(r, MO_CODE_CORE_ESCAPE, start, r->pos,
                                "expected Unicode escape braces");
            size_t digits = 0;
            decoded = 0;
            while (r->pos < r->length && r->bytes[r->pos] != '}') {
                int d = hex_digit(r->bytes[r->pos++]);
                if (d < 0 || ++digits > 6)
                    return error_at(r, MO_CODE_CORE_ESCAPE, start, r->pos,
                                    "Unicode escape needs 1 to 6 hex digits");
                decoded = decoded * 16 + (uint32_t)d;
            }
            if (digits == 0 || r->pos == r->length)
                return error_at(r, MO_CODE_CORE_ESCAPE, start, r->pos, "incomplete Unicode escape");
            r->pos++;
            if (decoded > 0x10ffff || (decoded >= 0xd800 && decoded <= 0xdfff))
                return error_at(r, MO_CODE_CORE_ESCAPE, start, r->pos, "not a Unicode scalar");
            break;
        }
        default:
            return error_at(r, MO_CODE_CORE_ESCAPE, r->pos - 2, r->pos, "unknown string escape");
        }
        if (!append_scalar(&r->string, decoded))
            break;
    }
    return error_at(r, r->pos == r->length ? MO_CODE_CORE_SYNTAX : MO_CODE_CORE_TEXT_LIMIT, start,
                    r->pos, "unterminated string or exhausted string buffer");
}

/* Lists are linked only in scratch. The returned Core tree uses typed contiguous lists. */
static MO_MUST_USE struct sexp *lex(struct reader *r) {
    struct frame {
        struct sexp *node;
        struct sexp **tail;
    } stack[MO_LIMIT_CORE_DEPTH];
    size_t depth = 0;
    struct sexp *root = NULL;
    for (;;) {
        skip(r);
        if (r->pos == r->length)
            break;
        if (r->bytes[r->pos] == ')') {
            if (depth == 0) {
                bool reported = error_at(r, MO_CODE_CORE_SYNTAX, r->pos, r->pos + 1,
                                         "unexpected closing parenthesis");
                MO_ASSERT(!reported);
                return NULL;
            }
            stack[--depth].node->end = (uint32_t)++r->pos;
            continue;
        }
        if (root != NULL && depth == 0) {
            bool reported = error_at(r, MO_CODE_CORE_SYNTAX, r->pos, r->pos + 1, "trailing input");
            MO_ASSERT(!reported);
            return NULL;
        }
        if (r->nodes == MO_LIMIT_CORE_NODES) {
            bool reported =
                error_at(r, MO_CODE_CORE_TEXT_LIMIT, r->pos, r->pos, "too many Core text nodes");
            MO_ASSERT(!reported);
            return NULL;
        }
        struct sexp *sx = mo_arena_allocate(&r->scratch, sizeof *sx, _Alignof(struct sexp));
        if (sx == NULL) {
            bool reported = error_at(r, MO_CODE_CORE_TEXT_LIMIT, r->pos, r->pos,
                                     "Core scratch memory is exhausted");
            MO_ASSERT(!reported);
            return NULL;
        }
        *sx = (struct sexp){.start = (uint32_t)r->pos};
        r->nodes++;
        if (depth == 0)
            root = sx;
        else {
            *stack[depth - 1].tail = sx;
            stack[depth - 1].tail = &sx->next;
            stack[depth - 1].node->count++;
        }
        uint8_t c = r->bytes[r->pos];
        if (c == '(') {
            if (depth == MO_LIMIT_CORE_DEPTH) {
                bool reported = error_at(r, MO_CODE_NESTED_TOO_DEEPLY, r->pos, r->pos + 1,
                                         "Core text nests too deeply");
                MO_ASSERT(!reported);
                return NULL;
            }
            sx->kind = SX_NODE;
            stack[depth++] = (struct frame){sx, &sx->children};
            r->pos++;
        } else if (c == '"') {
            sx->kind = SX_STRING;
            if (!read_string(r, &sx->text))
                return NULL;
            sx->end = (uint32_t)r->pos;
        } else {
            sx->kind = SX_ATOM;
            size_t start = r->pos;
            while (r->pos < r->length && !boundary(r->bytes[r->pos]))
                r->pos++;
            sx->text = (mo_core_string){(const char *)r->bytes + start, r->pos - start};
            sx->end = (uint32_t)r->pos;
        }
    }
    if (depth != 0 || root == NULL) {
        bool reported =
            error_at(r, MO_CODE_CORE_SYNTAX, r->pos, r->pos, "expected complete S-expression");
        MO_ASSERT(!reported);
        return NULL;
    }
    return root;
}
static MO_MUST_USE bool atom_is(const struct sexp *sx, const char *text) {
    return sx != NULL && sx->kind == SX_ATOM && sx->text.length == strlen(text) &&
           memcmp(sx->text.bytes, text, sx->text.length) == 0;
}
static MO_MUST_USE bool copy_string(struct reader *r, const struct sexp *sx, mo_core_string text,
                                    mo_core_string *out) {
    char *bytes = mo_arena_allocate(r->arena, text.length + 1, 1);
    if (bytes == NULL)
        return error_node(r, MO_CODE_CORE_NO_MEMORY, sx, "Core arena is exhausted");
    if (text.length != 0)
        memcpy(bytes, text.bytes, text.length);
    bytes[text.length] = '\0';
    *out = (mo_core_string){bytes, text.length};
    return true;
}
static MO_MUST_USE bool decode_integer(struct reader *r, const struct sexp *sx, bool natural,
                                       mo_core_integer *out) {
    if (sx->kind != SX_ATOM || sx->text.length == 0)
        return error_node(r, MO_CODE_CORE_NUMBER, sx, "expected decimal whole number");
    mo_core_string digits = sx->text;
    bool negative = digits.bytes[0] == '-';
    if (negative) {
        digits.bytes++;
        digits.length--;
    }
    if (digits.length == 0)
        return error_node(r, MO_CODE_CORE_NUMBER, sx, "expected decimal digits");
    for (size_t i = 0; i < digits.length; i++) {
        if (digits.bytes[i] < '0' || digits.bytes[i] > '9')
            return error_node(r, MO_CODE_CORE_NUMBER, sx, "expected decimal digits");
    }
    while (digits.length > 1 && digits.bytes[0] == '0') {
        digits.bytes++;
        digits.length--;
    }
    if (digits.length == 1 && digits.bytes[0] == '0')
        negative = false;
    if (natural && negative)
        return error_node(r, MO_CODE_CORE_NUMBER, sx, "expected nonnegative whole number");
    out->negative = negative;
    return copy_string(r, sx, digits, &out->digits);
}

static MO_MUST_USE bool decode(struct reader *r, const struct schema *s, const struct sexp *sx,
                               void *out, size_t depth);
static MO_MUST_USE bool decode_node(struct reader *r, const struct schema *s, const struct sexp *sx,
                                    uint8_t *out, size_t depth) {
    if (sx->kind != SX_NODE)
        return error_node(r, MO_CODE_CORE_FIELD, sx, "expected constructor list");
    const struct variant *v = s->variants;
    const struct sexp *child = sx->children;
    if (v->wire[0] != '\0') {
        while (v->wire != NULL && !atom_is(child, v->wire))
            v++;
        if (v->wire == NULL)
            return error_node(r, MO_CODE_CORE_CONSTRUCTOR, sx, "unknown Core constructor");
        child = child->next;
    }
    size_t count = 0;
    while (v->fields[count].schema != NULL)
        count++;
    if (sx->count != count + (v->wire[0] != '\0' ? 1u : 0u))
        return error_node(r, MO_CODE_CORE_ARITY, sx, "wrong Core constructor arity");
    if (s->tag_size != 0)
        memcpy(out + s->tag_offset, v->tag, s->tag_size);
    for (size_t i = 0; i < count; i++, child = child->next) {
        const struct field *f = &v->fields[i];
        MO_ASSERT(f->size == f->schema->size && f->offset <= s->size - f->size);
        if (!decode(r, f->schema, child, out + f->offset, depth + 1))
            return false;
    }
    return true;
}
static MO_MUST_USE bool decode(struct reader *r, const struct schema *s, const struct sexp *sx,
                               void *out, size_t depth) {
    MO_ASSERT(sx != NULL && out != NULL);
    if ((s->kind == SH_NODE || s->kind == SH_LIST) && depth > MO_LIMIT_CORE_DEPTH)
        return error_node(r, MO_CODE_NESTED_TOO_DEEPLY, sx, "Core text nests too deeply");
    switch (s->kind) {
    case SH_NODE:
        return decode_node(r, s, sx, out, depth);
    case SH_POINTER: {
        MO_ASSERT(s->size == sizeof(void *));
        void *p = mo_arena_allocate(r->arena, s->element->size, _Alignof(max_align_t));
        if (p == NULL)
            return error_node(r, MO_CODE_CORE_NO_MEMORY, sx, "Core arena is exhausted");
        memset(p, 0, s->element->size);
        memcpy(out, &p, sizeof p);
        return decode(r, s->element, sx, p, depth);
    }
    case SH_LIST: {
        if (sx->kind != SX_NODE)
            return error_node(r, MO_CODE_CORE_FIELD, sx, "expected list wrapper");
        void *items = NULL;
        if (sx->count != 0) {
            items = mo_arena_allocate_array(r->arena, sx->count, s->element->size,
                                            _Alignof(max_align_t));
            if (items == NULL)
                return error_node(r, MO_CODE_CORE_NO_MEMORY, sx, "Core arena is exhausted");
            memset(items, 0, sx->count * s->element->size);
        }
        memcpy((uint8_t *)out + s->items_offset, &items, sizeof items);
        memcpy((uint8_t *)out + s->count_offset, &sx->count, sizeof sx->count);
        const struct sexp *child = sx->children;
        for (size_t i = 0; i < sx->count; i++, child = child->next) {
            if (!decode(r, s->element, child, (uint8_t *)items + i * s->element->size, depth + 1))
                return false;
        }
        return true;
    }
    case SH_STRING:
        if (sx->kind != SX_STRING)
            return error_node(r, MO_CODE_CORE_FIELD, sx, "expected string");
        return copy_string(r, sx, sx->text, out);
    case SH_INTEGER:
    case SH_NATURAL:
        return decode_integer(r, sx, s->kind == SH_NATURAL, out);
    case SH_BITS: {
        if (sx->kind != SX_ATOM || sx->text.length != 18 || memcmp(sx->text.bytes, "0x", 2) != 0)
            return error_node(r, MO_CODE_CORE_NUMBER, sx, "expected 0x and 16 hex digits");
        uint64_t bits = 0;
        for (size_t i = 2; i < 18; i++) {
            int d = hex_digit((uint8_t)sx->text.bytes[i]);
            if (d < 0)
                return error_node(r, MO_CODE_CORE_NUMBER, sx, "invalid Float hexadecimal digit");
            bits = (bits << 4) | (uint64_t)d;
        }
        memcpy(out, &bits, sizeof bits);
        return true;
    }
    case SH_BOOLEAN: {
        bool b = atom_is(sx, "true");
        if (!b && !atom_is(sx, "false"))
            return error_node(r, MO_CODE_CORE_FIELD, sx, "expected true or false");
        memcpy(out, &b, sizeof b);
        return true;
    }
    case SH_CHARACTER: {
        uint32_t c;
        size_t pos = 0;
        if (sx->kind != SX_STRING ||
            !scalar((const uint8_t *)sx->text.bytes, sx->text.length, &pos, &c) ||
            pos != sx->text.length)
            return error_node(r, MO_CODE_CORE_CHARACTER, sx, "expected exactly one Unicode scalar");
        memcpy(out, &c, sizeof c);
        return true;
    }
    }
    MO_UNREACHABLE();
}

void *mo_core_text_parse(struct mo_arena *arena, enum mo_core_text_kind kind, const char *text,
                         size_t length, struct mo_diags *diags) {
    MO_ASSERT(arena != NULL && diags != NULL && (text != NULL || length == 0));
    const struct schema *s = document_schema(kind);
    struct reader r = {.bytes = (const uint8_t *)text, .length = length, .arena = arena};
    mo_arena_init(&r.scratch, MO_LIMIT_CORE_SCRATCH_SIZE);
    mo_buffer_init(&r.string, MO_LIMIT_CORE_TEXT_SIZE);
    void *out = NULL;
    if (length > MO_LIMIT_CORE_TEXT_SIZE) {
        /* Do not convert an unbounded caller length to a diagnostic span. */
        r.length = 0;
        bool reported = error_at(&r, MO_CODE_CORE_TEXT_LIMIT, 0, 0, "Core text file is too large");
        MO_ASSERT(!reported);
    } else {
        size_t pos = 0;
        uint32_t c;
        while (pos < length) {
            size_t start = pos;
            if (!scalar(r.bytes, length, &pos, &c)) {
                bool reported =
                    error_at(&r, MO_CODE_CORE_UTF8, start, start + 1, "input is not valid UTF-8");
                MO_ASSERT(!reported);
                break;
            }
        }
        struct sexp *root = r.code == 0 ? lex(&r) : NULL;
        if (root != NULL) {
            if (root->kind != SX_NODE || !atom_is(root->children, "core") || root->count != 3) {
                bool reported =
                    error_node(&r, MO_CODE_CORE_SYNTAX, root, "expected (core version payload)");
                MO_ASSERT(!reported);
            } else if (!atom_is(root->children->next, MO_CORE_TEXT_VERSION)) {
                bool reported = error_node(&r, MO_CODE_CORE_VERSION, root->children->next,
                                           "unsupported Core text version");
                MO_ASSERT(!reported);
            } else {
                const struct sexp *payload = root->children->next->next;
                out = mo_arena_allocate(arena, s->size, _Alignof(max_align_t));
                if (out == NULL) {
                    bool reported =
                        error_node(&r, MO_CODE_CORE_NO_MEMORY, payload, "Core arena is exhausted");
                    MO_ASSERT(!reported);
                } else {
                    memset(out, 0, s->size);
                    if (!decode(&r, s, payload, out, 2)) {
                        out = NULL;
                    } else {
                        /* Adjacent lists can expand when printed. Enforce the byte limit on
                         * canonical text too, so every accepted file can be reparsed. Reuse
                         * the lexical string buffer rather than retain a second tree. */
                        mo_buffer_clear(&r.string);
                        if (!mo_core_text_print(kind, out, &r.string)) {
                            bool reported = error_node(&r, MO_CODE_CORE_TEXT_LIMIT, payload,
                                                       "canonical Core text exceeds a limit");
                            MO_ASSERT(!reported);
                            out = NULL;
                        }
                    }
                }
            }
        }
    }
    if (r.code != 0) {
        struct mo_diag diag = {
            .code = r.code, .primary = {{0, r.start, r.end}, NULL}, .message = r.message};
        bool kept = mo_diags_add(diags, &diag);
        if (!kept)
            MO_ASSERT(diags->dropped != 0);
    }
    mo_buffer_free(&r.string);
    mo_arena_free(&r.scratch);
    return out;
}

struct writer {
    struct mo_buffer *out;
    size_t nodes;
};
static MO_MUST_USE bool quote(struct mo_buffer *out, mo_core_string s) {
    if (s.length > MO_LIMIT_CORE_TEXT_SIZE || (s.bytes == NULL && s.length != 0))
        return false;
    if (!mo_buffer_append_byte(out, '"'))
        return false;
    size_t pos = 0;
    while (pos < s.length) {
        size_t start = pos;
        uint32_t c;
        if (!scalar((const uint8_t *)s.bytes, s.length, &pos, &c))
            return false;
        const char *escape = NULL;
        switch (c) {
        case '"':
            escape = "\\\"";
            break;
        case '\\':
            escape = "\\\\";
            break;
        case '\n':
            escape = "\\n";
            break;
        case '\r':
            escape = "\\r";
            break;
        case '\t':
            escape = "\\t";
            break;
        default:
            break;
        }
        if (escape != NULL) {
            if (!mo_buffer_append_text(out, escape))
                return false;
        } else if (c < 32 || c == 127) {
            if (!mo_buffer_append_text(out, "\\u{") || !mo_buffer_append_hex(out, c, 0) ||
                !mo_buffer_append_byte(out, '}'))
                return false;
        } else if (!mo_buffer_append(out, s.bytes + start, pos - start))
            return false;
    }
    return mo_buffer_append_byte(out, '"');
}
static MO_MUST_USE bool print_integer(struct mo_buffer *out, mo_core_integer n, bool natural) {
    if (n.digits.length == 0 || n.digits.length > MO_LIMIT_CORE_TEXT_SIZE || n.digits.bytes == NULL)
        return false;
    for (size_t i = 0; i < n.digits.length; i++) {
        if (n.digits.bytes[i] < '0' || n.digits.bytes[i] > '9')
            return false;
    }
    while (n.digits.length > 1 && n.digits.bytes[0] == '0') {
        n.digits.bytes++;
        n.digits.length--;
    }
    if (n.digits.length == 1 && n.digits.bytes[0] == '0')
        n.negative = false;
    if (n.negative && (natural || !mo_buffer_append_byte(out, '-')))
        return false;
    return mo_buffer_append(out, n.digits.bytes, n.digits.length);
}
static MO_MUST_USE bool encode(struct writer *w, const struct schema *s, const void *value,
                               size_t depth);
static MO_MUST_USE bool encode_node(struct writer *w, const struct schema *s, const uint8_t *value,
                                    size_t depth) {
    const struct variant *v = s->variants;
    if (s->tag_size != 0) {
        while (v->wire != NULL && memcmp(value + s->tag_offset, v->tag, s->tag_size) != 0)
            v++;
        if (v->wire == NULL)
            return false;
    }
    if (!mo_buffer_append_byte(w->out, '(') || !mo_buffer_append_text(w->out, v->wire))
        return false;
    bool space = v->wire[0] != '\0';
    if (space && ++w->nodes > MO_LIMIT_CORE_NODES)
        return false;
    for (const struct field *f = v->fields; f->schema != NULL; f++) {
        MO_ASSERT(f->size == f->schema->size && f->offset <= s->size - f->size);
        if (space && !mo_buffer_append_byte(w->out, ' '))
            return false;
        if (!encode(w, f->schema, value + f->offset, depth + 1))
            return false;
        space = true;
    }
    return mo_buffer_append_byte(w->out, ')');
}
static MO_MUST_USE bool encode(struct writer *w, const struct schema *s, const void *value,
                               size_t depth) {
    if (value == NULL)
        return false;
    if (s->kind == SH_POINTER) {
        const void *p;
        MO_ASSERT(s->size == sizeof p);
        memcpy(&p, value, sizeof p);
        return encode(w, s->element, p, depth);
    }
    if (++w->nodes > MO_LIMIT_CORE_NODES)
        return false;
    if ((s->kind == SH_NODE || s->kind == SH_LIST) && depth > MO_LIMIT_CORE_DEPTH)
        return false;
    switch (s->kind) {
    case SH_NODE:
        return encode_node(w, s, value, depth);
    case SH_LIST: {
        const void *items;
        size_t count;
        memcpy(&items, (const uint8_t *)value + s->items_offset, sizeof items);
        memcpy(&count, (const uint8_t *)value + s->count_offset, sizeof count);
        if (count > MO_LIMIT_CORE_NODES || (items == NULL && count != 0))
            return false;
        if (!mo_buffer_append_byte(w->out, '('))
            return false;
        for (size_t i = 0; i < count; i++) {
            if (i != 0 && !mo_buffer_append_byte(w->out, ' '))
                return false;
            if (!encode(w, s->element, (const uint8_t *)items + i * s->element->size, depth + 1))
                return false;
        }
        return mo_buffer_append_byte(w->out, ')');
    }
    case SH_STRING:
        return quote(w->out, *(const mo_core_string *)value);
    case SH_INTEGER:
    case SH_NATURAL:
        return print_integer(w->out, *(const mo_core_integer *)value, s->kind == SH_NATURAL);
    case SH_BITS:
        return mo_buffer_append_text(w->out, "0x") &&
               mo_buffer_append_hex(w->out, *(const uint64_t *)value, 16);
    case SH_BOOLEAN:
        return mo_buffer_append_text(w->out, *(const bool *)value ? "true" : "false");
    case SH_CHARACTER: {
        /* Encode through a short buffer to share the exact quoting rules, including NUL. */
        struct mo_buffer buf;
        mo_buffer_init(&buf, 4);
        bool ok = append_scalar(&buf, *(const uint32_t *)value) &&
                  quote(w->out, (mo_core_string){(const char *)buf.bytes, buf.length});
        mo_buffer_free(&buf);
        return ok;
    }
    case SH_POINTER:
        MO_UNREACHABLE();
    }
    MO_UNREACHABLE();
}
bool mo_core_text_print(enum mo_core_text_kind kind, const void *value, struct mo_buffer *out) {
    MO_ASSERT(out != NULL);
    size_t start = out->length;
    struct writer w = {out, 3}; /* outer list, core, version */
    bool ok = mo_buffer_append_text(out, "(core " MO_CORE_TEXT_VERSION " ") &&
              encode(&w, document_schema(kind), value, 2) && mo_buffer_append_text(out, ")\n");
    if (!ok)
        out->length = start;
    return ok;
}
struct mo_core *mo_core_parse(struct mo_arena *arena, const char *text, size_t length,
                              struct mo_diags *diags) {
    return mo_core_text_parse(arena, MO_CORE_TEXT_PROGRAM, text, length, diags);
}
bool mo_core_print(const struct mo_core *core, struct mo_buffer *out) {
    return mo_core_text_print(MO_CORE_TEXT_PROGRAM, core, out);
}
