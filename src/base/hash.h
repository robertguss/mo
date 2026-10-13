/* SipHash-1-3, shared by process hash domains and deterministic tool seeds. */
#ifndef MO_HASH_H
#define MO_HASH_H

#include <stddef.h>
#include <stdint.h>

#include "base/base.h"

/* Native endian does not affect the result. key has two words; bytes may be
 * NULL only for an empty span. Hashing takes linear work and constant space. */
MO_MUST_USE uint64_t mo_hash(const uint64_t key[2], const void *bytes, size_t length);

#endif
