/* A service's recovery check: generated sequences of tool calls, clock advances, crashes and
 * restarts, each run on a fresh in-memory store through the simulated-service API. Before every
 * crash the live state is encoded with the saved-value codec; after every restart, before the
 * startup ticks run, the recovered state is encoded too, and the two must be equal byte for byte.
 * A check hook can judge every step too, as mo test does with service promises. The search is
 * deterministic, seeded from the check's name. */
#ifndef MO_TOOLS_RECOVERY_H
#define MO_TOOLS_RECOVERY_H

#include "tools/inputs.h"
#include "vm/vm.h"

/* A recovery check is named after its module plus this suffix; service is reserved, so no
 * example, promise or test has the name. */
#define MO_RECOVERY_SUFFIX ".service.recovery"

enum mo_recovery_step_kind {
    MO_RECOVERY_SEND,
    MO_RECOVERY_ADVANCE,
    MO_RECOVERY_CRASH,
    MO_RECOVERY_RESTART
};

/* What the supervisor did after a SEND: 5 failures within 60 seconds restart the service,
 * and 3 restarts within 10 minutes stop it. */
enum mo_recovery_supervisor { MO_SUPERVISOR_NONE, MO_SUPERVISOR_RESTARTED, MO_SUPERVISOR_STOPPED };

/* One step, and once it ran, what it did. */
struct mo_recovery_step {
    enum mo_recovery_step_kind kind;
    uint32_t tool; /* SEND: an index in the service's tools */
    /* SEND: the tool's arguments as literal values in the checker's heap, owned by the sequence. */
    struct mo_cell *arguments;
    int64_t duration; /* ADVANCE: microseconds */
    bool ran;
    enum mo_simulation_sent sent;           /* SEND */
    enum mo_service_status status;          /* SEND: the reply's status */
    bool updated;                           /* SEND: the tool's update committed */
    enum mo_recovery_supervisor supervisor; /* SEND */
    enum mo_simulation_outcome outcome;     /* ADVANCE and RESTART */
    uint64_t firings;                       /* ADVANCE: timer firings counted before any ran */
    uint32_t failed_firings;                /* ADVANCE: firings whose timer handler failed */
};

/* Steps in order. Every crash is followed by its restart. Argument arrays come from arena. */
struct mo_recovery_sequence {
    struct mo_arena arena;
    struct mo_recovery_step steps[MO_CLI_RECOVERY_STEPS];
    uint32_t count;
};
void mo_recovery_sequence_init(struct mo_recovery_sequence *sequence);
struct mo_recovery;
/* Drops every argument, frees the arena and leaves the sequence empty, ready to use again. */
void mo_recovery_sequence_clear(struct mo_recovery *recovery,
                                struct mo_recovery_sequence *sequence);

/* A failure the service handed back, copied out of its view. */
struct mo_recovery_failure {
    enum mo_service_status status;
    int64_t now;
    /* The start value, a timer's interval or handler, a replayed handler, or MO_NO_INDEX. */
    uint32_t function;
    struct mo_vm_span span; /* the defect's; file is MO_NO_INDEX when there is none */
    char reason[128];       /* the defect's reason, or empty; cut to fit */
    char detail[MO_LIMIT_SIMULATION_REPORT];
    size_t detail_length; /* the defect's Text detail, cut to fit */
};

enum mo_recovery_outcome {
    MO_RECOVERY_PASSED,     /* every restart compared recovered the state, and every check held */
    MO_RECOVERY_MISMATCH,   /* a restart recovered a state that differs from the one at its crash */
    MO_RECOVERY_REFUSED,    /* recovery refused the store at a restart; failure says why */
    MO_RECOVERY_STOPPED,    /* the service failed at its first open, as failure says */
    MO_RECOVERY_CHECK,      /* the check hook found a failure at step, as verdict says */
    MO_RECOVERY_UNAVAILABLE /* the checker could not go on: problem says why */
};

/* What a check hook found after a step. */
struct mo_recovery_verdict {
    uint32_t check; /* the hook's own index of the failed check; MO_NO_INDEX when all held */
    uint32_t line;  /* its first false line, one-based, or 0 when its body stopped */
};

/* Counted from what the simulated-service API reports. Commits are a lower bound, since ticks a
 * supervisor restart queues are not seen, and replayed and snapshots are worked out from them. */
struct mo_recovery_counts {
    uint64_t sequences; /* run to their end */
    uint64_t restarts;  /* compared and equal */
    uint64_t commits;   /* committed updates and timer firings */
    uint64_t replayed;  /* commits after the newest snapshot, at each compared restart */
    /* Compared restarts whose store had passed MO_LIMIT_SERVICE_SNAPSHOT_MESSAGES commits, so
     * recovery began at a snapshot taken on that boundary. */
    uint64_t snapshots;
};

struct mo_recovery_result {
    enum mo_recovery_outcome outcome;
    struct mo_recovery_counts counts;
    /* A search's MISMATCH, REFUSED or CHECK: the failing sequence, ending at the failing step. */
    struct mo_recovery_sequence sequence;
    uint64_t ordinal; /* a search's failing sequence */
    /* A search's failure: counts before the failing sequence ran, to resume from it. */
    struct mo_recovery_counts resume;
    uint32_t step; /* MISMATCH and REFUSED: the failing restart's index; CHECK: the step's */
    /* MISMATCH: the state's saved encoding before the crash, and as recovered. CHECK: the state
     * before the step, or at its crash for a restart, and after it. */
    struct mo_buffer before, after;
    struct mo_recovery_verdict verdict; /* CHECK */
    struct mo_recovery_failure failure; /* REFUSED and STOPPED */
    const char *problem;                /* UNAVAILABLE */
};
void mo_recovery_result_free(struct mo_recovery *recovery, struct mo_recovery_result *result);

