#include "base/stack.h"
#include "service_compile.h"
#include "test.h"
#include "vm/vm.h"

#include <errno.h>

static unsigned allocation_countdown;
static bool allocation_failed;
MO_MUST_USE void *mo_service_fault_calloc(size_t count, size_t size);
MO_MUST_USE void *mo_service_fault_realloc(void *pointer, size_t size);
MO_MUST_USE void *mo_service_fault_allocate(struct mo_arena *arena, size_t size, size_t alignment);
MO_MUST_USE void *mo_service_fault_array(struct mo_arena *arena, size_t count, size_t size,
                                         size_t alignment);

static bool refuse_allocation(void) {
    if (allocation_countdown != 0 && --allocation_countdown == 0) {
        allocation_failed = true;
        errno = ENOMEM;
        return true;
    }
    return false;
}

void *mo_service_fault_calloc(size_t count, size_t size) {
    return refuse_allocation() ? NULL : calloc(count, size);
}

void *mo_service_fault_realloc(void *pointer, size_t size) {
    return refuse_allocation() ? NULL : realloc(pointer, size);
}

void *mo_service_fault_allocate(struct mo_arena *arena, size_t size, size_t alignment) {
    return refuse_allocation() ? NULL : mo_arena_allocate(arena, size, alignment);
}

void *mo_service_fault_array(struct mo_arena *arena, size_t count, size_t size, size_t alignment) {
    return refuse_allocation() ? NULL : mo_arena_allocate_array(arena, count, size, alignment);
}

struct observation {
    struct mo_service_time time;
    struct mo_service_time after;
    unsigned clock_calls, advance_on;
    struct mo_heap *heap;
    unsigned replies, failures, restarts, stops;
    enum mo_service_status status;
    bool updated, timer;
    uint32_t handler;
    struct mo_bytecode_string failed;
    uint64_t id;
    int64_t value;
    char text[256], reason[128], events[64];
    unsigned event_count;
    const char *expected_frame;
};

static struct mo_service_time clock_now(void *context) {
    struct observation *o = context;
    if (++o->clock_calls == o->advance_on)
        o->time = o->after;
    return o->time;
}

static void inspect_defect(const struct observation *o, const struct mo_defect *defect) {
    if (o->expected_frame == NULL)
        return;
    CHECK(defect != NULL && defect->frames != NULL);
    if (defect == NULL || defect->frames == NULL)
        return;
    struct mo_buffer report;
    mo_buffer_init(&report, 16384);
    MO_ASSERT(mo_render_defect(defect->process, defect, &report) &&
              mo_buffer_append_byte(&report, 0));
    CHECK(strstr((const char *)report.bytes, o->expected_frame) != NULL);
    if (strcmp(defect->reason, "stop") == 0) {
        size_t length;
        CHECK(defect->has_detail);
        const uint8_t *bytes = mo_value_data(defect->detail, &length);
        CHECK(length == 6 && memcmp(bytes, "broken", 6) == 0);
    }
    mo_buffer_free(&report);
}

static void reply(const struct mo_service_response *r, void *context) {
    struct observation *o = context;
    allocation_countdown = 0;
    inspect_defect(o, r->defect);
    o->replies++;
    o->status = r->status;
    o->updated = r->updated;
    o->timer = r->timer;
    o->handler = r->handler;
    o->id = r->id;
    if (r->status == MO_SERVICE_OK && r->timer)
        o->value = mo_cell_get_int(r->value);
    o->text[0] = '\0';
    if (r->status == MO_SERVICE_OK) {
        struct mo_buffer text;
        mo_buffer_init(&text, 4096);
        CHECK(mo_value_print(o->heap, r->type, r->value, &text));
        CHECK(text.length < sizeof o->text);
        if (text.length < sizeof o->text) {
            memcpy(o->text, text.bytes, text.length);
            o->text[text.length] = '\0';
        }
        mo_buffer_free(&text);
    }
    if (r->defect != NULL)
        CHECK(snprintf(o->reason, sizeof o->reason, "%s", r->defect->reason) > 0);
}

static void event(const struct mo_service_event *e, void *context) {
    struct observation *o = context;
    allocation_countdown = 0;
    if (e->kind == MO_SERVICE_FAILURE) {
        inspect_defect(o, e->defect);
        o->failed = e->handler;
    }
    CHECK(e->reason != MO_SERVICE_OK);
    MO_ASSERT(o->event_count + 1 < sizeof o->events);
    o->events[o->event_count++] = e->kind == MO_SERVICE_FAILURE   ? 'F'
                                  : e->kind == MO_SERVICE_RESTART ? 'R'
                                                                  : 'S';
    o->events[o->event_count] = '\0';
    switch (e->kind) {
    case MO_SERVICE_FAILURE:
        o->failures++;
        break;
    case MO_SERVICE_RESTART:
        o->restarts++;
        break;
    case MO_SERVICE_STOP:
        o->stops++;
        break;
    }
}

