#ifndef LUAGENT_DIAG_H
#define LUAGENT_DIAG_H

#include <stddef.h>
#include <stdint.h>

struct lua_State;
struct session;

enum diag_level {
    DIAG_LEVEL_ERROR = 0,
    DIAG_LEVEL_WARN = 1,
    DIAG_LEVEL_INFO = 2,
    DIAG_LEVEL_DEBUG = 3,
    DIAG_LEVEL_TRACE = 4
};

struct diag_counters {
    uint64_t frames_rx;
    uint64_t frames_tx;
    uint64_t bytes_rx;
    uint64_t bytes_tx;
    uint64_t parse_failures;
    uint64_t protocol_errors;
    uint64_t lua_op_failures;
    uint64_t transfer_starts;
    uint64_t transfer_completes;
    uint64_t transfer_aborts;
    uint64_t process_spawns;
    uint64_t process_exits;
    uint64_t process_timeouts;
    uint64_t session_disconnects;
    uint64_t heartbeat_events;
    uint64_t relay_opens;
    uint64_t relay_closes;
};

int diag_init(void);
void diag_shutdown(void);
void diag_log(enum diag_level level, const char *subsystem,
    const struct session *session, uint32_t req_id, const char *fmt, ...);
void diag_wire_rx(const struct session *session, uint8_t type, uint32_t req_id,
    size_t payload_len, const void *frame, size_t frame_len);
void diag_wire_tx(const struct session *session, uint8_t type, uint32_t req_id,
    size_t payload_len, const void *frame, size_t frame_len);

void diag_count_frame_rx(struct session *session, size_t frame_len);
void diag_count_frame_tx(struct session *session, size_t frame_len);
void diag_count_parse_failure(void);
void diag_count_protocol_error(struct session *session);
void diag_count_lua_error(struct session *session);
void diag_count_transfer_start(struct session *session);
void diag_count_transfer_complete(struct session *session);
void diag_count_transfer_abort(struct session *session);
void diag_count_process_spawn(struct session *session);
void diag_count_process_exit(struct session *session);
void diag_count_process_timeout(struct session *session);
void diag_count_session_disconnect(struct session *session);
void diag_count_heartbeat(struct session *session);
void diag_count_relay_open(struct session *session);
void diag_count_relay_close(struct session *session);

void diag_push_sessions(struct lua_State *L, const struct session *session);
void diag_push_transfers(struct lua_State *L, const struct session *session);
void diag_push_procs(struct lua_State *L, const struct session *session);
void diag_push_stats(struct lua_State *L, const struct session *session);

#endif
