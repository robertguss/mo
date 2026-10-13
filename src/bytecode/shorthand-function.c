#include "bytecode/shorthand.h"

#include <string.h>

struct label_fix {
    struct mo_bytecode_string name;
    struct mo_buffer *buffer;
    uint32_t offset, source;
    struct label_fix *next;
};
struct function_reader {
    struct mo_shorthand *s;
    const struct mo_text_decl *decl;
    struct mo_buffer params, regs, captures, variables, spans, lines;
    struct mo_buffer variable_sources, span_sources, line_sources;
    uint32_t debug_sources[6];
    struct mo_text_index *labels;
    struct label_fix *fixes;
    uint32_t code_start;
    struct mo_bytecode_debug debug;
};

static MO_MUST_USE bool append(struct mo_text_reader *r, struct mo_buffer *b, uint32_t value) {
    if (!mo_buffer_append_le32(b, value)) {
        mo_text_refuse(r, b->length + 4 > b->limit ? r->token.start : 0,
                       b->length + 4 > b->limit ? "expected list within count limit" : "no memory");
        return false;
    }
    return true;
}
static MO_MUST_USE uint32_t get(const struct mo_buffer *b, uint32_t offset) {
    struct mo_text_entry entry = {.bytes = b->bytes, .length = (uint32_t)b->length};
    return mo_text_get(&entry, offset);
}
static void patch(struct mo_buffer *b, uint32_t offset, uint32_t value) {
    MO_ASSERT(offset <= b->length && b->length - offset >= 4);
    for (unsigned i = 0; i < 4; i++)
        b->bytes[offset + i] = (uint8_t)(value >> (8 * i));
}
static MO_MUST_USE bool comma(struct mo_text_reader *r) {
    return mo_text_take(r, ",", "expected , between operands or list items");
}
static MO_MUST_USE bool quoted_index(struct function_reader *f, bool optional, uint32_t *out) {
    struct mo_text_reader *r = f->s->r;
    if (optional && mo_text_is(r, "-")) {
        *out = MO_BYTECODE_NO_INDEX;
        mo_text_next(r);
        return !r->failed;
    }
    uint32_t source = r->token.start;
    if (r->token.kind != 'q') {
        mo_text_refuse(r, source, "expected quoted string");
        return false;
    }
    struct mo_bytecode_string bytes;
    return mo_text_quote(r, &bytes) && mo_short_string(f->s, bytes, source, out);
}
static MO_MUST_USE bool filename_index(struct function_reader *f, uint32_t *out) {
    struct mo_text_reader *r = f->s->r;
    uint32_t source = r->token.start;
    if (mo_text_is(r, "-") || r->token.kind == 'w')
        return mo_text_ref(r, 'P', true, out);
    struct mo_bytecode_string bytes;
    return mo_text_quote(r, &bytes) && mo_short_file(f->s, bytes, source, out);
}
static MO_MUST_USE bool location(struct function_reader *f, uint32_t *file, uint32_t *start,
                                 uint32_t *end) {
    struct mo_text_reader *r = f->s->r;
    uint32_t source = r->token.start;
    if (!filename_index(f, file) || !comma(r) || !mo_text_ref(r, 0, false, start) || !comma(r) ||
        !mo_text_ref(r, 0, false, end))
        return false;
    if (*start > *end || (*file == MO_BYTECODE_NO_INDEX && (*start != 0 || *end != 0))) {
        mo_text_refuse(r, source, "expected ordered location, or zero bounds for absent file");
        return false;
    }
    return true;
}

