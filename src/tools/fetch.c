#define _POSIX_C_SOURCE 200809L
#include "base/binary.h"
#include "base/export.h"
#include "base/sha256.h"
#include "base/stack.h"
#include "tools/protocol.h"
#include "tools/run.h"
#include "tools/tools.h"
#include "tools/upgrade.h"
#include "vm/vm.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t cancelled;
static void cancel(int number) {
    (void)number;
    cancelled = 1;
}
static uint64_t now(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0)
        return UINT64_MAX;
    return (uint64_t)t.tv_sec * UINT64_C(1000000) + (uint64_t)t.tv_nsec / 1000;
}
struct fetch_timer {
    timer_t id;
    struct sigaction action;
    sigset_t mask;
    bool masked, installed, created;
};
static bool timer_start(struct fetch_timer *timer, uint64_t end) {
    sigset_t alarms;
    if (sigemptyset(&alarms) != 0 || sigaddset(&alarms, SIGALRM) != 0 ||
        sigprocmask(SIG_BLOCK, &alarms, &timer->mask) != 0)
        return false;
    timer->masked = true;
    struct sigaction action = {.sa_handler = cancel};
    if (sigemptyset(&action.sa_mask) != 0 || sigaction(SIGALRM, &action, &timer->action) != 0)
        return false;
    timer->installed = true;
    struct sigevent event = {.sigev_notify = SIGEV_SIGNAL, .sigev_signo = SIGALRM};
    if (timer_create(CLOCK_MONOTONIC, &event, &timer->id) != 0)
        return false;
    timer->created = true;
    struct itimerspec deadline = {
        .it_value = {.tv_sec = (time_t)(end / 1000000), .tv_nsec = (long)((end % 1000000) * 1000)},
        .it_interval = {.tv_sec = 1}};
    sigset_t running = timer->mask;
    return timer_settime(timer->id, TIMER_ABSTIME, &deadline, NULL) == 0 &&
           sigdelset(&running, SIGALRM) == 0 && sigprocmask(SIG_SETMASK, &running, NULL) == 0;
}
static void timer_stop(struct fetch_timer *timer) {
    if (!timer->masked)
        return;
    sigset_t alarms;
    MO_ASSERT(sigemptyset(&alarms) == 0 && sigaddset(&alarms, SIGALRM) == 0);
    MO_ASSERT(sigprocmask(SIG_BLOCK, &alarms, NULL) == 0);
    if (timer->created)
        MO_ASSERT(timer_delete(timer->id) == 0);
    if (timer->installed) {
        struct timespec zero = {0};
        for (;;) {
            if (sigtimedwait(&alarms, NULL, &zero) >= 0 || errno == EINTR)
                continue;
            MO_ASSERT(errno == EAGAIN);
            break;
        }
        MO_ASSERT(sigaction(SIGALRM, &timer->action, NULL) == 0);
    }
    MO_ASSERT(sigprocmask(SIG_SETMASK, &timer->mask, NULL) == 0);
}
static bool ready(int fd, short events, uint64_t end) {
    while (!cancelled && now() < end) {
        struct pollfd p = {.fd = fd, .events = events};
        int n = poll(&p, 1, 100);
        if (n > 0)
            return (p.revents & (events | POLLHUP)) != 0;
        if (n < 0 && errno != EINTR)
            return false;
    }
    return false;
}
static bool loopback(const struct sockaddr *a) {
    if (a->sa_family == AF_INET)
        return (ntohl(((const struct sockaddr_in *)a)->sin_addr.s_addr) >> 24) == 127;
    if (a->sa_family == AF_INET6)
        return IN6_IS_ADDR_LOOPBACK(&((const struct sockaddr_in6 *)a)->sin6_addr);
    return false;
}
static bool url(const char *s, char host[10], char port[6]) {
    if (strnlen(s, MO_LIMIT_FETCH_URL_BYTES + 1) > MO_LIMIT_FETCH_URL_BYTES ||
        strncmp(s, "http://", 7) != 0)
        return false;
    s += 7;
    const char *hosts[] = {"127.0.0.1", "localhost", "[::1]"};
    size_t i;
    for (i = 0; i < 3; i++) {
        size_t n = strlen(hosts[i]);
        if (strncmp(s, hosts[i], n) == 0 && s[n] == ':') {
            s += n + 1;
            break;
        }
    }
    if (i == 3)
        return false;
    strcpy(host, i == 2 ? "::1" : hosts[i]);
    size_t n = 0;
    unsigned value = 0;
    while (*s >= '0' && *s <= '9') {
        if (n == 5)
            return false;
        value = value * 10 + (unsigned)(*s - '0');
        port[n++] = *s++;
    }
    port[n] = 0;
    return n != 0 && value > 0 && value <= 65535 && strcmp(s, "/mcp") == 0;
}
static int connect_local(const char *host, const char *port, uint64_t end) {
    struct addrinfo hints = {.ai_family = AF_UNSPEC,
                             .ai_socktype = SOCK_STREAM,
                             .ai_flags = AI_NUMERICSERV | AI_NUMERICHOST},
                    *addresses;
    if (getaddrinfo(strcmp(host, "localhost") == 0 ? "127.0.0.1" : host, port, &hints,
                    &addresses) != 0)
        return -1;
    int fd = -1;
    /* Refuse the entire resolution if it contains a non-loopback address. */
    for (struct addrinfo *a = addresses; a != NULL; a = a->ai_next)
        if (!loopback(a->ai_addr)) {
            freeaddrinfo(addresses);
            return -1;
        }
    for (struct addrinfo *a = addresses; a != NULL; a = a->ai_next) {
        fd = socket(a->ai_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0)
            continue;
        int receive_bytes = MO_LIMIT_FETCH_RECEIVE_BYTES;
        if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &receive_bytes, sizeof receive_bytes) != 0) {
            (void)close(fd);
            fd = -1;
            continue;
        }
        int connected = connect(fd, a->ai_addr, a->ai_addrlen);
        int error = 0;
        socklen_t size = sizeof error;
        struct sockaddr_storage peer;
        socklen_t peer_size = sizeof peer;
        if ((connected == 0 || (errno == EINPROGRESS && ready(fd, POLLOUT, end))) &&
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) == 0 && error == 0 &&
            getpeername(fd, (struct sockaddr *)&peer, &peer_size) == 0 &&
            loopback((struct sockaddr *)&peer))
            break;
        (void)close(fd);
        fd = -1;
    }
    freeaddrinfo(addresses);
    if (fd < 0 && strcmp(host, "localhost") == 0)
        return connect_local("::1", port, end);
    return fd;
}
static bool send_request(int fd, const char *request, size_t length, uint64_t end) {
    size_t at = 0;
    while (at < length && ready(fd, POLLOUT, end)) {
        ssize_t n = send(fd, request + at, length - at, MSG_NOSIGNAL);
        if (n > 0)
            at += (size_t)n;
        else if (n == 0 || (errno != EINTR && errno != EAGAIN))
            return false;
    }
    return at == length;
}

