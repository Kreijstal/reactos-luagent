#include "protocol.h"

#include "diag.h"
#include "frame.h"
#include "lua_engine.h"
#include "proc.h"
#include "session.h"
#include "transfer.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *protocol_type_name(uint8_t type)
{
    switch (type) {
    case FRAME_HELLO:
        return "HELLO";
    case FRAME_ACK:
        return "ACK";
    case FRAME_ERROR:
        return "ERROR";
    case FRAME_OP:
        return "OP";
    case FRAME_OP_RESULT:
        return "OP_RESULT";
    case FRAME_PUT_BEGIN:
        return "PUT_BEGIN";
    case FRAME_PUT_CHUNK:
        return "PUT_CHUNK";
    case FRAME_PUT_END:
        return "PUT_END";
    case FRAME_GET_BEGIN:
        return "GET_BEGIN";
    case FRAME_GET_CHUNK:
        return "GET_CHUNK";
    case FRAME_GET_END:
        return "GET_END";
    case FRAME_PROC_SPAWN:
        return "PROC_SPAWN";
    case FRAME_STDOUT:
        return "STDOUT";
    case FRAME_STDERR:
        return "STDERR";
    case FRAME_EXIT:
        return "EXIT";
    case FRAME_KILL:
        return "KILL";
    case FRAME_PING:
        return "PING";
    case FRAME_PONG:
        return "PONG";
    default:
        return "UNKNOWN";
    }
}

int protocol_handle_control(struct session *session, struct lua_engine *engine,
    uint8_t type, uint32_t req_id, const char *payload, size_t payload_len)
{
    char *payload_copy;

    switch (type) {
    case FRAME_PUT_CHUNK:
        return transfer_handle_put_chunk(session, req_id, payload, payload_len);
    case FRAME_PUT_END:
        return transfer_handle_put_end(session, req_id);
    default:
        break;
    }

    payload_copy = (char *) calloc(payload_len + 1u, 1u);
    if (payload_copy == NULL) {
        diag_log(DIAG_LEVEL_ERROR, "proto", session, req_id, "payload alloc failed");
        session_send_error(session, req_id, "internal", "out of memory");
        return -1;
    }
    if (payload != NULL && payload_len > 0) {
        memcpy(payload_copy, payload, payload_len);
    }

    switch (type) {
    case FRAME_HELLO:
        free(payload_copy);
        return session_send_text(session, FRAME_ACK, req_id,
            "status=ok\nmessage=hello");
    case FRAME_PING:
        free(payload_copy);
        return session_send_text(session, FRAME_PONG, req_id,
            "status=ok");
    case FRAME_OP:
        if (lua_engine_dispatch(engine, session, req_id, payload_copy) != 0) {
            diag_count_protocol_error(session);
            session_send_error(session, req_id, "internal",
                "lua dispatch failed");
            free(payload_copy);
            return -1;
        }
        free(payload_copy);
        return 0;
    case FRAME_PUT_BEGIN:
        if (transfer_handle_put_begin(session, req_id, payload_copy) != 0) {
            free(payload_copy);
            return -1;
        }
        free(payload_copy);
        return 0;
    case FRAME_GET_BEGIN:
        if (transfer_handle_get_begin(session, req_id, payload_copy) != 0) {
            free(payload_copy);
            return -1;
        }
        free(payload_copy);
        return 0;
    case FRAME_PROC_SPAWN:
        if (proc_handle_spawn(session, req_id, payload_copy) != 0) {
            free(payload_copy);
            return -1;
        }
        free(payload_copy);
        return 0;
    case FRAME_KILL:
        if (proc_handle_kill(session, req_id, payload_copy) != 0) {
            free(payload_copy);
            return -1;
        }
        free(payload_copy);
        return 0;
    default:
        diag_count_protocol_error(session);
        diag_log(DIAG_LEVEL_WARN, "proto", session, req_id,
            "unsupported type=%s", protocol_type_name(type));
        session_send_error(session, req_id, "bad_type", "unsupported frame type");
        free(payload_copy);
        return -1;
    }
}
