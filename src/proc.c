#include "proc.h"

#include "diag.h"
#include "frame.h"
#include "session.h"
#include "util.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct proc_read_ctx {
    struct session *session;
    struct proc_state *proc;
    uint8_t frame_type;
};

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

static uint64_t kv_u64_default(const char *payload, const char *key, uint64_t default_value)
{
    char buf[64];
    char *end = NULL;
    unsigned long long value;

    if (kv_copy(buf, sizeof(buf), payload, key) != 0) {
        return default_value;
    }

    errno = 0;
    value = strtoull(buf, &end, 10);
    if (errno != 0 || end == buf || *end != '\0') {
        return default_value;
    }

    return (uint64_t) value;
}

static int kv_u32(uint32_t *out, const char *payload, const char *key)
{
    char buf[32];
    char *end = NULL;
    unsigned long value;

    if (out == NULL || kv_copy(buf, sizeof(buf), payload, key) != 0) {
        return -1;
    }

    errno = 0;
    value = strtoul(buf, &end, 10);
    if (errno != 0 || end == buf || *end != '\0') {
        return -1;
    }

    *out = (uint32_t) value;
    return 0;
}

static const char *kill_reason_name(enum proc_kill_reason reason)
{
    switch (reason) {
    case PROC_KILL_REQUESTED:
        return "killed";
    case PROC_KILL_TIMEOUT:
        return "timeout";
    case PROC_KILL_IDLE:
        return "idle_timeout";
    default:
        return "exit";
    }
}

static void proc_free_args(struct proc_state *proc)
{
    size_t i;

    if (proc->args == NULL) {
        return;
    }

    for (i = 0; proc->args[i] != NULL; ++i) {
        free(proc->args[i]);
    }
    free(proc->args);
    proc->args = NULL;
}

static void proc_reset(struct proc_state *proc)
{
    proc->active = 0;
    proc->stdout_closed = 0;
    proc->stderr_closed = 0;
    proc->proc_id = 0;
    proc->started_ms = 0;
    proc->last_io_ms = 0;
    proc->timeout_ms = 0;
    proc->idle_timeout_ms = 0;
    proc->kill_reason = PROC_KILL_NONE;
    memset(&proc->options, 0, sizeof(proc->options));
    memset(proc->stdio, 0, sizeof(proc->stdio));
    free(proc->file);
    proc->file = NULL;
    proc_free_args(proc);
}

static void on_proc_handle_closed(uv_handle_t *handle)
{
    session_release_ref((struct session *) handle->data);
}

static void proc_maybe_finalize(struct session *session, struct proc_state *proc)
{
    if (!proc->active && proc->stdout_closed && proc->stderr_closed) {
        proc->stdout_pipe.data = session;
        proc->stderr_pipe.data = session;
        uv_close((uv_handle_t *) &proc->stdout_pipe, on_proc_handle_closed);
        uv_close((uv_handle_t *) &proc->stderr_pipe, on_proc_handle_closed);
        proc_reset(proc);
    }
}

static void alloc_cb(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf)
{
    (void) handle;
    buf->base = (char *) malloc(suggested_size > 0 ? suggested_size : 4096u);
    buf->len = suggested_size > 0 ? suggested_size : 4096u;
}

static void read_cb(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf)
{
    struct proc_read_ctx *ctx = (struct proc_read_ctx *) stream->data;

    if (nread > 0) {
        ctx->proc->last_io_ms = util_now_ms();
        session_send_frame(ctx->session, ctx->frame_type, ctx->proc->proc_id,
            buf->base, (size_t) nread);
    } else if (nread < 0) {
        struct proc_state *proc = ctx->proc;
        struct session *session = ctx->session;
        uv_read_stop(stream);
        if (ctx->frame_type == FRAME_STDOUT) {
            proc->stdout_closed = 1;
        } else {
            proc->stderr_closed = 1;
        }
        stream->data = NULL;
        free(ctx);
        proc_maybe_finalize(session, proc);
    }

    free(buf->base);
}

