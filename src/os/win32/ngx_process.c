
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


static WCHAR *ngx_win32_get_module_filename(ngx_cycle_t *cycle);
static WCHAR *ngx_win32_copy_command_line(ngx_cycle_t *cycle);
static WCHAR *ngx_win32_create_worker_environment(ngx_cycle_t *cycle,
    ngx_win32_worker_bootstrap_t *bootstrap);
static ngx_uint_t ngx_win32_worker_environment_variable(WCHAR *entry,
    size_t len);
static WCHAR *ngx_win32_copy_ascii(WCHAR *dst, u_char *src);
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
    u_long          rc, code;
    ngx_int_t       s;
    ngx_pid_t       pid;
    ngx_exec_ctx_t  ctx;
    HANDLE          events[2];

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

    ngx_memzero(&ctx, sizeof(ngx_exec_ctx_t));

    ctx.wpath = ngx_win32_get_module_filename(cycle);
    if (ctx.wpath == NULL) {
        return NGX_INVALID_PID;
    }

    ctx.wargs = ngx_win32_copy_command_line(cycle);
    if (ctx.wargs == NULL) {
        ngx_free(ctx.wpath);
        return NGX_INVALID_PID;
    }

    ngx_processes[s].bootstrap = ngx_alloc(
                                      sizeof(ngx_win32_worker_bootstrap_t),
                                      cycle->log);
    if (ngx_processes[s].bootstrap == NULL) {
        ngx_free(ctx.wargs);
        ngx_free(ctx.wpath);
        return NGX_INVALID_PID;
    }

    if (ngx_win32_master_create_bootstrap(cycle,
                                          ngx_processes[s].bootstrap,
                                          slot, generation, role)
        != NGX_OK)
    {
        ngx_free(ngx_processes[s].bootstrap);
        ngx_processes[s].bootstrap = NULL;
        ngx_free(ctx.wargs);
        ngx_free(ctx.wpath);
        return NGX_INVALID_PID;
    }

    ctx.name = name;
    ctx.argv = NULL;
    ctx.envp = NULL;
    ctx.wenvironment = ngx_win32_create_worker_environment(
                                           cycle,
                                           ngx_processes[s].bootstrap);

    if (ctx.wenvironment == NULL) {
        ngx_win32_master_close_bootstrap(ngx_processes[s].bootstrap);
        ngx_free(ngx_processes[s].bootstrap);
        ngx_processes[s].bootstrap = NULL;
        ngx_free(ctx.wargs);
        ngx_free(ctx.wpath);
        return NGX_INVALID_PID;
    }

    pid = ngx_execute(cycle, &ctx);

    ngx_free(ctx.wenvironment);
    ngx_free(ctx.wargs);
    ngx_free(ctx.wpath);

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
    BOOL                  inherit_handles, process_created;
    SIZE_T                size;
    STARTUPINFOEXW        si;
    PROCESS_INFORMATION   pi;
    LPPROC_THREAD_ATTRIBUTE_LIST  attributes;
    HANDLE                child_stderr, stderr_handle;
    u_long                flags;
    ngx_uint_t            attributes_initialized;

    ngx_memzero(&si, sizeof(STARTUPINFOEXW));
    si.StartupInfo.cb = sizeof(STARTUPINFOW);

    ngx_memzero(&pi, sizeof(PROCESS_INFORMATION));

    attributes = NULL;
    attributes_initialized = 0;
    child_stderr = NULL;
    inherit_handles = 0;

    stderr_handle = ngx_stderr;

    if (stderr_handle != NULL && stderr_handle != INVALID_HANDLE_VALUE) {
        if (DuplicateHandle(GetCurrentProcess(), stderr_handle,
                            GetCurrentProcess(), &child_stderr, 0, 1,
                            DUPLICATE_SAME_ACCESS)
            == 0)
        {
            ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                          "DuplicateHandle(stderr) failed");
            return NGX_INVALID_PID;
        }

        size = 0;
        (void) InitializeProcThreadAttributeList(NULL, 1, 0, &size);

        if (size == 0) {
            ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                          "InitializeProcThreadAttributeList() failed");
            ngx_close_handle(child_stderr);
            return NGX_INVALID_PID;
        }

        attributes = ngx_alloc(size, cycle->log);
        if (attributes == NULL) {
            ngx_close_handle(child_stderr);
            return NGX_INVALID_PID;
        }

        if (InitializeProcThreadAttributeList(attributes, 1, 0, &size) == 0) {
            ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                          "InitializeProcThreadAttributeList() failed");
            goto failed;
        }

        attributes_initialized = 1;

        if (UpdateProcThreadAttribute(attributes, 0,
                                      PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                      &child_stderr, sizeof(HANDLE), NULL, NULL)
            == 0)
        {
            ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                          "UpdateProcThreadAttribute() failed");
            goto failed;
        }

        si.lpAttributeList = attributes;
        si.StartupInfo.cb = sizeof(STARTUPINFOEXW);
        si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        si.StartupInfo.hStdInput = INVALID_HANDLE_VALUE;
        si.StartupInfo.hStdOutput = INVALID_HANDLE_VALUE;
        si.StartupInfo.hStdError = child_stderr;
        inherit_handles = 1;
    }

    flags = CREATE_NO_WINDOW | CREATE_SUSPENDED;

    if (attributes) {
        flags |= EXTENDED_STARTUPINFO_PRESENT;
    }

    if (ctx->wpath) {
        flags |= CREATE_UNICODE_ENVIRONMENT;

        process_created = CreateProcessW(ctx->wpath, ctx->wargs,
                                         NULL, NULL, inherit_handles, flags,
                                         ctx->wenvironment, NULL,
                                         &si.StartupInfo, &pi);

    } else {
        process_created = CreateProcessA(ctx->path, ctx->args,
                                         NULL, NULL, inherit_handles, flags,
                                         ctx->environment, NULL,
                                         (LPSTARTUPINFOA) &si.StartupInfo, &pi);
    }

    if (process_created == 0) {
        ngx_log_error(NGX_LOG_CRIT, cycle->log, ngx_errno,
                      "CreateProcess() failed");

        goto failed;
    }

    if (attributes_initialized) {
        DeleteProcThreadAttributeList(attributes);
    }

    if (attributes) {
        ngx_free(attributes);
    }

    if (child_stderr) {
        ngx_close_handle(child_stderr);
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

failed:

    if (attributes_initialized) {
        DeleteProcThreadAttributeList(attributes);
    }

    if (attributes) {
        ngx_free(attributes);
    }

    if (child_stderr) {
        ngx_close_handle(child_stderr);
    }

    return NGX_INVALID_PID;
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


static WCHAR *
ngx_win32_get_module_filename(ngx_cycle_t *cycle)
{
    DWORD   n, size;
    WCHAR  *file;

    size = MAX_PATH;

    for ( ;; ) {
        file = ngx_alloc(size * sizeof(WCHAR), cycle->log);
        if (file == NULL) {
            return NULL;
        }

        file[size - 1] = L'\0';
        n = GetModuleFileNameW(NULL, file, size);

        if (n == 0) {
            ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                          "GetModuleFileNameW() failed");
            ngx_free(file);
            return NULL;
        }

        if (n < size && file[n] == L'\0') {
            ngx_log_debug1(NGX_LOG_DEBUG_CORE, cycle->log, 0,
                           "GetModuleFileNameW() returned %ul characters", n);
            return file;
        }

        ngx_free(file);

        if (size >= 32768) {
            ngx_log_error(NGX_LOG_ALERT, cycle->log,
                          ERROR_INSUFFICIENT_BUFFER,
                          "GetModuleFileNameW() returned a path that is too "
                          "long");
            return NULL;
        }

        size *= 2;

        if (size > 32768) {
            size = 32768;
        }
    }
}


