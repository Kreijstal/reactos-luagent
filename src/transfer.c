#include "transfer.h"

#include "diag.h"
#include "frame.h"
#include "session.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *kv_value(const char *payload, const char *key)
{
    size_t key_len;
    const char *line;

    if (payload == NULL || key == NULL) {
        return NULL;
    }

    key_len = strlen(key);
    line = payload;
    while (*line != '\0') {
        const char *line_end = strchr(line, '\n');
        size_t line_len = line_end != NULL ? (size_t) (line_end - line) : strlen(line);

        if (line_len > key_len + 1 &&
                memcmp(line, key, key_len) == 0 &&
                line[key_len] == '=') {
            return line + key_len + 1;
        }

        if (line_end == NULL) {
            break;
        }
        line = line_end + 1;
    }

    return NULL;
}

static int kv_copy(char *dst, size_t dst_size, const char *payload, const char *key)
{
    const char *value = kv_value(payload, key);
    size_t len;

    if (value == NULL) {
        return -1;
    }

    len = strcspn(value, "\r\n");
    if (len + 1 > dst_size) {
        return -1;
    }

    memcpy(dst, value, len);
    dst[len] = '\0';
    return 0;
}

static int kv_u64(uint64_t *out, const char *payload, const char *key)
{
    char buf[64];
    char *end = NULL;
    unsigned long long value;

    if (out == NULL || kv_copy(buf, sizeof(buf), payload, key) != 0) {
        return -1;
    }

    errno = 0;
    value = strtoull(buf, &end, 10);
    if (errno != 0 || end == buf || *end != '\0') {
        return -1;
    }

    *out = (uint64_t) value;
    return 0;
}

static uint64_t kv_u64_default(const char *payload, const char *key, uint64_t default_value)
{
    uint64_t out = 0;
    if (kv_u64(&out, payload, key) != 0) {
        return default_value;
    }
    return out;
}

static int kv_bool_default(const char *payload, const char *key, int default_value)
{
    char buf[16];

    if (kv_copy(buf, sizeof(buf), payload, key) != 0) {
        return default_value;
    }

    return strcmp(buf, "1") == 0 ? 1 : 0;
}

static int transfer_finalize_rename(struct transfer_state *transfer)
{
    uv_fs_t req;
    int rc;

    if (transfer->overwrite) {
        uv_fs_unlink(NULL, &req, transfer->path, NULL);
        uv_fs_req_cleanup(&req);
    }

    rc = uv_fs_rename(NULL, &req, transfer->tmp_path, transfer->path, NULL);
    uv_fs_req_cleanup(&req);
    return rc;
}

void transfer_init(struct transfer_state *transfer)
{
    memset(transfer, 0, sizeof(*transfer));
    transfer->file = -1;
    transfer->chunk_size = LUAGENT_READ_CHUNK;
}

void transfer_reset(struct transfer_state *transfer)
{
    if (transfer == NULL) {
        return;
    }

    transfer->active = 0;
    transfer->overwrite = 0;
    transfer->mode = TRANSFER_MODE_NONE;
    transfer->req_id = 0;
    transfer->total_size = 0;
    transfer->offset = 0;
    transfer->last_progress_ms = 0;
    transfer->file = -1;
    transfer->chunk_size = LUAGENT_READ_CHUNK;

    free(transfer->path);
    transfer->path = NULL;
    free(transfer->tmp_path);
    transfer->tmp_path = NULL;
}

void transfer_abort(struct transfer_state *transfer)
{
    uv_fs_t req;

    if (transfer == NULL) {
        return;
    }

    if (transfer->file >= 0) {
        uv_fs_close(NULL, &req, transfer->file, NULL);
        uv_fs_req_cleanup(&req);
    }

    if (transfer->tmp_path != NULL) {
        uv_fs_unlink(NULL, &req, transfer->tmp_path, NULL);
        uv_fs_req_cleanup(&req);
    }

    transfer_reset(transfer);
}

void transfer_check_timeout(struct session *session, struct transfer_state *transfer,
    uint64_t now_ms)
{
    if (transfer == NULL || !transfer->active || transfer->last_progress_ms == 0) {
        return;
    }

    if (now_ms - transfer->last_progress_ms >= LUAGENT_TRANSFER_IDLE_MS) {
        diag_count_transfer_abort(session);
        diag_log(DIAG_LEVEL_WARN, "xfer", session, transfer->req_id,
            "idle_timeout elapsed_ms=%llu threshold_ms=%u action=abort",
            (unsigned long long) (now_ms - transfer->last_progress_ms),
            (unsigned int) LUAGENT_TRANSFER_IDLE_MS);
        session_send_error(session, transfer->req_id, "timeout", "transfer stalled");
        transfer_abort(transfer);
    }
}

