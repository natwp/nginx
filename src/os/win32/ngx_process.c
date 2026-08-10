
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_win32_worker.h>


int              ngx_argc;
char           **ngx_argv;
char           **ngx_os_argv;

ngx_int_t        ngx_last_process;
ngx_process_t    ngx_processes[NGX_MAX_PROCESSES];
HANDLE           ngx_process_exit_event;
static HANDLE    ngx_win32_worker_job;


static char *ngx_win32_create_worker_environment(ngx_cycle_t *cycle,
    ngx_win32_worker_bootstrap_t *bootstrap);
static ngx_uint_t ngx_win32_worker_environment_variable(u_char *entry,
    size_t len);
static VOID CALLBACK ngx_win32_process_wait_handler(PVOID data,
    BOOLEAN timed_out);


ngx_pid_t
ngx_spawn_process(ngx_cycle_t *cycle, char *name, ngx_int_t respawn)
{
    ngx_uint_t  generation, slot;

    if (respawn >= 0) {
        slot = ngx_processes[respawn].slot;
        generation = ngx_processes[respawn].generation;

    } else {
        slot = 0;
        generation = 1;
    }

    return ngx_spawn_worker(cycle, name, respawn, slot, generation,
                            NGX_WIN32_WORKER_ROLE);
}


ngx_pid_t
ngx_spawn_worker(ngx_cycle_t *cycle, char *name, ngx_int_t respawn,
    ngx_uint_t slot, ngx_uint_t generation, ngx_uint_t role)
{
    u_long          rc, n, code;
    ngx_int_t       s;
    ngx_pid_t       pid;
    ngx_exec_ctx_t  ctx;
    HANDLE          events[2];
    char            file[MAX_PATH + 1];

    if (respawn >= 0) {
        s = respawn;

    } else {
        for (s = 0; s < ngx_last_process; s++) {
            if (ngx_processes[s].handle == NULL) {
                break;
            }
        }

        if (s == NGX_MAX_PROCESSES) {
            ngx_log_error(NGX_LOG_ALERT, cycle->log, 0,
                          "no more than %d processes can be spawned",
                          NGX_MAX_PROCESSES);
            return NGX_INVALID_PID;
        }
    }

    ngx_memzero(&ngx_processes[s], sizeof(ngx_process_t));

    n = GetModuleFileName(NULL, file, MAX_PATH);

    if (n == 0) {
        ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                      "GetModuleFileName() failed");
        return NGX_INVALID_PID;
    }

    file[n] = '\0';

    ngx_log_debug1(NGX_LOG_DEBUG_CORE, cycle->log, 0,
                   "GetModuleFileName: \"%s\"", file);

    ngx_memzero(&ctx, sizeof(ngx_exec_ctx_t));

    ngx_processes[s].bootstrap = ngx_alloc(
                                      sizeof(ngx_win32_worker_bootstrap_t),
                                      cycle->log);
    if (ngx_processes[s].bootstrap == NULL) {
        return NGX_INVALID_PID;
    }

    if (ngx_win32_master_create_bootstrap(cycle,
                                          ngx_processes[s].bootstrap,
                                          slot, generation, role)
        != NGX_OK)
    {
        ngx_free(ngx_processes[s].bootstrap);
        ngx_processes[s].bootstrap = NULL;
        return NGX_INVALID_PID;
    }

    ctx.path = file;
    ctx.name = name;
    ctx.args = GetCommandLine();
    ctx.argv = NULL;
    ctx.envp = NULL;
    ctx.environment = ngx_win32_create_worker_environment(
                                          cycle,
                                          ngx_processes[s].bootstrap);

    if (ctx.environment == NULL) {
        ngx_win32_master_close_bootstrap(ngx_processes[s].bootstrap);
        ngx_free(ngx_processes[s].bootstrap);
        ngx_processes[s].bootstrap = NULL;
        return NGX_INVALID_PID;
    }

    pid = ngx_execute(cycle, &ctx);

    ngx_free(ctx.environment);

    if (pid == NGX_INVALID_PID) {
        ngx_win32_master_close_bootstrap(ngx_processes[s].bootstrap);
        ngx_free(ngx_processes[s].bootstrap);
        ngx_processes[s].bootstrap = NULL;
        return pid;
    }

    ngx_processes[s].handle = ctx.child;
    ngx_processes[s].pid = pid;
    ngx_processes[s].name = name;
    ngx_processes[s].slot = slot;
    ngx_processes[s].generation = generation;
    ngx_processes[s].role = role;

    if (ngx_win32_master_bootstrap_worker(cycle,
                                          ngx_processes[s].bootstrap,
                                          pid, ctx.child)
        != NGX_OK)
    {
        goto failed;
    }

    ngx_sprintf(ngx_processes[s].term_event, "ngx_%s_term_%P%Z", name, pid);
    ngx_sprintf(ngx_processes[s].quit_event, "ngx_%s_quit_%P%Z", name, pid);
    ngx_sprintf(ngx_processes[s].reopen_event, "ngx_%s_reopen_%P%Z",
                name, pid);

    events[0] = ngx_master_process_event;
    events[1] = ctx.child;

    rc = WaitForMultipleObjects(2, events, 0, 5000);

    ngx_time_update();

    ngx_log_debug1(NGX_LOG_DEBUG_CORE, cycle->log, 0,
                   "WaitForMultipleObjects: %ul", rc);

    switch (rc) {

    case WAIT_OBJECT_0:

        ngx_processes[s].term = OpenEvent(EVENT_MODIFY_STATE, 0,
                                          (char *) ngx_processes[s].term_event);
        if (ngx_processes[s].term == NULL) {
            ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                          "OpenEvent(\"%s\") failed",
                          ngx_processes[s].term_event);
            goto failed;
        }

        ngx_processes[s].quit = OpenEvent(EVENT_MODIFY_STATE, 0,
                                          (char *) ngx_processes[s].quit_event);
        if (ngx_processes[s].quit == NULL) {
            ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                          "OpenEvent(\"%s\") failed",
                          ngx_processes[s].quit_event);
            goto failed;
        }

        ngx_processes[s].reopen = OpenEvent(EVENT_MODIFY_STATE, 0,
                                       (char *) ngx_processes[s].reopen_event);
        if (ngx_processes[s].reopen == NULL) {
            ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                          "OpenEvent(\"%s\") failed",
                          ngx_processes[s].reopen_event);
            goto failed;
        }

        if (ResetEvent(ngx_master_process_event) == 0) {
            ngx_log_error(NGX_LOG_ALERT, cycle->log, 0,
                          "ResetEvent(\"%s\") failed",
                          ngx_master_process_event_name);
            goto failed;
        }

        break;

    case WAIT_OBJECT_0 + 1:
        if (GetExitCodeProcess(ctx.child, &code) == 0) {
            ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                          "GetExitCodeProcess(%P) failed", pid);
        }

        ngx_log_error(NGX_LOG_ALERT, cycle->log, 0,
                      "%s process %P exited with code %Xl",
                      name, pid, code);

        goto failed;

    case WAIT_TIMEOUT:
        ngx_log_error(NGX_LOG_ALERT, cycle->log, 0,
                      "the event \"%s\" was not signaled for 5s",
                      ngx_master_process_event_name);
        goto failed;

    case WAIT_FAILED:
        ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                      "WaitForSingleObject(\"%s\") failed",
                      ngx_master_process_event_name);

        goto failed;
    }

    if (ngx_win32_master_wait_status(cycle, ngx_processes[s].bootstrap,
                                     NGX_WIN32_BOOTSTRAP_CONTROL_READY,
                                     ctx.child)
        != NGX_OK
        || ngx_win32_master_wait_status(cycle,
                                        ngx_processes[s].bootstrap,
                                        NGX_WIN32_BOOTSTRAP_READY,
                                        ctx.child)
           != NGX_OK)
    {
        goto failed;
    }

    ngx_processes[s].ready_state = 1;

    if (RegisterWaitForSingleObject(&ngx_processes[s].wait, ctx.child,
                                    ngx_win32_process_wait_handler, NULL,
                                    INFINITE, WT_EXECUTEONLYONCE)
        == 0)
    {
        ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                      "RegisterWaitForSingleObject(%P) failed", pid);
        goto failed;
    }

    if (!ngx_processes[s].bootstrap->routed) {
        ngx_win32_master_close_bootstrap(ngx_processes[s].bootstrap);
        ngx_free(ngx_processes[s].bootstrap);
        ngx_processes[s].bootstrap = NULL;
    }

    if (respawn >= 0) {
        return pid;
    }

    switch (respawn) {

    case NGX_PROCESS_RESPAWN:
        ngx_processes[s].just_spawn = 0;
        break;

    case NGX_PROCESS_JUST_RESPAWN:
        ngx_processes[s].just_spawn = 1;
        break;
    }

    if (s == ngx_last_process) {
        ngx_last_process++;
    }

    return pid;

