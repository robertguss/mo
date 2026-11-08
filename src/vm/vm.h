/* Execution of loaded, verified images. No C recursion implements Mo calls. */
#ifndef MO_VM_H
#define MO_VM_H

#include "base/export.h"
#include "base/verdict.h"
#include "bytecode/bytecode.h"
#include "heap/heap.h"
#include "host/host.h"

enum mo_outcome { MO_RETURNED, MO_STOPPED, MO_FAILED };
struct mo_process;
struct mo_vm_span {
    uint32_t file, start, end;
};
/* Top frame first. These views and detail cells belong to the stopped process,
 * or to the image on initialization failure; they never outlive that owner. */
struct mo_defect_frame {
    uint32_t function, pc;
    struct mo_vm_span span;
    const struct mo_defect_frame *previous;
};
struct mo_defect {
    const char *reason;
    const char *limit_name;
    size_t limit;
    bool memory;
    bool has_detail;
    struct mo_cell detail;
    struct mo_vm_span span;
    const struct mo_defect_frame *frames;
    const struct mo_process *process;
};

/* Serialized, once per image after its runtime attaches. Repeated calls return
 * the original outcome; failure before attachment may be retried.
 * Failure reports remain valid until image_free. No process may use a failed
 * or uninitialized image. Initialization and process limits include metadata. */
MO_MUST_USE enum mo_outcome mo_image_init(struct mo_image *image, const struct mo_limits *limits,
                                          struct mo_defect *defect_out);
/* C harness only: one instruction budget across all initializer calls. Like
 * process_instruction_limit, exhaustion is an internal stop, not a Mo limit. */
MO_MUST_USE enum mo_outcome mo_image_init_bounded(struct mo_image *image,
                                                  const struct mo_limits *limits,
                                                  uint64_t instructions,
                                                  struct mo_defect *defect_out);
/* A module constant of an initialized image, borrowed: globals are immortal, so the cell may be
 * passed to any process's calls and duplicated in its heap. False when it is not set. */
MO_MUST_USE bool mo_image_global(const struct mo_image *image, uint32_t global,
                                 struct mo_cell *out);
MO_MUST_USE struct mo_process *mo_process_new(const struct mo_image *image,
                                              const struct mo_limits *limits);
void mo_process_free(struct mo_process *process);
void mo_process_confidential(struct mo_process *process);
MO_MUST_USE size_t mo_process_memory_used(const struct mo_process *process);
MO_MUST_USE size_t mo_process_memory_peak(const struct mo_process *process);
MO_MUST_USE struct mo_heap *mo_process_heap(struct mo_process *process);
/* C harness I/O descriptors and portable-resolution selection. The context
 * belongs to the process; descriptors are borrowed and must outlive its calls. */
MO_MUST_USE struct mo_host *mo_process_host(struct mo_process *process);
MO_MUST_USE size_t mo_process_stack_used(const struct mo_process *process);
/* Direct FUNCTION/spec calls only. INIT is invoked by image_init; closures by
 * call_closure. Owned parameters transfer on every outcome, borrowed ones do
 * not. Only RETURNED writes the owned result. Calls may repeat after RETURNED;
 * STOPPED/FAILED end this process and preserve its frames for the report.
 * C-owned borrowed arguments must outlive every frame/report reading them. */
MO_MUST_USE enum mo_outcome mo_call(struct mo_process *process, uint32_t function,
                                    const struct mo_cell *arguments, uint32_t argument_count,
                                    struct mo_cell *result_out, struct mo_defect *defect_out);
enum mo_controlled_outcome {
    MO_CONTROL_RETURNED,
    MO_CONTROL_STOPPED,
    MO_CONTROL_FAILED,
    MO_CONTROL_CANCELLED
};
/* mo_call's preconditions and ownership apply. keep_running is required,
 * bounded and non-reentrant; it and context are borrowed only during this
 * synchronous call. Poll after checking preconditions, before frame entry
 * and before every instruction, including nested/tail calls and RET.
 * False cancels before that instruction or harness exhaustion: CANCELLED is
 * terminal, writes neither output and fabricates no defect. Owned parameters
 * transfer even before frame entry; borrowed values retain their ownership.
 * Free the cancelled process normally. RETURNED alone writes result_out;
 * STOPPED/FAILED write defect_out and preserve the genuine defect.
 * The callback must not mutate/free/resume the process, heap or values.
 * The VM has no clock. Native operations, allocation/drop and blocking I/O
 * do not poll and may overshoot; initialization is not controlled. */
