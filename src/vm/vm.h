/* Execution of loaded, verified images. No C recursion implements Mo calls. */
#ifndef MO_VM_H
#define MO_VM_H

#include "bytecode/bytecode.h"
#include "heap/heap.h"

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
MO_MUST_USE size_t mo_process_stack_used(const struct mo_process *process);
/* Direct FUNCTION/spec calls only. INIT is invoked by image_init; closures by
 * call_closure. Owned parameters transfer on every outcome, borrowed ones do
 * not. Only RETURNED writes the owned result. Calls may repeat after RETURNED;
 * STOPPED/FAILED end this process and preserve its frames for the report.
 * C-owned borrowed arguments must outlive every frame/report reading them. */
MO_MUST_USE enum mo_outcome mo_call(struct mo_process *process, uint32_t function,
                                    const struct mo_cell *arguments, uint32_t argument_count,
                                    struct mo_cell *result_out, struct mo_defect *defect_out);
/* C harness hook, not a Mo resource limit. Default is unlimited. Exhaustion
 * produces an internal stop before the next instruction (for bounded fuzzing). */
void mo_process_instruction_limit(struct mo_process *process, uint64_t instructions);
/* NULL process selects defect->process, including an initialization defect.
 * Append transactionally. Replay/printing exhaustion never reads a register
 * whose readability could not be established. */
MO_MUST_USE bool mo_render_defect(const struct mo_process *process, const struct mo_defect *defect,
                                  struct mo_buffer *out);

#endif