static void proc_exit_cb(uv_process_t *process, int64_t exit_status, int term_signal)
{
    struct session *session = (struct session *) process->data;
    struct proc_state *proc = &session->proc;
    char payload[256];
    const char *reason;

    (void) term_signal;

    proc->active = 0;
    reason = kill_reason_name(proc->kill_reason);
    diag_count_process_exit(session);
    diag_log(DIAG_LEVEL_INFO, "proc", session, proc->proc_id,
        "exit code=%lld reason=%s signal=%d",
        (long long) exit_status, reason, term_signal);
    if (util_snprintf(payload, sizeof(payload),
            "status=ok\nproc_id=%u\nexit_code=%lld\nreason=%s",
            (unsigned int) proc->proc_id, (long long) exit_status, reason) == 0) {
        session_send_text(session, FRAME_EXIT, proc->proc_id, payload);
    }

    proc->process.data = session;
    uv_close((uv_handle_t *) &proc->process, on_proc_handle_closed);
    proc_maybe_finalize(session, proc);
}

void proc_init(struct proc_state *proc)
{
    memset(proc, 0, sizeof(*proc));
}

void proc_abort(struct proc_state *proc)
{
    if (proc == NULL) {
        return;
    }

    if (proc->active) {
        proc->kill_reason = PROC_KILL_REQUESTED;
        uv_process_kill(&proc->process, SIGKILL);
    } else {
        proc_reset(proc);
    }
}

static char **proc_build_args(const char *payload, const char *path)
{
    size_t count = 0;
    size_t i;
    char key[32];
    char value[512];
    char **args;

    for (;;) {
        if (util_snprintf(key, sizeof(key), "argv%u", (unsigned int) count) != 0) {
            return NULL;
        }
        if (kv_copy(value, sizeof(value), payload, key) != 0) {
            break;
        }
        count++;
    }

    if (count == 0) {
        count = 1;
    }

    args = (char **) calloc(count + 1u, sizeof(*args));
    if (args == NULL) {
        return NULL;
    }

    if (count == 1) {
        args[0] = strdup(path);
        return args;
    }

    for (i = 0; i < count; ++i) {
        if (util_snprintf(key, sizeof(key), "argv%u", (unsigned int) i) != 0 ||
                kv_copy(value, sizeof(value), payload, key) != 0) {
            struct proc_state tmp = {0};
            tmp.args = args;
            proc_free_args(&tmp);
            return NULL;
        }
        args[i] = strdup(value);
        if (args[i] == NULL) {
            struct proc_state tmp = {0};
            tmp.args = args;
            proc_free_args(&tmp);
            return NULL;
        }
    }

    return args;
}

static char **proc_dup_args(const char *const *argv, size_t argc)
{
    char **args;
    size_t i;

    if (argc == 0) {
        return NULL;
    }

    args = (char **) calloc(argc + 1u, sizeof(*args));
    if (args == NULL) {
        return NULL;
    }

    for (i = 0; i < argc; ++i) {
        args[i] = strdup(argv[i]);
        if (args[i] == NULL) {
            struct proc_state tmp = {0};
            tmp.args = args;
            proc_free_args(&tmp);
            return NULL;
        }
    }

    return args;
}