static struct mo_service_loop *start(struct mo_image *image, struct observation *o) {
    struct mo_service_hooks hooks = {clock_now, reply, event, o};
    struct mo_service_loop *s = NULL;
    MO_ASSERT(mo_service_new(image, 0, &hooks, &s) == MO_SERVICE_OK && s != NULL);
    o->heap = mo_service_heap(s);
    return s;
}

static void basic(void) {
    struct mo_image *image = compile(
        "module Main\nservice\nstate Int = 7\ntools add, reject, read\n"
        "token from env \"TOKEN\"\nend\n"
        "fn add(state: Int, _now: Time, n: Int) : (Int, Int)\n(state + n, n)\nend\n"
        "fn reject(_state: Int, _now: Time) : Result((Int, Int), Text)\nError(\"no\")\nend\n"
        "fn read(state: Int, _now: Time) : List(Int)\n[state]\nend\n");
    struct observation o = {0};
    struct mo_service_loop *s = start(image, &o);
    CHECK(!mo_service_step(s));
    struct mo_cell n = mo_cell_int(11);
    CHECK(mo_service_send(s, 0, &n, 1, 42) == MO_SERVICE_OK);
    CHECK(mo_service_pending(s) == 1 && mo_cell_get_int(mo_service_state(s)) == 7);
    CHECK(mo_service_step(s));
    CHECK(o.replies == 1 && o.id == 42 && o.status == MO_SERVICE_OK && o.updated);
    CHECK(strcmp(o.text, "(18, 11)") == 0);
    CHECK(mo_cell_get_int(mo_service_state(s)) == 18);
    for (unsigned i = 0; i < 20; i++) {
        CHECK(mo_service_send(s, 1, NULL, 0, i) == MO_SERVICE_OK);
        CHECK(mo_service_step(s));
        CHECK(o.status == MO_SERVICE_OK && !o.updated);
        CHECK(strcmp(o.text, "Error(\"no\")") == 0);
    }
    CHECK(o.failures == 0 && o.restarts == 0 && mo_service_running(s));
    CHECK(mo_service_send(s, 2, NULL, 0, 99) == MO_SERVICE_OK);
    CHECK(mo_service_step(s));
    CHECK(!o.updated && mo_cell_get_int(mo_service_state(s)) == 18);
    CHECK(mo_heap_objects(mo_service_heap(s)) == 0);
    mo_service_free(s);
    mo_image_free(image);
}

static const char *failure_source =
    "module Main\nservice\nstate List(Int) = [3, 8]\n"
    "tools add, broken, overflow, spin, memory, arrival\ntoken from env \"TOKEN\"\nend\n"
    "fn add(state: List(Int), _now: Time, n: Int) : Result((List(Int), Int), Text)\n"
    "Ok((state.append(n), n))\nend\n"
    "fn broken(state: List(Int), _now: Time) : (List(Int), Int)\n"
    "after = state.append(91)\n(after, stop \"broken\")\nend\n"
    "fn overflow(state: List(Int), _now: Time, n: Int) : (List(Int), Int)\n"
    "after = state.append(92)\n(after, n + 1)\nend\n"
    "fn spin(state: List(Int), now: Time) : (List(Int), Int)\nspin(state, now)\nend\n"
    "fn memory(_state: List(Int), _now: Time, n: Int) : Int\n\"x\".repeat(n).byte_count\nend\n"
    "fn arrival(_state: List(Int), now: Time) : Time\nnow\nend\n";

static void state_list(struct mo_service_loop *s, uint32_t count, int64_t last) {
    struct mo_cell state = mo_service_state(s), cell;
    CHECK(mo_list_size(state) == count);
    MO_ASSERT(mo_list_get(state, count - 1, &cell));
    CHECK(mo_cell_get_int(cell) == last);
    MO_ASSERT(mo_list_get(state, 0, &cell));
    CHECK(mo_cell_get_int(cell) == 3);
}