static MO_MUST_USE bool label(struct function_reader *f, struct mo_buffer *buffer) {
    struct mo_text_reader *r = f->s->r;
    uint32_t source = r->token.start;
    if (r->token.kind != 'w' || r->token.length < 2 || r->text[source] != '@') {
        mo_text_refuse(r, source, "expected @label or numeric word offset");
        return false;
    }
    if (r->text[source + 1] >= '0' && r->text[source + 1] <= '9') {
        uint32_t pc;
        return mo_text_ref(r, '@', false, &pc) && append(r, buffer, pc);
    }
    struct label_fix *fix = mo_arena_allocate(&r->arena, sizeof *fix, _Alignof(struct label_fix));
    if (fix == NULL) {
        mo_text_refuse(r, 0, "no memory");
        return false;
    }
    *fix = (struct label_fix){.name = {r->token.length, (const uint8_t *)r->text + source},
                              .buffer = buffer,
                              .offset = (uint32_t)buffer->length,
                              .source = source,
                              .next = f->fixes};
    f->fixes = fix;
    mo_text_next(r);
    return append(r, buffer, 0);
}

static MO_MUST_USE bool variables(struct function_reader *f) {
    struct mo_text_reader *r = f->s->r;
    if (!mo_text_take(r, "[", "expected variables list ["))
        return false;
    while (!r->failed && !mo_text_is(r, "]")) {
        uint32_t reg, name, source = r->token.start;
        if (!append(r, &f->variable_sources, source) ||
            !mo_text_take(r, "(", "expected variable (") || !mo_text_ref(r, 'r', false, &reg) ||
            !comma(r) || !quoted_index(f, false, &name) || !comma(r) ||
            !append(r, &f->variables, reg) || !append(r, &f->variables, name) ||
            !label(f, &f->variables) || !comma(r) || !label(f, &f->variables) ||
            !mo_text_take(r, ")", "expected ) after variable"))
            return false;
        if (mo_text_is(r, "]"))
            break;
        if (!comma(r))
            return false;
    }
    return mo_text_take(r, "]", "expected ] after variables");
}

static MO_MUST_USE bool lines(struct function_reader *f) {
    struct mo_text_reader *r = f->s->r;
    if (!mo_text_take(r, "[", "expected condition list ["))
        return false;
    while (!r->failed && !mo_text_is(r, "]")) {
        uint32_t file, start, end, text;
        if (!append(r, &f->line_sources, r->token.start) ||
            !mo_text_take(r, "(", "expected condition (") || !location(f, &file, &start, &end) ||
            !comma(r) || !quoted_index(f, false, &text) ||
            !mo_text_take(r, ")", "expected ) after condition") || !append(r, &f->lines, file) ||
            !append(r, &f->lines, start) || !append(r, &f->lines, end) ||
            !append(r, &f->lines, text))
            return false;
        if (mo_text_is(r, "]"))
            break;
        if (!comma(r))
            return false;
    }
    return mo_text_take(r, "]", "expected ] after conditions");
}

static MO_MUST_USE bool metadata(struct function_reader *f) {
    struct mo_text_reader *r = f->s->r;
    static const char *const keywords[] = {"variables", "text",        "declaration",
                                           "subject",   "expectation", "lines"};
    for (unsigned i = 0; i < 6; i++) {
        if (!mo_text_is(r, keywords[i]))
            continue;
        f->debug_sources[i] = r->token.start;
        mo_text_next(r);
        bool ok;
        if (i == 0)
            ok = variables(f);
        else if (i == 1)
            ok = quoted_index(f, false, &f->debug.text);
        else if (i == 2)
            ok = mo_text_take(r, "(", "expected declaration location (") &&
                 location(f, &f->debug.file, &f->debug.start, &f->debug.end) &&
                 mo_text_take(r, ")", "expected ) after declaration location");
        else if (i == 3) {
            if (mo_text_is(r, "-")) {
                mo_text_next(r);
                ok = !r->failed;
            } else
                ok = mo_short_reference(f->s, MO_SECTION_FUNCTIONS, 'f', &f->debug.subject);
        } else if (i == 4) {
            uint32_t expectation;
            ok = mo_text_enum(r, mo_bytecode_expectation_names, 3,
                              "expected none, allows or forbids", &expectation);
            if (ok)
                f->debug.expectation = (enum mo_bytecode_anchor_expectation)expectation;
        } else
            ok = lines(f);
        if (!ok || !mo_text_end_line(r))
            return false;
    }
    return true;
}

