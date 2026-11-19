#define _POSIX_C_SOURCE 200809L
#include "host/files.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

struct connection {
    struct mo_host *owner;
    int descriptor;
    uint64_t deadline;
};

enum status { READY, TIMED_OUT, CLOSED, REFUSED, UNKNOWN_HOST, BROKEN, INTERNAL };
struct problem {
    enum status status;
    int number;
    const char *reason;
};

enum lookup_state { LOOKUP_FREE, LOOKUP_RUNNING, LOOKUP_DONE, LOOKUP_ABANDONED };
struct lookup {
    enum lookup_state state;
    int ends[2];
    char host[MO_LIMIT_HOST_NAME + 1], port[6];
    int status, number;
    struct sockaddr_storage addresses[MO_LIMIT_LOOKUP_ADDRESSES];
    socklen_t lengths[MO_LIMIT_LOOKUP_ADDRESSES];
    uint32_t count;
};
static struct lookup lookups[MO_LIMIT_LOOKUPS];
static pthread_mutex_t lookups_lock = PTHREAD_MUTEX_INITIALIZER;

static void lock(void) {
    int result = pthread_mutex_lock(&lookups_lock);
    MO_ASSERT(result == 0);
}

static void unlock(void) {
    int result = pthread_mutex_unlock(&lookups_lock);
    MO_ASSERT(result == 0);
}

static void *resolve(void *argument) {
    struct lookup *slot = argument;
    struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM};
    struct addrinfo *found = NULL;
    int status = getaddrinfo(slot->host, slot->port, &hints, &found);
    int number = errno;
    lock();
    slot->status = status;
    slot->number = number;
    slot->count = 0;
    for (const struct addrinfo *a = found;
         status == 0 && a != NULL && slot->count < MO_LIMIT_LOOKUP_ADDRESSES; a = a->ai_next) {
        MO_ASSERT(a->ai_addrlen <= sizeof slot->addresses[0]);
        memcpy(&slot->addresses[slot->count], a->ai_addr, a->ai_addrlen);
        slot->lengths[slot->count++] = a->ai_addrlen;
    }
    mo_host_close(slot->ends[1]);
    if (slot->state == LOOKUP_ABANDONED) {
        mo_host_close(slot->ends[0]);
        slot->state = LOOKUP_FREE;
    } else
        slot->state = LOOKUP_DONE;
    unlock();
    if (status == 0)
        freeaddrinfo(found);
    return NULL;
}

static MO_MUST_USE bool close_on_exec(int fd) {
    int flags = fcntl(fd, F_GETFD);
    return flags >= 0 && fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0;
}

static MO_MUST_USE int start_thread(struct lookup *slot) {
    pthread_attr_t attributes;
    int result = pthread_attr_init(&attributes);
    if (result != 0)
        return result;
    sigset_t all, before;
    result = pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    if (result == 0 && sigfillset(&all) != 0)
        result = errno;
    if (result == 0)
        result = pthread_sigmask(SIG_SETMASK, &all, &before);
    if (result == 0) {
        pthread_t thread;
        result = pthread_create(&thread, &attributes, resolve, slot);
        int restored = pthread_sigmask(SIG_SETMASK, &before, NULL);
        MO_ASSERT(restored == 0);
    }
    int destroyed = pthread_attr_destroy(&attributes);
    MO_ASSERT(destroyed == 0);
    return result;
}

static MO_MUST_USE struct problem start_lookup(const uint8_t *host, size_t length, int64_t port,
                                               struct lookup **out) {
    lock();
    struct lookup *slot = NULL;
    for (uint32_t i = 0; i < MO_LIMIT_LOOKUPS && slot == NULL; i++) {
        if (lookups[i].state == LOOKUP_FREE)
            slot = &lookups[i];
    }
    struct problem problem = {.status = READY};
    if (slot == NULL)
        problem = (struct problem){.status = BROKEN, .reason = "too many lookups still running"};
    else if (pipe(slot->ends) != 0)
        problem = (struct problem){.status = BROKEN, .number = errno};
    else {
        memcpy(slot->host, host, length);
        slot->host[length] = '\0';
        int written = snprintf(slot->port, sizeof slot->port, "%d", (int)port);
        MO_ASSERT(written > 0 && (size_t)written < sizeof slot->port);
        int number = close_on_exec(slot->ends[0]) && close_on_exec(slot->ends[1]) ? 0 : errno;
        if (number == 0)
            number = start_thread(slot);
        if (number == 0)
            slot->state = LOOKUP_RUNNING;
        else {
            mo_host_close(slot->ends[0]);
            mo_host_close(slot->ends[1]);
            problem = (struct problem){.status = BROKEN, .number = number};
        }
    }
    unlock();
    *out = slot;
    return problem;
}

