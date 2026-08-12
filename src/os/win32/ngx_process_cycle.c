
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>
#include <nginx.h>
#include <ngx_win32_router.h>
#include <ngx_win32_worker.h>


static void ngx_console_init(ngx_cycle_t *cycle);
static int __stdcall ngx_console_handler(u_long type);
static ngx_int_t ngx_create_signal_events(ngx_cycle_t *cycle);
static ngx_int_t ngx_start_worker_processes(ngx_cycle_t *cycle, ngx_int_t type);
static void ngx_reopen_worker_processes(ngx_cycle_t *cycle);
static ngx_int_t ngx_quit_worker_processes(ngx_cycle_t *cycle,
    ngx_uint_t old);
static void ngx_terminate_worker_processes(ngx_cycle_t *cycle);
static ngx_uint_t ngx_reap_worker(ngx_cycle_t *cycle, HANDLE h);
static void ngx_master_process_exit(ngx_cycle_t *cycle, ngx_uint_t status);
static void ngx_worker_process_cycle(ngx_cycle_t *cycle, char *mevn);
static void ngx_worker_process_exit(ngx_cycle_t *cycle);
static ngx_thread_value_t __stdcall ngx_worker_thread(void *data);
static void ngx_cache_processes(ngx_cycle_t *cycle, ngx_uint_t *manager,
    ngx_uint_t *loader);
static ngx_thread_value_t __stdcall ngx_cache_manager_thread(void *data);
static void ngx_cache_manager_process_handler(void);
static ngx_thread_value_t __stdcall ngx_cache_loader_thread(void *data);
static void ngx_wait_worker_thread(ngx_tid_t tid, ngx_log_t *log);


ngx_uint_t     ngx_process;
ngx_uint_t     ngx_worker;
ngx_pid_t      ngx_pid;
ngx_pid_t      ngx_parent;

ngx_uint_t     ngx_inherited;
ngx_pid_t      ngx_new_binary;

volatile LONG  ngx_terminate;
volatile LONG  ngx_quit;
volatile LONG  ngx_reopen;
sig_atomic_t   ngx_reconfigure;
volatile ngx_uint_t  ngx_exiting;


HANDLE         ngx_master_process_event;
char           ngx_master_process_event_name[NGX_PROCESS_SYNC_NAME];

static HANDLE  ngx_stop_event;
static char    ngx_stop_event_name[NGX_PROCESS_SYNC_NAME];
static HANDLE  ngx_quit_event;
static char    ngx_quit_event_name[NGX_PROCESS_SYNC_NAME];
static HANDLE  ngx_reopen_event;
static char    ngx_reopen_event_name[NGX_PROCESS_SYNC_NAME];
static HANDLE  ngx_reload_event;
static char    ngx_reload_event_name[NGX_PROCESS_SYNC_NAME];

static ngx_uint_t  ngx_master_generation = 1;

static HANDLE  ngx_cache_manager_mutex;
static char    ngx_cache_manager_mutex_name[NGX_PROCESS_SYNC_NAME];
static HANDLE  ngx_cache_loader_mutex;
static char    ngx_cache_loader_mutex_name[NGX_PROCESS_SYNC_NAME];
static HANDLE  ngx_cache_manager_event;


