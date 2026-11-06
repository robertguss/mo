/* What mo replay, mo query and mo history share: reading their inputs, checking a record's
 * identities, and writing values and records as text or JSON. */
#ifndef MO_TOOLS_OFFLINE_H
#define MO_TOOLS_OFFLINE_H

#include "base/export.h"
#include "base/verdict.h"
#include "host/host.h"
#include "tools/tools.h"

struct mo_image;

/* The embedded views' saved-schema renderer: values as JSON without the program's types. */
struct mo_offline_renderer {
    struct mo_image *image;
    uint32_t function;
};
MO_MUST_USE bool mo_offline_renderer_open(struct mo_offline_renderer *renderer);
void mo_offline_renderer_close(struct mo_offline_renderer *renderer);
/* Appends `{"value":V,"complete":B}` for one saved value of length bytes. V is past
 * MO_LIMIT_OFFLINE_VALUE_BYTES marked truncated, as the live views mark it, and null with
 * complete false when the bytes do not render. False only when out cannot grow. */
MO_MUST_USE bool mo_offline_render(struct mo_offline_renderer *renderer, const uint8_t *saved,
                                   size_t length, struct mo_buffer *out);

/* DIR/service.sqlite in out; false when it would not fit. */
MO_MUST_USE bool mo_offline_store_path(const char *folder, char *out, size_t size);
/* Why a folder could not be read, for people and as a JSON code. */
MO_MUST_USE const char *mo_offline_folder_text(enum mo_store_folder status);
MO_MUST_USE const char *mo_offline_folder_code(enum mo_store_folder status);
/* Appends what the status says about the folder, the folder shown terminal-safe. */
MO_MUST_USE bool mo_offline_folder_message(struct mo_buffer *out, const char *folder,
                                           enum mo_store_folder status, uint64_t damaged_sequence);

/* Why this build cannot replay a record, or NULL when it can: a record that does not name its
 * build (an all-zero digest), a build that cannot read its own identity, or another build. */
MO_MUST_USE const char *mo_offline_build_refusal(const struct mo_failure_record *record);
/* The 32-byte digest as 64 lowercase hex digits. */
MO_MUST_USE bool mo_offline_hex(struct mo_buffer *out, const uint8_t *bytes, size_t length);
/* The class names the live failure view uses. */
MO_MUST_USE const char *mo_offline_class(enum mo_signature_class class);
/* A signature as the live failure view writes it:
 * {"version":1,"class":...,"reason":...,"limit":...,"value":...,"function":...,"file":...,
 * "start":...,"end":...}, with the numbers as strings. */
MO_MUST_USE bool mo_offline_signature_json(struct mo_buffer *out, uint8_t version,
                                           const struct mo_signature *signature);
/* The same as one line of text: class, reason or limit, and where. */
MO_MUST_USE bool mo_offline_signature_text(struct mo_buffer *out,
                                           const struct mo_signature *signature);
/* The handler's name as a record keeps it: without its module or overload suffix. */
MO_MUST_USE struct mo_failure_slice mo_offline_bare_name(const uint8_t *bytes, size_t length);

/* Writes the buffer and clears it. */
MO_MUST_USE bool mo_offline_flush(FILE *stream, struct mo_buffer *buffer);

MO_MUST_USE int mo_history(const struct mo_cli *cli, FILE *output, FILE *errors);

/* mo replay and mo query, after FILE's program checked: they lower, compile and run it. JSON's
 * version record is written once, through version; a command mistake sets mistake and returns
 * 2 with nothing written. Diagnostics go to diags; the result is the command's exit status. */
struct mo_program;
struct mo_offline_pipeline {
    struct mo_arena *arena;
    const struct mo_program *program;
    const struct mo_sources *sources;
    struct mo_diags *diags;
    const char *library; /* the installed library folder, or NULL */
    FILE *output, *errors;
    bool *version;
    const char **mistake;
};
MO_MUST_USE int mo_offline_program(const struct mo_cli *cli, struct mo_offline_pipeline *p);

#endif
