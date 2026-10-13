#define _POSIX_C_SOURCE 200809L

#include "base/stack.h"

#include <pthread.h>

struct work {
    void (*function)(void *);
    void *argument;
};

static MO_MUST_USE void *run(void *argument) {
    const struct work *work = argument;
    work->function(work->argument);
    return NULL;
}

int mo_with_stack(size_t stack_size, void (*function)(void *), void *argument) {
    MO_ASSERT(function != NULL);
    pthread_attr_t attributes;
    int error = pthread_attr_init(&attributes);
    if (error != 0)
        return error;
    error = pthread_attr_setstacksize(&attributes, stack_size);
    if (error != 0) {
        MO_ASSERT(pthread_attr_destroy(&attributes) == 0);
        return error;
    }
    struct work work = {function, argument};
    pthread_t thread;
    error = pthread_create(&thread, &attributes, run, &work);
    MO_ASSERT(pthread_attr_destroy(&attributes) == 0);
    if (error != 0)
        return error;
    /* A failed join would leave the callback borrowing this stack frame. */
    MO_ASSERT(pthread_join(thread, NULL) == 0);
    return 0;
}
