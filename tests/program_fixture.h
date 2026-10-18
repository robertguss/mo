/* The checked-program fixture shared by test_program.c and the allocation-failure
 * tests: a module parsed from text and checked into a program. */
#ifndef MO_TEST_PROGRAM_FIXTURE_H
#define MO_TEST_PROGRAM_FIXTURE_H

#include "base/limits.h"
#include "check/check.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct fixture {
    struct mo_arena syntax, arena;
    struct mo_sources sources;
    struct mo_diags diags;
    struct mo_module_syntax *module;
    uint32_t order, *uses;
    struct mo_module_set set;
    struct mo_program *program;
    char folder[64], path[96]; /* init_with_library's files, empty otherwise */
};

static void init(struct fixture *f, const char *text) {
    *f = (struct fixture){0};
    mo_arena_init(&f->syntax, 64 << 20);
    mo_arena_init(&f->arena, MO_CLI_CHECK_SIZE);
    MO_ASSERT(mo_sources_init(&f->sources) && mo_diags_init(&f->diags));
    uint32_t file;
    struct mo_source_error error;
    MO_ASSERT(mo_sources_add_bytes(&f->sources, "main.mo", (const uint8_t *)text, strlen(text),
                                   &file, &error));
    f->module = mo_parse(&f->syntax, &f->sources, file, &f->diags);
    MO_ASSERT(f->module != NULL && !mo_diags_any(&f->diags));
    f->set = (struct mo_module_set){.count = 1,
                                    .modules = &f->module,
                                    .order = &f->order,
                                    .uses = &f->uses,
                                    .sources = &f->sources};
    f->program = mo_check(&f->arena, &f->set, &f->diags);
}

static void free_fixture(struct fixture *f) {
    if (f->path[0] != '\0')
        MO_ASSERT(unlink(f->path) == 0 && rmdir(f->folder) == 0);
    mo_arena_free(&f->arena);
    mo_arena_free(&f->syntax);
    mo_diags_free(&f->diags);
    mo_sources_free(&f->sources);
}

#endif
