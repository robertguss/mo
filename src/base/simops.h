#ifndef MO_SIMOPS_H
#define MO_SIMOPS_H

enum mo_sim_operation {
#define HOSTOP(id, module, name, signature, params, bugs, spec_only)
#define SIMOP(id, module, name, spec_only) MO_SIM_##id,
#include "base/hostops.def"
    MO_SIM_COUNT
};

#endif