void
ngx_master_process_cycle(ngx_cycle_t *cycle)
{
    u_long          nev, ev, timeout;
    ngx_err_t       err;
    ngx_int_t       n, resumed, started;
    ngx_msec_t      timer;
    ngx_uint_t      live, new_routed, routed;
    ngx_core_conf_t *ccf;
    HANDLE          events[5];

    ngx_sprintf((u_char *) ngx_master_process_event_name,
                "ngx_master_%s%Z", ngx_unique);

    if (ngx_process == NGX_PROCESS_WORKER) {
        ngx_worker_process_cycle(cycle, ngx_master_process_event_name);
        return;
    }

    ngx_log_debug0(NGX_LOG_DEBUG_CORE, cycle->log, 0, "master started");

    ccf = (ngx_core_conf_t *) ngx_get_conf(cycle->conf_ctx, ngx_core_module);

    ngx_console_init(cycle);

    (void) ngx_win32_job_init(cycle);

    SetEnvironmentVariable("ngx_unique", ngx_unique);

    ngx_master_process_event = CreateEvent(NULL, 1, 0,
                                           ngx_master_process_event_name);
    if (ngx_master_process_event == NULL) {
        ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                      "CreateEvent(\"%s\") failed",
                      ngx_master_process_event_name);
        exit(2);
    }

    if (ngx_create_signal_events(cycle) != NGX_OK) {
        exit(2);
    }

    ngx_process_exit_event = CreateEvent(NULL, 1, 0, NULL);
    if (ngx_process_exit_event == NULL) {
        ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                      "CreateEvent(process exit) failed");
        exit(2);
    }

    ngx_sprintf((u_char *) ngx_cache_manager_mutex_name,
                "ngx_cache_manager_mutex_%s%Z", ngx_unique);

    ngx_cache_manager_mutex = CreateMutex(NULL, 0,
                                          ngx_cache_manager_mutex_name);
    if (ngx_cache_manager_mutex == NULL) {
        ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                   "CreateMutex(\"%s\") failed", ngx_cache_manager_mutex_name);
        exit(2);
    }

    ngx_sprintf((u_char *) ngx_cache_loader_mutex_name,
                "ngx_cache_loader_mutex_%s%Z", ngx_unique);

    ngx_cache_loader_mutex = CreateMutex(NULL, 0,
                                         ngx_cache_loader_mutex_name);
    if (ngx_cache_loader_mutex == NULL) {
        ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                      "CreateMutex(\"%s\") failed",
                      ngx_cache_loader_mutex_name);
        exit(2);
    }


    events[0] = ngx_stop_event;
    events[1] = ngx_quit_event;
    events[2] = ngx_reopen_event;
    events[3] = ngx_reload_event;
    events[4] = ngx_process_exit_event;

    if (ngx_start_worker_processes(cycle, NGX_PROCESS_RESPAWN)
        != ccf->worker_processes)
    {
        ngx_log_error(NGX_LOG_ALERT, cycle->log, 0,
                      "could not start the configured worker processes");

        ngx_win32_set_terminate(1);
        ngx_terminate_worker_processes(cycle);
        ngx_master_process_exit(cycle, 2);
    }

    if (ngx_win32_router_start(cycle, ngx_master_generation) != NGX_OK) {
        ngx_log_error(NGX_LOG_ALERT, cycle->log, 0,
                      "could not start the Win32 network router");

        ngx_win32_set_terminate(1);
        ngx_terminate_worker_processes(cycle);
        ngx_master_process_exit(cycle, 2);
    }

    timer = 0;
    timeout = INFINITE;

    for ( ;; ) {

        if (ngx_win32_router_failed()) {
            ngx_log_error(NGX_LOG_ALERT, cycle->log, 0,
                          "Win32 network router failed; terminating");

            ngx_win32_set_terminate(1);
            ngx_terminate_worker_processes(cycle);
            ngx_master_process_exit(cycle, 2);
        }

        nev = 5;

        if (timer) {
            timeout = timer > ngx_current_msec ? timer - ngx_current_msec : 0;
        }

        ev = WaitForMultipleObjects(nev, events, 0, timeout);

        err = ngx_errno;
        ngx_time_update();

        ngx_log_debug1(NGX_LOG_DEBUG_CORE, cycle->log, 0,
                       "master WaitForMultipleObjects: %ul", ev);

        if (ev == WAIT_OBJECT_0) {
            ngx_log_error(NGX_LOG_NOTICE, cycle->log, 0, "exiting");

            if (ResetEvent(ngx_stop_event) == 0) {
                ngx_log_error(NGX_LOG_ALERT, cycle->log, 0,
                              "ResetEvent(\"%s\") failed", ngx_stop_event_name);
            }

            if (timer == 0) {
                timer = ngx_current_msec + 5000;
            }

            ngx_win32_set_terminate(1);

            if (ngx_win32_router_pause(cycle) != NGX_OK) {
                ngx_log_error(NGX_LOG_ALERT, cycle->log, 0,
                              "could not pause the Win32 network router");
                ngx_terminate_worker_processes(cycle);
                ngx_master_process_exit(cycle, 2);
            }

            if (ngx_quit_worker_processes(cycle, 0) != NGX_OK) {
                ngx_terminate_worker_processes(cycle);
                ngx_master_process_exit(cycle, 2);
            }

            continue;
        }

        if (ev == WAIT_OBJECT_0 + 1) {
            ngx_log_error(NGX_LOG_NOTICE, cycle->log, 0, "shutting down");

            if (ResetEvent(ngx_quit_event) == 0) {
                ngx_log_error(NGX_LOG_ALERT, cycle->log, 0,
                              "ResetEvent(\"%s\") failed", ngx_quit_event_name);
            }

            ngx_win32_set_quit(1);

            if (ngx_win32_router_drain(cycle) != NGX_OK) {
                ngx_log_error(NGX_LOG_ALERT, cycle->log, 0,
                              "could not drain the Win32 network router");
                ngx_win32_set_terminate(1);
                ngx_terminate_worker_processes(cycle);
                ngx_master_process_exit(cycle, 2);
            }

            if (ngx_quit_worker_processes(cycle, 0) != NGX_OK) {
                ngx_terminate_worker_processes(cycle);
                ngx_master_process_exit(cycle, 2);
            }

            continue;
        }

        if (ev == WAIT_OBJECT_0 + 2) {
            ngx_log_error(NGX_LOG_NOTICE, cycle->log, 0, "reopening logs");

            if (ResetEvent(ngx_reopen_event) == 0) {
                ngx_log_error(NGX_LOG_ALERT, cycle->log, 0,
                              "ResetEvent(\"%s\") failed",
                              ngx_reopen_event_name);
            }

            ngx_reopen_files(cycle, -1);
            ngx_reopen_worker_processes(cycle);

            continue;
        }

        if (ev == WAIT_OBJECT_0 + 3) {
            ngx_log_error(NGX_LOG_NOTICE, cycle->log, 0, "reconfiguring");

            if (ResetEvent(ngx_reload_event) == 0) {
                ngx_log_error(NGX_LOG_ALERT, cycle->log, 0,
                              "ResetEvent(\"%s\") failed",
                              ngx_reload_event_name);
            }

            routed = ngx_win32_router_required(cycle);

            if (routed && ngx_win32_router_pause(cycle) != NGX_OK) {
                if (ngx_win32_router_resume(cycle,
                                            ngx_master_generation)
                    == NGX_OK)
                {
                    continue;
                }

                ngx_log_error(NGX_LOG_ALERT, cycle->log, 0,
                              "could not restore the Win32 network router "
                              "after a failed reload pause");

                ngx_win32_set_terminate(1);
                ngx_terminate_worker_processes(cycle);
                ngx_master_process_exit(cycle, 2);

                continue;
            }

            cycle = ngx_init_cycle(cycle);
            if (cycle == NULL) {
                cycle = (ngx_cycle_t *) ngx_cycle;

                if (routed
                    && ngx_win32_router_resume(cycle,
                                               ngx_master_generation)
                    != NGX_OK)
                {
                    ngx_log_error(NGX_LOG_ALERT, cycle->log, 0,
                                  "could not restore the Win32 network "
                                  "router after a failed reload");

                    ngx_win32_set_terminate(1);
                    ngx_terminate_worker_processes(cycle);
                    ngx_master_process_exit(cycle, 2);
                }

                continue;
            }

            ngx_cycle = cycle;

            ngx_master_generation++;

            ccf = (ngx_core_conf_t *) ngx_get_conf(cycle->conf_ctx,
                                                   ngx_core_module);
            new_routed = ngx_win32_router_required(cycle);

            started = ngx_start_worker_processes(cycle,
                                                  NGX_PROCESS_JUST_RESPAWN);

            resumed = NGX_OK;
            if (started == ccf->worker_processes) {
                if (new_routed) {
                    if (routed) {
                        resumed = ngx_win32_router_resume(
                                                    cycle,
                                                    ngx_master_generation);

                    } else {
                        resumed = ngx_win32_router_start(
                                                    cycle,
                                                    ngx_master_generation);
                    }

                } else if (routed) {
                    resumed = ngx_win32_router_stop(cycle);
                }

            } else {
                resumed = NGX_ERROR;
            }

            if (started == ccf->worker_processes && resumed == NGX_OK) {
                if (ngx_quit_worker_processes(cycle, 1) != NGX_OK) {
                    resumed = NGX_ERROR;
                }
            }

            if (started != ccf->worker_processes || resumed != NGX_OK) {
                ngx_log_error(NGX_LOG_ALERT, cycle->log, 0,
                              "could not activate the new worker generation "
                              "(started %i of %i, router activation %i)",
                              started, ccf->worker_processes, resumed);

                /*
                 * ngx_init_cycle() has already committed the new cycle and
                 * released the old one.  Do not retain a partially started
                 * generation: fail closed and let the service manager
                 * restart nginx.
                 */
                ngx_win32_set_terminate(1);
                ngx_terminate_worker_processes(cycle);
                ngx_master_process_exit(cycle, 2);
            }

            continue;
        }

        if (ev == WAIT_OBJECT_0 + 4) {

            ngx_log_debug0(NGX_LOG_DEBUG_CORE, cycle->log, 0, "reap worker");

            if (ResetEvent(ngx_process_exit_event) == 0) {
                ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                              "ResetEvent(process exit) failed");
            }

            live = 0;

            for (n = 0; n < ngx_last_process; n++) {
                if (ngx_processes[n].handle == NULL) {
                    continue;
                }

                if (WaitForSingleObject(ngx_processes[n].handle, 0)
                    == WAIT_OBJECT_0)
                {
                    (void) ngx_reap_worker(cycle,
                                           ngx_processes[n].handle);
                }
            }

            live = 0;

            for (n = 0; n < ngx_last_process; n++) {
                if (ngx_processes[n].handle) {
                    live = 1;
                    break;
                }
            }

            if (!live && (ngx_win32_terminate_requested()
                          || ngx_win32_quit_requested()))
            {
                ngx_master_process_exit(cycle, 0);
            }

            continue;
        }

        if (ev == WAIT_TIMEOUT) {
            ngx_terminate_worker_processes(cycle);

            ngx_master_process_exit(cycle, 0);
        }

        if (ev == WAIT_FAILED) {
            ngx_log_error(NGX_LOG_ALERT, cycle->log, err,
                          "WaitForMultipleObjects() failed");

            continue;
        }

        ngx_log_error(NGX_LOG_ALERT, cycle->log, 0,
            "WaitForMultipleObjects() returned unexpected value %ul", ev);
    }
}


