/* Diagnostics, and the source files they point into (architecture sections 3.1 and 3.5).
 *
 * Every stage reports into one struct mo_diags: a code from src/base/codes.h, a headline and a
 * reason in the reference's words, the section the reason cites, a primary span with a label,
 * secondary spans with labels, and fixes, each one or more text edits. Spans hold byte offsets;
 * lines and columns are computed only when a message is shown, from the files in struct
 * mo_sources. Showing diagnostics, as the text of reference appendix A or as JSON for mo check
 * --json, is src/tools's work. */
#ifndef MO_DIAG_H
#define MO_DIAG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/arena.h"
#include "base/base.h"

/* A piece of a source file, from byte start up to byte end, not included. A span with
 * start == end is the place between two bytes, such as the end of a file. */
struct mo_span {
    uint32_t file; /* an index into struct mo_sources */
    uint32_t start;
    uint32_t end;
};

/* A span to mark, and the text shown beside it, which may be NULL for none. */
struct mo_label {
    struct mo_span span;
    const char *text;
};

/* A text edit: the bytes of span are replaced with replacement, which may be empty to delete them.
 * An empty span inserts. */
struct mo_edit {
    struct mo_span span;
    const char *replacement;
};

/* A fix: edit_count edits, at least one and at most MO_LIMIT_FIX_EDITS, applied together or not
 * at all, each against the original text. So they must not overlap: no two may change a byte in
 * common, or insert at the same place, where their order would be unclear. An insertion may sit
 * at either end of a span another edit replaces. */
struct mo_fix {
    const char *title; /* what it does, such as "pass the failure upward with try"; may be NULL */
    const struct mo_edit *edits;
    uint32_t edit_count;
};

/* One error, as reference appendix A shows it. Mo has no warnings: a problem is an error, or it
 * isn't reported. */
struct mo_diag {
    uint16_t code;                    /* such as 412 for E0412, registered in src/base/codes.h */
    struct mo_label primary;          /* where the error is */
    const struct mo_label *secondary; /* secondary_count other places it involves */
    uint32_t secondary_count;
    /* The headline, appendix A's first line, in the reference's words: "a failure is ignored". */
    const char *message;
    /* One sentence that says why, citing the rule's section, such as "Mo never drops a failure
     * silently (reference 8.3)." NULL only for the codes of limits, which cite no section. */
    const char *reason;
    /* The reference section the reason cites, such as "8.3": always its code's, from
     * src/base/codes.h, and NULL for the codes of limits. mo_diags_add fills it in. */
    const char *section;
    /* fix_count fixes, each one way to fix the error: mo fix applies the fix of a diagnostic that
     * has exactly one. */
    const struct mo_fix *fixes;
    uint32_t fix_count;
};

/* The diagnostics of a run. They own every string, label and fix they hold, in their own arena,
 * so what a stage passes to mo_diags_add may go away after the call. */
struct mo_diags {
    struct mo_arena arena;
    struct mo_diag *items; /* count of them, in the order they were added until sorted */
    uint32_t count;
    uint32_t dropped; /* how many mo_diags_add refused */
};

/* Prepares an empty set of diagnostics, with room for MO_LIMIT_DIAGNOSTICS of them. False when the
 * system has no memory. */
MO_MUST_USE bool mo_diags_init(struct mo_diags *diags);

/* Frees the diagnostics and everything they hold. */
void mo_diags_free(struct mo_diags *diags);

/* Adds a copy of diag, with copies of its strings, labels, fixes and edits, in the order given, and
 * its code's section. Its code must be registered, its section NULL or its code's, its reason NULL
 * only if its code cites no section, and each fix as struct mo_fix says.
 *
 * Past MO_LIMIT_DIAGNOSTICS diagnostics or MO_LIMIT_DIAGNOSTICS_SIZE bytes, or when the system has
 * no memory, it adds nothing, takes no memory, counts the diagnostic in dropped, and returns false.
 * The stage should then stop, since it may not be able to report anything more. */
MO_MUST_USE bool mo_diags_add(struct mo_diags *diags, const struct mo_diag *diag);

