#include "portfwd.h"

#include "diag.h"
#include "lua_engine.h"
#include "session.h"
#include "util.h"

#include <errno.h>
#include <lua.h>
#include <lauxlib.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

#define PORTFWD_MAX_PENDING_BYTES 262144u

struct relay_conn;

struct portfwd_relay {
    struct lua_engine *engine;
    struct session *owner;
    struct portfwd_relay *next;
    struct relay_conn *conn;
    uv_tcp_t listener;
    uint32_t relay_id;
    uint16_t listen_port;
    uint16_t target_port;
    int closing;
    char *listen_host;
    char *target_host;
};

struct relay_write_req {
    uv_write_t req;
    uv_buf_t buf;
    struct relay_conn *conn;
    int direction;
};

struct relay_conn {
    struct portfwd_relay *relay;
    uv_tcp_t inbound;
    uv_tcp_t outbound;
    uv_connect_t connect_req;
    size_t pending_to_inbound;
    size_t pending_to_outbound;
    int inbound_closed;
    int outbound_closed;
    int inbound_connected;
    int outbound_connected;
    int closing;
};

static void relay_close(struct relay_conn *conn);
static void relay_maybe_free(struct relay_conn *conn);
static void relay_remove(struct lua_engine *engine, struct portfwd_relay *relay);
static void relay_listener_closed_cb(uv_handle_t *handle);

void portfwd_engine_init(struct lua_engine *engine)
{
    engine->relays = NULL;
    engine->next_relay_id = 1u;
}

static void relay_close_cb(uv_handle_t *handle)
{
    struct relay_conn *conn = (struct relay_conn *) handle->data;
    if (conn == NULL) {
        return;
    }

    if (handle == (uv_handle_t *) &conn->inbound) {
        conn->inbound_closed = 1;
    } else if (handle == (uv_handle_t *) &conn->outbound) {
        conn->outbound_closed = 1;
    }

    relay_maybe_free(conn);
}

static void relay_maybe_free(struct relay_conn *conn)
{
    if (conn != NULL && conn->inbound_closed && conn->outbound_closed) {
        if (conn->relay != NULL) {
            conn->relay->conn = NULL;
        }
        free(conn);
    }
}

static void relay_close(struct relay_conn *conn)
{
    if (conn == NULL || conn->closing) {
        return;
    }

    conn->closing = 1;
    if (!conn->inbound_closed) {
        uv_close((uv_handle_t *) &conn->inbound, relay_close_cb);
    }
    if (!conn->outbound_closed) {
        uv_close((uv_handle_t *) &conn->outbound, relay_close_cb);
    }
}

static void relay_alloc_cb(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf)
{
    (void) handle;
    buf->base = (char *) malloc(suggested_size > 0 ? suggested_size : 4096u);
    buf->len = suggested_size > 0 ? suggested_size : 4096u;
}

static void relay_write_cb(uv_write_t *req, int status)
{
    struct relay_write_req *write_req = (struct relay_write_req *) req;

    if (write_req->conn != NULL) {
        if (write_req->direction == 0) {
            if (write_req->conn->pending_to_outbound >= write_req->buf.len) {
                write_req->conn->pending_to_outbound -= write_req->buf.len;
            }
        } else {
            if (write_req->conn->pending_to_inbound >= write_req->buf.len) {
                write_req->conn->pending_to_inbound -= write_req->buf.len;
            }
        }
        if (status < 0) {
            relay_close(write_req->conn);
        }
    }

    free(write_req->buf.base);
    free(write_req);
}

static void relay_forward(struct relay_conn *conn, int direction,
    const char *data, size_t len)
{
    struct relay_write_req *write_req;
    uv_stream_t *dst;
    size_t *pending;

    if (conn == NULL || conn->closing || len == 0) {
        return;
    }

    if (direction == 0) {
        dst = (uv_stream_t *) &conn->outbound;
        pending = &conn->pending_to_outbound;
    } else {
        dst = (uv_stream_t *) &conn->inbound;
        pending = &conn->pending_to_inbound;
    }

    if (*pending + len > PORTFWD_MAX_PENDING_BYTES) {
        relay_close(conn);
        return;
    }

    write_req = (struct relay_write_req *) calloc(1, sizeof(*write_req));
    if (write_req == NULL) {
        relay_close(conn);
        return;
    }

    write_req->buf = uv_buf_init((char *) malloc(len), (unsigned int) len);
    if (write_req->buf.base == NULL) {
        free(write_req);
        relay_close(conn);
        return;
    }

    memcpy(write_req->buf.base, data, len);
    write_req->conn = conn;
    write_req->direction = direction;
    *pending += len;

    if (uv_write(&write_req->req, dst, &write_req->buf, 1, relay_write_cb) != 0) {
        *pending -= len;
        free(write_req->buf.base);
        free(write_req);
        relay_close(conn);
    }
}

