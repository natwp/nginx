
/*
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>
#include <ngx_win32_router.h>
#include <ngx_win32_worker.h>

#if (NGX_QUIC)
#include <ngx_event_quic.h>
#endif


#define NGX_WIN32_ROUTER_PAUSE_TIMEOUT 30000
#define NGX_WIN32_ROUTER_GRACE_TIMEOUT 5000
#define NGX_WIN32_ROUTER_ACK_TIMEOUT   10000
#define NGX_WIN32_ROUTER_ACCEPTS       32
#define NGX_WIN32_ROUTER_BATCH         64
#define NGX_WIN32_ROUTER_UDP_SIZE      65535
#define NGX_WIN32_ROUTER_CONTROL_SIZE  512
#define NGX_WIN32_ROUTER_FLOW_TIMEOUT  600000
#define NGX_WIN32_ROUTER_FLOW_BUCKETS  4096
#define NGX_WIN32_ROUTER_FLOW_MAX      65536
#define NGX_WIN32_ROUTER_ACCEPT_BUCKETS 1024
#define NGX_WIN32_ROUTER_CHANNEL_LIMIT (8 * 1024 * 1024)
#define NGX_WIN32_ROUTER_CHANNEL_MESSAGES 4096
#define NGX_WIN32_ROUTER_UDP_SEND_LIMIT (8 * 1024 * 1024)
#define NGX_WIN32_ROUTER_UDP_SENDS     4096

#define NGX_WIN32_ROUTER_FLOW_TUPLE    1
#define NGX_WIN32_ROUTER_FLOW_QUIC     2


typedef struct ngx_win32_router_listener_s ngx_win32_router_listener_t;
typedef struct ngx_win32_router_worker_s ngx_win32_router_worker_t;
typedef struct ngx_win32_router_control_s ngx_win32_router_control_t;


typedef struct {
    OVERLAPPED                  overlapped;
    ngx_uint_t                  type;
    ngx_win32_router_listener_t *listener;
} ngx_win32_router_op_t;


#define NGX_WIN32_ROUTER_OP_ACCEPT    1
#define NGX_WIN32_ROUTER_OP_UDP_RECV  2
#define NGX_WIN32_ROUTER_OP_UDP_SEND  3
#define NGX_WIN32_ROUTER_OP_CONTROL   4
#define NGX_WIN32_ROUTER_OP_PIPE_READ 5
#define NGX_WIN32_ROUTER_OP_PIPE_WRITE 6


#define NGX_WIN32_ROUTER_CONTROL_PAUSE  1
#define NGX_WIN32_ROUTER_CONTROL_RESUME 2
#define NGX_WIN32_ROUTER_CONTROL_UPDATE 3
#define NGX_WIN32_ROUTER_CONTROL_DRAIN  4
#define NGX_WIN32_ROUTER_CONTROL_STOP   5


typedef struct {
    ngx_win32_router_op_t       op;
    ngx_queue_t                  queue;
    ngx_queue_t                  id_queue;
    ngx_socket_t                 socket;
    ngx_pid_t                    target;
    uint64_t                     id;
    ULONGLONG                    deadline;
    u_char                       buffer[2 * (sizeof(ngx_sockaddr_t) + 16)];
} ngx_win32_router_accept_t;


typedef struct {
    ngx_win32_router_op_t       op;
    WSAMSG                       msg;
    WSABUF                       wsabuf;
    ngx_sockaddr_t               remote;
    int                          remote_len;
    DWORD                        flags;
    u_char                       control[NGX_WIN32_ROUTER_CONTROL_SIZE];
    u_char                       data[NGX_WIN32_ROUTER_UDP_SIZE];
    unsigned                     recvmsg:1;
} ngx_win32_router_udp_recv_t;


typedef struct {
    ngx_win32_router_op_t       op;
    WSAMSG                       msg;
    WSABUF                       wsabuf;
    ngx_sockaddr_t               remote;
    DWORD                        expected;
    DWORD                        size;
    u_char                       control[NGX_WIN32_ROUTER_CONTROL_SIZE];
    u_char                       data[1];
} ngx_win32_router_udp_send_t;


struct ngx_win32_router_listener_s {
    ngx_queue_t     queue;
    ngx_socket_t    socket;
    ngx_uint_t      index;
    ngx_uint_t      pending;
    ngx_uint_t      seen;
    int             family;
    int             type;
    int             protocol;
    socklen_t       socklen;
    ngx_sockaddr_t  sockaddr;
    unsigned        wildcard:1;
    unsigned        quic:1;
    LPFN_ACCEPTEX   acceptex;
    LPFN_WSARECVMSG recvmsg;
    LPFN_WSASENDMSG sendmsg;
    u_char          addr_text[NGX_SOCKADDR_STRLEN];
};


typedef struct {
    ngx_win32_router_op_t  op;
    ngx_win32_router_worker_t *worker;
    u_char                *data;
    size_t                 size;
    size_t                 offset;
    u_char                 header[sizeof(ngx_win32_channel_header_t)];
} ngx_win32_router_pipe_read_t;


typedef struct {
    ngx_win32_router_op_t  op;
    ngx_queue_t            queue;
    ngx_win32_router_worker_t *worker;
    size_t                 size;
    size_t                 offset;
    uint64_t               accept_id;
    u_char                *data;
} ngx_win32_router_pipe_write_t;


struct ngx_win32_router_worker_s {
    ngx_pid_t                    pid;
    ngx_uint_t                   slot;
    ngx_uint_t                   generation;
    HANDLE                       pipe;
    ngx_queue_t                  writes;
    ngx_win32_router_pipe_read_t read;
    ngx_uint_t                   read_pending;
    ngx_uint_t                   write_pending;
    ngx_uint_t                   ready;
    ngx_uint_t                   active;
    ngx_uint_t                   seen;
    ngx_uint_t                   removing;
    ngx_uint_t                   failed;
    ngx_uint_t                   queued_messages;
    size_t                       queued_bytes;
};


typedef struct {
    ngx_pid_t   pid;
    ngx_uint_t  slot;
    ngx_uint_t  generation;
    HANDLE      pipe;
    ngx_uint_t  exiting;
} ngx_win32_router_worker_snapshot_t;


struct ngx_win32_router_control_s {
    ngx_win32_router_op_t              op;
    volatile LONG                      refs;
    volatile LONG                      abandoned;
    HANDLE                             event;
    ngx_uint_t                         command;
    ngx_uint_t                         generation;
    ngx_cycle_t                       *cycle;
    ngx_int_t                          status;
    ngx_uint_t                         worker_n;
    ngx_win32_router_worker_snapshot_t workers[NGX_MAX_PROCESSES];
};


typedef struct {
    uint32_t       listener;
    uint16_t       type;
    uint16_t       local_family;
    uint16_t       remote_family;
    uint16_t       local_port;
    uint16_t       remote_port;
    uint16_t       cid_len;
    uint32_t       local_scope;
    uint32_t       remote_scope;
    u_char         local_addr[16];
    u_char         remote_addr[16];
    u_char         cid[NGX_WIN32_QUIC_ROUTE_CID_LEN];
} ngx_win32_router_flow_key_t;


typedef struct {
    ngx_queue_t                    bucket;
    ngx_queue_t                    expires;
    ngx_win32_router_flow_key_t    key;
    uint32_t                       hash;
    ngx_pid_t                      pid;
    ngx_uint_t                     slot;
    ngx_uint_t                     generation;
    ULONGLONG                      deadline;
} ngx_win32_router_flow_t;


static ngx_thread_value_t __stdcall ngx_win32_router_thread(void *data);
static ngx_int_t ngx_win32_router_command(ngx_cycle_t *cycle,
    ngx_uint_t command, ngx_uint_t generation, ngx_uint_t workers);
static void ngx_win32_router_complete_control(
    ngx_win32_router_control_t *control);
static void ngx_win32_router_release_control(
    ngx_win32_router_control_t *control);
static void ngx_win32_router_process_control(
    ngx_win32_router_control_t *control);
static ngx_uint_t ngx_win32_router_control_ready(void);
static void ngx_win32_router_cancel_io(ngx_uint_t streams_only);
static ngx_err_t ngx_win32_router_result(ngx_win32_router_op_t *op,
    OVERLAPPED_ENTRY *entry, DWORD *bytes);
static ngx_int_t ngx_win32_router_configure(ngx_cycle_t *cycle);
static ngx_win32_router_listener_t *ngx_win32_router_find_listener(
    ngx_socket_t socket);
static ngx_win32_router_listener_t *ngx_win32_router_find_listener_index(
    ngx_uint_t index);
static ngx_win32_router_listener_t *ngx_win32_router_find_udp_listener(
    ngx_uint_t index, struct sockaddr *local, socklen_t local_socklen);
static ngx_uint_t ngx_win32_router_listener_matches(
    ngx_win32_router_listener_t *listener, struct sockaddr *local,
    socklen_t local_socklen);
static ngx_int_t ngx_win32_router_add_listener(ngx_cycle_t *cycle,
    ngx_listening_t *ls, ngx_uint_t index);
static ngx_int_t ngx_win32_router_post_accept(
    ngx_win32_router_listener_t *listener);
static ngx_int_t ngx_win32_router_post_udp_recv(
    ngx_win32_router_listener_t *listener);
static void ngx_win32_router_complete_accept(
    ngx_win32_router_accept_t *accept, DWORD bytes, ngx_err_t error);
static void ngx_win32_router_complete_udp_recv(
    ngx_win32_router_udp_recv_t *op, DWORD bytes, ngx_err_t error);
static void ngx_win32_router_complete_udp_send(
    ngx_win32_router_udp_send_t *op, DWORD bytes, ngx_err_t error);
static ngx_int_t ngx_win32_router_dispatch_accept(
    ngx_win32_router_accept_t *accept);
static ngx_int_t ngx_win32_router_dispatch_udp(
    ngx_win32_router_udp_recv_t *op, DWORD bytes,
    struct sockaddr *local, socklen_t local_socklen);
static ngx_int_t ngx_win32_router_apply_workers(
    ngx_win32_router_control_t *control);
static ngx_int_t ngx_win32_router_post_pipe_read(
    ngx_win32_router_worker_t *worker);
static void ngx_win32_router_complete_pipe_read(
    ngx_win32_router_pipe_read_t *read, DWORD bytes, ngx_err_t error);
static ngx_int_t ngx_win32_router_handle_channel(
    ngx_win32_router_worker_t *worker, u_char *message, size_t size);
static ngx_int_t ngx_win32_router_queue_write(
    ngx_win32_router_worker_t *worker, const void *data, size_t size,
    uint64_t accept_id);
static ngx_win32_router_pipe_write_t *ngx_win32_router_alloc_write(
    ngx_win32_router_worker_t *worker, size_t size, uint64_t accept_id);
static void ngx_win32_router_queue_write_node(
    ngx_win32_router_worker_t *worker,
    ngx_win32_router_pipe_write_t *write);
static void ngx_win32_router_free_pipe_write(
    ngx_win32_router_pipe_write_t *write);
static ngx_int_t ngx_win32_router_post_pipe_write(
    ngx_win32_router_worker_t *worker);
static void ngx_win32_router_complete_pipe_write(
    ngx_win32_router_pipe_write_t *write, DWORD bytes, ngx_err_t error);
static ngx_uint_t ngx_win32_router_pipe_ended(ngx_err_t error);
static void ngx_win32_router_drop_writes(ngx_win32_router_worker_t *worker,
    ngx_uint_t pending);
static void ngx_win32_router_fail_accept(uint64_t id, ngx_pid_t pid);
static ngx_int_t ngx_win32_router_send_udp(
    ngx_win32_router_worker_t *worker, ngx_win32_channel_udp_t *udp,
    u_char *data);
static ngx_int_t ngx_win32_router_sockaddr_valid(struct sockaddr *sockaddr,
    socklen_t socklen);
static ngx_int_t ngx_win32_router_flow_key(
    ngx_win32_router_listener_t *listener, u_char *data, size_t size,
    struct sockaddr *local, socklen_t local_socklen,
    struct sockaddr *remote, socklen_t remote_socklen,
    ngx_win32_router_flow_key_t *key, uint32_t *hash,
    ngx_uint_t *route_slot, ngx_uint_t *route_generation);
static void ngx_win32_router_flow_addr(struct sockaddr *sockaddr,
    u_char *addr, uint16_t *port, uint32_t *scope);
static ngx_win32_router_flow_t *ngx_win32_router_find_flow(
    ngx_win32_router_flow_key_t *key, uint32_t hash);
static ngx_int_t ngx_win32_router_set_flow(
    ngx_win32_router_flow_key_t *key, uint32_t hash,
    ngx_win32_router_worker_t *worker);
static ngx_win32_router_worker_t *ngx_win32_router_find_worker(
    ngx_pid_t pid, ngx_uint_t slot, ngx_uint_t generation);
static ngx_win32_router_worker_t *ngx_win32_router_find_quic_worker(
    ngx_uint_t slot, ngx_uint_t generation);
static void ngx_win32_router_rebuild_ready_workers(void);
static ngx_uint_t ngx_win32_router_worker_eligible(
    ngx_win32_router_worker_t *worker);
static ngx_win32_router_worker_t *ngx_win32_router_select_worker(
    uint32_t hash, ngx_uint_t round_robin);
static void ngx_win32_router_expire_accepts(void);
static void ngx_win32_router_expire_flows(void);
static void ngx_win32_router_finish_accept(
    ngx_win32_router_accept_t *accept);
static void ngx_win32_router_free_listeners(void);
static void ngx_win32_router_free_flows(void);


static HANDLE           ngx_win32_router_port;
static HANDLE           ngx_win32_router_thread_handle;
static ngx_log_t       *ngx_win32_router_log;
static ngx_queue_t      ngx_win32_router_listeners;
static ngx_queue_t      ngx_win32_router_handoffs;
static ngx_queue_t      ngx_win32_router_accept_buckets[
                                               NGX_WIN32_ROUTER_ACCEPT_BUCKETS];
static ngx_uint_t       ngx_win32_router_stream_accepting;
static ngx_uint_t       ngx_win32_router_udp_accepting;
static ngx_uint_t       ngx_win32_router_new_flows;
static ngx_uint_t       ngx_win32_router_channels;
static ngx_uint_t       ngx_win32_router_stopping;
static ngx_uint_t       ngx_win32_router_pending;
static ngx_uint_t       ngx_win32_router_channel_pending;
static ngx_uint_t       ngx_win32_router_udp_sends;
static size_t           ngx_win32_router_udp_send_bytes;
static ngx_uint_t       ngx_win32_router_next_worker;
static uint64_t         ngx_win32_router_next_id;
static ngx_uint_t       ngx_win32_router_initialized;
static ngx_uint_t       ngx_win32_router_broken;
static ngx_uint_t       ngx_win32_router_generation;
static ngx_win32_router_worker_t ngx_win32_router_workers[NGX_MAX_PROCESSES];
static ngx_win32_router_worker_t *ngx_win32_router_ready_workers[
                                                          NGX_MAX_PROCESSES];
static ngx_uint_t       ngx_win32_router_ready_worker_n;
static ngx_win32_router_control_t *ngx_win32_router_pending_control;
static ngx_queue_t      ngx_win32_router_flow_buckets[
                                                 NGX_WIN32_ROUTER_FLOW_BUCKETS];
static ngx_queue_t      ngx_win32_router_flows;
static ngx_uint_t       ngx_win32_router_flow_n;


static GUID  ngx_win32_router_acceptex_guid = WSAID_ACCEPTEX;
static GUID  ngx_win32_router_recvmsg_guid = WSAID_WSARECVMSG;
static GUID  ngx_win32_router_sendmsg_guid = WSAID_WSASENDMSG;


ngx_uint_t
ngx_win32_router_required(ngx_cycle_t *cycle)
{
    ngx_core_conf_t   *ccf;
    ngx_event_conf_t  *ecf;

    if (cycle == NULL || cycle->conf_ctx == NULL) {
        return 0;
    }

    ccf = (ngx_core_conf_t *) ngx_get_conf(cycle->conf_ctx, ngx_core_module);
    ecf = ngx_event_get_conf(cycle->conf_ctx, ngx_event_core_module);

    /*
     * A listening socket can be associated with only one IOCP.  A reload
     * overlaps worker generations, so the master must retain ownership of
     * IOCP listeners even when there is only one worker.
     */

    return ccf && ecf && ccf->master && ccf->worker_processes > 0
           && ecf->use == ngx_iocp_module.ctx_index;
}


