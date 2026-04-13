#ifndef LUAGENT_UTIL_H
#define LUAGENT_UTIL_H

#include <stddef.h>
#include <stdint.h>

#define LUAGENT_MAX_FRAME_PAYLOAD 65536u
#define LUAGENT_MAGIC "RXSH"
#define LUAGENT_MAGIC_SIZE 4u

uint64_t util_now_ms(void);
int util_snprintf(char *dst, size_t dst_size, const char *fmt, ...);

#endif
