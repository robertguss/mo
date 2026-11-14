#ifndef MO_UPGRADE_INTERNAL_H
#define MO_UPGRADE_INTERNAL_H

#include "host/host.h"
#include "tools/upgrade.h"

struct upgrade_image {
    struct mo_image *image;
    const struct mo_image *running; /* a live service's image, borrowed in place of image */
    uint32_t service;
    struct mo_store_program program;
    struct mo_buffer context, items;
    bool schema_synthesized;
};
struct upgrade_report {
    uint16_t code;
    enum mo_compile_status compilation;
    const char *class, *message;
    char detail[MO_LIMIT_UPGRADE_REPORT_BYTES + 1];
    uint8_t base[32], candidate[32];
    bool have_base, have_candidate, activated, already_active;
    uint64_t ordinal, head, vm_work, native_work;
    size_t heap_peak, encoded_bytes;
    struct mo_buffer differences;
    uint8_t old_schema[32], new_schema[32];
    bool compared, implementation_changed, rehearsal, state_valid;
};

struct compile_job {
    char folder[sizeof "/tmp/mo-upgrade-XXXXXX"];
    char source[128], previous[128], image[128], diagnostics[128], input_context[128],
        output_context[128], items[128], list_diagnostics[128];
    const char *compiler;
    struct mo_compile_child child;
    bool scratch, listing, has_old, has_context;
};

bool mo_upgrade_error(struct mo_diags *diags, uint16_t code, const char *message);
bool mo_upgrade_file_bytes(const char *path, size_t limit, struct mo_buffer *out);
bool mo_upgrade_write_file_bytes(const char *path, const uint8_t *bytes, size_t length);

bool mo_upgrade_refuse(struct upgrade_report *r, uint16_t code, const char *class,
                       const char *message);
bool mo_upgrade_report_json(struct mo_buffer *out, const struct upgrade_report *r,
                            const char *migration);
bool mo_upgrade_report_text(struct mo_buffer *text, const struct upgrade_report *report);
void mo_upgrade_compile_cleanup(struct compile_job *job);
bool mo_upgrade_compile_begin(const char *compiler, const struct mo_buffer *bundle,
                              const struct mo_buffer *old, const char *migration,
                              const struct mo_buffer *context, struct compile_job *job,
                              struct upgrade_report *report);
enum mo_compile_status mo_upgrade_compile_poll(struct compile_job *job);
void mo_upgrade_image_buffers_init(struct upgrade_image *image);
void mo_upgrade_image_buffers_free(struct upgrade_image *image);
bool mo_upgrade_compile_end(struct compile_job *job, enum mo_compile_status status,
                            const struct mo_buffer *bundle, struct upgrade_image *out,
                            struct upgrade_report *report);
bool mo_upgrade_compare_programs(const struct upgrade_image *old, const struct upgrade_image *next,
                                 const char *migration, struct upgrade_report *report);
bool mo_upgrade_migration_state(const struct upgrade_image *old, const struct upgrade_image *next,
                                const char *migration, const uint8_t *bytes, size_t length,
                                struct mo_buffer *out, struct upgrade_report *report,
                                bool rehearsal);

#endif