ngx_uint_t
ngx_win32_router_failed(void)
{
    return ngx_win32_router_broken;
}


ngx_int_t
ngx_win32_router_start(ngx_cycle_t *cycle, ngx_uint_t generation)
{
    ngx_tid_t   tid;
    ngx_uint_t  i;

    if (!ngx_win32_router_required(cycle)) {
        return NGX_OK;
    }

    if (ngx_win32_router_initialized) {
        return ngx_win32_router_resume(cycle, generation);
    }

    ngx_win32_router_log = cycle->log;
    ngx_queue_init(&ngx_win32_router_listeners);
    ngx_queue_init(&ngx_win32_router_handoffs);
    ngx_queue_init(&ngx_win32_router_flows);

    for (i = 0; i < NGX_WIN32_ROUTER_ACCEPT_BUCKETS; i++) {
        ngx_queue_init(&ngx_win32_router_accept_buckets[i]);
    }

    for (i = 0; i < NGX_WIN32_ROUTER_FLOW_BUCKETS; i++) {
        ngx_queue_init(&ngx_win32_router_flow_buckets[i]);
    }

    ngx_win32_router_flow_n = 0;

    ngx_memzero(ngx_win32_router_workers,
                sizeof(ngx_win32_router_workers));
    ngx_win32_router_ready_worker_n = 0;

    for (i = 0; i < NGX_MAX_PROCESSES; i++) {
        ngx_queue_init(&ngx_win32_router_workers[i].writes);
        ngx_win32_router_workers[i].read.worker =
            &ngx_win32_router_workers[i];
        ngx_win32_router_workers[i].read.op.type =
            NGX_WIN32_ROUTER_OP_PIPE_READ;
    }

    ngx_win32_router_port = CreateIoCompletionPort(INVALID_HANDLE_VALUE,
                                                    NULL, 0, 1);
    if (ngx_win32_router_port == NULL) {
        ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_errno,
                      "CreateIoCompletionPort(network router) failed");
        goto failed;
    }

    ngx_win32_router_initialized = 1;
    ngx_win32_router_stream_accepting = 0;
    ngx_win32_router_udp_accepting = 0;
    ngx_win32_router_new_flows = 0;
    ngx_win32_router_channels = 0;
    ngx_win32_router_stopping = 0;
    ngx_win32_router_broken = 0;
    ngx_win32_router_pending = 0;
    ngx_win32_router_channel_pending = 0;
    ngx_win32_router_udp_sends = 0;
    ngx_win32_router_udp_send_bytes = 0;
    ngx_win32_router_next_id = 1;
    ngx_win32_router_pending_control = NULL;

    if (ngx_create_thread(&tid, ngx_win32_router_thread, NULL, cycle->log)
        != 0)
    {
        goto failed;
    }

    ngx_win32_router_thread_handle = tid;

    if (ngx_win32_router_resume(cycle, generation) != NGX_OK) {
        goto failed;
    }

    ngx_log_error(NGX_LOG_NOTICE, cycle->log, 0,
                  "started Win32 IOCP network router");

    return NGX_OK;

failed:

    ngx_win32_router_stop(cycle);
    return NGX_ERROR;
}


ngx_int_t
ngx_win32_router_pause(ngx_cycle_t *cycle)
{
    if (!ngx_win32_router_initialized) {
        return NGX_OK;
    }

    return ngx_win32_router_command(cycle, NGX_WIN32_ROUTER_CONTROL_PAUSE,
                                    ngx_win32_router_generation, 0);
}


ngx_int_t
ngx_win32_router_drain(ngx_cycle_t *cycle)
{
    if (!ngx_win32_router_initialized) {
        return NGX_OK;
    }

    return ngx_win32_router_command(cycle, NGX_WIN32_ROUTER_CONTROL_DRAIN,
                                    ngx_win32_router_generation, 1);
}


ngx_int_t
ngx_win32_router_resume(ngx_cycle_t *cycle, ngx_uint_t generation)
{
    if (!ngx_win32_router_initialized) {
        return NGX_ERROR;
    }

    return ngx_win32_router_command(cycle, NGX_WIN32_ROUTER_CONTROL_RESUME,
                                    generation, 1);
}


ngx_int_t
ngx_win32_router_update_workers(ngx_cycle_t *cycle, ngx_uint_t generation)
{
    if (!ngx_win32_router_initialized) {
        return NGX_OK;
    }

    if (ngx_win32_router_command(cycle, NGX_WIN32_ROUTER_CONTROL_UPDATE,
                                 generation, 1)
        != NGX_OK)
    {
        ngx_log_error(NGX_LOG_ALERT, cycle->log, 0,
                      "could not update Win32 network router workers");
        return NGX_ERROR;
    }

    return NGX_OK;
}


ngx_int_t
ngx_win32_router_stop(ngx_cycle_t *cycle)
{
    u_long                           wait;
    size_t                           queued_bytes;
    ngx_int_t                        rc;
    ngx_uint_t                       i, queued_messages;
    ngx_queue_t                     *q;
    ngx_win32_router_accept_t       *accept;
    ngx_win32_router_pipe_write_t   *write;
    ngx_win32_router_worker_t       *worker;

    if (!ngx_win32_router_initialized) {
        return NGX_OK;
    }

    rc = NGX_OK;

    if (ngx_win32_router_thread_handle
        && ngx_win32_router_command(cycle, NGX_WIN32_ROUTER_CONTROL_STOP,
                                    ngx_win32_router_generation, 0)
           != NGX_OK)
    {
        ngx_log_error(NGX_LOG_ALERT, cycle->log, 0,
                      "could not stop the Win32 network router safely");
        return NGX_ERROR;
    }

    if (ngx_win32_router_thread_handle) {
        wait = WaitForSingleObject(ngx_win32_router_thread_handle,
                                   NGX_WIN32_ROUTER_PAUSE_TIMEOUT);
        if (wait != WAIT_OBJECT_0) {
            ngx_log_error(NGX_LOG_ALERT, cycle->log,
                          wait == WAIT_FAILED ? ngx_errno : 0,
                          "could not join the Win32 network router thread");
            return NGX_ERROR;
        }

        ngx_close_handle(ngx_win32_router_thread_handle);
        ngx_win32_router_thread_handle = NULL;
    }

    while (!ngx_queue_empty(&ngx_win32_router_handoffs)) {
        q = ngx_queue_head(&ngx_win32_router_handoffs);
        ngx_queue_remove(q);
        accept = ngx_queue_data(q, ngx_win32_router_accept_t, queue);
        ngx_queue_remove(&accept->id_queue);
        ngx_win32_router_finish_accept(accept);
    }

    for (i = 0; i < NGX_MAX_PROCESSES; i++) {
        worker = &ngx_win32_router_workers[i];
        queued_bytes = 0;
        queued_messages = 0;

        for (q = ngx_queue_head(&worker->writes);
             q != ngx_queue_sentinel(&worker->writes);
             q = ngx_queue_next(q))
        {
            write = ngx_queue_data(q, ngx_win32_router_pipe_write_t, queue);
            queued_messages++;
            queued_bytes += write->size;
        }

        if (worker->read_pending || worker->write_pending
            || queued_messages != worker->queued_messages
            || queued_bytes != worker->queued_bytes)
        {
            ngx_log_error(NGX_LOG_ALERT, cycle->log, 0,
                          "Win32 network router worker %P shutdown state "
                          "differs (pending:%ui/%ui, tracked:%ui/%uz, "
                          "actual:%ui/%uz)",
                          worker->pid, worker->read_pending,
                          worker->write_pending, worker->queued_messages,
                          worker->queued_bytes, queued_messages, queued_bytes);
            rc = NGX_ERROR;
        }

        ngx_win32_router_drop_writes(worker, 0);

        if (worker->read.data != worker->read.header)
        {
            ngx_free(worker->read.data);
        }
    }

    if (ngx_win32_router_pending || ngx_win32_router_channel_pending
        || ngx_win32_router_udp_sends || ngx_win32_router_udp_send_bytes)
    {
        ngx_log_error(NGX_LOG_ALERT, cycle->log, 0,
                      "Win32 network router shutdown left pending work "
                      "(io:%ui, channel:%ui, udp:%ui, bytes:%uz)",
                      ngx_win32_router_pending,
                      ngx_win32_router_channel_pending,
                      ngx_win32_router_udp_sends,
                      ngx_win32_router_udp_send_bytes);
        rc = NGX_ERROR;
    }

    ngx_win32_router_free_listeners();
    ngx_win32_router_free_flows();

    if (ngx_win32_router_port) {
        ngx_close_handle(ngx_win32_router_port);
        ngx_win32_router_port = NULL;
    }

    ngx_win32_router_initialized = 0;
    ngx_win32_router_stopping = 0;
    ngx_win32_router_log = NULL;

    return rc;
}