MO_MUST_USE enum mo_controlled_outcome
mo_call_controlled(struct mo_process *process, uint32_t function, const struct mo_cell *arguments,
                   uint32_t argument_count, bool (*keep_running)(void *context), void *context,
                   struct mo_cell *result_out, struct mo_defect *defect_out);
struct mo_main_capabilities {
    const struct mo_cell *values;
    uint32_t count;
};
/* main's parameters are capabilities. Make each declared real kind once per
 * process and retain it until teardown. args and env are borrowed List(Text)
 * and Map(Text,Text) cells from this heap, needed only if main declares them;
 * folder is a borrowed directory descriptor for Files/ReadFiles.
 * Non-NULL supplied replaces construction with verified, borrowed capabilities
 * from this heap, one per parameter in declaration order, including repeats of
 * a kind. Owned parameters are duplicated; borrowed ones must outlive stopped
 * reports. The vector need only outlive this call. No supplied value is cached. */
MO_MUST_USE enum mo_outcome mo_call_main(struct mo_process *process, uint32_t function,
                                         const struct mo_cell *args, const struct mo_cell *env,
                                         int folder, const struct mo_main_capabilities *supplied,
                                         struct mo_cell *result_out, struct mo_defect *defect_out);
/* C harness hook, not a Mo resource limit. Default is unlimited. Exhaustion
 * produces an internal stop before the next instruction (for bounded fuzzing). */
void mo_process_instruction_limit(struct mo_process *process, uint64_t instructions);
MO_MUST_USE uint64_t mo_process_instructions_remaining(const struct mo_process *process);
/* Exact image name bytes (length authoritative), file type index and borrowed
 * cell. The view itself lasts only for the callback; its contents belong to
 * the stopped process/image and never transfer ownership. */
struct mo_defect_variable {
    const struct mo_defect_frame *frame;
    const uint8_t *name;
    size_t name_length;
    uint32_t type;
    struct mo_cell value;
};
enum mo_defect_variables_result {
    MO_DEFECT_VARIABLES_COMPLETE,
    MO_DEFECT_VARIABLES_LIMIT,
    MO_DEFECT_VARIABLES_STOPPED,
    MO_DEFECT_VARIABLES_UNAVAILABLE
};
/* NULL process selects defect->process, including initialization failures.
 * Visit frames top first, names in debug-table order, after replay and live
 * state/lifetime filtering. count bounds delivered variables; work charges
 * each frame/name and reserves (registers + code words + 32) *
 * (MO_LIMIT_VERIFY_WORK_PER_WORD + 1) before each replay. This conservatively
 * bounds replay, its boundary scan and readability initialization; callback
 * printing/work needs the caller's own budget.
 * False from visit returns STOPPED; exhausted budgets return LIMIT; failed
 * replay/allocation returns UNAVAILABLE, never an empty COMPLETE. Any prefix
 * already delivered remains valid; COMPLETE with no callbacks means no names.
 * No value is duplicated. Copied cells/names/frames remain borrowed until
 * process_free (image_free for initialization), with C-owned borrowed call
 * arguments kept alive too. Callbacks may print structurally with
 * mo_value_print, never user Text customizers; they must not mutate/free the
 * process or values, resume execution or reenter this visitor. */
MO_MUST_USE enum mo_defect_variables_result mo_visit_defect_variables(
    const struct mo_process *process, const struct mo_defect *defect, uint32_t count, uint64_t work,
    bool (*visit)(const struct mo_defect_variable *variable, void *context), void *context);
/* NULL process selects defect->process, including an initialization defect.
 * Append at most MO_LIMIT_DEFECT_TEXT_SIZE bytes, reason and top frame first.
 * Long stacks elide middle frames; details truncate; variables are best-effort
 * and Text/Char are quoted. Non-Text values that exceed their print budget
 * show <value unavailable>. false means the reason/top frame could not fit or
 * native output allocation failed; any partial text is kept. Replay/printing
 * exhaustion never reads a register whose readability could not be established. */
