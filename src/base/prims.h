/* Primitive ids are the zero-based rows of the one shared contract table. */
#ifndef MO_PRIMS_H
#define MO_PRIMS_H

enum mo_primitive_id {
#define PRIM(id, module, name, signature, params, bugs) MO_PRIM_##id,
#include "base/prims.def"
    MO_PRIM_COUNT,
};

#endif