static bool receive_response(int fd, struct mo_buffer *wire, uint64_t end) {
    uint8_t bytes[4096];
    while (ready(fd, POLLIN, end)) {
        ssize_t n = recv(fd, bytes, sizeof bytes, 0);
        if (n == 0)
            return true;
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            return errno == ECONNRESET;
        }
        if (!mo_buffer_append(wire, bytes, (size_t)n))
            return false;
    }
    return false;
}
static bool decimal(const char *s, uint64_t *out) {
    uint64_t n = 0;
    if (*s == 0 || (*s == '0' && s[1] != 0))
        return false;
    for (; *s; s++) {
        if (*s < '0' || *s > '9' || n > (UINT64_MAX - (unsigned)(*s - '0')) / 10)
            return false;
        n = n * 10 + (unsigned)(*s - '0');
    }
    *out = n;
    return true;
}
static bool unhex(const char *s, uint8_t *out, size_t n) {
    static const char alphabet[] = "0123456789abcdef";
    if (strlen(s) != n * 2)
        return false;
    for (size_t i = 0; i < n * 2; i++) {
        const char *p = strchr(alphabet, s[i]);
        if (p == NULL)
            return false;
        unsigned v = (unsigned)(p - alphabet);
        if (i % 2 == 0)
            out[i / 2] = (uint8_t)(v << 4);
        else
            out[i / 2] |= (uint8_t)v;
    }
    return true;
}
static bool base64(const char *s, uint8_t *out, size_t length) {
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t encoded = strlen(s);
    if (encoded != 4 * ((length + 2) / 3))
        return false;
    uint32_t bits = 0;
    unsigned count = 0;
    size_t at = 0;
    for (size_t i = 0; i < encoded; i++) {
        if (s[i] == '=') {
            if (i < encoded - 2 || at != length)
                return false;
            continue;
        }
        const char *p = strchr(alphabet, s[i]);
        if (p == NULL || (i > 0 && s[i - 1] == '='))
            return false;
        bits = (bits << 6) | (uint32_t)(p - alphabet);
        count += 6;
        if (count >= 8) {
            count -= 8;
            if (at >= length)
                return false;
            out[at++] = (uint8_t)(bits >> count);
        }
    }
    return at == length && (bits & ((UINT32_C(1) << count) - 1)) == 0;
}

