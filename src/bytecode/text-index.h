/* Ordered byte keys for incremental text pools and symbols. */
#ifndef MO_BYTECODE_TEXT_INDEX_H
#define MO_BYTECODE_TEXT_INDEX_H

#include "bytecode/assemble.h"

struct mo_text_index {
    struct mo_bytecode_string key;
    uint32_t value;
    unsigned height;
    struct mo_text_index *left, *right;
};
MO_MUST_USE struct mo_text_index *mo_text_find(struct mo_text_index *root,
                                               struct mo_bytecode_string key);
/* Keys belong to the reader's arena or input. Retain the first index for equal seeds. */
MO_MUST_USE struct mo_text_index *mo_text_insert(struct mo_text_reader *r,
                                                 struct mo_text_index **root,
                                                 struct mo_bytecode_string key, uint32_t value);

#endif
