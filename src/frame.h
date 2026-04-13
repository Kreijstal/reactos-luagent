#ifndef LUAGENT_FRAME_H
#define LUAGENT_FRAME_H

#include <stddef.h>
#include <stdint.h>

enum frame_type {
    FRAME_HELLO = 1,
    FRAME_ACK = 2,
    FRAME_ERROR = 3,
    FRAME_OP = 10,
    FRAME_OP_RESULT = 11,
    FRAME_PUT_BEGIN = 20,
    FRAME_PUT_CHUNK = 21,
    FRAME_PUT_END = 22,
    FRAME_GET_BEGIN = 30,
    FRAME_GET_CHUNK = 31,
    FRAME_GET_END = 32,
    FRAME_PROC_SPAWN = 40,
    FRAME_STDIN = 41,
    FRAME_STDOUT = 42,
    FRAME_STDERR = 43,
    FRAME_EXIT = 44,
    FRAME_KILL = 45,
    FRAME_PING = 50,
    FRAME_PONG = 51
};

struct frame_header {
    uint8_t type;
    uint8_t flags;
    uint32_t req_id;
    uint32_t length;
};

enum frame_status {
    FRAME_STATUS_NEED_MORE = 0,
    FRAME_STATUS_OK = 1,
    FRAME_STATUS_ERROR = -1
};

size_t frame_header_size(void);
int frame_encode_header(unsigned char *dst, size_t dst_size,
    const struct frame_header *header);
int frame_decode_header(const unsigned char *src, size_t src_size,
    struct frame_header *header);
int frame_validate_header(const struct frame_header *header);

#endif
