#define _POSIX_C_SOURCE 200809L
#include "host/network.h"

#include <stdio.h>
#include <string.h>

struct target_rule {
    char host[MO_LIMIT_HOST_NAME + 1];
    size_t length;
    int32_t port; /* 0 = any port 1..65535 */
};

static MO_MUST_USE bool parse_port_digits(const uint8_t *bytes, size_t length, int32_t *out) {
    if (length == 0 || length > 5)
        return false;
    int32_t port = 0;
    for (size_t i = 0; i < length; i++) {
        if (bytes[i] < '0' || bytes[i] > '9')
            return false;
        port = port * 10 + (int32_t)(bytes[i] - '0');
    }
    if (port < 1 || port > 65535)
        return false;
    *out = port;
    return true;
}

static MO_MUST_USE bool parse_target(const uint8_t *bytes, size_t length, struct target_rule *out) {
    if (length == 0 || memchr(bytes, 0, length) != NULL)
        return false;
    const uint8_t *host_start = NULL;
    size_t host_length = 0;
    int32_t port = 0;
    if (bytes[0] == '[') {
        const uint8_t *close = memchr(bytes + 1, ']', length - 1);
        if (close == NULL || close == bytes + 1)
            return false;
        host_start = bytes + 1;
        host_length = (size_t)(close - host_start);
        if (memchr(host_start, '[', host_length) != NULL ||
            memchr(host_start, ':', host_length) == NULL)
            return false;
        size_t consumed = (size_t)(close - bytes) + 1;
        if (consumed < length) {
            if (bytes[consumed] != ':' ||
                !parse_port_digits(bytes + consumed + 1, length - consumed - 1, &port))
                return false;
        }
    } else {
        if (memchr(bytes, '[', length) != NULL || memchr(bytes, ']', length) != NULL)
            return false;
        size_t colons = 0;
        const uint8_t *colon = NULL;
        for (size_t i = 0; i < length; i++) {
            if (bytes[i] == ':') {
                colons++;
                colon = bytes + i;
            }
        }
        if (colons == 1) {
            host_start = bytes;
            host_length = (size_t)(colon - bytes);
            size_t port_offset = host_length + 1;
            if (!parse_port_digits(bytes + port_offset, length - port_offset, &port))
                return false;
        } else {
            host_start = bytes;
            host_length = length;
        }
    }
    if (!mo_host_named(host_start, host_length))
        return false;
    for (size_t i = 0; i < host_length; i++) {
        uint8_t ch = host_start[i];
        out->host[i] = (ch >= 'A' && ch <= 'Z') ? (char)(ch + ('a' - 'A')) : (char)ch;
    }
    out->host[host_length] = '\0';
    out->length = host_length;
    out->port = port;
    return true;
}

static MO_MUST_USE bool parse_target_cell(struct mo_cell cell, struct target_rule *out) {
    size_t length;
    const uint8_t *bytes = mo_value_data(cell, &length);
    return parse_target(bytes, length, out);
}

static MO_MUST_USE bool append_unique_rule(struct mo_heap *heap, uint32_t texts_type,
                                           uint32_t text_type, struct mo_cell *allowed,
                                           const struct target_rule *rule) {
    char canonical[MO_LIMIT_HOST_NAME + 10];
    size_t length = 0;
    if (rule->port == 0) {
        memcpy(canonical, rule->host, rule->length);
        length = rule->length;
    } else if (memchr(rule->host, ':', rule->length) != NULL) {
        int written = snprintf(canonical, sizeof canonical, "[%.*s]:%d", (int)rule->length,
                               rule->host, (int)rule->port);
        MO_ASSERT(written > 0 && (size_t)written < sizeof canonical);
        length = (size_t)written;
    } else {
        int written = snprintf(canonical, sizeof canonical, "%.*s:%d", (int)rule->length,
                               rule->host, (int)rule->port);
        MO_ASSERT(written > 0 && (size_t)written < sizeof canonical);
        length = (size_t)written;
    }
    uint32_t count = mo_list_size(*allowed);
    for (uint32_t i = 0; i < count; i++) {
        struct mo_cell existing;
        MO_ASSERT(mo_list_get(*allowed, i, &existing));
        size_t existing_length;
        const uint8_t *existing_bytes = mo_value_data(existing, &existing_length);
        if (existing_length == length && memcmp(existing_bytes, canonical, length) == 0)
            return true;
    }
    struct mo_cell entry;
    if (!mo_value_text(heap, canonical, length, &entry))
        return false;
    bool ok = mo_list_append(heap, texts_type, allowed, entry);
    mo_value_drop(heap, text_type, entry);
    return ok;
}