enum mo_fetch_read_status mo_fetch_read(struct mo_image *image, uint32_t function,
                                        const struct mo_buffer *wire, struct mo_export_page *out) {
    struct mo_limits limits = mo_limits_default();
    limits.heap_size = MO_LIMIT_EXPORT_SCRATCH;
    struct mo_process *process = mo_process_new(image, &limits);
    if (process == NULL)
        return MO_FETCH_REFUSED;
    mo_process_confidential(process);
    struct mo_heap *heap = mo_process_heap(process);
    struct mo_cell input, output;
    enum mo_fetch_read_status status = MO_FETCH_REFUSED;
    bool ok = mo_value_text(heap, (const char *)wire->bytes, wire->length, &input);
    char fields[MO_LIMIT_FETCH_PAGE_FIELDS][MO_LIMIT_FETCH_FIELD_BYTES];
    if (ok) {
        struct mo_defect defect;
        mo_process_instruction_limit(process, MO_LIMIT_MCP_REQUEST_WORK);
        ok = mo_call(process, function, &input, 1, &output, &defect) == MO_RETURNED;
        const struct mo_bytecode_function *fn = &mo_image_bytecode(image)->functions[function];
        if (fn->parameters[0].ownership != MO_PARAMETER_OWNED)
            mo_value_drop(heap, fn->parameters[0].type, input);
        if (ok) {
            uint32_t count = mo_list_size(output);
            ok = count == MO_LIMIT_FETCH_PAGE_FIELDS || count == 1;
            for (uint32_t i = 0; ok && i < count; i++) {
                struct mo_cell item;
                MO_ASSERT(mo_list_get(output, i, &item));
                size_t size;
                const uint8_t *bytes = mo_value_data(item, &size);
                ok = size < sizeof fields[i] && memchr(bytes, 0, size) == NULL;
                if (ok) {
                    memcpy(fields[i], bytes, size);
                    fields[i][size] = 0;
                }
            }
            mo_value_drop(heap, fn->result_type, output);
            if (ok && count == 1) {
                if (strcmp(fields[0], "export_replaced") == 0)
                    status = MO_FETCH_REPLACED;
                if (strcmp(fields[0], "export_released") == 0)
                    status = MO_FETCH_RELEASED;
                ok = false;
            }
        }
    }
    if (ok) {
        struct mo_export_page page = {0};
        uint64_t numbers[6];
        uint8_t checksum[8];
        ok = unhex(fields[0], page.id, 16) && unhex(fields[7], checksum, 8);
        for (size_t i = 0; ok && i < 6; i++)
            ok = decimal(fields[i + 1], &numbers[i]);
        ok = ok && numbers[4] >= MO_EXPORT_FIXED_BYTES && numbers[4] <= SIZE_MAX &&
             numbers[2] <= SIZE_MAX &&
             mo_export_page_bounds((size_t)numbers[4], (size_t)numbers[2], &page.length,
                                   &page.count) &&
             numbers[3] == page.length && numbers[5] == page.count &&
             base64(fields[9], page.bytes, page.length);
        if (ok) {
            page.sequence = numbers[0];
            page.encoded_sequence = numbers[1];
            page.offset = (size_t)numbers[2];
            page.total = (size_t)numbers[4];
            page.final = page.length == page.total - page.offset;
            for (size_t i = 0; i < 8; i++)
                page.checksum = (page.checksum << 8) | checksum[i];
            ok = mo_binary_checksum(page.bytes, page.length) == page.checksum &&
                 (page.final ? unhex(fields[8], page.digest, 32) : fields[8][0] == 0);
            if (ok) {
                *out = page;
                status = MO_FETCH_PAGE;
            }
        }
    }
    mo_process_free(process);
    return status;
}

struct fetch_endpoint {
    uint64_t failure;
    const char *host, *port, *token;
    uint64_t end, last_sent;
};
static int request_page(struct fetch_endpoint *endpoint, const char *id, size_t offset) {
    uint64_t interval = UINT64_C(1000000) / MO_LIMIT_MCP_RATE + 5000;
    uint64_t elapsed = now() - endpoint->last_sent;
    if (endpoint->last_sent != 0 && elapsed < interval) {
        struct timespec pause = {.tv_nsec = (long)((interval - elapsed) * 1000)};
        if (nanosleep(&pause, NULL) != 0 && errno != EINTR)
            return -1;
    }
    if (cancelled || now() >= endpoint->end)
        return -1;
    char body[512], request[2048];
    int n;
    char selector[96];
    if (endpoint->failure == 0)
        n = snprintf(selector, sizeof selector, "\"kind\":\"state\"");
    else
        n = snprintf(selector, sizeof selector, "\"kind\":\"failure\",\"failure\":%llu",
                     (unsigned long long)endpoint->failure);
    if (n <= 0 || (size_t)n >= sizeof selector)
        return -1;
    if (offset == 0)
        n = snprintf(body, sizeof body,
                     "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/"
                     "call\",\"params\":{\"name\":\"mo-export\",\"arguments\":{%s}}}",
                     selector);
    else
        n = snprintf(body, sizeof body,
                     "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/"
                     "call\",\"params\":{\"name\":\"mo-export\",\"arguments\":{%s,\"export\":\"%"
                     "s\",\"offset\":%zu}}}",
                     selector, id, offset);
    if (n <= 0 || (size_t)n >= sizeof body)
        return -1;
    int fd = connect_local(endpoint->host, endpoint->port, endpoint->end);
    if (fd < 0)
        return -1;
    n = snprintf(request, sizeof request,
                 "POST /mcp HTTP/1.1\r\nHost: %s:%s\r\nAuthorization: Bearer %s\r\nContent-Type: "
                 "application/json\r\nMCP-Protocol-Version: 2025-11-25\r\nConnection: "
                 "close\r\nContent-Length: %zu\r\n\r\n%s",
                 strcmp(endpoint->host, "::1") == 0 ? "[::1]" : endpoint->host, endpoint->port,
                 endpoint->token, strlen(body), body);
    bool ok =
        n > 0 && (size_t)n < sizeof request && send_request(fd, request, (size_t)n, endpoint->end);
    for (size_t i = 0; i < sizeof request; i++)
        ((volatile char *)request)[i] = 0;
    if (!ok) {
        (void)close(fd);
        return -1;
    }
    endpoint->last_sent = now();
    return fd;
}