static WCHAR *
ngx_win32_copy_command_line(ngx_cycle_t *cycle)
{
    size_t  len;
    WCHAR  *args, *command;

    command = GetCommandLineW();
    if (command == NULL) {
        ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                      "GetCommandLineW() failed");
        return NULL;
    }

    for (len = 0; command[len]; len++) { /* void */ }

    args = ngx_alloc((len + 1) * sizeof(WCHAR), cycle->log);
    if (args == NULL) {
        return NULL;
    }

    ngx_memcpy(args, command, (len + 1) * sizeof(WCHAR));

    return args;
}


static WCHAR *
ngx_win32_create_worker_environment(ngx_cycle_t *cycle,
    ngx_win32_worker_bootstrap_t *bootstrap)
{
    size_t  size, extra, len;
    WCHAR  *base, *dst, *env, *p;
    u_char  slot[NGX_INT_T_LEN + 1];
    u_char  generation[NGX_INT_T_LEN + 1];
    u_char  role[NGX_INT_T_LEN + 1];

    base = GetEnvironmentStringsW();
    if (base == NULL) {
        ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_errno,
                      "GetEnvironmentStringsW() failed");
        return NULL;
    }

    size = 0;
    p = base;

    while (*p) {
        for (len = 0; p[len]; len++) { /* void */ }
        len++;

        if (!ngx_win32_worker_environment_variable(p, len - 1)) {
            size += len;
        }

        p += len;
    }

    (void) ngx_sprintf(slot, "%ui%Z", bootstrap->slot);
    (void) ngx_sprintf(generation, "%ui%Z", bootstrap->generation);
    (void) ngx_sprintf(role, "%ui%Z", bootstrap->role);

    extra = sizeof(NGX_WIN32_WORKER_SLOT_ENV) + 1 + ngx_strlen(slot)
            + sizeof(NGX_WIN32_WORKER_GENERATION_ENV) + 1
              + ngx_strlen(generation)
            + sizeof(NGX_WIN32_WORKER_PIPE_ENV) + 1
              + ngx_strlen(bootstrap->name)
            + sizeof(NGX_WIN32_WORKER_ROLE_ENV) + 1 + ngx_strlen(role)
            + 1;

    env = ngx_alloc((size + extra) * sizeof(WCHAR), cycle->log);
    if (env == NULL) {
        FreeEnvironmentStringsW(base);
        return NULL;
    }

    p = base;
    dst = env;

    while (*p) {
        for (len = 0; p[len]; len++) { /* void */ }
        len++;

        if (!ngx_win32_worker_environment_variable(p, len - 1)) {
            ngx_memcpy(dst, p, len * sizeof(WCHAR));
            dst += len;
        }

        p += len;
    }

    dst = ngx_win32_copy_ascii(dst,
                               (u_char *) NGX_WIN32_WORKER_SLOT_ENV "=");
    dst = ngx_win32_copy_ascii(dst, slot);
    *dst++ = L'\0';

    dst = ngx_win32_copy_ascii(dst,
                         (u_char *) NGX_WIN32_WORKER_GENERATION_ENV "=");
    dst = ngx_win32_copy_ascii(dst, generation);
    *dst++ = L'\0';

    dst = ngx_win32_copy_ascii(dst,
                               (u_char *) NGX_WIN32_WORKER_PIPE_ENV "=");
    dst = ngx_win32_copy_ascii(dst, bootstrap->name);
    *dst++ = L'\0';

    dst = ngx_win32_copy_ascii(dst,
                               (u_char *) NGX_WIN32_WORKER_ROLE_ENV "=");
    dst = ngx_win32_copy_ascii(dst, role);
    *dst++ = L'\0';
    *dst = L'\0';

    FreeEnvironmentStringsW(base);

    return env;
}