static void failures(void) {
    struct mo_image *image = compile(failure_source);
    struct observation o = {0};
    struct mo_service_loop *s = start(image, &o);
    struct mo_cell n = mo_cell_int(17);
    CHECK(mo_service_send(s, 0, &n, 1, 1) == MO_SERVICE_OK);
    CHECK(mo_service_step(s));
    CHECK(strcmp(o.text, "Ok(([3, 8, 17], 17))") == 0 && o.updated);
    state_list(s, 3, 17);
    size_t used = mo_heap_used(o.heap), objects = mo_heap_objects(o.heap);
    for (uint32_t tool = 1; tool <= 4; tool++) {
        o.expected_frame = tool == 1 ? "Main.broken" : tool == 2 ? "Main.overflow" : NULL;
        n = mo_cell_int(tool == 2 ? INT64_MAX : MO_LIMIT_SERVICE_MESSAGE_MEMORY);
        bool argument = tool == 2 || tool == 4;
        CHECK(mo_service_send(s, tool, argument ? &n : NULL, argument ? 1 : 0, tool) ==
              MO_SERVICE_OK);
        CHECK(mo_service_step(s));
        CHECK(o.status == (tool == 3   ? MO_SERVICE_WORK
                           : tool == 4 ? MO_SERVICE_MEMORY
                                       : MO_SERVICE_DEFECT));
        CHECK(!o.updated && o.failures == tool && o.restarts == 0);
        if (tool == 1)
            CHECK(strcmp(o.reason, "stop") == 0);
        if (tool == 2)
            CHECK(strcmp(o.reason, "overflow: Int") == 0);
        state_list(s, 3, 17);
        CHECK(mo_heap_used(o.heap) == used && mo_heap_objects(o.heap) == objects);
    }
    n = mo_cell_int(19);
    CHECK(mo_service_send(s, 0, &n, 1, 8) == MO_SERVICE_OK);
    CHECK(mo_service_step(s));
    state_list(s, 4, 19);
    CHECK(mo_service_send(s, 1, NULL, 0, 9) == MO_SERVICE_OK);
    CHECK(mo_service_step(s));
    CHECK(o.restarts == 1 && mo_service_running(s));
    state_list(s, 4, 19);
    mo_service_free(s);
    mo_image_free(image);
}

static void mailbox(void) {
    struct mo_image *image = compile(failure_source);
    struct observation o = {.time = {.now = 123456}};
    struct mo_service_loop *s = start(image, &o);
    for (uint32_t i = 0; i < MO_LIMIT_SERVICE_MAILBOX; i++)
        CHECK(mo_service_send(s, 5, NULL, 0, i + 10) == MO_SERVICE_OK);
    CHECK(mo_service_pending(s) == MO_LIMIT_SERVICE_MAILBOX);
    unsigned calls = o.clock_calls;
    CHECK(mo_service_send(s, 5, NULL, 0, 999) == MO_SERVICE_BUSY);
    CHECK(o.clock_calls == calls && o.replies == 0 && o.failures == 0);
    o.time.now = 999999;
    for (uint32_t i = 0; i < MO_LIMIT_SERVICE_MAILBOX; i++) {
        CHECK(mo_service_step(s));
        CHECK(o.id == i + 10 && strcmp(o.text, "1970-01-01T00:00:00.123456Z") == 0);
    }
    CHECK(!mo_service_step(s) && mo_service_pending(s) == 0);
    mo_service_free(s);
    mo_image_free(image);
}

static void queued_arguments(void) {
    struct mo_image *image = compile(
        "module Main\nservice\nstate Int = 7\ntools echo, broken\n"
        "token from env \"TOKEN\"\nend\n"
        "fn echo(state: Int, _now: Time, text: Text) : (Int, Text)\n(state + 1, text)\nend\n"
        "fn broken(_state: Int, _now: Time, _text: Text) : Bool\nstop \"broken\"\nend\n");
    struct observation o = {0};
    struct mo_service_loop *s = start(image, &o);
    uint32_t type = mo_image_bytecode(image)->services[0].tools[0].argument_types[0];
    struct mo_cell text;
    MO_ASSERT(mo_value_text(o.heap, "kept", 4, &text));
    for (uint32_t i = 0; i < MO_LIMIT_SERVICE_MAILBOX; i++)
        CHECK(mo_service_send(s, i % 2, &text, 1, i) == MO_SERVICE_OK);
    mo_value_drop(o.heap, type, text);
    MO_ASSERT(mo_value_text(o.heap, "refused", 7, &text));
    CHECK(mo_service_send(s, 0, &text, 1, 999) == MO_SERVICE_BUSY);
    mo_value_drop(o.heap, type, text);
    for (uint32_t i = 0; i < 30; i++) {
        CHECK(mo_service_step(s));
        if (i % 2 == 0) {
            char expected[64];
            CHECK(snprintf(expected, sizeof expected, "(%u, \"kept\")", 8 + i / 2) > 0);
            CHECK(o.status == MO_SERVICE_OK && strcmp(o.text, expected) == 0);
        } else if (i < 29)
            CHECK(o.status == MO_SERVICE_DEFECT);
    }
    CHECK(!mo_service_running(s) && o.stops == 1 && o.replies == MO_LIMIT_SERVICE_MAILBOX);
    CHECK(o.id == MO_LIMIT_SERVICE_MAILBOX - 1 && o.status == MO_SERVICE_DOWN);
    CHECK(mo_cell_get_int(mo_service_state(s)) == 22 && mo_heap_objects(o.heap) == 0);
    mo_service_free(s);
    mo_image_free(image);
}

static void fail_messages(struct mo_service_loop *s, unsigned count) {
    for (unsigned i = 0; i < count; i++) {
        CHECK(mo_service_send(s, 1, NULL, 0, i) == MO_SERVICE_OK);
        CHECK(mo_service_step(s));
    }
}