int transfer_handle_put_begin(struct session *session, uint32_t req_id,
    const char *payload)
{
    struct transfer_state *transfer = &session->upload;
    uv_fs_t req;
    char path[512];
    char tmp_path[576];
    uint64_t size = 0;
    int rc;

    if (transfer->active) {
        session_send_error(session, req_id, "io_error", "transfer already active");
        return -1;
    }

    if (kv_copy(path, sizeof(path), payload, "path") != 0 ||
            kv_u64(&size, payload, "size") != 0) {
        session_send_error(session, req_id, "bad_payload", "missing path or size");
        return -1;
    }

    if (util_snprintf(tmp_path, sizeof(tmp_path), "%s.part", path) != 0) {
        session_send_error(session, req_id, "bad_payload", "path too long");
        return -1;
    }

    transfer->path = strdup(path);
    transfer->tmp_path = strdup(tmp_path);
    if (transfer->path == NULL || transfer->tmp_path == NULL) {
        transfer_abort(transfer);
        session_send_error(session, req_id, "internal", "out of memory");
        return -1;
    }

    transfer->overwrite = kv_bool_default(payload, "overwrite", 0);
    transfer->mode = TRANSFER_MODE_UPLOAD;
    transfer->req_id = req_id;
    transfer->total_size = size;
    transfer->offset = 0;
    transfer->last_progress_ms = util_now_ms();
    transfer->chunk_size = (size_t) kv_u64_default(payload, "chunk_size", LUAGENT_READ_CHUNK);
    diag_count_transfer_start(session);
    diag_log(DIAG_LEVEL_INFO, "xfer", session, req_id,
        "upload begin path=%s size=%llu", transfer->path,
        (unsigned long long) transfer->total_size);

    rc = uv_fs_open(NULL, &req, transfer->tmp_path,
        O_CREAT | O_TRUNC | O_WRONLY, 0666, NULL);
    if (rc < 0) {
        uv_fs_req_cleanup(&req);
        transfer_abort(transfer);
        session_send_error(session, req_id, "io_error", uv_strerror(rc));
        return -1;
    }

    transfer->file = rc;
    transfer->active = 1;
    uv_fs_req_cleanup(&req);

    {
        char reply[256];
        if (util_snprintf(reply, sizeof(reply),
                "status=ok\ntransfer_id=%u\npath=%s\nsize=%llu",
                (unsigned int) req_id, transfer->path,
                (unsigned long long) transfer->total_size) != 0) {
            session_send_error(session, req_id, "internal", "reply overflow");
            transfer_abort(transfer);
            return -1;
        }
        return session_send_text(session, FRAME_ACK, req_id, reply);
    }
}

int transfer_handle_put_chunk(struct session *session, uint32_t req_id,
    const void *payload, size_t payload_len)
{
    struct transfer_state *transfer = &session->upload;
    uv_fs_t req;
    uv_buf_t buf;
    int rc;

    if (!transfer->active || transfer->req_id != req_id) {
        session_send_error(session, req_id, "bad_payload", "no active transfer");
        return -1;
    }

    if (transfer->mode != TRANSFER_MODE_UPLOAD) {
        session_send_error(session, req_id, "bad_payload", "wrong transfer mode");
        return -1;
    }

    if (transfer->offset + payload_len > transfer->total_size) {
        session_send_error(session, req_id, "bad_payload", "chunk exceeds declared size");
        diag_count_transfer_abort(session);
        transfer_abort(transfer);
        return -1;
    }

    buf = uv_buf_init((char *) payload, (unsigned int) payload_len);
    rc = uv_fs_write(NULL, &req, transfer->file, &buf, 1,
        (int64_t) transfer->offset, NULL);
    uv_fs_req_cleanup(&req);
    if (rc < 0 || (size_t) rc != payload_len) {
        session_send_error(session, req_id, "io_error", "write failed");
        diag_count_transfer_abort(session);
        transfer_abort(transfer);
        return -1;
    }

    transfer->offset += payload_len;
    transfer->last_progress_ms = util_now_ms();
    return 0;
}

int transfer_handle_put_end(struct session *session, uint32_t req_id)
{
    struct transfer_state *transfer = &session->upload;
    uv_fs_t req;
    int rc;
    char reply[256];

    if (!transfer->active || transfer->req_id != req_id) {
        session_send_error(session, req_id, "bad_payload", "no active transfer");
        return -1;
    }

    if (transfer->mode != TRANSFER_MODE_UPLOAD) {
        session_send_error(session, req_id, "bad_payload", "wrong transfer mode");
        return -1;
    }

    if (transfer->offset != transfer->total_size) {
        session_send_error(session, req_id, "bad_payload", "size mismatch");
        transfer_abort(transfer);
        return -1;
    }

    rc = uv_fs_close(NULL, &req, transfer->file, NULL);
    uv_fs_req_cleanup(&req);
    transfer->file = -1;
    if (rc < 0) {
        session_send_error(session, req_id, "io_error", uv_strerror(rc));
        transfer_abort(transfer);
        return -1;
    }

    rc = transfer_finalize_rename(transfer);
    if (rc < 0) {
        session_send_error(session, req_id, "io_error", uv_strerror(rc));
        transfer_abort(transfer);
        return -1;
    }

    if (util_snprintf(reply, sizeof(reply),
            "status=ok\npath=%s\nbytes=%llu",
            transfer->path,
            (unsigned long long) transfer->offset) != 0) {
        session_send_error(session, req_id, "internal", "reply overflow");
        transfer_abort(transfer);
        return -1;
    }

    session_send_text(session, FRAME_ACK, req_id, reply);
    diag_count_transfer_complete(session);
    diag_log(DIAG_LEVEL_INFO, "xfer", session, req_id,
        "upload complete path=%s bytes=%llu", transfer->path,
        (unsigned long long) transfer->offset);
    transfer_reset(transfer);
    return 0;
}

