#include "base/intent-list.h"
#include "tools/protocol-server.h"
#include "tools/upgrade.h"

MO_MUST_USE enum door_outcome mo_door_bypass_decide(struct mo_protocol_server *s,
                                                    struct protocol_connection *caller);

enum door_outcome mo_door_bypass_decide(struct mo_protocol_server *s,
                                        struct protocol_connection *caller) {
    struct protocol_upgrade *u = &s->upgrade;
    if (u->yes)
        return mo_protocol_door_decide(s, caller);
    struct mo_arena arena;
    mo_arena_init(&arena, MO_DOOR_BYTES);
    struct mo_door_view view;
    mo_buffer_init(&view.text, MO_DOOR_DIFFERENCE_BYTES);
    struct mo_store_review r = {0};
    struct mo_intent_list items;
    const struct mo_store_program *candidate = mo_live_program(u->live);
    bool read = mo_store_review_read(s->store, &arena, &r) == MO_STORE_OK && r.reviewed &&
                mo_intent_list_read(candidate->items, candidate->items_size, &items) &&
                mo_door_difference(&arena, &r, &items, &view);
    u->intent = r.intent;
    u->would = read && view.keeps && !u->has_migration;
    u->approval_version = r.version;
    mo_buffer_free(&view.text);
    mo_arena_free(&arena);
    if (!read)
        return mo_protocol_door_decide(s, caller);
    u->decided = true;
    return DOOR_PROCEED;
}
