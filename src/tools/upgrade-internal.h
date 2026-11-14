#ifndef MO_UPGRADE_INTERNAL_H
#define MO_UPGRADE_INTERNAL_H

#include "tools/upgrade.h"

bool mo_upgrade_error(struct mo_diags *diags, uint16_t code, const char *message);
bool mo_upgrade_file_bytes(const char *path, size_t limit, struct mo_buffer *out);
bool mo_upgrade_write_file_bytes(const char *path, const uint8_t *bytes, size_t length);

#endif
