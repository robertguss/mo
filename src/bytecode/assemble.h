/* Bytecode text parsing and its indexed wire tables. No native type pointers are patched. */
#ifndef MO_BYTECODE_ASSEMBLE_H
#define MO_BYTECODE_ASSEMBLE_H

#include "bytecode/text.h"

struct mo_text_token {
    uint32_t start, length;
    char kind; /* w: word, q: quoted, c: Char, x: hex, n: newline, 0: end, or punctuation */
};
struct mo_text_reference {
    uint32_t source, index, space;
    struct mo_text_reference *next;
};
struct mo_text_position {
    uint32_t wire, text;
};
struct mo_text_entry {
    uint8_t *bytes;
    uint32_t length;
    const struct mo_text_position *positions;
    uint32_t position_count;
    uint32_t source;
};
struct mo_text_table {
    struct mo_text_entry *entries;
    uint32_t count, capacity, source;
};
struct mo_text_reader {
    const char *text;
    uint32_t length, cursor;
    struct mo_text_token token;
    struct mo_assemble_error *error;
    struct mo_arena arena;
    struct mo_buffer raw, positions;
    struct mo_text_table tables[MO_SECTION_COUNT];
    struct mo_text_reference *references, **last_reference;
    size_t wire_size;
    bool failed, shorthand;
};

void mo_text_next(struct mo_text_reader *r);
void mo_text_refuse(struct mo_text_reader *r, uint32_t offset, const char *reason);
MO_MUST_USE bool mo_text_is(const struct mo_text_reader *r, const char *text);
MO_MUST_USE bool mo_text_take(struct mo_text_reader *r, const char *text, const char *expected);
MO_MUST_USE bool mo_text_end_line(struct mo_text_reader *r);
MO_MUST_USE bool mo_text_ref(struct mo_text_reader *r, char prefix, bool optional, uint32_t *out);
MO_MUST_USE bool mo_text_enum(struct mo_text_reader *r, const char *const *names, uint32_t count,
                              const char *expected, uint32_t *out);
MO_MUST_USE bool mo_text_word(struct mo_text_reader *r, uint32_t word, uint32_t source);
MO_MUST_USE bool mo_text_bytes(struct mo_text_reader *r, const void *bytes, size_t length,
                               uint32_t source);
void mo_text_patch(struct mo_text_reader *r, uint32_t offset, uint32_t word);
MO_MUST_USE uint32_t mo_text_get(const struct mo_text_entry *e, uint32_t offset);
MO_MUST_USE bool mo_text_save(struct mo_text_reader *r, enum mo_bytecode_section section,
                              uint32_t index, uint32_t source);
MO_MUST_USE bool mo_text_quote(struct mo_text_reader *r, struct mo_bytecode_string *out);
MO_MUST_USE bool mo_text_payload(struct mo_text_reader *r, uint32_t type, bool shorthand);
MO_MUST_USE bool mo_text_indexed_section(struct mo_text_reader *r,
                                         enum mo_bytecode_section section);
MO_MUST_USE bool mo_text_instruction(struct mo_text_reader *r);
MO_MUST_USE bool mo_text_shorthand(struct mo_text_reader *r);

#endif
