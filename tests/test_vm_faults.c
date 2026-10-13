#define _DEFAULT_SOURCE
#include "test.h"
#include "vm/vm.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <unistd.h>

/* Only this test's copies of heap.c and vm/image.c rename the system calls.
 * No production hook, non-POSIX linker interposition or global allocator. */
static unsigned countdown;
static bool entropy_failure;
MO_MUST_USE void *mo_vm_fault_calloc(size_t count, size_t size);
MO_MUST_USE int mo_vm_fault_entropy(void *bytes, size_t length);

void *mo_vm_fault_calloc(size_t count, size_t size) {
    if (countdown != 0 && --countdown == 0) {
        errno = ENOMEM;
        return NULL;
    }
    return calloc(count, size);
}

int mo_vm_fault_entropy(void *bytes, size_t length) {
    if (entropy_failure) {
        errno = ENOSYS;
        return -1;
    }
    return getentropy(bytes, length);
}

int main(void) {
    const char *source =
        "bytecode 0.1 shorthand\nrecord Box\n value: Int\nend\nmodule M exposes [M.box]\n"
        "function M.box(x: Int owned) : Box function in M\n registers [r1: Box]\n"
        " construct r1, Box, r0\n ret r1\nend\n";
    struct mo_buffer wire;
    struct mo_assemble_error error;
    mo_buffer_init(&wire, MO_LIMIT_BYTECODE_SIZE);
    CHECK(mo_bytecode_assemble(source, strlen(source), &wire, &error));
    struct mo_limits limits = mo_limits_default();
    for (unsigned fault = 0; fault < 3; fault++) {
        struct mo_load_error load_error;
        struct mo_image *image = mo_load(wire.bytes, wire.length, &load_error);
        MO_ASSERT(image != NULL);
        /* First allocation is runtime, second heap image; entropy is distinct. */
        countdown = fault < 2 ? fault + 1 : 0;
        entropy_failure = fault == 2;
        struct mo_defect defect;
        CHECK(mo_image_init(image, &limits, &defect) == MO_STOPPED);
        CHECK(strcmp(defect.reason, fault == 2 ? "internal error: system randomness unavailable"
                                               : "running out of memory") == 0);
        CHECK(defect.limit_name == NULL && defect.process == NULL &&
              mo_image_runtime(image) == NULL);
        entropy_failure = false;
        countdown = 0;
        CHECK(mo_image_init(image, &limits, &defect) == MO_RETURNED);
        struct mo_process *p = mo_process_new(image, &limits);
        MO_ASSERT(p != NULL);
        struct mo_cell arg = mo_cell_int(37), out = mo_cell_int(99);
        countdown = 1; /* The constructor's native calloc, with ample heap. */
        CHECK(mo_call(p, 0, &arg, 1, &out, &defect) == MO_STOPPED);
        CHECK(strcmp(defect.reason, "running out of memory") == 0 && defect.limit_name == NULL &&
              defect.frames->pc == 0 && mo_cell_get_int(out) == 99);
        mo_process_free(p);
        mo_image_free(image);
    }
    mo_buffer_free(&wire);
    return mo_test_report("vm faults");
}
