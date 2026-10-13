/* Shorthand reservation, encounter-ordered pools and deferred operands. */
#ifndef MO_BYTECODE_SHORTHAND_H
#define MO_BYTECODE_SHORTHAND_H

#include "bytecode/text-index.h"

struct mo_text_decl {
    enum mo_bytecode_section section;
    uint32_t source, index;
    struct mo_bytecode_string name;
    struct mo_text_decl *next;
};
struct mo_text_request {
    bool constant, automatic;
    uint32_t kind, type, tag, function, offset, source;
    struct mo_bytecode_string payload; /* eagerly checked literal; empty for a forward type */
    struct mo_text_request *next;
};
struct mo_shorthand {
    struct mo_text_reader *r;
    struct mo_text_index *pools[4], *symbols[MO_SECTION_COUNT];
    struct mo_text_decl *first, **last;
    struct mo_text_request *requests, **last_request;
    uint32_t *function_types;
};

MO_MUST_USE bool mo_short_index(struct mo_bytecode_string name);
MO_MUST_USE bool mo_short_name(struct mo_shorthand *s, bool header, struct mo_bytecode_string *out);
MO_MUST_USE bool mo_short_lookup(struct mo_shorthand *s, enum mo_bytecode_section section,
                                 struct mo_bytecode_string name, uint32_t source, uint32_t *out);
MO_MUST_USE bool mo_short_reference(struct mo_shorthand *s, enum mo_bytecode_section section,
                                    char prefix, uint32_t *out);
MO_MUST_USE bool mo_short_string(struct mo_shorthand *s, struct mo_bytecode_string bytes,
                                 uint32_t source, uint32_t *out);
MO_MUST_USE bool mo_short_pool(struct mo_shorthand *s, enum mo_bytecode_section section,
                               uint32_t source, uint32_t *out);
MO_MUST_USE bool mo_short_type(struct mo_shorthand *s, unsigned depth, uint32_t *out);
MO_MUST_USE bool mo_short_function(struct mo_shorthand *s, const struct mo_text_decl *decl);
MO_MUST_USE bool mo_short_request(struct mo_shorthand *s, const struct mo_text_request *request);
MO_MUST_USE bool mo_short_literal(struct mo_shorthand *s, uint32_t type, bool shorthand);
MO_MUST_USE bool mo_short_finish(struct mo_shorthand *s);

#endif
