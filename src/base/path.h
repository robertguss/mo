/* The one spelling of a dotted CapCase module name as a relative file path. */
#ifndef MO_PATH_H
#define MO_PATH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/base.h"
#include "base/limits.h"

/* XMLReader -> xml-reader.mo, Shop.OrderBook -> shop/order-book.mo. Rejects
 * invalid names or output longer than MO_LIMIT_PATH_SIZE, without allocation.
 * Output is NUL-terminated on success; its contents on failure are unspecified. */
MO_MUST_USE bool mo_module_path(const uint8_t *name, size_t length,
                                char out[MO_LIMIT_PATH_SIZE + 1]);

#endif
