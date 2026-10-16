/* Canonical scalar literal values, private to check. */
#ifndef MO_CHECK_MAP_KEYS_H
#define MO_CHECK_MAP_KEYS_H

#include "syntax/syntax.h"

enum mo_map_key_category {
    MO_MAP_KEY_UNKNOWN,
    MO_MAP_KEY_NUMBER,
    MO_MAP_KEY_TEXT,
    MO_MAP_KEY_CHAR,
    MO_MAP_KEY_BOOL,
    MO_MAP_KEY_NAME /* Caller identifies a bare name using its source bytes. */
};

struct mo_map_literal_key {
    const uint8_t *bytes;
    size_t length;
    enum mo_map_key_category category;
    bool negative;
};

struct mo_map_key_context {
    void *context;
    bool (*work)(void *context, size_t amount);
    void *(*allocate)(void *context, size_t bytes);
};

/* True with UNKNOWN means an expression outside literal syntax. Other keys
 * compare equal exactly when category, negative, length and bytes agree.
 * Numeric bytes contain a normalized decimal spelling (including its point).
 * Decodes validated syntax only. Each allocation's bytes and each loop's work
 * are charged before proceeding; false means a callback reported the failure. */
MO_MUST_USE bool mo_map_literal_key(const struct mo_map_key_context *context,
                                    const struct mo_syntax *syntax, struct mo_map_literal_key *out);

#endif