static MO_MUST_USE bool function_type(struct function_reader *f, uint32_t result) {
    struct mo_text_reader *r = f->s->r;
    struct mo_buffer saved = r->raw, positions = r->positions;
    mo_buffer_init(&r->raw, MO_LIMIT_BYTECODE_SIZE);
    mo_buffer_init(&r->positions, 2 * (size_t)MO_LIMIT_BYTECODE_SIZE);
    bool ok = mo_text_word(r, MO_TYPE_FUNCTION, f->decl->source) &&
              mo_text_word(r, (uint32_t)(f->params.length / 8), f->decl->source);
    for (uint32_t i = 0; i < f->params.length && ok; i += 8)
        ok = mo_text_word(r, get(&f->params, i), f->decl->source);
    if (ok)
        ok = mo_text_word(r, result, f->decl->source) &&
             mo_short_pool(f->s, MO_SECTION_TYPES, f->decl->source,
                           &f->s->function_types[f->decl->index]);
    mo_buffer_free(&r->positions);
    mo_buffer_free(&r->raw);
    r->raw = saved;
    r->positions = positions;
    return ok;
}

static MO_MUST_USE bool header(struct function_reader *f) {
    struct mo_text_reader *r = f->s->r;
    struct mo_bytecode_string ignored;
    uint32_t name, kind, result, module;
    mo_text_next(r);
    if (!mo_short_name(f->s, true, &ignored) ||
        !mo_short_string(f->s, f->decl->name, f->decl->source, &name) ||
        !mo_text_take(r, "(", "expected function parameters ("))
        return false;
    while (!r->failed && !mo_text_is(r, ")")) {
        uint32_t type, ownership, source = r->token.start, unused;
        static const char *const modes[] = {"owned", "borrowed"};
        if (!mo_short_name(f->s, true, &ignored) ||
            !mo_short_string(f->s, ignored, source, &unused) ||
            !mo_text_take(r, ":", "expected : after parameter name") ||
            !mo_short_type(f->s, 0, &type) ||
            !mo_text_enum(r, modes, 2, "expected owned or borrowed", &ownership) ||
            !append(r, &f->params, type) || !append(r, &f->params, ownership) ||
            !append(r, &f->regs, type))
            return false;
        if (mo_text_is(r, ")"))
            break;
        if (!comma(r))
            return false;
    }
    if (!mo_text_take(r, ")", "expected ) after parameters") ||
        !mo_text_take(r, ":", "expected : before result") || !mo_short_type(f->s, 0, &result) ||
        !mo_text_enum(r, mo_bytecode_function_names, MO_FUNCTION_ANCHOR + 1,
                      "expected function kind", &kind))
        return false;
    if (kind == MO_FUNCTION_CLOSURE && !function_type(f, result))
        return false;
    bool spec = mo_text_is(r, "spec");
    if (spec)
        mo_text_next(r);
    if (!mo_text_take(r, "in", "expected in module") ||
        !mo_short_reference(f->s, MO_SECTION_MODULES, 'm', &module))
        return false;
    if (mo_text_is(r, "captures")) {
        mo_text_next(r);
        if (!mo_text_take(r, "[", "expected captures list ["))
            return false;
        while (!r->failed && !mo_text_is(r, "]")) {
            uint32_t type;
            if (!mo_short_type(f->s, 0, &type) || !append(r, &f->captures, type))
                return false;
            if (mo_text_is(r, "]"))
                break;
            if (!comma(r))
                return false;
        }
        if (!mo_text_take(r, "]", "expected ] after captures"))
            return false;
    }
    if (kind == MO_FUNCTION_CLOSURE) {
        struct mo_buffer saved = r->raw, positions = r->positions;
        mo_buffer_init(&r->raw, MO_LIMIT_BYTECODE_SIZE);
        mo_buffer_init(&r->positions, MO_LIMIT_BYTECODE_SIZE);
        uint32_t env;
        bool ok = mo_text_word(r, MO_TYPE_ENV, f->decl->source) &&
                  mo_text_word(r, f->decl->index, f->decl->source) &&
                  mo_short_pool(f->s, MO_SECTION_TYPES, f->decl->source, &env);
        mo_buffer_free(&r->raw);
        mo_buffer_free(&r->positions);
        r->raw = saved;
        r->positions = positions;
        if (!ok || !append(r, &f->regs, env))
            return false;
    }
    if (!mo_text_end_line(r) || !mo_text_take(r, "registers", "expected registers") ||
        !mo_text_take(r, "[", "expected register list ["))
        return false;
    while (!r->failed && !mo_text_is(r, "]")) {
        uint32_t reg, type, source = r->token.start;
        if (!mo_text_ref(r, 'r', false, &reg))
            return false;
        if (reg != f->regs.length / 4) {
            mo_text_refuse(r, source, "expected consecutive additional register number");
            return false;
        }
        if (!mo_text_take(r, ":", "expected : after register") || !mo_short_type(f->s, 0, &type) ||
            !append(r, &f->regs, type))
            return false;
        if (mo_text_is(r, "]"))
            break;
        if (!comma(r))
            return false;
    }
    return mo_text_take(r, "]", "expected ] after registers") && mo_text_end_line(r) &&
           mo_text_word(r, name, f->decl->source) && mo_text_word(r, module, f->decl->source) &&
           mo_text_word(r, kind, f->decl->source) && mo_text_word(r, spec, f->decl->source) &&
           mo_text_word(r, (uint32_t)(f->params.length / 8), f->decl->source) &&
           mo_text_bytes(r, f->params.bytes, f->params.length, f->decl->source) &&
           mo_text_word(r, result, f->decl->source) &&
           mo_text_word(r, (uint32_t)(f->captures.length / 4), f->decl->source) &&
           mo_text_bytes(r, f->captures.bytes, f->captures.length, f->decl->source) &&
           mo_text_word(r, (uint32_t)(f->regs.length / 4), f->decl->source) &&
           mo_text_bytes(r, f->regs.bytes, f->regs.length, f->decl->source);
}