static void supervisor(void) {
    struct mo_image *image = compile(failure_source);
    for (unsigned outside = 0; outside < 2; outside++) {
        struct observation o = {0};
        struct mo_service_loop *s = start(image, &o);
        fail_messages(s, 4);
        CHECK(o.restarts == 0);
        o.time.elapsed = MO_LIMIT_SERVICE_FAILURE_WINDOW_US - 1 + outside;
        fail_messages(s, 1);
        CHECK(o.restarts == (outside == 0 ? 1u : 0u));
        mo_service_free(s);
    }
    for (unsigned outside = 0; outside < 2; outside++) {
        struct observation o = {0};
        struct mo_service_loop *s = start(image, &o);
        fail_messages(s, 5);
        o.time.elapsed = 300000000;
        fail_messages(s, 5);
        CHECK(o.restarts == 2 && o.stops == 0);
        o.time.elapsed = MO_LIMIT_SERVICE_RESTART_WINDOW_US - 1 + outside;
        fail_messages(s, 4);
        CHECK(mo_service_send(s, 1, NULL, 0, 77) == MO_SERVICE_OK);
        struct mo_cell n = mo_cell_int(27);
        CHECK(mo_service_send(s, 0, &n, 1, 88) == MO_SERVICE_OK);
        CHECK(mo_service_step(s));
        CHECK(o.restarts == 3 && o.failures == 15);
        CHECK(mo_service_running(s) == (outside != 0));
        if (outside == 0) {
            CHECK(o.stops == 1 && o.status == MO_SERVICE_DOWN && o.id == 88);
            CHECK(strcmp(o.events, "FFFFFRFFFFFRFFFFFRS") == 0);
            CHECK(mo_service_pending(s) == 0);
            o.time.elapsed += 1000000000;
            CHECK(!mo_service_step(s));
            CHECK(mo_service_send(s, 0, &n, 1, 90) == MO_SERVICE_DOWN);
            state_list(s, 2, 8);
        } else {
            CHECK(o.stops == 0 && mo_service_pending(s) == 1);
            CHECK(mo_service_step(s));
            state_list(s, 3, 27);
        }
        mo_service_free(s);
    }
    mo_image_free(image);
}

static void timers(void) {
    struct mo_image *image =
        compile("module Main\nservice\nstate Int = 0\ntools read, broken\n"
                "every 1.second z_tick\nevery 1.second a_tick\ntoken from env \"TOKEN\"\nend\n"
                "fn read(state: Int, _now: Time) : List(Int)\n[state]\nend\n"
                "fn broken(_state: Int, _now: Time) : Bool\nstop \"broken\"\nend\n"
                "fn z_tick(state: Int, _now: Time) : Int\nstate * 10 + 1\nend\n"
                "fn a_tick(state: Int, _now: Time) : Int\nstate * 10 + 2\nend\n");
    struct observation o = {0};
    struct mo_service_loop *s = start(image, &o);
    CHECK(mo_service_pending(s) == 2 && o.replies == 0);
    CHECK(mo_service_step(s) && o.handler == 0 && o.value == 1);
    CHECK(mo_service_step(s) && o.handler == 1 && o.value == 12);
    o.time.elapsed = 999999;
    CHECK(!mo_service_step(s));
    CHECK(mo_service_send(s, 0, NULL, 0, 72) == MO_SERVICE_OK);
    /* The second sample in step is after the handler. It observes ten periods
     * that elapsed during the call, without sleeping or running another thread. */
    o.advance_on = o.clock_calls + 2;
    o.after = (struct mo_service_time){.elapsed = 10000000, .now = 10000000};
    CHECK(mo_service_step(s) && !o.timer);
    CHECK(mo_service_pending(s) == 2);
    CHECK(mo_service_step(s) && o.handler == 0 && o.value == 121);
    CHECK(mo_service_step(s) && o.handler == 1 && o.value == 1212);
    CHECK(!mo_service_step(s));
    o.time.elapsed = 11000000;
    CHECK(mo_service_step(s) && o.handler == 0);
    CHECK(mo_service_step(s) && o.handler == 1);
    CHECK(!mo_service_step(s));
    mo_service_free(s);
    o = (struct observation){.time = {.elapsed = 1000000000}};
    s = start(image, &o);
    CHECK(mo_service_pending(s) == 2);
    CHECK(mo_service_step(s) && o.value == 1);
    CHECK(mo_service_step(s) && o.value == 12);
    CHECK(!mo_service_step(s));
    mo_service_free(s);

    o = (struct observation){0};
    s = start(image, &o);
    CHECK(mo_service_step(s) && mo_service_step(s));
    for (uint32_t i = 0; i < MO_LIMIT_SERVICE_MAILBOX; i++)
        CHECK(mo_service_send(s, 0, NULL, 0, i) == MO_SERVICE_OK);
    o.time.elapsed = 1000000;
    CHECK(mo_service_step(s) && mo_service_pending(s) == MO_LIMIT_SERVICE_MAILBOX);
    CHECK(mo_service_step(s) && mo_service_pending(s) == MO_LIMIT_SERVICE_MAILBOX);
    o.time.elapsed = 10000000;
    for (uint32_t i = 2; i < MO_LIMIT_SERVICE_MAILBOX; i++)
        CHECK(mo_service_step(s) && !o.timer && o.id == i);
    CHECK(mo_service_pending(s) == 2);
    CHECK(mo_service_step(s) && o.handler == 0 && o.value == 121);
    CHECK(mo_service_step(s) && o.handler == 1 && o.value == 1212);
    CHECK(!mo_service_step(s));
    mo_service_free(s);

    o = (struct observation){0};
    s = start(image, &o);
    CHECK(mo_service_step(s) && mo_service_step(s));
    fail_messages(s, 4);
    CHECK(mo_service_send(s, 1, NULL, 0, 7) == MO_SERVICE_OK);
    CHECK(mo_service_send(s, 0, NULL, 0, 8) == MO_SERVICE_OK);
    o.time.elapsed = 1000000;
    CHECK(mo_service_step(s) && o.restarts == 1);
    CHECK(mo_service_pending(s) == 3 && mo_cell_get_int(mo_service_state(s)) == 12);
    CHECK(mo_service_step(s) && !o.timer && o.id == 8);
    CHECK(mo_service_step(s) && o.value == 121);
    CHECK(mo_service_step(s) && o.value == 1212);
    CHECK(!mo_service_step(s));
    mo_service_free(s);

    o = (struct observation){.time = {.elapsed = UINT64_MAX - 500000}};
    s = start(image, &o);
    CHECK(mo_service_step(s) && mo_service_step(s));
    o.time.elapsed = UINT64_MAX;
    CHECK(!mo_service_step(s));
    mo_service_free(s);
    mo_image_free(image);
}