static ngx_thread_value_t __stdcall
ngx_win32_router_thread(void *data)
{
    BOOL                         rc;
    ULONG                        i, events;
    ngx_err_t                    error;
    OVERLAPPED_ENTRY             entries[NGX_WIN32_ROUTER_BATCH];
    ngx_win32_router_op_t       *op;
    ngx_win32_router_accept_t   *accept;
    ngx_win32_router_control_t  *control;
    ngx_win32_router_pipe_read_t *read;
    ngx_win32_router_pipe_write_t *write;
    ngx_win32_router_udp_recv_t *udp;
    ngx_win32_router_udp_send_t *send;

    (void) data;

#if (NGX_WIN32_GPROF)
    ngx_win32_gprof_start("router", 0, ngx_win32_router_log);
#endif

    while (!ngx_win32_router_stopping) {
        events = 0;
        rc = GetQueuedCompletionStatusEx(ngx_win32_router_port, entries,
                                         NGX_WIN32_ROUTER_BATCH, &events,
                                         1000, FALSE);

        if (rc == 0) {
            error = ngx_errno;

            if (error != WAIT_TIMEOUT && ngx_win32_router_log) {
                ngx_log_error(NGX_LOG_ALERT, ngx_win32_router_log, error,
                              "GetQueuedCompletionStatusEx(network router) "
                              "failed");
            }

        } else {
            for (i = 0; i < events; i++) {
                op = (ngx_win32_router_op_t *) entries[i].lpOverlapped;
                error = 0;

                if (op == NULL) {
                    continue;
                }

                if (op->type != NGX_WIN32_ROUTER_OP_CONTROL) {
                    error = ngx_win32_router_result(op, &entries[i], NULL);
                }

                if (op->type == NGX_WIN32_ROUTER_OP_ACCEPT) {
                    accept = (ngx_win32_router_accept_t *) op;
                    ngx_win32_router_complete_accept(accept,
                                               entries[i].dwNumberOfBytesTransferred,
                                               error);

                } else if (op->type == NGX_WIN32_ROUTER_OP_UDP_RECV) {
                    udp = (ngx_win32_router_udp_recv_t *) op;
                    ngx_win32_router_complete_udp_recv(udp,
                                               entries[i].dwNumberOfBytesTransferred,
                                               error);

                } else if (op->type == NGX_WIN32_ROUTER_OP_UDP_SEND) {
                    send = (ngx_win32_router_udp_send_t *) op;
                    ngx_win32_router_complete_udp_send(send,
                                               entries[i].dwNumberOfBytesTransferred,
                                               error);

                } else if (op->type == NGX_WIN32_ROUTER_OP_PIPE_READ) {
                    read = (ngx_win32_router_pipe_read_t *) op;
                    ngx_win32_router_complete_pipe_read(read,
                                               entries[i].dwNumberOfBytesTransferred,
                                               error);

                } else if (op->type == NGX_WIN32_ROUTER_OP_PIPE_WRITE) {
                    write = (ngx_win32_router_pipe_write_t *) op;
                    ngx_win32_router_complete_pipe_write(write,
                                               entries[i].dwNumberOfBytesTransferred,
                                               error);

                } else if (op->type == NGX_WIN32_ROUTER_OP_CONTROL) {
                    control = (ngx_win32_router_control_t *) op;
                    ngx_win32_router_process_control(control);

                } else if (ngx_win32_router_log) {
                    ngx_log_error(NGX_LOG_ALERT, ngx_win32_router_log, 0,
                                  "unknown Win32 router operation %ui",
                                  op->type);
                }
            }
        }

        ngx_win32_router_expire_accepts();
        ngx_win32_router_expire_flows();

        if (ngx_win32_router_pending_control
            && ngx_win32_router_control_ready())
        {
            control = ngx_win32_router_pending_control;
            ngx_win32_router_pending_control = NULL;

            if (control->command == NGX_WIN32_ROUTER_CONTROL_PAUSE) {
                ngx_win32_router_log = NULL;
            }

            if (control->command == NGX_WIN32_ROUTER_CONTROL_STOP) {
                ngx_win32_router_stopping = 1;
            }

            ngx_win32_router_complete_control(control);
        }
    }

#if (NGX_WIN32_GPROF)
    ngx_win32_gprof_stop();
#endif

    return 0;
}


static ngx_int_t
ngx_win32_router_command(ngx_cycle_t *cycle, ngx_uint_t command,
    ngx_uint_t generation, ngx_uint_t workers)
{
    u_long                         wait;
    ngx_int_t                      i, rc;
    ngx_win32_router_control_t    *control;
    ngx_win32_router_worker_snapshot_t *snapshot;

    if (ngx_win32_router_broken) {
        return NGX_ERROR;
    }

    control = ngx_calloc(sizeof(ngx_win32_router_control_t), cycle->log);
    if (control == NULL) {
        return NGX_ERROR;
    }

    control->event = CreateEvent(NULL, 1, 0, NULL);
    if (control->event == NULL) {
        ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                      "CreateEvent(network router control) failed");
        ngx_free(control);
        return NGX_ERROR;
    }

    control->op.type = NGX_WIN32_ROUTER_OP_CONTROL;
    control->refs = 2;
    control->command = command;
    control->generation = generation;
    control->cycle = cycle;
    control->status = NGX_ERROR;

    if (workers) {
        for (i = 0; i < ngx_last_process; i++) {
            if (ngx_processes[i].handle == NULL
                || !ngx_processes[i].ready_state
                || ngx_processes[i].bootstrap == NULL
                || !ngx_processes[i].bootstrap->routed
                || ngx_processes[i].bootstrap->pipe == NULL)
            {
                continue;
            }

            snapshot = &control->workers[control->worker_n++];
            snapshot->pid = ngx_processes[i].pid;
            snapshot->slot = ngx_processes[i].slot;
            snapshot->generation = ngx_processes[i].generation;
            snapshot->pipe = ngx_processes[i].bootstrap->pipe;
            snapshot->exiting = ngx_processes[i].exiting;
        }
    }

    if (PostQueuedCompletionStatus(ngx_win32_router_port, 0, 0,
                                   &control->op.overlapped)
        == 0)
    {
        ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                      "PostQueuedCompletionStatus(network router control) "
                      "failed");
        ngx_win32_router_broken = 1;
        ngx_win32_router_release_control(control);
        ngx_win32_router_release_control(control);
        return NGX_ERROR;
    }

    wait = WaitForSingleObject(control->event,
                               NGX_WIN32_ROUTER_PAUSE_TIMEOUT);

    if (wait == WAIT_TIMEOUT) {
        ngx_log_error(NGX_LOG_ALERT, cycle->log, 0,
                      "Win32 network router control %ui for generation %ui "
                      "exceeded %dms", command, generation,
                      NGX_WIN32_ROUTER_PAUSE_TIMEOUT);

        wait = WaitForSingleObject(control->event,
                                   NGX_WIN32_ROUTER_GRACE_TIMEOUT);

        if (wait == WAIT_TIMEOUT) {
            ngx_log_error(NGX_LOG_ALERT, cycle->log, 0,
                          "Win32 network router control %ui for generation "
                          "%ui did not complete within an additional %dms; "
                          "router is unusable", command, generation,
                          NGX_WIN32_ROUTER_GRACE_TIMEOUT);

            InterlockedExchange(&control->abandoned, 1);
            ngx_win32_router_broken = 1;
            ngx_win32_router_release_control(control);
            return NGX_ERROR;
        }
    }

    if (wait == WAIT_OBJECT_0) {
        rc = control->status;

    } else {
        ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                      "WaitForSingleObject(network router control %ui for "
                      "generation %ui) failed", command, generation);

        InterlockedExchange(&control->abandoned, 1);
        ngx_win32_router_broken = 1;
        rc = NGX_ERROR;
    }

    ngx_win32_router_release_control(control);

    return rc;
}


static void
ngx_win32_router_complete_control(ngx_win32_router_control_t *control)
{
    if (SetEvent(control->event) == 0 && ngx_win32_router_log) {
        ngx_log_error(NGX_LOG_ALERT, ngx_win32_router_log, ngx_errno,
                      "SetEvent(network router control) failed");
    }

    ngx_win32_router_release_control(control);
}


static void
ngx_win32_router_release_control(ngx_win32_router_control_t *control)
{
    if (InterlockedDecrement(&control->refs) != 0) {
        return;
    }

    ngx_close_handle(control->event);
    ngx_free(control);
}


static void
ngx_win32_router_process_control(ngx_win32_router_control_t *control)
{
    ngx_queue_t                 *q;
    ngx_uint_t                   i, n;
    ngx_iocp_conf_t             *iocpcf;
    ngx_win32_router_listener_t *listener;
    ngx_win32_router_worker_t   *worker;

    if (InterlockedCompareExchange(&control->abandoned, 0, 0)) {
        ngx_win32_router_complete_control(control);
        return;
    }

    ngx_win32_router_log = control->cycle->log;

    if (ngx_win32_router_pending_control) {
        ngx_log_error(NGX_LOG_ALERT, ngx_win32_router_log, 0,
                      "concurrent Win32 network router controls");
        control->status = NGX_ERROR;
        ngx_win32_router_complete_control(control);
        return;
    }

    if (control->command == NGX_WIN32_ROUTER_CONTROL_DRAIN) {
        ngx_win32_router_stream_accepting = 0;
        ngx_win32_router_new_flows = 0;
        ngx_win32_router_cancel_io(1);
        control->status = NGX_OK;
        ngx_win32_router_complete_control(control);
        return;
    }

    if (control->command == NGX_WIN32_ROUTER_CONTROL_UPDATE) {
        control->status = ngx_win32_router_apply_workers(control);

        if (control->status == NGX_OK
            && !ngx_win32_router_control_ready())
        {
            ngx_win32_router_pending_control = control;
            return;
        }

        ngx_win32_router_complete_control(control);
        return;
    }

    if (control->command == NGX_WIN32_ROUTER_CONTROL_PAUSE
        || control->command == NGX_WIN32_ROUTER_CONTROL_STOP)
    {
        ngx_win32_router_stream_accepting = 0;
        ngx_win32_router_udp_accepting = 0;
        ngx_win32_router_new_flows = 0;
        ngx_win32_router_channels = 0;
        ngx_win32_router_cancel_io(0);
        control->status = NGX_OK;

        if (!ngx_win32_router_control_ready()) {
            ngx_win32_router_pending_control = control;
            return;
        }

        if (control->command == NGX_WIN32_ROUTER_CONTROL_PAUSE) {
            ngx_win32_router_log = NULL;

        } else {
            ngx_win32_router_stopping = 1;
        }

        ngx_win32_router_complete_control(control);
        return;
    }

    if (control->command != NGX_WIN32_ROUTER_CONTROL_RESUME
        || ngx_win32_router_pending != 0
        || ngx_win32_router_channel_pending != 0)
    {
        ngx_log_error(NGX_LOG_ALERT, ngx_win32_router_log, 0,
                      "cannot resume a busy Win32 network router");
        control->status = NGX_ERROR;
        ngx_win32_router_complete_control(control);
        return;
    }

    if (ngx_win32_router_configure(control->cycle) != NGX_OK
        || ngx_win32_router_apply_workers(control) != NGX_OK)
    {
        control->status = NGX_ERROR;
        ngx_win32_router_complete_control(control);
        return;
    }

    ngx_win32_router_generation = control->generation;
    ngx_win32_router_stream_accepting = 1;
    ngx_win32_router_udp_accepting = 1;
    ngx_win32_router_new_flows = 1;
    ngx_win32_router_channels = 1;

    for (i = 0; i < NGX_MAX_PROCESSES; i++) {
        worker = &ngx_win32_router_workers[i];

        if (worker->ready
            && (ngx_win32_router_post_pipe_read(worker) != NGX_OK
                || ngx_win32_router_post_pipe_write(worker) != NGX_OK))
        {
            goto failed;
        }
    }

    iocpcf = ngx_event_get_conf(control->cycle->conf_ctx, ngx_iocp_module);

    for (q = ngx_queue_head(&ngx_win32_router_listeners);
         q != ngx_queue_sentinel(&ngx_win32_router_listeners);
         q = ngx_queue_next(q))
    {
        listener = ngx_queue_data(q, ngx_win32_router_listener_t, queue);

        n = listener->type == SOCK_STREAM
            ? (iocpcf && iocpcf->post_acceptex > 0
               ? (ngx_uint_t) iocpcf->post_acceptex
               : NGX_WIN32_ROUTER_ACCEPTS)
            : (iocpcf && iocpcf->udp_receives > 0
               ? (ngx_uint_t) iocpcf->udp_receives : 8);

        for (i = 0; i < n; i++) {
            if ((listener->type == SOCK_STREAM
                 ? ngx_win32_router_post_accept(listener)
                 : ngx_win32_router_post_udp_recv(listener)) != NGX_OK)
            {
                goto failed;
            }
        }
    }

    control->status = NGX_OK;
    ngx_win32_router_complete_control(control);
    return;

failed:

    ngx_win32_router_stream_accepting = 0;
    ngx_win32_router_udp_accepting = 0;
    ngx_win32_router_new_flows = 0;
    ngx_win32_router_channels = 0;
    ngx_win32_router_cancel_io(0);
    control->status = NGX_ERROR;

    if (!ngx_win32_router_control_ready()) {
        ngx_win32_router_pending_control = control;
        return;
    }

    ngx_win32_router_complete_control(control);
}


static ngx_uint_t
ngx_win32_router_control_ready(void)
{
    ngx_uint_t                  i;
    ngx_win32_router_worker_t  *worker;

    if (ngx_win32_router_pending_control
        && ngx_win32_router_pending_control->command
           == NGX_WIN32_ROUTER_CONTROL_UPDATE)
    {
        for (i = 0; i < NGX_MAX_PROCESSES; i++) {
            worker = &ngx_win32_router_workers[i];

            if (worker->removing
                && (worker->read_pending || worker->write_pending))
            {
                return 0;
            }
        }

        return 1;
    }

    return ngx_win32_router_pending == 0
           && ngx_win32_router_channel_pending == 0;
}


