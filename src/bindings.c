#include "bindings.h"
#include "diag.h"
#include "frame.h"
#include "lua_engine.h"
#include "portfwd.h"
#include "proc.h"
#include "session.h"
#include "util.h"

#include <lua.h>
#include <lauxlib.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <uv.h>

static struct lua_engine *binding_engine(lua_State *L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, "luagent.engine");
    if (!lua_islightuserdata(L, -1)) {
        lua_pop(L, 1);
        return NULL;
    }

    {
        struct lua_engine *engine =
            (struct lua_engine *) lua_touserdata(L, -1);
        lua_pop(L, 1);
        return engine;
    }
}

static int l_log(lua_State *L)
{
    struct lua_engine *engine = binding_engine(L);
    struct session *session = engine != NULL ? engine->active_session : NULL;
    const char *level = luaL_checkstring(L, 1);
    const char *msg = luaL_checkstring(L, 2);
    enum diag_level diag_level = DIAG_LEVEL_INFO;

    if (strcmp(level, "ERROR") == 0) diag_level = DIAG_LEVEL_ERROR;
    else if (strcmp(level, "WARN") == 0) diag_level = DIAG_LEVEL_WARN;
    else if (strcmp(level, "DEBUG") == 0) diag_level = DIAG_LEVEL_DEBUG;
    else if (strcmp(level, "TRACE") == 0) diag_level = DIAG_LEVEL_TRACE;

    diag_log(diag_level, "lua", session, 0, "%s", msg);
    return 0;
}

static int l_now_ms(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer) (uv_hrtime() / 1000000u));
    return 1;
}

static int l_reply(lua_State *L)
{
    struct lua_engine *engine = binding_engine(L);
    unsigned int req_id = (unsigned int) luaL_checkinteger(L, 1);
    const char *kind = luaL_checkstring(L, 2);
    const char *payload = luaL_optstring(L, 3, "");
    uint8_t type = FRAME_OP_RESULT;

    if (engine == NULL || engine->active_session == NULL) {
        return luaL_error(L, "no active session");
    }

    if (strcmp(kind, "ACK") == 0) {
        type = FRAME_ACK;
    } else if (strcmp(kind, "OP_RESULT") == 0) {
        type = FRAME_OP_RESULT;
    } else if (strcmp(kind, "PONG") == 0) {
        type = FRAME_PONG;
    }

    if (session_send_text(engine->active_session, type, req_id, payload) != 0) {
        return luaL_error(L, "failed to send reply");
    }

    return 0;
}

static int l_error(lua_State *L)
{
    struct lua_engine *engine = binding_engine(L);
    unsigned int req_id = (unsigned int) luaL_checkinteger(L, 1);
    const char *code = luaL_checkstring(L, 2);
    const char *message = luaL_optstring(L, 3, "");

    if (engine == NULL || engine->active_session == NULL) {
        return luaL_error(L, "no active session");
    }

    if (session_send_error(engine->active_session, req_id, code, message) != 0) {
        return luaL_error(L, "failed to send error");
    }

    return 0;
}

static int l_fs_list(lua_State *L)
{
    uv_fs_t req;
    uv_dirent_t dent;
    const char *path = luaL_checkstring(L, 1);
    int rc;
    int index = 1;

    lua_newtable(L);

    rc = uv_fs_scandir(NULL, &req, path, 0, NULL);
    if (rc < 0) {
        return luaL_error(L, "fs_list failed for '%s': %s",
            path, uv_strerror(rc));
    }

    while (uv_fs_scandir_next(&req, &dent) != UV_EOF) {
        lua_pushinteger(L, index++);
        lua_pushstring(L, dent.name);
        lua_settable(L, -3);
    }

    uv_fs_req_cleanup(&req);
    return 1;
}

static int l_port_open(lua_State *L)
{
    struct lua_engine *engine = binding_engine(L);
    struct session *session;
    const char *listen_host = luaL_optstring(L, 1, "0.0.0.0");
    unsigned int listen_port = (unsigned int) luaL_checkinteger(L, 2);
    const char *target_host = luaL_optstring(L, 3, "127.0.0.1");
    unsigned int target_port = (unsigned int) luaL_checkinteger(L, 4);
    uint32_t relay_id = 0;
    uint16_t actual_port = 0;
    int rc;

    if (engine == NULL || engine->active_session == NULL) {
        return luaL_error(L, "no active session");
    }

    session = engine->active_session;
    rc = portfwd_open(engine, session, listen_host, (uint16_t) listen_port,
        target_host, (uint16_t) target_port, &relay_id, &actual_port);
    if (rc != 0) {
        return luaL_error(L, "port_open failed: %s", uv_strerror(rc));
    }

    lua_newtable(L);
    lua_pushinteger(L, (lua_Integer) relay_id);
    lua_setfield(L, -2, "relay_id");
    lua_pushinteger(L, (lua_Integer) actual_port);
    lua_setfield(L, -2, "listen_port");
    return 1;
}