static void
ngx_console_init(ngx_cycle_t *cycle)
{
    ngx_core_conf_t  *ccf;

    ccf = (ngx_core_conf_t *) ngx_get_conf(cycle->conf_ctx, ngx_core_module);

    if (ccf->daemon) {
        if (FreeConsole() == 0) {
            ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                          "FreeConsole() failed");
        }

        return;
    }

    if (SetConsoleCtrlHandler(ngx_console_handler, 1) == 0) {
        ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                      "SetConsoleCtrlHandler() failed");
    }
}


static int __stdcall
ngx_console_handler(u_long type)
{
    char  *msg;

    switch (type) {

    case CTRL_C_EVENT:
        msg = "Ctrl-C pressed, exiting";
        break;

    case CTRL_BREAK_EVENT:
        msg = "Ctrl-Break pressed, exiting";
        break;

    case CTRL_CLOSE_EVENT:
        msg = "console closing, exiting";
        break;

    case CTRL_LOGOFF_EVENT:
        msg = "user logs off, exiting";
        break;

    default:
        return 0;
    }

    ngx_log_error(NGX_LOG_NOTICE, ngx_cycle->log, 0, msg);

    if (ngx_stop_event == NULL) {
        return 1;
    }

    if (SetEvent(ngx_stop_event) == 0) {
        ngx_log_error(NGX_LOG_ALERT, ngx_cycle->log, 0,
                      "SetEvent(\"%s\") failed", ngx_stop_event_name);
    }

    return 1;
}


static ngx_int_t
ngx_create_signal_events(ngx_cycle_t *cycle)
{
    ngx_sprintf((u_char *) ngx_stop_event_name,
                "Global\\ngx_stop_%s%Z", ngx_unique);

    ngx_stop_event = CreateEvent(NULL, 1, 0, ngx_stop_event_name);
    if (ngx_stop_event == NULL) {
        ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                      "CreateEvent(\"%s\") failed", ngx_stop_event_name);
        return NGX_ERROR;
    }


    ngx_sprintf((u_char *) ngx_quit_event_name,
                "Global\\ngx_quit_%s%Z", ngx_unique);

    ngx_quit_event = CreateEvent(NULL, 1, 0, ngx_quit_event_name);
    if (ngx_quit_event == NULL) {
        ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                      "CreateEvent(\"%s\") failed", ngx_quit_event_name);
        return NGX_ERROR;
    }


    ngx_sprintf((u_char *) ngx_reopen_event_name,
                "Global\\ngx_reopen_%s%Z", ngx_unique);

    ngx_reopen_event = CreateEvent(NULL, 1, 0, ngx_reopen_event_name);
    if (ngx_reopen_event == NULL) {
        ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                      "CreateEvent(\"%s\") failed", ngx_reopen_event_name);
        return NGX_ERROR;
    }


    ngx_sprintf((u_char *) ngx_reload_event_name,
                "Global\\ngx_reload_%s%Z", ngx_unique);

    ngx_reload_event = CreateEvent(NULL, 1, 0, ngx_reload_event_name);
    if (ngx_reload_event == NULL) {
        ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                      "CreateEvent(\"%s\") failed", ngx_reload_event_name);
        return NGX_ERROR;
    }

    return NGX_OK;
}


