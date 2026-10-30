#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "host/host.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

enum listener_slot_state { SLOT_EMPTY, SLOT_PENDING, SLOT_OPEN, SLOT_HALF_CLOSED };

struct listener_slot {
    enum listener_slot_state state;
    uint64_t generation;
    int fd;
    struct mo_listener_fake_peer *peer;
};

struct mo_listener {
    struct mo_arena arena;
    struct mo_bearer bearer;
    uint64_t cookie, generation;
    int fd;
    uint16_t port;
    bool fake;
    struct mo_listener_time time;
    struct listener_slot slots[MO_LIMIT_MCP_CONNECTIONS];
};

static pthread_mutex_t cookie_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint64_t next_cookie;

const char *mo_listener_status_text(enum mo_listener_status status) {
    switch (status) {
    case MO_LISTENER_OK:
        return NULL;
    case MO_LISTENER_AGAIN:
        return "listener operation would block or was interrupted";
    case MO_LISTENER_EOF:
        return "listener connection reached end of input";
    case MO_LISTENER_IO:
        return "listener input or output failed";
    case MO_LISTENER_MEMORY:
        return "listener memory allocation failed";
    case MO_LISTENER_FULL:
        return "listener connection capacity exhausted";
    case MO_LISTENER_STALE:
        return "listener connection is no longer valid";
    case MO_LISTENER_CONFIG:
        return "listener configuration or request is invalid";
    case MO_LISTENER_CREDENTIAL:
        return "listener credential is missing or invalid";
    }
    MO_UNREACHABLE();
}

static enum mo_listener_status io_status(void) {
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
        return MO_LISTENER_AGAIN;
    if (errno == ENOMEM || errno == ENOBUFS)
        return MO_LISTENER_MEMORY;
    if (errno == EMFILE || errno == ENFILE)
        return MO_LISTENER_FULL;
    return MO_LISTENER_IO;
}

static enum mo_listener_status create_listener(const char *environment, bool fake,
                                               struct mo_listener **out) {
    struct mo_bearer bearer = {0};
    if (environment == NULL || !mo_bearer_read(&bearer, environment)) {
        mo_host_wipe(&bearer, sizeof bearer);
        return MO_LISTENER_CREDENTIAL;
    }
    if (out == NULL) {
        mo_host_wipe(&bearer, sizeof bearer);
        return MO_LISTENER_CONFIG;
    }
    struct mo_arena arena;
    mo_arena_init_confidential(&arena, sizeof(struct mo_listener) + MO_ARENA_CHUNK_HEADER);
    struct mo_listener *listener =
        mo_arena_allocate(&arena, sizeof *listener, _Alignof(struct mo_listener));
    if (listener == NULL) {
        mo_host_wipe(&bearer, sizeof bearer);
        mo_arena_free(&arena);
        return MO_LISTENER_MEMORY;
    }
    *listener = (struct mo_listener){.arena = arena, .bearer = bearer, .fd = -1, .fake = fake};
    mo_host_wipe(&bearer, sizeof bearer);
    for (size_t i = 0; i < MO_LIMIT_MCP_CONNECTIONS; i++)
        listener->slots[i].fd = -1;
    MO_ASSERT(pthread_mutex_lock(&cookie_mutex) == 0);
    if (next_cookie != UINT64_MAX)
        listener->cookie = ++next_cookie;
    MO_ASSERT(pthread_mutex_unlock(&cookie_mutex) == 0);
    if (listener->cookie == 0) {
        mo_listener_close(listener);
        return MO_LISTENER_FULL;
    }
    *out = listener;
    return MO_LISTENER_OK;
}

enum mo_listener_status mo_listener_fake(const char *environment, struct mo_listener **out) {
    return create_listener(environment, true, out);
}