MO_MUST_USE bool mo_render_defect(const struct mo_process *process, const struct mo_defect *defect,
                                  struct mo_buffer *out);

struct mo_service_loop;
struct mo_service_time {
    int64_t now;      /* valid Time, UTC microseconds */
    uint64_t elapsed; /* monotonic microseconds, independent of wall-clock corrections */
};
enum mo_service_status {
    MO_SERVICE_OK,
    MO_SERVICE_BUSY,
    MO_SERVICE_DOWN,
    MO_SERVICE_MEMORY,
    MO_SERVICE_WORK,
    MO_SERVICE_DEFECT,
    MO_SERVICE_INTERVAL,
    MO_SERVICE_STORE,
    MO_SERVICE_SAVED_INVALID,
    /* A queued call whose tool an upgrade removed or retyped; it had no effect. */
    MO_SERVICE_REMOVED
};
/* Stable descriptions for embedding errors; no saved bytes or secrets appear. */
MO_MUST_USE const char *mo_service_status_text(enum mo_service_status status);
enum mo_service_event_kind {
    MO_SERVICE_FAILURE,
    MO_SERVICE_RESTART,
    MO_SERVICE_STOP,
    MO_SERVICE_UPGRADE
};
/* One live upgrade outcome. Digests identify source bundles; no source bytes or credentials.
 * A refusal has a nonzero E14xx code and leaves the program as it was. On acceptance the
 * loop fills answered (queued calls the new program cannot take) and dropped (queued ticks
 * of removed timers). */
struct mo_service_upgrade {
    bool accepted;
    uint16_t code;
    uint64_t ordinal;
    uint8_t old_source[32], new_source[32];
    uint32_t answered, dropped;
};
struct mo_service_event {
    enum mo_service_event_kind kind;
    enum mo_service_status reason;
    struct mo_service_time time;
    const struct mo_defect *defect;
    uint32_t function;
    /* Bare tool or timer name; bytes NULL when none. The bytes belong to the image. */
    struct mo_bytecode_string handler;
    /* This restart's count in the window; 0 on other events. */
    uint32_t restart;
    /* First rejected log sequence during startup replay; zero when unavailable
     * or when this event is not a replay failure. Separate from capture IDs. */
    uint64_t replay_sequence;
    uint64_t record_id;
    const char *record_unavailable;
    const struct mo_service_upgrade *upgrade; /* MO_SERVICE_UPGRADE only */
};
struct mo_service_response {
    uint64_t id;
    uint32_t handler; /* tool or timer table index */
    bool timer, updated;
    enum mo_service_status status;
    uint32_t type; /* complete result type, not a schema-unwrapped reply */
    struct mo_cell value;
    const struct mo_defect *defect;
};
struct mo_service_hooks {
    struct mo_service_time (*clock)(void *context);
    void (*reply)(const struct mo_service_response *reply, void *context);
    void (*event)(const struct mo_service_event *event, void *context);
    void *context;
};
/* C embedding only. The image is initialized and outlives the service. All
 * hooks are required, synchronous, bounded and non-reentrant. Views and reply
 * values are borrowed only during their callback. Events are the embedding's
 * logging boundary. Clock elapsed never decreases; now is a valid Time.
 * No service is published and no tick is queued until all intervals evaluate
 * to positive Durations. Only OK writes out. No token/environment access. */
MO_MUST_USE enum mo_service_status mo_service_new(const struct mo_image *image, uint32_t index,
                                                  const struct mo_service_hooks *hooks,
                                                  struct mo_service_loop **out);
/* Durable construction borrows a store that outlives the service. It validates
 * and replays before publishing out or scheduling startup ticks. The initial
 * value is evaluated only for an empty store. Store failures stop the service;
 * callers must recover before submitting more messages. No successful update
 * reply precedes the durable append. No Mo-facing API is introduced. */
MO_MUST_USE enum mo_service_status mo_service_open(const struct mo_image *image, uint32_t index,
                                                   const struct mo_service_hooks *hooks,
                                                   struct mo_store *store,
                                                   struct mo_service_loop **out);