static int proc_spawn_common(struct session *session, uint32_t req_id,
    const char *path, char **args, uint64_t timeout_ms, uint64_t idle_timeout_ms,
    int send_ack, uint32_t *proc_id_out)
{
    struct proc_state *proc = &session->proc;
    struct proc_read_ctx *stdout_ctx = NULL;
    struct proc_read_ctx *stderr_ctx = NULL;
    char reply[256];
    int rc;

    if (proc->active) {
        proc_free_args((struct proc_state *) &(struct proc_state){ .args = args });
        session_send_error(session, req_id, "spawn_failed", "process already active");
        return -1;
    }

    proc_reset(proc);
    proc->file = strdup(path);
    proc->args = args;
    if (proc->file == NULL || proc->args == NULL) {
        proc_reset(proc);
        session_send_error(session, req_id, "internal", "out of memory");
        return -1;
    }

    proc->proc_id = session->next_proc_id++;
    proc->started_ms = util_now_ms();
    proc->last_io_ms = proc->started_ms;
    proc->timeout_ms = timeout_ms;
    proc->idle_timeout_ms = idle_timeout_ms;
    proc->kill_reason = PROC_KILL_NONE;
    diag_count_process_spawn(session);
    diag_log(DIAG_LEVEL_INFO, "proc", session, req_id,
        "spawn path=%s proc_id=%u", path, (unsigned int) proc->proc_id);

    rc = uv_pipe_init(session->loop, &proc->stdout_pipe, 0);
    if (rc != 0) {
        proc_reset(proc);
        session_send_error(session, req_id, "spawn_failed", uv_strerror(rc));
        return -1;
    }
    rc = uv_pipe_init(session->loop, &proc->stderr_pipe, 0);
    if (rc != 0) {
        uv_close((uv_handle_t *) &proc->stdout_pipe, NULL);
        proc_reset(proc);
        session_send_error(session, req_id, "spawn_failed", uv_strerror(rc));
        return -1;
    }

    stdout_ctx = (struct proc_read_ctx *) calloc(1, sizeof(*stdout_ctx));
    stderr_ctx = (struct proc_read_ctx *) calloc(1, sizeof(*stderr_ctx));
    if (stdout_ctx == NULL || stderr_ctx == NULL) {
        free(stdout_ctx);
        free(stderr_ctx);
        uv_close((uv_handle_t *) &proc->stdout_pipe, NULL);
        uv_close((uv_handle_t *) &proc->stderr_pipe, NULL);
        proc_reset(proc);
        session_send_error(session, req_id, "internal", "out of memory");
        return -1;
    }

    stdout_ctx->session = session;
    stdout_ctx->proc = proc;
    stdout_ctx->frame_type = FRAME_STDOUT;
    stderr_ctx->session = session;
    stderr_ctx->proc = proc;
    stderr_ctx->frame_type = FRAME_STDERR;
    proc->stdout_pipe.data = stdout_ctx;
    proc->stderr_pipe.data = stderr_ctx;

    memset(proc->stdio, 0, sizeof(proc->stdio));
    proc->stdio[0].flags = UV_IGNORE;
    proc->stdio[1].flags = UV_CREATE_PIPE | UV_WRITABLE_PIPE;
    proc->stdio[1].data.stream = (uv_stream_t *) &proc->stdout_pipe;
    proc->stdio[2].flags = UV_CREATE_PIPE | UV_WRITABLE_PIPE;
    proc->stdio[2].data.stream = (uv_stream_t *) &proc->stderr_pipe;

    memset(&proc->options, 0, sizeof(proc->options));
    proc->options.exit_cb = proc_exit_cb;
    proc->options.file = proc->file;
    proc->options.args = proc->args;
    proc->options.stdio_count = 3;
    proc->options.stdio = proc->stdio;

    proc->process.data = session;
    rc = uv_spawn(session->loop, &proc->process, &proc->options);
    if (rc != 0) {
        free(stdout_ctx);
        free(stderr_ctx);
        proc->stdout_pipe.data = NULL;
        proc->stderr_pipe.data = NULL;
        uv_close((uv_handle_t *) &proc->stdout_pipe, NULL);
        uv_close((uv_handle_t *) &proc->stderr_pipe, NULL);
        proc_reset(proc);
        session_send_error(session, req_id, "spawn_failed", uv_strerror(rc));
        return -1;
    }

    proc->active = 1;
    session_add_ref(session);
    session_add_ref(session);
    session_add_ref(session);
    uv_read_start((uv_stream_t *) &proc->stdout_pipe, alloc_cb, read_cb);
    uv_read_start((uv_stream_t *) &proc->stderr_pipe, alloc_cb, read_cb);

    if (proc_id_out != NULL) {
        *proc_id_out = proc->proc_id;
    }

    if (!send_ack) {
        return 0;
    }

    if (util_snprintf(reply, sizeof(reply),
            "status=ok\nproc_id=%u\npath=%s",
            (unsigned int) proc->proc_id, proc->file) != 0) {
        session_send_error(session, req_id, "internal", "reply overflow");
        return -1;
    }

    return session_send_text(session, FRAME_ACK, req_id, reply);
}

