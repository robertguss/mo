#define _POSIX_C_SOURCE 200809L

#include "base/stack.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <unistd.h>

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
    errno = 0;
    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0)
        return errno == 0 ? EINVAL : errno;
    if ((uintmax_t)page > SIZE_MAX)
        return EOVERFLOW;
    size_t size = (size_t)page, remainder = stack_size % size;
    if (remainder != 0) {
        size_t padding = size - remainder;
        if (padding > SIZE_MAX - stack_size)
            return EOVERFLOW;
        stack_size += padding;
    }
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