/* Checked-restart staging: restore using recorded_image, verify every replay checksum,
 * and evaluate intervals without writing snapshots or logs. The returned loop is detached
 * from the store. An empty store evaluates the start value but does not record it. */
MO_MUST_USE enum mo_service_status mo_service_recover(const struct mo_image *image, uint32_t index,
                                                      const struct mo_service_hooks *hooks,
                                                      struct mo_store *store,
                                                      uint64_t recorded_image,
                                                      struct mo_service_loop **out);
void mo_service_free(struct mo_service_loop *service);
/* Arguments must be valid typed values from this heap or the image. Submission
 * retains values only on OK; durable updates copy them through the saved codec
 * so sharing cannot hide recovery's memory cost. Refusal leaves no queued item.
 * Callers drop their own references.
 * Heap/state access is idle-only; a state view lasts until the next step/free. */
MO_MUST_USE struct mo_heap *mo_service_heap(struct mo_service_loop *service);
MO_MUST_USE struct mo_cell mo_service_state(const struct mo_service_loop *service);
MO_MUST_USE enum mo_service_status mo_service_send(struct mo_service_loop *service, uint32_t tool,
                                                   const struct mo_cell *arguments, uint32_t count,
                                                   uint64_t id);
/* Poll timers, run at most one FIFO message, then poll again. false means no
 * message ran (idle or stopped). On a supervisor stop, queued tool requests
 * receive DOWN and ticks are discarded. A stopped service never restarts
 * itself. New construction is the explicit start operation for this stage. */
MO_MUST_USE bool mo_service_step(struct mo_service_loop *service);
MO_MUST_USE uint32_t mo_service_pending(const struct mo_service_loop *service);
MO_MUST_USE bool mo_service_running(const struct mo_service_loop *service);
MO_MUST_USE bool mo_service_next_deadline(const struct mo_service_loop *service, uint64_t *out);

/* Native observations are idle-only and allocate nothing. Encoded bytes are borrowed
 * until the next step/free; copy them before running another handler. Nonpersistent
 * services report no encoding. Mailbox sizes are unavailable when admission did not
 * encode arguments; observation never imposes a new saved-codec requirement. */
struct mo_service_observation {
    uint64_t sequence, snapshot_sequence, encoded_sequence, observed_elapsed;
    uint64_t failure_times[MO_LIMIT_SERVICE_FAILURES];
    uint64_t restart_times[MO_LIMIT_SERVICE_RESTARTS];
    size_t heap_used, heap_peak, stack_capacity, stack_limit, encoded_bytes, encoded_capacity;
    uint32_t pending, failures, restarts, timers, events;
    bool running, persistent, retained_available;
    uint32_t retained_failures;
    uint64_t evicted_kinds;
    /* Live upgrades this process applied; event summaries name the epoch they ran in. */
    uint64_t epoch;
};
struct mo_service_message_summary {
    int64_t arrival;
    uint64_t id;
    size_t argument_bytes;
    uint32_t handler;
    bool timer, argument_bytes_available;
};
struct mo_service_timer_summary {
    mo_u128 next;
    uint64_t interval;
    bool waiting;
};
struct mo_service_event_summary {
    uint64_t number;
    enum mo_service_event_kind kind;
    enum mo_service_status reason;
    struct mo_service_time time;
    uint32_t function, restart;
    uint64_t record_id;
    const char *record_unavailable;
    uint64_t epoch;
    struct mo_service_upgrade upgrade; /* MO_SERVICE_UPGRADE only */
};
MO_MUST_USE struct mo_service_observation mo_service_observe(const struct mo_service_loop *service);
MO_MUST_USE bool mo_service_encoded_state(const struct mo_service_loop *service,
                                          const uint8_t **bytes, size_t *length);
MO_MUST_USE bool mo_service_mailbox_entry(const struct mo_service_loop *service, uint32_t index,
                                          struct mo_service_message_summary *out);
MO_MUST_USE bool mo_service_timer_entry(const struct mo_service_loop *service, uint32_t index,
                                        struct mo_service_timer_summary *out);
MO_MUST_USE bool mo_service_event_entry(const struct mo_service_loop *service, uint32_t index,
                                        struct mo_service_event_summary *out);