static ngx_int_t
ngx_start_worker_processes(ngx_cycle_t *cycle, ngx_int_t type)
{
    ngx_int_t         n;
    ngx_core_conf_t  *ccf;

    ngx_log_error(NGX_LOG_NOTICE, cycle->log, 0, "start worker processes");

    ccf = (ngx_core_conf_t *) ngx_get_conf(cycle->conf_ctx, ngx_core_module);

    for (n = 0; n < ccf->worker_processes; n++) {
        if (ngx_spawn_worker(cycle, "worker", type, n,
                             ngx_master_generation,
                             NGX_WIN32_WORKER_ROLE)
            == NGX_INVALID_PID)
        {
            break;
        }
    }

    if (n == ccf->worker_processes
        && ngx_win32_router_update_workers(cycle, ngx_master_generation)
           != NGX_OK)
    {
        return NGX_ERROR;
    }

    return n;
}


static void
ngx_reopen_worker_processes(ngx_cycle_t *cycle)
{
    ngx_int_t  n;

    for (n = 0; n < ngx_last_process; n++) {

        if (ngx_processes[n].handle == NULL) {
            continue;
        }

        if (SetEvent(ngx_processes[n].reopen) == 0) {
            ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                          "SetEvent(\"%s\") failed",
                          ngx_processes[n].reopen_event);
        }
    }
}


static ngx_int_t
ngx_quit_worker_processes(ngx_cycle_t *cycle, ngx_uint_t old)
{
    ngx_int_t  n, rc;

    rc = NGX_OK;

    for (n = 0; n < ngx_last_process; n++) {

        ngx_log_debug5(NGX_LOG_DEBUG_CORE, cycle->log, 0,
                       "process: %d %P %p e:%d j:%d",
                       n,
                       ngx_processes[n].pid,
                       ngx_processes[n].handle,
                       ngx_processes[n].exiting,
                       ngx_processes[n].just_spawn);

        if (old && ngx_processes[n].just_spawn) {
            ngx_processes[n].just_spawn = 0;
            continue;
        }

        if (ngx_processes[n].handle == NULL) {
            continue;
        }

        if (SetEvent(ngx_processes[n].quit) == 0) {
            ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                          "SetEvent(\"%s\") failed",
                          ngx_processes[n].quit_event);
            rc = NGX_ERROR;
        }

        ngx_processes[n].exiting = 1;
    }

    if (ngx_win32_router_update_workers(cycle, ngx_master_generation)
        != NGX_OK)
    {
        rc = NGX_ERROR;
    }

    return rc;
}


static void
ngx_terminate_worker_processes(ngx_cycle_t *cycle)
{
    ngx_int_t  n;

    for (n = 0; n < ngx_last_process; n++) {

        if (ngx_processes[n].handle == NULL) {
            continue;
        }

        if (TerminateProcess(ngx_processes[n].handle, 0) == 0) {
            ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                          "TerminateProcess(\"%p\") failed",
                          ngx_processes[n].handle);
        }

        ngx_processes[n].exiting = 1;

        if (ngx_processes[n].wait) {
            (void) UnregisterWaitEx(ngx_processes[n].wait,
                                    INVALID_HANDLE_VALUE);
            ngx_processes[n].wait = NULL;
        }

        if (ngx_processes[n].reopen) {
            ngx_close_handle(ngx_processes[n].reopen);
            ngx_processes[n].reopen = NULL;
        }

        if (ngx_processes[n].quit) {
            ngx_close_handle(ngx_processes[n].quit);
            ngx_processes[n].quit = NULL;
        }

        if (ngx_processes[n].term) {
            ngx_close_handle(ngx_processes[n].term);
            ngx_processes[n].term = NULL;
        }

        if (ngx_processes[n].handle) {
            ngx_close_handle(ngx_processes[n].handle);
            ngx_processes[n].handle = NULL;
        }
    }
}


static ngx_uint_t
ngx_reap_worker(ngx_cycle_t *cycle, HANDLE h)
{
    u_long     code;
    ngx_int_t  n;
    char      *name;

    for (n = 0; n < ngx_last_process; n++) {

        if (ngx_processes[n].handle != h) {
            continue;
        }

        if (GetExitCodeProcess(h, &code) == 0) {
            ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                          "GetExitCodeProcess(%P) failed",
                          ngx_processes[n].pid);
        }

        ngx_log_error(NGX_LOG_NOTICE, cycle->log, 0,
                      "%s process %P exited with code %Xl",
                      ngx_processes[n].name, ngx_processes[n].pid, code);

        ngx_processes[n].ready_state = 0;
        if (ngx_win32_router_update_workers(cycle, ngx_master_generation)
            != NGX_OK)
        {
            ngx_win32_set_terminate(1);
            ngx_terminate_worker_processes(cycle);
            ngx_master_process_exit(cycle, 2);
        }

        if (ngx_processes[n].wait) {
            (void) UnregisterWaitEx(ngx_processes[n].wait,
                                    INVALID_HANDLE_VALUE);
            ngx_processes[n].wait = NULL;
        }

        ngx_close_handle(ngx_processes[n].reopen);
        ngx_close_handle(ngx_processes[n].quit);
        ngx_close_handle(ngx_processes[n].term);
        ngx_close_handle(h);

        if (ngx_processes[n].bootstrap) {
            ngx_win32_master_close_bootstrap(ngx_processes[n].bootstrap);
            ngx_free(ngx_processes[n].bootstrap);
            ngx_processes[n].bootstrap = NULL;
        }

        ngx_processes[n].handle = NULL;
        ngx_processes[n].term = NULL;
        ngx_processes[n].quit = NULL;
        ngx_processes[n].reopen = NULL;

        if (!ngx_processes[n].exiting
            && !ngx_win32_terminate_requested()
            && !ngx_win32_quit_requested())
        {

            name = ngx_processes[n].name;

            if (ngx_spawn_process(cycle, name, n)
                == NGX_INVALID_PID)
            {
                ngx_log_error(NGX_LOG_ALERT, cycle->log, 0,
                              "could not respawn %s", name);

                if (n == ngx_last_process - 1) {
                    ngx_last_process--;
                }

                ngx_win32_set_terminate(1);
                ngx_terminate_worker_processes(cycle);
                ngx_master_process_exit(cycle, 2);

            } else {
                if (ngx_win32_router_update_workers(cycle,
                                                    ngx_master_generation)
                    != NGX_OK)
                {
                    ngx_win32_set_terminate(1);
                    ngx_terminate_worker_processes(cycle);
                    ngx_master_process_exit(cycle, 2);
                }
            }
        }

        goto found;
    }

    ngx_log_error(NGX_LOG_ALERT, cycle->log, 0, "unknown process handle %p", h);

