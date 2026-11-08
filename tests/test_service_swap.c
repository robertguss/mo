/* A live upgrade replaces a running loop between two messages. These cases are the boundary
 * contract: what the replacement inherits, what it answers, and that refusal leaves the old loop
 * exactly as it was. The store transaction between prepare and finish belongs to the caller. */
#include "base/stack.h"
#include "service_compile.h"
#include "test.h"
#include "vm/vm.h"

#include <inttypes.h>

static const char old_source[] =
    "module Main\nservice\nstate Int = 0\ntools add, read, gone, retyped\n"
    "every 40.second tick\nevery 10.second lost\ntoken from env \"TOKEN\"\nend\n"
    "fn add(state: Int, _now: Time, amount: Int) : (Int, Int)\n(state + amount, state + "
    "amount)\nend\n"
    "fn read(state: Int, _now: Time) : List(Int)\n[state]\nend\n"
    "fn gone(state: Int, _now: Time) : (Int, Int)\n(state + 1000, state)\nend\n"
    "fn retyped(state: Int, _now: Time, value: Int) : (Int, Int)\n(state + value, state)\nend\n"
    "fn tick(state: Int, _now: Time) : Int\nstate + 1\nend\n"
    "fn lost(state: Int, _now: Time) : Int\nstate + 100\nend\n";

/* Tool order differs, so remapping by name is observable; add now multiplies by ten. */
static const char new_source[] =
    "module Main\nservice\nstate Int = 999\ntools read, retyped, add, double\n"
    "every 40.second tick\nevery 5.second fresh\ntoken from env \"TOKEN\"\nend\n"
    "fn add(state: Int, _now: Time, amount: Int) : (Int, Int)\n"
    "(state + 10 * amount, state + 10 * amount)\nend\n"
    "fn read(state: Int, _now: Time) : List(Int)\n[state]\nend\n"
    "fn retyped(state: Int, _now: Time, value: Text) : (Int, Text)\n(state, value)\nend\n"
    "fn double(state: Int, _now: Time) : (Int, Int)\n(state * 2, state * 2)\nend\n"
    "fn tick(state: Int, _now: Time) : Int\nstate + 1\nend\n"
    "fn fresh(state: Int, _now: Time) : Int\nstate\nend\n";

struct replies {
    struct mo_service_time time;
    const struct mo_types *types;
    unsigned count, removed, down;
    uint64_t ids[32];
    enum mo_service_status statuses[32];
    uint32_t handlers[32];
    int64_t values[32];
    bool timers[32];
    unsigned upgrades, stops;
    struct mo_service_upgrade upgrade;
};

static struct mo_service_time now(void *context) {
    return ((struct replies *)context)->time;
}

static void reply(const struct mo_service_response *r, void *context) {
    struct replies *o = context;
    MO_ASSERT(o->count < 32);
    o->ids[o->count] = r->id;
    o->statuses[o->count] = r->status;
    o->handlers[o->count] = r->handler;
    o->timers[o->count] = r->timer;
    o->values[o->count] = 0;
    if (r->status == MO_SERVICE_OK) {
        const struct mo_types *types = o->types;
        struct mo_cell value = r->value;
        if (types->items[r->type].kind == MO_TYPE_TUPLE)
            value = mo_value_field(value, 1);
        else if (types->items[r->type].kind == MO_TYPE_LIST)
            MO_ASSERT(mo_list_get(value, 0, &value));
        if (!r->timer)
            o->values[o->count] = mo_cell_get_int(value);
    }
    o->removed += r->status == MO_SERVICE_REMOVED;
    o->down += r->status == MO_SERVICE_DOWN;
    o->count++;
}

static void event(const struct mo_service_event *e, void *context) {
    struct replies *o = context;
    if (e->kind == MO_SERVICE_UPGRADE) {
        MO_ASSERT(e->upgrade != NULL);
        o->upgrade = *e->upgrade;
        o->upgrades++;
    }
    o->stops += e->kind == MO_SERVICE_STOP;
}

static struct mo_service_loop *start(struct mo_image *image, struct replies *o) {
    struct mo_service_hooks hooks = {now, reply, event, o};
    struct mo_service_loop *s = NULL;
    MO_ASSERT(mo_service_new(image, 0, &hooks, &s) == MO_SERVICE_OK && s != NULL);
    o->types = mo_image_types(image);
    return s;
}

static void encoded_int(struct mo_service_loop *s, const struct mo_image *image, int64_t value,
                        struct mo_buffer *out) {
    const struct mo_bytecode_service *d = &mo_image_bytecode(image)->services[0];
    MO_ASSERT(mo_value_encode(mo_service_heap(s), d->state_type, mo_cell_int(value), out));
}

static void send_int(struct mo_service_loop *s, uint32_t tool, int64_t value, uint64_t id) {
    struct mo_cell argument = mo_cell_int(value);
    CHECK(mo_service_send(s, tool, &argument, 1, id) == MO_SERVICE_OK);
}