/* Borrowed for one callback, after each step runs. */
struct mo_recovery_observation {
    uint64_t sequence; /* the search's ordinal of the sequence; 0 in a replay */
    uint32_t index;    /* the step's index in it */
    const struct mo_recovery_step *step;
    const struct mo_simulation_reply *reply; /* a SEND that REPLIED or was REJECTED; else NULL */
    struct mo_simulation *simulation;        /* to read the state; never to drive it */
    /* check only: the state's saved encodings before and after the step, NULL when unreadable. A
     * restart's before is the state at its crash, and its after follows the startup ticks. */
    const struct mo_buffer *before, *after;
};
struct mo_recovery_hooks {
    void (*observe)(const struct mo_recovery_observation *observation, void *context);
    /* Runs after every step but a crash, once the startup ticks of a restart ran; may be NULL. */
    struct mo_recovery_verdict (*check)(const struct mo_recovery_observation *observation,
                                        void *context);
    void *context;
};

/* A checker for service, an index in an initialized image that outlives it. It owns a process
 * whose heap holds every argument and reply value, one input generator per tool, and the timers'
 * intervals. A tool whose arguments cannot be generated (an uninhabited type) is never sent; any
 * other refusal returns without writing out. */
MO_MUST_USE enum mo_input_result mo_recovery_new(const struct mo_image *image, uint32_t service,
                                                 const struct mo_limits *limits,
                                                 struct mo_recovery **out);
/* Every result and sequence must be cleared first. */
void mo_recovery_free(struct mo_recovery *recovery);
/* <Module>.service.recovery, NUL-terminated. */
MO_MUST_USE const char *mo_recovery_name(const struct mo_recovery *recovery);
MO_MUST_USE struct mo_heap *mo_recovery_heap(struct mo_recovery *recovery);
/* Whether restarts are compared, as they are at first. When not, a refused restart ends its
 * sequence. */
void mo_recovery_compare_restarts(struct mo_recovery *recovery, bool compare);

/* Runs the generated sequences numbered from first up to sequences, until one fails. Each one's
 * steps depend only on the check's name and its number. Always writes out; free it after. Hooks
 * may be NULL. */
MO_MUST_USE enum mo_recovery_outcome mo_recovery_search(struct mo_recovery *recovery,
                                                        uint64_t first, uint64_t sequences,
                                                        const struct mo_recovery_hooks *hooks,
                                                        struct mo_recovery_result *out);
/* Runs exactly sequence, from a fresh store, recording what each step did, until it ends or a
 * step fails. Its crashes and restarts must alternate, starting with a crash. out's sequence
 * stays empty. Always writes out; free it after. */
MO_MUST_USE enum mo_recovery_outcome mo_recovery_replay(struct mo_recovery *recovery,
                                                        struct mo_recovery_sequence *sequence,
                                                        const struct mo_recovery_hooks *hooks,
                                                        struct mo_recovery_result *out);

/* Shrinks sequence, which failed with failure (MISMATCH, REFUSED, or CHECK with verdict) at its
 * last step: deletes steps, a crash always with its restart, then simplifies tool arguments with
 * the input shrinker, then shortens advances, as long as any pass makes progress. Each candidate
 * is replayed from a fresh store with hooks, and kept, cut after its failing step, when it fails
 * the same way, with the same verdict. Arguments are literal values, so one an earlier reply
 * supplied keeps its value when that step goes. At most MO_CLI_RECOVERY_SHRINK_RUNS replays;
 * returns how many ran. sequence's records of what each step did are stale afterwards; replay it
 * to read them. */
MO_MUST_USE uint32_t mo_recovery_shrink(struct mo_recovery *recovery,
                                        struct mo_recovery_sequence *sequence,
                                        enum mo_recovery_outcome failure,
                                        const struct mo_recovery_hooks *hooks,
                                        struct mo_recovery_verdict verdict);

/* The part of a SEND's reply a report shows, borrowed from reply: an update's reply without its
 * new state, or the whole result of a read-only tool or of a rejection. */
MO_MUST_USE struct mo_cell mo_recovery_reply(const struct mo_recovery *recovery, uint32_t tool,
                                             enum mo_simulation_sent sent,
                                             const struct mo_simulation_reply *reply,
                                             uint32_t *type);

/* What mo test reports for one recovery check or service promise. */
struct mo_recovery_report {
    struct mo_recovery *recovery;
    uint32_t service;                 /* its index in the image */
    struct mo_recovery_counts counts; /* the search's, a failing sequence included */
    /* A failure, or a service promise whose body stopped: the shrunk sequence and its last run,
     * which failed at result->step. The service's own stop: result holds the failure and
     * sequence is NULL. */
    const struct mo_recovery_sequence *sequence;
    const struct mo_recovery_result *result;
    /* Per step of sequence: the reply a SEND showed, of type reply_types[i], in the checker's
     * heap; MO_NO_INDEX when it showed none. */
    const struct mo_cell *replies;
    const uint32_t *reply_types;
};

enum mo_recovery_compared { MO_RECOVERY_EQUAL, MO_RECOVERY_DIFFERENT, MO_RECOVERY_UNREADABLE };
/* The oracle, after a restart: replaces after with the live state's saved encoding, and compares
 * it with before. UNREADABLE when the service is crashed or never opened, or encoding fails. */
MO_MUST_USE enum mo_recovery_compared mo_recovery_compare(struct mo_simulation *simulation,
                                                          const struct mo_buffer *before,
                                                          struct mo_buffer *after);

#endif
