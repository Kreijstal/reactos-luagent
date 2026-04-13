#include "session.h"
#include "diag.h"
#include "frame.h"
#include "lua_engine.h"
#include "portfwd.h"
#include "proc.h"
#include "transfer.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t g_next_session_id = 1u;

struct write_req {
    uv_write_t req;
    uv_buf_t buf;
    struct session *session;
};

static void on_write_done(uv_write_t *req, int status)
{
    struct write_req *write_req = (struct write_req *) req;

    if (status < 0) {
        diag_log(DIAG_LEVEL_WARN, "net", write_req->session, 0,
            "write failed err=%s", uv_strerror(status));
    }

    if (write_req->session != NULL &&
            write_req->session->pending_write_bytes >= write_req->buf.len) {
        write_req->session->pending_write_bytes -= write_req->buf.len;
    }

    free(write_req->buf.base);
    free(write_req);
}

static void maybe_free_session(struct session *session)
{
    if (session == NULL) {
        return;
    }

    session->close_refs--;
    if (session->close_refs > 0) {
        return;
    }

    transfer_abort(&session->upload);
    transfer_abort(&session->download);
    proc_abort(&session->proc);
    if (session->engine != NULL) {
        portfwd_close_session(session->engine, session);
    }

    free(session->rx_buf);
    free(session);
}

void session_add_ref(struct session *session)
{
    if (session != NULL) {
        session->close_refs++;
    }
}

void session_release_ref(struct session *session)
{
    maybe_free_session(session);
}

static void on_close_done(uv_handle_t *handle)
{
    struct session *session = (struct session *) handle->data;
    maybe_free_session(session);
}

static void heartbeat_cb(uv_timer_t *timer)
{
    struct session *session = (struct session *) timer->data;
    uint64_t now_ms = util_now_ms();

    if (session == NULL || session->closed) {
        return;
    }

    if (now_ms - session->last_rx_ms >= LUAGENT_SESSION_IDLE_MS) {
        diag_log(DIAG_LEVEL_WARN, "timer", session, 0,
            "session idle timeout elapsed_ms=%llu threshold_ms=%u action=close",
            (unsigned long long) (now_ms - session->last_rx_ms),
            (unsigned int) LUAGENT_SESSION_IDLE_MS);
        session_close(session);
        return;
    }

    if (now_ms - session->last_tx_ms >= LUAGENT_HEARTBEAT_IDLE_MS) {
        diag_count_heartbeat(session);
        diag_log(DIAG_LEVEL_DEBUG, "timer", session, 0,
            "heartbeat send elapsed_ms=%llu threshold_ms=%u",
            (unsigned long long) (now_ms - session->last_tx_ms),
            (unsigned int) LUAGENT_HEARTBEAT_IDLE_MS);
        session_send_text(session, FRAME_PING, session->next_req_id++, "status=ping");
    }

    transfer_check_timeout(session, &session->upload, now_ms);
    transfer_check_timeout(session, &session->download, now_ms);
    proc_check_timeouts(session, now_ms);
}

void session_init(struct session *session)
{
    memset(session, 0, sizeof(*session));
    session->tcp.data = session;
    session->heartbeat_timer.data = session;
    transfer_init(&session->upload);
    transfer_init(&session->download);
    proc_init(&session->proc);
    session->session_id = g_next_session_id++;
    session->last_rx_ms = util_now_ms();
    session->last_tx_ms = session->last_rx_ms;
    session->next_req_id = 1u;
    session->next_proc_id = 1u;
}

int session_start_timers(struct session *session)
{
    int rc;

    rc = uv_timer_init(session->loop, &session->heartbeat_timer);
    if (rc != 0) {
        return rc;
    }
    session->heartbeat_timer.data = session;
    session_add_ref(session);
    session->timer_started = 1;
    rc = uv_timer_start(&session->heartbeat_timer, heartbeat_cb, 1000u, 1000u);
    if (rc != 0) {
        uv_close((uv_handle_t *) &session->heartbeat_timer, on_close_done);
    }
    return rc;
}

void session_reset_buffer(struct session *session)
{
    if (session == NULL) {
        return;
    }

    session->rx_used = 0;
}

void session_close(struct session *session)
{
    if (session == NULL || session->closed) {
        return;
    }

    session->closed = 1;
    diag_count_session_disconnect(session);
    diag_log(DIAG_LEVEL_INFO, "net", session, 0, "session close");
    if (session->timer_started) {
        uv_timer_stop(&session->heartbeat_timer);
        uv_close((uv_handle_t *) &session->heartbeat_timer, on_close_done);
        session->timer_started = 0;
    }
    session_add_ref(session);
    uv_close((uv_handle_t *) &session->tcp, on_close_done);
}

int session_send_frame(struct session *session, uint8_t type, uint32_t req_id,
    const void *payload, size_t payload_len)
{
    struct write_req *write_req;
    struct frame_header header;
    size_t header_size;

    if (session == NULL || session->closed) {
        return -1;
    }

    header_size = frame_header_size();
    if (session->pending_write_bytes + header_size + payload_len >
            LUAGENT_MAX_WRITE_QUEUE_BYTES) {
        diag_log(DIAG_LEVEL_WARN, "net", session, req_id,
            "write queue exceeded pending=%llu add=%llu action=close",
            (unsigned long long) session->pending_write_bytes,
            (unsigned long long) (header_size + payload_len));
        session_close(session);
        return -1;
    }

    header.type = type;
    header.flags = 0;
    header.req_id = req_id;
    header.length = (uint32_t) payload_len;

    write_req = (struct write_req *) calloc(1, sizeof(*write_req));
    if (write_req == NULL) {
        return -1;
    }

    write_req->session = session;
    write_req->buf = uv_buf_init((char *) malloc(header_size + payload_len),
        (unsigned int) (header_size + payload_len));
    if (write_req->buf.base == NULL) {
        free(write_req);
        return -1;
    }

    if (frame_encode_header((unsigned char *) write_req->buf.base,
            header_size, &header) != 0) {
        free(write_req->buf.base);
        free(write_req);
        return -1;
    }

    if (payload_len > 0 && payload != NULL) {
        memcpy(write_req->buf.base + header_size, payload, payload_len);
    }

    diag_count_frame_tx(session, write_req->buf.len);
    diag_wire_tx(session, type, req_id, payload_len, write_req->buf.base, write_req->buf.len);
    session->last_tx_ms = util_now_ms();
    session->pending_write_bytes += write_req->buf.len;
    if (uv_write(&write_req->req, (uv_stream_t *) &session->tcp,
            &write_req->buf, 1, on_write_done) != 0) {
        session->pending_write_bytes -= write_req->buf.len;
        free(write_req->buf.base);
        free(write_req);
        return -1;
    }

    return 0;
}

int session_send_text(struct session *session, uint8_t type, uint32_t req_id,
    const char *payload)
{
    size_t payload_len = 0;

    if (payload != NULL) {
        payload_len = strlen(payload);
    }

    return session_send_frame(session, type, req_id, payload, payload_len);
}

int session_send_error(struct session *session, uint32_t req_id,
    const char *code, const char *message)
{
    char payload[512];

    if (util_snprintf(payload, sizeof(payload), "code=%s\nmessage=%s",
            code != NULL ? code : "internal",
            message != NULL ? message : "") != 0) {
        return -1;
    }

    return session_send_text(session, FRAME_ERROR, req_id, payload);
}
