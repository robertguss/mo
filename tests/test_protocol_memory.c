#include "base/limits.h"
#include "host/host.h"
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
static void *process_address;
static size_t process_size;
static unsigned processes_released, operand_wipes, nonzero_operands;

void *mo_protocol_memory_malloc(size_t size);
void *mo_protocol_memory_calloc(size_t count, size_t size);
void *mo_protocol_memory_realloc(void *address, size_t size);
void mo_protocol_memory_free(void *address);
void mo_protocol_memory_wipe(void *address, size_t size);

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

void *mo_protocol_memory_calloc(size_t count, size_t size) {
    void *address = calloc(count, size);
    if (watching && address != NULL) {
        MO_ASSERT(count == 1 && process_address == NULL);
        process_address = address;
        process_size = size;
    }
    return address;
}

void *mo_protocol_memory_realloc(void *address, size_t size) {
    for (size_t i = 0; i < sizeof allocations / sizeof *allocations; i++)
        MO_ASSERT(allocations[i].address == NULL || allocations[i].address != address);
    return realloc(address, size);
}

void mo_protocol_memory_wipe(void *address, size_t size) {
    bool nonzero = false;
    const uint8_t *bytes = address;
    for (size_t i = 0; i < size; i++)
        nonzero = nonzero || bytes[i] != 0;
    mo_host_wipe(address, size);
    for (size_t i = 0; i < size; i++)
        CHECK(bytes[i] == 0);
    if (watching && address != process_address) {
        operand_wipes++;
        nonzero_operands += nonzero ? 1u : 0u;
    }
}

void mo_protocol_memory_free(void *address) {
    if (address != NULL && address == process_address) {
        const uint8_t *bytes = address;
        bool cleared = true;
        for (size_t i = 0; i < process_size; i++)
            cleared = cleared && bytes[i] == 0;
        CHECK(cleared);
        process_address = NULL;
        processes_released++;
    }
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
    static const uint8_t zero[4096] = {0};
    struct mo_limits limits = mo_limits_default();
    watching = true;
    struct mo_process *process = mo_process_new(image, &limits);
    MO_ASSERT(process != NULL);
    mo_process_confidential(process);
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
        if (grown != NULL) {
            CHECK(grown != bytes);
            CHECK(memcmp(bytes, zero, sizeof zero) == 0);
            memset(grown, 0x56, 8192);
            uint8_t *shrunk = mo_scratch_resize(&work, grown, 8192, 4096);
            CHECK(shrunk == grown);
            CHECK(memcmp(grown + 4096, zero, 4096) == 0);
            for (size_t i = 0; i < 4096; i++)
                CHECK(grown[i] == 0x56);
            uint8_t *newest = mo_scratch_resize(&work, NULL, 0, 16);
            MO_ASSERT(newest != NULL);
            memset(newest, 0x37, 16);
            shrunk = mo_scratch_resize(&work, grown, 4096, 2048);
            CHECK(shrunk == grown);
            CHECK(memcmp(grown + 2048, zero, 2048) == 0);
            for (size_t i = 0; i < 2048; i++)
                CHECK(grown[i] == 0x56);
            for (size_t i = 0; i < 16; i++)
                CHECK(newest[i] == 0x37);
            CHECK(mo_scratch_resize(&work, newest, 16, 0) == newest);
            CHECK(memcmp(newest, zero, 16) == 0);
            CHECK(mo_scratch_resize(&work, grown, 2048, 0) == grown);
            CHECK(memcmp(grown, zero, 2048) == 0);
        }
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
    watching = true;
    unsigned previous = processes_released;
    struct mo_process *process = mo_process_new(image, &limits);
    MO_ASSERT(process != NULL);
    mo_process_confidential(process);
    mo_process_account_stack(process);
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
    calls = 0;
    operand_wipes = 0;
    nonzero_operands = 0;
    fail_after = fault;
    struct mo_cell result;
    struct mo_defect defect;
    enum mo_outcome outcome = mo_call(process, parse, arguments, 2, &result, &defect);
    CHECK(outcome == (fault != 0 || budget == 0 ? MO_STOPPED : MO_RETURNED));
    if (outcome == MO_RETURNED)
        CHECK(mo_cell_get_tag(result) == 0);
    if (fault == 0 && budget != 0)
        CHECK(operand_wipes > 0 && nonzero_operands > 0);
    unsigned count = calls;
    fail_after = 0;
    mo_process_free(process);
    CHECK(process_address == NULL && processes_released == previous + 1);
    watching = false;
    for (size_t i = 0; i < sizeof allocations / sizeof *allocations; i++)
        CHECK(allocations[i].address == NULL);
    return count;
}

