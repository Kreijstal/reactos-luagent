#include "server.h"

#include "diag.h"
#include "frame.h"
#include "lua_engine.h"
#include "protocol.h"
#include "session.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

struct server_state {
    uv_loop_t *loop;
    uv_tcp_t listener;
    struct lua_engine lua;
};

static void alloc_cb(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf)
{
    (void) handle;
    (void) suggested_size;
    buf->base = (char *) malloc(LUAGENT_READ_CHUNK);
    buf->len = LUAGENT_READ_CHUNK;
}

static void read_cb(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf)
{
    struct session *client = (struct session *) stream->data;
    size_t offset = 0;

    if (nread < 0) {
        if (nread != UV_EOF) {
            diag_log(DIAG_LEVEL_WARN, "net", client, 0,
                "read failed err=%s", uv_strerror((int) nread));
        }
        free(buf->base);
        session_close(client);
        return;
    }

    if (nread == 0) {
        free(buf->base);
        return;
    }

    client->last_rx_ms = util_now_ms();

    if (client->rx_used + (size_t) nread > client->rx_cap) {
        diag_log(DIAG_LEVEL_WARN, "frame", client, 0,
            "read buffer overflow used=%llu add=%llu cap=%llu",
            (unsigned long long) client->rx_used,
            (unsigned long long) nread,
            (unsigned long long) client->rx_cap);
        free(buf->base);
        session_close(client);
        return;
    }

    memcpy(client->rx_buf + client->rx_used, buf->base, (size_t) nread);
    client->rx_used += (size_t) nread;
    free(buf->base);

    while (client->rx_used - offset >= frame_header_size()) {
        struct frame_header header;
        int status_header = frame_decode_header(client->rx_buf + offset,
            client->rx_used - offset, &header);
        size_t frame_size;

        if (status_header == FRAME_STATUS_NEED_MORE) {
            break;
        }
        if (status_header == FRAME_STATUS_ERROR) {
            diag_count_parse_failure();
            diag_log(DIAG_LEVEL_WARN, "frame", client, 0,
                "bad frame header action=close");
            session_close(client);
            return;
        }

        frame_size = frame_header_size() + header.length;
        if (client->rx_used - offset < frame_size) {
            break;
        }

        diag_count_frame_rx(client, frame_size);
        diag_wire_rx(client, header.type, header.req_id, header.length,
            client->rx_buf + offset, frame_size);
        if (protocol_handle_control(client, client->engine, header.type,
                header.req_id,
                (const char *) (client->rx_buf + offset + frame_header_size()),
                header.length) != 0 &&
                header.type != FRAME_OP) {
            diag_log(DIAG_LEVEL_DEBUG, "proto", client, header.req_id,
                "handler returned failure type=%s", protocol_type_name(header.type));
        }

        offset += frame_size;
    }

    if (offset > 0) {
        memmove(client->rx_buf, client->rx_buf + offset,
            client->rx_used - offset);
        client->rx_used -= offset;
    }
}

static void on_new_connection(uv_stream_t *server, int status)
{
    struct server_state *state = (struct server_state *) server->data;
    struct session *session;
    int rc;

    if (status < 0) {
        diag_log(DIAG_LEVEL_WARN, "net", NULL, 0,
            "accept failed err=%s", uv_strerror(status));
        return;
    }

    session = (struct session *) calloc(1, sizeof(*session));
    if (session == NULL) {
        diag_log(DIAG_LEVEL_ERROR, "net", NULL, 0, "accept failed out_of_memory");
        return;
    }

    session_init(session);
    session->loop = state->loop;
    session->engine = &state->lua;
    session->rx_cap = LUAGENT_READ_CHUNK * 4u;
    session->rx_buf = (unsigned char *) malloc(session->rx_cap);
    if (session->rx_buf == NULL) {
        free(session);
        diag_log(DIAG_LEVEL_ERROR, "net", NULL, 0, "accept failed out_of_memory");
        return;
    }

    rc = uv_tcp_init(state->loop, &session->tcp);
    if (rc != 0) {
        diag_log(DIAG_LEVEL_ERROR, "net", session, 0,
            "uv_tcp_init session failed err=%s", uv_strerror(rc));
        free(session->rx_buf);
        free(session);
        return;
    }
    session->tcp.data = session;

    rc = uv_accept(server, (uv_stream_t *) &session->tcp);
    if (rc != 0) {
        diag_log(DIAG_LEVEL_WARN, "net", session, 0,
            "uv_accept failed err=%s", uv_strerror(rc));
        session_close(session);
        return;
    }

    rc = session_start_timers(session);
    if (rc != 0) {
        diag_log(DIAG_LEVEL_ERROR, "timer", session, 0,
            "session_start_timers failed err=%s", uv_strerror(rc));
        session_close(session);
        return;
    }

    diag_log(DIAG_LEVEL_INFO, "net", session, 0, "accepted");
    rc = uv_read_start((uv_stream_t *) &session->tcp, alloc_cb, read_cb);

    if (rc != 0) {
        diag_log(DIAG_LEVEL_ERROR, "net", session, 0,
            "uv_read_start failed err=%s", uv_strerror(rc));
        session_close(session);
        return;
    }
}

int server_run(const char *bind_host, int port)
{
    struct server_state state;
    struct sockaddr_in addr;
    int rc;

    memset(&state, 0, sizeof(state));
    state.loop = uv_default_loop();
    diag_init();

    if (lua_engine_init(&state.lua) != 0) {
        return -1;
    }

    rc = uv_tcp_init(state.loop, &state.listener);
    if (rc != 0) {
        diag_log(DIAG_LEVEL_ERROR, "net", NULL, 0,
            "uv_tcp_init failed err=%s", uv_strerror(rc));
        lua_engine_close(&state.lua);
        diag_shutdown();
        return -1;
    }
    state.listener.data = &state;

    rc = uv_ip4_addr(bind_host, port, &addr);
    if (rc != 0) {
        diag_log(DIAG_LEVEL_ERROR, "net", NULL, 0,
            "uv_ip4_addr failed err=%s", uv_strerror(rc));
        lua_engine_close(&state.lua);
        diag_shutdown();
        return -1;
    }

    rc = uv_tcp_bind(&state.listener, (const struct sockaddr *) &addr, 0);
    if (rc != 0) {
        diag_log(DIAG_LEVEL_ERROR, "net", NULL, 0,
            "uv_tcp_bind failed err=%s", uv_strerror(rc));
        lua_engine_close(&state.lua);
        diag_shutdown();
        return -1;
    }

    rc = uv_listen((uv_stream_t *) &state.listener, 16, on_new_connection);
    if (rc != 0) {
        diag_log(DIAG_LEVEL_ERROR, "net", NULL, 0,
            "uv_listen failed err=%s", uv_strerror(rc));
        lua_engine_close(&state.lua);
        diag_shutdown();
        return -1;
    }

    diag_log(DIAG_LEVEL_INFO, "net", NULL, 0,
        "listening host=%s port=%d", bind_host, port);
    rc = uv_run(state.loop, UV_RUN_DEFAULT);
    lua_engine_close(&state.lua);
    diag_shutdown();
    return rc;
}