failed:

    if (ngx_processes[s].wait) {
        (void) UnregisterWaitEx(ngx_processes[s].wait,
                                INVALID_HANDLE_VALUE);
        ngx_processes[s].wait = NULL;
    }

    if (ngx_processes[s].bootstrap) {
        ngx_win32_master_close_bootstrap(ngx_processes[s].bootstrap);
        ngx_free(ngx_processes[s].bootstrap);
        ngx_processes[s].bootstrap = NULL;
    }

    if (ngx_processes[s].reopen) {
        ngx_close_handle(ngx_processes[s].reopen);
    }

    if (ngx_processes[s].quit) {
        ngx_close_handle(ngx_processes[s].quit);
    }

    if (ngx_processes[s].term) {
        ngx_close_handle(ngx_processes[s].term);
    }

    TerminateProcess(ngx_processes[s].handle, 2);

    if (ngx_processes[s].handle) {
        ngx_close_handle(ngx_processes[s].handle);
        ngx_processes[s].handle = NULL;
    }

    return NGX_INVALID_PID;
}


ngx_pid_t
ngx_execute(ngx_cycle_t *cycle, ngx_exec_ctx_t *ctx)
{
    STARTUPINFO          si;
    PROCESS_INFORMATION  pi;
    u_long               flags;

    ngx_memzero(&si, sizeof(STARTUPINFO));
    si.cb = sizeof(STARTUPINFO);

    ngx_memzero(&pi, sizeof(PROCESS_INFORMATION));

    flags = CREATE_NO_WINDOW | CREATE_SUSPENDED;

    if (CreateProcess(ctx->path, ctx->args,
                      NULL, NULL, 0, flags, ctx->environment,
                      NULL, &si, &pi)
        == 0)
    {
        ngx_log_error(NGX_LOG_CRIT, cycle->log, ngx_errno,
                      "CreateProcess(\"%s\") failed", ngx_argv[0]);

        return 0;
    }

    if (ngx_win32_worker_job
        && AssignProcessToJobObject(ngx_win32_worker_job, pi.hProcess) == 0)
    {
        ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                      "AssignProcessToJobObject() failed");
        (void) TerminateProcess(pi.hProcess, 2);
        ngx_close_handle(pi.hThread);
        ngx_close_handle(pi.hProcess);
        return NGX_INVALID_PID;
    }

    if (ResumeThread(pi.hThread) == (DWORD) -1) {
        ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                      "ResumeThread() failed");
        (void) TerminateProcess(pi.hProcess, 2);
        ngx_close_handle(pi.hThread);
        ngx_close_handle(pi.hProcess);
        return NGX_INVALID_PID;
    }

    ctx->child = pi.hProcess;

    if (CloseHandle(pi.hThread) == 0) {
        ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                      "CloseHandle(pi.hThread) failed");
    }

    ngx_log_error(NGX_LOG_NOTICE, cycle->log, 0,
                  "start %s process %P", ctx->name, pi.dwProcessId);

    return pi.dwProcessId;
}