static void
ngx_win32_router_cancel_io(ngx_uint_t streams_only)
{
    ngx_queue_t                 *q;
    ngx_uint_t                   i;
    ngx_win32_router_listener_t *listener;
    ngx_win32_router_worker_t   *worker;

    for (q = ngx_queue_head(&ngx_win32_router_listeners);
         q != ngx_queue_sentinel(&ngx_win32_router_listeners);
         q = ngx_queue_next(q))
    {
        listener = ngx_queue_data(q, ngx_win32_router_listener_t, queue);

        if (listener->pending
            && (!streams_only || listener->type == SOCK_STREAM))
        {
            (void) CancelIoEx((HANDLE) listener->socket, NULL);
        }
    }

    if (streams_only) {
        return;
    }

    for (i = 0; i < NGX_MAX_PROCESSES; i++) {
        worker = &ngx_win32_router_workers[i];

        if (worker->ready && (worker->read_pending || worker->write_pending)) {
            (void) CancelIoEx(worker->pipe, NULL);
        }
    }
}


static ngx_err_t
ngx_win32_router_result(ngx_win32_router_op_t *op,
    OVERLAPPED_ENTRY *entry, DWORD *result_bytes)
{
    BOOL                          rc;
    DWORD                         bytes, flags;
    HANDLE                        handle;
    ngx_win32_router_pipe_read_t *read;
    ngx_win32_router_pipe_write_t *write;

    bytes = entry->dwNumberOfBytesTransferred;
    flags = 0;

    if (op->type == NGX_WIN32_ROUTER_OP_ACCEPT
        || op->type == NGX_WIN32_ROUTER_OP_UDP_RECV
        || op->type == NGX_WIN32_ROUTER_OP_UDP_SEND)
    {
        rc = WSAGetOverlappedResult(op->listener->socket, &op->overlapped,
                                    &bytes, FALSE, &flags);
        if (rc == 0) {
            return ngx_socket_errno;
        }

    } else {
        if (op->type == NGX_WIN32_ROUTER_OP_PIPE_READ) {
            read = (ngx_win32_router_pipe_read_t *) op;
            handle = read->worker->pipe;

        } else {
            write = (ngx_win32_router_pipe_write_t *) op;
            handle = write->worker->pipe;
        }

        rc = GetOverlappedResult(handle, &op->overlapped, &bytes, FALSE);
        if (rc == 0) {
            return ngx_errno;
        }
    }

    if (result_bytes) {
        *result_bytes = bytes;
    }

    return 0;
}


static ngx_int_t
ngx_win32_router_configure(ngx_cycle_t *cycle)
{
    ngx_queue_t                 *q, *next;
    ngx_uint_t                   i, n;
    ngx_listening_t             *ls;
    ngx_win32_router_listener_t *listener;

    for (q = ngx_queue_head(&ngx_win32_router_listeners);
         q != ngx_queue_sentinel(&ngx_win32_router_listeners);
         q = ngx_queue_next(q))
    {
        listener = ngx_queue_data(q, ngx_win32_router_listener_t, queue);
        listener->seen = 0;
    }

    n = 0;
    ls = cycle->listening.elts;

    for (i = 0; i < cycle->listening.nelts; i++) {
        if (ls[i].ignore) {
            continue;
        }

        if (ls[i].type != SOCK_STREAM && ls[i].type != SOCK_DGRAM) {
            n++;
            continue;
        }

        listener = ngx_win32_router_find_listener(ls[i].fd);

        if (listener == NULL) {
            if (ngx_win32_router_add_listener(cycle, &ls[i], n) != NGX_OK) {
                return NGX_ERROR;
            }

        } else {
            listener->index = n;
            listener->family = ls[i].sockaddr->sa_family;
            listener->type = ls[i].type;
            listener->protocol = ls[i].protocol;
            listener->socklen = ls[i].socklen;
            ngx_memcpy(&listener->sockaddr, ls[i].sockaddr, ls[i].socklen);
            listener->wildcard = ls[i].wildcard;
            listener->quic = ls[i].quic;
            listener->seen = 1;
            (void) ngx_sock_ntop(ls[i].sockaddr, ls[i].socklen,
                                 listener->addr_text,
                                 NGX_SOCKADDR_STRLEN, 1);
        }

        n++;
    }

    for (q = ngx_queue_head(&ngx_win32_router_listeners);
         q != ngx_queue_sentinel(&ngx_win32_router_listeners);
         q = next)
    {
        next = ngx_queue_next(q);
        listener = ngx_queue_data(q, ngx_win32_router_listener_t, queue);

        if (listener->seen) {
            continue;
        }

        if (listener->pending) {
            ngx_log_error(NGX_LOG_ALERT, cycle->log, 0,
                          "removing a busy Win32 router listener");
            return NGX_ERROR;
        }

        ngx_queue_remove(q);
        ngx_free(listener);
    }

    return NGX_OK;
}


static ngx_win32_router_listener_t *
ngx_win32_router_find_listener(ngx_socket_t socket)
{
    ngx_queue_t                 *q;
    ngx_win32_router_listener_t *listener;

    for (q = ngx_queue_head(&ngx_win32_router_listeners);
         q != ngx_queue_sentinel(&ngx_win32_router_listeners);
         q = ngx_queue_next(q))
    {
        listener = ngx_queue_data(q, ngx_win32_router_listener_t, queue);

        if (listener->socket == socket) {
            return listener;
        }
    }

    return NULL;
}


static ngx_win32_router_listener_t *
ngx_win32_router_find_listener_index(ngx_uint_t index)
{
    ngx_queue_t                 *q;
    ngx_win32_router_listener_t *listener;

    for (q = ngx_queue_head(&ngx_win32_router_listeners);
         q != ngx_queue_sentinel(&ngx_win32_router_listeners);
         q = ngx_queue_next(q))
    {
        listener = ngx_queue_data(q, ngx_win32_router_listener_t, queue);

        if (listener->index == index) {
            return listener;
        }
    }

    return NULL;
}


static ngx_win32_router_listener_t *
ngx_win32_router_find_udp_listener(ngx_uint_t index, struct sockaddr *local,
    socklen_t local_socklen)
{
    ngx_queue_t                 *q;
    ngx_win32_router_listener_t *listener;

    listener = ngx_win32_router_find_listener_index(index);

    if (listener
        && ngx_win32_router_listener_matches(listener, local, local_socklen))
    {
        return listener;
    }

    for (q = ngx_queue_head(&ngx_win32_router_listeners);
         q != ngx_queue_sentinel(&ngx_win32_router_listeners);
         q = ngx_queue_next(q))
    {
        listener = ngx_queue_data(q, ngx_win32_router_listener_t, queue);

        if (ngx_win32_router_listener_matches(listener, local,
                                               local_socklen))
        {
            return listener;
        }
    }

    return NULL;
}


static ngx_uint_t
ngx_win32_router_listener_matches(ngx_win32_router_listener_t *listener,
    struct sockaddr *local, socklen_t local_socklen)
{
    return listener->type == SOCK_DGRAM
           && listener->family == local->sa_family
           && ngx_inet_get_port(&listener->sockaddr.sockaddr)
              == ngx_inet_get_port(local)
           && (listener->wildcard
               || ngx_cmp_sockaddr(&listener->sockaddr.sockaddr,
                                   listener->socklen, local, local_socklen, 1)
                  == NGX_OK);
}


static ngx_int_t
ngx_win32_router_add_listener(ngx_cycle_t *cycle, ngx_listening_t *ls,
    ngx_uint_t index)
{
    BOOL                           enabled;
    DWORD                          bytes;
    int                            on;
    ngx_win32_router_listener_t   *listener;

    listener = ngx_calloc(sizeof(ngx_win32_router_listener_t), cycle->log);
    if (listener == NULL) {
        return NGX_ERROR;
    }

    listener->socket = ls->fd;
    listener->index = index;
    listener->family = ls->sockaddr->sa_family;
    listener->type = ls->type;
    listener->protocol = ls->protocol;
    listener->socklen = ls->socklen;
    ngx_memcpy(&listener->sockaddr, ls->sockaddr, ls->socklen);
    listener->wildcard = ls->wildcard;
    listener->quic = ls->quic;
    listener->seen = 1;
    (void) ngx_sock_ntop(ls->sockaddr, ls->socklen, listener->addr_text,
                         NGX_SOCKADDR_STRLEN, 1);

    if (CreateIoCompletionPort((HANDLE) listener->socket,
                               ngx_win32_router_port,
                               (ULONG_PTR) listener, 0)
        == NULL)
    {
        ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_errno,
                      "CreateIoCompletionPort(network listener %s) failed",
                      listener->addr_text);
        ngx_free(listener);
        return NGX_ERROR;
    }

    bytes = 0;

    if (ls->type == SOCK_STREAM) {
        if (WSAIoctl(listener->socket, SIO_GET_EXTENSION_FUNCTION_POINTER,
                     &ngx_win32_router_acceptex_guid,
                     sizeof(ngx_win32_router_acceptex_guid),
                     &listener->acceptex, sizeof(listener->acceptex),
                     &bytes, NULL, NULL)
            == SOCKET_ERROR)
        {
            ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_socket_errno,
                          "WSAIoctl(WSAID_ACCEPTEX) failed for %s",
                          listener->addr_text);
            ngx_free(listener);
            return NGX_ERROR;
        }

    } else {
        enabled = FALSE;
        bytes = 0;
        (void) WSAIoctl(listener->socket, SIO_UDP_CONNRESET,
                        &enabled, sizeof(enabled), NULL, 0, &bytes,
                        NULL, NULL);

        (void) WSAIoctl(listener->socket,
                        SIO_GET_EXTENSION_FUNCTION_POINTER,
                        &ngx_win32_router_recvmsg_guid,
                        sizeof(ngx_win32_router_recvmsg_guid),
                        &listener->recvmsg, sizeof(listener->recvmsg),
                        &bytes, NULL, NULL);

        bytes = 0;
        (void) WSAIoctl(listener->socket,
                        SIO_GET_EXTENSION_FUNCTION_POINTER,
                        &ngx_win32_router_sendmsg_guid,
                        sizeof(ngx_win32_router_sendmsg_guid),
                        &listener->sendmsg, sizeof(listener->sendmsg),
                        &bytes, NULL, NULL);

        if (listener->wildcard
            && (listener->recvmsg == NULL || listener->sendmsg == NULL))
        {
            ngx_log_error(NGX_LOG_EMERG, cycle->log, 0,
                          "WSARecvMsg()/WSASendMsg() are required for "
                          "wildcard UDP listener %s", listener->addr_text);
            ngx_free(listener);
            return NGX_ERROR;
        }

        if (listener->wildcard) {
            on = 1;

            if ((listener->family == AF_INET
                 && setsockopt(listener->socket, IPPROTO_IP, IP_PKTINFO,
                               (const char *) &on, sizeof(on)) == -1)
#if (NGX_HAVE_INET6)
                || (listener->family == AF_INET6
                    && setsockopt(listener->socket, IPPROTO_IPV6,
                                  IPV6_PKTINFO, (const char *) &on,
                                  sizeof(on)) == -1)
#endif
               )
            {
                ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_socket_errno,
                              "could not enable packet information for %s",
                              listener->addr_text);
                ngx_free(listener);
                return NGX_ERROR;
            }
        }
    }

    ngx_queue_insert_tail(&ngx_win32_router_listeners, &listener->queue);

    return NGX_OK;
}


static ngx_int_t
ngx_win32_router_post_accept(ngx_win32_router_listener_t *listener)
{
    int                        rc;
    DWORD                      bytes;
    ngx_err_t                  error;
    ngx_win32_router_accept_t *accept;

    if (!ngx_win32_router_stream_accepting) {
        return NGX_OK;
    }

    accept = ngx_calloc(sizeof(ngx_win32_router_accept_t),
                        ngx_win32_router_log);
    if (accept == NULL) {
        return NGX_ERROR;
    }

    accept->op.type = NGX_WIN32_ROUTER_OP_ACCEPT;
    accept->op.listener = listener;
    accept->socket = WSASocket(listener->family, listener->type,
                               listener->protocol, NULL, 0,
                               WSA_FLAG_OVERLAPPED);

    if (accept->socket == (ngx_socket_t) -1) {
        ngx_log_error(NGX_LOG_ALERT, ngx_win32_router_log, ngx_socket_errno,
                      "WSASocket() failed for routed listener %s",
                      listener->addr_text);
        ngx_free(accept);
        return NGX_ERROR;
    }

    listener->pending++;
    ngx_win32_router_pending++;
    bytes = 0;

    rc = listener->acceptex(listener->socket, accept->socket, accept->buffer,
                            0, listener->socklen + 16,
                            listener->socklen + 16, &bytes,
                            &accept->op.overlapped);

    if (rc == 0) {
        error = ngx_socket_errno;

        if (error != WSA_IO_PENDING) {
            listener->pending--;
            ngx_win32_router_pending--;
            ngx_log_error(NGX_LOG_ALERT, ngx_win32_router_log, error,
                          "AcceptEx() failed for routed listener %s",
                          listener->addr_text);
            ngx_win32_router_finish_accept(accept);
            return NGX_ERROR;
        }
    }

    return NGX_OK;
}