static ngx_uint_t
ngx_win32_worker_environment_variable(WCHAR *entry, size_t len)
{
    static ngx_str_t  names[] = {
        ngx_string(NGX_WIN32_WORKER_SLOT_ENV),
        ngx_string(NGX_WIN32_WORKER_GENERATION_ENV),
        ngx_string(NGX_WIN32_WORKER_PIPE_ENV),
        ngx_string(NGX_WIN32_WORKER_ROLE_ENV)
    };

    u_char      c;
    size_t      n;
    ngx_uint_t  i;

    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (len <= names[i].len || entry[names[i].len] != L'=') {
            continue;
        }

        for (n = 0; n < names[i].len; n++) {
            c = names[i].data[n];

            if (c >= 'a' && c <= 'z') {
                c &= ~0x20;
            }

            if (entry[n] >= L'a' && entry[n] <= L'z') {
                if ((WCHAR) (entry[n] & ~0x20) != c) {
                    break;
                }

            } else if (entry[n] != c) {
                break;
            }
        }

        if (n == names[i].len) {
            return 1;
        }
    }

    return 0;
}


static WCHAR *
ngx_win32_copy_ascii(WCHAR *dst, u_char *src)
{
    while (*src) {
        *dst++ = *src++;
    }

    return dst;
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
