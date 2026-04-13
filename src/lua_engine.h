#ifndef LUAGENT_LUA_ENGINE_H
#define LUAGENT_LUA_ENGINE_H

#include <stdint.h>

typedef struct lua_State lua_State;
struct session;
struct portfwd_relay;

struct lua_engine {
    lua_State *L;
    struct session *active_session;
    struct portfwd_relay *relays;
    uint32_t next_relay_id;
};

int lua_engine_init(struct lua_engine *engine);
void lua_engine_close(struct lua_engine *engine);
int lua_engine_dispatch(struct lua_engine *engine, struct session *session,
    unsigned int req_id, const char *payload);

#endif
