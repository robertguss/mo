#ifndef MO_PROTOCOL_H
#define MO_PROTOCOL_H

#include "bytecode/bytecode.h"
#include "heap/heap.h"

struct mo_protocol_descriptor {
    uint32_t type;
    struct mo_cell value;
};

MO_MUST_USE struct mo_image *mo_protocol_build(void);
MO_MUST_USE bool mo_protocol_body_marks(const uint8_t *bytes, size_t size, size_t *count);
MO_MUST_USE bool mo_protocol_descriptor(const struct mo_image *protocol, struct mo_heap *heap,
                                        const struct mo_image *service, uint32_t index,
                                        struct mo_protocol_descriptor *out);

#endif
