#include "diag.h"

#include "protocol.h"
#include "session.h"
#include "util.h"

#include <errno.h>
#include <lua.h>
#include <lauxlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <process.h>
#define diag_getpid _getpid
#else
#include <unistd.h>
#define diag_getpid getpid
#endif

#define DIAG_LOG_ROTATE_BYTES (1024u * 1024u)
#define DIAG_LOG_ROTATE_KEEP 3u

enum wire_mode {
    WIRE_MODE_OFF = 0,
    WIRE_MODE_SUMMARY = 1,
    WIRE_MODE_HEX = 2
};

struct diag_state {
    int initialized;
    enum diag_level level;
    enum wire_mode wire_mode;
    FILE *file;
    char path[512];
    struct diag_counters counters;
};

static struct diag_state g_diag;

static const char *level_name(enum diag_level level)
{
    switch (level) {
    case DIAG_LEVEL_ERROR: return "ERROR";
    case DIAG_LEVEL_WARN: return "WARN";
    case DIAG_LEVEL_INFO: return "INFO";
    case DIAG_LEVEL_DEBUG: return "DEBUG";
    default: return "TRACE";
    }
}

static enum diag_level parse_level(const char *value)
{
    if (value == NULL) return DIAG_LEVEL_INFO;
    if (strcmp(value, "ERROR") == 0) return DIAG_LEVEL_ERROR;
    if (strcmp(value, "WARN") == 0) return DIAG_LEVEL_WARN;
    if (strcmp(value, "DEBUG") == 0) return DIAG_LEVEL_DEBUG;
    if (strcmp(value, "TRACE") == 0) return DIAG_LEVEL_TRACE;
    return DIAG_LEVEL_INFO;
}

static enum wire_mode parse_wire_mode(const char *value)
{
    if (value == NULL || strcmp(value, "off") == 0) return WIRE_MODE_OFF;
    if (strcmp(value, "hex") == 0) return WIRE_MODE_HEX;
    return WIRE_MODE_SUMMARY;
}

static void rotate_logs_if_needed(size_t incoming_len)
{
    struct stat st;
    unsigned int i;
    char src[640];
    char dst[640];

    if (g_diag.path[0] == '\0') {
        return;
    }

    if (stat(g_diag.path, &st) != 0) {
        return;
    }

    if ((size_t) st.st_size + incoming_len < DIAG_LOG_ROTATE_BYTES) {
        return;
    }

    if (g_diag.file != NULL) {
        fclose(g_diag.file);
        g_diag.file = NULL;
    }

    for (i = DIAG_LOG_ROTATE_KEEP; i > 0; --i) {
        if (i == 1) {
            util_snprintf(src, sizeof(src), "%s", g_diag.path);
        } else {
            util_snprintf(src, sizeof(src), "%s.%u", g_diag.path, i - 1);
        }
        util_snprintf(dst, sizeof(dst), "%s.%u", g_diag.path, i);
        rename(src, dst);
    }
}

static void ensure_log_file(void)
{
    if (g_diag.path[0] == '\0' || g_diag.file != NULL) {
        return;
    }

    g_diag.file = fopen(g_diag.path, "a");
}

int diag_init(void)
{
    const char *level_env;
    const char *wire_env;
    const char *file_env;

    memset(&g_diag, 0, sizeof(g_diag));
    level_env = getenv("LUAGENT_LOG_LEVEL");
    wire_env = getenv("LUAGENT_WIRE");
    file_env = getenv("LUAGENT_LOG_FILE");

    g_diag.level = parse_level(level_env);
    g_diag.wire_mode = parse_wire_mode(wire_env);
    if (file_env != NULL && file_env[0] != '\0') {
        util_snprintf(g_diag.path, sizeof(g_diag.path), "%s", file_env);
    }
    ensure_log_file();
    g_diag.initialized = 1;
    return 0;
}

void diag_shutdown(void)
{
    if (g_diag.file != NULL) {
        fclose(g_diag.file);
        g_diag.file = NULL;
    }
    memset(&g_diag, 0, sizeof(g_diag));
}

void diag_log(enum diag_level level, const char *subsystem,
    const struct session *session, uint32_t req_id, const char *fmt, ...)
{
    char msg[1024];
    char line[1400];
    va_list ap;
    int session_id = session != NULL ? (int) session->session_id : 0;
    int pid = diag_getpid();

    if (!g_diag.initialized || level > g_diag.level) {
        return;
    }

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    util_snprintf(line, sizeof(line),
        "%llu %-5s pid=%d session=%d req=%u %-6s %s\n",
        (unsigned long long) util_now_ms(), level_name(level), pid,
        session_id, (unsigned int) req_id,
        subsystem != NULL ? subsystem : "misc", msg);

    fputs(line, stderr);
    rotate_logs_if_needed(strlen(line));
    ensure_log_file();
    if (g_diag.file != NULL) {
        fputs(line, g_diag.file);
        fflush(g_diag.file);
    }
}