ngx_int_t
ngx_win32_job_init(ngx_cycle_t *cycle)
{
    BOOL                                  in_job;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info;

    if (ngx_win32_worker_job) {
        return NGX_OK;
    }

    in_job = FALSE;

    if (IsProcessInJob(GetCurrentProcess(), NULL, &in_job) == 0) {
        ngx_log_error(NGX_LOG_WARN, cycle->log, ngx_errno,
                      "IsProcessInJob() failed, worker crash cleanup "
                      "is unavailable");
        return NGX_DECLINED;
    }

    if (in_job) {
        ngx_log_error(NGX_LOG_NOTICE, cycle->log, 0,
                      "master process is already contained in a Job Object");
        return NGX_DECLINED;
    }

    ngx_win32_worker_job = CreateJobObject(NULL, NULL);
    if (ngx_win32_worker_job == NULL) {
        ngx_log_error(NGX_LOG_WARN, cycle->log, ngx_errno,
                      "CreateJobObject() failed, worker crash cleanup "
                      "is unavailable");
        return NGX_DECLINED;
    }

    ngx_memzero(&info, sizeof(JOBOBJECT_EXTENDED_LIMIT_INFORMATION));
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;

    if (SetInformationJobObject(ngx_win32_worker_job,
                                JobObjectExtendedLimitInformation, &info,
                                sizeof(info)) == 0)
    {
        ngx_log_error(NGX_LOG_WARN, cycle->log, ngx_errno,
                      "SetInformationJobObject() failed, worker crash "
                      "cleanup is unavailable");
        ngx_close_handle(ngx_win32_worker_job);
        ngx_win32_worker_job = NULL;
        return NGX_DECLINED;
    }

    return NGX_OK;
}


