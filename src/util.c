#include "util.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <uv.h>

uint64_t util_now_ms(void)
{
    return uv_hrtime() / 1000000u;
}

int util_snprintf(char *dst, size_t dst_size, const char *fmt, ...)
{
    int rc;
    va_list ap;

    if (dst == NULL || dst_size == 0 || fmt == NULL) {
        return -1;
    }

    va_start(ap, fmt);
    rc = vsnprintf(dst, dst_size, fmt, ap);
    va_end(ap);

    if (rc < 0) {
        dst[0] = '\0';
        return -1;
    }

    if ((size_t) rc >= dst_size) {
        dst[dst_size - 1] = '\0';
        return -1;
    }

    return 0;
}