static ngx_int_t
ngx_win32_router_post_udp_recv(ngx_win32_router_listener_t *listener)
{
    int                         rc;
    DWORD                       bytes;
    ngx_err_t                   error;
    ngx_win32_router_udp_recv_t *op;

    if (!ngx_win32_router_udp_accepting) {
        return NGX_OK;
    }

    op = ngx_calloc(sizeof(ngx_win32_router_udp_recv_t),
                    ngx_win32_router_log);
    if (op == NULL) {
        return NGX_ERROR;
    }

    op->op.type = NGX_WIN32_ROUTER_OP_UDP_RECV;
    op->op.listener = listener;
    op->remote_len = sizeof(op->remote);
    op->wsabuf.buf = (char *) op->data;
    op->wsabuf.len = sizeof(op->data);
    op->msg.name = (LPSOCKADDR) &op->remote;
    op->msg.namelen = sizeof(op->remote);
    op->msg.lpBuffers = &op->wsabuf;
    op->msg.dwBufferCount = 1;
    op->msg.Control.buf = (char *) op->control;
    op->msg.Control.len = sizeof(op->control);

    listener->pending++;
    ngx_win32_router_pending++;
    bytes = 0;

    if (listener->recvmsg) {
        op->recvmsg = 1;
        rc = listener->recvmsg(listener->socket, &op->msg, &bytes,
                                &op->op.overlapped, NULL);

    } else {
        op->recvmsg = 0;
        rc = WSARecvFrom(listener->socket, &op->wsabuf, 1, &bytes, &op->flags,
                         (LPSOCKADDR) &op->remote, &op->remote_len,
                         &op->op.overlapped, NULL);
    }

    if (rc == SOCKET_ERROR) {
        error = ngx_socket_errno;

        if (error == WSA_IO_PENDING) {
            return NGX_OK;
        }

        listener->pending--;
        ngx_win32_router_pending--;
        ngx_log_error(NGX_LOG_ALERT, ngx_win32_router_log, error,
                      "%s failed for routed UDP listener %s",
                      op->recvmsg ? "WSARecvMsg()" : "WSARecvFrom()",
                      listener->addr_text);
        ngx_free(op);
        return NGX_ERROR;
    }

    return NGX_OK;
}


static void
ngx_win32_router_complete_accept(ngx_win32_router_accept_t *accept,
    DWORD bytes, ngx_err_t error)
{
    ngx_win32_router_listener_t *listener;

    (void) bytes;

    listener = accept->op.listener;
    listener->pending--;
    ngx_win32_router_pending--;

    if (ngx_win32_router_stream_accepting
        && ngx_win32_router_post_accept(listener) != NGX_OK)
    {
        ngx_log_error(NGX_LOG_ALERT, ngx_win32_router_log, 0,
                      "could not replenish routed AcceptEx queue for %s",
                      listener->addr_text);
    }

    if (error) {
        if (error != ERROR_OPERATION_ABORTED) {
            ngx_log_error(NGX_LOG_ERR, ngx_win32_router_log, error,
                          "routed AcceptEx() failed for %s",
                          listener->addr_text);
        }

        ngx_win32_router_finish_accept(accept);
        return;
    }

    if (!ngx_win32_router_stream_accepting) {
        ngx_win32_router_finish_accept(accept);
        return;
    }

    if (setsockopt(accept->socket, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
                   (char *) &listener->socket, sizeof(ngx_socket_t))
        == -1)
    {
        ngx_log_error(NGX_LOG_ERR, ngx_win32_router_log, ngx_socket_errno,
                      "setsockopt(SO_UPDATE_ACCEPT_CONTEXT) failed for %s",
                      listener->addr_text);
        ngx_win32_router_finish_accept(accept);
        return;
    }

    if (ngx_win32_router_dispatch_accept(accept) != NGX_OK) {
        ngx_win32_router_finish_accept(accept);
    }
}


static void
ngx_win32_router_complete_udp_recv(ngx_win32_router_udp_recv_t *op,
    DWORD bytes, ngx_err_t error)
{
    size_t                       len, remaining, step;
    u_char                      *control, *end;
    struct cmsghdr              *cmsg;
    struct msghdr                msg;
    ngx_uint_t                   local_found;
    ngx_sockaddr_t               local;
    socklen_t                    local_socklen, remote_socklen;
    ngx_win32_router_listener_t *listener;

    listener = op->op.listener;
    listener->pending--;
    ngx_win32_router_pending--;

    if (ngx_win32_router_udp_accepting
        && ngx_win32_router_post_udp_recv(listener) != NGX_OK)
    {
        ngx_log_error(NGX_LOG_ALERT, ngx_win32_router_log, 0,
                      "could not replenish routed UDP receives for %s",
                      listener->addr_text);
    }

    if (error) {
        if (error != ERROR_OPERATION_ABORTED && error != WSAEMSGSIZE) {
            ngx_log_error(NGX_LOG_ERR, ngx_win32_router_log, error,
                          "routed UDP receive failed for %s",
                          listener->addr_text);
        }

        ngx_free(op);
        return;
    }

    if (!ngx_win32_router_udp_accepting) {
        ngx_free(op);
        return;
    }

    if (bytes > sizeof(op->data)
        || (op->recvmsg && op->msg.dwFlags & (MSG_TRUNC|MSG_CTRUNC)))
    {
        ngx_log_error(NGX_LOG_ERR, ngx_win32_router_log, 0,
                      "routed UDP receive was truncated for %s",
                      listener->addr_text);
        ngx_free(op);
        return;
    }

    remote_socklen = op->recvmsg ? (socklen_t) op->msg.namelen
                                  : (socklen_t) op->remote_len;

    if (remote_socklen <= 0
        || remote_socklen > (socklen_t) sizeof(op->remote))
    {
        ngx_log_error(NGX_LOG_ERR, ngx_win32_router_log, 0,
                      "routed UDP receive returned an invalid peer for %s",
                      listener->addr_text);
        ngx_free(op);
        return;
    }

    ngx_memcpy(&local, &listener->sockaddr, listener->socklen);
    local_socklen = listener->socklen;
    local_found = listener->wildcard ? 0 : 1;

    if (listener->wildcard && op->recvmsg) {
        if (op->msg.Control.len > sizeof(op->control)) {
            ngx_free(op);
            return;
        }

        ngx_memzero(&msg, sizeof(msg));
        msg.msg_control = op->control;
        msg.msg_controllen = op->msg.Control.len;
        control = msg.msg_control;
        end = control + msg.msg_controllen;

        for (cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL; /* void */) {
            remaining = (size_t) (end - (u_char *) cmsg);
            len = (size_t) cmsg->cmsg_len;

            if (remaining < sizeof(struct cmsghdr)
                || len < sizeof(struct cmsghdr) || len > remaining)
            {
                break;
            }

            if (ngx_get_srcaddr_cmsg(cmsg, &local.sockaddr) == NGX_OK) {
                local_found = 1;
                break;
            }

            step = NGX_CMSG_ALIGN(len);
            if (step > remaining
                || remaining - step < sizeof(struct cmsghdr))
            {
                break;
            }

            cmsg = (struct cmsghdr *) ((u_char *) cmsg + step);
        }
    }

    if (!local_found) {
        ngx_log_error(NGX_LOG_ERR, ngx_win32_router_log, 0,
                      "routed UDP receive returned no destination for %s",
                      listener->addr_text);
        ngx_free(op);
        return;
    }

    (void) ngx_win32_router_dispatch_udp(op, bytes, &local.sockaddr,
                                         local_socklen);
    ngx_free(op);
}


static void
ngx_win32_router_complete_udp_send(ngx_win32_router_udp_send_t *op,
    DWORD bytes, ngx_err_t error)
{
    ngx_win32_router_listener_t *listener;

    listener = op->op.listener;
    listener->pending--;
    ngx_win32_router_pending--;
    ngx_win32_router_udp_sends--;
    ngx_win32_router_udp_send_bytes -= op->size;

    if (error) {
        if (error != ERROR_OPERATION_ABORTED) {
            ngx_log_error(NGX_LOG_ERR, ngx_win32_router_log, error,
                          "routed UDP send failed for %s",
                          listener->addr_text);
        }

    } else if (bytes != op->expected) {
        ngx_log_error(NGX_LOG_ERR, ngx_win32_router_log, WSAEMSGSIZE,
                      "routed UDP send was partial for %s",
                      listener->addr_text);
    }

    ngx_free(op);
}


static ngx_int_t
ngx_win32_router_dispatch_udp(ngx_win32_router_udp_recv_t *op, DWORD bytes,
    struct sockaddr *local, socklen_t local_socklen)
{
    uint32_t                       hash;
    ngx_uint_t                     route_slot, route_generation;
    ngx_win32_channel_udp_t       *message;
    ngx_win32_router_flow_t       *flow;
    ngx_win32_router_flow_key_t    key;
    ngx_win32_router_pipe_write_t *write;
    ngx_win32_router_worker_t     *worker;
    u_char                        *data;
    socklen_t                      remote_socklen;

    remote_socklen = op->recvmsg ? (socklen_t) op->msg.namelen
                                  : (socklen_t) op->remote_len;

    if (ngx_win32_router_flow_key(op->op.listener, op->data, bytes,
                                  local, local_socklen,
                                  &op->remote.sockaddr, remote_socklen,
                                  &key, &hash, &route_slot,
                                  &route_generation)
        != NGX_OK)
    {
        return NGX_ERROR;
    }

    worker = NULL;

    if (route_slot != NGX_CONF_UNSET_UINT) {
        worker = ngx_win32_router_find_quic_worker(route_slot,
                                                   route_generation);
    }

    if (worker == NULL) {
        flow = ngx_win32_router_find_flow(&key, hash);

        if (flow) {
            worker = ngx_win32_router_find_worker(flow->pid, flow->slot,
                                                   flow->generation);
        }

        if (worker == NULL && ngx_win32_router_new_flows) {
            worker = ngx_win32_router_select_worker(hash, 0);

            if (worker
                && ngx_win32_router_set_flow(&key, hash, worker) != NGX_OK)
            {
                ngx_log_error(NGX_LOG_WARN, ngx_win32_router_log, 0,
                              "could not remember a routed UDP flow");
            }
        }
    }

    if (worker == NULL) {
        return NGX_ERROR;
    }

    write = ngx_win32_router_alloc_write(worker,
                              sizeof(ngx_win32_channel_udp_t) + bytes, 0);
    if (write == NULL) {
        return NGX_ERROR;
    }

    message = (ngx_win32_channel_udp_t *) write->data;
    ngx_memzero(message, sizeof(ngx_win32_channel_udp_t));

    message->header.magic = NGX_WIN32_CHANNEL_MAGIC;
    message->header.version = NGX_WIN32_CHANNEL_VERSION;
    message->header.type = NGX_WIN32_CHANNEL_UDP_RECV;
    message->header.length = (uint32_t)
                             (sizeof(ngx_win32_channel_udp_t) + bytes);
    message->header.slot = (uint32_t) worker->slot;
    message->header.generation = (uint32_t) worker->generation;
    message->header.listener = (uint32_t) op->op.listener->index;
    message->header.id = ngx_win32_router_next_id++;
    message->local_socklen = (uint32_t) local_socklen;
    message->remote_socklen = (uint32_t) remote_socklen;
    message->data_len = bytes;
    message->flags = op->recvmsg ? op->msg.dwFlags : op->flags;
    ngx_memcpy(&message->local, local, local_socklen);
    ngx_memcpy(&message->remote, &op->remote, remote_socklen);
    data = (u_char *) message + sizeof(ngx_win32_channel_udp_t);
    ngx_memcpy(data, op->data, bytes);

    ngx_win32_router_queue_write_node(worker, write);

    return NGX_OK;
}


static ngx_int_t
ngx_win32_router_dispatch_accept(ngx_win32_router_accept_t *accept)
{
    int                           local_socklen, remote_socklen;
    ngx_win32_channel_accept_t    message;
    ngx_win32_router_worker_t    *worker;

    ngx_memzero(&message, sizeof(ngx_win32_channel_accept_t));
    local_socklen = sizeof(ngx_sockaddr_t);
    remote_socklen = sizeof(ngx_sockaddr_t);

    if (getsockname(accept->socket, &message.local.sockaddr,
                    &local_socklen)
        == -1
        || getpeername(accept->socket, &message.remote.sockaddr,
                       &remote_socklen)
           == -1)
    {
        ngx_log_error(NGX_LOG_ERR, ngx_win32_router_log, ngx_socket_errno,
                      "could not query a routed accepted socket");
        return NGX_ERROR;
    }

    worker = ngx_win32_router_select_worker(0, 1);

    if (worker == NULL) {
        ngx_log_error(NGX_LOG_ALERT, ngx_win32_router_log, 0,
                      "no ready workers for a routed connection");
        return NGX_ERROR;
    }

    message.header.magic = NGX_WIN32_CHANNEL_MAGIC;
    message.header.version = NGX_WIN32_CHANNEL_VERSION;
    message.header.type = NGX_WIN32_CHANNEL_ACCEPT;
    message.header.length = sizeof(ngx_win32_channel_accept_t);
    message.header.slot = (uint32_t) worker->slot;
    message.header.generation = (uint32_t) worker->generation;
    message.header.listener = (uint32_t) accept->op.listener->index;
    message.header.id = ngx_win32_router_next_id++;
    message.local_socklen = (uint32_t) local_socklen;
    message.remote_socklen = (uint32_t) remote_socklen;

    if (WSADuplicateSocket(accept->socket, worker->pid, &message.info) == -1) {
        ngx_log_error(NGX_LOG_ERR, ngx_win32_router_log, ngx_socket_errno,
                      "WSADuplicateSocket(%P) failed for routed connection",
                      worker->pid);
        return NGX_ERROR;
    }

    accept->target = worker->pid;
    accept->id = message.header.id;
    accept->deadline = GetTickCount64() + NGX_WIN32_ROUTER_ACK_TIMEOUT;
    ngx_queue_insert_tail(&ngx_win32_router_handoffs, &accept->queue);
    ngx_queue_insert_tail(
        &ngx_win32_router_accept_buckets[accept->id
                                        % NGX_WIN32_ROUTER_ACCEPT_BUCKETS],
        &accept->id_queue);

    if (ngx_win32_router_queue_write(worker, &message,
                                     sizeof(ngx_win32_channel_accept_t),
                                     accept->id)
        != NGX_OK)
    {
        ngx_queue_remove(&accept->queue);
        ngx_queue_remove(&accept->id_queue);
        return NGX_ERROR;
    }

    return NGX_OK;
}