void
ngx_win32_job_done(void)
{
    if (ngx_win32_worker_job) {
        ngx_close_handle(ngx_win32_worker_job);
        ngx_win32_worker_job = NULL;
    }
}


static char *
ngx_win32_create_worker_environment(ngx_cycle_t *cycle,
    ngx_win32_worker_bootstrap_t *bootstrap)
{
    size_t  size, extra, len;
    char   *base, *env;
    u_char *p;
    char    slot[NGX_INT_T_LEN + 1];
    char    generation[NGX_INT_T_LEN + 1];
    char    role[NGX_INT_T_LEN + 1];

    base = GetEnvironmentStrings();
    if (base == NULL) {
        ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_errno,
                      "GetEnvironmentStrings() failed");
        return NULL;
    }

    size = 0;
    p = (u_char *) base;

    while (*p) {
        len = ngx_strlen(p) + 1;

        if (!ngx_win32_worker_environment_variable(p, len - 1)) {
            size += len;
        }

        p += len;
    }

    (void) ngx_sprintf((u_char *) slot, "%ui%Z", bootstrap->slot);
    (void) ngx_sprintf((u_char *) generation, "%ui%Z",
                       bootstrap->generation);
    (void) ngx_sprintf((u_char *) role, "%ui%Z", bootstrap->role);

    extra = sizeof(NGX_WIN32_WORKER_SLOT_ENV) + 1 + ngx_strlen(slot)
            + sizeof(NGX_WIN32_WORKER_GENERATION_ENV) + 1
              + ngx_strlen(generation)
            + sizeof(NGX_WIN32_WORKER_PIPE_ENV) + 1
              + ngx_strlen(bootstrap->name)
            + sizeof(NGX_WIN32_WORKER_ROLE_ENV) + 1 + ngx_strlen(role)
            + 1;

    env = ngx_alloc(size + extra, cycle->log);
    if (env == NULL) {
        FreeEnvironmentStrings(base);
        return NULL;
    }

    p = (u_char *) base;
    env[0] = '\0';
    {
        u_char *dst;

        dst = (u_char *) env;

        while (*p) {
            len = ngx_strlen(p) + 1;

            if (!ngx_win32_worker_environment_variable(p, len - 1)) {
                dst = ngx_cpymem(dst, p, len);
            }

            p += len;
        }

        p = dst;
    }

    p = ngx_sprintf((u_char *) p, NGX_WIN32_WORKER_SLOT_ENV "=%s%Z", slot);
    p = ngx_sprintf((u_char *) p,
                    NGX_WIN32_WORKER_GENERATION_ENV "=%s%Z", generation);
    p = ngx_sprintf((u_char *) p, NGX_WIN32_WORKER_PIPE_ENV "=%s%Z",
                    bootstrap->name);
    p = ngx_sprintf((u_char *) p, NGX_WIN32_WORKER_ROLE_ENV "=%s%Z", role);
    *p = '\0';

    FreeEnvironmentStrings(base);

    return env;
}


static ngx_uint_t
ngx_win32_worker_environment_variable(u_char *entry, size_t len)
{
    static ngx_str_t  names[] = {
        ngx_string(NGX_WIN32_WORKER_SLOT_ENV),
        ngx_string(NGX_WIN32_WORKER_GENERATION_ENV),
        ngx_string(NGX_WIN32_WORKER_PIPE_ENV),
        ngx_string(NGX_WIN32_WORKER_ROLE_ENV)
    };

    ngx_uint_t  i;

    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (len > names[i].len && entry[names[i].len] == '='
            && ngx_strncasecmp(entry, names[i].data, names[i].len) == 0)
        {
            return 1;
        }
    }

    return 0;
}


static VOID CALLBACK
ngx_win32_process_wait_handler(PVOID data, BOOLEAN timed_out)
{
    (void) data;
    (void) timed_out;

    if (ngx_process_exit_event) {
        (void) SetEvent(ngx_process_exit_event);
    }
}
