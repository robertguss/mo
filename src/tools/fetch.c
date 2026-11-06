#define _POSIX_C_SOURCE 200809L
#include "base/binary.h"
#include "base/export.h"
#include "tools/protocol.h"
#include "tools/tools.h"
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
    (void)fputs("mo fetch: maximum state needs 2049 pages, about 103 seconds at 20 calls/second\n",
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
