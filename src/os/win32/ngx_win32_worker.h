
/*
 * Copyright (C) Nginx, Inc.
 */


#ifndef _NGX_WIN32_WORKER_H_INCLUDED_
#define _NGX_WIN32_WORKER_H_INCLUDED_


#include <ngx_config.h>
#include <ngx_core.h>


struct msghdr;


#define NGX_WIN32_BOOTSTRAP_MAGIC       0x4e475857U
#define NGX_WIN32_BOOTSTRAP_VERSION     1

#define NGX_WIN32_WORKER_SLOT_ENV       "ngx_worker_slot"
#define NGX_WIN32_WORKER_GENERATION_ENV "ngx_worker_generation"
#define NGX_WIN32_WORKER_PIPE_ENV       "ngx_worker_pipe"
#define NGX_WIN32_WORKER_ROLE_ENV       "ngx_worker_role"

#define NGX_WIN32_WORKER_ROLE           1
#define NGX_WIN32_ROUTER_ROLE           2

#define NGX_WIN32_BOOTSTRAP_TIMEOUT     30000

#define NGX_WIN32_CHANNEL_MAGIC         0x4e475843U
#define NGX_WIN32_CHANNEL_VERSION       1
#define NGX_WIN32_CHANNEL_MAX_MESSAGE   (128 * 1024)
#define NGX_WIN32_CHANNEL_UDP_MAX        65535

#define NGX_WIN32_QUIC_ROUTE_CID_LEN     20
#define NGX_WIN32_QUIC_ROUTE_MAGIC       0x4e57


typedef enum {
    NGX_WIN32_BOOTSTRAP_HELLO = 1,
    NGX_WIN32_BOOTSTRAP_LISTENERS,
    NGX_WIN32_BOOTSTRAP_IMPORT_OK,
    NGX_WIN32_BOOTSTRAP_CONTROL_READY,
    NGX_WIN32_BOOTSTRAP_READY,
    NGX_WIN32_BOOTSTRAP_FAIL
} ngx_win32_bootstrap_type_e;


typedef enum {
    NGX_WIN32_CHANNEL_ACCEPT = 1,
    NGX_WIN32_CHANNEL_ACCEPT_ACK,
    NGX_WIN32_CHANNEL_UDP_RECV,
    NGX_WIN32_CHANNEL_UDP_SEND
} ngx_win32_channel_type_e;


typedef struct {
    uint32_t    magic;
    uint32_t    version;
    uint32_t    type;
    uint32_t    length;
    uint32_t    slot;
    uint32_t    generation;
    uint32_t    pid;
    uint32_t    status;
} ngx_win32_bootstrap_header_t;


typedef struct {
    uint32_t       index;
    uint32_t       flags;
    WSAPROTOCOL_INFO info;
} ngx_win32_bootstrap_listener_t;


typedef struct {
    uint32_t       count;
    uint32_t       flags;
    uint64_t       route_key[2];
} ngx_win32_bootstrap_listeners_t;


#define NGX_WIN32_LISTENERS_ROUTED      0x00000001


typedef struct {
    uint32_t       magic;
    uint32_t       version;
    uint32_t       type;
    uint32_t       length;
    uint32_t       slot;
    uint32_t       generation;
    uint32_t       listener;
    uint32_t       status;
    uint64_t       id;
} ngx_win32_channel_header_t;


typedef struct {
    ngx_win32_channel_header_t header;
    uint32_t       local_socklen;
    uint32_t       remote_socklen;
    WSAPROTOCOL_INFO info;
    ngx_sockaddr_t  local;
    ngx_sockaddr_t  remote;
} ngx_win32_channel_accept_t;


typedef struct {
    ngx_win32_channel_header_t header;
    uint32_t       local_socklen;
    uint32_t       remote_socklen;
    uint32_t       data_len;
    uint32_t       flags;
    ngx_sockaddr_t  local;
    ngx_sockaddr_t  remote;
} ngx_win32_channel_udp_t;


struct ngx_win32_worker_bootstrap_s {
    HANDLE         pipe;
    ngx_uint_t     slot;
    ngx_uint_t     generation;
    ngx_uint_t     role;
    ngx_uint_t     imported;
    ngx_uint_t     expected;
    ngx_uint_t     routed;
    u_char         name[NGX_PROCESS_SYNC_NAME];
};


extern ngx_uint_t ngx_win32_worker_slot;
extern ngx_uint_t ngx_win32_worker_generation;
extern ngx_uint_t ngx_win32_worker_role;
extern ngx_uint_t ngx_win32_worker_bootstrap_active;
extern ngx_uint_t ngx_win32_worker_expected_listeners;
extern ngx_uint_t ngx_win32_worker_routed;


ngx_int_t ngx_win32_worker_bootstrap_init(ngx_log_t *log);
ngx_int_t ngx_win32_worker_import_listeners(ngx_cycle_t *cycle);
ngx_int_t ngx_win32_worker_validate_listeners(ngx_cycle_t *cycle);
ngx_int_t ngx_win32_worker_send_status(ngx_uint_t type, ngx_uint_t status);
ngx_int_t ngx_win32_worker_channel_init(ngx_cycle_t *cycle);
void ngx_win32_worker_channel_done(void);
ssize_t ngx_win32_worker_udp_sendmsg(ngx_connection_t *c,
    struct msghdr *msg, int flags);
void ngx_win32_worker_quic_route_id(u_char *id, size_t len);
ngx_int_t ngx_win32_quic_route_decode(u_char *id, size_t len,
    ngx_uint_t *slot, ngx_uint_t *generation);

ngx_int_t ngx_win32_master_create_bootstrap(ngx_cycle_t *cycle,
    ngx_win32_worker_bootstrap_t *bootstrap, ngx_uint_t slot,
    ngx_uint_t generation, ngx_uint_t role);
ngx_int_t ngx_win32_master_bootstrap_worker(ngx_cycle_t *cycle,
    ngx_win32_worker_bootstrap_t *bootstrap, ngx_pid_t pid, HANDLE process);
ngx_int_t ngx_win32_master_wait_status(ngx_cycle_t *cycle,
    ngx_win32_worker_bootstrap_t *bootstrap, ngx_uint_t type, HANDLE process);
void ngx_win32_master_close_bootstrap(ngx_win32_worker_bootstrap_t *bootstrap);

ngx_int_t ngx_win32_accept_mutex_init(ngx_cycle_t *cycle);
ngx_int_t ngx_win32_accept_mutex_trylock(ngx_cycle_t *cycle);
void ngx_win32_accept_mutex_unlock(void);
void ngx_win32_accept_mutex_done(void);


#endif /* _NGX_WIN32_WORKER_H_INCLUDED_ */