static ngx_int_t
ngx_win32_router_apply_workers(ngx_win32_router_control_t *control)
{
    ngx_uint_t                         i, n;
    ngx_win32_router_worker_t         *worker;
    ngx_win32_router_worker_snapshot_t *snapshot;

    for (i = 0; i < NGX_MAX_PROCESSES; i++) {
        ngx_win32_router_workers[i].seen = 0;
    }

    for (n = 0; n < control->worker_n; n++) {
        snapshot = &control->workers[n];
        worker = NULL;

        for (i = 0; i < NGX_MAX_PROCESSES; i++) {
            if (ngx_win32_router_workers[i].pid == snapshot->pid
                && ngx_win32_router_workers[i].slot == snapshot->slot
                && ngx_win32_router_workers[i].generation
                   == snapshot->generation
                && (ngx_win32_router_workers[i].ready
                    || ngx_win32_router_workers[i].removing
                    || ngx_win32_router_workers[i].failed))
            {
                worker = &ngx_win32_router_workers[i];
                break;
            }
        }

        if (worker == NULL) {
            for (i = 0; i < NGX_MAX_PROCESSES; i++) {
                if (!ngx_win32_router_workers[i].ready
                    && !ngx_win32_router_workers[i].removing
                    && !ngx_win32_router_workers[i].failed
                    && !ngx_win32_router_workers[i].read_pending
                    && !ngx_win32_router_workers[i].write_pending
                    && ngx_queue_empty(&ngx_win32_router_workers[i].writes))
                {
                    worker = &ngx_win32_router_workers[i];
                    break;
                }
            }

            if (worker == NULL) {
                ngx_log_error(NGX_LOG_ALERT, ngx_win32_router_log, 0,
                              "no room for another Win32 routed worker");
                return NGX_ERROR;
            }

            if (CreateIoCompletionPort(snapshot->pipe,
                                       ngx_win32_router_port, 0, 0)
                == NULL)
            {
                ngx_log_error(NGX_LOG_ALERT, ngx_win32_router_log, ngx_errno,
                              "CreateIoCompletionPort(worker %P pipe) failed",
                              snapshot->pid);
                return NGX_ERROR;
            }

            worker->pid = snapshot->pid;
            worker->slot = snapshot->slot;
            worker->generation = snapshot->generation;
            worker->pipe = snapshot->pipe;
            worker->failed = 0;
            worker->read.data = worker->read.header;
            worker->read.size = sizeof(ngx_win32_channel_header_t);
            worker->read.offset = 0;
        }

        worker->seen = 1;

        if (worker->failed) {
            continue;
        }

        worker->ready = 1;
        worker->removing = 0;
        worker->active = !snapshot->exiting
                         && snapshot->generation == control->generation;

        if (ngx_win32_router_channels
            && (ngx_win32_router_post_pipe_read(worker) != NGX_OK
                || ngx_win32_router_post_pipe_write(worker) != NGX_OK))
        {
            return NGX_ERROR;
        }
    }

    for (i = 0; i < NGX_MAX_PROCESSES; i++) {
        worker = &ngx_win32_router_workers[i];

        if ((!worker->ready && !worker->failed) || worker->seen) {
            continue;
        }

        worker->ready = 0;
        worker->active = 0;
        worker->removing = 1;
        ngx_win32_router_drop_writes(worker, worker->write_pending);

        if (worker->read_pending || worker->write_pending) {
            (void) CancelIoEx(worker->pipe, NULL);

        } else {
            worker->pipe = NULL;
            worker->removing = 0;
            worker->failed = 0;

            if (worker->read.data != worker->read.header) {
                ngx_free(worker->read.data);
                worker->read.data = worker->read.header;
                worker->read.size = sizeof(ngx_win32_channel_header_t);
                worker->read.offset = 0;
            }
        }
    }

    ngx_win32_router_rebuild_ready_workers();
    ngx_win32_router_generation = control->generation;

    return NGX_OK;
}


static ngx_int_t
ngx_win32_router_post_pipe_read(ngx_win32_router_worker_t *worker)
{
    BOOL                           rc;
    DWORD                          bytes, error;
    ngx_uint_t                     expected;
    ngx_win32_router_pipe_read_t  *read;

    if (!ngx_win32_router_channels || !worker->ready
        || worker->read_pending)
    {
        return NGX_OK;
    }

    read = &worker->read;

    if (read->data == NULL) {
        read->data = read->header;
        read->size = sizeof(ngx_win32_channel_header_t);
        read->offset = 0;
    }

    ngx_memzero(&read->op.overlapped, sizeof(OVERLAPPED));
    worker->read_pending = 1;
    ngx_win32_router_channel_pending++;
    bytes = 0;

    rc = ReadFile(worker->pipe, read->data + read->offset,
                  (DWORD) (read->size - read->offset), &bytes,
                  &read->op.overlapped);

    if (rc == 0) {
        error = ngx_errno;

        if (error != ERROR_IO_PENDING) {
            worker->read_pending = 0;
            ngx_win32_router_channel_pending--;
            expected = !worker->active
                       && ngx_win32_router_pipe_ended(error);

            if (!expected) {
                ngx_log_error(NGX_LOG_ERR, ngx_win32_router_log, error,
                              "ReadFile(worker %P channel) failed",
                              worker->pid);
            }

            worker->ready = 0;
            worker->active = 0;
            worker->removing = 1;
            worker->failed = 1;
            ngx_win32_router_drop_writes(worker, worker->write_pending);

            if (worker->write_pending) {
                (void) CancelIoEx(worker->pipe, NULL);
            }

            return expected ? NGX_OK : NGX_ERROR;
        }
    }

    return NGX_OK;
}


static void
ngx_win32_router_complete_pipe_read(ngx_win32_router_pipe_read_t *read,
    DWORD bytes, ngx_err_t error)
{
    ngx_win32_channel_header_t *header;
    ngx_win32_router_worker_t  *worker;
    u_char                     *message;

    worker = read->worker;
    worker->read_pending = 0;
    ngx_win32_router_channel_pending--;

    if (bytes > read->size - read->offset) {
        error = ERROR_INVALID_DATA;
        bytes = 0;
    }

    read->offset += bytes;

    if (error) {
        if (error != ERROR_OPERATION_ABORTED
            && worker->ready && ngx_win32_router_channels)
        {
            if (worker->active
                || !ngx_win32_router_pipe_ended(error))
            {
                ngx_log_error(NGX_LOG_ERR, ngx_win32_router_log, error,
                              "worker %P channel read failed", worker->pid);
            }

            worker->ready = 0;
            worker->active = 0;
            worker->removing = 1;
            worker->failed = 1;
            ngx_win32_router_drop_writes(worker, worker->write_pending);

            if (worker->write_pending) {
                (void) CancelIoEx(worker->pipe, NULL);
            }
        }

        goto done;
    }

    if (bytes == 0) {
        if (worker->ready && ngx_win32_router_channels) {
            if (worker->active) {
                ngx_log_error(NGX_LOG_ERR, ngx_win32_router_log, 0,
                              "worker %P channel was closed", worker->pid);
            }

            worker->ready = 0;
            worker->active = 0;
            worker->removing = 1;
            worker->failed = 1;
            ngx_win32_router_drop_writes(worker, worker->write_pending);
        }

        goto done;
    }

    if (read->offset != read->size) {
        (void) ngx_win32_router_post_pipe_read(worker);
        return;
    }

    if (read->data == read->header) {
        header = (ngx_win32_channel_header_t *) read->header;

        if (header->magic != NGX_WIN32_CHANNEL_MAGIC
            || header->version != NGX_WIN32_CHANNEL_VERSION
            || header->slot != worker->slot
            || header->generation != worker->generation
            || header->length < sizeof(ngx_win32_channel_header_t)
            || header->length > NGX_WIN32_CHANNEL_MAX_MESSAGE)
        {
            ngx_log_error(NGX_LOG_ALERT, ngx_win32_router_log, 0,
                          "invalid worker %P channel message", worker->pid);
            worker->ready = 0;
            worker->active = 0;
            worker->removing = 1;
            worker->failed = 1;
            goto done;
        }

        if (header->length == sizeof(ngx_win32_channel_header_t)) {
            if (ngx_win32_router_handle_channel(worker, read->header,
                                                 header->length)
                != NGX_OK)
            {
                worker->ready = 0;
                worker->active = 0;
                worker->removing = 1;
                worker->failed = 1;
                goto done;
            }

            read->offset = 0;
            (void) ngx_win32_router_post_pipe_read(worker);
            return;
        }

        message = ngx_alloc(header->length, ngx_win32_router_log);
        if (message == NULL) {
            worker->ready = 0;
            worker->active = 0;
            worker->removing = 1;
            worker->failed = 1;
            goto done;
        }

        ngx_memcpy(message, read->header,
                   sizeof(ngx_win32_channel_header_t));
        read->data = message;
        read->size = header->length;
        read->offset = sizeof(ngx_win32_channel_header_t);
        (void) ngx_win32_router_post_pipe_read(worker);
        return;
    }

    message = read->data;
    error = ngx_win32_router_handle_channel(worker, message, read->size);

    if (error != NGX_OK) {
        worker->ready = 0;
        worker->active = 0;
        worker->removing = 1;
        worker->failed = 1;
    }

    ngx_free(message);
    read->data = read->header;
    read->size = sizeof(ngx_win32_channel_header_t);
    read->offset = 0;

    if (error != NGX_OK) {
        goto done;
    }

    (void) ngx_win32_router_post_pipe_read(worker);
    return;

done:

    if (worker->removing && worker->write_pending) {
        (void) CancelIoEx(worker->pipe, NULL);
    }

    if (worker->removing && !worker->write_pending) {
        if (read->data != read->header) {
            ngx_free(read->data);
        }

        read->data = read->header;
        read->size = sizeof(ngx_win32_channel_header_t);
        read->offset = 0;
        if (!worker->failed) {
            worker->pipe = NULL;
        }

        worker->removing = 0;
    }
}


static ngx_int_t
ngx_win32_router_handle_channel(ngx_win32_router_worker_t *worker,
    u_char *message, size_t size)
{
    ngx_queue_t                 *q;
    ngx_win32_channel_header_t  *header;
    ngx_win32_channel_udp_t     *udp;
    ngx_win32_router_accept_t   *accept;

    header = (ngx_win32_channel_header_t *) message;

    if (header->length != size) {
        return NGX_ERROR;
    }

    if (header->type == NGX_WIN32_CHANNEL_ACCEPT_ACK) {
        if (size != sizeof(ngx_win32_channel_header_t)) {
            return NGX_ERROR;
        }

        for (q = ngx_queue_head(
                 &ngx_win32_router_accept_buckets[
                               header->id % NGX_WIN32_ROUTER_ACCEPT_BUCKETS]);
             q != ngx_queue_sentinel(
                 &ngx_win32_router_accept_buckets[
                               header->id % NGX_WIN32_ROUTER_ACCEPT_BUCKETS]);
             q = ngx_queue_next(q))
        {
            accept = ngx_queue_data(q, ngx_win32_router_accept_t, id_queue);

            if (accept->id != header->id || accept->target != worker->pid) {
                continue;
            }

            ngx_queue_remove(&accept->queue);
            ngx_queue_remove(&accept->id_queue);

            if (header->status) {
                ngx_log_error(NGX_LOG_ERR, ngx_win32_router_log,
                              header->status,
                              "worker %P rejected a routed connection",
                              worker->pid);
            }

            ngx_win32_router_finish_accept(accept);
            break;
        }

        return NGX_OK;
    }

    if (header->type != NGX_WIN32_CHANNEL_UDP_SEND
        || size < sizeof(ngx_win32_channel_udp_t)
        || size > sizeof(ngx_win32_channel_udp_t)
                  + NGX_WIN32_CHANNEL_UDP_MAX)
    {
        return NGX_ERROR;
    }

    udp = (ngx_win32_channel_udp_t *) message;

    if (udp->data_len > NGX_WIN32_CHANNEL_UDP_MAX
        || size != sizeof(ngx_win32_channel_udp_t) + udp->data_len)
    {
        return NGX_ERROR;
    }

    if (ngx_win32_router_udp_sends >= NGX_WIN32_ROUTER_UDP_SENDS
        || udp->data_len > NGX_WIN32_ROUTER_UDP_SEND_LIMIT
        || ngx_win32_router_udp_send_bytes
           > NGX_WIN32_ROUTER_UDP_SEND_LIMIT - udp->data_len)
    {
        ngx_log_error(NGX_LOG_WARN, ngx_win32_router_log, 0,
                      "routed UDP send queue is full");
        return NGX_OK;
    }

    (void) ngx_win32_router_send_udp(worker, udp,
                     message + sizeof(ngx_win32_channel_udp_t));

    return NGX_OK;
}