found:

    for (n = 0; n < ngx_last_process; n++) {

        ngx_log_debug5(NGX_LOG_DEBUG_CORE, cycle->log, 0,
                       "process: %d %P %p e:%d j:%d",
                       n,
                       ngx_processes[n].pid,
                       ngx_processes[n].handle,
                       ngx_processes[n].exiting,
                       ngx_processes[n].just_spawn);

        if (ngx_processes[n].handle) {
            return 1;
        }
    }

    return 0;
}


static void
ngx_master_process_exit(ngx_cycle_t *cycle, ngx_uint_t status)
{
    ngx_uint_t  i;

    ngx_delete_pidfile(cycle);

    if (ngx_win32_router_stop(cycle) != NGX_OK) {
        status = 2;
    }
    ngx_win32_job_done();

    ngx_close_handle(ngx_cache_manager_mutex);
    ngx_close_handle(ngx_cache_loader_mutex);
    ngx_close_handle(ngx_stop_event);
    ngx_close_handle(ngx_quit_event);
    ngx_close_handle(ngx_reopen_event);
    ngx_close_handle(ngx_reload_event);
    ngx_close_handle(ngx_process_exit_event);
    ngx_process_exit_event = NULL;
    ngx_close_handle(ngx_master_process_event);

    ngx_log_error(NGX_LOG_NOTICE, cycle->log, 0, "exit");

    for (i = 0; cycle->modules[i]; i++) {
        if (cycle->modules[i]->exit_master) {
            cycle->modules[i]->exit_master(cycle);
        }
    }

    ngx_destroy_pool(cycle->pool);

    exit(status);
}