static int fetch_attempt(const struct mo_cli *cli, FILE *errors, const char *host, const char *port,
                         uint64_t end) {
    size_t output_size = strnlen(cli->output, MO_LIMIT_PATH_SIZE + 1);
    if (output_size == 0 || output_size > MO_LIMIT_PATH_SIZE ||
        strnlen(cli->fetch_token, MO_CLI_ARGUMENT_SIZE + 1) > MO_CLI_ARGUMENT_SIZE ||
        cli->fetch_token[0] == 0) {
        (void)fputs("mo fetch: invalid output path or environment name\n", errors);
        return 2;
    }
    struct stat st;
    if (lstat(cli->output, &st) == 0 || errno != ENOENT) {
        (void)fputs("mo fetch: destination exists or cannot be checked\n", errors);
        return 1;
    }
    const char *token = getenv(cli->fetch_token);
    size_t token_size = token == NULL ? 0 : strnlen(token, MO_LIMIT_MCP_TOKEN_BYTES + 1);
    if (token_size == 0 || token_size > MO_LIMIT_MCP_TOKEN_BYTES) {
        (void)fputs("mo fetch: token unavailable or invalid\n", errors);
        return 1;
    }
    for (size_t i = 0; i < token_size; i++)
        if ((unsigned char)token[i] <= 32 || (unsigned char)token[i] >= 127) {
            (void)fputs("mo fetch: token unavailable or invalid\n", errors);
            return 1;
        }
    struct mo_image *image = mo_protocol_build();
    if (image == NULL) {
        (void)fputs("mo fetch: reader unavailable\n", errors);
        return 1;
    }
    struct mo_limits limits = mo_limits_default();
    limits.heap_size = MO_LIMIT_EXPORT_SCRATCH;
    struct mo_defect initialization;
    if (mo_image_init(image, &limits, &initialization) != MO_RETURNED) {
        mo_image_free(image);
        (void)fputs("mo fetch: reader initialization failed\n", errors);
        return 1;
    }
    uint32_t function = MO_NO_INDEX;
    const struct mo_bytecode *file = mo_image_bytecode(image);
    for (uint32_t i = 0; i < file->function_count; i++) {
        const struct mo_bytecode_string *name = &file->strings[file->functions[i].name];
        if (name->length == 13 && memcmp(name->bytes, "McpFetch.read", 13) == 0)
            function = i;
    }
    struct mo_arena arena;
    mo_arena_init_confidential(&arena, MO_LIMIT_EXPORT_BYTES);
    size_t path_size = output_size + 64;
    char *temporary = mo_arena_allocate(&arena, path_size, 1);
    int fd = -1;
    bool ok = temporary != NULL && function != MO_NO_INDEX && now() < end;
    if (ok) {
        int n = snprintf(temporary, path_size, "%s.mo-fetch-%ld", cli->output, (long)getpid());
        ok = n > 0 && (size_t)n < path_size;
    }
    struct sigaction action = {.sa_handler = cancel}, old_int, old_term;
    (void)sigemptyset(&action.sa_mask);
    bool sigint_set = sigaction(SIGINT, &action, &old_int) == 0;
    bool sigterm_set = sigaction(SIGTERM, &action, &old_term) == 0;
    ok = ok && sigint_set && sigterm_set && !cancelled && now() < end;
    if (ok) {
        fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        ok = fd >= 0 && fchmod(fd, 0600) == 0;
    }
    bool created = fd >= 0;
    (void)fputs(cli->fetch_failure != 0
                    ? "mo fetch: maximum failure pair needs 2176 pages, about 109 seconds at 20 "
                      "calls/second\n"
                    : "mo fetch: maximum state needs 2049 pages, about 103 seconds at 20 "
                      "calls/second\n",
                errors);
    struct mo_export_consumer consumer = {0};
    enum mo_fetch_read_status status = MO_FETCH_REFUSED;
    char export_id[33] = "";
    struct fetch_endpoint endpoint = {
        .failure = cli->fetch_failure, .host = host, .port = port, .token = token, .end = end};
    int pending = -1;
    while (ok && !cancelled && now() < end) {
        int socket_fd =
            pending >= 0 ? pending : request_page(&endpoint, export_id, consumer.offset);
        pending = -1;
        if (socket_fd < 0) {
            ok = false;
            break;
        }
        struct mo_buffer wire;
        mo_buffer_init(&wire, MO_LIMIT_FETCH_RECEIVE_BYTES);
        wire.arena.confidential = true;
        wire.bytes = mo_arena_allocate(&wire.arena, wire.limit, 1);
        if (wire.bytes != NULL)
            wire.capacity = wire.limit;
        ok = receive_response(socket_fd, &wire, end);
        (void)close(socket_fd);
        bool lookahead_failed = false;
        size_t expected_length, count;
        if (ok && consumer.bytes != NULL &&
            mo_export_page_bounds(consumer.total, consumer.offset, &expected_length, &count) &&
            expected_length < consumer.total - consumer.offset) {
            pending = request_page(&endpoint, export_id, consumer.offset + expected_length);
            lookahead_failed = pending < 0;
        }
        struct mo_export_page page;
        if (ok)
            status = mo_fetch_read(image, function, &wire, &page);
        ok = ok && status == MO_FETCH_PAGE && mo_export_accept(&arena, &consumer, &page) &&
             !lookahead_failed;
        mo_buffer_free(&wire);
        if (!ok || consumer.complete)
            break;
        static const char digits[] = "0123456789abcdef";
        for (size_t i = 0; i < 16; i++) {
            export_id[2 * i] = digits[consumer.id[i] >> 4];
            export_id[2 * i + 1] = digits[consumer.id[i] & 15];
        }
        export_id[32] = 0;
    }
    if (pending >= 0)
        (void)close(pending);
    ok = ok && consumer.complete && !cancelled && now() < end;
    ok = ok && consumer.kind == (cli->fetch_failure == 0 ? 1 : 2) &&
         consumer.failure_id == cli->fetch_failure;
    size_t written = 0;
    while (ok && written < consumer.total) {
        if (cancelled || now() >= end) {
            ok = false;
            break;
        }
        ssize_t n = write(fd, consumer.bytes + written, consumer.total - written);
        if (n > 0)
            written += (size_t)n;
        else if (n == 0 || errno != EINTR)
            ok = false;
    }
    if (fd >= 0) {
        if (ok && fsync(fd) != 0)
            ok = false;
        if (close(fd) != 0)
            ok = false;
    }
    if (ok) {
        ok = !cancelled && now() < end && link(temporary, cli->output) == 0;
        if (ok && (cancelled || now() >= end)) {
            if (unlink(cli->output) != 0)
                (void)fputs("mo fetch: late publication cleanup failed\n", errors);
            ok = false;
        }
    }
    if (created && unlink(temporary) != 0)
        (void)fputs(ok ? "mo fetch: artifact published; temporary-name cleanup failed\n"
                       : "mo fetch: private temporary-name cleanup failed\n",
                    errors);
    if (sigint_set)
        (void)sigaction(SIGINT, &old_int, NULL);
    if (sigterm_set)
        (void)sigaction(SIGTERM, &old_term, NULL);
    mo_arena_free(&arena);
    mo_image_free(image);
    if (!ok) {
        const char *reason =
            status == MO_FETCH_REPLACED
                ? "mo fetch: export replaced; retry creates a new generation\n"
            : status == MO_FETCH_RELEASED
                ? "mo fetch: export released or expired; retry creates a new generation\n"
                : "mo fetch: refused or incomplete; artifact not accepted\n";
        (void)fputs(reason, errors);
    }
    return ok ? 0 : 1;
}

