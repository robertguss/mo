/* Execution of loaded, verified images. No C recursion implements Mo calls. */
#ifndef MO_VM_H
#define MO_VM_H

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
void mo_process_account_stack(struct mo_process *process);
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
    MO_SERVICE_SAVED_INVALID
};
/* Stable descriptions for embedding errors; no saved bytes or secrets appear. */
MO_MUST_USE const char *mo_service_status_text(enum mo_service_status status);
enum mo_service_event_kind { MO_SERVICE_FAILURE, MO_SERVICE_RESTART, MO_SERVICE_STOP };
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

#endif
