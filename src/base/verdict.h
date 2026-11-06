#ifndef MO_VERDICT_H
#define MO_VERDICT_H
#include "base/failure.h"

/* What replay and the fix check decide, as pure functions over structured signatures
 * (introspection design sections 6.2 and 7.1). Rendered report text is never compared. */

/* The class numbers are the ones a failure record stores. */
enum mo_signature_class {
    MO_SIGNATURE_DEFECT = 0,
    MO_SIGNATURE_WORK = 1,
    MO_SIGNATURE_MEMORY = 2,
    MO_SIGNATURE_STATE_SIZE = 3
};

/* A failure's signature, version 1. Text borrows the record's bytes or static strings.
 * `reason` is the VM's fixed reason, never the program's own `stop` text; `limit` is the limit
 * name (`--max-heap`, `native allocation`, `MO_LIMIT_SAVED_BYTES`, ...) and `limit_value` its
 * value; `file`, `start` and `end` are the span in `function`. */
struct mo_signature {
    enum mo_signature_class class;
    struct mo_failure_slice reason, limit;
    uint64_t limit_value;
    uint32_t function, file, start, end;
};
/* The record's slices must outlive the signature. */
MO_MUST_USE struct mo_signature mo_signature_from_record(const struct mo_failure_record *record);

/* Equal when design section 6.2 says two signatures are the same failure: a defect by reason,
 * function and span; work by the limit value; memory by limit name and value, so
 * `native allocation` matches only itself; state size by the saved-copy limit's name and value.
 * Different classes never match. */
MO_MUST_USE bool mo_signature_match(const struct mo_signature *a, const struct mo_signature *b);

/* How one offline run ended. A state-size status is a handler that returned but whose new state
 * failed the saved-copy size check; only updating tools and timers have one. */
enum mo_outcome_status {
    MO_OUTCOME_SUCCEEDED,
    MO_OUTCOME_DEFECT,
    MO_OUTCOME_WORK,
    MO_OUTCOME_MEMORY,
    MO_OUTCOME_STATE_SIZE,
    MO_OUTCOME_REFUSED
};
/* An `Error` reply is a success that reports its kind. */
enum mo_reply_kind { MO_REPLY_OK, MO_REPLY_ERROR };
/* `signature` holds for the four failure statuses, with the class the status names. `reply` and
 * `state_changed` hold when the handler returned: succeeded or state size. */
struct mo_run_outcome {
    enum mo_outcome_status status;
    struct mo_signature signature;
    enum mo_reply_kind reply;
    bool state_changed;
};

enum mo_replay_class {
    MO_REPLAY_REPRODUCED,
    MO_REPLAY_DIFFERENT,
    MO_REPLAY_NOT_REPRODUCED,
    MO_REPLAY_REFUSED
};
/* Compares an offline run with the recorded failure's signature: the same failure reproduced, a
 * different failure, a run that succeeded, or a run refused before it started. */
MO_MUST_USE enum mo_replay_class mo_replay_classify(const struct mo_signature *recorded,
                                                    const struct mo_run_outcome *offline);

enum mo_verdict {
    MO_VERDICT_VERIFIED,
    MO_VERDICT_NOT_FIXED,
    MO_VERDICT_UNVERIFIED,
    MO_VERDICT_REFUSED
};
/* Design section 7.1: `verified` only when the original reproduced and the revision succeeded,
 * its new state passing the size check; `not_fixed` when the original reproduced and the revision
 * failed; `unverified` for a different or unreproduced original whatever the revision did;
 * `refused` when either run was refused. */
MO_MUST_USE enum mo_verdict mo_verdict(enum mo_replay_class original,
                                       const struct mo_run_outcome *revised);
#endif
