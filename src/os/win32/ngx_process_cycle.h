
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#ifndef _NGX_PROCESS_CYCLE_H_INCLUDED_
#define _NGX_PROCESS_CYCLE_H_INCLUDED_


#include <ngx_config.h>
#include <ngx_core.h>


#define NGX_PROCESS_SINGLE     0
#define NGX_PROCESS_MASTER     1
#define NGX_PROCESS_SIGNALLER  2
#define NGX_PROCESS_WORKER     3


void ngx_master_process_cycle(ngx_cycle_t *cycle);
void ngx_single_process_cycle(ngx_cycle_t *cycle);
void ngx_close_handle(HANDLE h);


extern ngx_uint_t      ngx_process;
extern ngx_uint_t      ngx_worker;
extern ngx_pid_t       ngx_pid;
extern volatile ngx_uint_t  ngx_exiting;

extern volatile LONG   ngx_quit;
extern volatile LONG   ngx_terminate;
extern volatile LONG   ngx_reopen;

/*
 * Lifecycle flags are read in the event hot path.  Aligned volatile reads
 * remain cheap, while interlocked stores publish changes from control threads.
 */

static ngx_inline ngx_uint_t
ngx_win32_terminate_requested(void)
{
    return ngx_terminate != 0;
}

static ngx_inline void
ngx_win32_set_terminate(LONG value)
{
    (void) InterlockedExchange(&ngx_terminate, value);
}

static ngx_inline ngx_uint_t
ngx_win32_quit_requested(void)
{
    return ngx_quit != 0;
}

static ngx_inline void
ngx_win32_set_quit(LONG value)
{
    (void) InterlockedExchange(&ngx_quit, value);
}

static ngx_inline ngx_uint_t
ngx_win32_reopen_requested(void)
{
    return ngx_reopen != 0;
}

static ngx_inline void
ngx_win32_set_reopen(LONG value)
{
    (void) InterlockedExchange(&ngx_reopen, value);
}

static ngx_inline ngx_uint_t
ngx_win32_exiting_requested(void)
{
    return ngx_exiting != 0;
}

static ngx_inline void
ngx_win32_set_exiting(ngx_uint_t value)
{
#if (NGX_PTR_SIZE == 8)
    (void) InterlockedExchangePointer((PVOID volatile *) &ngx_exiting,
                                      (PVOID) value);
#else
    (void) InterlockedExchange((LONG *) &ngx_exiting, (LONG) value);
#endif
}

extern ngx_uint_t      ngx_inherited;
extern ngx_pid_t       ngx_new_binary;


extern HANDLE          ngx_master_process_event;
extern char            ngx_master_process_event_name[];


#endif /* _NGX_PROCESS_CYCLE_H_INCLUDED_ */