static void diag_wire(const char *dir, const struct session *session,
    uint8_t type, uint32_t req_id, size_t payload_len, const void *frame, size_t frame_len)
{
    const unsigned char *bytes = (const unsigned char *) frame;
    char hex[1024];
    size_t i;
    size_t pos = 0;

    if (!g_diag.initialized || g_diag.wire_mode == WIRE_MODE_OFF) {
        return;
    }

    diag_log(DIAG_LEVEL_DEBUG, "frame", session, req_id,
        "%s type=%s len=%llu", dir, protocol_type_name(type),
        (unsigned long long) payload_len);

    if (g_diag.wire_mode != WIRE_MODE_HEX || bytes == NULL) {
        return;
    }

    for (i = 0; i < frame_len && pos + 4 < sizeof(hex); ++i) {
        pos += (size_t) snprintf(hex + pos, sizeof(hex) - pos, "%02X ", bytes[i]);
    }
    diag_log(DIAG_LEVEL_TRACE, "frame", session, req_id, "%s raw=%s", dir, hex);
}

void diag_wire_rx(const struct session *session, uint8_t type, uint32_t req_id,
    size_t payload_len, const void *frame, size_t frame_len)
{
    diag_wire("RX", session, type, req_id, payload_len, frame, frame_len);
}

void diag_wire_tx(const struct session *session, uint8_t type, uint32_t req_id,
    size_t payload_len, const void *frame, size_t frame_len)
{
    diag_wire("TX", session, type, req_id, payload_len, frame, frame_len);
}

void diag_count_frame_rx(struct session *session, size_t frame_len)
{
    g_diag.counters.frames_rx++;
    g_diag.counters.bytes_rx += frame_len;
    if (session != NULL) {
        session->frames_rx++;
        session->bytes_rx += frame_len;
    }
}

void diag_count_frame_tx(struct session *session, size_t frame_len)
{
    g_diag.counters.frames_tx++;
    g_diag.counters.bytes_tx += frame_len;
    if (session != NULL) {
        session->frames_tx++;
        session->bytes_tx += frame_len;
    }
}

#define DIAG_COUNTER_FN(name, field) \
void name(struct session *session) { \
    g_diag.counters.field++; \
    if (session != NULL) { session->field++; } \
}

DIAG_COUNTER_FN(diag_count_protocol_error, protocol_errors)
DIAG_COUNTER_FN(diag_count_lua_error, lua_op_failures)
DIAG_COUNTER_FN(diag_count_transfer_start, transfer_starts)
DIAG_COUNTER_FN(diag_count_transfer_complete, transfer_completes)
DIAG_COUNTER_FN(diag_count_transfer_abort, transfer_aborts)
DIAG_COUNTER_FN(diag_count_process_spawn, process_spawns)
DIAG_COUNTER_FN(diag_count_process_exit, process_exits)
DIAG_COUNTER_FN(diag_count_process_timeout, process_timeouts)
DIAG_COUNTER_FN(diag_count_session_disconnect, session_disconnects)
DIAG_COUNTER_FN(diag_count_heartbeat, heartbeat_events)
DIAG_COUNTER_FN(diag_count_relay_open, relay_opens)
DIAG_COUNTER_FN(diag_count_relay_close, relay_closes)

void diag_count_parse_failure(void)
{
    g_diag.counters.parse_failures++;
}

static void table_set_u64(struct lua_State *L, const char *key, uint64_t value)
{
    lua_pushstring(L, key);
    lua_pushinteger(L, (lua_Integer) value);
    lua_settable(L, -3);
}

static void table_set_str(struct lua_State *L, const char *key, const char *value)
{
    lua_pushstring(L, key);
    lua_pushstring(L, value != NULL ? value : "");
    lua_settable(L, -3);
}

void diag_push_sessions(struct lua_State *L, const struct session *session)
{
    lua_newtable(L);
    table_set_u64(L, "count", session != NULL ? 1u : 0u);
    if (session == NULL) {
        return;
    }

    table_set_u64(L, "session.1.id", session->session_id);
    table_set_u64(L, "session.1.last_rx_ms", session->last_rx_ms);
    table_set_u64(L, "session.1.last_tx_ms", session->last_tx_ms);
    table_set_u64(L, "session.1.pending_writes", session->pending_write_bytes);
    table_set_u64(L, "session.1.frames_rx", session->frames_rx);
    table_set_u64(L, "session.1.frames_tx", session->frames_tx);
    table_set_u64(L, "session.1.closed", session->closed ? 1u : 0u);
}

