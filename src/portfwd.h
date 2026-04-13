#ifndef LUAGENT_PORTFWD_H
#define LUAGENT_PORTFWD_H

#include <stdint.h>

struct lua_engine;
struct session;
struct lua_State;

void portfwd_engine_init(struct lua_engine *engine);
void portfwd_engine_close(struct lua_engine *engine);
void portfwd_close_session(struct lua_engine *engine, struct session *session);
int portfwd_open(struct lua_engine *engine, struct session *session,
    const char *listen_host, uint16_t listen_port,
    const char *target_host, uint16_t target_port,
    uint32_t *relay_id_out, uint16_t *actual_port_out);
int portfwd_close(struct lua_engine *engine, uint32_t relay_id);
void portfwd_push_list(struct lua_State *L, struct lua_engine *engine,
    struct session *session);

#endif