static bool socket_options(int fd) {
    int status = fcntl(fd, F_GETFL);
    if (status < 0 || fcntl(fd, F_SETFL, status | O_NONBLOCK) < 0)
        return false;
    int flags = fcntl(fd, F_GETFD);
    if (flags < 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0)
        return false;
#if defined(SO_NOSIGPIPE)
    int enabled = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof enabled) != 0)
        return false;
#elif !defined(MSG_NOSIGNAL)
    errno = ENOTSUP;
    return false;
#endif
    return true;
}

enum mo_listener_status mo_listener_open(const struct mo_listener_config *config,
                                         const char *environment, struct mo_listener **out) {
    struct mo_listener *listener = NULL;
    enum mo_listener_status status = create_listener(environment, false, &listener);
    if (status != MO_LISTENER_OK)
        return status;
    status = MO_LISTENER_CONFIG;
    if (config == NULL || out == NULL)
        goto fail;
    const char *address = config->address == NULL ? "127.0.0.1" : config->address;
    if (strnlen(address, INET6_ADDRSTRLEN) == INET6_ADDRSTRLEN)
        goto fail;
    struct sockaddr_in v4 = {.sin_family = AF_INET, .sin_port = htons(config->port)};
    struct sockaddr_in6 v6 = {.sin6_family = AF_INET6, .sin6_port = htons(config->port)};
    const struct sockaddr *target;
    socklen_t length;
    int family;
    if (inet_pton(AF_INET, address, &v4.sin_addr) == 1) {
        if (!config->expose && (ntohl(v4.sin_addr.s_addr) >> 24) != 127)
            goto fail;
        family = AF_INET;
        target = (const struct sockaddr *)&v4;
        length = sizeof v4;
    } else if (inet_pton(AF_INET6, address, &v6.sin6_addr) == 1) {
        if (!config->expose && !IN6_IS_ADDR_LOOPBACK(&v6.sin6_addr))
            goto fail;
        family = AF_INET6;
        target = (const struct sockaddr *)&v6;
        length = sizeof v6;
    } else
        goto fail;
    int type = SOCK_STREAM;
#if defined(SOCK_NONBLOCK) && defined(SOCK_CLOEXEC)
    type |= SOCK_NONBLOCK | SOCK_CLOEXEC;
#endif
    listener->fd = socket(family, type, 0);
    if (listener->fd < 0) {
        status = io_status();
        goto fail;
    }
    if (!socket_options(listener->fd)) {
        status = io_status();
        goto fail;
    }
    if (family == AF_INET6) {
        int enabled = 1;
        if (setsockopt(listener->fd, IPPROTO_IPV6, IPV6_V6ONLY, &enabled, sizeof enabled) != 0) {
            status = io_status();
            goto fail;
        }
    }
    if (bind(listener->fd, target, length) != 0 ||
        listen(listener->fd, MO_LIMIT_MCP_CONNECTIONS) != 0) {
        status = io_status();
        goto fail;
    }
    struct sockaddr_storage bound;
    socklen_t bound_length = sizeof bound;
    if (getsockname(listener->fd, (struct sockaddr *)&bound, &bound_length) != 0) {
        status = io_status();
        goto fail;
    }
    if (family == AF_INET) {
        struct sockaddr_in actual;
        memcpy(&actual, &bound, sizeof actual);
        listener->port = ntohs(actual.sin_port);
    } else {
        struct sockaddr_in6 actual;
        memcpy(&actual, &bound, sizeof actual);
        listener->port = ntohs(actual.sin6_port);
    }
    *out = listener;
    return MO_LISTENER_OK;
fail:
    mo_listener_close(listener);
    return status;
}

static enum mo_listener_status close_slot(struct listener_slot *slot) {
    int fd = slot->fd;
    if (slot->peer != NULL)
        slot->peer->closed = true;
    mo_host_wipe(slot, sizeof *slot);
    slot->fd = -1;
    return fd < 0 || close(fd) == 0 ? MO_LISTENER_OK : MO_LISTENER_IO;
}

