#include "base/export.h"
#include "tools/protocol.h"
#include "tools/tools.h"
#include "vm/vm.h"
#include <stdio.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
static struct mo_image *image;
static uint32_t function;

static bool consume(const uint8_t *data, size_t size, struct mo_arena *arena,
                    struct mo_export_consumer *consumer) {
    if (size > MO_LIMIT_MCP_HEADER_BYTES + MO_LIMIT_MCP_RESPONSE_BYTES)
        return false;
    struct mo_buffer wire;
    mo_buffer_init(&wire, MO_LIMIT_MCP_HEADER_BYTES + MO_LIMIT_MCP_RESPONSE_BYTES);
    struct mo_export_page page;
    bool accepted = mo_buffer_append(&wire, data, size) &&
                    mo_fetch_read(image, function, &wire, &page) == MO_FETCH_PAGE;
    uint8_t first_id[16];
    memcpy(first_id, consumer->id, 16);
    uint64_t sequence = consumer->sequence, encoded_sequence = consumer->encoded_sequence;
    size_t before = consumer->offset;
    accepted = accepted && mo_export_accept(arena, consumer, &page);
    if (accepted) {
        MO_ASSERT(consumer->offset == before + page.length);
        MO_ASSERT(consumer->offset <= consumer->total);
        if (before != 0) {
            MO_ASSERT(memcmp(first_id, page.id, 16) == 0);
            MO_ASSERT(sequence == page.sequence && encoded_sequence == page.encoded_sequence);
        }
        if (consumer->complete) {
            struct mo_export_identity identity;
            MO_ASSERT(mo_export_verify(consumer->bytes, consumer->total, &identity));
            MO_ASSERT(memcmp(identity.id, consumer->id, 16) == 0);
        }
    }
    mo_buffer_free(&wire);
    return accepted;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (image == NULL) {
        image = mo_protocol_build();
        MO_ASSERT(image != NULL);
        struct mo_limits limits = mo_limits_default();
        struct mo_defect defect;
        MO_ASSERT(mo_image_init(image, &limits, &defect) == MO_RETURNED);
        const struct mo_bytecode *file = mo_image_bytecode(image);
        function = MO_NO_INDEX;
        for (uint32_t i = 0; i < file->function_count; i++) {
            const struct mo_bytecode_string *name = &file->strings[file->functions[i].name];
            if (name->length == 13 && memcmp(name->bytes, "McpFetch.read", 13) == 0)
                function = i;
        }
        MO_ASSERT(function != MO_NO_INDEX);
    }
    static FILE *errors;
    if (errors == NULL) {
        errors = fopen("/dev/null", "w");
        MO_ASSERT(errors != NULL);
    }
    char address[MO_LIMIT_FETCH_URL_BYTES + 2];
    size_t address_size = size < sizeof address - 1 ? size : sizeof address - 1;
    if (address_size != 0)
        memcpy(address, data, address_size);
    address[address_size] = 0;
    struct mo_cli cli = {.fetch_url = address, .output = ".", .fetch_token = "UNREAD"};
    int result = mo_fetch(&cli, errors);
    MO_ASSERT(result == 1 || result == 2);
    struct mo_arena arena;
    mo_arena_init_confidential(&arena, MO_LIMIT_EXPORT_BYTES);
    struct mo_export_consumer consumer = {0};
    if (size > 0 && data[0] == 255) {
        size_t at = 1;
        /* Four pages reach continuation, completion and generation replacement paths. */
        for (unsigned page = 0; page < 4 && size - at >= 4; page++) {
            uint32_t length = 0;
            for (unsigned i = 0; i < 4; i++)
                length |= (uint32_t)data[at++] << (8 * i);
            if (length > size - at || !consume(data + at, length, &arena, &consumer))
                break;
            at += length;
        }
    } else {
        bool accepted = consume(data, size, &arena, &consumer);
        (void)accepted;
    }
    mo_arena_free(&arena);
    struct mo_export_identity identity;
    bool artifact = mo_export_verify(data, size, &identity);
    if (artifact)
        MO_ASSERT(identity.kind == 1 && size >= MO_EXPORT_FIXED_BYTES);
    return 0;
}