static void relay_read_cb(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf)
{
    struct relay_conn *conn = (struct relay_conn *) stream->data;

    if (nread > 0) {
        if (stream == (uv_stream_t *) &conn->inbound) {
            relay_forward(conn, 0, buf->base, (size_t) nread);
        } else {
            relay_forward(conn, 1, buf->base, (size_t) nread);
        }
    } else if (nread < 0) {
        relay_close(conn);
    }

    free(buf->base);
}

static void relay_connect_cb(uv_connect_t *req, int status)
{
    struct relay_conn *conn = (struct relay_conn *) req->data;

    if (conn == NULL) {
        return;
    }

    if (status < 0) {
        relay_close(conn);
        return;
    }

    conn->outbound_connected = 1;
    uv_read_start((uv_stream_t *) &conn->inbound, relay_alloc_cb, relay_read_cb);
    uv_read_start((uv_stream_t *) &conn->outbound, relay_alloc_cb, relay_read_cb);
}

static void relay_accept_cb(uv_stream_t *server, int status)
{
    struct portfwd_relay *relay = (struct portfwd_relay *) server->data;
    struct relay_conn *conn;
    struct sockaddr_in target_addr;

    if (status < 0 || relay == NULL || relay->closing) {
        return;
    }

    if (relay->conn != NULL) {
        uv_tcp_t *reject_client = (uv_tcp_t *) calloc(1, sizeof(*reject_client));
        if (reject_client != NULL &&
                uv_tcp_init(relay->owner->loop, reject_client) == 0 &&
                uv_accept(server, (uv_stream_t *) reject_client) == 0) {
            uv_close((uv_handle_t *) reject_client, relay_listener_closed_cb);
        } else {
            free(reject_client);
        }
        return;
    }

    conn = (struct relay_conn *) calloc(1, sizeof(*conn));
    if (conn == NULL) {
        return;
    }

    conn->relay = relay;
    if (uv_tcp_init(relay->owner->loop, &conn->inbound) != 0 ||
            uv_tcp_init(relay->owner->loop, &conn->outbound) != 0) {
        free(conn);
        return;
    }

    conn->inbound.data = conn;
    conn->outbound.data = conn;
    conn->inbound_connected = 1;

    if (uv_accept(server, (uv_stream_t *) &conn->inbound) != 0) {
        uv_close((uv_handle_t *) &conn->inbound, relay_close_cb);
        uv_close((uv_handle_t *) &conn->outbound, relay_close_cb);
        return;
    }

    if (uv_ip4_addr(relay->target_host, relay->target_port, &target_addr) != 0) {
        relay_close(conn);
        return;
    }

    relay->conn = conn;
    conn->connect_req.data = conn;
    if (uv_tcp_connect(&conn->connect_req, &conn->outbound,
            (const struct sockaddr *) &target_addr, relay_connect_cb) != 0) {
        relay_close(conn);
    }
}

static void relay_listener_closed_cb(uv_handle_t *handle)
{
    struct portfwd_relay *relay = (struct portfwd_relay *) handle->data;

    if (relay == NULL) {
        free(handle);
        return;
    }

    free(relay->listen_host);
    free(relay->target_host);
    free(relay);
}

static void relay_remove(struct lua_engine *engine, struct portfwd_relay *relay)
{
    struct portfwd_relay **it = &engine->relays;

    while (*it != NULL) {
        if (*it == relay) {
            *it = relay->next;
            return;
        }
        it = &(*it)->next;
    }
}

static void relay_destroy(struct portfwd_relay *relay)
{
    if (relay == NULL || relay->closing) {
        return;
    }

    relay->closing = 1;
    if (relay->conn != NULL) {
        relay_close(relay->conn);
        relay->conn = NULL;
    }
    relay_remove(relay->engine, relay);
    uv_close((uv_handle_t *) &relay->listener, relay_listener_closed_cb);
}

void portfwd_engine_close(struct lua_engine *engine)
{
    struct portfwd_relay *relay = engine->relays;
    struct portfwd_relay *next;

    while (relay != NULL) {
        next = relay->next;
        relay_destroy(relay);
        relay = next;
    }
}

void portfwd_close_session(struct lua_engine *engine, struct session *session)
{
    struct portfwd_relay *relay = engine->relays;
    struct portfwd_relay *next;

    while (relay != NULL) {
        next = relay->next;
        if (relay->owner == session) {
            relay_destroy(relay);
        }
        relay = next;
    }
}

