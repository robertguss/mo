#ifndef MO_TOOLS_UPGRADE_H
#define MO_TOOLS_UPGRADE_H
#include "check/check.h"
#include "tools/tools.h"
MO_MUST_USE int mo_upgrade_emit(const struct mo_cli *cli, FILE *output, FILE *errors);
MO_MUST_USE int mo_upgrade_serve(const struct mo_cli *cli, FILE *output, FILE *errors);
MO_MUST_USE int mo_upgrade_rehearse(const struct mo_cli *cli, FILE *output, FILE *errors);
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

/* Live upgrades of a running service. start copies the candidate bundle and the active
 * program and starts the compiler child; NULL means memory, and any refusal is reported
 * through the report once poll returns true. admit repeats the typed check against the
 * running program and migrates the state current at the boundary under the live limits. */
struct mo_live_upgrade;
struct mo_store_program;
MO_MUST_USE struct mo_live_upgrade *mo_live_start(const char *compiler,
                                                  const struct mo_store_program *active,
                                                  const uint8_t *bundle, size_t size,
                                                  const char *migration);
MO_MUST_USE bool mo_live_poll(struct mo_live_upgrade *upgrade);
MO_MUST_USE bool mo_live_staged(const struct mo_live_upgrade *upgrade);
MO_MUST_USE uint16_t mo_live_code(const struct mo_live_upgrade *upgrade);
MO_MUST_USE bool mo_live_admit(struct mo_live_upgrade *upgrade, const struct mo_image *running,
                               uint32_t index, const struct mo_store_program *active,
                               const uint8_t *state, size_t length);
void mo_live_refuse(struct mo_live_upgrade *upgrade, uint16_t code, const char *class,
                    const char *message);
/* The accepted report for the activation record, valid until the next report call. */
MO_MUST_USE const char *mo_live_activation(struct mo_live_upgrade *upgrade, uint64_t ordinal,
                                           uint64_t head, size_t *size);
MO_MUST_USE const struct mo_store_program *mo_live_program(const struct mo_live_upgrade *upgrade);
MO_MUST_USE const uint8_t *mo_live_state(const struct mo_live_upgrade *upgrade, size_t *length);
MO_MUST_USE const struct mo_image *mo_live_image(const struct mo_live_upgrade *upgrade,
                                                 uint32_t *service);
MO_MUST_USE struct mo_image *mo_live_take_image(struct mo_live_upgrade *upgrade);
MO_MUST_USE const char *mo_live_migration(const struct mo_live_upgrade *upgrade);
MO_MUST_USE bool mo_live_report(const struct mo_live_upgrade *upgrade, bool json,
                                struct mo_buffer *out);
void mo_live_free(struct mo_live_upgrade *upgrade);
/* The shared report for a refusal decided before a candidate is compiled; code 0 reports an
 * already active candidate. */
MO_MUST_USE bool mo_live_refusal(struct mo_buffer *out, bool json, uint16_t code, const char *class,
                                 const char *message, const uint8_t *base, const uint8_t *candidate,
                                 const char *migration);

#endif
