#include "frame.h"
#include "util.h"

#include <string.h>

static uint32_t read_u32le(const unsigned char *src)
{
    return ((uint32_t) src[0]) |
        ((uint32_t) src[1] << 8) |
        ((uint32_t) src[2] << 16) |
        ((uint32_t) src[3] << 24);
}

static void write_u32le(unsigned char *dst, uint32_t value)
{
    dst[0] = (unsigned char) (value & 0xffu);
    dst[1] = (unsigned char) ((value >> 8) & 0xffu);
    dst[2] = (unsigned char) ((value >> 16) & 0xffu);
    dst[3] = (unsigned char) ((value >> 24) & 0xffu);
}

size_t frame_header_size(void)
{
    return 14u;
}

int frame_validate_header(const struct frame_header *header)
{
    if (header == NULL) {
        return -1;
    }

    if (header->length > LUAGENT_MAX_FRAME_PAYLOAD) {
        return -1;
    }

    if (header->type == 0) {
        return -1;
    }

    return 0;
}

int frame_encode_header(unsigned char *dst, size_t dst_size,
    const struct frame_header *header)
{
    if (dst == NULL || header == NULL || dst_size < frame_header_size()) {
        return -1;
    }

    if (frame_validate_header(header) != 0) {
        return -1;
    }

    memcpy(dst, LUAGENT_MAGIC, LUAGENT_MAGIC_SIZE);
    dst[4] = header->type;
    dst[5] = header->flags;
    write_u32le(dst + 6, header->req_id);
    write_u32le(dst + 10, header->length);
    return 0;
}

int frame_decode_header(const unsigned char *src, size_t src_size,
    struct frame_header *header)
{
    if (src == NULL || header == NULL || src_size < frame_header_size()) {
        return FRAME_STATUS_NEED_MORE;
    }

    if (memcmp(src, LUAGENT_MAGIC, LUAGENT_MAGIC_SIZE) != 0) {
        return FRAME_STATUS_ERROR;
    }

    header->type = src[4];
    header->flags = src[5];
    header->req_id = read_u32le(src + 6);
    header->length = read_u32le(src + 10);

    if (frame_validate_header(header) != 0) {
        return FRAME_STATUS_ERROR;
    }

    return FRAME_STATUS_OK;
}