static void
ngx_worker_process_cycle(ngx_cycle_t *cycle, char *mevn)
{
    char        wtevn[NGX_PROCESS_SYNC_NAME];
    char        wqevn[NGX_PROCESS_SYNC_NAME];
    char        wroevn[NGX_PROCESS_SYNC_NAME];
    HANDLE      mev, events[3];
    u_long      ev;
    ngx_err_t   err;
    ngx_tid_t   wtid, cmtid, cltid;
    ngx_uint_t  cache_loader, cache_manager;
    ngx_log_t  *log;

    log = cycle->log;

    wtid = NULL;
    cmtid = NULL;
    cltid = NULL;

    ngx_cache_processes(cycle, &cache_manager, &cache_loader);

    ngx_worker = ngx_win32_worker_slot;

    ngx_log_debug0(NGX_LOG_DEBUG_CORE, log, 0, "worker started");

    ngx_sprintf((u_char *) wtevn, "ngx_worker_term_%P%Z", ngx_pid);
    events[0] = CreateEvent(NULL, 1, 0, wtevn);
    if (events[0] == NULL) {
        ngx_log_error(NGX_LOG_ALERT, log, ngx_errno,
                      "CreateEvent(\"%s\") failed", wtevn);
        goto failed;
    }

    ngx_sprintf((u_char *) wqevn, "ngx_worker_quit_%P%Z", ngx_pid);
    events[1] = CreateEvent(NULL, 1, 0, wqevn);
    if (events[1] == NULL) {
        ngx_log_error(NGX_LOG_ALERT, log, ngx_errno,
                      "CreateEvent(\"%s\") failed", wqevn);
        goto failed;
    }

    ngx_sprintf((u_char *) wroevn, "ngx_worker_reopen_%P%Z", ngx_pid);
    events[2] = CreateEvent(NULL, 1, 0, wroevn);
    if (events[2] == NULL) {
        ngx_log_error(NGX_LOG_ALERT, log, ngx_errno,
                      "CreateEvent(\"%s\") failed", wroevn);
        goto failed;
    }

    mev = OpenEvent(EVENT_MODIFY_STATE, 0, mevn);
    if (mev == NULL) {
        ngx_log_error(NGX_LOG_ALERT, log, ngx_errno,
                      "OpenEvent(\"%s\") failed", mevn);
        goto failed;
    }

    if (SetEvent(mev) == 0) {
        ngx_log_error(NGX_LOG_ALERT, log, ngx_errno,
                      "SetEvent(\"%s\") failed", mevn);
        goto failed;
    }

    if (ngx_win32_worker_send_status(NGX_WIN32_BOOTSTRAP_CONTROL_READY, 0)
        == NGX_ERROR)
    {
        goto failed;
    }


    if (cache_manager || cache_loader) {
        ngx_cache_manager_event = CreateEvent(NULL, 1, 0, NULL);
        if (ngx_cache_manager_event == NULL) {
            ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                          "CreateEvent(\"ngx_cache_manager_event\") failed");
            goto failed;
        }
    }

    if (cache_manager) {
        ngx_sprintf((u_char *) ngx_cache_manager_mutex_name,
                    "ngx_cache_manager_mutex_%s%Z", ngx_unique);

        ngx_cache_manager_mutex = OpenMutex(SYNCHRONIZE|MUTEX_MODIFY_STATE, 0,
                                            ngx_cache_manager_mutex_name);
        if (ngx_cache_manager_mutex == NULL) {
            ngx_log_error(NGX_LOG_ALERT, log, ngx_errno,
                          "OpenMutex(\"%s\") failed",
                          ngx_cache_manager_mutex_name);
            goto failed;
        }
    }

    if (cache_loader) {
        ngx_sprintf((u_char *) ngx_cache_loader_mutex_name,
                    "ngx_cache_loader_mutex_%s%Z", ngx_unique);

        ngx_cache_loader_mutex = OpenMutex(SYNCHRONIZE|MUTEX_MODIFY_STATE, 0,
                                           ngx_cache_loader_mutex_name);
        if (ngx_cache_loader_mutex == NULL) {
            ngx_log_error(NGX_LOG_ALERT, log, ngx_errno,
                          "OpenMutex(\"%s\") failed",
                          ngx_cache_loader_mutex_name);
            goto failed;
        }
    }


    if (ngx_create_thread(&wtid, ngx_worker_thread, NULL, log) != 0) {
        goto failed;
    }

    if (cache_manager
        && ngx_create_thread(&cmtid, ngx_cache_manager_thread, NULL, log) != 0)
    {
        goto failed;
    }

    if (cache_loader
        && ngx_create_thread(&cltid, ngx_cache_loader_thread, NULL, log) != 0)
    {
        goto failed;
    }

    for ( ;; ) {
        ev = WaitForMultipleObjects(3, events, 0, INFINITE);

        err = ngx_errno;
        ngx_time_update();

        ngx_log_debug1(NGX_LOG_DEBUG_CORE, log, 0,
                       "worker WaitForMultipleObjects: %ul", ev);

        if (ev == WAIT_OBJECT_0) {
            ngx_win32_set_terminate(1);
            ngx_log_error(NGX_LOG_NOTICE, log, 0, "exiting");

            if (ResetEvent(events[0]) == 0) {
                ngx_log_error(NGX_LOG_ALERT, log, 0,
                              "ResetEvent(\"%s\") failed", wtevn);
            }

            break;
        }

        if (ev == WAIT_OBJECT_0 + 1) {
            ngx_win32_set_quit(1);
            ngx_log_error(NGX_LOG_NOTICE, log, 0, "gracefully shutting down");
            break;
        }

        if (ev == WAIT_OBJECT_0 + 2) {
            ngx_win32_set_reopen(1);
            ngx_log_error(NGX_LOG_NOTICE, log, 0, "reopening logs");

            if (ResetEvent(events[2]) == 0) {
                ngx_log_error(NGX_LOG_ALERT, log, 0,
                              "ResetEvent(\"%s\") failed", wroevn);
            }

            continue;
        }

        if (ev == WAIT_FAILED) {
            ngx_log_error(NGX_LOG_ALERT, log, err,
                          "WaitForMultipleObjects() failed");

            goto failed;
        }
    }

    /* wait threads */

    if (ngx_cache_manager_event && SetEvent(ngx_cache_manager_event) == 0) {
        ngx_log_error(NGX_LOG_ALERT, log, ngx_errno,
                      "SetEvent(\"ngx_cache_manager_event\") failed");
    }

    ngx_wait_worker_thread(wtid, log);
    ngx_wait_worker_thread(cmtid, log);
    ngx_wait_worker_thread(cltid, log);

    if (ngx_cache_manager_event) {
        ngx_close_handle(ngx_cache_manager_event);
        ngx_cache_manager_event = NULL;
    }

    if (ngx_cache_manager_mutex) {
        ngx_close_handle(ngx_cache_manager_mutex);
        ngx_cache_manager_mutex = NULL;
    }

    if (ngx_cache_loader_mutex) {
        ngx_close_handle(ngx_cache_loader_mutex);
        ngx_cache_loader_mutex = NULL;
    }
    ngx_close_handle(events[0]);
    ngx_close_handle(events[1]);
    ngx_close_handle(events[2]);
    ngx_close_handle(mev);

    ngx_worker_process_exit(cycle);

failed:

    exit(2);
}


static ngx_thread_value_t __stdcall
ngx_worker_thread(void *data)
{
    ngx_int_t     n;
    ngx_time_t   *tp;
    ngx_cycle_t  *cycle;

    tp = ngx_timeofday();
    srand((ngx_pid << 16) ^ (unsigned) tp->sec ^ tp->msec);

    cycle = (ngx_cycle_t *) ngx_cycle;

    for (n = 0; cycle->modules[n]; n++) {
        if (cycle->modules[n]->init_process) {
            if (cycle->modules[n]->init_process(cycle) == NGX_ERROR) {
                /* fatal */
                exit(2);
            }
        }
    }

    if (ngx_win32_worker_channel_init(cycle) != NGX_OK) {
        exit(2);
    }

    if (ngx_win32_worker_send_status(NGX_WIN32_BOOTSTRAP_READY, 0)
        == NGX_ERROR)
    {
        exit(2);
    }

    while (!ngx_win32_quit_requested()) {

        if (ngx_win32_exiting_requested()) {
            if (ngx_event_no_timers_left() == NGX_OK) {
                break;
            }
        }

        ngx_log_debug0(NGX_LOG_DEBUG_CORE, cycle->log, 0, "worker cycle");

        ngx_process_events_and_timers(cycle);

        if (ngx_win32_terminate_requested()) {
            return 0;
        }

        if (ngx_win32_quit_requested()) {
            ngx_win32_set_quit(0);

            if (!ngx_win32_exiting_requested()) {
                ngx_win32_set_exiting(1);
                ngx_set_shutdown_timer(cycle);
                ngx_close_listening_sockets(cycle);
                ngx_close_idle_connections(cycle);
                ngx_event_process_posted(cycle, &ngx_posted_events);
            }
        }

        if (ngx_win32_reopen_requested()) {
            ngx_win32_set_reopen(0);
            ngx_reopen_files(cycle, -1);
        }
    }

    ngx_log_error(NGX_LOG_NOTICE, cycle->log, 0, "exiting");

    return 0;
}