static ngx_int_t
ngx_win32_router_queue_write(ngx_win32_router_worker_t *worker,
    const void *data, size_t size, uint64_t accept_id)
{
    ngx_win32_router_pipe_write_t *write;

    write = ngx_win32_router_alloc_write(worker, size, accept_id);
    if (write == NULL) {
        return NGX_ERROR;
    }

    ngx_memcpy(write->data, data, size);
    ngx_win32_router_queue_write_node(worker, write);

    return NGX_OK;
}


static ngx_win32_router_pipe_write_t *
ngx_win32_router_alloc_write(ngx_win32_router_worker_t *worker, size_t size,
    uint64_t accept_id)
{
    ngx_win32_router_pipe_write_t *write;

    if (!ngx_win32_router_channels || !worker->ready || size == 0
        || worker->queued_messages >= NGX_WIN32_ROUTER_CHANNEL_MESSAGES
        || size > NGX_WIN32_ROUTER_CHANNEL_LIMIT
        || worker->queued_bytes > NGX_WIN32_ROUTER_CHANNEL_LIMIT - size)
    {
        return NULL;
    }

    write = ngx_alloc(sizeof(ngx_win32_router_pipe_write_t) + size,
                      ngx_win32_router_log);
    if (write == NULL) {
        return NULL;
    }

    ngx_memzero(write, sizeof(ngx_win32_router_pipe_write_t));
    write->op.type = NGX_WIN32_ROUTER_OP_PIPE_WRITE;
    write->worker = worker;
    write->size = size;
    write->accept_id = accept_id;
    write->data = (u_char *) (write + 1);

    return write;
}


static void
ngx_win32_router_queue_write_node(ngx_win32_router_worker_t *worker,
    ngx_win32_router_pipe_write_t *write)
{
    ngx_queue_insert_tail(&worker->writes, &write->queue);
    worker->queued_messages++;
    worker->queued_bytes += write->size;

    (void) ngx_win32_router_post_pipe_write(worker);
}


static void
ngx_win32_router_free_pipe_write(ngx_win32_router_pipe_write_t *write)
{
    ngx_free(write);
}


static ngx_int_t
ngx_win32_router_post_pipe_write(ngx_win32_router_worker_t *worker)
{
    BOOL                            rc;
    DWORD                           bytes, error;
    ngx_uint_t                      expected;
    ngx_queue_t                    *q;
    ngx_win32_router_pipe_write_t  *write;

    if (!ngx_win32_router_channels || !worker->ready
        || worker->write_pending || ngx_queue_empty(&worker->writes))
    {
        return NGX_OK;
    }

    q = ngx_queue_head(&worker->writes);
    write = ngx_queue_data(q, ngx_win32_router_pipe_write_t, queue);
    ngx_memzero(&write->op.overlapped, sizeof(OVERLAPPED));
    worker->write_pending = 1;
    ngx_win32_router_channel_pending++;
    bytes = 0;

    rc = WriteFile(worker->pipe, write->data + write->offset,
                   (DWORD) (write->size - write->offset), &bytes,
                   &write->op.overlapped);

    if (rc == 0) {
        error = ngx_errno;

        if (error != ERROR_IO_PENDING) {
            worker->write_pending = 0;
            ngx_win32_router_channel_pending--;
            expected = !worker->active
                       && ngx_win32_router_pipe_ended(error);

            if (!expected) {
                ngx_log_error(NGX_LOG_ERR, ngx_win32_router_log, error,
                              "WriteFile(worker %P channel) failed",
                              worker->pid);
            }

            ngx_queue_remove(&write->queue);
            worker->queued_messages--;
            worker->queued_bytes -= write->size;
            ngx_win32_router_fail_accept(write->accept_id, worker->pid);
            ngx_win32_router_free_pipe_write(write);
            worker->ready = 0;
            worker->active = 0;
            worker->removing = 1;
            worker->failed = 1;
            ngx_win32_router_drop_writes(worker, 0);

            if (worker->read_pending) {
                (void) CancelIoEx(worker->pipe, NULL);
            }

            return expected ? NGX_OK : NGX_ERROR;
        }
    }

    return NGX_OK;
}


static void
ngx_win32_router_complete_pipe_write(ngx_win32_router_pipe_write_t *write,
    DWORD bytes, ngx_err_t error)
{
    ngx_win32_router_worker_t *worker;

    worker = write->worker;
    worker->write_pending = 0;
    ngx_win32_router_channel_pending--;

    if (bytes > write->size - write->offset) {
        error = ERROR_INVALID_DATA;
        bytes = 0;
    }

    write->offset += bytes;

    if (error == ERROR_OPERATION_ABORTED && !ngx_win32_router_channels
        && !worker->removing)
    {
        return;
    }

    if (error || bytes == 0 || worker->removing) {
        if (error && error != ERROR_OPERATION_ABORTED && worker->ready
            && (worker->active || !ngx_win32_router_pipe_ended(error)))
        {
            ngx_log_error(NGX_LOG_ERR, ngx_win32_router_log, error,
                          "worker %P channel write failed", worker->pid);
        }

        ngx_queue_remove(&write->queue);
        worker->queued_messages--;
        worker->queued_bytes -= write->size;
        ngx_win32_router_fail_accept(write->accept_id, worker->pid);
        ngx_win32_router_free_pipe_write(write);

        if (error != ERROR_OPERATION_ABORTED) {
            worker->ready = 0;
            worker->active = 0;
            worker->removing = 1;
            worker->failed = 1;
        }

        ngx_win32_router_drop_writes(worker, 0);

        if (worker->read_pending) {
            (void) CancelIoEx(worker->pipe, NULL);
        }

        if (worker->removing && !worker->read_pending) {
            if (!worker->failed) {
                worker->pipe = NULL;
            }

            worker->removing = 0;
        }

        return;
    }

    if (write->offset != write->size) {
        (void) ngx_win32_router_post_pipe_write(worker);
        return;
    }

    ngx_queue_remove(&write->queue);
    worker->queued_messages--;
    worker->queued_bytes -= write->size;
    ngx_win32_router_free_pipe_write(write);
    (void) ngx_win32_router_post_pipe_write(worker);
}


static ngx_uint_t
ngx_win32_router_pipe_ended(ngx_err_t error)
{
    return error == ERROR_BROKEN_PIPE
           || error == ERROR_PIPE_NOT_CONNECTED
           || error == ERROR_NO_DATA;
}


static void
ngx_win32_router_drop_writes(ngx_win32_router_worker_t *worker,
    ngx_uint_t pending)
{
    ngx_queue_t                    *q, *next;
    ngx_win32_router_pipe_write_t  *write;

    for (q = ngx_queue_head(&worker->writes);
         q != ngx_queue_sentinel(&worker->writes);
         q = next)
    {
        next = ngx_queue_next(q);
        write = ngx_queue_data(q, ngx_win32_router_pipe_write_t, queue);

        if (pending) {
            pending = 0;
            continue;
        }

        ngx_queue_remove(q);
        worker->queued_messages--;
        worker->queued_bytes -= write->size;
        ngx_win32_router_fail_accept(write->accept_id, worker->pid);
        ngx_win32_router_free_pipe_write(write);
    }
}


static void
ngx_win32_router_fail_accept(uint64_t id, ngx_pid_t pid)
{
    ngx_queue_t                *q;
    ngx_win32_router_accept_t  *accept;

    if (id == 0) {
        return;
    }

    for (q = ngx_queue_head(
             &ngx_win32_router_accept_buckets[
                                      id % NGX_WIN32_ROUTER_ACCEPT_BUCKETS]);
         q != ngx_queue_sentinel(
             &ngx_win32_router_accept_buckets[
                                      id % NGX_WIN32_ROUTER_ACCEPT_BUCKETS]);
         q = ngx_queue_next(q))
    {
        accept = ngx_queue_data(q, ngx_win32_router_accept_t, id_queue);

        if (accept->id == id && accept->target == pid) {
            ngx_queue_remove(&accept->queue);
            ngx_queue_remove(&accept->id_queue);
            ngx_win32_router_finish_accept(accept);
            return;
        }
    }
}


static ngx_int_t
ngx_win32_router_send_udp(ngx_win32_router_worker_t *worker,
    ngx_win32_channel_udp_t *udp, u_char *data)
{
    int                           rc;
    DWORD                         bytes;
    size_t                        control_len;
    ngx_err_t                     error;
    ngx_win32_router_udp_send_t  *op;
    ngx_win32_router_listener_t  *listener;

    if (udp->local_socklen > sizeof(ngx_sockaddr_t)
        || udp->remote_socklen > sizeof(ngx_sockaddr_t)
        || ngx_win32_router_sockaddr_valid(&udp->local.sockaddr,
                                           udp->local_socklen)
           != NGX_OK
        || ngx_win32_router_sockaddr_valid(&udp->remote.sockaddr,
                                           udp->remote_socklen)
           != NGX_OK
        || udp->local.sockaddr.sa_family
           != udp->remote.sockaddr.sa_family)
    {
        goto invalid;
    }

    listener = ngx_win32_router_find_udp_listener(udp->header.listener,
                                                   &udp->local.sockaddr,
                                                   udp->local_socklen);
    if (listener == NULL) {
        goto invalid;
    }

    if (udp->remote.sockaddr.sa_family != listener->family) {
        goto invalid;
    }

    op = ngx_alloc(sizeof(ngx_win32_router_udp_send_t) + udp->data_len,
                   ngx_win32_router_log);
    if (op == NULL) {
        return NGX_ERROR;
    }

    ngx_memzero(op, sizeof(ngx_win32_router_udp_send_t));
    op->op.type = NGX_WIN32_ROUTER_OP_UDP_SEND;
    op->op.listener = listener;
    op->expected = udp->data_len;
    op->size = udp->data_len;
    op->wsabuf.buf = (char *) op->data;
    op->wsabuf.len = udp->data_len;
    ngx_memcpy(op->data, data, udp->data_len);
    ngx_memcpy(&op->remote, &udp->remote, udp->remote_socklen);

    op->msg.name = (LPSOCKADDR) &op->remote;
    op->msg.namelen = (INT) udp->remote_socklen;
    op->msg.lpBuffers = &op->wsabuf;
    op->msg.dwBufferCount = 1;

    if (listener->wildcard) {
        if (listener->sendmsg == NULL) {
            ngx_log_error(NGX_LOG_ALERT, ngx_win32_router_log, 0,
                          "WSASendMsg() is unavailable for routed UDP "
                          "listener %s", listener->addr_text);
            ngx_free(op);
            return NGX_ERROR;
        }

        control_len = ngx_set_srcaddr_cmsg(
                               (struct cmsghdr *) op->control,
                               &udp->local.sockaddr);

        if (control_len == 0 || control_len > sizeof(op->control)) {
            ngx_log_error(NGX_LOG_ALERT, ngx_win32_router_log, 0,
                          "could not select the routed UDP source for %s",
                          listener->addr_text);
            ngx_free(op);
            return NGX_ERROR;
        }

        op->msg.Control.buf = (char *) op->control;
        op->msg.Control.len = (ULONG) control_len;
    }

    listener->pending++;
    ngx_win32_router_pending++;
    ngx_win32_router_udp_sends++;
    ngx_win32_router_udp_send_bytes += udp->data_len;
    bytes = 0;

    if (listener->wildcard) {
        rc = listener->sendmsg(listener->socket, &op->msg,
                               (DWORD) udp->flags, &bytes,
                               &op->op.overlapped, NULL);

    } else {
        rc = WSASendTo(listener->socket, &op->wsabuf, 1, &bytes,
                       (DWORD) udp->flags, op->msg.name, op->msg.namelen,
                       &op->op.overlapped, NULL);
    }

    if (rc == SOCKET_ERROR) {
        error = ngx_socket_errno;

        if (error == WSA_IO_PENDING) {
            return NGX_OK;
        }

        listener->pending--;
        ngx_win32_router_pending--;
        ngx_win32_router_udp_sends--;
        ngx_win32_router_udp_send_bytes -= udp->data_len;
        ngx_log_error(NGX_LOG_ERR, ngx_win32_router_log, error,
                      "could not send routed UDP datagram for worker %P",
                      worker->pid);
        ngx_free(op);
        return NGX_ERROR;
    }

    return NGX_OK;

invalid:

    ngx_log_error(NGX_LOG_ALERT, ngx_win32_router_log, 0,
                  "worker %P supplied an invalid routed UDP send",
                  worker->pid);
    return NGX_ERROR;
}