/* Live upgrade between two messages. prepare builds a replacement for a running loop from an
 * initialized image that outlives it and the validated saved state for its state type: queued
 * calls are remapped by tool name with their arguments re-decoded for the new types, queued
 * ticks and timer phases are kept by timer name, and intervals are evaluated in the new
 * process. It writes, replies and changes nothing; old stays authoritative. After the caller
 * commits the activation, finish answers queued calls the new program cannot take with
 * MO_SERVICE_REMOVED, drops ticks of removed timers, records the upgrade event and frees old;
 * the replacement then serves. abandon frees an uncommitted replacement. */
enum mo_service_swap_status {
    MO_SERVICE_SWAP_READY,
    MO_SERVICE_SWAP_MEMORY,
    MO_SERVICE_SWAP_INTERVAL,
    MO_SERVICE_SWAP_STATE,
    MO_SERVICE_SWAP_STOPPED
};
MO_MUST_USE enum mo_service_swap_status
mo_service_swap_prepare(struct mo_service_loop *old, const struct mo_image *image, uint32_t index,
                        const uint8_t *state, size_t length, struct mo_service_loop **out);
void mo_service_swap_finish(struct mo_service_loop *old, struct mo_service_loop *next,
                            const struct mo_service_upgrade *upgrade);
void mo_service_swap_abandon(struct mo_service_loop *next);
/* Record a refused upgrade in the event ring; the loop is otherwise unchanged. */
void mo_service_note_upgrade(struct mo_service_loop *service,
                             const struct mo_service_upgrade *upgrade);
/* Stop serving, as a supervisor stop does: queued calls are answered DOWN. */
void mo_service_stop(struct mo_service_loop *service, enum mo_service_status reason);

MO_MUST_USE enum mo_store_status
mo_service_failures(const struct mo_service_loop *service,
                    bool (*consume)(const struct mo_store_failure *, void *), void *context);

MO_MUST_USE enum mo_store_status
mo_service_failure_catalog(const struct mo_service_loop *service, uint64_t after,
                           bool (*consume)(const struct mo_store_failure *, void *), void *context,
                           uint32_t *omitted);

MO_MUST_USE bool mo_service_failure_export(const struct mo_service_loop *service, uint64_t id,
                                           struct mo_buffer *out);

/* Replaces *pin only once the record is found and decodes; a missing or unreadable
 * record leaves it unchanged. A failed copy after that leaves it released. */
MO_MUST_USE bool mo_service_pin_failure(const struct mo_service_loop *service, uint64_t failure,
                                        struct mo_export_pin *pin,
                                        const struct mo_export_identity *identity, uint64_t now);

/* Committed scalar metadata since the last snapshot, in sequence order. */
MO_MUST_USE bool mo_service_log_entry(const struct mo_service_loop *service, uint32_t index,
                                      struct mo_service_message_summary *out);

/* The limits a service's handler process runs under, and so the budgets a failure record
 * carries: MO_LIMIT_SERVICE_WORK instructions per call, these heap, stack and depth limits, and
 * MO_LIMIT_SAVED_BYTES for a new state's saved copy. */
MO_MUST_USE struct mo_limits mo_service_limits(void);
/* Replay runs only under this build's budgets, never under budgets read from a record. A record
 * whose budgets differ is damaged or crafted: false names the first that differs, with the
 * record's value and this build's. */
struct mo_service_budget {
    const char *name;
    uint64_t recorded, expected;
};
MO_MUST_USE bool mo_service_budgets_match(const struct mo_failure_record *record,
                                          struct mo_service_budget *differs);

/* One handler call made exactly as a service makes it, shared by the service loop, recovery and
 * offline replay. */
struct mo_service_call {
    /* OK when the handler returned and any new state was copied; otherwise MEMORY, WORK or
     * DEFECT. A handler that returned a new state whose saved copy failed is MEMORY. */
    enum mo_service_status status;
    /* The handler returned: value is its owned complete result, of type `type`. */
    bool returned;
    /* An updating tool's Ok or a timer's new state was copied: `state` owns the copy. */
    bool updated;
    /* Why the saved copy failed, or MO_SAVED_OK. */
    enum mo_saved_status saved;
    uint32_t function, type;
    struct mo_cell value, state;
    struct mo_defect defect;
};
/* Runs a tool, or a timer when `timer`, of `service` in the process's image on the state, the
 * arrival time and the tool's arguments, under MO_LIMIT_SERVICE_WORK. The state and arguments
 * are borrowed and must outlive the call's frames. A new state is copied through the saved codec
 * into `encoded`, as a store-backed service does, or shared when `encoded` is NULL. The frames
 * stay live for mo_service_context and mo_process_frames until the process is cleaned up. */
