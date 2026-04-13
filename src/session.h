#ifndef LUAGENT_SESSION_H
#define LUAGENT_SESSION_H

#include <stdint.h>
#include <uv.h>
#include "proc.h"
#include "transfer.h"

#define LUAGENT_READ_CHUNK 4096u
#define LUAGENT_MAX_WRITE_QUEUE_BYTES 262144u
#define LUAGENT_SESSION_IDLE_MS 15000u
#define LUAGENT_HEARTBEAT_IDLE_MS 5000u
#define LUAGENT_TRANSFER_IDLE_MS 20000u

struct lua_engine;

struct session {
    uv_tcp_t tcp;
    uv_timer_t heartbeat_timer;
    uv_loop_t *loop;
    struct lua_engine *engine;
    struct transfer_state upload;
    struct transfer_state download;
    struct proc_state proc;
    unsigned char *rx_buf;
    size_t rx_used;
    size_t rx_cap;
    size_t pending_write_bytes;
    uint64_t last_rx_ms;
    uint64_t last_tx_ms;
    uint64_t session_id;
    uint64_t frames_rx;
    uint64_t frames_tx;
    uint64_t bytes_rx;
    uint64_t bytes_tx;
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
    uint32_t next_req_id;
    uint32_t next_proc_id;
    int close_refs;
    int timer_started;
    int closed;
};

void session_init(struct session *session);
void session_reset_buffer(struct session *session);
void session_close(struct session *session);
int session_start_timers(struct session *session);
void session_add_ref(struct session *session);
void session_release_ref(struct session *session);
int session_send_frame(struct session *session, uint8_t type, uint32_t req_id,
    const void *payload, size_t payload_len);
int session_send_text(struct session *session, uint8_t type, uint32_t req_id,
    const char *payload);
int session_send_error(struct session *session, uint32_t req_id,
    const char *code, const char *message);

#endif
