/* Generated fake capabilities for promise inputs (architecture 10.1: "Capabilities are fresh fakes
 * with generated contents"). They plug into mo_inputs_init as exact-type factories. */
#ifndef MO_FAKES_H
#define MO_FAKES_H

#include "host/host.h"
#include "tools/inputs.h"

/* How to rebuild one generated fake: it stands for the request it was made from, so a copy starts
 * from the same contents however much the original has been written to since. */
struct mo_fake_recipe {
    const void *object;
    uint32_t type;
    struct mo_input_request request;
};

struct mo_fakes;
struct mo_fake_context {
    struct mo_fakes *fakes;
    uint32_t type;
};

struct mo_fakes {
    struct mo_host *host;
    struct mo_input_factory factories[MO_CAP_RANDOM + 1];
    struct mo_fake_context contexts[MO_CAP_RANDOM + 1];
    uint32_t factory_count;
    /* Recipes of the tuple being built, and of the retained best tuple that shrinking copies. */
    struct mo_fake_recipe pending[MO_CLI_FAKE_RECIPES], kept[MO_CLI_FAKE_RECIPES];
    uint32_t pending_count, kept_count;
};

/* One factory per capability type in the table. They stay valid until the struct moves. */
void mo_fakes_init(struct mo_fakes *fakes, const struct mo_types *types);
static inline MO_MUST_USE const struct mo_input_factory *
mo_fakes_factories(const struct mo_fakes *fakes) {
    return fakes->factories;
}
static inline MO_MUST_USE uint32_t mo_fakes_count(const struct mo_fakes *fakes) {
    return fakes->factory_count;
}
/* Before generating or copying inputs into a process: its host makes the fakes. The tuple built
 * next starts a new pending set. */
void mo_fakes_bind(struct mo_fakes *fakes, struct mo_host *host);
/* The tuple just built is the one shrinking copies from now on. */
void mo_fakes_keep(struct mo_fakes *fakes);

#endif
