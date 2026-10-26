/* Compiler service metadata, separate from the Core language and wire format.
 * Function indices refer to the accompanying Core function table. All storage
 * belongs to the lowering arena; nothing borrows the checked source program. */
#ifndef MO_SERVICE_H
#define MO_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

struct mo_service_tool {
    const char *name;
    uint32_t function;
    bool read_only;
};

struct mo_service_timer {
    uint32_t function;
    uint32_t interval; /* ordinary () : Duration function */
};

struct mo_service {
    uint32_t start; /* ordinary () : State function; its module owns the service */
    const char *token_env;
    uint32_t tool_count;
    const struct mo_service_tool *tools;
    uint32_t timer_count;
    const struct mo_service_timer *timers;
};

struct mo_services {
    uint32_t count;
    const struct mo_service *items;
};

#endif