int mo_fetch(const struct mo_cli *cli, FILE *errors) {
    uint64_t started = now();
    if (started == UINT64_MAX || started > UINT64_MAX - MO_LIMIT_FETCH_US)
        return 1;
    uint64_t end = started + MO_LIMIT_FETCH_US;
    char host[10], port[6];
    if (!url(cli->fetch_url, host, port)) {
        (void)fputs("mo fetch: expected a plain HTTP loopback URL with numeric port and /mcp\n",
                    errors);
        return 2;
    }
    cancelled = 0;
    struct fetch_timer timer = {0};
    bool armed = timer_start(&timer, end);
    int status = armed ? fetch_attempt(cli, errors, host, port, end) : 1;
    timer_stop(&timer);
    if (!armed)
        (void)fputs("mo fetch: could not configure deadline\n", errors);
    return status;
}

struct push_reply {
    bool failed, rate_limited;
    struct mo_buffer text, json;
    char slot[33];
};

static bool push_read(struct mo_image *image, uint32_t function, const struct mo_buffer *wire,
                      struct push_reply *out) {
    struct mo_limits limits = mo_limits_default();
    limits.heap_size = MO_LIMIT_EXPORT_SCRATCH;
    struct mo_process *process = mo_process_new(image, &limits);
    if (process == NULL)
        return false;
    mo_process_confidential(process);
    struct mo_heap *heap = mo_process_heap(process);
    struct mo_cell input, output;
    bool ok = mo_value_text(heap, (const char *)wire->bytes, wire->length, &input);
    if (ok) {
        struct mo_defect defect;
        mo_process_instruction_limit(process, MO_LIMIT_MCP_REQUEST_WORK);
        ok = mo_call(process, function, &input, 1, &output, &defect) == MO_RETURNED;
        const struct mo_bytecode_function *fn = &mo_image_bytecode(image)->functions[function];
        if (fn->parameters[0].ownership != MO_PARAMETER_OWNED)
            mo_value_drop(heap, fn->parameters[0].type, input);
        if (ok) {
            out->rate_limited = false;
            if (mo_list_size(output) == 1) {
                struct mo_cell item;
                MO_ASSERT(mo_list_get(output, 0, &item));
                size_t length;
                const uint8_t *text = mo_value_data(item, &length);
                out->rate_limited = length == 12 && memcmp(text, "rate_limited", 12) == 0;
            }
            ok = mo_list_size(output) == 4;
            struct mo_cell item;
            size_t size;
            const uint8_t *bytes;
            if (ok) {
                MO_ASSERT(mo_list_get(output, 0, &item));
                bytes = mo_value_data(item, &size);
                out->failed = size == 4 && memcmp(bytes, "true", 4) == 0;
                MO_ASSERT(mo_list_get(output, 1, &item));
                bytes = mo_value_data(item, &size);
                ok = mo_buffer_append(&out->text, bytes, size);
                MO_ASSERT(mo_list_get(output, 2, &item));
                bytes = mo_value_data(item, &size);
                ok = ok && mo_buffer_append(&out->json, bytes, size);
                MO_ASSERT(mo_list_get(output, 3, &item));
                bytes = mo_value_data(item, &size);
                ok = ok && (size == 0 || size == 32);
                if (ok) {
                    memcpy(out->slot, bytes, size);
                    out->slot[size] = 0;
                }
            }
            ok = ok || out->rate_limited;
            mo_value_drop(heap, fn->result_type, output);
        }
    }
    mo_process_free(process);
    return ok;
}

