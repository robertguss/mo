/* CBMC proof of lexical path containment, src/host/files.c mo_host_normalize (P8).
 *
 * Real and fake Files operations run this before they open or look up anything.
 * Architecture section 7 ("Files") and sandbox-properties P8: a path is split on '/',
 * '.' is skipped, '..' removes the preceding component, and absolute paths, empty
 * components and a '..' that would leave the root are refused. The proof checks that
 * for every input of up to MO_CBMC_PATH bytes over {'.', '/', 'a'}. Other bytes
 * are component characters like 'a': the function only special-cases '.' and '/'.
 *
 * files.c is included rather than linked, so the proof runs the real function. Heap
 * neighbours that the rest of the file calls are unused from this harness. */
#define MO_CBMC_NORMALIZE
#include "host/files.c"

#include <stdlib.h>

#pragma CPROVER check push
#pragma CPROVER check disable "bounds"
#pragma CPROVER check disable "pointer"
#pragma CPROVER check disable "pointer-overflow"
#pragma CPROVER check disable "pointer-primitive"
#pragma CPROVER check disable "signed-overflow"
#pragma CPROVER check disable "conversion"
#pragma CPROVER check disable "undefined-shift"
#pragma CPROVER check disable "div-by-zero"

#ifndef MO_CBMC_PATH
#define MO_CBMC_PATH 6
#endif
#define SPAN MO_CBMC_PATH

size_t nondet_size(void);

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

static const uint8_t *path_bytes;
static size_t path_length;

const uint8_t *mo_value_data(struct mo_cell value, size_t *length) {
    (void)value;
    *length = path_length;
    return path_bytes;
}

/* Architecture section 7: split on '/', refuse empty components, skip '.', pop on '..'. */
static bool spec_normalize(const uint8_t *bytes, size_t length, char *out, size_t *size) {
    size_t starts[SPAN], lengths[SPAN], depth = 0, at = 0;
    if (length == 0)
        return false;
    for (;;) {
        size_t end = at;
        while (end < length && bytes[end] != '/')
            end++;
        size_t n = end - at;
        if (n == 0)
            return false;
        if (n == 2 && bytes[at] == '.' && bytes[at + 1] == '.') {
            if (depth == 0)
                return false;
            depth--;
        } else if (!(n == 1 && bytes[at] == '.')) {
            starts[depth] = at;
            lengths[depth] = n;
            depth++;
        }
        if (end == length)
            break;
        at = end + 1;
    }
    size_t written = 0;
    for (size_t i = 0; i < depth; i++) {
        if (i != 0)
            out[written++] = '/';
        for (size_t k = 0; k < lengths[i]; k++)
            out[written++] = (char)bytes[starts[i] + k];
    }
    out[written] = '\0';
    *size = written;
    return true;
}

void harness_normalize(void) {
    size_t length = nondet_size();
    __CPROVER_assume(length <= SPAN);
    uint8_t *bytes = malloc(SPAN);
    __CPROVER_assume(bytes != NULL);
    for (size_t i = 0; i < SPAN; i++) {
        uint8_t b = bytes[i];
        __CPROVER_assume(b == '.' || b == '/' || b == 'a');
    }
    path_bytes = bytes;
    path_length = length;
    char *out = malloc(SPAN + 1);
    __CPROVER_assume(out != NULL);
    char *want = malloc(SPAN + 1);
    __CPROVER_assume(want != NULL);
    size_t size = nondet_size(), expected = nondet_size();
    struct mo_cell value = {{0}};
    bool allowed = spec_normalize(bytes, length, want, &expected);
    struct mo_file_error error = mo_host_normalize(value, out, &size);
    if (allowed) {
        __CPROVER_assert(error.status == FILE_OK, "normalize: accepts a contained relative path");
        __CPROVER_assert(size == expected, "normalize: writes the spec's length");
        __CPROVER_assert(size <= SPAN, "normalize: output fits the input");
        for (size_t i = 0; i < size; i++)
            __CPROVER_assert(out[i] == want[i], "normalize: writes the spec's bytes");
        __CPROVER_assert(out[size] == '\0', "normalize: NUL-terminates");
    } else {
        __CPROVER_assert(error.status == FILE_NOT_ALLOWED,
                         "normalize: refuses an empty, absolute, or escaping path");
        __CPROVER_assert(error.containment, "normalize: a refusal is a containment failure");
    }
    reached_end();
}

#pragma CPROVER check pop
