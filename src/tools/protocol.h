#ifndef MO_PROTOCOL_H
#define MO_PROTOCOL_H

#include "bytecode/bytecode.h"

MO_MUST_USE struct mo_image *mo_protocol_build(void);
MO_MUST_USE bool mo_protocol_body_marks(const uint8_t *bytes, size_t size, size_t *count);

#endif