void mo_listener_close(struct mo_listener *listener) {
    if (listener == NULL)
        return;
    for (size_t i = 0; i < MO_LIMIT_MCP_CONNECTIONS; i++)
        (void)close_slot(&listener->slots[i]);
    if (listener->fd >= 0)
        (void)close(listener->fd);
    struct mo_arena arena = listener->arena;
    mo_host_wipe(listener, sizeof *listener);
    mo_arena_free(&arena);
    mo_host_wipe(&arena, sizeof arena);
}

uint16_t mo_listener_port(const struct mo_listener *listener) {
    MO_ASSERT(listener != NULL);
    return listener->port;
}

static size_t empty_slot(const struct mo_listener *listener) {
    for (size_t i = 0; i < MO_LIMIT_MCP_CONNECTIONS; i++)
        if (listener->slots[i].state == SLOT_EMPTY)
            return i;
    return MO_LIMIT_MCP_CONNECTIONS;
}

static struct listener_slot *lookup(struct mo_listener *listener, struct mo_listener_key key) {
    if (key.cookie != listener->cookie || key.slot >= MO_LIMIT_MCP_CONNECTIONS)
        return NULL;
    struct listener_slot *slot = &listener->slots[key.slot];
    if ((slot->state != SLOT_OPEN && slot->state != SLOT_HALF_CLOSED) ||
        slot->generation != key.generation)
        return NULL;
    return slot;
}

enum mo_listener_status mo_listener_fake_connect(struct mo_listener *listener,
                                                 struct mo_listener_fake_peer *peer) {
    if (listener == NULL || !listener->fake || peer == NULL ||
        (peer->input == NULL && peer->input_size != 0) ||
        (peer->output == NULL && peer->output_capacity != 0))
        return MO_LISTENER_CONFIG;
    for (size_t i = 0; i < MO_LIMIT_MCP_CONNECTIONS; i++)
        if (listener->slots[i].peer == peer)
            return MO_LISTENER_CONFIG;
    size_t index = empty_slot(listener);
    if (index == MO_LIMIT_MCP_CONNECTIONS || listener->generation == UINT64_MAX)
        return MO_LISTENER_FULL;
    listener->slots[index] = (struct listener_slot){
        .state = SLOT_PENDING, .generation = ++listener->generation, .fd = -1, .peer = peer};
    peer->input_offset = 0;
    peer->output_size = 0;
    peer->closed = false;
    peer->half_closed = false;
    return MO_LISTENER_OK;
}

enum mo_listener_status mo_listener_fake_advance(struct mo_listener *listener,
                                                 uint64_t microseconds) {
    if (listener == NULL || !listener->fake || microseconds > INT64_MAX ||
        microseconds > (uint64_t)(INT64_MAX - listener->time.wall_us) ||
        microseconds > UINT64_MAX - listener->time.monotonic_us)
        return MO_LISTENER_CONFIG;
    listener->time.monotonic_us += microseconds;
    listener->time.wall_us += (int64_t)microseconds;
    return MO_LISTENER_OK;
}

static enum mo_listener_status sample_time(const struct mo_listener *listener,
                                           struct mo_listener_time *out) {
    if (listener->fake) {
        *out = listener->time;
        return MO_LISTENER_OK;
    }
    struct timespec wall, monotonic;
    if (clock_gettime(CLOCK_MONOTONIC, &monotonic) != 0 ||
        clock_gettime(CLOCK_REALTIME, &wall) != 0)
        return io_status();
    if (monotonic.tv_sec < 0 || (uint64_t)monotonic.tv_sec > UINT64_MAX / 1000000 ||
        wall.tv_sec < 0 || (uint64_t)wall.tv_sec > (uint64_t)INT64_MAX / 1000000)
        return MO_LISTENER_IO;
    uint64_t mono_seconds = (uint64_t)monotonic.tv_sec * 1000000;
    uint64_t wall_seconds = (uint64_t)wall.tv_sec * 1000000;
    uint64_t mono_fraction = (uint64_t)monotonic.tv_nsec / 1000;
    uint64_t wall_fraction = (uint64_t)wall.tv_nsec / 1000;
    if (mono_fraction > UINT64_MAX - mono_seconds ||
        wall_fraction > (uint64_t)INT64_MAX - wall_seconds)
        return MO_LISTENER_IO;
    *out = (struct mo_listener_time){mono_seconds + mono_fraction,
                                     (int64_t)(wall_seconds + wall_fraction)};
    return MO_LISTENER_OK;
}

