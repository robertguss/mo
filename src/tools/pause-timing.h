/* Private, opt-in diagnostics; no timing code in ordinary builds. */
#ifndef MO_TOOLS_PAUSE_TIMING_H
#define MO_TOOLS_PAUSE_TIMING_H
#ifdef MO_UPGRADE_PAUSE_TIMING
#include <stdint.h>
#include <stdio.h>
#include <time.h>
extern uint64_t mo_pause_attempt;
static inline uint64_t mo_pause_now(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0 || t.tv_sec < 0 || t.tv_nsec < 0 ||
        t.tv_nsec >= 1000000000L || (uint64_t)t.tv_sec > (UINT64_MAX - 999999999) / 1000000000)
        return UINT64_MAX;
    return (uint64_t)t.tv_sec * 1000000000 + (uint64_t)t.tv_nsec;
}
static inline void mo_pause_emit(const char *stage, uint64_t start, uint64_t end,
                                 const char *outcome) {
    if (start == UINT64_MAX || end == UINT64_MAX || end < start) {
        (void)fprintf(stderr,
                      "{\"mo_pause\":1,\"attempt\":%llu,\"stage\":\"%s\","
                      "\"outcome\":\"%s\",\"elapsed_ns\":null}\n",
                      (unsigned long long)mo_pause_attempt, stage, outcome);
    } else {
        (void)fprintf(stderr,
                      "{\"mo_pause\":1,\"attempt\":%llu,\"stage\":\"%s\","
                      "\"outcome\":\"%s\",\"elapsed_ns\":%llu}\n",
                      (unsigned long long)mo_pause_attempt, stage, outcome,
                      (unsigned long long)(end - start));
    }
}
#define PAUSE_START(name) uint64_t name = mo_pause_now()
#define PAUSE_END(name, stage, outcome) mo_pause_emit(stage, name, mo_pause_now(), outcome)
#define PAUSE_AT(name, end, stage, outcome) mo_pause_emit(stage, name, end, outcome)
#define PAUSE_ATTEMPT() (++mo_pause_attempt)
#else
#define PAUSE_AT(name, end, stage, outcome) ((void)0)
#define PAUSE_START(name) ((void)0)
#define PAUSE_END(name, stage, outcome) ((void)0)
#define PAUSE_ATTEMPT() ((void)0)
#endif
#endif