static MO_MUST_USE bool library(struct function_reader *f, bool host, uint32_t *out) {
    static const char *const primitives[] = {
#define PRIM(id, module, name, signature, params, bugs) module "." name,
#include "base/prims.def"
    };
    static const char *const hosts[] = {
#define HOSTOP(id, module, name, signature, params, bugs, spec_only) module "." name,
#include "base/hostops.def"
    };
    struct mo_text_reader *r = f->s->r;
    char prefix = host ? 'h' : 'p';
    if (r->token.kind == 'w' && r->text[r->token.start] == prefix &&
        mo_short_index((struct mo_bytecode_string){r->token.length,
                                                   (const uint8_t *)r->text + r->token.start}))
        return mo_text_ref(r, prefix, false, out);
    uint32_t source = r->token.start;
    struct mo_bytecode_string name;
    if (!mo_short_name(f->s, false, &name))
        return false;
    const char *const *items = host ? hosts : primitives;
    uint32_t count = host ? MO_HOST_COUNT : MO_PRIM_COUNT;
    for (uint32_t i = 0; i < count; i++) {
        if (strlen(items[i]) == name.length && memcmp(items[i], name.bytes, name.length) == 0) {
            *out = i;
            return true;
        }
    }
    mo_text_refuse(r, source,
                   host ? "expected full host operation name" : "expected full primitive name");
    return false;
}

