#include "base/codes.h"
#include "base/intent-list.h"
#include "tools/offline.h"
#include "tools/upgrade-internal.h"

#include <stdio.h>
#include <string.h>

static bool shown(FILE *errors, const struct mo_buffer *difference) {
    struct mo_buffer text;
    mo_buffer_init(&text, 2 * MO_DOOR_DIFFERENCE_BYTES);
    bool ok = mo_door_lines(&text, difference->bytes, difference->length) &&
              fwrite(text.bytes, 1, text.length, errors) == text.length && fflush(errors) == 0;
    mo_buffer_free(&text);
    return ok;
}

bool mo_upgrade_restart_door(const struct mo_cli *cli, struct mo_store *store,
                             struct mo_arena *arena, struct mo_store_activation *activation,
                             struct mo_store_decision *decision, FILE *errors,
                             struct upgrade_report *report) {
    struct mo_door_view view = {0};
    mo_buffer_init(&view.text, MO_DOOR_DIFFERENCE_BYTES);
    struct mo_store_review r = {0};
    struct mo_intent_list items;
    const char *migration = cli->migration == NULL ? "" : cli->migration;
    bool ok = mo_store_review_read(store, arena, &r) == MO_STORE_OK &&
              mo_intent_list_read(activation->candidate.items, activation->candidate.items_size,
                                  &items) &&
              mo_door_difference(arena, &r, &items, &view);
    bool would = view.keeps && cli->migration == NULL;
    uint8_t id[32];
    mo_door_review_id(activation->previous.source, activation->candidate.source, migration,
                      r.version, view.text.bytes, view.text.length, id);
    struct mo_buffer hex;
    mo_buffer_init(&hex, 65);
    ok = ok && mo_offline_hex(&hex, id, 32) && mo_buffer_append_byte(&hex, 0);
    *decision = (struct mo_store_decision){.kind = "restart",
                                           .has_candidate = true,
                                           .has_review = !(r.intent && would),
                                           .intent = r.intent,
                                           .unattended = would,
                                           .time = activation->time};
    memcpy(decision->base, activation->previous.source, 32);
    memcpy(decision->candidate, activation->candidate.source, 32);
    memcpy(decision->review, id, 32);
    if (!ok)
        (void)mo_upgrade_refuse(report, MO_CODE_UPGRADE_INVALID_VALUE, "store",
                                "the data folder's review tables could not be read");
    else if (r.records >= MO_LIMIT_REVIEW_RECORDS) {
        ok = false;
        (void)mo_upgrade_refuse(report, MO_CODE_REVIEW_FULL, "record",
                                "the review record is full; only a fresh data folder accepts "
                                "upgrades");
    } else if (decision->has_review &&
               (cli->review == NULL || strcmp(cli->review, (const char *)hex.bytes) != 0)) {
        ok = false;
        decision->kind = "refused";
        decision->detail = "E1435";
        int n = snprintf(report->detail, sizeof report->detail,
                         "the upgrade waits for review; restart with --review %s to activate it",
                         (const char *)hex.bytes);
        MO_ASSERT(n > 0 && (size_t)n < sizeof report->detail);
        (void)mo_upgrade_refuse(report, MO_CODE_REVIEW_MISMATCH, "review", report->detail);
        if (!shown(errors, &view.text) ||
            (r.reviewed && mo_store_decide(store, decision, NULL) != MO_STORE_REVIEW_OK))
            (void)mo_upgrade_refuse(report, MO_CODE_UPGRADE_INVALID_VALUE, "store",
                                    "the refused restart could not be shown or recorded");
    } else {
        activation->decision = decision;
        activation->approval_version = r.version;
        activation->approve = (const uint8_t(*)[32])view.approve;
        activation->approve_count = view.approve_count;
        activation->retire = (const uint8_t(*)[32])view.retire;
        activation->retire_count = view.retire_count;
    }
    mo_buffer_free(&hex);
    mo_buffer_free(&view.text);
    return ok;
}