static MO_MUST_USE enum mo_host_outcome network_allow_hosts(struct mo_host *host,
                                                            struct mo_heap *heap, uint32_t type,
                                                            const struct mo_cell *args,
                                                            struct mo_cell *out) {
    const struct mo_capability_info *info = mo_capability_get(args[0]);
    const struct mo_host_operations *operations = info->operations;
    bool fake = operations->fake;
    bool has_existing = mo_state_length(info->state) > (fake ? 1u : 0u);
    struct mo_cell existing =
        has_existing ? mo_state_get(info->state, fake ? 1u : 0u) : mo_cell_unit();
    struct mo_cell allowed;
    if (!mo_value_list(heap, host->types->texts, NULL, 0, &allowed))
        return MO_HOST_OUT_OF_MEMORY;
    uint32_t requested_count = mo_list_size(args[1]);
    for (uint32_t i = 0; i < requested_count; i++) {
        struct mo_cell item;
        MO_ASSERT(mo_list_get(args[1], i, &item));
        struct target_rule req;
        if (!parse_target_cell(item, &req))
            continue;
        if (!has_existing) {
            if (!append_unique_rule(heap, host->types->texts, host->types->text, &allowed, &req)) {
                mo_value_drop(heap, host->types->texts, allowed);
                return MO_HOST_OUT_OF_MEMORY;
            }
            continue;
        }
        uint32_t existing_count = mo_list_size(existing);
        for (uint32_t j = 0; j < existing_count; j++) {
            struct mo_cell old_item;
            MO_ASSERT(mo_list_get(existing, j, &old_item));
            struct target_rule old;
            if (!parse_target_cell(old_item, &old))
                continue;
            if (old.length != req.length || memcmp(old.host, req.host, req.length) != 0)
                continue;
            if (old.port != 0 && req.port != 0 && old.port != req.port)
                continue;
            struct target_rule narrowed = req;
            narrowed.port = old.port != 0 ? old.port : req.port;
            if (!append_unique_rule(heap, host->types->texts, host->types->text, &allowed,
                                    &narrowed)) {
                mo_value_drop(heap, host->types->texts, allowed);
                return MO_HOST_OUT_OF_MEMORY;
            }
        }
    }
    enum mo_host_outcome result;
    if (fake) {
        uint32_t slot_types[] = {host->types->peers, host->types->texts};
        struct mo_cell slot_values[] = {mo_state_get(info->state, 0), allowed};
        result = mo_host_state_cap(host, heap, type, true, slot_types, slot_values, 2, out);
    } else {
        result = mo_host_state_cap(host, heap, type, false, &host->types->texts, &allowed, 1, out);
    }
    mo_value_drop(heap, host->types->texts, allowed);
    return result;
}

static MO_MUST_USE bool network_allowed(const struct mo_capability_info *info, bool fake,
                                        struct mo_cell host_cell, int64_t port) {
    if (mo_state_length(info->state) <= (fake ? 1u : 0u))
        return true;
    size_t length;
    const uint8_t *name = mo_value_data(host_cell, &length);
    if (!mo_host_named(name, length))
        return false;
    char lower[MO_LIMIT_HOST_NAME + 1];
    for (size_t i = 0; i < length; i++) {
        uint8_t ch = name[i];
        lower[i] = (ch >= 'A' && ch <= 'Z') ? (char)(ch + ('a' - 'A')) : (char)ch;
    }
    struct mo_cell allowed = mo_state_get(info->state, fake ? 1u : 0u);
    uint32_t count = mo_list_size(allowed);
    for (uint32_t i = 0; i < count; i++) {
        struct mo_cell item;
        MO_ASSERT(mo_list_get(allowed, i, &item));
        struct target_rule rule;
        if (!parse_target_cell(item, &rule))
            continue;
        if (rule.length == length && memcmp(rule.host, lower, length) == 0 &&
            (rule.port == 0 || (int64_t)rule.port == port))
            return true;
    }
    return false;
}