static bool push_call_once(struct fetch_endpoint *e, const char *upgrade, const char *tool,
                           const char *arguments, struct mo_image *image, uint32_t function,
                           struct push_reply *out) {
    uint64_t interval = UINT64_C(1000000) / MO_LIMIT_MCP_RATE + 5000;
    uint64_t elapsed = now() - e->last_sent;
    if (e->last_sent != 0 && elapsed < interval) {
        struct timespec pause = {.tv_nsec = (long)((interval - elapsed) * 1000)};
        if (nanosleep(&pause, NULL) != 0 && errno != EINTR)
            return false;
    }
    if (cancelled || now() >= e->end)
        return false;
    struct mo_buffer request, wire;
    mo_buffer_init(&request, MO_LIMIT_MCP_INGRESS_BYTES);
    request.arena.confidential = true;
    mo_buffer_init(&wire, MO_LIMIT_FETCH_RECEIVE_BYTES);
    wire.arena.confidential = true;
    char head[160];
    size_t body = strlen("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{"
                         "\"name\":\"\",\"arguments\":}}") +
                  strlen(tool) + strlen(arguments);
    int n = snprintf(head, sizeof head, "POST /mcp HTTP/1.1\r\nHost: %s:%s\r\n",
                     strcmp(e->host, "::1") == 0 ? "[::1]" : e->host, e->port);
    bool ok = n > 0 && (size_t)n < sizeof head && mo_buffer_append(&request, head, (size_t)n) &&
              mo_buffer_append_text(&request, "Authorization: Bearer ") &&
              mo_buffer_append_text(&request, e->token) &&
              mo_buffer_append_text(&request, "\r\nMo-Upgrade-Authorization: Bearer ") &&
              mo_buffer_append_text(&request, upgrade) &&
              mo_buffer_append_text(&request,
                                    "\r\nContent-Type: application/json\r\nMCP-Protocol-Version: "
                                    "2025-11-25\r\nConnection: close\r\nContent-Length: ") &&
              mo_buffer_append_uint(&request, body) &&
              mo_buffer_append_text(&request, "\r\n\r\n{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":"
                                              "\"tools/call\",\"params\":{\"name\":\"") &&
              mo_buffer_append_text(&request, tool) &&
              mo_buffer_append_text(&request, "\",\"arguments\":") &&
              mo_buffer_append_text(&request, arguments) && mo_buffer_append_text(&request, "}}");
    int fd = ok ? connect_local(e->host, e->port, e->end) : -1;
    ok = fd >= 0 && send_request(fd, (const char *)request.bytes, request.length, e->end);
    mo_host_wipe(request.bytes, request.length);
    mo_buffer_free(&request);
    if (ok) {
        e->last_sent = now();
        ok = receive_response(fd, &wire, e->end) && push_read(image, function, &wire, out);
    }
    if (fd >= 0)
        (void)close(fd);
    mo_buffer_free(&wire);
    return ok;
}

/* Only the explicit no-effect rate refusal is retried. Lost replies remain uncertain.
 * The existing whole-command deadline bounds retries and credential exposure. */
static bool push_call(struct fetch_endpoint *e, const char *upgrade, const char *tool,
                      const char *arguments, struct mo_image *image, uint32_t function,
                      struct push_reply *out) {
    do {
        if (!push_call_once(e, upgrade, tool, arguments, image, function, out))
            return false;
    } while (out->rate_limited);
    return true;
}

static bool credential(const char *name, const char **out) {
    const char *value = getenv(name);
    size_t size = value == NULL ? 0 : strnlen(value, MO_LIMIT_MCP_TOKEN_BYTES + 1);
    if (size == 0 || size > MO_LIMIT_MCP_TOKEN_BYTES)
        return false;
    for (size_t i = 0; i < size; i++)
        if ((unsigned char)value[i] <= 32 || (unsigned char)value[i] >= 127)
            return false;
    *out = value;
    return true;
}

static void base64_text(const uint8_t *in, size_t length, char *out) {
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t at = 0;
    for (size_t i = 0; i < length; i += 3) {
        uint32_t a = in[i], b = i + 1 < length ? in[i + 1] : 0, c = i + 2 < length ? in[i + 2] : 0;
        uint32_t triple = (a << 16) | (b << 8) | c;
        out[at++] = alphabet[(triple >> 18) & 63];
        out[at++] = alphabet[(triple >> 12) & 63];
        out[at++] = i + 1 < length ? alphabet[(triple >> 6) & 63] : '=';
        out[at++] = i + 2 < length ? alphabet[triple & 63] : '=';
    }
    out[at] = 0;
}

