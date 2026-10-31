#ifndef MO_PROTOCOL_H
#define MO_PROTOCOL_H

#include "bytecode/bytecode.h"
#include "heap/heap.h"

struct mo_listener;
struct mo_store;
struct mo_protocol_server;

enum mo_protocol_server_status {
    MO_PROTOCOL_SERVER_OK,
    MO_PROTOCOL_SERVER_MEMORY,
    MO_PROTOCOL_SERVER_CONFIG,
    MO_PROTOCOL_SERVER_PROTOCOL,
    MO_PROTOCOL_SERVER_SERVICE,
    MO_PROTOCOL_SERVER_IO
};

struct mo_protocol_metrics {
    uint64_t work, preauth_work, stage_us, admitted, completed;
    size_t request_memory, preauth_memory, native_memory, concurrent_memory, catalog_memory;
};

MO_MUST_USE enum mo_protocol_server_status
mo_protocol_server_new(const struct mo_image *service, uint32_t index, struct mo_store *store,
                       struct mo_listener *listener, const char *const *authorities,
                       size_t authority_count, struct mo_protocol_server **out);
void mo_protocol_server_free(struct mo_protocol_server *server);
MO_MUST_USE enum mo_protocol_server_status
mo_protocol_server_poll(struct mo_protocol_server *server, uint64_t until_us);
MO_MUST_USE struct mo_protocol_metrics
mo_protocol_server_metrics(const struct mo_protocol_server *server);

struct mo_protocol_descriptor {
    uint32_t type;
    struct mo_cell value;
};

enum mo_protocol_value_status {
    MO_PROTOCOL_VALUE_OK,
    MO_PROTOCOL_VALUE_INVALID,
    MO_PROTOCOL_VALUE_MEMORY,
    MO_PROTOCOL_VALUE_LIMIT,
};

struct mo_protocol_values {
    const struct mo_types *protocol, *service;
    const struct mo_bytecode_service *descriptor;
    uint32_t value;
    uint64_t *work;
    size_t node_limit;
};

MO_MUST_USE enum mo_protocol_value_status
mo_protocol_value_pack(const struct mo_protocol_values *types, struct mo_heap *to, uint32_t type,
                       struct mo_cell value, size_t byte_limit, struct mo_cell *out);
MO_MUST_USE enum mo_protocol_value_status
mo_protocol_value_unpack(const struct mo_protocol_values *types, struct mo_heap *to, uint32_t type,
                         struct mo_cell value, size_t byte_limit, struct mo_cell *out);

MO_MUST_USE enum mo_protocol_value_status
mo_protocol_scalar_pack(const struct mo_types *protocol, uint32_t scalar, struct mo_heap *to,
                        enum mo_type_kind kind, struct mo_cell value, size_t byte_limit,
                        struct mo_cell *out);
MO_MUST_USE enum mo_protocol_value_status
mo_protocol_scalar_unpack(const struct mo_types *protocol, uint32_t scalar, struct mo_heap *to,
                          enum mo_type_kind kind, struct mo_cell value, size_t byte_limit,
                          struct mo_cell *out);

MO_MUST_USE struct mo_image *mo_protocol_build(void);
MO_MUST_USE bool mo_protocol_body_marks(const uint8_t *bytes, size_t size, size_t *count);
MO_MUST_USE bool mo_protocol_descriptor(const struct mo_image *protocol, struct mo_heap *heap,
                                        const struct mo_image *service, uint32_t index,
                                        struct mo_protocol_descriptor *out);

#endif