void mo_service_call_handler(struct mo_process *process, const struct mo_bytecode_service *service,
                             bool timer, uint32_t handler, struct mo_cell state, int64_t arrival,
                             const struct mo_cell *arguments, struct mo_buffer *encoded,
                             struct mo_service_call *call);
/* The signature of a failed call, as capture records it and replay compares it. Its text is the
 * VM's static reason and limit names, which outlive the process. */
MO_MUST_USE struct mo_signature mo_service_signature(const struct mo_service_call *call);
/* Renders a failed call's live frames, top first, then its top frame's variables, as capture
 * records them, into out (MO_LIMIT_FAILURE_CONTEXT_BYTES). Run it before the process is cleaned
 * up. Values print in the process's heap, so a full heap leaves some unavailable. */
MO_MUST_USE enum mo_failure_context mo_service_context(struct mo_process *process,
                                                       const struct mo_defect *defect,
                                                       struct mo_buffer *out);
/* A walk over a process's live frames, top first: those of a call stopped by a defect or
 * cancelled by its work limit, before cleanup. A cancelled call has no defect frames, so this is
 * the only way to reach them. Each frame's `previous` is NULL. */
struct mo_frame_walk {
    const struct mo_process *process;
    size_t offset;
};
MO_MUST_USE struct mo_frame_walk mo_process_frames(const struct mo_process *process);
MO_MUST_USE bool mo_process_frame_next(struct mo_frame_walk *walk, struct mo_defect_frame *out);

/* One handler run offline on a failure record's inputs, in its own fresh process under the
 * service limits, with no timers, mailbox or store. The new state of an updating tool or a timer
 * passes the saved-copy size check as it does live. */
struct mo_offline_run {
    struct mo_run_outcome outcome;
    /* A refused run never started: the process could not be made or the inputs did not decode. */
    const char *refusal;
    /* A failed run's context status; the text is in the caller's buffer. */
    enum mo_failure_context context;
};
/* The image is initialized; `service` is one of its descriptors and `handler` a tool, or a timer
 * when the record's timer flag is set, of that service (the record's own handler for replay).
 * `state` is the pre-call state's saved bytes; the record supplies the arrival time and the
 * argument bytes. A failed run's context text is appended to `context`. */
MO_MUST_USE struct mo_offline_run
mo_offline_call(const struct mo_image *image, const struct mo_bytecode_service *service,
                uint32_t handler, const uint8_t *state, size_t state_length,
                const struct mo_failure_record *record, struct mo_buffer *context);

/* A simulated service: open, send, advance, crash and restart one service as the Simulation
 * capability does, for that capability and for C callers that drive a service the same way. It
 * owns a fresh in-memory store and a fake clock that only advance moves. */
struct mo_simulation;
struct mo_simulation_failure {
    enum mo_service_status status;
    int64_t now;
    /* The start value, a timer's interval or handler, a replayed handler, or MO_NO_INDEX. */
    uint32_t function;
    const struct mo_defect *defect; /* NULL when there is none */
};
/* Borrowed for one call. failure sees each failure at open, failing timer handler and store
 * failure as it happens; its view lasts only for that call. false ends the operation there and
 * leaves later messages queued for the next one. */