/* Whether any diagnostic was reported, whether it was kept or dropped. */
MO_MUST_USE bool mo_diags_any(const struct mo_diags *diags);

/* Sorts the diagnostics by position: by file, in the order the files were added to struct
 * mo_sources, then by where the primary span starts. Diagnostics at the same place keep the order
 * they were added in. */
void mo_diags_sort(struct mo_diags *diags);

/* A source file, as it was read. */
struct mo_source {
    const char *path;     /* as given to mo_sources_add */
    const uint8_t *bytes; /* length bytes, then a NUL that isn't part of the file */
    uint32_t length;
    uint32_t line_count;         /* at least 1 */
    const uint32_t *line_starts; /* line_count offsets: where each line starts, 0 first */
};

/* The source files of a program, indexed by mo_span.file. They own their paths, bytes and line
 * tables, in their own arena. */
struct mo_sources {
    struct mo_arena arena;
    struct mo_source *files; /* count of them */
    uint32_t count;
    size_t size; /* bytes of all of them together */
};

/* Why mo_sources_add refused a file. Each limit has an error code for the stage that reports it. */
enum mo_source_problem {
    MO_SOURCE_UNREADABLE,      /* the system couldn't open or read it; system_error says why */
    MO_SOURCE_TOO_BIG,         /* more than MO_LIMIT_FILE_SIZE bytes: E1001 */
    MO_SOURCE_PROGRAM_TOO_BIG, /* the files would be more than MO_LIMIT_PROGRAM_SIZE bytes: E1002 */
    MO_SOURCE_TOO_MANY,        /* there would be more than MO_LIMIT_FILES files: E1003 */
    MO_SOURCE_PATH_TOO_LONG,   /* the path is more than MO_LIMIT_PATH_SIZE bytes: E1004 */
    MO_SOURCE_NO_MEMORY,       /* the system has no memory */
};

struct mo_source_error {
    enum mo_source_problem problem;
    int system_error; /* errno, for MO_SOURCE_UNREADABLE if the system gave one; 0 otherwise */
};

/* Prepares an empty set of source files, with room for MO_LIMIT_FILES of them. False when the
 * system has no memory. */
MO_MUST_USE bool mo_sources_init(struct mo_sources *sources);

/* Frees the files, their paths, bytes and line tables. */
void mo_sources_free(struct mo_sources *sources);

/* Reads the file at path, within the limits, and keeps its path and bytes. Sets *file_out to its
 * index, the number of files added before it. On failure it adds nothing, takes no memory, and
 * says why in *error_out. */
MO_MUST_USE bool mo_sources_add(struct mo_sources *sources, const char *path, uint32_t *file_out,
                                struct mo_source_error *error_out);

/* Does the same for bytes in memory, such as a test's or a fuzzer's, named by path. It copies
 * them. */
MO_MUST_USE bool mo_sources_add_bytes(struct mo_sources *sources, const char *path,
                                      const uint8_t *bytes, size_t length, uint32_t *file_out,
                                      struct mo_source_error *error_out);

/* A place in a source file as messages show it: a line and a column, both counted from 1. */
struct mo_position {
    uint32_t line;
    uint32_t column;
};

/* The line and column of a byte offset in a file, from 0 up to the file's length.
 *
 * Each "\n" ends a line, and a "\r" just before it is part of the line ending, so "\n" and "\r\n"
 * both end lines. A "\r" alone stays in its line, as Text.lines treats it (reference 14). The
 * place after a final line ending is on a line of its own, with nothing in it.
 *
 * Columns count characters, as the executable grammar does (grammar/lexer.c): a character takes
 * one column, whatever the length of its UTF-8, and so does a tab, and a byte that isn't part of
 * valid UTF-8. An offset inside a character, or inside a "\r\n", gives that character's column. */
MO_MUST_USE struct mo_position mo_sources_position(const struct mo_sources *sources, uint32_t file,
                                                   uint32_t offset);

/* The text of a line, counted from 1 up to the file's line_count, without its line ending. */
MO_MUST_USE struct mo_span mo_sources_line(const struct mo_sources *sources, uint32_t file,
                                           uint32_t line);

#endif