static struct mo_service_time advancing_clock(void *context) {
    struct observation *o = context;
    o->time.elapsed += 1000000;
    o->time.now += 1000000;
    return o->time;
}

static void many_timers(void) {
    char source[16384];
    int length =
        snprintf(source, sizeof source, "module Main\nservice\nstate Int = 0\ntools read\n");
    MO_ASSERT(length > 0);
    size_t used = (size_t)length;
    for (unsigned i = 0; i <= MO_LIMIT_SERVICE_MAILBOX; i++) {
        length = snprintf(source + used, sizeof source - used, "every 1.second tick_%u\n", i);
        MO_ASSERT(length > 0 && (size_t)length < sizeof source - used);
        used += (size_t)length;
    }
    length = snprintf(source + used, sizeof source - used,
                      "token from env \"TOKEN\"\nend\n"
                      "fn read(state: Int, _now: Time) : List(Int)\n[state]\nend\n");
    MO_ASSERT(length > 0 && (size_t)length < sizeof source - used);
    used += (size_t)length;
    for (unsigned i = 0; i <= MO_LIMIT_SERVICE_MAILBOX; i++) {
        length = snprintf(source + used, sizeof source - used,
                          "fn tick_%u(state: Int, _now: Time) : Int\nstate + 1\nend\n", i);
        MO_ASSERT(length > 0 && (size_t)length < sizeof source - used);
        used += (size_t)length;
    }
    struct mo_image *image = compile(source);
    for (unsigned moving = 0; moving < 2; moving++) {
        struct observation o = {0};
        struct mo_service_hooks hooks = {moving ? advancing_clock : clock_now, reply, event, &o};
        struct mo_service_loop *s = NULL;
        MO_ASSERT(mo_service_new(image, 0, &hooks, &s) == MO_SERVICE_OK);
        o.heap = mo_service_heap(s);
        for (unsigned i = 0; i <= MO_LIMIT_SERVICE_MAILBOX; i++) {
            CHECK(mo_service_step(s));
            CHECK(o.handler == i && o.value == (int64_t)i + 1);
        }
        if (!moving)
            CHECK(!mo_service_step(s));
        mo_service_free(s);
    }
    mo_image_free(image);
}