static size_t pending_slot(const struct mo_listener *listener) {
    size_t found = MO_LIMIT_MCP_CONNECTIONS;
    for (size_t i = 0; i < MO_LIMIT_MCP_CONNECTIONS; i++)
        if (listener->slots[i].state == SLOT_PENDING &&
            (found == MO_LIMIT_MCP_CONNECTIONS ||
             listener->slots[i].generation < listener->slots[found].generation))
            found = i;
    return found;
}

static enum mo_listener_status accept_connection(struct mo_listener *listener,
                                                 struct mo_listener_key *out) {
    size_t index;
    if (listener->fake) {
        index = pending_slot(listener);
        if (index == MO_LIMIT_MCP_CONNECTIONS)
            return empty_slot(listener) == MO_LIMIT_MCP_CONNECTIONS ? MO_LISTENER_FULL
                                                                    : MO_LISTENER_AGAIN;
    } else {
        index = empty_slot(listener);
        if (index == MO_LIMIT_MCP_CONNECTIONS || listener->generation == UINT64_MAX)
            return MO_LISTENER_FULL;
#if defined(__linux__)
        int fd = accept4(listener->fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
#else
        int fd = accept(listener->fd, NULL, NULL);
#endif
        if (fd < 0)
            return io_status();
        if (!socket_options(fd)) {
            enum mo_listener_status status = io_status();
            (void)close(fd);
            return status;
        }
        listener->slots[index].fd = fd;
        listener->slots[index].generation = ++listener->generation;
    }
    struct listener_slot *slot = &listener->slots[index];
    slot->state = SLOT_OPEN;
    *out = (struct mo_listener_key){listener->cookie, slot->generation, index};
    return MO_LISTENER_OK;
}

static unsigned fake_events(const struct listener_slot *slot, unsigned interest) {
    const struct mo_listener_fake_peer *peer = slot->peer;
    unsigned ready = 0;
    if ((interest & MO_LISTENER_READABLE) != 0) {
        if (peer->read_error)
            ready |= MO_LISTENER_ERROR;
        if ((peer->readable && peer->input_offset < peer->input_size) ||
            (peer->eof && peer->input_offset == peer->input_size))
            ready |= MO_LISTENER_READABLE;
    }
    if ((interest & MO_LISTENER_WRITABLE) != 0) {
        if (peer->write_error || slot->state == SLOT_HALF_CLOSED)
            ready |= MO_LISTENER_ERROR;
        else if (peer->writable && peer->output_size < peer->output_capacity)
            ready |= MO_LISTENER_WRITABLE;
    }
    return ready;
}

static enum mo_listener_status wait_ready(struct mo_listener *listener,
                                          const struct mo_listener_wait *request,
                                          struct mo_listener_ready *out) {
    if (request->count > MO_LIMIT_MCP_CONNECTIONS)
        return MO_LISTENER_CONFIG;
    struct pollfd descriptors[MO_LIMIT_MCP_CONNECTIONS + 1] = {0};
    bool accepting =
        request->accept && (listener->fake ? pending_slot(listener) != MO_LIMIT_MCP_CONNECTIONS
                                           : empty_slot(listener) != MO_LIMIT_MCP_CONNECTIONS);
    descriptors[0] = (struct pollfd){.fd = accepting ? listener->fd : -1, .events = POLLIN};
    for (size_t i = 0; i < request->count; i++) {
        const struct mo_listener_interest *interest = &request->interests[i];
        if ((interest->events & ~(unsigned)(MO_LISTENER_READABLE | MO_LISTENER_WRITABLE)) != 0)
            return MO_LISTENER_CONFIG;
        struct listener_slot *slot = lookup(listener, interest->connection);
        if (slot == NULL)
            return MO_LISTENER_STALE;
        for (size_t j = 0; j < i; j++)
            if (request->interests[j].connection.slot == interest->connection.slot)
                return MO_LISTENER_CONFIG;
        if (listener->fake)
            out->events[i] = fake_events(slot, interest->events);
        else {
            descriptors[i + 1].fd = slot->fd;
            if ((interest->events & MO_LISTENER_READABLE) != 0)
                descriptors[i + 1].events |= POLLIN;
            if ((interest->events & MO_LISTENER_WRITABLE) != 0)
                descriptors[i + 1].events |= POLLOUT;
        }
    }
    if (listener->fake) {
        out->accept = accepting;
        out->now = listener->time;
        return MO_LISTENER_OK;
    }
    struct mo_listener_time before;
    enum mo_listener_status status = sample_time(listener, &before);
    if (status != MO_LISTENER_OK)
        return status;
    uint64_t remaining =
        request->until_us > before.monotonic_us ? request->until_us - before.monotonic_us : 0;
    uint64_t milliseconds = remaining / 1000;
    int timeout = milliseconds > INT_MAX ? INT_MAX : (int)milliseconds;
    int polled = poll(descriptors, (nfds_t)(request->count + 1), timeout);
    if (polled < 0)
        return io_status();
    if ((descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
        return MO_LISTENER_IO;
    out->accept = (descriptors[0].revents & POLLIN) != 0;
    for (size_t i = 0; i < request->count; i++) {
        short events = descriptors[i + 1].revents;
        if ((events & (POLLIN | POLLHUP)) != 0 &&
            (request->interests[i].events & MO_LISTENER_READABLE) != 0)
            out->events[i] |= MO_LISTENER_READABLE;
        if ((events & POLLOUT) != 0)
            out->events[i] |= MO_LISTENER_WRITABLE;
        if ((events & (POLLERR | POLLNVAL)) != 0)
            out->events[i] |= MO_LISTENER_ERROR;
    }
    return sample_time(listener, &out->now);
}

static enum mo_listener_status read_bytes(struct mo_listener *listener,
                                          const struct mo_listener_read *request, size_t *out) {
    struct listener_slot *slot = lookup(listener, request->connection);
    if (slot == NULL)
        return MO_LISTENER_STALE;
    if ((request->bytes == NULL && request->capacity != 0) || request->capacity > (size_t)SSIZE_MAX)
        return MO_LISTENER_CONFIG;
    if (request->capacity == 0)
        return MO_LISTENER_OK;
    if (!listener->fake) {
        ssize_t count = recv(slot->fd, request->bytes, request->capacity, 0);
        if (count < 0)
            return io_status();
        *out = (size_t)count;
        return count == 0 ? MO_LISTENER_EOF : MO_LISTENER_OK;
    }
    struct mo_listener_fake_peer *peer = slot->peer;
    if (peer->read_error)
        return MO_LISTENER_IO;
    if (peer->input_offset == peer->input_size)
        return peer->eof ? MO_LISTENER_EOF : MO_LISTENER_AGAIN;
    if (!peer->readable)
        return MO_LISTENER_AGAIN;
    size_t count = peer->input_size - peer->input_offset;
    if (count > request->capacity)
        count = request->capacity;
    if (peer->read_quantum != 0 && count > peer->read_quantum)
        count = peer->read_quantum;
    memcpy(request->bytes, peer->input + peer->input_offset, count);
    peer->input_offset += count;
    *out = count;
    return MO_LISTENER_OK;
}

static enum mo_listener_status write_bytes(struct mo_listener *listener,
                                           const struct mo_listener_write *request, size_t *out) {
    struct listener_slot *slot = lookup(listener, request->connection);
    if (slot == NULL)
        return MO_LISTENER_STALE;
    if ((request->bytes == NULL && request->size != 0) || request->size > (size_t)SSIZE_MAX)
        return MO_LISTENER_CONFIG;
    if (slot->state == SLOT_HALF_CLOSED)
        return MO_LISTENER_IO;
    if (request->size == 0)
        return MO_LISTENER_OK;
    if (!listener->fake) {
        int flags = 0;
#if defined(MSG_NOSIGNAL)
        flags = MSG_NOSIGNAL;
#endif
        ssize_t count = send(slot->fd, request->bytes, request->size, flags);
        if (count < 0)
            return io_status();
        *out = (size_t)count;
        return MO_LISTENER_OK;
    }
    struct mo_listener_fake_peer *peer = slot->peer;
    if (peer->write_error)
        return MO_LISTENER_IO;
    if (!peer->writable || peer->output_size == peer->output_capacity)
        return MO_LISTENER_AGAIN;
    size_t count = peer->output_capacity - peer->output_size;
    if (count > request->size)
        count = request->size;
    if (peer->write_quantum != 0 && count > peer->write_quantum)
        count = peer->write_quantum;
    memcpy(peer->output + peer->output_size, request->bytes, count);
    peer->output_size += count;
    *out = count;
    return MO_LISTENER_OK;
}

enum mo_listener_status mo_listener_call(struct mo_listener *listener,
                                         enum mo_listener_operation operation,
                                         const union mo_listener_request *request,
                                         union mo_listener_result *out) {
    if (out == NULL)
        return MO_LISTENER_CONFIG;
    memset(out, 0, sizeof *out);
    if (listener == NULL ||
        (request == NULL && operation != MO_LISTENER_NOW && operation != MO_LISTENER_ACCEPT))
        return MO_LISTENER_CONFIG;
    switch (operation) {
    case MO_LISTENER_NOW:
        return sample_time(listener, &out->now);
    case MO_LISTENER_WAIT:
        return wait_ready(listener, &request->wait, &out->wait);
    case MO_LISTENER_ACCEPT:
        return accept_connection(listener, &out->accepted);
    case MO_LISTENER_READ:
        return read_bytes(listener, &request->read, &out->transferred);
    case MO_LISTENER_WRITE:
        return write_bytes(listener, &request->write, &out->transferred);
    case MO_LISTENER_AUTH: {
        const struct mo_listener_auth *auth = &request->auth;
        if ((auth->bytes == NULL && auth->size != 0) || auth->start > auth->size ||
            auth->length > auth->size - auth->start)
            return MO_LISTENER_CONFIG;
        const uint8_t *bytes = auth->bytes == NULL ? NULL : auth->bytes + auth->start;
        out->authenticated = mo_bearer_check(&listener->bearer, bytes, auth->length);
        return MO_LISTENER_OK;
    }
    case MO_LISTENER_HALF_CLOSE: {
        struct listener_slot *slot = lookup(listener, request->connection);
        if (slot == NULL)
            return MO_LISTENER_STALE;
        if (slot->state == SLOT_HALF_CLOSED)
            return MO_LISTENER_OK;
        if (listener->fake)
            slot->peer->half_closed = true;
        else if (shutdown(slot->fd, SHUT_WR) != 0)
            return io_status();
        slot->state = SLOT_HALF_CLOSED;
        return MO_LISTENER_OK;
    }
    case MO_LISTENER_CLOSE: {
        struct listener_slot *slot = lookup(listener, request->connection);
        return slot == NULL ? MO_LISTENER_STALE : close_slot(slot);
    }
    }
    return MO_LISTENER_CONFIG;
}
