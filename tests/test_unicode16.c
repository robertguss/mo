#define _POSIX_C_SOURCE 200809L
#include "prim_fixture.h"
#include "test.h"

#include <inttypes.h>
#include <stdlib.h>

static MO_MUST_USE enum mo_primitive_id operation(char code) {
    switch (code) {
    case 'g':
        return MO_PRIM_UNICODE16_GRAPHEMES;
    case 'w':
        return MO_PRIM_UNICODE16_DISPLAY_WIDTH;
    case 'n':
        return MO_PRIM_UNICODE16_NORMALIZE_NFC;
    case 'l':
        return MO_PRIM_UNICODE16_LOWERCASE;
    case 'u':
        return MO_PRIM_UNICODE16_UPPERCASE;
    default:
        MO_UNREACHABLE();
    }
}

static void print_hex(struct mo_cell text) {
    size_t length;
    const uint8_t *bytes = mo_value_data(text, &length);
    for (size_t i = 0; i < length; i++)
        MO_ASSERT(printf("%02x", (unsigned)bytes[i]) >= 0);
}

/* Compact transport for UCD's complete conformance files, not Core's oracle.
 * Each request is an operation letter and UTF-8 bytes in lowercase hex. */
static void oracle(void) {
    struct prim_fixture f;
    prim_fixture_init(&f, MO_DEFAULT_HEAP_SIZE);
    size_t baseline = mo_heap_used(f.heap), capacity = 0;
    char *line = NULL;
    ssize_t length;
    while ((length = getline(&line, &capacity, stdin)) >= 0) {
        MO_ASSERT(length >= 3 && line[1] == ' ' && line[length - 1] == '\n');
        size_t n = (size_t)length - 3;
        MO_ASSERT(n % 2 == 0 && n <= 2 * MO_LIMIT_STRING_SIZE);
        struct mo_buffer bytes;
        mo_buffer_init(&bytes, MO_LIMIT_STRING_SIZE);
        for (size_t i = 0; i < n; i += 2) {
            unsigned value = 0;
            for (unsigned j = 0; j < 2; j++) {
                unsigned ch = (unsigned char)line[i + 2 + j];
                MO_ASSERT((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'));
                value = value * 16 + (ch <= '9' ? ch - '0' : ch - 'a' + 10);
            }
            MO_ASSERT(mo_buffer_append_byte(&bytes, (uint8_t)value));
        }
        struct mo_cell input, out;
        MO_ASSERT(mo_value_text(f.heap, (const char *)bytes.bytes, bytes.length, &input));
        mo_buffer_free(&bytes);
        enum mo_primitive_id id = operation(line[0]);
        const char *reason;
        MO_ASSERT(mo_prim_call(f.heap, &f.types, id, f.signature[id], &input, &out, &reason) ==
                  MO_PRIM_RETURNED);
        MO_ASSERT(reason == NULL);
        if (id == MO_PRIM_UNICODE16_GRAPHEMES) {
            for (uint32_t i = 0; i < mo_list_size(out); i++) {
                if (i != 0)
                    MO_ASSERT(putchar(':') != EOF);
                struct mo_cell item;
                MO_ASSERT(mo_list_get(out, i, &item));
                print_hex(item);
            }
        } else if (id == MO_PRIM_UNICODE16_DISPLAY_WIDTH)
            MO_ASSERT(printf("%" PRId64, mo_cell_get_int(out)) >= 0);
        else
            print_hex(out);
        MO_ASSERT(putchar('\n') != EOF);
        mo_value_drop(f.heap, f.types.items[f.signature[id]].result, out);
        if (id == MO_PRIM_UNICODE16_GRAPHEMES || id == MO_PRIM_UNICODE16_DISPLAY_WIDTH)
            mo_value_drop(f.heap, f.builtin[MO_TYPE_TEXT], input);
        MO_ASSERT(mo_heap_objects(f.heap) == 0 && mo_heap_used(f.heap) == baseline);
    }
    MO_ASSERT(feof(stdin) && !ferror(stdin));
    free(line);
    prim_fixture_free(&f);
}

static void graphemes(void) {
    struct prim_fixture f;
    prim_fixture_init(&f, 1 << 20);
    const char text[] = "a\xcc\x88\r\n\xf0\x9f\x87\xba\xf0\x9f\x87\xb8";
    struct mo_cell input, out = mo_cell_unit();
    MO_ASSERT(mo_value_text(f.heap, text, sizeof text - 1, &input));
    const char *reason;
    enum mo_prim_status status =
        mo_prim_call(f.heap, &f.types, MO_PRIM_UNICODE16_GRAPHEMES,
                     f.signature[MO_PRIM_UNICODE16_GRAPHEMES], &input, &out, &reason);
    CHECK(status == MO_PRIM_RETURNED);
    if (status == MO_PRIM_RETURNED) {
        CHECK(mo_list_size(out) == 3);
        const size_t starts[] = {0, 3, 5, sizeof text - 1};
        for (uint32_t i = 0; i < 3; i++) {
            struct mo_cell item;
            MO_ASSERT(mo_list_get(out, i, &item));
            size_t length;
            const uint8_t *bytes = mo_value_data(item, &length);
            CHECK(length == starts[i + 1] - starts[i] &&
                  memcmp(bytes, text + starts[i], length) == 0);
        }
        mo_value_drop(f.heap, f.types.items[f.signature[MO_PRIM_UNICODE16_GRAPHEMES]].result, out);
    }
    mo_value_drop(f.heap, f.builtin[MO_TYPE_TEXT], input);
    CHECK(mo_heap_objects(f.heap) == 0);
    prim_fixture_free(&f);
}

static void mappings(void) {
    struct prim_fixture f;
    prim_fixture_init(&f, 1 << 20);
    const struct {
        char op;
        const char *text, *want;
    } cases[] = {
        {'u', "Straße ﬃ", "STRASSE FFI"},
        {'l', "ΟΣΟΣ", "οσος"},
        {'l', "ΟΣ'Α", "οσ'α"},
        {'l', "ΟΣ'", "ος'"},
        {'l', "Σ", "σ"},
        {'l', "İI", "i\xcc\x87i"},
        {'n', "a\xcc\x95\xcc\x80", "à\xcc\x95"},
        {'n', "\xcc\x95\xcc\x80", "\xcc\x80\xcc\x95"},
        {'n', "A\xcc\x85\xcc\x8a", "A\xcc\x85\xcc\x8a"},
        {'n', "각", "각"},
        {'n', "\xcd\x84", "\xcc\x88\xcc\x81"},
        {'n', "", ""},
        {'l', "", ""},
        {'u', "", ""},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        struct mo_cell input, out;
        MO_ASSERT(mo_value_text(f.heap, cases[i].text, strlen(cases[i].text), &input));
        enum mo_primitive_id id = operation(cases[i].op);
        const char *reason;
        MO_ASSERT(mo_prim_call(f.heap, &f.types, id, f.signature[id], &input, &out, &reason) ==
                  MO_PRIM_RETURNED);
        size_t length;
        const uint8_t *bytes = mo_value_data(out, &length);
        CHECK(reason == NULL && length == strlen(cases[i].want) &&
              memcmp(bytes, cases[i].want, length) == 0);
        mo_value_drop(f.heap, f.builtin[MO_TYPE_TEXT], out);
        CHECK(mo_heap_objects(f.heap) == 0);
    }
    prim_fixture_free(&f);
}

static void widths(void) {
    struct prim_fixture f;
    prim_fixture_init(&f, 1 << 20);
    const struct {
        const char *text;
        int64_t width;
    } cases[] = {
        {"", 0},
        {"abc", 3},
        {"a\xcc\x88", 1},
        {"\xcc\x88", 0},
        {"\xe2\x80\x8b", 0},
        {"中Ａ", 4},
        {"각", 2},
        {"क्\u0937", 1},
        {"©", 1},
        {"©\ufe0f", 2},
        {"©\ufe0e", 1},
        {"1\u20e3", 2},
        {"1\ufe0f\u20e3", 2},
        {"🇺🇸", 2},
        {"🇺🇸🇨", 4},
        {"👩‍👩‍👧‍👦", 2},
        {"\r\n", 1},
        {"\t", 1},
        {"\xc2\xa1", 1},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        struct mo_cell input, out;
        MO_ASSERT(mo_value_text(f.heap, cases[i].text, strlen(cases[i].text), &input));
        const char *reason;
        MO_ASSERT(mo_prim_call(f.heap, &f.types, MO_PRIM_UNICODE16_DISPLAY_WIDTH,
                               f.signature[MO_PRIM_UNICODE16_DISPLAY_WIDTH], &input, &out,
                               &reason) == MO_PRIM_RETURNED);
        CHECK(reason == NULL && mo_cell_get_int(out) == cases[i].width);
        mo_value_drop(f.heap, f.builtin[MO_TYPE_TEXT], input);
    }
    prim_fixture_free(&f);
}

static void refusal(void) {
    struct prim_fixture f;
    prim_fixture_init(&f, 4096);
    uint8_t text[2048];
    memset(text, 'a', sizeof text);
    struct mo_cell input;
    MO_ASSERT(mo_value_text(f.heap, (const char *)text, sizeof text, &input));
    struct mo_cell before = input;
    size_t used = mo_heap_used(f.heap);
    uint32_t references = mo_cell_get_ref(input)->count;
    for (unsigned i = MO_PRIM_UNICODE16_GRAPHEMES; i <= MO_PRIM_UNICODE16_UPPERCASE; i++) {
        if (i == MO_PRIM_UNICODE16_DISPLAY_WIDTH)
            continue;
        struct mo_cell out = mo_cell_int(41);
        const char *reason;
        CHECK(mo_prim_call(f.heap, &f.types, (enum mo_primitive_id)i, f.signature[i], &input, &out,
                           &reason) == MO_PRIM_OUT_OF_MEMORY);
        CHECK(reason != NULL && strcmp(reason, "out of memory") == 0);
        CHECK(mo_cell_get_int(out) == 41 && memcmp(&input, &before, sizeof input) == 0);
        size_t length;
        const uint8_t *bytes = mo_value_data(input, &length);
        CHECK(length == sizeof text && memcmp(bytes, text, length) == 0);
        CHECK(mo_heap_used(f.heap) == used && mo_cell_get_ref(input)->count == references);
    }
    mo_value_drop(f.heap, f.builtin[MO_TYPE_TEXT], input);
    CHECK(mo_heap_objects(f.heap) == 0);
    prim_fixture_free(&f);
}

/* Repeated RI and reverse-ordered combining runs distinguish suffix-state
 * segmentation and counting sort from quadratic rescans/insertion sort. */
static void work(void) {
    struct prim_fixture f;
    prim_fixture_init(&f, MO_DEFAULT_HEAP_SIZE);
    size_t baseline = mo_heap_used(f.heap);
    struct mo_buffer text;
    mo_buffer_init(&text, MO_LIMIT_STRING_SIZE);
    const size_t count = 131072;
    for (size_t i = 0; i < count; i++)
        MO_ASSERT(mo_buffer_append(&text, "\xf0\x9f\x87\xa6", 4));
    struct mo_cell input, out;
    MO_ASSERT(mo_value_text(f.heap, (const char *)text.bytes, text.length, &input));
    const char *reason;
    MO_ASSERT(mo_prim_call(f.heap, &f.types, MO_PRIM_UNICODE16_GRAPHEMES,
                           f.signature[MO_PRIM_UNICODE16_GRAPHEMES], &input, &out,
                           &reason) == MO_PRIM_RETURNED);
    CHECK(mo_list_size(out) == count / 2);
    struct mo_cell item;
    MO_ASSERT(mo_list_get(out, (uint32_t)(count / 2 - 1), &item));
    size_t length;
    const uint8_t *bytes = mo_value_data(item, &length);
    CHECK(length == 8 && memcmp(bytes, text.bytes, 8) == 0);
    mo_value_drop(f.heap, f.types.items[f.signature[MO_PRIM_UNICODE16_GRAPHEMES]].result, out);
    mo_value_drop(f.heap, f.builtin[MO_TYPE_TEXT], input);
    text.length = 0;
    MO_ASSERT(mo_buffer_append_byte(&text, 'a'));
    for (size_t i = 0; i < count; i++)
        MO_ASSERT(mo_buffer_append(&text, "\xcc\x95", 2));
    for (size_t i = 0; i < count; i++)
        MO_ASSERT(mo_buffer_append(&text, "\xcc\x80", 2));
    MO_ASSERT(mo_value_text(f.heap, (const char *)text.bytes, text.length, &input));
    MO_ASSERT(mo_prim_call(f.heap, &f.types, MO_PRIM_UNICODE16_NORMALIZE_NFC,
                           f.signature[MO_PRIM_UNICODE16_NORMALIZE_NFC], &input, &out,
                           &reason) == MO_PRIM_RETURNED);
    bytes = mo_value_data(out, &length);
    CHECK(length == count * 4 && bytes[0] == 0xc3 && bytes[1] == 0xa0);
    for (size_t i = 2; i < length; i += 2)
        CHECK(bytes[i] == 0xcc && bytes[i + 1] == (i < count * 2 ? 0x80 : 0x95));
    mo_value_drop(f.heap, f.builtin[MO_TYPE_TEXT], out);
    mo_buffer_free(&text);
    CHECK(mo_heap_objects(f.heap) == 0 && mo_heap_used(f.heap) == baseline);
    prim_fixture_free(&f);
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--oracle") == 0) {
        oracle();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--work") == 0) {
        work();
        return mo_test_report("Unicode16 work");
    }
    MO_ASSERT(argc == 1);
    graphemes();
    mappings();
    widths();
    refusal();
    return mo_test_report("Unicode16");
}