/* Queued calls survive in order under the new program; removed or retyped ones are answered
 * only at finish, and the replacement handles nothing before its caller commits. */
static void readmission(void) {
    struct mo_image *old = compile(old_source), *next = compile(new_source);
    struct replies o = {.time = {.now = 1000000, .elapsed = 1000000}};
    struct mo_service_loop *s = start(old, &o);
    /* Startup queues both ticks; run them, leaving tick due at 41 s and lost at 11 s. */
    while (mo_service_step(s))
        ;
    o.time = (struct mo_service_time){.now = 2000000, .elapsed = 2000000};
    for (uint64_t i = 0; i < 3; i++)
        send_int(s, 0, (int64_t)i + 1, 10 + i);
    CHECK(mo_service_send(s, 2, NULL, 0, 20) == MO_SERVICE_OK);
    send_int(s, 3, 5, 21);
    CHECK(mo_service_send(s, 1, NULL, 0, 22) == MO_SERVICE_OK);
    unsigned before = o.count;
    int64_t state = mo_cell_get_int(mo_service_state(s));
    struct mo_service_timer_summary tick;
    CHECK(mo_service_timer_entry(s, 0, &tick) && tick.next == 41000000 && !tick.waiting);

    struct mo_buffer migrated;
    mo_buffer_init(&migrated, MO_LIMIT_SAVED_BYTES);
    encoded_int(s, old, 7, &migrated);
    struct mo_service_loop *replacement = NULL;
    CHECK(mo_service_swap_prepare(s, next, 0, migrated.bytes, migrated.length, &replacement) ==
          MO_SERVICE_SWAP_READY);
    MO_ASSERT(replacement != NULL);
    /* Preparing answers nothing and leaves the old loop untouched. */
    CHECK(o.count == before && mo_service_pending(s) == 6 &&
          mo_cell_get_int(mo_service_state(s)) == state);
    CHECK(mo_service_pending(replacement) == 4);
    CHECK(mo_cell_get_int(mo_service_state(replacement)) == 7);
    struct mo_service_message_summary first;
    CHECK(mo_service_mailbox_entry(replacement, 0, &first) && first.id == 10 && first.handler == 2);
    CHECK(mo_service_mailbox_entry(replacement, 3, &first) && first.id == 22 && first.handler == 0);

    struct mo_service_upgrade upgrade = {.accepted = true, .ordinal = 3};
    mo_service_swap_finish(s, replacement, &upgrade);
    s = replacement;
    o.types = mo_image_types(next);
    CHECK(o.count == before + 2 && o.removed == 2);
    CHECK(o.ids[before] == 20 && o.statuses[before] == MO_SERVICE_REMOVED);
    CHECK(o.ids[before + 1] == 21 && o.statuses[before + 1] == MO_SERVICE_REMOVED);
    CHECK(o.upgrades == 1 && o.upgrade.answered == 2 && o.upgrade.ordinal == 3);
    CHECK(mo_service_observe(s).epoch == 1);
    /* The new program handles the three adds in order, then the read. */
    for (unsigned i = 0; i < 4; i++)
        CHECK(mo_service_step(s));
    CHECK(o.ids[before + 2] == 10 && o.values[before + 2] == 17 && o.handlers[before + 2] == 2);
    CHECK(o.ids[before + 3] == 11 && o.values[before + 3] == 37);
    CHECK(o.ids[before + 4] == 12 && o.values[before + 4] == 67);
    CHECK(o.ids[before + 5] == 22 && o.values[before + 5] == 67 && o.handlers[before + 5] == 0);
    /* The kept timer is still due at 41 s; the new one starts now; the removed one is gone. */
    CHECK(mo_service_timer_entry(s, 0, &tick) && tick.next == 41000000 && !tick.waiting);
    struct mo_service_timer_summary fresh;
    CHECK(mo_service_timer_entry(s, 1, &fresh) && fresh.interval == 5000000);
    CHECK(!mo_service_timer_entry(s, 2, &fresh));
    struct mo_service_event_summary note;
    uint32_t events = mo_service_observe(s).events;
    CHECK(events > 0 && mo_service_event_entry(s, events - 1, &note));
    CHECK(note.kind == MO_SERVICE_UPGRADE && note.epoch == 1 && note.upgrade.answered == 2);
    mo_buffer_free(&migrated);
    mo_service_free(s);
    mo_image_free(next);
    mo_image_free(old);
}

