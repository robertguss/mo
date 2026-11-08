#ifndef MO_TOOLS_UPGRADE_H
#define MO_TOOLS_UPGRADE_H
#include "check/check.h"
#include "tools/tools.h"
MO_MUST_USE int mo_upgrade_emit(const struct mo_cli *cli, FILE *output, FILE *errors);
MO_MUST_USE int mo_upgrade_serve(const struct mo_cli *cli, FILE *output, FILE *errors);
MO_MUST_USE int mo_upgrade_check(const struct mo_cli *cli, FILE *output, FILE *errors);
MO_MUST_USE bool mo_upgrade_bundle(const char *path, bool migration, bool serving,
                                   struct mo_buffer *out, struct mo_diags *diags,
                                   struct mo_sources *sources);
MO_MUST_USE bool mo_upgrade_old_sources(const struct mo_program *old, struct mo_sources *candidate);
MO_MUST_USE bool mo_upgrade_schema_sources(const uint8_t *saved, size_t length,
                                           struct mo_sources *sources);
enum mo_upgrade_metadata_status {
    MO_UPGRADE_METADATA_OK,
    MO_UPGRADE_METADATA_ERROR,
    MO_UPGRADE_METADATA_INITIALIZER,
    MO_UPGRADE_METADATA_TIMER
};
MO_MUST_USE enum mo_upgrade_metadata_status mo_upgrade_metadata(const struct mo_program *program,
                                                                const struct mo_buffer *wire,
                                                                struct mo_buffer *out,
                                                                FILE *errors);

#endif