/* Prints the service's report: the text form, or the structured JSON with --json. Refusals go
 * to the error stream unless JSON was asked for. */
static int push_report(const struct mo_cli *cli, FILE *output, FILE *errors,
                       const struct push_reply *r) {
    const struct mo_buffer *shown = cli->json ? &r->json : &r->text;
    FILE *stream = cli->json || !r->failed ? output : errors;
    bool ok = fwrite(shown->bytes, 1, shown->length, stream) == shown->length &&
              (shown->length != 0 && shown->bytes[shown->length - 1] == '\n'
                   ? true
                   : fputc('\n', stream) != EOF) &&
              fflush(stream) == 0;
    return ok && !r->failed ? 0 : 1;
}

/* A failure on the client's side: the message on the error stream, or with --json a report on
 * standard output so it stays parseable. */
static int push_failure(const struct mo_cli *cli, FILE *output, FILE *errors, const char *message,
                        int status) {
    if (!cli->json) {
        (void)fprintf(errors, "mo push: %s\n", message);
        return status;
    }
    struct mo_buffer json;
    mo_buffer_init(&json, 1024);
    bool ok = mo_buffer_append_text(&json, "{\"version\":1,\"status\":\"refused\",\"code\":null,"
                                           "\"class\":\"client\",\"message\":") &&
              mo_render_json_text(&json, message, strlen(message)) &&
              mo_buffer_append_text(&json, "}\n") &&
              fwrite(json.bytes, 1, json.length, output) == json.length && fflush(output) == 0;
    mo_buffer_free(&json);
    return ok ? status : 1;
}

