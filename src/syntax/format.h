/* The tree-only source printer (D238). */
#ifndef MO_SYNTAX_FORMAT_H
#define MO_SYNTAX_FORMAT_H

#include "base/buffer.h"
#include "syntax/syntax.h"

/* Display columns in a valid UTF-8 segment without line breaks. The caller
 * supplies Unicode16's implementation; syntax depends only on base. */
typedef uint32_t (*mo_format_width_fn)(const uint8_t *bytes, size_t length);
struct mo_format_options {
    uint32_t width;
    mo_format_width_fn display_width;
};

/* Prints a complete, successfully parsed tree into an emptied buffer. On
 * memory/output/depth/source-line refusal the buffer is empty and false is returned.
 * Chooses breaks from the tree, including its comment anchors and blank-line
 * bits, without reading source storage or changing the tree. */
MO_MUST_USE bool mo_format(const struct mo_module_syntax *module, struct mo_format_options options,
                           struct mo_buffer *output);

#endif