struct mo_simulation_hooks {
    bool (*failure)(const struct mo_simulation_failure *failure, void *context);
    void *context;
};
enum mo_simulation_outcome {
    MO_SIMULATION_OK,
    MO_SIMULATION_STOPPED,  /* failure returned false */
    MO_SIMULATION_DOWN,     /* open failed and failure saw why; the service stays down */
    MO_SIMULATION_RUNNING,  /* open needs a crashed or stopped service; nothing changed */
    MO_SIMULATION_DURATION, /* advance needs a nonnegative duration ending by year 9999 */
    MO_SIMULATION_TICKS     /* more firings due than MO_LIMIT_SIMULATION_TICKS; nothing ran */
};
enum mo_simulation_sent {
    MO_SENT_REPLIED,     /* the tool returned; reply holds its whole result */
    MO_SENT_REJECTED,    /* an updating tool returned an Error, held in reply; nothing committed */
    MO_SENT_TOOL_FAILED, /* status says how the handler failed; nothing committed */
    MO_SENT_DOWN,        /* the service is crashed or stopped; nothing was queued */
    MO_SENT_FAILED,      /* the service answered with another status, such as a store failure */
    MO_SENT_ARGUMENT,    /* copying argument in was refused; nothing was queued */
    MO_SENT_TOO_LARGE,   /* the saved message, bytes long, exceeds MO_LIMIT_SAVED_BYTES */
    MO_SENT_REFUSED,     /* the service refused admission; nothing was queued and no handler ran */
    MO_SENT_REPLY_COPY,  /* copying the reply out was refused; the tool ran and updated says */
    MO_SENT_STOPPED      /* failure returned false */
};
struct mo_simulation_reply {
    enum mo_service_status status;
    bool updated;         /* the tool's update committed */
    uint32_t type;        /* the tool's complete result type */
    struct mo_cell reply; /* REPLIED and REJECTED: owned by the caller's heap */
    uint32_t argument;    /* ARGUMENT: the refused argument's index */
    const char *refusal;  /* ARGUMENT and REPLY_COPY: the saved codec's reason */
    uint64_t bytes;       /* TOO_LARGE */
};
/* Service index of an initialized image that outlives the simulation, on a fresh in-memory store,
 * its clock at now, a valid Time. The service is not open yet. Only OK writes out. */
MO_MUST_USE enum mo_store_status mo_simulation_new(const struct mo_image *image, uint32_t index,
                                                   int64_t now, struct mo_simulation **out);
void mo_simulation_free(struct mo_simulation *simulation);
/* Opens the service on the simulation's store, recovering what it holds. Startup ticks stay
 * queued, so the state can be read as recovery left it; drain runs them. */
MO_MUST_USE enum mo_simulation_outcome mo_simulation_open(struct mo_simulation *simulation,
                                                          const struct mo_simulation_hooks *hooks);
/* Runs every queued message. */
MO_MUST_USE enum mo_simulation_outcome mo_simulation_drain(struct mo_simulation *simulation,
                                                           const struct mo_simulation_hooks *hooks);
/* Drains, then sends tool its arguments: typed values from heap, a heap of the same image. They
 * are copied in through the saved codec and the reply is copied back into heap. Runs the tool and
 * whatever was queued behind it. */
MO_MUST_USE enum mo_simulation_sent mo_simulation_send(struct mo_simulation *simulation,
                                                       const struct mo_simulation_hooks *hooks,
                                                       struct mo_heap *heap, uint32_t tool,
                                                       const struct mo_cell *arguments,
                                                       struct mo_simulation_reply *out);
/* The firings due if the clock moved by `by`; needs nothing queued, as after a drain. false,
 * writing nothing, when `by` is negative or ends after year 9999. */
MO_MUST_USE bool mo_simulation_firings(const struct mo_simulation *simulation, int64_t by,
                                       uint64_t *out);
/* Drains, then moves the clock by `by`, stopping at each due deadline to run what fell due.
 * Unless DURATION, firings receives the firings counted before any ran, or 0 if none were. */
MO_MUST_USE enum mo_simulation_outcome
mo_simulation_advance(struct mo_simulation *simulation, const struct mo_simulation_hooks *hooks,
                      int64_t by, uint64_t *firings);
/* Ends the service where it stands; the store keeps what it committed. */
void mo_simulation_crash(struct mo_simulation *simulation);
MO_MUST_USE bool mo_simulation_running(const struct mo_simulation *simulation);
/* The supervisor's restarts so far, counting the one that stops the service. */
MO_MUST_USE uint64_t mo_simulation_restarts(const struct mo_simulation *simulation);
/* Appends the live state's saved encoding. false when the service is crashed or never opened, or
 * the encoding fails. */
MO_MUST_USE bool mo_simulation_encode(struct mo_simulation *simulation, struct mo_buffer *out);

#endif
