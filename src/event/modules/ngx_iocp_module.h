
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#ifndef _NGX_IOCP_MODULE_H_INCLUDED_
#define _NGX_IOCP_MODULE_H_INCLUDED_


typedef enum {
    NGX_IOCP_OP_ACCEPT = 0,
    NGX_IOCP_OP_CONNECT,
    NGX_IOCP_OP_READ_NOTIFY,
    NGX_IOCP_OP_WRITE_NOTIFY,
    NGX_IOCP_OP_RECV,
    NGX_IOCP_OP_RECV_CHAIN,
    NGX_IOCP_OP_SEND,
    NGX_IOCP_OP_SEND_CHAIN,
    NGX_IOCP_OP_UDP_RECV,
    NGX_IOCP_OP_UDP_SEND,
    NGX_IOCP_OP_FILE_READ,
    NGX_IOCP_OP_FILE_WRITE,
    NGX_IOCP_OP_TRANSMIT,
    NGX_IOCP_OP_MAX
} ngx_iocp_op_type_e;


typedef void (*ngx_iocp_completion_pt)(ngx_iocp_op_t *op);
typedef void (*ngx_iocp_cleanup_pt)(ngx_iocp_op_t *op);
typedef void (*ngx_iocp_owner_cleanup_pt)(ngx_iocp_owner_t *owner);


struct ngx_iocp_owner_s {
    HANDLE                  handle;
    ngx_queue_t             operations;
    ngx_queue_t             queue;
    ngx_iocp_owner_t       *port_owner;
    ngx_connection_t       *connection;
    ngx_pool_t             *pool;
    void                   *data;
    ngx_iocp_owner_cleanup_pt cleanup;
    ngx_log_t              *log;
    ngx_log_t               safe_log;
    ngx_uint_t              generation;
    ngx_uint_t              pending;
    ngx_uint_t              arming;
    ngx_uint_t              children;
    unsigned                socket:1;
    unsigned                associated:1;
    unsigned                closing:1;
    unsigned                shared:1;
    unsigned                skip_completion:1;
    unsigned                udp_pktinfo:1;
    unsigned                udp_connreset:1;
    LPFN_ACCEPTEX           acceptex;
    LPFN_GETACCEPTEXSOCKADDRS getacceptexsockaddrs;
    LPFN_CONNECTEX           connectex;
    LPFN_TRANSMITFILE        transmitfile;
    LPFN_TRANSMITPACKETS     transmitpackets;
    LPFN_WSARECVMSG          recvmsg;
    LPFN_WSASENDMSG          sendmsg;
};


struct ngx_iocp_op_s {
    OVERLAPPED              overlapped;
    ngx_queue_t             queue;
    ngx_iocp_owner_t       *owner;
    ngx_iocp_owner_t       *completion_owner;
    ngx_event_t            *event;
    ngx_pool_t             *owner_pool;
    ngx_pool_t             *data_pool;
    ngx_iocp_completion_pt  handler;
    ngx_iocp_cleanup_pt     cleanup;
    DWORD                   bytes;
    DWORD                   expected;
    DWORD                   flags;
    ngx_err_t               error;
    size_t                  allocation_size;
    ngx_uint_t              generation;
    ngx_uint_t              type;
    unsigned                linked:1;
    unsigned                completing:1;
    unsigned                cancel_requested:1;
    unsigned                prepared:1;
    unsigned                owner_held:1;
    unsigned                data_held:1;
    volatile LONG           state;
};


typedef struct {
    ngx_int_t   threads;
    ngx_int_t   post_acceptex;
    ngx_flag_t  acceptex_read;
    ngx_int_t   udp_receives;
} ngx_iocp_conf_t;


ngx_iocp_owner_t *ngx_iocp_create_owner(HANDLE handle,
    ngx_connection_t *c, ngx_log_t *log, ngx_uint_t socket,
    ngx_uint_t shared);
ngx_iocp_owner_t *ngx_iocp_create_shared_owner(ngx_connection_t *c,
    ngx_iocp_owner_t *port_owner);
ngx_int_t ngx_iocp_associate(ngx_iocp_owner_t *owner);
void ngx_iocp_load_extensions(ngx_iocp_owner_t *owner);
ngx_int_t ngx_iocp_add_connection(ngx_connection_t *c);
ngx_int_t ngx_iocp_enable_skip_completion(ngx_connection_t *c);
ngx_int_t ngx_iocp_post_read(ngx_event_t *rev);
ngx_int_t ngx_iocp_post_write(ngx_event_t *wev);
void ngx_iocp_close_connection(ngx_connection_t *c);
void ngx_iocp_close_owner(ngx_iocp_owner_t *owner);
void ngx_iocp_free_event_buffer(ngx_event_t *ev);

ngx_iocp_op_t *ngx_iocp_op_create(size_t size, ngx_iocp_owner_t *owner,
    ngx_event_t *event, ngx_pool_t *data_pool, ngx_uint_t type,
    ngx_iocp_completion_pt handler, ngx_iocp_cleanup_pt cleanup);
ngx_iocp_op_t *ngx_iocp_op_prepare(size_t size, ngx_iocp_owner_t *owner,
    ngx_event_t *event, ngx_pool_t *data_pool, ngx_uint_t type,
    ngx_iocp_completion_pt handler, ngx_iocp_cleanup_pt cleanup);
ngx_int_t ngx_iocp_op_arm(ngx_iocp_op_t *op, ngx_uint_t pending);
void ngx_iocp_op_abort(ngx_iocp_op_t *op);
void ngx_iocp_event_complete(ngx_iocp_op_t *op);

HANDLE ngx_iocp_port(void);


extern ngx_module_t  ngx_iocp_module;


#endif /* _NGX_IOCP_MODULE_H_INCLUDED_ */