int transfer_handle_get_begin(struct session *session, uint32_t req_id,
    const char *payload)
{
    struct transfer_state *transfer = &session->download;
    uv_fs_t req;
    char path[512];
    char reply[256];
    int rc;
    uv_stat_t *statbuf;
    unsigned char *chunk = NULL;

    if (transfer->active) {
        session_send_error(session, req_id, "io_error", "download already active");
        return -1;
    }

    if (kv_copy(path, sizeof(path), payload, "path") != 0) {
        session_send_error(session, req_id, "bad_payload", "missing path");
        return -1;
    }

    transfer->path = strdup(path);
    if (transfer->path == NULL) {
        session_send_error(session, req_id, "internal", "out of memory");
        return -1;
    }

    transfer->chunk_size = (size_t) kv_u64_default(payload, "chunk_size", LUAGENT_READ_CHUNK);
    if (transfer->chunk_size == 0 || transfer->chunk_size > LUAGENT_MAX_FRAME_PAYLOAD) {
        transfer->chunk_size = LUAGENT_READ_CHUNK;
    }

    rc = uv_fs_open(NULL, &req, path, O_RDONLY, 0, NULL);
    if (rc < 0) {
        uv_fs_req_cleanup(&req);
        transfer_abort(transfer);
        session_send_error(session, req_id, "not_found", uv_strerror(rc));
        return -1;
    }
    transfer->file = rc;
    uv_fs_req_cleanup(&req);

    rc = uv_fs_fstat(NULL, &req, transfer->file, NULL);
    if (rc < 0) {
        uv_fs_req_cleanup(&req);
        transfer_abort(transfer);
        session_send_error(session, req_id, "io_error", uv_strerror(rc));
        return -1;
    }
    statbuf = &req.statbuf;
    transfer->active = 1;
    transfer->mode = TRANSFER_MODE_DOWNLOAD;
    transfer->req_id = req_id;
    transfer->total_size = (uint64_t) statbuf->st_size;
    transfer->offset = 0;
    transfer->last_progress_ms = util_now_ms();
    diag_count_transfer_start(session);
    diag_log(DIAG_LEVEL_INFO, "xfer", session, req_id,
        "download begin path=%s size=%llu", transfer->path,
        (unsigned long long) transfer->total_size);
    uv_fs_req_cleanup(&req);

    if (util_snprintf(reply, sizeof(reply),
            "status=ok\ntransfer_id=%u\npath=%s\nsize=%llu",
            (unsigned int) req_id, transfer->path,
            (unsigned long long) transfer->total_size) != 0) {
        transfer_abort(transfer);
        session_send_error(session, req_id, "internal", "reply overflow");
        return -1;
    }

    if (session_send_text(session, FRAME_ACK, req_id, reply) != 0) {
        transfer_abort(transfer);
        return -1;
    }

    chunk = (unsigned char *) malloc(transfer->chunk_size);
    if (chunk == NULL) {
        transfer_abort(transfer);
        session_send_error(session, req_id, "internal", "out of memory");
        return -1;
    }

    while (transfer->offset < transfer->total_size) {
        uv_buf_t buf;
        size_t to_read = transfer->chunk_size;

        if (transfer->total_size - transfer->offset < to_read) {
            to_read = (size_t) (transfer->total_size - transfer->offset);
        }

        buf = uv_buf_init((char *) chunk, (unsigned int) to_read);
        rc = uv_fs_read(NULL, &req, transfer->file, &buf, 1,
            (int64_t) transfer->offset, NULL);
        uv_fs_req_cleanup(&req);
        if (rc <= 0) {
            free(chunk);
            diag_count_transfer_abort(session);
            transfer_abort(transfer);
            session_send_error(session, req_id, "io_error", "read failed");
            return -1;
        }

        if (session_send_frame(session, FRAME_GET_CHUNK, req_id, chunk, (size_t) rc) != 0) {
            free(chunk);
            diag_count_transfer_abort(session);
            transfer_abort(transfer);
            return -1;
        }

        transfer->offset += (uint64_t) rc;
        transfer->last_progress_ms = util_now_ms();
    }

    free(chunk);
    uv_fs_close(NULL, &req, transfer->file, NULL);
    uv_fs_req_cleanup(&req);
    transfer->file = -1;

    session_send_text(session, FRAME_GET_END, req_id, "status=ok");
    diag_count_transfer_complete(session);
    diag_log(DIAG_LEVEL_INFO, "xfer", session, req_id,
        "download complete path=%s bytes=%llu", transfer->path,
        (unsigned long long) transfer->offset);
    transfer_reset(transfer);
    return 0;
}
