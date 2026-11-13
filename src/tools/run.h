/* Execute the verified lowered entry. Called inside the source-stack callback. */
#ifndef MO_TOOLS_RUN_H
#define MO_TOOLS_RUN_H

#include "check/check.h"
#include "core/core.h"
#include "tools/tools.h"

struct mo_bytecode;
struct mo_image;
struct mo_store;
MO_MUST_USE bool mo_serve_watch_signals(void);
/* Exact filename-byte lookup shared by anchor and observation reports. */
MO_MUST_USE struct mo_span mo_runtime_span(const struct mo_bytecode *file,
                                           const struct mo_sources *sources, uint32_t path,
                                           uint32_t start, uint32_t end);
/* Lowering's function for a service promise, or for its anchor when anchor is not NULL. */
MO_MUST_USE uint32_t mo_service_function(const struct mo_bytecode *file, const char *module,
                                         uint32_t promise, const struct mo_checked_spec *anchor);
MO_MUST_USE bool mo_program_has_anchors(const struct mo_program *program);
/* Image must be initialized. Does not call main or ordinary observations. */
MO_MUST_USE bool mo_check_anchors(struct mo_image *image, const struct mo_cli *cli,
                                  const struct mo_sources *sources,
                                  const struct mo_program *program, struct mo_diags *diags);
MO_MUST_USE int mo_run_program(const struct mo_cli *cli, const struct mo_program *program,
                               const struct mo_core *core, const struct mo_buffer *wire,
                               const struct mo_sources *sources, FILE *output, FILE *errors,
                               struct mo_diags *diags);
MO_MUST_USE int mo_serve_image(const struct mo_cli *cli, const struct mo_image *image,
                               uint32_t service, FILE *output, FILE *errors, struct mo_diags *diags,
                               struct mo_store *held_store);

#endif
