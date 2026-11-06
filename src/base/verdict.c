#include "base/verdict.h"
#include <string.h>

struct mo_signature mo_signature_from_record(const struct mo_failure_record *record) {
    MO_ASSERT(record->class <= MO_SIGNATURE_STATE_SIZE);
    return (struct mo_signature){.class = (enum mo_signature_class)record->class,
                                 .reason = record->reason,
                                 .limit = record->limit,
                                 .limit_value = record->limit_value,
                                 .function = record->function,
                                 .file = record->file,
                                 .start = record->start,
                                 .end = record->end};
}

static bool same_text(struct mo_failure_slice a, struct mo_failure_slice b) {
    return a.length == b.length && (a.length == 0 || memcmp(a.bytes, b.bytes, a.length) == 0);
}

bool mo_signature_match(const struct mo_signature *a, const struct mo_signature *b) {
    if (a->class != b->class)
        return false;
    switch (a->class) {
    case MO_SIGNATURE_DEFECT:
        return same_text(a->reason, b->reason) && a->function == b->function &&
               a->file == b->file && a->start == b->start && a->end == b->end;
    case MO_SIGNATURE_WORK:
        return a->limit_value == b->limit_value;
    case MO_SIGNATURE_MEMORY:
    case MO_SIGNATURE_STATE_SIZE:
        return same_text(a->limit, b->limit) && a->limit_value == b->limit_value;
    }
    MO_UNREACHABLE();
}

static bool failed(enum mo_outcome_status status) {
    switch (status) {
    case MO_OUTCOME_DEFECT:
    case MO_OUTCOME_WORK:
    case MO_OUTCOME_MEMORY:
    case MO_OUTCOME_STATE_SIZE:
        return true;
    case MO_OUTCOME_SUCCEEDED:
    case MO_OUTCOME_REFUSED:
        return false;
    }
    MO_UNREACHABLE();
}

/* The signature's class is the one its failure status names. */
static void check_outcome(const struct mo_outcome *o) {
    enum mo_signature_class c = o->signature.class;
    MO_ASSERT(!failed(o->status) || (o->status == MO_OUTCOME_DEFECT && c == MO_SIGNATURE_DEFECT) ||
              (o->status == MO_OUTCOME_WORK && c == MO_SIGNATURE_WORK) ||
              (o->status == MO_OUTCOME_MEMORY && c == MO_SIGNATURE_MEMORY) ||
              (o->status == MO_OUTCOME_STATE_SIZE && c == MO_SIGNATURE_STATE_SIZE));
}

enum mo_replay_class mo_replay_classify(const struct mo_signature *recorded,
                                        const struct mo_outcome *offline) {
    check_outcome(offline);
    if (offline->status == MO_OUTCOME_REFUSED)
        return MO_REPLAY_REFUSED;
    if (offline->status == MO_OUTCOME_SUCCEEDED)
        return MO_REPLAY_NOT_REPRODUCED;
    return mo_signature_match(recorded, &offline->signature) ? MO_REPLAY_REPRODUCED
                                                             : MO_REPLAY_DIFFERENT;
}

enum mo_verdict mo_verdict(enum mo_replay_class original, const struct mo_outcome *revised) {
    check_outcome(revised);
    if (original == MO_REPLAY_REFUSED || revised->status == MO_OUTCOME_REFUSED)
        return MO_VERDICT_REFUSED;
    if (original != MO_REPLAY_REPRODUCED)
        return MO_VERDICT_UNVERIFIED;
    /* A state-size status is the size check failing, so only a plain success is verified. */
    return revised->status == MO_OUTCOME_SUCCEEDED ? MO_VERDICT_VERIFIED : MO_VERDICT_NOT_FIXED;
}