static int push_attempt(const struct mo_cli *cli, FILE *output, FILE *errors, const char *host,
                        const char *port, uint64_t end) {
    uint8_t base[32];
    if (!unhex(cli->push_base, base, 32))
        return push_failure(cli, output, errors,
                            "--base needs the 64-digit lowercase source digest of the active "
                            "program",
                            2);
    const char *token = NULL, *upgrade = NULL;
    if (!credential(cli->fetch_token, &token) || !credential(cli->upgrade_token, &upgrade))
        return push_failure(cli, output, errors, "token unavailable or invalid", 1);
    struct mo_diags diags;
    struct mo_sources sources = {0};
    struct mo_buffer bundle;
    mo_buffer_init(&bundle, MO_LIMIT_BUNDLE_BYTES);
    if (!mo_diags_init(&diags))
        return 1;
    int status = 1;
    struct mo_image *image = NULL;
    if (!mo_sources_init(&sources) ||
        !mo_upgrade_bundle(mo_cli_file(cli, 0), true, true, &bundle, &diags, &sources)) {
        status = mo_write_diagnostics(output, errors, cli->json, MO_COMMAND_PUSH, &sources, &diags,
                                      NULL);
        status = status == 0 ? 1 : status;
        goto done;
    }
    uint8_t digest[32];
    struct mo_sha256 hash;
    mo_sha256_init(&hash);
    mo_sha256_update(&hash, bundle.bytes, bundle.length);
    mo_sha256_finish(&hash, digest);
    char candidate[65];
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < 32; i++) {
        candidate[2 * i] = digits[digest[i] >> 4];
        candidate[2 * i + 1] = digits[digest[i] & 15];
    }
    candidate[64] = 0;
    image = mo_protocol_build();
    struct mo_limits limits = mo_limits_default();
    limits.heap_size = MO_LIMIT_EXPORT_SCRATCH;
    struct mo_defect initialization;
    uint32_t function = MO_NO_INDEX;
    if (image == NULL || mo_image_init(image, &limits, &initialization) != MO_RETURNED) {
        status = push_failure(cli, output, errors, "reader unavailable", 1);
        goto done;
    }
    const struct mo_bytecode *file = mo_image_bytecode(image);
    for (uint32_t i = 0; i < file->function_count; i++) {
        const struct mo_bytecode_string *name = &file->strings[file->functions[i].name];
        if (name->length == 16 && memcmp(name->bytes, "McpFetch.upgrade", 16) == 0)
            function = i;
    }
    MO_ASSERT(function != MO_NO_INDEX);
    struct fetch_endpoint endpoint = {.host = host, .port = port, .token = token, .end = end};
    struct push_reply reply = {0};
    mo_buffer_init(&reply.text, MO_LIMIT_FETCH_RECEIVE_BYTES);
    mo_buffer_init(&reply.json, MO_LIMIT_FETCH_RECEIVE_BYTES);
    struct mo_buffer arguments;
    mo_buffer_init(&arguments, MO_LIMIT_MCP_BODY_BYTES);
    bool ok = mo_buffer_append_text(&arguments, "{\"base\":\"") &&
              mo_buffer_append_text(&arguments, cli->push_base) &&
              mo_buffer_append_text(&arguments, "\",\"candidate\":\"") &&
              mo_buffer_append_text(&arguments, candidate) &&
              mo_buffer_append_text(&arguments, "\",\"total\":") &&
              mo_buffer_append_uint(&arguments, bundle.length);
    if (ok && cli->migration != NULL)
        ok = mo_buffer_append_text(&arguments, ",\"migration\":") &&
             mo_render_json_text(&arguments, cli->migration, strlen(cli->migration));
    ok = ok && mo_buffer_append_byte(&arguments, '}') && mo_buffer_append_byte(&arguments, 0);
    bool answered = ok && push_call(&endpoint, upgrade, "mo-upgrade-open",
                                    (const char *)arguments.bytes, image, function, &reply);
    char slot[33] = "";
    bool proceed = answered && !reply.failed && reply.slot[0] != 0;
    if (proceed)
        memcpy(slot, reply.slot, sizeof slot);
    size_t pages = (bundle.length + MO_LIMIT_UPGRADE_PAGE_BYTES - 1) / MO_LIMIT_UPGRADE_PAGE_BYTES;
    char text[MO_LIMIT_UPGRADE_PAGE_TEXT + 1];
    for (size_t i = 0; proceed && i < pages; i++) {
        size_t offset = i * MO_LIMIT_UPGRADE_PAGE_BYTES;
        size_t length = bundle.length - offset < MO_LIMIT_UPGRADE_PAGE_BYTES
                            ? bundle.length - offset
                            : MO_LIMIT_UPGRADE_PAGE_BYTES;
        base64_text(bundle.bytes + offset, length, text);
        mo_buffer_clear(&arguments);
        mo_buffer_clear(&reply.text);
        mo_buffer_clear(&reply.json);
        char numbers[96];
        int n = snprintf(numbers, sizeof numbers, "\",\"index\":%zu,\"offset\":%zu,\"total\":%zu",
                         i, offset, bundle.length);
        answered =
            n > 0 && (size_t)n < sizeof numbers &&
            mo_buffer_append_text(&arguments, "{\"slot\":\"") &&
            mo_buffer_append_text(&arguments, slot) && mo_buffer_append_text(&arguments, numbers) &&
            mo_buffer_append_text(&arguments, ",\"bytes\":\"") &&
            mo_buffer_append_text(&arguments, text) && mo_buffer_append_text(&arguments, "\"}") &&
            mo_buffer_append_byte(&arguments, 0) &&
            push_call(&endpoint, upgrade, "mo-upgrade-page", (const char *)arguments.bytes, image,
                      function, &reply);
        proceed = answered && !reply.failed;
    }
    bool committing = proceed;
    if (proceed) {
        mo_buffer_clear(&arguments);
        mo_buffer_clear(&reply.text);
        mo_buffer_clear(&reply.json);
        answered = mo_buffer_append_text(&arguments, "{\"slot\":\"") &&
                   mo_buffer_append_text(&arguments, slot) &&
                   mo_buffer_append_text(&arguments, "\",\"base\":\"") &&
                   mo_buffer_append_text(&arguments, cli->push_base) &&
                   mo_buffer_append_text(&arguments, "\"}") &&
                   mo_buffer_append_byte(&arguments, 0) &&
                   push_call(&endpoint, upgrade, "mo-upgrade-commit", (const char *)arguments.bytes,
                             image, function, &reply);
    }
    if (answered)
        status = push_report(cli, output, errors, &reply);
    else
        /* Only the commit can change the service; an earlier failure changed nothing. */
        status = push_failure(cli, output, errors,
                              committing ? "the service did not answer the commit; the upgrade "
                                           "may or may not have been applied; push again to "
                                           "learn which program is active"
                                         : "the service did not answer or refused the request "
                                           "before the commit; nothing was changed",
                              1);
    mo_buffer_free(&arguments);
    mo_buffer_free(&reply.text);
    mo_buffer_free(&reply.json);
done:
    if (image != NULL)
        mo_image_free(image);
    mo_buffer_free(&bundle);
    mo_sources_free(&sources);
    mo_diags_free(&diags);
    return status;
}

struct push_work {
    const struct mo_cli *cli;
    FILE *output, *errors;
    const char *host, *port;
    uint64_t end;
    int status;
};
static void push_run(void *context) {
    struct push_work *w = context;
    w->status = push_attempt(w->cli, w->output, w->errors, w->host, w->port, w->end);
}

int mo_push(const struct mo_cli *cli, FILE *output, FILE *errors) {
    uint64_t started = now();
    if (started == UINT64_MAX || started > UINT64_MAX - MO_LIMIT_FETCH_US)
        return 1;
    char host[10], port[6];
    /* The destination is checked before either credential is read. */
    if (!url(cli->push_url, host, port)) {
        (void)fputs("mo push: expected a plain HTTP loopback URL with numeric port and /mcp\n",
                    errors);
        return 2;
    }
    cancelled = 0;
    struct fetch_timer timer = {0};
    struct push_work work = {cli, output, errors, host, port, started + MO_LIMIT_FETCH_US, 1};
    bool armed = timer_start(&timer, work.end);
    if (armed && mo_with_stack(MO_SOURCE_STACK_SIZE, push_run, &work) != 0)
        work.status = 1;
    timer_stop(&timer);
    if (!armed)
        (void)fputs("mo push: could not configure deadline\n", errors);
    return armed ? work.status : 1;
}