static MO_MUST_USE bool layout(struct function_reader *f, uint32_t offset, uint32_t *out) {
    struct mo_text_reader *r = f->s->r;
    uint32_t source = r->token.start;
    if (r->token.kind == 'w' && r->text[source] == 'L' &&
        mo_short_index(
            (struct mo_bytecode_string){r->token.length, (const uint8_t *)r->text + source})) {
        return mo_text_ref(r, 'L', false, out);
    }
    struct mo_text_request q = {.kind = MO_BYTECODE_NO_INDEX,
                                .type = MO_BYTECODE_NO_INDEX,
                                .tag = MO_BYTECODE_NO_INDEX,
                                .function = f->decl->index,
                                .offset = offset,
                                .source = source};
    if (mo_text_is(r, "closure")) {
        mo_text_next(r);
        q.kind = MO_LAYOUT_CLOSURE;
        if (!mo_text_take(r, "(", "expected ( after closure") ||
            !mo_short_reference(f->s, MO_SECTION_FUNCTIONS, 'f', &q.tag) ||
            !mo_text_take(r, ")", "expected ) after closure function"))
            return false;
    } else {
        struct mo_text_token token = r->token;
        uint32_t cursor = r->cursor;
        struct mo_bytecode_string name;
        bool ordinary =
            token.kind == '(' || (token.kind == 'w' && r->text[source] == 't' &&
                                  mo_short_index((struct mo_bytecode_string){
                                      token.length, (const uint8_t *)r->text + source}));
        if (!ordinary) {
            if (!mo_short_name(f->s, false, &name))
                return false;
            struct mo_text_index *found = mo_text_find(f->s->symbols[MO_SECTION_TYPES], name);
            if (found == NULL) {
                uint32_t dot = name.length;
                while (dot != 0 && name.bytes[dot - 1] != '.')
                    dot--;
                if (dot != 0) {
                    struct mo_bytecode_string type_name = {dot - 1, name.bytes};
                    found = mo_text_find(f->s->symbols[MO_SECTION_TYPES], type_name);
                    struct mo_bytecode_string case_name = {name.length - dot, name.bytes + dot};
                    struct mo_text_index *case_string =
                        mo_text_find(f->s->pools[MO_SECTION_STRINGS], case_name);
                    if (found != NULL && case_string != NULL) {
                        q.kind = MO_LAYOUT_CASE;
                        q.type = found->value;
                        q.tag = case_string->value;
                    }
                }
            } else
                ordinary = true;
            if (q.kind != MO_LAYOUT_CASE && !ordinary) {
                /* Structural layout types such as List are parsed for the normal type error. */
                ordinary = true;
            }
        }
        if (ordinary) {
            r->token = token;
            r->cursor = cursor;
            if (!mo_short_type(f->s, 0, &q.type))
                return false;
        }
    }
    *out = 0;
    return mo_short_request(f->s, &q);
}

static MO_MUST_USE bool skip_literal(struct mo_text_reader *r) {
    uint32_t depth = 0;
    bool any = false;
    while (!r->failed && r->token.kind != 0 && r->token.kind != 'n') {
        if (depth == 0 && r->token.kind == ',')
            break;
        if (r->token.kind == '(' || r->token.kind == '[') {
            if (++depth > MO_LIMIT_NESTING_DEPTH) {
                mo_text_refuse(r, r->token.start, "expected literal within nesting limit");
                return false;
            }
        } else if (r->token.kind == ')' || r->token.kind == ']') {
            if (depth == 0) {
                mo_text_refuse(r, r->token.start, "expected balanced literal");
                return false;
            }
            depth--;
        }
        any = true;
        mo_text_next(r);
    }
    if (!any || depth != 0) {
        mo_text_refuse(r, r->token.start, "expected complete literal");
        return false;
    }
    return !r->failed;
}

static MO_MUST_USE bool constant(struct function_reader *f, struct mo_text_request *q) {
    struct mo_text_reader *r = f->s->r;
    if (q->type >= r->tables[MO_SECTION_TYPES].count)
        return skip_literal(r) && mo_short_request(f->s, q);
    struct mo_buffer saved = r->raw, positions = r->positions;
    mo_buffer_init(&r->raw, MO_LIMIT_BYTECODE_SIZE);
    mo_buffer_init(&r->positions, 2 * (size_t)MO_LIMIT_BYTECODE_SIZE);
    bool ok = mo_text_word(r, q->type, q->source) && mo_short_literal(f->s, q->type, true);
    if (ok && r->token.kind != 'n' && r->token.kind != 0 && r->token.kind != ',') {
        mo_text_refuse(r, r->token.start, "expected end of literal operand");
        ok = false;
    }
    if (ok) {
        q->payload.length = (uint32_t)r->raw.length;
        q->payload.bytes = mo_arena_copy(&r->arena, r->raw.bytes, r->raw.length, 1);
        if (q->payload.bytes == NULL) {
            mo_text_refuse(r, 0, "no memory");
            ok = false;
        }
    }
    mo_buffer_free(&r->positions);
    mo_buffer_free(&r->raw);
    r->raw = saved;
    r->positions = positions;
    return ok && mo_short_request(f->s, q);
}

