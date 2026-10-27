#define _DEFAULT_SOURCE
#include "test.h"
#include "vm/vm.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <unistd.h>

/* Only this test's copies of heap.c, vm/image.c and vm/defect.c substitute
 * allocation/entropy/replay. No production hook or global allocator. */
static unsigned countdown;
static bool entropy_failure;
static bool replay_failure;
MO_MUST_USE void *mo_vm_fault_calloc(size_t count, size_t size);
MO_MUST_USE int mo_vm_fault_entropy(void *bytes, size_t length);
MO_MUST_USE bool mo_vm_fault_readable(const struct mo_image *image, uint32_t function, uint32_t pc,
                                      bool *readable, struct mo_load_error *error);

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

bool mo_vm_fault_readable(const struct mo_image *image, uint32_t function, uint32_t pc,
                          bool *readable, struct mo_load_error *error) {
    if (!replay_failure)
        return mo_image_registers_readable(image, function, pc, readable, error);
    /* A refusing replay may have filled a partial, untrusted mask. */
    uint32_t count = mo_image_bytecode(image)->functions[function].register_count;
    for (uint32_t i = 0; i < count; i++)
        readable[i] = true;
    *error = (struct mo_load_error){.reason = "no memory", .kind = MO_BYTECODE_NO_MEMORY};
    return false;
}

static MO_MUST_USE bool count_variable(const struct mo_defect_variable *variable, void *context) {
    unsigned *count = context;
    CHECK(variable->name_length == 5 && memcmp(variable->name, "value", 5) == 0 &&
          mo_cell_get_int(variable->value) == 37);
    (*count)++;
    return true;
}

static void reporting_faults(void) {
    const char *source =
        "bytecode 0.2 shorthand\nmodule M exposes [M.stop]\n"
        "function M.stop(x: Int owned) : Unit function in M\n registers [r1: Text]\n"
        " variables [(r0, \"value\", @start, @end), (r1, \"unchecked\", @start, @end)]\n"
        " @start:\n const r1, \"stop\"\n stop r1\n @end:\nend\n";
    struct mo_buffer wire;
    struct mo_assemble_error error;
    mo_buffer_init(&wire, MO_LIMIT_BYTECODE_SIZE);
    MO_ASSERT(mo_bytecode_assemble(source, strlen(source), &wire, &error));
    struct mo_load_error load_error;
    struct mo_image *image = mo_load(wire.bytes, wire.length, &load_error);
    MO_ASSERT(image != NULL);
    struct mo_limits limits = mo_limits_default();
    struct mo_defect defect;
    MO_ASSERT(mo_image_init(image, &limits, &defect) == MO_RETURNED);
    struct mo_process *p = mo_process_new(image, &limits);
    MO_ASSERT(p != NULL);
    /* Stop before writing r1, so a fabricated partial mask would expose an
     * unchecked cell even if the valid input's value happened to look right. */
    mo_process_instruction_limit(p, 0);
    struct mo_cell argument = mo_cell_int(37), result;
    MO_ASSERT(mo_call(p, 0, &argument, 1, &result, &defect) == MO_STOPPED);
    for (unsigned fault = 0; fault < 2; fault++) {
        countdown = fault == 0 ? 1 : 0;
        replay_failure = fault == 1;
        unsigned count = 0;
        CHECK(mo_visit_defect_variables(p, &defect, 8, UINT64_MAX, count_variable, &count) ==
              MO_DEFECT_VARIABLES_UNAVAILABLE);
        CHECK(count == 0);
        /* Human reporting still distinguishes unavailable variables. */
        countdown = fault == 0 ? 1 : 0;
        struct mo_buffer human;
        mo_buffer_init(&human, 1024);
        CHECK(mo_render_defect(p, &defect, &human) && mo_buffer_append_byte(&human, 0));
        CHECK(strstr((const char *)human.bytes, "  <variables unavailable>\n") != NULL &&
              strstr((const char *)human.bytes, "value =") == NULL &&
              strstr((const char *)human.bytes, "unchecked =") == NULL);
        mo_buffer_free(&human);
        countdown = 0;
        replay_failure = false;
        CHECK(mo_visit_defect_variables(p, &defect, 8, UINT64_MAX, count_variable, &count) ==
              MO_DEFECT_VARIABLES_COMPLETE);
        CHECK(count == 1);
    }
    mo_process_free(p);
    mo_image_free(image);
    mo_buffer_free(&wire);
}

int main(void) {
    const char *source =
        "bytecode 0.2 shorthand\nrecord Box\n value: Int\nend\nmodule M exposes [M.box]\n"
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
        CHECK(defect.limit_name == NULL && defect.memory == (fault != 2) &&
              defect.process == NULL && mo_image_runtime(image) == NULL);
        entropy_failure = false;
        countdown = 0;
        CHECK(mo_image_init(image, &limits, &defect) == MO_RETURNED);
        struct mo_process *p = mo_process_new(image, &limits);
        MO_ASSERT(p != NULL);
        struct mo_cell arg = mo_cell_int(37), out = mo_cell_int(99);
        countdown = 1; /* The constructor's native calloc, with ample heap. */
        CHECK(mo_call(p, 0, &arg, 1, &out, &defect) == MO_STOPPED);
        CHECK(strcmp(defect.reason, "running out of memory") == 0 && defect.limit_name == NULL &&
              defect.memory && defect.frames->pc == 0 && mo_cell_get_int(out) == 99);
        mo_process_free(p);
        mo_image_free(image);
    }
    mo_buffer_free(&wire);
    reporting_faults();
    return mo_test_report("vm faults");
}