static MO_MUST_USE bool monotonic(uint64_t *out) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return false;
    MO_ASSERT(now.tv_sec >= 0 && now.tv_nsec >= 0 && now.tv_nsec < 1000000000);
    *out = (uint64_t)now.tv_sec * 1000000u + (uint64_t)now.tv_nsec / 1000u;
    return true;
}

static MO_MUST_USE struct problem wait_for(int fd, short events, uint64_t deadline) {
    for (;;) {
        uint64_t now;
        if (!monotonic(&now))
            return (struct problem){.status = INTERNAL};
        if (now >= deadline)
            return (struct problem){.status = TIMED_OUT};
        uint64_t left = (deadline - now + 999u) / 1000u;
        struct pollfd item = {.fd = fd, .events = events};
        int ready = poll(&item, 1, left > INT_MAX ? INT_MAX : (int)left);
        if (ready > 0)
            return (struct problem){.status = READY};
        if (ready < 0 && errno != EINTR)
            return (struct problem){.status = BROKEN, .number = errno};
    }
}

static MO_MUST_USE struct problem finish_lookup(struct lookup *slot, uint64_t deadline,
                                                struct lookup *out) {
    struct problem problem = wait_for(slot->ends[0], POLLIN, deadline);
    lock();
    if (slot->state == LOOKUP_RUNNING) {
        slot->state = LOOKUP_ABANDONED;
        unlock();
        MO_ASSERT(problem.status != READY);
        return problem;
    }
    MO_ASSERT(slot->state == LOOKUP_DONE);
    *out = *slot;
    mo_host_close(slot->ends[0]);
    slot->state = LOOKUP_FREE;
    unlock();
    if (problem.status != READY && problem.status != TIMED_OUT)
        return problem;
    if (out->status == 0) {
        MO_ASSERT(out->count != 0);
        return (struct problem){.status = READY};
    }
    if (out->status == EAI_NONAME)
        return (struct problem){.status = UNKNOWN_HOST};
#ifdef EAI_NODATA
    if (out->status == EAI_NODATA)
        return (struct problem){.status = UNKNOWN_HOST};
#endif
    if (out->status == EAI_SYSTEM)
        return (struct problem){.status = BROKEN, .number = out->number};
    return (struct problem){.status = BROKEN, .reason = gai_strerror(out->status)};
}

static MO_MUST_USE struct problem open_socket(const struct lookup *found, uint64_t deadline,
                                              int *out) {
    bool refused = false;
    int last = 0;
    for (uint32_t i = 0; i < found->count; i++) {
        const struct sockaddr *address = (const struct sockaddr *)&found->addresses[i];
        int fd = socket(address->sa_family, SOCK_STREAM, 0);
        if (fd < 0) {
            last = errno;
            continue;
        }
        int flags = fcntl(fd, F_GETFL);
        int number = flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0 && close_on_exec(fd)
                         ? 0
                         : errno;
        if (number == 0 && connect(fd, address, found->lengths[i]) != 0)
            number = errno;
        if (number == EINPROGRESS || number == EINTR) {
            struct problem problem = wait_for(fd, POLLOUT, deadline);
            if (problem.status != READY) {
                mo_host_close(fd);
                return problem;
            }
            socklen_t size = sizeof number;
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &number, &size) != 0)
                number = errno;
        }
        if (number == 0) {
            *out = fd;
            return (struct problem){.status = READY};
        }
        mo_host_close(fd);
        refused = refused || number == ECONNREFUSED;
        last = number;
    }
    return refused ? (struct problem){.status = REFUSED}
                   : (struct problem){.status = BROKEN, .number = last};
}