static MO_MUST_USE bool instruction(struct function_reader *f) {
    struct mo_text_reader *r = f->s->r;
    uint32_t source = r->token.start, offset = (uint32_t)r->raw.length;
    const struct mo_opcode_info *op = NULL;
    for (uint32_t i = 0; i < MO_OPCODE_COUNT; i++)
        if (mo_text_is(r, mo_opcodes[i].name)) {
            op = &mo_opcodes[i];
            break;
        }
    if (op == NULL) {
        mo_text_refuse(r, source, "expected opcode, @label: or span");
        return false;
    }
    mo_text_next(r);
    if (!mo_text_word(r, op->number, source))
        return false;
    uint32_t count = 0, fixed[MO_OPCODE_FIXED_OPERANDS] = {0};
    while (!r->failed && r->token.kind != 'n' && r->token.kind != 0) {
        if (count >= (uint32_t)op->fixed_count + op->variable_limit) {
            mo_text_refuse(r, r->token.start, "expected instruction within operand limit");
            return false;
        }
        if (count != 0 && !comma(r))
            return false;
        uint32_t value = 0, at = r->token.start;
        enum mo_operand_kind kind =
            count < op->fixed_count ? op->fixed[count].kind : op->variable.kind;
        bool ok;
        switch (kind) {
        case MO_OPERAND_REGISTER:
            ok = mo_text_ref(r, 'r', false, &value);
            break;
        case MO_OPERAND_TYPE:
            ok = mo_short_type(f->s, 0, &value);
            break;
        case MO_OPERAND_LAYOUT:
            ok = layout(f, (uint32_t)r->raw.length, &value);
            break;
        case MO_OPERAND_FUNCTION:
            ok = mo_short_reference(f->s, MO_SECTION_FUNCTIONS, 'f', &value);
            break;
        case MO_OPERAND_GLOBAL:
            ok = mo_short_reference(f->s, MO_SECTION_GLOBALS, 'g', &value);
            break;
        case MO_OPERAND_PRIMITIVE:
            ok = library(f, false, &value);
            break;
        case MO_OPERAND_HOST:
            ok = library(f, true, &value);
            break;
        case MO_OPERAND_CASE:
        case MO_OPERAND_FIELD:
            ok = mo_text_ref(r, 0, false, &value);
            break;
        case MO_OPERAND_TARGET:
            /* label() appends directly; retain its own token position. */
            ok = label(f, &r->raw);
            if (ok) {
                struct mo_text_position pos = {(uint32_t)r->raw.length - 4, at};
                ok = mo_buffer_append(&r->positions, &pos, sizeof pos);
                if (!ok)
                    mo_text_refuse(r, 0, "no memory");
            }
            if (!ok)
                return false;
            count++;
            continue;
        case MO_OPERAND_CONSTANT:
            if (r->token.length > 1 && r->text[at] == 'k' && r->text[at + 1] >= '0' &&
                r->text[at + 1] <= '9') {
                ok = mo_text_ref(r, 'k', false, &value);
            } else if (fixed[0] >= f->regs.length / 4) {
                mo_text_refuse(r, at, "expected literal destination register in range");
                return false;
            } else {
                struct mo_text_request q = {.constant = true,
                                            .type = get(&f->regs, fixed[0] * 4),
                                            .function = f->decl->index,
                                            .offset = (uint32_t)r->raw.length,
                                            .source = at};
                ok = constant(f, &q);
            }
            break;
        case MO_OPERAND_NONE:
            MO_UNREACHABLE();
        }
        if (!ok || !mo_text_word(r, value, at))
            return false;
        if (count < MO_OPCODE_FIXED_OPERANDS)
            fixed[count] = value;
        count++;
    }
    if (count < op->fixed_count) {
        mo_text_refuse(r, r->token.start, "expected instruction operand");
        return false;
    }
    mo_text_patch(r, offset, op->number | ((count - op->fixed_count) << MO_OPCODE_VARIABLE_SHIFT));
    if (op->number == MO_OP_MAKE_CASE || op->number == MO_OP_MAKE_CLOSURE) {
        struct mo_text_request q = {
            .kind = op->number == MO_OP_MAKE_CASE ? MO_LAYOUT_CASE : MO_LAYOUT_CLOSURE,
            .type = op->number == MO_OP_MAKE_CASE ? fixed[1] : MO_BYTECODE_NO_INDEX,
            .tag = op->number == MO_OP_MAKE_CASE ? fixed[2] : fixed[1],
            .function = f->decl->index,
            .offset = MO_BYTECODE_NO_INDEX,
            .source = source};
        /* A numeric tag is distinguished from a symbolic layout's case-name index. */
        q.automatic = true;
        if (!mo_short_request(f->s, &q))
            return false;
    }
    return mo_text_end_line(r);
}