int proc_spawn_direct(struct session *session, const char *path,
    const char *const *argv, size_t argc, uint64_t timeout_ms,
    uint64_t idle_timeout_ms, uint32_t *proc_id_out)
{
    char **args;

    if (session == NULL || path == NULL || argv == NULL || argc == 0) {
        return -1;
    }

    args = proc_dup_args(argv, argc);
    if (args == NULL) {
        return -1;
    }

    return proc_spawn_common(session, 0, path, args, timeout_ms, idle_timeout_ms,
        0, proc_id_out);
}

int proc_handle_spawn(struct session *session, uint32_t req_id, const char *payload)
{
    char path[512];
    char **args;
    int rc;

    if (kv_copy(path, sizeof(path), payload, "path") != 0) {
        session_send_error(session, req_id, "bad_payload", "missing path");
        return -1;
    }

    args = proc_build_args(payload, path);
    if (args == NULL) {
        session_send_error(session, req_id, "internal", "out of memory");
        return -1;
    }

    rc = proc_spawn_common(session, req_id, path, args,
        kv_u64_default(payload, "timeout_ms", 60000u),
        kv_u64_default(payload, "idle_timeout_ms", 10000u),
        1, NULL);
    return rc;
}

int proc_handle_kill(struct session *session, uint32_t req_id, const char *payload)
{
    struct proc_state *proc = &session->proc;
    uint32_t proc_id = 0;
    char reply[128];

    if (kv_u32(&proc_id, payload, "proc_id") != 0) {
        session_send_error(session, req_id, "bad_payload", "missing proc_id");
        return -1;
    }

    if (!proc->active || proc->proc_id != proc_id) {
        session_send_error(session, req_id, "not_found", "process not active");
        return -1;
    }

    proc->kill_reason = PROC_KILL_REQUESTED;
    if (uv_process_kill(&proc->process, SIGKILL) != 0) {
        session_send_error(session, req_id, "io_error", "kill failed");
        return -1;
    }
    diag_log(DIAG_LEVEL_INFO, "proc", session, req_id,
        "kill requested proc_id=%u", (unsigned int) proc_id);

    if (util_snprintf(reply, sizeof(reply), "status=ok\nproc_id=%u",
            (unsigned int) proc_id) != 0) {
        session_send_error(session, req_id, "internal", "reply overflow");
        return -1;
    }
    return session_send_text(session, FRAME_ACK, req_id, reply);
}

void proc_check_timeouts(struct session *session, uint64_t now_ms)
{
    struct proc_state *proc = &session->proc;

    if (!proc->active) {
        return;
    }

    if (proc->timeout_ms > 0 &&
            now_ms - proc->started_ms >= proc->timeout_ms) {
        proc->kill_reason = PROC_KILL_TIMEOUT;
        diag_count_process_timeout(session);
        diag_log(DIAG_LEVEL_WARN, "proc", session, proc->proc_id,
            "timeout elapsed_ms=%llu threshold_ms=%llu action=kill",
            (unsigned long long) (now_ms - proc->started_ms),
            (unsigned long long) proc->timeout_ms);
        uv_process_kill(&proc->process, SIGKILL);
        return;
    }

    if (proc->idle_timeout_ms > 0 &&
            now_ms - proc->last_io_ms >= proc->idle_timeout_ms) {
        proc->kill_reason = PROC_KILL_IDLE;
        diag_count_process_timeout(session);
        diag_log(DIAG_LEVEL_WARN, "proc", session, proc->proc_id,
            "idle_timeout elapsed_ms=%llu threshold_ms=%llu action=kill",
            (unsigned long long) (now_ms - proc->last_io_ms),
            (unsigned long long) proc->idle_timeout_ms);
        uv_process_kill(&proc->process, SIGKILL);
    }
}
