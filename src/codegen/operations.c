#include "codegen/operations.h"

static const struct mo_codegen_operation primitives[] = {
#define PRIM(id, module, name, signature, params, bugs)                                            \
    {MO_OP_PRIM, MO_PRIM_##id, signature, params},
#include "base/prims.def"
};

bool mo_codegen_operation(const struct mo_core_prim *p, struct mo_codegen_operation *out) {
    if (p->tag == MO_CORE_PRIM_TEXT && p->as.text == MO_CORE_TEXT_OP_CONCAT) {
        *out = primitives[MO_PRIM_TEXT_CONCAT];
        return true;
    }
    return false;
}
