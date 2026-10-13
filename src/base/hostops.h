/* Operation numbers shared by bytecode and the host. */
#ifndef MO_HOSTOPS_H
#define MO_HOSTOPS_H

enum mo_host_id {
#define HOSTOP(id, module, name, signature, params, bugs, spec_only) MO_HOST_##id,
#include "base/hostops.def"
    MO_HOST_COUNT,
};

#endif