bool mo_host_named(const uint8_t *name, size_t length) {
    return length != 0 && length <= MO_LIMIT_HOST_NAME && memchr(name, 0, length) == NULL;
}

uint32_t mo_host_network_payload(const struct mo_host *host, uint32_t type, const char *name) {
    return host->types->types.items[type].cases[mo_host_tag(host, type, name)].fields[0].type;
}

enum mo_host_outcome mo_host_network_ok(struct mo_host *host, struct mo_heap *heap, uint32_t type,
                                        struct mo_cell value, struct mo_cell *out) {
    bool ok = mo_host_case(host, heap, type, "Ok", &value, 1, out);
    mo_value_drop(heap, mo_host_network_payload(host, type, "Ok"), value);
    return ok ? MO_HOST_RETURNED : MO_HOST_OUT_OF_MEMORY;
}

enum mo_host_outcome mo_host_network_error(struct mo_host *host, struct mo_heap *heap,
                                           uint32_t type, struct mo_network_error error,
                                           struct mo_cell name, int64_t port, struct mo_cell *out,
                                           struct mo_host_error *stop) {
    static const char *const cases[] = {[NETWORK_TIMED_OUT] = "TimedOut",
                                        [NETWORK_CLOSED] = "Closed",
                                        [NETWORK_REFUSED] = "Refused",
                                        [NETWORK_UNKNOWN_HOST] = "UnknownHost",
                                        [NETWORK_BROKEN] = "Broken"};
    if (error.status == NETWORK_INTERNAL) {
        *stop = (struct mo_host_error){.reason = "internal error: monotonic clock unavailable"};
        return MO_HOST_INTERNAL;
    }
    MO_ASSERT(error.status != NETWORK_OK);
    struct mo_cell fields[] = {name, mo_cell_int(port)};
    uint32_t count = error.status == NETWORK_REFUSED        ? 2
                     : error.status == NETWORK_UNKNOWN_HOST ? 1
                                                            : 0;
    if (error.status == NETWORK_BROKEN) {
        char system[256];
        const char *reason = error.reason;
        if (reason == NULL)
            reason = strerror_r(error.number, system, sizeof system) == 0 ? system : "system error";
        if (!mo_value_text(heap, reason, strlen(reason), &fields[0]))
            return MO_HOST_OUT_OF_MEMORY;
        count = 1;
    }
    uint32_t error_type = mo_host_network_payload(host, type, "Error");
    struct mo_cell value;
    bool built = mo_host_case(host, heap, error_type, cases[error.status], fields, count, &value);
    if (error.status == NETWORK_BROKEN)
        mo_value_drop(heap, host->types->text, fields[0]);
    if (!built)
        return MO_HOST_OUT_OF_MEMORY;
    bool wrapped = mo_host_case(host, heap, type, "Error", &value, 1, out);
    mo_value_drop(heap, error_type, value);
    return wrapped ? MO_HOST_RETURNED : MO_HOST_OUT_OF_MEMORY;
}

enum mo_host_outcome mo_host_network(struct mo_host *host, struct mo_heap *heap, uint32_t operation,
                                     uint32_t type, const struct mo_cell *args, struct mo_cell *out,
                                     struct mo_host_error *error) {
    if (operation == MO_HOST_NETWORK_ALLOW_HOSTS)
        return network_allow_hosts(host, heap, type, args, out);
    const struct mo_capability_info *info = mo_capability_get(args[0]);
    const struct mo_host_operations *operations = info->operations;
    if (operation == MO_HOST_NETWORK_CONNECT) {
        int64_t port = mo_cell_get_int(args[2]);
        if (port < 1 || port > 65535)
            return mo_host_bug(error, "port outside 1 to 65535");
        if (!network_allowed(info, operations->fake, args[1], port))
            return mo_host_network_error(host, heap, type,
                                         (struct mo_network_error){.status = NETWORK_REFUSED},
                                         args[1], port, out, error);
    }
    if (operation == MO_HOST_CONNECTION_RECEIVE) {
        int64_t most = mo_cell_get_int(args[1]);
        if (most < 1 || most > MO_LIMIT_RECEIVE_BYTES)
            return mo_host_bug(error, "most below 1 or above the receive limit");
    }
    if (operations->fake)
        return mo_host_fake_network_call(host, heap, operation, type, args, out, error);
    return mo_host_real_network_call(host, heap, operation, type, args, out, error);
}