static void request_budget(struct mo_image *image, uint32_t chunk) {
    const struct mo_bytecode_function *function = &mo_image_bytecode(image)->functions[chunk];
    for (unsigned pressure = 0; pressure < 2; pressure++) {
        struct mo_limits limits = mo_limits_default();
        limits.heap_size = MO_LIMIT_MCP_HEADER_HEAP;
        limits.stack_size = MO_LIMIT_MCP_HEADER_HEAP;
        struct mo_process *process = mo_process_new(image, &limits);
        MO_ASSERT(process != NULL);
        mo_process_confidential(process);
        mo_process_account_stack(process);
        mo_process_instruction_limit(process, MO_LIMIT_MCP_HEADER_WORK);
        struct mo_heap *heap = mo_process_heap(process);
        struct mo_cell bytes[] = {mo_cell_byte('0'), mo_cell_byte('\r'), mo_cell_byte('\n')};
        struct mo_cell input;
        MO_ASSERT(mo_value_list(heap, function->parameters[0].type, bytes, 3, &input));
        struct mo_scratch native;
        mo_scratch_init(&native, heap);
        size_t available = limits.heap_size - mo_process_memory_used(process);
        size_t size = pressure == 0 ? 4096 : available - MO_ARENA_CHUNK_HEADER - 256;
        uint8_t *buffer = mo_scratch_resize(&native, NULL, 0, size);
        MO_ASSERT(buffer != NULL);
        memset(buffer, 0x69, size);
        uint64_t previous = mo_process_instructions_remaining(process), consumed = 0;
        for (unsigned repeat = 0; repeat < (pressure == 0 ? 2u : 1u); repeat++) {
            struct mo_cell arguments[] = {input, mo_cell_int(0), mo_cell_int(8)};
            if (function->parameters[0].ownership == MO_PARAMETER_OWNED)
                arguments[0] = mo_value_dup(heap, function->parameters[0].type, input);
            struct mo_cell result;
            struct mo_defect defect;
            enum mo_outcome outcome = mo_call(process, chunk, arguments, 3, &result, &defect);
            CHECK(outcome == (pressure == 0 ? MO_RETURNED : MO_STOPPED));
            if (outcome == MO_RETURNED) {
                CHECK(mo_cell_get_tag(result) == 0);
                mo_value_drop(heap, function->result_type, result);
            }
            uint64_t remaining = mo_process_instructions_remaining(process);
            if (pressure == 0) {
                CHECK(remaining < previous);
                CHECK(repeat == 0 || consumed == previous - remaining);
                consumed = previous - remaining;
                previous = remaining;
            }
            CHECK(mo_process_memory_peak(process) >= mo_process_memory_used(process));
            CHECK(mo_process_memory_peak(process) <= limits.heap_size);
            CHECK(buffer[0] == 0x69 && buffer[size - 1] == 0x69);
        }
        size_t peak = mo_process_memory_peak(process);
        mo_scratch_free(&native);
        CHECK(mo_process_memory_peak(process) == peak);
        CHECK(mo_process_memory_used(process) < peak);
        mo_value_drop(heap, function->parameters[0].type, input);
        mo_process_free(process);
    }
}

int main(void) {
    struct mo_image *image = mo_protocol_build();
    MO_ASSERT(image != NULL);
    struct mo_limits limits = mo_limits_default();
    struct mo_defect defect;
    MO_ASSERT(mo_image_init(image, &limits, &defect) == MO_RETURNED);
    uint32_t parse = UINT32_MAX, chunk = UINT32_MAX;
    const struct mo_bytecode *file = mo_image_bytecode(image);
    for (uint32_t i = 0; i < file->function_count; i++) {
        const struct mo_bytecode_string *name = &file->strings[file->functions[i].name];
        if (name->length == 18 && memcmp(name->bytes, "Http.parse_headers", 18) == 0)
            parse = i;
        if (name->length == 15 && memcmp(name->bytes, "Http.chunk_size", 15) == 0)
            chunk = i;
    }
    MO_ASSERT(parse != UINT32_MAX && chunk != UINT32_MAX);
    request_budget(image, chunk);
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
