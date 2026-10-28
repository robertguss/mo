#include "base/limits.h"
#include "test.h"
#include "tools/protocol.h"
#include "vm/vm.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static struct {
    void *address;
    size_t size;
} allocations[256];
static bool watching;
static unsigned calls, released, fail_after;

void *mo_protocol_memory_malloc(size_t size);
void *mo_protocol_memory_realloc(void *address, size_t size);
void mo_protocol_memory_free(void *address);

void *mo_protocol_memory_malloc(size_t size) {
    if (watching) {
        calls++;
        if (fail_after != 0 && --fail_after == 0) {
            errno = ENOMEM;
            return NULL;
        }
    }
    void *address = malloc(size);
    if (watching && address != NULL) {
        for (size_t i = 0; i < sizeof allocations / sizeof *allocations; i++) {
            if (allocations[i].address == NULL) {
                allocations[i].address = address;
                allocations[i].size = size;
                return address;
            }
        }
        MO_UNREACHABLE();
    }
    return address;
}

void *mo_protocol_memory_realloc(void *address, size_t size) {
    for (size_t i = 0; i < sizeof allocations / sizeof *allocations; i++)
        MO_ASSERT(allocations[i].address == NULL || allocations[i].address != address);
    return realloc(address, size);
}

void mo_protocol_memory_free(void *address) {
    for (size_t i = 0; i < sizeof allocations / sizeof *allocations; i++) {
        if (address != NULL && allocations[i].address == address) {
            const uint8_t *bytes = address;
            bool cleared = true;
            for (size_t j = 0; j < allocations[i].size; j++)
                cleared = cleared && bytes[j] == 0;
            CHECK(cleared);
            allocations[i].address = NULL;
            released++;
        }
    }
    free(address);
}

static void scratch(struct mo_image *image) {
    struct mo_limits limits = mo_limits_default();
    struct mo_process *process = mo_process_new(image, &limits);
    MO_ASSERT(process != NULL);
    mo_process_confidential(process);
    watching = true;
    for (unsigned fault = 0; fault < 2; fault++) {
        struct mo_scratch work;
        mo_scratch_init(&work, mo_process_heap(process));
        uint8_t *bytes = mo_scratch_resize(&work, NULL, 0, 4096);
        MO_ASSERT(bytes != NULL);
        memset(bytes, 0x71, 4096);
        fail_after = fault;
        uint8_t *grown = mo_scratch_resize(&work, bytes, 4096, 8192);
        CHECK((grown == NULL) == (fault != 0));
        const uint8_t *kept = grown == NULL ? bytes : grown;
        for (size_t i = 0; i < 4096; i++)
            CHECK(kept[i] == 0x71);
        fail_after = 0;
        mo_scratch_free(&work);
    }
    mo_process_free(process);
    watching = false;
}

static unsigned headers(struct mo_image *image, uint32_t parse, unsigned fault, uint64_t budget) {
    const char *header = "POST /mcp HTTP/1.1\r\nAuthorization: Bearer canary-wipe-47\r\n\r\n";
    struct mo_cell input[128];
    size_t size = strlen(header);
    MO_ASSERT(size <= sizeof input / sizeof *input);
    for (size_t i = 0; i < size; i++)
        input[i] = mo_cell_byte((uint8_t)header[i]);
    struct mo_limits limits = mo_limits_default();
    struct mo_process *process = mo_process_new(image, &limits);
    MO_ASSERT(process != NULL);
    mo_process_confidential(process);
    mo_process_instruction_limit(process, budget);
    struct mo_heap *heap = mo_process_heap(process);
    const struct mo_bytecode_function *function = &mo_image_bytecode(image)->functions[parse];
    struct mo_cell arguments[2];
    MO_ASSERT(
        mo_value_list(heap, function->parameters[0].type, input, (uint32_t)size, &arguments[0]));
    struct mo_cell bounds[] = {mo_cell_int(MO_LIMIT_MCP_HEADER_BYTES),
                               mo_cell_int(MO_LIMIT_MCP_LINE_BYTES),
                               mo_cell_int(MO_LIMIT_MCP_HEADER_FIELDS)};
    MO_ASSERT(mo_value_case(heap, function->parameters[1].type, 0, bounds, 3, &arguments[1]));
    watching = true;
    calls = 0;
    fail_after = fault;
    struct mo_cell result;
    struct mo_defect defect;
    enum mo_outcome outcome = mo_call(process, parse, arguments, 2, &result, &defect);
    CHECK(outcome == (fault != 0 || budget == 0 ? MO_STOPPED : MO_RETURNED));
    if (outcome == MO_RETURNED)
        CHECK(mo_cell_get_tag(result) == 0);
    unsigned count = calls;
    fail_after = 0;
    mo_process_free(process);
    watching = false;
    for (size_t i = 0; i < sizeof allocations / sizeof *allocations; i++)
        CHECK(allocations[i].address == NULL);
    return count;
}

int main(void) {
    struct mo_image *image = mo_protocol_build();
    MO_ASSERT(image != NULL);
    struct mo_limits limits = mo_limits_default();
    struct mo_defect defect;
    MO_ASSERT(mo_image_init(image, &limits, &defect) == MO_RETURNED);
    uint32_t parse = UINT32_MAX;
    const struct mo_bytecode *file = mo_image_bytecode(image);
    for (uint32_t i = 0; i < file->function_count; i++) {
        const struct mo_bytecode_string *name = &file->strings[file->functions[i].name];
        if (name->length == 18 && memcmp(name->bytes, "Http.parse_headers", 18) == 0)
            parse = i;
    }
    MO_ASSERT(parse != UINT32_MAX);
    scratch(image);
    unsigned count = headers(image, parse, 0, MO_LIMIT_MCP_HEADER_WORK);
    CHECK(count > 1);
    for (unsigned fault = 1; fault <= count; fault++)
        (void)headers(image, parse, fault, MO_LIMIT_MCP_HEADER_WORK);
    (void)headers(image, parse, 0, 0);
    CHECK(released > 0);
    mo_image_free(image);
    return mo_test_report("protocol memory");
}
