#include "base/capabilities.h"

#include <string.h>

const char *const mo_capability_names[] = {"Args",      "Env",   "Console", "Files",
                                           "ReadFiles", "Clock", "Random",  "Simulation"};

bool mo_capability_named(const char *name) {
    for (unsigned i = 0; i < MO_CAPABILITY_COUNT; i++)
        if (strcmp(name, mo_capability_names[i]) == 0)
            return true;
    return false;
}
