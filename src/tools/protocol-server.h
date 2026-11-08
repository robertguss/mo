#ifndef MO_PROTOCOL_SERVER_INTERNAL_H
#define MO_PROTOCOL_SERVER_INTERNAL_H

#include "base/export.h"
#include "tools/protocol.h"
#include "vm/vm.h"

enum protocol_function {
    PF_SCAN,
    PF_HEADERS,
    PF_AUTHORITY,
    PF_INSPECT,
    PF_PLAN,
    PF_FIXED,
    PF_CHUNKED,
    PF_FEED,
    PF_PREPARE,
    PF_RECEIVE,
    PF_ARGUMENTS,
    PF_ENCODE,
    PF_TOOL_REPLY,
    PF_FALLBACK,
    PF_HTTP,
    PF_HTTP_HEAD,
    PF_HTTP100,
    PF_VIEW,
    PF_EXPORT_ID,
    PF_FAILURE_ID,
    PF_FAILURE_AFTER,
    PF_FAILURE_VIEW,
    PF_EXPORT,
    PF_OPERATOR_REPLY,
    PF_STREAM_PLAN,
    PF_HTTP_SESSION,
    PF_STREAM_HEAD,
    PF_LIST_CHANGED,
    PF_COUNT
};

enum protocol_phase {
    PS_EMPTY,
    PS_HEADERS,
    PS_BODY,
    PS_CONTINUE,
    PS_SERVICE,
    PS_WRITE,
    PS_DRAIN,
    PS_STREAM
};

enum protocol_error {
    PE_ARGUMENTS,
    PE_BUSY,
    PE_STOPPED,
    PE_MEMORY,
    PE_WORK,
    PE_HANDLER,
    PE_STORE,
    PE_RATE,
    PE_RESPONSE,
    PE_INTERNAL,
    PE_COUNT
};

enum protocol_effect { EFFECT_NONE, EFFECT_APPLIED, EFFECT_UNKNOWN, EFFECT_COUNT };

struct protocol_bytes {
    const uint8_t *bytes;
    size_t size;
};

struct protocol_fallback {
    struct protocol_bytes prefix, suffix;
};

struct protocol_connection {
    enum protocol_phase phase;
    struct mo_listener_key key;
    struct mo_process *process;
    struct mo_scratch scratch;
    uint64_t id, deadline, work_left, preauth_left;
    size_t header_size, body_size, write_offset, drained, peak_memory, peak_native;
    uint8_t input[MO_LIMIT_MCP_HEADER_BYTES];
    uint8_t escaped_id[6 * MO_LIMIT_MCP_ID_BYTES + 2];
    size_t id_size;
    uint8_t *body, *fallback;
    size_t fallback_capacity;
    struct protocol_bytes output;
    struct mo_cell scan, body_state, version, action, descriptor, catalog;
    struct mo_cell response;
    struct mo_service_response reply;
    /* A GET stream's unwritten output and the session it serves (index + 1, 0 for none). */
    uint8_t stream[MO_LIMIT_MCP_STREAM_BYTES];
    size_t stream_size, session;
    bool reply_ready, retained, authenticated, upgrade_authorized;
};

struct protocol_session {
    char id[MO_LIMIT_MCP_SESSION_ID_BYTES];
    uint64_t used;
    size_t stream; /* connection slot of the session's one stream, or SIZE_MAX */
    bool live;
};

