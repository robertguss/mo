#include "vm/internal.h"

bool mo_vm_number(uint32_t opcode, struct mo_cell a, struct mo_cell b, struct mo_cell *out,
                  const char **reason) {
    (void)opcode;
    (void)a;
    (void)b;
    (void)out;
    *reason = "internal error: arithmetic unavailable";
    return false;
}