static MO_MUST_USE bool code(struct function_reader *f) {
    struct mo_text_reader *r = f->s->r;
    uint32_t offset = (uint32_t)r->raw.length, instructions = 0;
    if (!mo_text_word(r, 0, r->token.start))
        return false;
    f->code_start = (uint32_t)r->raw.length;
    while (!r->failed && !mo_text_is(r, "end")) {
        uint32_t source = r->token.start, pc = (uint32_t)((r->raw.length - f->code_start) / 4);
        if (mo_text_is(r, "span")) {
            mo_text_next(r);
            uint32_t file, start, end;
            if (!filename_index(f, &file) || !mo_text_ref(r, 0, false, &start) ||
                !mo_text_ref(r, 0, false, &end))
                return false;
            if (start > end || (file == MO_BYTECODE_NO_INDEX && (start != 0 || end != 0)) ||
                (f->spans.length != 0 && get(&f->spans, (uint32_t)f->spans.length - 16) == pc)) {
                mo_text_refuse(r, source,
                               "expected one ordered span per offset, with zero absent bounds");
                return false;
            }
            if (!append(r, &f->span_sources, source) || !append(r, &f->spans, pc) ||
                !append(r, &f->spans, file) || !append(r, &f->spans, start) ||
                !append(r, &f->spans, end) || !mo_text_end_line(r))
                return false;
        } else if (r->token.kind == 'w' && r->token.length > 1 && r->text[source] == '@') {
            struct mo_bytecode_string name = {r->token.length, (const uint8_t *)r->text + source};
            if (r->text[source + 1] >= '0' && r->text[source + 1] <= '9') {
                mo_text_refuse(r, source, "expected symbolic label, not numeric offset definition");
                return false;
            }
            if (mo_text_find(f->labels, name) != NULL) {
                mo_text_refuse(r, source, "expected unique label");
                return false;
            }
            if (mo_text_insert(r, &f->labels, name, pc) == NULL)
                return false;
            mo_text_next(r);
            if (!mo_text_take(r, ":", "expected : after label") || !mo_text_end_line(r))
                return false;
        } else {
            if (++instructions > MO_LIMIT_FUNCTION_INSTRUCTIONS) {
                mo_text_refuse(r, source, "expected code within instruction limit");
                return false;
            }
            if (!instruction(f))
                return false;
            if ((r->raw.length - f->code_start) / 4 > MO_LIMIT_FUNCTION_CODE_WORDS) {
                mo_text_refuse(r, source, "expected code within word limit");
                return false;
            }
        }
    }
    for (struct label_fix *fix = f->fixes; fix != NULL; fix = fix->next) {
        struct mo_text_index *found = mo_text_find(f->labels, fix->name);
        if (found == NULL) {
            mo_text_refuse(r, fix->source, "expected defined label in this function");
            return false;
        }
        patch(fix->buffer, fix->offset, found->value);
    }
    mo_text_patch(r, offset, (uint32_t)((r->raw.length - f->code_start) / 4));
    return mo_text_take(r, "end", "expected function end") && mo_text_end_line(r);
}