int portfwd_open(struct lua_engine *engine, struct session *session,
    const char *listen_host, uint16_t listen_port,
    const char *target_host, uint16_t target_port,
    uint32_t *relay_id_out, uint16_t *actual_port_out)
{
    struct portfwd_relay *relay;
    struct sockaddr_in bind_addr;
    struct sockaddr_storage sockname;
    int namelen;

    relay = (struct portfwd_relay *) calloc(1, sizeof(*relay));
    if (relay == NULL) {
        return UV_ENOMEM;
    }

    relay->engine = engine;
    relay->owner = session;
    relay->relay_id = engine->next_relay_id++;
    relay->listen_port = listen_port;
    relay->target_port = target_port;
    relay->listen_host = strdup(listen_host != NULL ? listen_host : "0.0.0.0");
    relay->target_host = strdup(target_host != NULL ? target_host : "127.0.0.1");
    if (relay->listen_host == NULL || relay->target_host == NULL) {
        free(relay->listen_host);
        free(relay->target_host);
        free(relay);
        return UV_ENOMEM;
    }

    if (uv_tcp_init(session->loop, &relay->listener) != 0) {
        free(relay->listen_host);
        free(relay->target_host);
        free(relay);
        return UV_EIO;
    }
    relay->listener.data = relay;

    if (uv_ip4_addr(relay->listen_host, relay->listen_port, &bind_addr) != 0) {
        relay->closing = 1;
        uv_close((uv_handle_t *) &relay->listener, relay_listener_closed_cb);
        return UV_EINVAL;
    }

    if (uv_tcp_bind(&relay->listener, (const struct sockaddr *) &bind_addr, 0) != 0) {
        relay->closing = 1;
        uv_close((uv_handle_t *) &relay->listener, relay_listener_closed_cb);
        return UV_EADDRINUSE;
    }

    if (uv_listen((uv_stream_t *) &relay->listener, 8, relay_accept_cb) != 0) {
        relay->closing = 1;
        uv_close((uv_handle_t *) &relay->listener, relay_listener_closed_cb);
        return UV_EIO;
    }

    relay->next = engine->relays;
    engine->relays = relay;
    diag_count_relay_open(session);
    diag_log(DIAG_LEVEL_INFO, "relay", session, 0,
        "open relay_id=%u listen=%s:%u target=%s:%u",
        (unsigned int) relay->relay_id, relay->listen_host,
        (unsigned int) relay->listen_port, relay->target_host,
        (unsigned int) relay->target_port);

    namelen = (int) sizeof(sockname);
    if (uv_tcp_getsockname(&relay->listener, (struct sockaddr *) &sockname, &namelen) == 0) {
        struct sockaddr_in *sin = (struct sockaddr_in *) &sockname;
        relay->listen_port = ntohs(sin->sin_port);
    }

    if (relay_id_out != NULL) {
        *relay_id_out = relay->relay_id;
    }
    if (actual_port_out != NULL) {
        *actual_port_out = relay->listen_port;
    }
    return 0;
}

int portfwd_close(struct lua_engine *engine, uint32_t relay_id)
{
    struct portfwd_relay *relay = engine->relays;

    while (relay != NULL) {
        if (relay->relay_id == relay_id) {
            diag_count_relay_close(relay->owner);
            diag_log(DIAG_LEVEL_INFO, "relay", relay->owner, 0,
                "close relay_id=%u", (unsigned int) relay_id);
            relay_destroy(relay);
            return 0;
        }
        relay = relay->next;
    }

    return UV_ENOENT;
}

void portfwd_push_list(struct lua_State *L, struct lua_engine *engine,
    struct session *session)
{
    struct portfwd_relay *relay = engine->relays;
    int index = 1;

    lua_newtable(L);
    while (relay != NULL) {
        if (session == NULL || relay->owner == session) {
            lua_pushinteger(L, index++);
            lua_newtable(L);

            lua_pushinteger(L, (lua_Integer) relay->relay_id);
            lua_setfield(L, -2, "relay_id");

            lua_pushstring(L, relay->listen_host);
            lua_setfield(L, -2, "listen_host");

            lua_pushinteger(L, (lua_Integer) relay->listen_port);
            lua_setfield(L, -2, "listen_port");

            lua_pushstring(L, relay->target_host);
            lua_setfield(L, -2, "target_host");

            lua_pushinteger(L, (lua_Integer) relay->target_port);
            lua_setfield(L, -2, "target_port");

            lua_pushboolean(L, relay->conn != NULL);
            lua_setfield(L, -2, "connected");

            lua_settable(L, -3);
        }
        relay = relay->next;
    }
}
