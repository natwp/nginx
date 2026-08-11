
/*
 * Copyright (C) Nginx, Inc.
 */


#ifndef _NGX_WIN32_ROUTER_H_INCLUDED_
#define _NGX_WIN32_ROUTER_H_INCLUDED_


#include <ngx_config.h>
#include <ngx_core.h>


ngx_uint_t ngx_win32_router_required(ngx_cycle_t *cycle);
ngx_int_t ngx_win32_router_start(ngx_cycle_t *cycle, ngx_uint_t generation);
ngx_int_t ngx_win32_router_pause(ngx_cycle_t *cycle);
ngx_int_t ngx_win32_router_drain(ngx_cycle_t *cycle);
ngx_int_t ngx_win32_router_resume(ngx_cycle_t *cycle,
    ngx_uint_t generation);
ngx_uint_t ngx_win32_router_failed(void);
void ngx_win32_router_update_workers(ngx_cycle_t *cycle,
    ngx_uint_t generation);
void ngx_win32_router_stop(ngx_cycle_t *cycle);


#endif /* _NGX_WIN32_ROUTER_H_INCLUDED_ */
