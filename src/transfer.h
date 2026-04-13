#ifndef LUAGENT_TRANSFER_H
#define LUAGENT_TRANSFER_H

#include <stddef.h>
#include <stdint.h>
#include <uv.h>

struct session;

enum transfer_mode {
    TRANSFER_MODE_NONE = 0,
    TRANSFER_MODE_UPLOAD = 1,
    TRANSFER_MODE_DOWNLOAD = 2
};

struct transfer_state {
    int active;
    int overwrite;
    enum transfer_mode mode;
    uint32_t req_id;
    uv_file file;
    uint64_t total_size;
    uint64_t offset;
    uint64_t last_progress_ms;
    size_t chunk_size;
    char *path;
    char *tmp_path;
};

void transfer_init(struct transfer_state *transfer);
void transfer_reset(struct transfer_state *transfer);
void transfer_abort(struct transfer_state *transfer);
void transfer_check_timeout(struct session *session, struct transfer_state *transfer,
    uint64_t now_ms);
int transfer_handle_put_begin(struct session *session, uint32_t req_id,
    const char *payload);
int transfer_handle_put_chunk(struct session *session, uint32_t req_id,
    const void *payload, size_t payload_len);
int transfer_handle_put_end(struct session *session, uint32_t req_id);
int transfer_handle_get_begin(struct session *session, uint32_t req_id,
    const char *payload);

#endif