struct mo_protocol_server {
    struct mo_arena arena;
    struct mo_export_pin state_export, failure_export;
    struct mo_image *protocol;
    const struct mo_image *image;
    const struct mo_bytecode_service *descriptor;
    struct mo_service_loop *service;
    struct mo_heap *service_heap;
    struct mo_listener *listener;
    struct mo_protocol_events events;
    struct mo_listener_time now;
    struct mo_process *catalog_process;
    struct mo_scratch catalog_scratch;
    struct mo_cell authorities, cached_descriptor, cached_catalog;
    uint32_t functions[PF_COUNT];
    uint32_t service_index;
    uint64_t sequence, sequence_sessions, rate_at, rate_credit, started_at;
    size_t next_slot, fallback_body_max, view_scratch_peak;
    struct protocol_fallback fallbacks[PE_COUNT][EFFECT_COUNT];
    struct protocol_bytes *fallback_headers;
    struct protocol_bytes errors[506];
    struct protocol_connection connections[MO_LIMIT_MCP_CONNECTIONS];
    struct protocol_session sessions[MO_LIMIT_MCP_SESSIONS];
    struct protocol_bytes stream_head, list_changed;
    struct mo_bearer upgrade_bearer;
    bool upgrade_enabled;
    struct mo_protocol_metrics metrics;
    bool io_failed, closing;
};

MO_MUST_USE const struct mo_bytecode_function *
mo_protocol_function(const struct mo_protocol_server *server, enum protocol_function function);
MO_MUST_USE struct mo_cell mo_protocol_field(const struct mo_image *image, uint32_t type,
                                             struct mo_cell value, uint32_t field);
MO_MUST_USE bool mo_protocol_invoke(struct mo_protocol_server *server,
                                    struct protocol_connection *connection,
                                    enum protocol_function function, const struct mo_cell *args,
                                    struct mo_cell *out);
void mo_protocol_drop(struct mo_protocol_server *server, struct protocol_connection *connection,
                      enum protocol_function function, struct mo_cell value);
MO_MUST_USE bool mo_protocol_charge(struct protocol_connection *connection, size_t amount);
void mo_protocol_observe(struct mo_protocol_server *server, struct protocol_connection *connection);
void mo_protocol_dispose(struct mo_protocol_server *server, struct protocol_connection *connection);
void mo_protocol_close(struct mo_protocol_server *server, struct protocol_connection *connection);
void mo_protocol_fail(struct mo_protocol_server *server, struct protocol_connection *connection,
                      int status);
void mo_protocol_fallback(struct mo_protocol_server *server, struct protocol_connection *connection,
                          enum protocol_error error, enum protocol_effect effect);
void mo_protocol_start_write(struct mo_protocol_server *server,
                             struct protocol_connection *connection, struct protocol_bytes bytes);
void mo_protocol_read(struct mo_protocol_server *server, struct protocol_connection *connection);
MO_MUST_USE bool mo_protocol_body_complete(const struct protocol_connection *connection);
void mo_protocol_receive(struct mo_protocol_server *server, struct protocol_connection *connection);
void mo_protocol_complete(struct mo_protocol_server *server,
                          struct protocol_connection *connection);
MO_MUST_USE bool mo_protocol_catalog(struct mo_protocol_server *server);
MO_MUST_USE bool mo_protocol_request_process(struct mo_protocol_server *server,
                                             struct protocol_connection *connection);
/* Session lookup by the bytes a request carried; SIZE_MAX when no live session matches. */
MO_MUST_USE size_t mo_protocol_session_find(struct mo_protocol_server *server, const uint8_t *id,
                                            size_t length);
MO_MUST_USE bool mo_protocol_session_open(struct mo_protocol_server *server, size_t *out);
void mo_protocol_stream_open(struct mo_protocol_server *server,
                             struct protocol_connection *connection, size_t session);
/* Queue a tools/list_changed notification on each session's one stream. */
void mo_protocol_list_changed(struct mo_protocol_server *server);

MO_MUST_USE bool mo_protocol_view(struct mo_protocol_server *s, struct protocol_connection *c,
                                  struct mo_cell *out);

MO_MUST_USE bool mo_protocol_failures(struct mo_protocol_server *s, struct protocol_connection *c,
                                      struct mo_cell *out);
MO_MUST_USE bool mo_protocol_export(struct mo_protocol_server *s, struct protocol_connection *c,
                                    struct mo_cell *out, bool *refused);

#endif