static MO_MUST_USE struct problem send_all(const struct connection *connection,
                                           const uint8_t *bytes, size_t length) {
    size_t sent = 0;
    while (sent < length) {
        struct problem problem = wait_for(connection->descriptor, POLLOUT, connection->deadline);
        if (problem.status != READY)
            return problem;
        ssize_t n = send(connection->descriptor, bytes + sent, length - sent, 0);
        if (n >= 0)
            sent += (size_t)n;
        else if (errno == EPIPE || errno == ECONNRESET)
            return (struct problem){.status = CLOSED};
        else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
            return (struct problem){.status = BROKEN, .number = errno};
    }
    return (struct problem){.status = READY};
}

static MO_MUST_USE struct problem receive_some(const struct connection *connection, uint8_t *bytes,
                                               size_t most, size_t *count) {
    for (;;) {
        struct problem problem = wait_for(connection->descriptor, POLLIN, connection->deadline);
        if (problem.status != READY)
            return problem;
        ssize_t n = recv(connection->descriptor, bytes, most, 0);
        if (n >= 0) {
            *count = (size_t)n;
            return problem;
        }
        if (errno == ECONNRESET)
            return (struct problem){.status = CLOSED};
        if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
            return (struct problem){.status = BROKEN, .number = errno};
    }
}

static void release(intptr_t handle) {
    struct connection *connection = (struct connection *)handle;
    mo_host_close(connection->descriptor);
    MO_ASSERT(connection->owner->open_connections != 0);
    connection->owner->open_connections--;
}

static MO_MUST_USE uint32_t payload(const struct mo_host *host, uint32_t type, const char *name) {
    return host->types->types.items[type].cases[mo_host_tag(host, type, name)].fields[0].type;
}

static MO_MUST_USE enum mo_host_outcome succeed(struct mo_host *host, struct mo_heap *heap,
                                                uint32_t type, struct mo_cell value,
                                                struct mo_cell *out) {
    bool ok = mo_host_case(host, heap, type, "Ok", &value, 1, out);
    mo_value_drop(heap, payload(host, type, "Ok"), value);
    return ok ? MO_HOST_RETURNED : MO_HOST_OUT_OF_MEMORY;
}

static MO_MUST_USE enum mo_host_outcome fail(struct mo_host *host, struct mo_heap *heap,
                                             uint32_t type, struct problem problem,
                                             struct mo_cell name, int64_t port, struct mo_cell *out,
                                             struct mo_host_error *error) {
    static const char *const cases[] = {[TIMED_OUT] = "TimedOut",
                                        [CLOSED] = "Closed",
                                        [REFUSED] = "Refused",
                                        [UNKNOWN_HOST] = "UnknownHost",
                                        [BROKEN] = "Broken"};
    if (problem.status == INTERNAL) {
        *error = (struct mo_host_error){.reason = "internal error: monotonic clock unavailable"};
        return MO_HOST_INTERNAL;
    }
    MO_ASSERT(problem.status != READY);
    struct mo_cell fields[] = {name, mo_cell_int(port)};
    uint32_t count = problem.status == REFUSED ? 2 : problem.status == UNKNOWN_HOST ? 1 : 0;
    if (problem.status == BROKEN) {
        char system[256];
        const char *reason = problem.reason;
        if (reason == NULL)
            reason =
                strerror_r(problem.number, system, sizeof system) == 0 ? system : "system error";
        if (!mo_value_text(heap, reason, strlen(reason), &fields[0]))
            return MO_HOST_OUT_OF_MEMORY;
        count = 1;
    }
    uint32_t error_type = payload(host, type, "Error");
    struct mo_cell value;
    bool built = mo_host_case(host, heap, error_type, cases[problem.status], fields, count, &value);
    if (problem.status == BROKEN)
        mo_value_drop(heap, host->types->text, fields[0]);
    if (!built)
        return MO_HOST_OUT_OF_MEMORY;
    bool wrapped = mo_host_case(host, heap, type, "Error", &value, 1, out);
    mo_value_drop(heap, error_type, value);
    return wrapped ? MO_HOST_RETURNED : MO_HOST_OUT_OF_MEMORY;
}