static void interval_values(void) {
    const char *expressions[] = {"0.seconds", "(0 - 1).seconds", "stop \"interval\"", "interval()",
                                 "1.millisecond"};
    const enum mo_service_status expected[] = {MO_SERVICE_INTERVAL, MO_SERVICE_INTERVAL,
                                               MO_SERVICE_DEFECT, MO_SERVICE_WORK, MO_SERVICE_OK};
    for (unsigned trial = 0; trial < 5; trial++) {
        char source[1024];
        int length =
            snprintf(source, sizeof source,
                     "module Main\nexpose interval\nservice\nstate Int = 0\ntools read\n"
                     "every 1.second first\nevery 1.second second\ntoken from env \"TOKEN\"\nend\n"
                     "fn read(state: Int, _now: Time) : List(Int)\n[state]\nend\n"
                     "fn first(state: Int, _now: Time) : Int\nstate + 1\nend\n"
                     "fn second(state: Int, _now: Time) : Int\nstate + 2\nend\n"
                     "fn interval() : Duration\n%s\nend\n",
                     expressions[trial]);
        MO_ASSERT(length > 0 && (size_t)length < sizeof source);
        struct mo_image *original = compile(source);
        const struct mo_bytecode *file = mo_image_bytecode(original);
        uint32_t interval = MO_NO_INDEX;
        for (uint32_t i = 0; i < file->function_count; i++) {
            const struct mo_bytecode_string *name = &file->strings[file->functions[i].name];
            if (name->length == 13 && memcmp(name->bytes, "Main.interval", 13) == 0)
                interval = i;
        }
        MO_ASSERT(interval != MO_NO_INDEX);
        struct mo_bytecode changed = *file;
        struct mo_bytecode_service descriptor = file->services[0];
        struct mo_bytecode_service_timer timers[2] = {descriptor.timers[0], descriptor.timers[1]};
        descriptor.timers = timers;
        changed.services = &descriptor;
        timers[1].interval = interval;
        struct mo_buffer wire;
        struct mo_write_error write_error;
        mo_buffer_init(&wire, MO_LIMIT_BYTECODE_SIZE);
        MO_ASSERT(mo_bytecode_write(&changed, &wire, &write_error));
        struct mo_load_error load_error;
        struct mo_image *image = mo_load(wire.bytes, wire.length, &load_error);
        MO_ASSERT(image != NULL);
        struct mo_limits limits = mo_limits_default();
        struct mo_defect defect;
        MO_ASSERT(mo_image_init(image, &limits, &defect) == MO_RETURNED);
        struct observation o = {0};
        struct mo_service_hooks hooks = {clock_now, reply, event, &o};
        struct mo_service_loop *s = NULL;
        CHECK(mo_service_new(image, 0, &hooks, &s) == expected[trial]);
        CHECK(expected[trial] == MO_SERVICE_OK || named(o.failed, "second"));
        CHECK(o.replies == 0 && o.restarts == 0);
        if (expected[trial] == MO_SERVICE_OK) {
            MO_ASSERT(s != NULL);
            o.heap = mo_service_heap(s);
            CHECK(mo_service_pending(s) == 2 && o.failures == 0);
            CHECK(mo_service_step(s) && o.value == 1);
            CHECK(mo_service_step(s) && o.value == 3);
        } else
            CHECK(s == NULL && o.failures == 1);
        mo_service_free(s);
        mo_image_free(image);
        mo_buffer_free(&wire);
        mo_image_free(original);
    }
}

static bool scratch_fits(struct mo_heap *heap, size_t bytes) {
    struct mo_scratch scratch;
    mo_scratch_init(&scratch, heap);
    bool fits = mo_scratch_resize(&scratch, NULL, 0, bytes) != NULL;
    mo_scratch_free(&scratch);
    return fits;
}

static void memory_boundary(void) {
    struct mo_image *image = compile(
        "module Main\nservice\nstate List(Int) = [3, 8]\ntools memory, echo\n"
        "token from env \"TOKEN\"\nend\n"
        "fn memory(_state: List(Int), _now: Time, n: Int) : Int\n\"x\".repeat(n).byte_count\nend\n"
        "fn echo(_state: List(Int), _now: Time, text: Text) : Text\ntext\nend\n");
    struct mo_limits limits = mo_limits_default();
    limits.heap_size = MO_LIMIT_SERVICE_MESSAGE_MEMORY - MO_DEFAULT_STACK_SIZE;
    struct mo_process *reference = mo_process_new(image, &limits);
    MO_ASSERT(reference != NULL);
    struct mo_heap *heap = mo_process_heap(reference);
    size_t low = 0, high = MO_LIMIT_SERVICE_MESSAGE_MEMORY;
    /* Find the allocator's exact boundary through its public API, including
     * native process and arena overhead rather than duplicating private sizes. */
    while (high - low > 1) {
        size_t middle = low + (high - low) / 2;
        if (scratch_fits(heap, middle))
            low = middle;
        else
            high = middle;
    }
    struct observation o = {0};
    struct mo_service_loop *s = start(image, &o);
    size_t used = mo_heap_used(o.heap);
    CHECK(used > mo_heap_used(heap));
    size_t available = low - (used - mo_heap_used(heap));
    CHECK(scratch_fits(o.heap, available) && !scratch_fits(o.heap, available + 1));
    struct mo_cell n = mo_cell_int(1024), text;
    MO_ASSERT(mo_value_text(o.heap, "retained", 8, &text));
    CHECK(mo_service_send(s, 0, &n, 1, 1) == MO_SERVICE_OK);
    CHECK(mo_service_send(s, 1, &text, 1, 2) == MO_SERVICE_OK);
    uint32_t type = mo_image_bytecode(image)->services[0].tools[1].argument_types[0];
    mo_value_drop(o.heap, type, text);
    size_t queued = mo_heap_used(o.heap) - used;
    CHECK(queued != 0 && !scratch_fits(o.heap, available - queued + 1));
    struct mo_scratch scratch;
    mo_scratch_init(&scratch, o.heap);
    MO_ASSERT(mo_scratch_resize(&scratch, NULL, 0, available - queued) != NULL);
    CHECK(mo_service_step(s) && o.status == MO_SERVICE_MEMORY && o.failures == 1);
    state_list(s, 2, 8);
    mo_scratch_free(&scratch);
    CHECK(mo_service_step(s) && o.status == MO_SERVICE_OK && strcmp(o.text, "retained") == 0);
    CHECK(mo_heap_used(o.heap) == used && scratch_fits(o.heap, available));
    CHECK(mo_service_send(s, 0, &n, 1, 3) == MO_SERVICE_OK && mo_service_step(s));
    CHECK(o.status == MO_SERVICE_OK && strcmp(o.text, "1024") == 0);
    CHECK(mo_heap_used(o.heap) == used);
    mo_process_free(reference);
    mo_service_free(s);
    mo_image_free(image);
}

