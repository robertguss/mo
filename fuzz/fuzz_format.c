/* Reparse, compare trees modulo +=, and format twice. Invalid source is not
 * a formatter input. Width varies independently of the source's layout. */
#include "../tests/format-contract.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

/* Test-only stand-in until the allocation-free Unicode16 helper lands. */
static uint32_t character_width(const uint8_t *bytes, size_t length) {
    uint32_t width = 0;
    for (size_t i = 0; i < length; i++)
        if ((bytes[i] & 0xc0) != 0x80)
            width++;
    return width;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > (1 << 16))
        return 0;
    struct mo_sources sources;
    struct mo_diags diags;
    struct mo_arena arena;
    MO_ASSERT(mo_sources_init(&sources));
    MO_ASSERT(mo_diags_init(&diags));
    mo_arena_init(&arena, 1 << 27);
    uint32_t file;
    struct mo_source_error error;
    MO_ASSERT(mo_sources_add_bytes(&sources, "fuzz.mo", data, size, &file, &error));
    struct mo_module_syntax *original = mo_parse_fragment(&arena, &sources, file, &diags);
    struct mo_buffer output, twice, before, after;
    mo_buffer_init(&output, MO_LIMIT_FILE_SIZE);
    mo_buffer_init(&twice, MO_LIMIT_FILE_SIZE);
    mo_buffer_init(&before, 1 << 27);
    mo_buffer_init(&after, 1 << 27);
    if (original != NULL && !mo_diags_any(&diags)) {
        struct mo_format_options options = {
            MO_FORMAT_WIDTH, size == 0 ? 100 : 20u + data[size - 1] % 81u, character_width};
        bool rendered = mo_format(original, &sources, options, &output);
        if (rendered) {
            MO_ASSERT(mo_sources_add_bytes(&sources, "formatted.mo", output.bytes, output.length,
                                           &file, &error));
            struct mo_module_syntax *formatted = mo_parse_fragment(&arena, &sources, file, &diags);
            MO_ASSERT(formatted != NULL && !mo_diags_any(&diags));
            format_tree(&before, original->tree);
            format_tree(&after, formatted->tree);
            MO_ASSERT(format_buffer_equal(&before, &after));
            MO_ASSERT(mo_format(formatted, &sources, options, &twice));
            MO_ASSERT(format_buffer_equal(&output, &twice));
        } else
            MO_ASSERT(output.length == 0);
    }
    mo_buffer_free(&output);
    mo_buffer_free(&twice);
    mo_buffer_free(&before);
    mo_buffer_free(&after);
    mo_arena_free(&arena);
    mo_diags_free(&diags);
    mo_sources_free(&sources);
    return 0;
}