static int l_port_close(lua_State *L)
{
    struct lua_engine *engine = binding_engine(L);
    uint32_t relay_id = (uint32_t) luaL_checkinteger(L, 1);
    int rc;

    if (engine == NULL) {
        return luaL_error(L, "no engine");
    }

    rc = portfwd_close(engine, relay_id);
    if (rc != 0) {
        return luaL_error(L, "port_close failed: %s", uv_strerror(rc));
    }

    lua_pushboolean(L, 1);
    return 1;
}

static int l_port_list(lua_State *L)
{
    struct lua_engine *engine = binding_engine(L);
    if (engine == NULL || engine->active_session == NULL) {
        return luaL_error(L, "no active session");
    }

    portfwd_push_list(L, engine, engine->active_session);
    return 1;
}

static int l_debug_sessions(lua_State *L)
{
    struct lua_engine *engine = binding_engine(L);
    struct session *session = engine != NULL ? engine->active_session : NULL;
    if (session == NULL) {
        return luaL_error(L, "no active session");
    }
    diag_push_sessions(L, session);
    return 1;
}

static int l_debug_transfers(lua_State *L)
{
    struct lua_engine *engine = binding_engine(L);
    struct session *session = engine != NULL ? engine->active_session : NULL;
    if (session == NULL) {
        return luaL_error(L, "no active session");
    }
    diag_push_transfers(L, session);
    return 1;
}

static int l_debug_procs(lua_State *L)
{
    struct lua_engine *engine = binding_engine(L);
    struct session *session = engine != NULL ? engine->active_session : NULL;
    if (session == NULL) {
        return luaL_error(L, "no active session");
    }
    diag_push_procs(L, session);
    return 1;
}

static int l_debug_stats(lua_State *L)
{
    struct lua_engine *engine = binding_engine(L);
    struct session *session = engine != NULL ? engine->active_session : NULL;
    if (session == NULL) {
        return luaL_error(L, "no active session");
    }
    diag_push_stats(L, session);
    return 1;
}

static int l_proc_spawn(lua_State *L)
{
    struct lua_engine *engine = binding_engine(L);
    struct session *session = engine != NULL ? engine->active_session : NULL;
    const char *path = luaL_checkstring(L, 1);
    size_t argc;
    size_t i;
    const char **argv = NULL;
    uint64_t timeout_ms = (uint64_t) luaL_optinteger(L, 3, 60000);
    uint64_t idle_timeout_ms = (uint64_t) luaL_optinteger(L, 4, 10000);
    uint32_t proc_id = 0;

    if (session == NULL) {
        return luaL_error(L, "no active session");
    }
    luaL_checktype(L, 2, LUA_TTABLE);
    argc = (size_t) lua_rawlen(L, 2);
    if (argc == 0) {
        return luaL_error(L, "argv table must not be empty");
    }

    argv = (const char **) calloc(argc, sizeof(*argv));
    if (argv == NULL) {
        return luaL_error(L, "out of memory");
    }

    for (i = 0; i < argc; ++i) {
        lua_rawgeti(L, 2, (lua_Integer) (i + 1));
        argv[i] = luaL_checkstring(L, -1);
        lua_pop(L, 1);
    }

    if (proc_spawn_direct(session, path, argv, argc, timeout_ms, idle_timeout_ms, &proc_id) != 0) {
        free(argv);
        return luaL_error(L, "proc_spawn failed");
    }
    free(argv);

    lua_newtable(L);
    lua_pushinteger(L, (lua_Integer) proc_id);
    lua_setfield(L, -2, "proc_id");
    lua_pushstring(L, path);
    lua_setfield(L, -2, "path");
    return 1;
}

void bindings_register(lua_State *L, struct lua_engine *engine)
{
    lua_pushlightuserdata(L, engine);
    lua_setfield(L, LUA_REGISTRYINDEX, "luagent.engine");

    lua_newtable(L);

    lua_pushcfunction(L, l_log);
    lua_setfield(L, -2, "log");

    lua_pushcfunction(L, l_now_ms);
    lua_setfield(L, -2, "now_ms");

    lua_pushcfunction(L, l_reply);
    lua_setfield(L, -2, "reply");

    lua_pushcfunction(L, l_error);
    lua_setfield(L, -2, "error");

    lua_pushcfunction(L, l_fs_list);
    lua_setfield(L, -2, "fs_list");

    lua_pushcfunction(L, l_port_open);
    lua_setfield(L, -2, "port_open");

    lua_pushcfunction(L, l_port_close);
    lua_setfield(L, -2, "port_close");

    lua_pushcfunction(L, l_port_list);
    lua_setfield(L, -2, "port_list");

    lua_pushcfunction(L, l_debug_sessions);
    lua_setfield(L, -2, "debug_sessions");

    lua_pushcfunction(L, l_debug_transfers);
    lua_setfield(L, -2, "debug_transfers");

    lua_pushcfunction(L, l_debug_procs);
    lua_setfield(L, -2, "debug_procs");

    lua_pushcfunction(L, l_debug_stats);
    lua_setfield(L, -2, "debug_stats");

    lua_pushcfunction(L, l_proc_spawn);
    lua_setfield(L, -2, "proc_spawn");

    lua_setglobal(L, "agent");
}