static MO_MUST_USE enum mo_host_outcome network_connect(struct mo_host *host, struct mo_heap *heap,
                                                        uint32_t type, const struct mo_cell *args,
                                                        struct mo_cell *out,
                                                        struct mo_host_error *error) {
    int64_t port = mo_cell_get_int(args[2]);
    if (port < 1 || port > 65535)
        return mo_host_bug(error, "port outside 1 to 65535");
    if (host->open_connections == MO_LIMIT_OPEN_CONNECTIONS)
        return mo_host_bug(error, "too many connections open at once");
    size_t length;
    const uint8_t *name = mo_value_data(args[1], &length);
    struct problem problem = {.status = UNKNOWN_HOST};
    uint64_t now;
    int fd = -1;
    struct connection connection = {.owner = host};
    if (length != 0 && length <= MO_LIMIT_HOST_NAME && memchr(name, 0, length) == NULL) {
        int64_t within = mo_cell_get_duration(args[3]);
        struct lookup *slot, found;
        if (!monotonic(&now))
            problem = (struct problem){.status = INTERNAL};
        else if (within <= 0)
            problem = (struct problem){.status = TIMED_OUT};
        else {
            connection.deadline =
                (uint64_t)within > UINT64_MAX - now ? UINT64_MAX : now + (uint64_t)within;
            problem = start_lookup(name, length, port, &slot);
            if (problem.status == READY)
                problem = finish_lookup(slot, connection.deadline, &found);
            if (problem.status == READY)
                problem = open_socket(&found, connection.deadline, &fd);
        }
    }
    if (problem.status != READY)
        return fail(host, heap, type, problem, args[1], port, out, error);
    connection.descriptor = fd;
    struct mo_capability_info info = {.operations = &mo_host_operations[false][MO_CAP_CONNECTION],
                                      .release = release};
    struct mo_cell value;
    if (!mo_value_capability_native(heap, payload(host, type, "Ok"), &info, &connection,
                                    sizeof connection, &value)) {
        mo_host_close(fd);
        return MO_HOST_OUT_OF_MEMORY;
    }
    host->open_connections++;
    return succeed(host, heap, type, value, out);
}

enum mo_host_outcome mo_host_network(struct mo_host *host, struct mo_heap *heap, uint32_t operation,
                                     uint32_t type, const struct mo_cell *args, struct mo_cell *out,
                                     struct mo_host_error *error) {
    if (operation == MO_HOST_NETWORK_CONNECT)
        return network_connect(host, heap, type, args, out, error);
    const struct connection *connection =
        (const struct connection *)mo_capability_get(args[0])->handle;
    if (operation == MO_HOST_CONNECTION_SEND) {
        size_t length;
        const uint8_t *bytes = mo_value_data(args[1], &length);
        struct problem problem = send_all(connection, bytes, length);
        if (problem.status != READY)
            return fail(host, heap, type, problem, mo_cell_unit(), 0, out, error);
        return succeed(host, heap, type, mo_cell_unit(), out);
    }
    MO_ASSERT(operation == MO_HOST_CONNECTION_RECEIVE);
    int64_t most = mo_cell_get_int(args[1]);
    if (most < 1 || most > MO_LIMIT_RECEIVE_BYTES)
        return mo_host_bug(error, "most below 1 or above the receive limit");
    struct mo_scratch scratch;
    mo_scratch_init(&scratch, heap);
    uint8_t *bytes = mo_scratch_resize(&scratch, NULL, 0, (size_t)most);
    if (bytes == NULL)
        return MO_HOST_OUT_OF_MEMORY;
    size_t count = 0;
    struct problem problem = receive_some(connection, bytes, (size_t)most, &count);
    struct mo_cell value;
    bool ok = problem.status != READY || mo_value_bytes(heap, bytes, count, &value);
    mo_scratch_free(&scratch);
    if (!ok)
        return MO_HOST_OUT_OF_MEMORY;
    if (problem.status != READY)
        return fail(host, heap, type, problem, mo_cell_unit(), 0, out, error);
    return succeed(host, heap, type, value, out);
}
