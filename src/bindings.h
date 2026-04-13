#ifndef LUAGENT_BINDINGS_H
#define LUAGENT_BINDINGS_H

struct lua_State;
struct lua_engine;

void bindings_register(struct lua_State *L, struct lua_engine *engine);

#endif
