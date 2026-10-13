#ifndef MO_VM_INTERNAL_H
#define MO_VM_INTERNAL_H

#include "vm/vm.h"

enum mo_vm_state { VM_UNDEFINED, VM_VALUE, VM_OWNED, VM_BORROWED, VM_MOVED, VM_TOKEN };
struct mo_vm_frame {
    size_t previous, extent;
    uint32_t function, pc, destination, next;
    struct mo_defect_frame trace;
    uint32_t owners;
};
struct mo_vm_image;
struct mo_process {
    const struct mo_image *image;
    const struct mo_bytecode *file;
    const struct mo_types *types;
    struct mo_vm_image *runtime;
    struct mo_heap *heap;
    struct mo_host host;
    struct mo_cell capabilities[MO_CAP_RANDOM + 1];
    bool capability_set[MO_CAP_RANDOM + 1];
    struct mo_limits limits;
    uint8_t *stack;
    size_t used, capacity, top;
    uint32_t depth;
    uint64_t instructions;
    bool bounded, terminal, owns_heap;
    struct mo_defect defect;
};
struct mo_vm_image {
    struct mo_heap_image *heap;
    struct mo_host_types host_types;
    struct mo_cell *constants, *globals;
    bool *set;
    enum mo_outcome outcome;
    bool ready;
    struct mo_process initializer;
};

MO_MUST_USE struct mo_vm_frame *mo_vm_frame(struct mo_process *process);
MO_MUST_USE struct mo_cell *mo_vm_cells(struct mo_vm_frame *frame);
MO_MUST_USE uint8_t *mo_vm_states(const struct mo_process *process, struct mo_vm_frame *frame);
MO_MUST_USE size_t mo_vm_frame_size(uint32_t registers);
MO_MUST_USE const char *mo_vm_reserve(struct mo_process *process, size_t needed, uint32_t depth);
void mo_vm_enter(struct mo_process *process, uint32_t function, const struct mo_cell *arguments,
                 struct mo_cell environment, bool closure, bool tail, uint32_t destination,
                 uint32_t next);
MO_MUST_USE enum mo_outcome mo_vm_execute(struct mo_process *process, struct mo_cell *result);
MO_MUST_USE enum mo_outcome mo_vm_stop(struct mo_process *process, const char *reason,
                                       const char *limit, bool detail, struct mo_cell value,
                                       enum mo_outcome outcome);
MO_MUST_USE struct mo_vm_span mo_vm_span(const struct mo_bytecode *file, uint32_t function,
                                         uint32_t pc);
MO_MUST_USE bool mo_vm_number(uint32_t opcode, struct mo_cell a, struct mo_cell b,
                              struct mo_cell *out, const char **reason);
void mo_vm_dispose(struct mo_process *process);

#endif
