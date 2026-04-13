#include "lua_engine.h"
#include "bindings.h"
#include "diag.h"
#include "portfwd.h"

#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
#include <stdio.h>
#include <string.h>

int lua_engine_init(struct lua_engine *engine)
{
    if (engine == NULL) {
        return -1;
    }

    memset(engine, 0, sizeof(*engine));
    portfwd_engine_init(engine);
    engine->L = luaL_newstate();
    if (engine->L == NULL) {
        return -1;
    }

    luaL_openlibs(engine->L);
    bindings_register(engine->L, engine);

    if (luaL_dofile(engine->L, "lua/bootstrap.lua") != LUA_OK) {
        diag_log(DIAG_LEVEL_ERROR, "lua", NULL, 0,
            "bootstrap failed err=%s", lua_tostring(engine->L, -1));
        lua_engine_close(engine);
        return -1;
    }

    return 0;
}

void lua_engine_close(struct lua_engine *engine)
{
    portfwd_engine_close(engine);
    if (engine != NULL && engine->L != NULL) {
        lua_close(engine->L);
        engine->L = NULL;
    }
}

int lua_engine_dispatch(struct lua_engine *engine, struct session *session,
    unsigned int req_id, const char *payload)
{
    lua_State *L;

    if (engine == NULL || engine->L == NULL) {
        return -1;
    }

    L = engine->L;
    engine->active_session = session;
    lua_getglobal(L, "dispatch_request");
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        engine->active_session = NULL;
        return -1;
    }

    lua_pushinteger(L, (lua_Integer) req_id);
    lua_pushstring(L, payload != NULL ? payload : "");

    if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
        diag_count_lua_error(session);
        diag_log(DIAG_LEVEL_ERROR, "lua", session, req_id,
            "dispatch failed err=%s", lua_tostring(L, -1));
        lua_pop(L, 1);
        engine->active_session = NULL;
        return -1;
    }

    engine->active_session = NULL;
    return 0;
}
