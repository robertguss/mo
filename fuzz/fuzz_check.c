#define _POSIX_C_SOURCE 200809L

#include "base/codes.h"
#include "check/check.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static char root[] = "/tmp/mo-fuzz-check-XXXXXX";
static char entry[128], library[128], library_file[160];
static bool ready;

static void cleanup(void) {
    MO_ASSERT(unlink(entry) == 0);
    MO_ASSERT(unlink(library_file) == 0);
    MO_ASSERT(rmdir(library) == 0);
    MO_ASSERT(rmdir(root) == 0);
}

static void write_file(const char *path, const uint8_t *data, size_t size) {
    FILE *file = fopen(path, "wb");
    MO_ASSERT(file != NULL);
    MO_ASSERT(fwrite(data, 1, size, file) == size);
    MO_ASSERT(fclose(file) == 0);
}

static void setup(void) {
    if (ready)
        return;
    MO_ASSERT(mkdtemp(root) != NULL);
    int a = snprintf(entry, sizeof(entry), "%s/main.mo", root);
    int b = snprintf(library, sizeof(library), "%s/lib", root);
    int c = snprintf(library_file, sizeof(library_file), "%s/library.mo", library);
    MO_ASSERT(a > 0 && (size_t)a < sizeof(entry));
    MO_ASSERT(b > 0 && (size_t)b < sizeof(library));
    MO_ASSERT(c > 0 && (size_t)c < sizeof(library_file));
    MO_ASSERT(mkdir(library, 0700) == 0);
    const char text[] = "module Library\nexpose Item(..), Choice(..), read\n"
                        "struct Item\n  value: Int\nend\n"
                        "enum Choice\n  Fresh\n  Old\nend\n"
                        "fn read(item: Item) : Int\n  item.value\nend\n";
    write_file(library_file, (const uint8_t *)text, sizeof(text) - 1);
    MO_ASSERT(atexit(cleanup) == 0);
    ready = true;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > (1 << 16))
        return 0;
    setup();
    write_file(entry, data, size);
    struct mo_sources sources;
    struct mo_diags diags;
    struct mo_arena arena;
    MO_ASSERT(mo_sources_init(&sources));
    MO_ASSERT(mo_diags_init(&diags));
    size_t limit = size != 0 && (data[size - 1] & 7) == 0 ? size * (1u + data[0]) : 16 << 20;
    mo_arena_init(&arena, limit);
    struct mo_module_set *set = mo_load(&arena, &sources, entry, library, &diags);
    MO_ASSERT(set != NULL ? !mo_diags_any(&diags) : mo_diags_any(&diags));
    if (set != NULL) {
        struct mo_module_interfaces *first = mo_resolve_modules(&arena, set, &diags);
        MO_ASSERT(first != NULL ? !mo_diags_any(&diags) : mo_diags_any(&diags));
        struct mo_arena second_arena;
        struct mo_diags second_diags;
        mo_arena_init(&second_arena, 16 << 20);
        MO_ASSERT(mo_diags_init(&second_diags));
        struct mo_module_interfaces *second = mo_resolve_modules(&second_arena, set, &second_diags);
        if (first == NULL && diags.items[0].code == MO_CODE_CHECK_NO_MEMORY) {
            /* The loader used part of the first arena; a fresh arena can have room. */
            MO_ASSERT(mo_diags_any(&diags));
        } else {
            MO_ASSERT((first == NULL) == (second == NULL));
            if (first == NULL) {
                MO_ASSERT(diags.count == second_diags.count);
                MO_ASSERT(diags.items[0].code == second_diags.items[0].code);
                MO_ASSERT(diags.items[0].primary.span.start ==
                          second_diags.items[0].primary.span.start);
            } else {
                for (uint32_t i = 0; i < first->count; i++) {
                    const struct mo_module_scope *a = &first->scopes[i], *b = &second->scopes[i];
                    MO_ASSERT(a->count == b->count && a->imported_count == b->imported_count &&
                              a->alias_count == b->alias_count);
                    for (uint32_t j = 0; j < a->count; j++) {
                        MO_ASSERT(strcmp(a->symbols[j].name, b->symbols[j].name) == 0);
                        MO_ASSERT(a->symbols[j].declaration == b->symbols[j].declaration);
                        MO_ASSERT(a->symbols[j].owner == b->symbols[j].owner);
                        MO_ASSERT(a->symbols[j].module == b->symbols[j].module);
                        MO_ASSERT(a->symbols[j].exported == b->symbols[j].exported);
                        MO_ASSERT(a->symbols[j].insides == b->symbols[j].insides);
                    }
                    for (uint32_t j = 0; j < a->imported_count; j++) {
                        MO_ASSERT(strcmp(a->imported[j].name, b->imported[j].name) == 0);
                        MO_ASSERT(a->imported[j].count == b->imported[j].count);
                        MO_ASSERT(a->imported[j].origin == b->imported[j].origin);
                        for (uint32_t k = 0; k < a->imported[j].count; k++)
                            MO_ASSERT(a->imported[j].symbols[k].declaration ==
                                      b->imported[j].symbols[k].declaration);
                    }
                    for (uint32_t j = 0; j < a->alias_count; j++) {
                        MO_ASSERT(strcmp(a->aliases[j].name, b->aliases[j].name) == 0);
                        MO_ASSERT(a->aliases[j].module == b->aliases[j].module);
                        MO_ASSERT(a->aliases[j].origin == b->aliases[j].origin);
                    }
                }
            }
        }
        mo_diags_free(&second_diags);
        mo_arena_free(&second_arena);
    }
    mo_arena_free(&arena);
    mo_diags_free(&diags);
    mo_sources_free(&sources);
    return 0;
}