static void
ngx_cache_processes(ngx_cycle_t *cycle, ngx_uint_t *manager,
    ngx_uint_t *loader)
{
    ngx_uint_t    i;
    ngx_path_t  **path;

    *manager = 0;
    *loader = 0;

    if (ngx_win32_worker_slot != 0) {
        return;
    }

    path = cycle->paths.elts;

    for (i = 0; i < cycle->paths.nelts; i++) {
        if (path[i]->manager) {
            *manager = 1;
        }

        if (path[i]->loader) {
            *loader = 1;
        }
    }

    if (*manager == 0) {
        *loader = 0;
    }
}


static void
ngx_worker_process_exit(ngx_cycle_t *cycle)
{
    ngx_uint_t         i;
    ngx_connection_t  *c;

    ngx_log_error(NGX_LOG_NOTICE, cycle->log, 0, "exit");

    /*
     * Stop the routed channel before event modules tear down their IOCP.
     * The channel reader may otherwise post a notification into a port that
     * ngx_iocp_done() has already started closing.
     */
    ngx_win32_worker_channel_done();

    for (i = 0; cycle->modules[i]; i++) {
        if (cycle->modules[i]->exit_process) {
            cycle->modules[i]->exit_process(cycle);
        }
    }

    ngx_win32_accept_mutex_done();

    if (ngx_win32_exiting_requested()
        && !ngx_win32_terminate_requested())
    {
        c = cycle->connections;
        for (i = 0; i < cycle->connection_n; i++) {
            if (c[i].fd != (ngx_socket_t) -1
                && c[i].read
                && !c[i].read->accept
                && !c[i].read->channel
                && !c[i].read->resolver)
            {
                ngx_log_error(NGX_LOG_ALERT, cycle->log, 0,
                              "*%uA open socket #%d left in connection %ui",
                              c[i].number, c[i].fd, i);
            }
        }
    }

    if (ngx_process == NGX_PROCESS_SINGLE) {
        ngx_delete_pidfile(cycle);

        ngx_close_handle(ngx_stop_event);
        ngx_stop_event = NULL;
        ngx_close_handle(ngx_quit_event);
        ngx_quit_event = NULL;
        ngx_close_handle(ngx_reopen_event);
        ngx_reopen_event = NULL;
        ngx_close_handle(ngx_reload_event);
        ngx_reload_event = NULL;

        for (i = 0; cycle->modules[i]; i++) {
            if (cycle->modules[i]->exit_master) {
                cycle->modules[i]->exit_master(cycle);
            }
        }
    }

    ngx_destroy_pool(cycle->pool);

    exit(0);
}


static ngx_thread_value_t __stdcall
ngx_cache_manager_thread(void *data)
{
    u_long        ev;
    HANDLE        events[2];
    ngx_err_t     err;
    ngx_cycle_t  *cycle;

    cycle = (ngx_cycle_t *) ngx_cycle;

    events[0] = ngx_cache_manager_event;
    events[1] = ngx_cache_manager_mutex;

    for ( ;; ) {
        ev = WaitForMultipleObjects(2, events, 0, INFINITE);

        err = ngx_errno;
        ngx_time_update();

        ngx_log_debug1(NGX_LOG_DEBUG_CORE, cycle->log, 0,
                       "cache manager WaitForMultipleObjects: %ul", ev);

        if (ev == WAIT_FAILED) {
            ngx_log_error(NGX_LOG_ALERT, cycle->log, err,
                          "WaitForMultipleObjects() failed");
            return 0;
        }

        /*
         * ev == WAIT_OBJECT_0
         * ev == WAIT_OBJECT_0 + 1
         * ev == WAIT_ABANDONED_0 + 1
         */

        if (ngx_win32_terminate_requested()
            || ngx_win32_quit_requested()
            || ngx_win32_exiting_requested())
        {
            ngx_log_error(NGX_LOG_NOTICE, cycle->log, 0, "exiting");
            return 0;
        }

        if (ev == WAIT_OBJECT_0 + 1 || ev == WAIT_ABANDONED_0 + 1) {
            break;
        }

        ngx_log_error(NGX_LOG_ALERT, cycle->log, 0,
                      "cache manager WaitForMultipleObjects() returned "
                      "unexpected value %ul", ev);
        return 0;
    }

    for ( ;; ) {

        if (ngx_win32_terminate_requested()
            || ngx_win32_quit_requested()
            || ngx_win32_exiting_requested())
        {
            ngx_log_error(NGX_LOG_NOTICE, cycle->log, 0, "exiting");
            break;
        }

        ngx_cache_manager_process_handler();
    }

    if (ReleaseMutex(ngx_cache_manager_mutex) == 0) {
        ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                      "ReleaseMutex() failed");
    }

    return 0;
}


static void
ngx_cache_manager_process_handler(void)
{
    u_long        ev;
    ngx_uint_t    i;
    ngx_msec_t    next, n;
    ngx_path_t  **path;

    next = 60 * 60 * 1000;

    path = ngx_cycle->paths.elts;
    for (i = 0; i < ngx_cycle->paths.nelts; i++) {

        if (path[i]->manager) {
            n = path[i]->manager(path[i]->data);

            next = (n <= next) ? n : next;

            ngx_time_update();
        }
    }

    if (next == 0) {
        next = 1;
    }

    ev = WaitForSingleObject(ngx_cache_manager_event, (u_long) next);

    if (ev != WAIT_TIMEOUT) {

        ngx_time_update();

        ngx_log_debug1(NGX_LOG_DEBUG_CORE, ngx_cycle->log, 0,
                       "cache manager WaitForSingleObject: %ul", ev);
    }
}


