#define _POSIX_C_SOURCE 200809L
#include "base/export.h"
#include "base/limits.h"
#include "base/log-record.h"
#include "host/host.h"
#include "test.h"
#include "tools/protocol.h"
#include "vm/vm.h"

#include <errno.h>
#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static struct {
    void *address;
    size_t size;
} allocations[256];
static bool watching, expect_zero = true;
static unsigned calls, released, fail_after, canary_leftovers;
static void *process_address;
static size_t process_size;
static unsigned processes_released, operand_wipes, nonzero_operands;
static const uint8_t *canary;
static size_t canary_size;

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

static bool holds_canary(const uint8_t *bytes, size_t size) {
    if (canary == NULL || canary_size == 0 || size < canary_size)
        return false;
    for (size_t i = 0; i + canary_size <= size; i++)
        if (memcmp(bytes + i, canary, canary_size) == 0)
            return true;
    return false;
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
            if (holds_canary(bytes, allocations[i].size))
                canary_leftovers++;
            if (expect_zero)
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

static void export_storage(struct mo_image *image) {
    struct mo_limits limits = mo_limits_default();
    struct mo_process *process = mo_process_new(image, &limits);
    MO_ASSERT(process != NULL);
    struct mo_heap *heap = mo_process_heap(process);
    size_t used = mo_heap_used(heap), objects = mo_heap_objects(heap);
    mo_heap_set_limit(heap, used); /* pin allocation still succeeds with no live heap allowance */
    uint8_t saved[] = {'M', 'O', 'S', 'V', 1, 1, 0, 0, 0, 42, 73};
    struct mo_export_identity id = {.kind = 1, .label = "memory"};
    struct mo_export_pin pin = {0};
    watching = true;
    fail_after = 1;
    CHECK(!mo_export_create(&pin, &id, saved, sizeof saved, 0));
    CHECK(pin.bytes == NULL && pin.arena.used == 0);
    fail_after = 0;
    CHECK(mo_export_create(&pin, &id, saved, sizeof saved, 0));
    CHECK(pin.bytes != NULL && pin.arena.used <= MO_LIMIT_EXPORT_BYTES / MO_LIMIT_EXPORT_PINS);
    CHECK(mo_heap_used(heap) == used && mo_heap_objects(heap) == objects);
    saved[10] = 99;
    CHECK(pin.bytes[pin.length - 33] == 73);
    mo_export_release(&pin); /* existing allocator observer checks confidential zeroing */
    watching = false;
    CHECK(mo_heap_used(heap) == used && mo_heap_objects(heap) == objects);
    mo_heap_set_limit(heap, limits.heap_size);
    mo_process_free(process);
}

/* Larger than store_new's first chunk, so a reused ordinary 4096-byte allocation cannot
 * hold a leftover copy of the payload. */
enum { HOST_CANARY_BYTES = 5000 };
static uint8_t host_canary[HOST_CANARY_BYTES];

static void paint_canary(void) {
    static const char mark[] = "CANARY-HOST-COPY-WIPE-47";
    memcpy(host_canary, mark, sizeof mark - 1);
    for (size_t i = sizeof mark - 1; i < HOST_CANARY_BYTES; i++)
        host_canary[i] = (uint8_t)(0xA5 ^ (uint8_t)i);
}

static void watch_copies(void) {
    expect_zero = false;
    canary = host_canary;
    canary_size = sizeof host_canary;
    canary_leftovers = 0;
    watching = true;
}

static void unwatch_copies(void) {
    watching = false;
    expect_zero = true;
    canary = NULL;
    canary_size = 0;
    for (size_t i = 0; i < sizeof allocations / sizeof *allocations; i++)
        CHECK(allocations[i].address == NULL);
}

static void fill_store(const char *path, bool with_log) {
    struct mo_store *store;
    MO_ASSERT(mo_store_open(path, &store) == MO_STORE_OK);
    struct mo_store_record snapshot = {MO_STORE_STATE, 0, 5, host_canary, sizeof host_canary};
    CHECK(mo_store_call(store, MO_STORE_SNAPSHOT, &snapshot, NULL, NULL) == MO_STORE_OK);
    if (with_log) {
        struct mo_buffer payload;
        mo_buffer_init(&payload, 64);
        struct mo_log_record_header header = {.arrival = 1, .timer = true};
        CHECK(mo_log_record_header_write(&payload, &header));
        struct mo_store_record log = {MO_STORE_LOG, 1, 5, payload.bytes, payload.length};
        CHECK(mo_store_call(store, MO_STORE_APPEND, &log, NULL, NULL) == MO_STORE_OK);
        mo_buffer_free(&payload);
    }
    struct mo_store_record state = {MO_STORE_FAILURE_STATE, 0, 5, host_canary, sizeof host_canary};
    struct mo_store_record message = {MO_STORE_FAILURE_MESSAGE, 0, 5, host_canary,
                                      sizeof host_canary};
    uint64_t id = 0;
    CHECK(mo_store_capture(store, "wipe", &state, &message, &id) == MO_STORE_CAPTURE_OK);
    mo_store_close(store);
}

static void damage_log(const char *path) {
    sqlite3 *db;
    sqlite3_blob *blob;
    MO_ASSERT(sqlite3_open(path, &db) == SQLITE_OK);
    MO_ASSERT(sqlite3_blob_open(db, "main", "log", "record", 1, 1, &blob) == SQLITE_OK);
    int n = sqlite3_blob_bytes(blob);
    uint8_t byte;
    MO_ASSERT(n > 0 && sqlite3_blob_read(blob, &byte, 1, n - 1) == SQLITE_OK);
    byte ^= 0x40;
    MO_ASSERT(sqlite3_blob_write(blob, &byte, 1, n - 1) == SQLITE_OK);
    MO_ASSERT(sqlite3_blob_close(blob) == SQLITE_OK && sqlite3_close(db) == SQLITE_OK);
}

/* Host copies of saved state/messages are confidential arenas: live canary, wiped before
 * free. An ordinary arena is the negative control. Each init site is a separate free. */
static void history_host_copies(void) {
    paint_canary();
    char dir[] = "/tmp/mo-history-wipe-XXXXXX", path[256];
    MO_ASSERT(mkdtemp(dir) != NULL);
    MO_ASSERT(snprintf(path, sizeof path, "%s/service.sqlite", dir) > 0);
    fill_store(path, false);

    watch_copies();
    struct mo_store_history history;
    CHECK(mo_store_history_read(path, false, &history) == MO_STORE_FOLDER_OK);
    CHECK(history.snapshot.bytes != NULL && history.snapshot.length == sizeof host_canary &&
          memcmp(history.snapshot.bytes, host_canary, sizeof host_canary) == 0);
    canary_leftovers = 0;
    mo_store_history_free(&history);
    unsigned leftover = canary_leftovers;
    unwatch_copies();
    CHECK(leftover == 0);

    watch_copies();
    struct mo_store_history_pair pair;
    CHECK(mo_store_history_pair(path, false, 1, &pair) == MO_STORE_FOLDER_OK);
    CHECK(pair.found && pair.state.bytes != NULL && pair.state.length == sizeof host_canary &&
          memcmp(pair.state.bytes, host_canary, sizeof host_canary) == 0);
    canary_leftovers = 0;
    mo_store_history_pair_free(&pair);
    leftover = canary_leftovers;
    unwatch_copies();
    CHECK(leftover == 0);

    CHECK(unlink(path) == 0);
    fill_store(path, true);
    damage_log(path);
    watch_copies();
    CHECK(mo_store_history_read(path, false, &history) == MO_STORE_FOLDER_DAMAGED);
    leftover = canary_leftovers;
    unwatch_copies();
    CHECK(leftover == 0);

    FILE *file = fopen(path, "wb");
    static const char garbage[64] = "not a store";
    MO_ASSERT(file != NULL && fwrite(garbage, 1, sizeof garbage, file) == sizeof garbage &&
              fclose(file) == 0);
    watch_copies();
    CHECK(mo_store_history_pair(path, false, 1, &pair) == MO_STORE_FOLDER_DAMAGED);
    leftover = canary_leftovers;
    unwatch_copies();
    CHECK(leftover == 0);
    CHECK(unlink(path) == 0 && rmdir(dir) == 0);

    /* After the confidential frees, so an unwiped ordinary chunk cannot be reused. */
    watch_copies();
    struct mo_arena ordinary;
    mo_arena_init(&ordinary, HOST_CANARY_BYTES + MO_ARENA_CHUNK_HEADER);
    uint8_t *copy = mo_arena_copy(&ordinary, host_canary, sizeof host_canary, 1);
    MO_ASSERT(copy != NULL);
    CHECK(memcmp(copy, host_canary, sizeof host_canary) == 0);
    mo_arena_free(&ordinary);
    leftover = canary_leftovers;
    unwatch_copies();
    CHECK(leftover > 0);
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
    export_storage(image);
    history_host_copies();
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
