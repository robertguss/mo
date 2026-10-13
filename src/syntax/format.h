/* The source printer. The source-break policy is a measurement prototype:
 * it reads gaps through spans until Robert selects the layout contract. */
#ifndef MO_SYNTAX_FORMAT_H
#define MO_SYNTAX_FORMAT_H

#include "base/buffer.h"
#include "syntax/syntax.h"

enum mo_format_policy { MO_FORMAT_WIDTH, MO_FORMAT_SOURCE_PROTOTYPE };
/* Display columns in a valid UTF-8 segment without line breaks. The caller
 * supplies Unicode16's implementation; syntax depends only on base. */
typedef uint32_t (*mo_format_width_fn)(const uint8_t *bytes, size_t length);
struct mo_format_options {
    enum mo_format_policy policy;
    uint32_t width;
    mo_format_width_fn display_width;
};

/* Prints a complete, successfully parsed tree into an emptied buffer. On
 * memory/output/depth refusal the buffer is empty and false is returned.
 * WIDTH needs no source storage. SOURCE_PROTOTYPE requires the original
 * sources. Neither policy changes the tree or the sources. */
MO_MUST_USE bool mo_format(const struct mo_module_syntax *module, const struct mo_sources *sources,
                           struct mo_format_options options, struct mo_buffer *output);

#endif
