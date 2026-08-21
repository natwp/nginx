
/*
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>


#if (NGX_WIN32_GPROF)

void moncontrol(int mode);


static u_char      ngx_win32_gprof_output[NGX_MAX_PATH];
static ngx_uint_t  ngx_win32_gprof_configured;
static ngx_uint_t  ngx_win32_gprof_active;


void
ngx_win32_gprof_start(const char *role, ngx_uint_t slot, ngx_log_t *log)
{
    DWORD    n;
    u_char  *p;
    u_char   root[NGX_MAX_PATH];

    if (role == NULL || log == NULL) {
        return;
    }

    n = GetEnvironmentVariableA("NGX_GPROF_DIR", (char *) root,
                                NGX_MAX_PATH);
    if (n == 0 || n >= NGX_MAX_PATH) {
        return;
    }

    p = ngx_snprintf(ngx_win32_gprof_output,
                     sizeof(ngx_win32_gprof_output) - 1,
                     "%s\\%s-%P-%ui%Z", root, role, ngx_pid, slot);

    if (p >= ngx_win32_gprof_output
             + sizeof(ngx_win32_gprof_output))
    {
        ngx_log_error(NGX_LOG_ALERT, log, 0,
                      "MinGW gprof output path is too long");
        return;
    }

    if (CreateDirectoryA((char *) root, NULL) == 0
        && ngx_errno != ERROR_ALREADY_EXISTS)
    {
        ngx_log_error(NGX_LOG_ALERT, log, ngx_errno,
                      "CreateDirectoryA(\"%s\") failed", root);
        return;
    }

    if (CreateDirectoryA((char *) ngx_win32_gprof_output, NULL) == 0
        && ngx_errno != ERROR_ALREADY_EXISTS)
    {
        ngx_log_error(NGX_LOG_ALERT, log, ngx_errno,
                      "CreateDirectoryA(\"%s\") failed",
                      ngx_win32_gprof_output);
        return;
    }

    ngx_win32_gprof_configured = 1;

    /*
     * MinGW profil() samples the thread that enables it.  nginx runs its
     * event loop in a worker thread, while the CRT initially enables gprof
     * in the process entry thread.  Restart profiling here so sampled PCs
     * describe the actual event loop or router thread.
     */
    moncontrol(0);
    moncontrol(1);
    ngx_win32_gprof_active = 1;

    ngx_log_error(NGX_LOG_NOTICE, log, 0,
                  "MinGW gprof sampling enabled for %s in %s",
                  role, ngx_win32_gprof_output);
}


void
ngx_win32_gprof_stop(void)
{
    if (!ngx_win32_gprof_active) {
        return;
    }

    moncontrol(0);
    ngx_win32_gprof_active = 0;
}


void
ngx_win32_gprof_finish(ngx_log_t *log)
{
    if (!ngx_win32_gprof_configured) {
        return;
    }

    ngx_win32_gprof_stop();

    /* The CRT writes gmon.out during exit(). */
    if (SetCurrentDirectoryA((char *) ngx_win32_gprof_output) == 0 && log) {
        ngx_log_error(NGX_LOG_ALERT, log, ngx_errno,
                      "SetCurrentDirectoryA(\"%s\") failed",
                      ngx_win32_gprof_output);
    }
}


#else


void
ngx_win32_gprof_start(const char *role, ngx_uint_t slot, ngx_log_t *log)
{
    (void) role;
    (void) slot;
    (void) log;
}


void
ngx_win32_gprof_stop(void)
{
}


void
ngx_win32_gprof_finish(ngx_log_t *log)
{
    (void) log;
}


#endif