static void work_boundary(void) {
    struct mo_image *original =
        compile("module Main\nservice\n"
                "state (Int, Int) = (7, 9)\ntools work\ntoken from env \"TOKEN\"\nend\n"
                "fn work(_state: (Int, Int), _now: Time, n: Int) : Bool\nn == 0 or n == 1\nend\n");
    const struct mo_bytecode *file = mo_image_bytecode(original);
    uint32_t target = file->services[0].tools[0].function;
    uint32_t box = file->services[0].state_type;
    uint32_t integer = file->functions[target].parameters[2].type;
    uint32_t zero = MO_NO_INDEX, one = MO_NO_INDEX, layout = MO_NO_INDEX;
    for (uint32_t i = 0; i < file->constant_count; i++)
        if (file->constants[i].type == integer) {
            if (file->constants[i].value.immediate[0] == 0)
                zero = i;
            if (file->constants[i].value.immediate[0] == 1)
                one = i;
        }
    for (uint32_t i = 0; i < file->layout_count; i++)
        if (file->layouts[i].type == box)
            layout = i;
    MO_ASSERT(zero != MO_NO_INDEX && one != MO_NO_INDEX && layout != MO_NO_INDEX);
    struct mo_arena arena;
    mo_arena_init(&arena, MO_LIMIT_BYTECODE_SIZE);
    struct mo_bytecode_function *functions = mo_arena_allocate_array(
        &arena, file->function_count, sizeof *functions, _Alignof(struct mo_bytecode_function));
    struct mo_bytecode_debug *debug = mo_arena_allocate_array(
        &arena, file->debug_count, sizeof *debug, _Alignof(struct mo_bytecode_debug));
    MO_ASSERT(functions != NULL && debug != NULL);
    memcpy(functions, file->functions, file->function_count * sizeof *functions);
    memcpy(debug, file->debug, file->debug_count * sizeof *debug);
    debug[target] = (struct mo_bytecode_debug){
        .text = MO_NO_INDEX, .file = MO_NO_INDEX, .subject = MO_NO_INDEX};
    struct mo_parameter parameters[3];
    memcpy(parameters, functions[target].parameters, sizeof parameters);
    parameters[0].ownership = MO_PARAMETER_OWNED;
    uint32_t registers[] = {
        box, parameters[1].type, integer, integer, integer, functions[target].result_type, box,
        box};
    functions[target].parameters = parameters;
    functions[target].register_types = registers;
    functions[target].register_count = 8;
    struct mo_bytecode changed = *file;
    changed.functions = functions;
    changed.debug = debug;
    for (unsigned trial = 0; trial < 3; trial++) {
        /* Eight setup instructions, four per decrement, then EQ/BRANCH/FREE/RET.
         * The extra CLEAR puts RET one past the limit. Another decrement instead
         * cancels with a nonempty reuse token still owned by the frame. */
        uint32_t code[] = {MO_OP_DROP,
                           0,
                           MO_OP_CONST,
                           3,
                           one,
                           MO_OP_CONST,
                           4,
                           zero,
                           MO_OP_CONSTRUCT | (2u << 16),
                           6,
                           layout,
                           3,
                           4,
                           MO_OP_RESET,
                           7,
                           6,
                           MO_OP_CLEAR,
                           6,
                           MO_OP_CLEAR,
                           6,
                           MO_OP_EQ_INT,
                           5,
                           4,
                           4,
                           MO_OP_EQ_INT,
                           5,
                           2,
                           4,
                           MO_OP_BRANCH,
                           5,
                           38,
                           32,
                           MO_OP_SUB_INT,
                           2,
                           2,
                           3,
                           MO_OP_JUMP,
                           24,
                           MO_OP_FREE_TOKEN,
                           7,
                           MO_OP_CLEAR,
                           6,
                           MO_OP_RET,
                           5};
        functions[target].code = code;
        functions[target].code_count = 44;
        if (trial != 1) {
            code[40] = MO_OP_RET;
            code[41] = 5;
            functions[target].code_count = 42;
        }
        struct mo_buffer wire;
        mo_buffer_init(&wire, MO_LIMIT_BYTECODE_SIZE);
        struct mo_write_error write_error;
        bool written = mo_bytecode_write(&changed, &wire, &write_error);
        if (!written)
            CHECK(fprintf(stderr, "boundary bytecode: %s\n", write_error.reason) > 0);
        MO_ASSERT(written);
        struct mo_load_error load_error;
        struct mo_image *image = mo_load(wire.bytes, wire.length, &load_error);
        MO_ASSERT(image != NULL);
        struct mo_limits limits = mo_limits_default();
        struct mo_defect defect;
        MO_ASSERT(mo_image_init(image, &limits, &defect) == MO_RETURNED);
        struct observation o = {0};
        struct mo_service_loop *s = start(image, &o);
        size_t used = mo_heap_used(o.heap), objects = mo_heap_objects(o.heap);
        MO_ASSERT((MO_LIMIT_SERVICE_WORK - 12) % 4 == 0);
        struct mo_cell n = mo_cell_int((MO_LIMIT_SERVICE_WORK - 12) / 4 + (trial == 2 ? 1 : 0));
        CHECK(mo_service_send(s, 0, &n, 1, 1) == MO_SERVICE_OK && mo_service_step(s));
        CHECK(o.status == (trial == 0 ? MO_SERVICE_OK : MO_SERVICE_WORK));
        CHECK(mo_heap_used(o.heap) == used && mo_heap_objects(o.heap) == objects);
        n = mo_cell_int(0);
        CHECK(mo_service_send(s, 0, &n, 1, 2) == MO_SERVICE_OK && mo_service_step(s));
        CHECK(o.status == MO_SERVICE_OK && strcmp(o.text, "true") == 0);
        CHECK(mo_heap_used(o.heap) == used && mo_heap_objects(o.heap) == objects);
        CHECK(mo_cell_get_int(mo_value_field(mo_service_state(s), 0)) == 7);
        mo_service_free(s);
        mo_image_free(image);
        mo_buffer_free(&wire);
    }
    mo_arena_free(&arena);
    mo_image_free(original);
}

