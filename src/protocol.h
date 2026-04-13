#ifndef LUAGENT_PROTOCOL_H
#define LUAGENT_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

struct lua_engine;
struct session;

int protocol_handle_control(struct session *session, struct lua_engine *engine,
    uint8_t type, uint32_t req_id, const char *payload, size_t payload_len);
const char *protocol_type_name(uint8_t type);

#endif
