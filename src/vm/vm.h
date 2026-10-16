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
MO_MUST_USE struct mo_process *mo_process_new(const struct mo_image *image,
                                              const struct mo_limits *limits);
void mo_process_free(struct mo_process *process);
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

#endif
