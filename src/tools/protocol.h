#ifndef MO_PROTOCOL_H
#define MO_PROTOCOL_H

#include "bytecode/bytecode.h"
#include "heap/heap.h"

struct mo_protocol_descriptor {
    uint32_t type;
    struct mo_cell value;
};

enum mo_protocol_value_status {
    MO_PROTOCOL_VALUE_OK,
    MO_PROTOCOL_VALUE_INVALID,
    MO_PROTOCOL_VALUE_MEMORY,
    MO_PROTOCOL_VALUE_LIMIT,
};

MO_MUST_USE enum mo_protocol_value_status
mo_protocol_scalar_pack(const struct mo_types *protocol, uint32_t scalar, struct mo_heap *to,
                        enum mo_type_kind kind, struct mo_cell value, size_t byte_limit,
                        struct mo_cell *out);
MO_MUST_USE enum mo_protocol_value_status
mo_protocol_scalar_unpack(const struct mo_types *protocol, uint32_t scalar, struct mo_heap *to,
                          enum mo_type_kind kind, struct mo_cell value, size_t byte_limit,
                          struct mo_cell *out);

MO_MUST_USE struct mo_image *mo_protocol_build(void);
MO_MUST_USE bool mo_protocol_body_marks(const uint8_t *bytes, size_t size, size_t *count);
MO_MUST_USE bool mo_protocol_descriptor(const struct mo_image *protocol, struct mo_heap *heap,
                                        const struct mo_image *service, uint32_t index,
                                        struct mo_protocol_descriptor *out);

#endif