static ngx_thread_value_t __stdcall
ngx_cache_loader_thread(void *data)
{
    u_long        ev;
    ngx_err_t     err;
    ngx_uint_t    i;
    ngx_path_t  **path;
    ngx_cycle_t  *cycle;
    HANDLE        events[2];

    cycle = (ngx_cycle_t *) ngx_cycle;

    events[0] = ngx_cache_manager_event;
    events[1] = ngx_cache_loader_mutex;

    ev = WaitForMultipleObjects(2, events, 0, INFINITE);
    err = ngx_errno;
    ngx_time_update();

    if (ev == WAIT_FAILED) {
        ngx_log_error(NGX_LOG_ALERT, cycle->log, err,
                      "cache loader WaitForMultipleObjects() failed");
        return 0;
    }

    if (ev == WAIT_OBJECT_0) {
        return 0;
    }

    if (ev != WAIT_OBJECT_0 + 1 && ev != WAIT_ABANDONED_0 + 1) {
        ngx_log_error(NGX_LOG_ALERT, cycle->log, 0,
                      "cache loader WaitForMultipleObjects() returned "
                      "unexpected value %ul", ev);
        return 0;
    }

    ev = WaitForSingleObject(ngx_cache_manager_event, 60000);

    if (ev == WAIT_OBJECT_0) {
        goto done;
    }

    if (ev != WAIT_TIMEOUT) {
        ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                      "cache loader WaitForSingleObject() failed");
        goto done;
    }

    ngx_time_update();

    path = cycle->paths.elts;
    for (i = 0; i < cycle->paths.nelts; i++) {

        if (ngx_win32_terminate_requested()
            || ngx_win32_quit_requested()
            || ngx_win32_exiting_requested())
        {
            break;
        }

        if (path[i]->loader) {
            path[i]->loader(path[i]->data);
            ngx_time_update();
        }
    }

done:

    if (ReleaseMutex(ngx_cache_loader_mutex) == 0) {
        ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                      "ReleaseMutex() failed");
    }

    return 0;
}


static void
ngx_wait_worker_thread(ngx_tid_t tid, ngx_log_t *log)
{
    if (tid == NULL) {
        return;
    }

    if (WaitForSingleObject(tid, INFINITE) == WAIT_FAILED) {
        ngx_log_error(NGX_LOG_ALERT, log, ngx_errno,
                      "WaitForSingleObject(thread) failed");
    }

    ngx_close_handle(tid);
}


void
ngx_single_process_cycle(ngx_cycle_t *cycle)
{
    u_long     ev;
    ngx_err_t  err;
    ngx_tid_t  tid;
    HANDLE     events[5];

    ngx_console_init(cycle);

    if (ngx_create_signal_events(cycle) != NGX_OK) {
        exit(2);
    }

    if (ngx_create_thread(&tid, ngx_worker_thread, NULL, cycle->log) != 0) {
        /* fatal */
        exit(2);
    }

    events[0] = ngx_stop_event;
    events[1] = ngx_quit_event;
    events[2] = ngx_reopen_event;
    events[3] = ngx_reload_event;
    events[4] = tid;

    for ( ;; ) {
        ev = WaitForMultipleObjects(5, events, 0, INFINITE);

        err = ngx_errno;
        ngx_time_update();

        if (ev == WAIT_OBJECT_0) {
            ngx_log_error(NGX_LOG_NOTICE, cycle->log, 0, "exiting");

            if (ResetEvent(ngx_stop_event) == 0) {
                ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                              "ResetEvent(\"%s\") failed",
                              ngx_stop_event_name);
            }

            ngx_win32_set_terminate(1);
            break;
        }

        if (ev == WAIT_OBJECT_0 + 1) {
            ngx_log_error(NGX_LOG_NOTICE, cycle->log, 0, "shutting down");

            if (ResetEvent(ngx_quit_event) == 0) {
                ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                              "ResetEvent(\"%s\") failed",
                              ngx_quit_event_name);
            }

            ngx_win32_set_quit(1);
            break;
        }

        if (ev == WAIT_OBJECT_0 + 2) {
            if (ResetEvent(ngx_reopen_event) == 0) {
                ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                              "ResetEvent(\"%s\") failed",
                              ngx_reopen_event_name);
            }

            ngx_win32_set_reopen(1);
            continue;
        }

        if (ev == WAIT_OBJECT_0 + 3) {
            if (ResetEvent(ngx_reload_event) == 0) {
                ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                              "ResetEvent(\"%s\") failed",
                              ngx_reload_event_name);
            }

            ngx_log_error(NGX_LOG_ERR, cycle->log, 0,
                          "reload is not supported with "
                          "master_process off");
            continue;
        }

        if (ev == WAIT_OBJECT_0 + 4) {
            ngx_log_error(NGX_LOG_ALERT, cycle->log, 0,
                          "worker thread exited unexpectedly");
            ngx_win32_set_terminate(1);
            break;
        }

        if (ev == WAIT_FAILED) {
            ngx_log_error(NGX_LOG_ALERT, cycle->log, err,
                          "WaitForMultipleObjects() failed");
            ngx_win32_set_terminate(1);
            break;
        }
    }

    if (WaitForSingleObject(tid, INFINITE) == WAIT_FAILED) {
        ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                      "WaitForSingleObject() failed");
    }

    ngx_close_handle(tid);

    ngx_worker_process_exit((ngx_cycle_t *) ngx_cycle);
}


ngx_int_t
ngx_os_signal_process(ngx_cycle_t *cycle, char *sig, ngx_pid_t pid)
{
    HANDLE     ev;
    ngx_int_t  rc;
    char       evn[NGX_PROCESS_SYNC_NAME];

    ngx_sprintf((u_char *) evn, "Global\\ngx_%s_%P%Z", sig, pid);

    ev = OpenEvent(EVENT_MODIFY_STATE, 0, evn);
    if (ev == NULL) {
        ngx_log_error(NGX_LOG_ERR, cycle->log, ngx_errno,
                      "OpenEvent(\"%s\") failed", evn);
        return 1;
    }

    if (SetEvent(ev) == 0) {
        ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                      "SetEvent(\"%s\") failed", evn);
        rc = 1;

    } else {
        rc = 0;
    }

    ngx_close_handle(ev);

    return rc;
}


void
ngx_close_handle(HANDLE h)
{
    if (CloseHandle(h) == 0) {
        ngx_log_error(NGX_LOG_ALERT, ngx_cycle->log, ngx_errno,
                      "CloseHandle(%p) failed", h);
    }
}
