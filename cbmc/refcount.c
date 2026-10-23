/* Count transitions, not graph destruction. The real type dispatch includes
 * immediate, reference, empty, inline, reference-payload and boxed choices. */
#include "heap/cell.c"
#include "heap/heap.c"
#include "heap/types.c"

#pragma CPROVER check push
#pragma CPROVER check disable "bounds"
#pragma CPROVER check disable "pointer"
#pragma CPROVER check disable "pointer-overflow"
#pragma CPROVER check disable "pointer-primitive"
#pragma CPROVER check disable "signed-overflow"
#pragma CPROVER check disable "conversion"

uint32_t nondet_u32(void);
bool nondet_bool(void);

_Noreturn void mo_assert_failed(const char *expression, const char *file, int line) {
    (void)expression;
    (void)file;
    (void)line;
    __CPROVER_assert(0, "MO_ASSERT holds");
    __CPROVER_assume(0);
    abort();
}

void reached_end(void) {
}

void harness_refcount_dup(void) {
    struct mo_type_field fields[] = {{.type = 1}, {.type = 1}, {.type = 0}};
    struct mo_type_case cases[] = {{.field_count = 0},
                                   {.field_count = 1, .fields = fields + 2},
                                   {.field_count = 1, .fields = fields},
                                   {.field_count = 2, .fields = fields}};
    struct mo_type items[] = {{.kind = MO_TYPE_INT},
                              {.kind = MO_TYPE_TEXT, .reference = true},
                              {.kind = MO_TYPE_CHOICE, .cases = cases, .case_count = 4}};
    struct mo_types types = {.items = items, .count = 3, .finished = true};
    struct mo_heap process = {.types = &types};
    uint32_t count = nondet_u32(), layout = nondet_u32();
    __CPROVER_assume(count != 0);
    struct mo_object object = {.count = count, .layout = layout};
    uint32_t type = nondet_u32(), tag = nondet_u32();
    __CPROVER_assume(type < 3 && tag < 4);
    struct mo_cell value = type == 0 ? mo_cell_int(-17) : mo_cell_ref(&object);
    if (type == 2)
        value = mo_cell_case(tag, tag < 2 ? mo_cell_int(23) : mo_cell_ref(&object));
    struct mo_cell result = mo_value_dup(&process, type, value);
    bool reference = type == 1 || (type == 2 && tag >= 2);
    uint64_t expected = count;
    if (reference && count < UINT32_MAX)
        expected++;
    __CPROVER_assert(object.count == expected, "dup: increments references, saturates immortals");
    __CPROVER_assert(object.layout == layout, "dup: preserves layout");
    __CPROVER_assert(memcmp(&result, &value, sizeof value) == 0, "dup: returns the same cell");
    reached_end();
}

void harness_refcount_enqueue(void) {
    struct mo_allocation allocation, previous;
    uint32_t count = nondet_u32(), layout = nondet_u32();
    __CPROVER_assume(count != 0);
    allocation.object = (struct mo_object){.count = count, .layout = layout};
    allocation.pending = nondet_bool() ? &previous : NULL;
    struct mo_allocation *old_link = allocation.pending;
    struct mo_allocation *pending = nondet_bool() ? &previous : NULL;
    struct mo_allocation *old_pending = pending;
    bool present = nondet_bool();
    enqueue_object(present ? &allocation.object : NULL, &pending);
    uint32_t expected = present && count < UINT32_MAX ? count - 1 : count;
    __CPROVER_assert(allocation.object.count == expected,
                     "drop: decrements live references, preserves immortals and null");
    __CPROVER_assert(allocation.object.layout == layout, "drop: preserves layout");
    if (present && count == 1) {
        __CPROVER_assert(pending == &allocation, "drop: queues exactly the zero transition");
        __CPROVER_assert(allocation.pending == old_pending, "drop: keeps the pending tail");
    } else {
        __CPROVER_assert(pending == old_pending && allocation.pending == old_link,
                         "drop: otherwise leaves the queue alone");
    }
    reached_end();
}

#pragma CPROVER check pop
