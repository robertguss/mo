#ifndef MO_SERVICE_INTERNAL_H
#define MO_SERVICE_INTERNAL_H

#include "vm/internal.h"

struct service_message {
    struct mo_cell arguments[MO_LIMIT_BYTECODE_PARAMETERS - 2];
    int64_t arrival;
    uint64_t id;
    size_t argument_bytes;
    bool argument_bytes_available;
    uint32_t handler;
    bool timer;
};

struct service_timer {
    mo_u128 next;
    uint64_t interval;
    bool waiting;
};

struct mo_service_loop {
    struct mo_arena arena;
    struct mo_process *process;
    const struct mo_bytecode_service *descriptor;
    struct mo_service_hooks hooks;
    struct mo_cell state;
    struct service_message mailbox[MO_LIMIT_SERVICE_MAILBOX];
    struct service_timer *timers;
    uint64_t failures[MO_LIMIT_SERVICE_FAILURES];
    uint64_t restarts[MO_LIMIT_SERVICE_RESTARTS];
    uint64_t last_clock;
    struct mo_store *store;
    uint64_t sequence, image_key, restore_key, snapshot_sequence, encoded_sequence;
    struct mo_buffer current_encoding;
    struct mo_service_message_summary log[MO_LIMIT_SERVICE_SNAPSHOT_MESSAGES];
    uint32_t log_count;
    struct mo_service_event_summary events[MO_LIMIT_SERVICE_EVENTS];
    uint64_t event_number;
    uint32_t event_head, event_count;
    uint32_t head, count, failure_count, restart_count;
    uint64_t capture_id;
    const char *capture_refusal;
    bool capture_unsafe;
    bool running;
    /* Live upgrades applied in this process, and a prepared replacement's deferred answers. */
    uint64_t epoch;
    uint64_t removed[MO_LIMIT_SERVICE_MAILBOX];
    uint32_t removed_count, dropped;
};

struct mo_service_time mo_service_clock(struct mo_service_loop *s);
struct mo_bytecode_string mo_service_short_name(struct mo_bytecode_string name);
struct mo_bytecode_string mo_service_handler_name(const struct mo_service_loop *s,
                                                  uint32_t function);
void mo_service_record_event(struct mo_service_loop *s, enum mo_service_event_kind kind,
                             enum mo_service_status reason, struct mo_service_time time,
                             const struct mo_defect *defect, uint32_t handler, uint32_t restart,
                             const struct mo_service_upgrade *upgrade);
void mo_service_finish_call(struct mo_process *p);
enum mo_service_status mo_service_invoke(struct mo_process *p, uint32_t function,
                                         const struct mo_cell *arguments, uint32_t count,
                                         struct mo_cell *out, struct mo_defect *defect);
struct service_message *mo_service_append(struct mo_service_loop *s);
void mo_service_capture_failure(struct mo_service_loop *s, const struct service_message *m,
                                const struct mo_service_call *call);

#endif