static MO_MUST_USE bool debug_array(struct function_reader *f, const struct mo_buffer *entries,
                                    const struct mo_buffer *sources, uint32_t source) {
    struct mo_text_reader *r = f->s->r;
    MO_ASSERT(entries->length / 16 == sources->length / 4);
    if (!mo_text_word(r, (uint32_t)(entries->length / 16), source))
        return false;
    for (uint32_t i = 0; i < entries->length / 16; i++)
        if (!mo_text_bytes(r, entries->bytes + i * 16, 16, get(sources, i * 4)))
            return false;
    return true;
}

bool mo_short_function(struct mo_shorthand *s, const struct mo_text_decl *decl) {
    struct mo_text_reader *r = s->r;
    struct function_reader f = {.s = s,
                                .decl = decl,
                                .debug = {.text = MO_BYTECODE_NO_INDEX,
                                          .file = MO_BYTECODE_NO_INDEX,
                                          .subject = MO_BYTECODE_NO_INDEX}};
    mo_buffer_init(&f.params, MO_LIMIT_BYTECODE_PARAMETERS * 8);
    mo_buffer_init(&f.regs, MO_LIMIT_FUNCTION_REGISTERS * 4);
    mo_buffer_init(&f.captures, MO_LIMIT_FIELDS * 4);
    mo_buffer_init(&f.variables, MO_LIMIT_BYTECODE_SIZE);
    mo_buffer_init(&f.spans, MO_LIMIT_BYTECODE_SIZE);
    mo_buffer_init(&f.lines, MO_LIMIT_BYTECODE_SIZE);
    mo_buffer_init(&f.variable_sources, MO_LIMIT_BYTECODE_SIZE);
    mo_buffer_init(&f.span_sources, MO_LIMIT_BYTECODE_SIZE);
    mo_buffer_init(&f.line_sources, MO_LIMIT_BYTECODE_SIZE);
    for (unsigned i = 0; i < 6; i++)
        f.debug_sources[i] = decl->source;
    bool ok = header(&f) && metadata(&f) && code(&f) &&
              mo_text_save(r, MO_SECTION_FUNCTIONS, decl->index, decl->source);
    if (ok)
        ok = debug_array(&f, &f.spans, &f.span_sources, decl->source) &&
             debug_array(&f, &f.variables, &f.variable_sources, f.debug_sources[0]) &&
             mo_text_word(r, f.debug.text, f.debug_sources[1]) &&
             mo_text_word(r, f.debug.file, f.debug_sources[2]) &&
             mo_text_word(r, f.debug.start, f.debug_sources[2]) &&
             mo_text_word(r, f.debug.end, f.debug_sources[2]) &&
             mo_text_word(r, f.debug.subject, f.debug_sources[3]) &&
             mo_text_word(r, f.debug.expectation, f.debug_sources[4]) &&
             debug_array(&f, &f.lines, &f.line_sources, f.debug_sources[5]) &&
             mo_text_save(r, MO_SECTION_DEBUG, decl->index, decl->source);
    mo_buffer_free(&f.line_sources);
    mo_buffer_free(&f.span_sources);
    mo_buffer_free(&f.variable_sources);
    mo_buffer_free(&f.lines);
    mo_buffer_free(&f.spans);
    mo_buffer_free(&f.variables);
    mo_buffer_free(&f.captures);
    mo_buffer_free(&f.regs);
    mo_buffer_free(&f.params);
    return ok;
}
