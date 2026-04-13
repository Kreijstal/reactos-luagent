#ifndef LUAGENT_PROC_H
#define LUAGENT_PROC_H

#include <stdint.h>
#include <uv.h>

struct session;

enum proc_kill_reason {
    PROC_KILL_NONE = 0,
    PROC_KILL_REQUESTED = 1,
    PROC_KILL_TIMEOUT = 2,
    PROC_KILL_IDLE = 3
};

struct proc_state {
    int active;
    int stdout_closed;
    int stderr_closed;
    uint32_t proc_id;
    uint64_t started_ms;
    uint64_t last_io_ms;
    uint64_t timeout_ms;
    uint64_t idle_timeout_ms;
    enum proc_kill_reason kill_reason;
    uv_process_t process;
    uv_process_options_t options;
    uv_stdio_container_t stdio[3];
    uv_pipe_t stdout_pipe;
    uv_pipe_t stderr_pipe;
    char *file;
    char **args;
};

void proc_init(struct proc_state *proc);
void proc_abort(struct proc_state *proc);
int proc_spawn_direct(struct session *session, const char *path,
    const char *const *argv, size_t argc, uint64_t timeout_ms,
    uint64_t idle_timeout_ms, uint32_t *proc_id_out);
int proc_handle_spawn(struct session *session, uint32_t req_id, const char *payload);
int proc_handle_kill(struct session *session, uint32_t req_id, const char *payload);
void proc_check_timeouts(struct session *session, uint64_t now_ms);

#endif