void diag_push_transfers(struct lua_State *L, const struct session *session)
{
    unsigned int count = 0;
    lua_newtable(L);
    if (session != NULL && session->upload.active) {
        count++;
        table_set_u64(L, "xfer.1.req_id", session->upload.req_id);
        table_set_str(L, "xfer.1.kind", "upload");
        table_set_str(L, "xfer.1.path", session->upload.path);
        table_set_u64(L, "xfer.1.offset", session->upload.offset);
        table_set_u64(L, "xfer.1.total", session->upload.total_size);
        table_set_u64(L, "xfer.1.last_progress_ms", session->upload.last_progress_ms);
    }
    if (session != NULL && session->download.active) {
        count++;
        table_set_u64(L, count == 1 ? "xfer.1.req_id" : "xfer.2.req_id", session->download.req_id);
        table_set_str(L, count == 1 ? "xfer.1.kind" : "xfer.2.kind", "download");
        table_set_str(L, count == 1 ? "xfer.1.path" : "xfer.2.path", session->download.path);
        table_set_u64(L, count == 1 ? "xfer.1.offset" : "xfer.2.offset", session->download.offset);
        table_set_u64(L, count == 1 ? "xfer.1.total" : "xfer.2.total", session->download.total_size);
        table_set_u64(L, count == 1 ? "xfer.1.last_progress_ms" : "xfer.2.last_progress_ms", session->download.last_progress_ms);
    }
    table_set_u64(L, "count", count);
}

void diag_push_procs(struct lua_State *L, const struct session *session)
{
    lua_newtable(L);
    table_set_u64(L, "count", (session != NULL && session->proc.active) ? 1u : 0u);
    if (session == NULL || !session->proc.active) {
        return;
    }

    table_set_u64(L, "proc.1.id", session->proc.proc_id);
    table_set_str(L, "proc.1.path", session->proc.file);
    table_set_u64(L, "proc.1.started_ms", session->proc.started_ms);
    table_set_u64(L, "proc.1.last_io_ms", session->proc.last_io_ms);
    table_set_u64(L, "proc.1.timeout_ms", session->proc.timeout_ms);
    table_set_u64(L, "proc.1.idle_timeout_ms", session->proc.idle_timeout_ms);
}

void diag_push_stats(struct lua_State *L, const struct session *session)
{
    lua_newtable(L);
    table_set_u64(L, "global.frames_rx", g_diag.counters.frames_rx);
    table_set_u64(L, "global.frames_tx", g_diag.counters.frames_tx);
    table_set_u64(L, "global.bytes_rx", g_diag.counters.bytes_rx);
    table_set_u64(L, "global.bytes_tx", g_diag.counters.bytes_tx);
    table_set_u64(L, "global.parse_failures", g_diag.counters.parse_failures);
    table_set_u64(L, "global.protocol_errors", g_diag.counters.protocol_errors);
    table_set_u64(L, "global.lua_op_failures", g_diag.counters.lua_op_failures);
    table_set_u64(L, "global.transfer_starts", g_diag.counters.transfer_starts);
    table_set_u64(L, "global.transfer_completes", g_diag.counters.transfer_completes);
    table_set_u64(L, "global.transfer_aborts", g_diag.counters.transfer_aborts);
    table_set_u64(L, "global.process_spawns", g_diag.counters.process_spawns);
    table_set_u64(L, "global.process_exits", g_diag.counters.process_exits);
    table_set_u64(L, "global.process_timeouts", g_diag.counters.process_timeouts);
    table_set_u64(L, "global.session_disconnects", g_diag.counters.session_disconnects);
    table_set_u64(L, "global.heartbeat_events", g_diag.counters.heartbeat_events);
    table_set_u64(L, "global.relay_opens", g_diag.counters.relay_opens);
    table_set_u64(L, "global.relay_closes", g_diag.counters.relay_closes);

    if (session != NULL) {
        table_set_u64(L, "session.id", session->session_id);
        table_set_u64(L, "session.frames_rx", session->frames_rx);
        table_set_u64(L, "session.frames_tx", session->frames_tx);
        table_set_u64(L, "session.bytes_rx", session->bytes_rx);
        table_set_u64(L, "session.bytes_tx", session->bytes_tx);
        table_set_u64(L, "session.protocol_errors", session->protocol_errors);
        table_set_u64(L, "session.lua_op_failures", session->lua_op_failures);
        table_set_u64(L, "session.transfer_starts", session->transfer_starts);
        table_set_u64(L, "session.transfer_completes", session->transfer_completes);
        table_set_u64(L, "session.transfer_aborts", session->transfer_aborts);
        table_set_u64(L, "session.process_spawns", session->process_spawns);
        table_set_u64(L, "session.process_exits", session->process_exits);
        table_set_u64(L, "session.process_timeouts", session->process_timeouts);
        table_set_u64(L, "session.session_disconnects", session->session_disconnects);
        table_set_u64(L, "session.heartbeat_events", session->heartbeat_events);
        table_set_u64(L, "session.relay_opens", session->relay_opens);
        table_set_u64(L, "session.relay_closes", session->relay_closes);
    }
}
