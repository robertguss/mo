#ifndef MO_COMPILE_METADATA_H
#define MO_COMPILE_METADATA_H
#include "base/binary.h"
#include "base/bundle.h"
/* Structural evidence emitted from a checked service, never executable bytecode.
 * Handler digests cover types, exposure shapes, argument names and documentation. */
struct mo_compile_metadata {
    struct mo_bundle_span token, schema, tools, timers;
    uint32_t type_count, tool_count, timer_count;
    struct mo_bundle_file types[MO_LIMIT_BUNDLE_FILES];
};
struct mo_compile_handler {
    struct mo_bundle_span name;
    uint8_t digest[32];
    uint64_t interval;
};
/* Shared MOCC/MOCS framing. Text is length-prefixed UTF-8 without NUL.
 * Files require strictly ordered generated UpgradeOld paths and valid text.
 * The caller provides space for count files. On failure the input position
 * and scratch outputs may have advanced; enclosing readers publish only success. */
MO_MUST_USE bool mo_compile_text_read(struct mo_binary *in, size_t maximum,
                                      struct mo_bundle_span *out);
MO_MUST_USE bool mo_compile_files_read(struct mo_binary *in, uint32_t count,
                                       struct mo_bundle_file *files);
MO_MUST_USE bool mo_compile_metadata_read(const uint8_t *bytes, size_t length,
                                          struct mo_compile_metadata *out);
/* Each call consumes one previously validated handler section entry. */
MO_MUST_USE bool mo_compile_handler_read(struct mo_binary *input, bool timer,
                                         struct mo_compile_handler *out);
#endif