static void faults(void) {
    struct mo_image *image = compile(failure_source);
    unsigned refused = 0;
    for (unsigned nth = 1; nth < 32; nth++) {
        struct observation o = {0};
        struct mo_service_hooks hooks = {clock_now, reply, event, &o};
        struct mo_service_loop *s = NULL;
        allocation_countdown = nth;
        allocation_failed = false;
        enum mo_service_status status = mo_service_new(image, 0, &hooks, &s);
        allocation_countdown = 0;
        if (!allocation_failed) {
            CHECK(status == MO_SERVICE_OK && s != NULL);
            mo_service_free(s);
            break;
        }
        if (status == MO_SERVICE_OK) {
            /* List growth retries a refused slack allocation at exact size. */
            MO_ASSERT(s != NULL);
            state_list(s, 2, 8);
            mo_service_free(s);
        } else {
            refused++;
            CHECK(status == MO_SERVICE_MEMORY && s == NULL && o.replies == 0);
        }
    }
    CHECK(refused >= 5 && refused < 31);
    refused = 0;
    for (unsigned nth = 1; nth < 32; nth++) {
        struct observation o = {0};
        struct mo_service_loop *s = start(image, &o);
        size_t used = mo_heap_used(o.heap);
        struct mo_cell n = mo_cell_int(17);
        CHECK(mo_service_send(s, 0, &n, 1, 5) == MO_SERVICE_OK);
        allocation_countdown = nth;
        allocation_failed = false;
        CHECK(mo_service_step(s));
        if (!allocation_failed) {
            CHECK(o.status == MO_SERVICE_OK);
            state_list(s, 3, 17);
            mo_service_free(s);
            break;
        }
        refused++;
        CHECK(o.status == MO_SERVICE_MEMORY && o.failures == 1 && !o.updated);
        state_list(s, 2, 8);
        CHECK(mo_heap_used(o.heap) == used);
        CHECK(mo_service_send(s, 0, &n, 1, 6) == MO_SERVICE_OK);
        CHECK(mo_service_step(s));
        state_list(s, 3, 17);
        mo_service_free(s);
    }
    CHECK(refused >= 2 && refused < 31);
    mo_image_free(image);
}

static void run(void *unused) {
    (void)unused;
    basic();
    failures();
    mailbox();
    queued_arguments();
    supervisor();
    timers();
    many_timers();
    interval_values();
    memory_boundary();
    work_boundary();
    faults();
}

int main(void) {
    CHECK(mo_with_stack(MO_SOURCE_STACK_SIZE, run, NULL) == 0);
    return mo_test_report("service runtime");
}