/* A queued tick of a removed timer is dropped and counted in the upgrade event. */
static void removed_timer(void) {
    struct mo_image *old = compile(old_source), *next = compile(new_source);
    struct replies o = {.time = {.now = 1000000, .elapsed = 1000000}};
    struct mo_service_loop *s = start(old, &o);
    CHECK(mo_service_pending(s) == 2);
    struct mo_buffer migrated;
    mo_buffer_init(&migrated, MO_LIMIT_SAVED_BYTES);
    encoded_int(s, old, 0, &migrated);
    struct mo_service_loop *replacement = NULL;
    CHECK(mo_service_swap_prepare(s, next, 0, migrated.bytes, migrated.length, &replacement) ==
          MO_SERVICE_SWAP_READY);
    struct mo_service_upgrade upgrade = {.accepted = true, .ordinal = 1};
    mo_service_swap_finish(s, replacement, &upgrade);
    s = replacement;
    o.types = mo_image_types(next);
    CHECK(o.upgrade.dropped == 1 && o.upgrade.answered == 0 && o.removed == 0);
    struct mo_service_timer_summary tick;
    CHECK(mo_service_timer_entry(s, 0, &tick) && tick.waiting);
    mo_buffer_free(&migrated);
    mo_service_free(s);
    mo_image_free(next);
    mo_image_free(old);
}

/* Refusals in preparation and an abandoned replacement both leave the old loop serving. The
 * checker refuses nonpositive interval literals, so the interval refusal is a defensive guard. */
static void refusals(void) {
    struct mo_image *old = compile(old_source), *next = compile(new_source);
    struct replies o = {.time = {.now = 1000000, .elapsed = 1000000}};
    struct mo_service_loop *s = start(old, &o);
    while (mo_service_step(s))
        ;
    send_int(s, 0, 4, 30);
    struct mo_buffer migrated;
    mo_buffer_init(&migrated, MO_LIMIT_SAVED_BYTES);
    encoded_int(s, old, 7, &migrated);
    struct mo_service_loop *replacement = NULL;
    static const uint8_t garbage[] = {1, 2, 3};
    CHECK(mo_service_swap_prepare(s, next, 0, garbage, sizeof garbage, &replacement) ==
          MO_SERVICE_SWAP_STATE);
    CHECK(replacement == NULL);
    CHECK(mo_service_swap_prepare(s, next, 0, migrated.bytes, migrated.length, &replacement) ==
          MO_SERVICE_SWAP_READY);
    mo_service_swap_abandon(replacement);
    struct mo_service_upgrade refused = {.code = 1424};
    unsigned before = o.count;
    mo_service_note_upgrade(s, &refused);
    CHECK(o.upgrades == 1 && !o.upgrade.accepted && o.upgrade.code == 1424);
    CHECK(mo_service_observe(s).epoch == 0 && mo_service_pending(s) == 1);
    CHECK(mo_service_step(s) && o.count == before + 1 && o.ids[before] == 30 &&
          o.values[before] == 105);
    /* An uncertain activation stops the old program; its queue is answered as stopped. */
    send_int(s, 0, 1, 31);
    mo_service_stop(s, MO_SERVICE_STORE);
    CHECK(!mo_service_running(s) && o.stops == 1 && o.down == 1 && mo_service_pending(s) == 0);
    CHECK(mo_service_swap_prepare(s, next, 0, migrated.bytes, migrated.length, &replacement) ==
          MO_SERVICE_SWAP_STOPPED);
    mo_buffer_free(&migrated);
    mo_service_free(s);
    mo_image_free(next);
    mo_image_free(old);
}

/* A retyped argument is answered as removed even when its saved bytes would still decode. */
static void retyped_scalar(void) {
    static const char before[] =
        "module Main\nservice\nstate Int = 0\ntools scale\ntoken from env \"TOKEN\"\nend\n"
        "fn scale(state: Int, _now: Time, factor: Int) : (Int, Int)\n(state + factor, "
        "state)\nend\n";
    static const char after[] =
        "module Main\nservice\nstate Int = 0\ntools scale\ntoken from env \"TOKEN\"\nend\n"
        "fn scale(state: Int, _now: Time, factor: Float) : (Int, Float)\n(state, factor)\nend\n";
    struct mo_image *old = compile(before), *next = compile(after);
    struct replies o = {.time = {.now = 1000000, .elapsed = 1000000}};
    struct mo_service_loop *s = start(old, &o);
    send_int(s, 0, 3, 40);
    struct mo_buffer migrated;
    mo_buffer_init(&migrated, MO_LIMIT_SAVED_BYTES);
    encoded_int(s, old, 0, &migrated);
    struct mo_service_loop *replacement = NULL;
    CHECK(mo_service_swap_prepare(s, next, 0, migrated.bytes, migrated.length, &replacement) ==
          MO_SERVICE_SWAP_READY);
    CHECK(mo_service_pending(replacement) == 0);
    struct mo_service_upgrade upgrade = {.accepted = true, .ordinal = 1};
    mo_service_swap_finish(s, replacement, &upgrade);
    CHECK(o.removed == 1 && o.ids[o.count - 1] == 40 && o.upgrade.answered == 1);
    mo_buffer_free(&migrated);
    mo_service_free(replacement);
    mo_image_free(next);
    mo_image_free(old);
}

static void run(void *unused) {
    (void)unused;
    readmission();
    removed_timer();
    refusals();
    retyped_scalar();
}

int main(void) {
    MO_ASSERT(mo_with_stack(MO_SOURCE_STACK_SIZE, run, NULL) == 0);
    return mo_test_report("service_swap");
}