static ngx_int_t
ngx_win32_router_sockaddr_valid(struct sockaddr *sockaddr,
    socklen_t socklen)
{
    if (sockaddr == NULL
        || socklen < (socklen_t) sizeof(sockaddr->sa_family))
    {
        return NGX_ERROR;
    }

    switch (sockaddr->sa_family) {

    case AF_INET:
        return socklen >= (socklen_t) sizeof(struct sockaddr_in)
               ? NGX_OK : NGX_ERROR;

#if (NGX_HAVE_INET6)
    case AF_INET6:
        return socklen >= (socklen_t) sizeof(struct sockaddr_in6)
               ? NGX_OK : NGX_ERROR;
#endif

    default:
        return NGX_ERROR;
    }
}


static ngx_int_t
ngx_win32_router_flow_key(ngx_win32_router_listener_t *listener,
    u_char *data, size_t size, struct sockaddr *local,
    socklen_t local_socklen, struct sockaddr *remote,
    socklen_t remote_socklen, ngx_win32_router_flow_key_t *key,
    uint32_t *hash, ngx_uint_t *route_slot,
    ngx_uint_t *route_generation)
{
#if (NGX_QUIC)
    ngx_str_t  dcid;
#endif

    if (ngx_win32_router_sockaddr_valid(local, local_socklen) != NGX_OK
        || ngx_win32_router_sockaddr_valid(remote, remote_socklen) != NGX_OK)
    {
        return NGX_ERROR;
    }

    ngx_memzero(key, sizeof(ngx_win32_router_flow_key_t));
    key->listener = ((uint32_t) (listener->protocol & 0xffff) << 1)
                    | (listener->quic ? 1 : 0);
    key->local_family = (uint16_t) local->sa_family;
    ngx_win32_router_flow_addr(local, key->local_addr, &key->local_port,
                               &key->local_scope);

    *route_slot = NGX_CONF_UNSET_UINT;
    *route_generation = NGX_CONF_UNSET_UINT;

    if (listener->quic) {
#if (NGX_QUIC)
        if (ngx_quic_get_packet_dcid(ngx_win32_router_log, data, size, &dcid)
            != NGX_OK
            || dcid.len > sizeof(key->cid))
        {
            return NGX_ERROR;
        }

        key->type = NGX_WIN32_ROUTER_FLOW_QUIC;
        key->cid_len = (uint16_t) dcid.len;
        ngx_memcpy(key->cid, dcid.data, dcid.len);

        if (ngx_win32_quic_route_decode(dcid.data, dcid.len, route_slot,
                                        route_generation)
            != NGX_OK)
        {
            *route_slot = NGX_CONF_UNSET_UINT;
            *route_generation = NGX_CONF_UNSET_UINT;
        }
#else
        return NGX_ERROR;
#endif

    } else {
        key->type = NGX_WIN32_ROUTER_FLOW_TUPLE;
        key->remote_family = (uint16_t) remote->sa_family;
        ngx_win32_router_flow_addr(remote, key->remote_addr,
                                   &key->remote_port, &key->remote_scope);
    }

    *hash = ngx_crc32_long((u_char *) key,
                           sizeof(ngx_win32_router_flow_key_t));

    return NGX_OK;
}


static void
ngx_win32_router_flow_addr(struct sockaddr *sockaddr, u_char *addr,
    uint16_t *port, uint32_t *scope)
{
    struct sockaddr_in  *sin;
#if (NGX_HAVE_INET6)
    struct sockaddr_in6 *sin6;
#endif

    if (sockaddr->sa_family == AF_INET) {
        sin = (struct sockaddr_in *) sockaddr;
        *port = sin->sin_port;
        ngx_memcpy(addr, &sin->sin_addr, sizeof(struct in_addr));
        return;
    }

#if (NGX_HAVE_INET6)
    sin6 = (struct sockaddr_in6 *) sockaddr;
    *port = sin6->sin6_port;
    *scope = sin6->sin6_scope_id;
    ngx_memcpy(addr, &sin6->sin6_addr, sizeof(struct in6_addr));
#endif
}


static ngx_win32_router_flow_t *
ngx_win32_router_find_flow(ngx_win32_router_flow_key_t *key, uint32_t hash)
{
    ngx_queue_t             *q, *bucket;
    ngx_win32_router_flow_t *flow;

    bucket = &ngx_win32_router_flow_buckets[
                              hash & (NGX_WIN32_ROUTER_FLOW_BUCKETS - 1)];

    for (q = ngx_queue_head(bucket);
         q != ngx_queue_sentinel(bucket);
         q = ngx_queue_next(q))
    {
        flow = ngx_queue_data(q, ngx_win32_router_flow_t, bucket);

        if (flow->hash != hash
            || ngx_memcmp(&flow->key, key,
                          sizeof(ngx_win32_router_flow_key_t)) != 0)
        {
            continue;
        }

        flow->deadline = GetTickCount64() + NGX_WIN32_ROUTER_FLOW_TIMEOUT;
        ngx_queue_remove(&flow->expires);
        ngx_queue_insert_tail(&ngx_win32_router_flows, &flow->expires);

        return flow;
    }

    return NULL;
}


static ngx_int_t
ngx_win32_router_set_flow(ngx_win32_router_flow_key_t *key, uint32_t hash,
    ngx_win32_router_worker_t *worker)
{
    ngx_queue_t             *q, *bucket;
    ngx_win32_router_flow_t *flow;

    flow = ngx_win32_router_find_flow(key, hash);

    if (flow == NULL) {
        if (ngx_win32_router_flow_n >= NGX_WIN32_ROUTER_FLOW_MAX) {
            q = ngx_queue_head(&ngx_win32_router_flows);
            flow = ngx_queue_data(q, ngx_win32_router_flow_t, expires);
            ngx_queue_remove(&flow->expires);
            ngx_queue_remove(&flow->bucket);
            ngx_free(flow);
            ngx_win32_router_flow_n--;
        }

        flow = ngx_alloc(sizeof(ngx_win32_router_flow_t),
                         ngx_win32_router_log);
        if (flow == NULL) {
            return NGX_ERROR;
        }

        ngx_memzero(flow, sizeof(ngx_win32_router_flow_t));
        flow->key = *key;
        flow->hash = hash;
        bucket = &ngx_win32_router_flow_buckets[
                              hash & (NGX_WIN32_ROUTER_FLOW_BUCKETS - 1)];
        ngx_queue_insert_tail(bucket, &flow->bucket);
        ngx_queue_insert_tail(&ngx_win32_router_flows, &flow->expires);
        ngx_win32_router_flow_n++;
    }

    flow->pid = worker->pid;
    flow->slot = worker->slot;
    flow->generation = worker->generation;
    flow->deadline = GetTickCount64() + NGX_WIN32_ROUTER_FLOW_TIMEOUT;

    return NGX_OK;
}


static ngx_win32_router_worker_t *
ngx_win32_router_find_worker(ngx_pid_t pid, ngx_uint_t slot,
    ngx_uint_t generation)
{
    ngx_uint_t                  i;
    ngx_win32_router_worker_t  *worker;

    for (i = 0; i < ngx_win32_router_ready_worker_n; i++) {
        worker = ngx_win32_router_ready_workers[i];

        if (worker->ready && worker->pid == pid && worker->slot == slot
            && worker->generation == generation)
        {
            return worker;
        }
    }

    return NULL;
}


static ngx_win32_router_worker_t *
ngx_win32_router_find_quic_worker(ngx_uint_t slot, ngx_uint_t generation)
{
    ngx_uint_t                  i;
    ngx_win32_router_worker_t  *worker;

    for (i = 0; i < ngx_win32_router_ready_worker_n; i++) {
        worker = ngx_win32_router_ready_workers[i];

        if (worker->ready && worker->slot == slot
            && (worker->generation & 0xffff) == generation)
        {
            return worker;
        }
    }

    return NULL;
}


static void
ngx_win32_router_rebuild_ready_workers(void)
{
    ngx_uint_t  i;

    ngx_win32_router_ready_worker_n = 0;

    for (i = 0; i < NGX_MAX_PROCESSES; i++) {
        if (ngx_win32_router_workers[i].ready) {
            ngx_win32_router_ready_workers[
                ngx_win32_router_ready_worker_n++] =
                    &ngx_win32_router_workers[i];
        }
    }
}


static ngx_uint_t
ngx_win32_router_worker_eligible(ngx_win32_router_worker_t *worker)
{
    return worker->ready && worker->active
           && worker->queued_messages < NGX_WIN32_ROUTER_CHANNEL_MESSAGES
           && worker->queued_bytes < NGX_WIN32_ROUTER_CHANNEL_LIMIT;
}


static ngx_win32_router_worker_t *
ngx_win32_router_select_worker(uint32_t hash, ngx_uint_t round_robin)
{
    ngx_uint_t                  i, n, start;
    ngx_win32_router_worker_t  *best, *worker;

    if (ngx_win32_router_ready_worker_n == 0) {
        return NULL;
    }

    if (round_robin) {
        best = NULL;

        /*
         * Accepted connections carry no affinity, so they go to the least
         * loaded worker.  Rotating the starting point keeps equally loaded
         * workers from all receiving the same connection, which matters
         * while the queues are still empty.
         */

        start = ngx_win32_router_next_worker++
                % ngx_win32_router_ready_worker_n;

        for (n = 0; n < ngx_win32_router_ready_worker_n; n++) {
            i = (start + n) % ngx_win32_router_ready_worker_n;
            worker = ngx_win32_router_ready_workers[i];

            if (!ngx_win32_router_worker_eligible(worker)) {
                continue;
            }

            if (best == NULL || worker->queued_bytes < best->queued_bytes
                || (worker->queued_bytes == best->queued_bytes
                    && worker->queued_messages < best->queued_messages))
            {
                best = worker;
            }
        }

        return best;
    }

    /*
     * A datagram that starts a new flow is placed by hash so that the flow
     * table, the QUIC route identifier, and this fallback all agree on an
     * owner.  The hash is reduced modulo the whole ready set rather than
     * modulo the eligible subset: using the eligible count would change the
     * divisor whenever any worker hit its queue limit and would therefore
     * remap every later flow onto a different worker.  Only the workers that
     * are actually backlogged are skipped, by probing forward from the
     * hashed position.
     */

    start = hash % ngx_win32_router_ready_worker_n;

    for (n = 0; n < ngx_win32_router_ready_worker_n; n++) {
        i = (start + n) % ngx_win32_router_ready_worker_n;
        worker = ngx_win32_router_ready_workers[i];

        if (ngx_win32_router_worker_eligible(worker)) {
            return worker;
        }
    }

    return NULL;
}


static void
ngx_win32_router_expire_accepts(void)
{
    ULONGLONG                    now;
    ngx_queue_t                 *q, *next;
    ngx_win32_router_accept_t   *accept;

    now = GetTickCount64();

    for (q = ngx_queue_head(&ngx_win32_router_handoffs);
         q != ngx_queue_sentinel(&ngx_win32_router_handoffs);
         q = next)
    {
        next = ngx_queue_next(q);
        accept = ngx_queue_data(q, ngx_win32_router_accept_t, queue);

        if (accept->deadline > now) {
            break;
        }

        if (ngx_win32_router_log) {
            ngx_log_error(NGX_LOG_ERR, ngx_win32_router_log, 0,
                          "worker %P did not acknowledge a routed connection",
                          accept->target);
        }

        ngx_queue_remove(q);
        ngx_queue_remove(&accept->id_queue);
        ngx_win32_router_finish_accept(accept);
    }
}


static void
ngx_win32_router_expire_flows(void)
{
    ULONGLONG              now;
    ngx_queue_t            *q;
    ngx_win32_router_flow_t *flow;

    now = GetTickCount64();

    while (!ngx_queue_empty(&ngx_win32_router_flows)) {
        q = ngx_queue_head(&ngx_win32_router_flows);
        flow = ngx_queue_data(q, ngx_win32_router_flow_t, expires);

        if (flow->deadline > now) {
            break;
        }

        ngx_queue_remove(&flow->expires);
        ngx_queue_remove(&flow->bucket);
        ngx_free(flow);
        ngx_win32_router_flow_n--;
    }
}


static void
ngx_win32_router_finish_accept(ngx_win32_router_accept_t *accept)
{
    if (accept->socket != (ngx_socket_t) -1) {
        (void) ngx_close_socket(accept->socket);
        accept->socket = (ngx_socket_t) -1;
    }

    ngx_free(accept);
}


static void
ngx_win32_router_free_listeners(void)
{
    ngx_queue_t                 *q;
    ngx_win32_router_listener_t *listener;

    while (!ngx_queue_empty(&ngx_win32_router_listeners)) {
        q = ngx_queue_head(&ngx_win32_router_listeners);
        ngx_queue_remove(q);
        listener = ngx_queue_data(q, ngx_win32_router_listener_t, queue);
        ngx_free(listener);
    }
}


static void
ngx_win32_router_free_flows(void)
{
    ngx_queue_t             *q;
    ngx_win32_router_flow_t *flow;

    while (!ngx_queue_empty(&ngx_win32_router_flows)) {
        q = ngx_queue_head(&ngx_win32_router_flows);
        ngx_queue_remove(q);
        flow = ngx_queue_data(q, ngx_win32_router_flow_t, expires);
        ngx_queue_remove(&flow->bucket);
        ngx_free(flow);
    }

    ngx_win32_router_flow_n = 0;
}
