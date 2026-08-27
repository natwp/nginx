
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>
#include <ngx_iocp_module.h>


#define NGX_IOCP_BATCH          64
#define NGX_IOCP_NOTIFY_KEY     1
#define NGX_IOCP_SHUTDOWN_WAIT  5000
#define NGX_IOCP_MAX_ACCEPTS    1024
#define NGX_IOCP_MAX_UDP_RECV   256


typedef struct {
    OVERLAPPED            overlapped;
    ngx_event_handler_pt  handler;
} ngx_iocp_notify_t;


typedef struct {
    ngx_iocp_op_t  op;
    WSABUF         wsabuf;
} ngx_iocp_read_notify_op_t;


typedef struct {
    ngx_iocp_op_t  op;
    WSAEVENT       event;
    PTP_WAIT       wait;
    volatile LONG  posted;
    unsigned       selected:1;
} ngx_iocp_write_notify_op_t;


static ngx_int_t ngx_iocp_init(ngx_cycle_t *cycle, ngx_msec_t timer);
static void ngx_iocp_done(ngx_cycle_t *cycle);
static ngx_int_t ngx_iocp_add_event(ngx_event_t *ev, ngx_int_t event,
    ngx_uint_t flags);
static ngx_int_t ngx_iocp_del_connection(ngx_connection_t *c,
    ngx_uint_t flags);
static ngx_int_t ngx_iocp_notify(ngx_event_handler_pt handler);
static ngx_int_t ngx_iocp_process_events(ngx_cycle_t *cycle,
    ngx_msec_t timer, ngx_uint_t flags);
static ngx_int_t ngx_iocp_process_entry(ngx_cycle_t *cycle,
    OVERLAPPED_ENTRY *entry, ngx_uint_t flags);
static void ngx_iocp_read_notify_complete(ngx_iocp_op_t *op);
static void CALLBACK ngx_iocp_write_notify_callback(
    PTP_CALLBACK_INSTANCE instance, PVOID context, PTP_WAIT wait,
    TP_WAIT_RESULT result);
static void ngx_iocp_write_notify_complete(ngx_iocp_op_t *base);
static void ngx_iocp_write_notify_cleanup(ngx_iocp_op_t *base);
static void ngx_iocp_write_notify_retire(ngx_iocp_write_notify_op_t *op);
static void ngx_iocp_write_notify_cancel(ngx_iocp_write_notify_op_t *op);
static void ngx_iocp_cancel(ngx_iocp_owner_t *owner);
static void ngx_iocp_close_children(ngx_iocp_owner_t *owner);
static void ngx_iocp_finalize_owner(ngx_iocp_owner_t *owner);
static void ngx_iocp_finish_op(ngx_iocp_op_t *op, ngx_uint_t cleanup);
static void ngx_iocp_set_log(ngx_iocp_owner_t *owner, ngx_log_t *log);
static void ngx_iocp_query_extension(ngx_iocp_owner_t *owner, GUID *guid,
    void *target, DWORD size);
static void *ngx_iocp_create_conf(ngx_cycle_t *cycle);
static char *ngx_iocp_init_conf(ngx_cycle_t *cycle, void *conf);


static ngx_str_t      iocp_name = ngx_string("iocp");

static ngx_command_t  ngx_iocp_commands[] = {

    { ngx_string("iocp_threads"),
      NGX_EVENT_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_num_slot,
      0,
      offsetof(ngx_iocp_conf_t, threads),
      NULL },

    { ngx_string("post_acceptex"),
      NGX_EVENT_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_num_slot,
      0,
      offsetof(ngx_iocp_conf_t, post_acceptex),
      NULL },

    { ngx_string("acceptex_read"),
      NGX_EVENT_CONF|NGX_CONF_FLAG,
      ngx_conf_set_flag_slot,
      0,
      offsetof(ngx_iocp_conf_t, acceptex_read),
      NULL },

    { ngx_string("iocp_udp_receives"),
      NGX_EVENT_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_num_slot,
      0,
      offsetof(ngx_iocp_conf_t, udp_receives),
      NULL },

      ngx_null_command
};


static ngx_event_module_t  ngx_iocp_module_ctx = {
    &iocp_name,
    ngx_iocp_create_conf,                  /* create configuration */
    ngx_iocp_init_conf,                    /* init configuration */

    {
        ngx_iocp_add_event,                /* add an event */
        NULL,                              /* delete an event */
        NULL,                              /* enable an event */
        NULL,                              /* disable an event */
        ngx_iocp_add_connection,           /* add a connection */
        ngx_iocp_del_connection,           /* delete a connection */
        ngx_iocp_notify,                   /* trigger a notify */
        ngx_iocp_process_events,           /* process the events */
        ngx_iocp_init,                     /* init the events */
        ngx_iocp_done                      /* done the events */
    }
};


ngx_module_t  ngx_iocp_module = {
    NGX_MODULE_V1,
    &ngx_iocp_module_ctx,                  /* module context */
    ngx_iocp_commands,                     /* module directives */
    NGX_EVENT_MODULE,                      /* module type */
    NULL,                                  /* init master */
    NULL,                                  /* init module */
    NULL,                                  /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    ngx_iocp_done,                         /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};


ngx_os_io_t ngx_iocp_io = {
    ngx_overlapped_wsarecv,
    ngx_overlapped_wsarecv_chain,
    ngx_udp_overlapped_wsarecv,
    ngx_overlapped_wsasend,
    ngx_iocp_udp_send,
    ngx_iocp_udp_send_chain,
    ngx_overlapped_wsasend_chain,
    NGX_IO_SENDFILE
};


static HANDLE       iocp;
static ngx_queue_t  ngx_iocp_owners;
static ngx_uint_t   ngx_iocp_generation;
static ngx_uint_t   ngx_iocp_pending;
static ngx_atomic_t ngx_iocp_notifications;
static ngx_msec_t   ngx_iocp_timer_resolution;
static ngx_event_t  ngx_iocp_notify_event;
static u_char        ngx_iocp_zero_byte;

static GUID  iocp_acceptex_guid = WSAID_ACCEPTEX;
static GUID  iocp_getacceptexsockaddrs_guid = WSAID_GETACCEPTEXSOCKADDRS;
static GUID  iocp_connectex_guid = WSAID_CONNECTEX;
static GUID  iocp_transmitfile_guid = WSAID_TRANSMITFILE;
static GUID  iocp_transmitpackets_guid = WSAID_TRANSMITPACKETS;
static GUID  iocp_wsarecvmsg_guid = WSAID_WSARECVMSG;
static GUID  iocp_wsasendmsg_guid = WSAID_WSASENDMSG;


static ngx_int_t
ngx_iocp_init(ngx_cycle_t *cycle, ngx_msec_t timer)
{
    if (iocp == NULL) {
        iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
        if (iocp == NULL) {
            ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                          "CreateIoCompletionPort() failed");
            return NGX_ERROR;
        }

        ngx_queue_init(&ngx_iocp_owners);
        ngx_iocp_generation = 0;
        ngx_iocp_pending = 0;
        ngx_iocp_notifications = 0;
    }

    ngx_memzero(&ngx_iocp_notify_event, sizeof(ngx_event_t));
    ngx_iocp_notify_event.log = cycle->log;
    ngx_iocp_notify_event.active = 1;

    ngx_io = ngx_iocp_io;
    ngx_event_actions = ngx_iocp_module_ctx.actions;
    ngx_event_flags = NGX_USE_IOCP_EVENT;

#if (NGX_HAVE_FILE_AIO)
    ngx_file_aio = 1;
#endif

    ngx_iocp_timer_resolution = timer;
    if (timer) {
        ngx_event_flags |= NGX_USE_TIMER_EVENT;
    }

    return NGX_OK;
}


static void
ngx_iocp_done(ngx_cycle_t *cycle)
{
    ULONGLONG          deadline;
    ngx_pool_t        *pool;
    ngx_queue_t       *q, *next;
    ngx_iocp_owner_t  *owner;

    if (iocp == NULL) {
        return;
    }

#if (NGX_HAVE_FILE_AIO)
    ngx_file_aio = 0;
#endif

    for (q = ngx_queue_head(&ngx_iocp_owners);
         q != ngx_queue_sentinel(&ngx_iocp_owners);
         q = next)
    {
        next = ngx_queue_next(q);
        owner = ngx_queue_data(q, ngx_iocp_owner_t, queue);

        if (owner->port_owner == owner || owner->closing) {
            continue;
        }

        if (owner->connection) {
            pool = owner->connection->pool;
            ngx_iocp_close_connection(owner->connection);

            if (pool) {
                ngx_destroy_pool(pool);
            }

        } else {
            ngx_iocp_close_owner(owner);
        }
    }

    for (q = ngx_queue_head(&ngx_iocp_owners);
         q != ngx_queue_sentinel(&ngx_iocp_owners);
         q = next)
    {
        next = ngx_queue_next(q);
        owner = ngx_queue_data(q, ngx_iocp_owner_t, queue);

        if (owner->port_owner != owner || owner->closing) {
            continue;
        }

        if (owner->connection) {
            ngx_iocp_close_connection(owner->connection);
        } else {
            ngx_iocp_close_owner(owner);
        }
    }

    deadline = GetTickCount64() + NGX_IOCP_SHUTDOWN_WAIT;

    while ((ngx_iocp_pending || ngx_iocp_notifications)
           && GetTickCount64() < deadline)
    {
        (void) ngx_iocp_process_events(cycle, 50, 0);
    }

    if (ngx_iocp_pending) {
        ngx_log_error(NGX_LOG_ALERT, cycle->log, 0,
                      "%ui IOCP operations did not drain during shutdown",
                      ngx_iocp_pending);
    }

    if (ngx_iocp_notifications) {
        ngx_log_error(NGX_LOG_ALERT, cycle->log, 0,
                      "%uA IOCP notifications did not drain during shutdown",
                      ngx_iocp_notifications);
    }

    if (iocp && CloseHandle(iocp) == 0) {
        ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                      "iocp CloseHandle() failed");
    }

    iocp = NULL;
}


HANDLE
ngx_iocp_port(void)
{
    return iocp;
}


ngx_iocp_owner_t *
ngx_iocp_create_owner(HANDLE handle, ngx_connection_t *c, ngx_log_t *log,
    ngx_uint_t socket, ngx_uint_t shared)
{
    ngx_iocp_owner_t  *owner;

    if (log == NULL) {
        return NULL;
    }

    owner = ngx_calloc(sizeof(ngx_iocp_owner_t), log);
    if (owner == NULL) {
        return NULL;
    }

    owner->handle = handle;
    owner->port_owner = owner;
    owner->connection = c;
    owner->pool = c ? c->pool : NULL;

    ngx_iocp_set_log(owner, log);
    owner->generation = ++ngx_iocp_generation;
    owner->socket = socket ? 1 : 0;
    owner->shared = shared ? 1 : 0;

    ngx_queue_init(&owner->operations);
    ngx_queue_insert_tail(&ngx_iocp_owners, &owner->queue);

    if (c) {
        c->iocp = owner;
    }

    return owner;
}


ngx_iocp_owner_t *
ngx_iocp_create_shared_owner(ngx_connection_t *c,
    ngx_iocp_owner_t *port_owner)
{
    ngx_iocp_owner_t  *owner;

    if (c == NULL || port_owner == NULL || port_owner->closing
        || !port_owner->associated || !port_owner->socket)
    {
        return NULL;
    }

    owner = ngx_iocp_create_owner(port_owner->handle, c, c->log, 1, 1);
    if (owner == NULL) {
        return NULL;
    }

    owner->port_owner = port_owner;
    owner->associated = 1;
    owner->acceptex = port_owner->acceptex;
    owner->getacceptexsockaddrs = port_owner->getacceptexsockaddrs;
    owner->connectex = port_owner->connectex;
    owner->transmitfile = port_owner->transmitfile;
    owner->transmitpackets = port_owner->transmitpackets;
    owner->recvmsg = port_owner->recvmsg;
    owner->sendmsg = port_owner->sendmsg;

    port_owner->children++;

    return owner;
}


ngx_int_t
ngx_iocp_associate(ngx_iocp_owner_t *owner)
{
    if (owner->associated) {
        return NGX_OK;
    }

    if (CreateIoCompletionPort(owner->handle, iocp, (ULONG_PTR) owner, 0)
        == NULL)
    {
        ngx_log_error(NGX_LOG_ALERT, owner->log, ngx_errno,
                      "CreateIoCompletionPort() failed");
        return NGX_ERROR;
    }

    owner->associated = 1;
    ngx_iocp_load_extensions(owner);

    return NGX_OK;
}


void
ngx_iocp_load_extensions(ngx_iocp_owner_t *owner)
{
    if (!owner->socket || owner->handle == INVALID_HANDLE_VALUE) {
        return;
    }

    ngx_iocp_query_extension(owner, &iocp_acceptex_guid, &owner->acceptex,
                             sizeof(owner->acceptex));
    ngx_iocp_query_extension(owner, &iocp_getacceptexsockaddrs_guid,
                             &owner->getacceptexsockaddrs,
                             sizeof(owner->getacceptexsockaddrs));
    ngx_iocp_query_extension(owner, &iocp_connectex_guid, &owner->connectex,
                             sizeof(owner->connectex));
    ngx_iocp_query_extension(owner, &iocp_transmitfile_guid,
                             &owner->transmitfile, sizeof(owner->transmitfile));
    ngx_iocp_query_extension(owner, &iocp_transmitpackets_guid,
                             &owner->transmitpackets,
                             sizeof(owner->transmitpackets));
    ngx_iocp_query_extension(owner, &iocp_wsarecvmsg_guid, &owner->recvmsg,
                             sizeof(owner->recvmsg));
    ngx_iocp_query_extension(owner, &iocp_wsasendmsg_guid, &owner->sendmsg,
                             sizeof(owner->sendmsg));
}


static void
ngx_iocp_set_log(ngx_iocp_owner_t *owner, ngx_log_t *log)
{
    owner->safe_log = *log;
    owner->safe_log.handler = NULL;
    owner->safe_log.data = NULL;
    owner->safe_log.action = NULL;
    owner->safe_log.next = NULL;
    owner->log = &owner->safe_log;
}


static void
ngx_iocp_query_extension(ngx_iocp_owner_t *owner, GUID *guid, void *target,
    DWORD size)
{
    DWORD  bytes;

    (void) WSAIoctl((SOCKET) owner->handle,
                    SIO_GET_EXTENSION_FUNCTION_POINTER,
                    guid, sizeof(GUID), target, size, &bytes, NULL, NULL);
}


ngx_int_t
ngx_iocp_add_connection(ngx_connection_t *c)
{
    ngx_iocp_owner_t  *owner;

    owner = c->iocp;

    if (owner == NULL) {
        owner = ngx_iocp_create_owner((HANDLE) c->fd, c, c->log, 1,
                                      c->shared);
        if (owner == NULL) {
            return NGX_ERROR;
        }
    }

    if (ngx_iocp_associate(owner) != NGX_OK) {
        c->iocp = NULL;
        ngx_queue_remove(&owner->queue);
        ngx_free(owner);
        return NGX_ERROR;
    }

    if (c->type == SOCK_STREAM && c->sendfile
        && owner->transmitfile == NULL && owner->transmitpackets == NULL)
    {
        c->sendfile = 0;
    }

    c->read->active = 1;
    c->write->active = 1;

    ngx_log_debug2(NGX_LOG_DEBUG_EVENT, c->log, 0,
                   "iocp add connection: fd:%d owner:%p", c->fd, owner);

    return NGX_OK;
}


ngx_int_t
ngx_iocp_post_read(ngx_event_t *rev)
{
    int                         rc;
    ngx_err_t                   err;
    ngx_pool_t                 *pool;
    ngx_connection_t           *c;
    ngx_iocp_read_notify_op_t  *op;

    c = rev->data;

    if (rev->closed || c == NULL || c->fd == (ngx_socket_t) -1) {
        return NGX_OK;
    }

    if (rev->handler == NULL || rev->posted || rev->iocp_op) {
        return NGX_OK;
    }

    if (rev->complete || rev->iocp_buffer || rev->iocp_error)
    {
        ngx_post_event(rev, &ngx_posted_events);
        return NGX_OK;
    }

    if (rev->ready) {
        return NGX_OK;
    }

    /*
     * A zero-byte receive is a readiness notification for stream sockets.
     * It does not consume data, which is required by SSL and MSG_PEEK users.
     * Datagram handlers post a real receive so zero-length datagrams retain
     * their normal semantics.
     */
    if (c->type != SOCK_STREAM) {
        return NGX_OK;
    }

    if (c->iocp == NULL && ngx_iocp_add_connection(c) != NGX_OK) {
        rev->error = 1;
        return NGX_ERROR;
    }

    pool = rev->iocp_pool ? rev->iocp_pool : c->pool;

    op = (ngx_iocp_read_notify_op_t *)
         ngx_iocp_op_create(sizeof(ngx_iocp_read_notify_op_t), c->iocp, rev,
                            pool, NGX_IOCP_OP_READ_NOTIFY,
                            ngx_iocp_read_notify_complete, NULL);
    if (op == NULL) {
        rev->error = 1;
        return NGX_ERROR;
    }

    op->wsabuf.buf = (char *) &ngx_iocp_zero_byte;
    op->wsabuf.len = 0;
    op->op.flags = 0;
    op->op.bytes = 0;
    op->op.expected = 0;

    rc = WSARecv(c->fd, &op->wsabuf, 1, &op->op.bytes, &op->op.flags,
                 &op->op.overlapped, NULL);

    rev->active = 1;
    rev->ready = 0;
    rev->complete = 0;

    if (rc == 0) {
        return NGX_OK;
    }

    err = ngx_socket_errno;
    if (err == WSA_IO_PENDING) {
        return NGX_OK;
    }

    rev->active = 0;
    rev->ready = 1;
    rev->error = 1;
    ngx_iocp_op_abort(&op->op);
    (void) ngx_connection_error(c, err, "zero-byte WSARecv() failed");

    return NGX_ERROR;
}


ngx_int_t
ngx_iocp_post_write(ngx_event_t *wev)
{
    ngx_err_t                    err;
    ngx_pool_t                  *pool;
    ngx_connection_t            *c;
    ngx_iocp_write_notify_op_t  *op;

    c = wev->data;

    if (wev->closed || c == NULL || c->fd == (ngx_socket_t) -1) {
        return NGX_OK;
    }

    if (wev->handler == NULL || wev->posted || wev->iocp_op) {
        return NGX_OK;
    }

    if (wev->ready || wev->complete || wev->iocp_error) {
        return NGX_OK;
    }

    /*
     * Overlapped sends provide their own completion.  This bridge is for
     * consumers such as the OpenSSL socket BIO which use nonblocking send()
     * and need an FD_WRITE notification after WSAEWOULDBLOCK.
     */
    if (c->type != SOCK_STREAM) {
        return NGX_OK;
    }

    if (c->iocp == NULL && ngx_iocp_add_connection(c) != NGX_OK) {
        wev->error = 1;
        return NGX_ERROR;
    }

    pool = wev->iocp_pool ? wev->iocp_pool : c->pool;

    op = (ngx_iocp_write_notify_op_t *)
         ngx_iocp_op_create(sizeof(ngx_iocp_write_notify_op_t), c->iocp,
                            wev, pool, NGX_IOCP_OP_WRITE_NOTIFY,
                            ngx_iocp_write_notify_complete,
                            ngx_iocp_write_notify_cleanup);
    if (op == NULL) {
        wev->error = 1;
        return NGX_ERROR;
    }

    op->event = WSACreateEvent();
    if (op->event == WSA_INVALID_EVENT) {
        err = ngx_socket_errno;
        ngx_iocp_op_abort(&op->op);
        wev->error = 1;
        ngx_connection_error(c, err, "WSACreateEvent() failed");
        return NGX_ERROR;
    }

    op->wait = CreateThreadpoolWait(ngx_iocp_write_notify_callback, op,
                                    NULL);
    if (op->wait == NULL) {
        err = ngx_errno;
        ngx_iocp_op_abort(&op->op);
        wev->error = 1;
        ngx_connection_error(c, err, "CreateThreadpoolWait() failed");
        return NGX_ERROR;
    }

    if (WSAEventSelect(c->fd, op->event, FD_WRITE|FD_CLOSE) == SOCKET_ERROR) {
        err = ngx_socket_errno;
        ngx_iocp_op_abort(&op->op);
        wev->error = 1;
        ngx_connection_error(c, err, "WSAEventSelect() failed");
        return NGX_ERROR;
    }

    op->selected = 1;
    wev->active = 1;
    wev->ready = 0;
    wev->complete = 0;

    SetThreadpoolWait(op->wait, op->event, NULL);

    return NGX_OK;
}


static void CALLBACK
ngx_iocp_write_notify_callback(PTP_CALLBACK_INSTANCE instance, PVOID context,
    PTP_WAIT wait, TP_WAIT_RESULT result)
{
    ngx_iocp_write_notify_op_t  *op;

    op = context;

    (void) instance;
    (void) wait;
    (void) result;

    if (InterlockedCompareExchange(&op->posted, 1, 0) != 0) {
        return;
    }

    if (PostQueuedCompletionStatus(iocp, 0,
                                   (ULONG_PTR) op->op.completion_owner,
                                   &op->op.overlapped)
        == 0)
    {
        InterlockedExchange(&op->posted, 0);
    }
}


static void
ngx_iocp_write_notify_retire(ngx_iocp_write_notify_op_t *op)
{
    if (op->wait) {
        SetThreadpoolWait(op->wait, NULL, NULL);
        WaitForThreadpoolWaitCallbacks(op->wait, TRUE);
        CloseThreadpoolWait(op->wait);
        op->wait = NULL;
    }
}


static void
ngx_iocp_write_notify_complete(ngx_iocp_op_t *base)
{
    long                        events;
    ngx_err_t                   err;
    ngx_event_t                *wev;
    ngx_connection_t           *c;
    WSANETWORKEVENTS            nevents;
    ngx_iocp_write_notify_op_t  *op;

    op = (ngx_iocp_write_notify_op_t *) base;
    wev = base->event;
    c = base->owner->connection;
    err = base->error;

    ngx_iocp_write_notify_retire(op);

    ngx_memzero(&nevents, sizeof(WSANETWORKEVENTS));

    if (err == 0
        && WSAEnumNetworkEvents(c->fd, op->event, &nevents) == SOCKET_ERROR)
    {
        err = ngx_socket_errno;
    }

    events = nevents.lNetworkEvents;

    if (err == 0 && (events & FD_WRITE)
        && nevents.iErrorCode[FD_WRITE_BIT])
    {
        err = nevents.iErrorCode[FD_WRITE_BIT];
    }

    if (err == 0 && (events & FD_CLOSE)
        && nevents.iErrorCode[FD_CLOSE_BIT])
    {
        err = nevents.iErrorCode[FD_CLOSE_BIT];
    }

    if (op->selected) {
        if (WSAEventSelect(c->fd, NULL, 0) == SOCKET_ERROR && err == 0) {
            err = ngx_socket_errno;
        }

        op->selected = 0;
    }

    wev->iocp_op = NULL;
    wev->iocp_error = err;
    wev->active = 0;
    wev->ready = 1;
    wev->complete = 0;

    if (err) {
        wev->error = 1;

#if (NGX_SSL)
        if (c->ssl) {
            wev->iocp_error = 0;
            (void) ngx_connection_error(c, err,
                                      "IOCP write notification failed");
        }
#endif
    }

    if (wev->handler) {
        wev->handler(wev);
    }
}


static void
ngx_iocp_write_notify_cleanup(ngx_iocp_op_t *base)
{
    ngx_iocp_write_notify_op_t  *op;

    op = (ngx_iocp_write_notify_op_t *) base;

    ngx_iocp_write_notify_retire(op);

    if (op->selected) {
        if (base->owner->handle != INVALID_HANDLE_VALUE) {
            (void) WSAEventSelect((SOCKET) base->owner->handle, NULL, 0);
        }

        op->selected = 0;
    }

    if (op->event != WSA_INVALID_EVENT) {
        (void) WSACloseEvent(op->event);
        op->event = WSA_INVALID_EVENT;
    }
}


static void
ngx_iocp_write_notify_cancel(ngx_iocp_write_notify_op_t *op)
{
    ngx_iocp_write_notify_retire(op);

    if (op->selected) {
        if (op->op.owner->handle != INVALID_HANDLE_VALUE) {
            (void) WSAEventSelect((SOCKET) op->op.owner->handle, NULL, 0);
        }

        op->selected = 0;
    }

    if (InterlockedCompareExchange(&op->posted, 1, 0) == 0) {
        if (PostQueuedCompletionStatus(iocp, 0,
                                      (ULONG_PTR) op->op.completion_owner,
                                      &op->op.overlapped)
            == 0)
        {
            InterlockedExchange(&op->posted, 0);
            ngx_log_error(NGX_LOG_ALERT, op->op.owner->log, ngx_errno,
                          "PostQueuedCompletionStatus() failed");
        }
    }
}


static void
ngx_iocp_read_notify_complete(ngx_iocp_op_t *op)
{
    ngx_event_t  *rev;

    rev = op->event;

    rev->iocp_op = NULL;
    rev->iocp_error = op->error;
    rev->active = 0;
    rev->ready = 1;
    /* a zero-byte receive does not report the queued byte count */
    rev->available = -1;
    rev->complete = 0;

    if (op->error) {
        rev->error = 1;
    }

    if (rev->handler) {
        rev->handler(rev);
    }
}


static ngx_int_t
ngx_iocp_add_event(ngx_event_t *ev, ngx_int_t event, ngx_uint_t flags)
{
    ngx_connection_t  *c;

    (void) event;
    (void) flags;

    c = ev->data;

    return ngx_iocp_add_connection(c);
}


static ngx_int_t
ngx_iocp_del_connection(ngx_connection_t *c, ngx_uint_t flags)
{
    (void) flags;

    c->read->active = 0;
    c->write->active = 0;

    return NGX_OK;
}


ngx_iocp_op_t *
ngx_iocp_op_create(size_t size, ngx_iocp_owner_t *owner, ngx_event_t *event,
    ngx_pool_t *data_pool, ngx_uint_t type, ngx_iocp_completion_pt handler,
    ngx_iocp_cleanup_pt cleanup)
{
    ngx_iocp_op_t  *op;

    if (owner == NULL || owner->closing || !owner->associated
        || size < sizeof(ngx_iocp_op_t))
    {
        return NULL;
    }

    if (owner->connection && owner->connection->pool) {
        owner->pool = owner->connection->pool;
        ngx_iocp_set_log(owner, owner->connection->log);
    }

    op = ngx_calloc(size, owner->log);
    if (op == NULL) {
        return NULL;
    }

    op->owner = owner;
    op->completion_owner = owner->port_owner;
    op->event = event;
    op->handler = handler;
    op->cleanup = cleanup;
    op->generation = owner->generation;
    op->type = type;

    op->owner_pool = owner->pool;

    if (op->owner_pool && ngx_pool_hold(op->owner_pool) != NGX_OK) {
        ngx_free(op);
        return NULL;
    }

    if (data_pool && data_pool != op->owner_pool) {
        if (ngx_pool_hold(data_pool) != NGX_OK) {
            ngx_pool_release(op->owner_pool);
            ngx_free(op);
            return NULL;
        }

        op->data_pool = data_pool;
    }

    ngx_queue_insert_tail(&owner->operations, &op->queue);
    op->linked = 1;
    owner->pending++;
    ngx_iocp_pending++;

    if (event) {
        event->iocp_op = op;
    }

    return op;
}


void
ngx_iocp_op_abort(ngx_iocp_op_t *op)
{
    ngx_iocp_finish_op(op, 1);
}


static void
ngx_iocp_finish_op(ngx_iocp_op_t *op, ngx_uint_t cleanup)
{
    ngx_uint_t         finalize;
    ngx_pool_t        *owner_pool, *data_pool;
    ngx_iocp_owner_t  *owner;

    owner = op->owner;
    owner_pool = op->owner_pool;
    data_pool = op->data_pool;

    if (op->event && op->event->iocp_op == op) {
        op->event->iocp_op = NULL;
    }

    if (cleanup && op->cleanup) {
        op->cleanup(op);
    }

    if (op->linked) {
        ngx_queue_remove(&op->queue);
        op->linked = 0;
        owner->pending--;
        ngx_iocp_pending--;
    }

    finalize = owner->closing && owner->pending == 0 && owner->children == 0;

    if (finalize) {
        ngx_iocp_finalize_owner(owner);
    }

    if (data_pool) {
        ngx_pool_release(data_pool);
    }

    if (owner_pool) {
        ngx_pool_release(owner_pool);
    }

    ngx_free(op);
}


static void
ngx_iocp_cancel(ngx_iocp_owner_t *owner)
{
    ngx_err_t      err;
    ngx_queue_t   *q;
    ngx_iocp_op_t *op;

    if (owner->handle == INVALID_HANDLE_VALUE) {
        return;
    }

    for (q = ngx_queue_head(&owner->operations);
         q != ngx_queue_sentinel(&owner->operations);
         q = ngx_queue_next(q))
    {
        op = ngx_queue_data(q, ngx_iocp_op_t, queue);

        if (op->completing || op->cancel_requested) {
            continue;
        }

        if (op->type == NGX_IOCP_OP_WRITE_NOTIFY) {
            op->cancel_requested = 1;
            ngx_iocp_write_notify_cancel(
                                      (ngx_iocp_write_notify_op_t *) op);
            continue;
        }

        /*
         * A datagram is owned by the kernel once WSASendMsg()/WSASendTo()
         * accepts it.  Logical UDP and QUIC connections share the listener
         * socket, so let an already submitted response drain even when the
         * logical connection is closed immediately afterwards.
         */
        if (owner->shared && op->type == NGX_IOCP_OP_UDP_SEND) {
            continue;
        }

        op->cancel_requested = 1;

        if (CancelIoEx(owner->handle, &op->overlapped) == 0) {
            err = ngx_errno;

            if (err != ERROR_NOT_FOUND) {
                ngx_log_error(NGX_LOG_ALERT, owner->log, err,
                              "CancelIoEx() failed");
            }
        }

    }
}


static void
ngx_iocp_close_children(ngx_iocp_owner_t *owner)
{
    ngx_pool_t        *pool;
    ngx_queue_t       *q, *next;
    ngx_iocp_owner_t  *child;

    for (q = ngx_queue_head(&ngx_iocp_owners);
         q != ngx_queue_sentinel(&ngx_iocp_owners);
         q = next)
    {
        next = ngx_queue_next(q);
        child = ngx_queue_data(q, ngx_iocp_owner_t, queue);

        if (child == owner || child->port_owner != owner || child->closing) {
            continue;
        }

        if (child->connection) {
            pool = child->connection->pool;
            ngx_iocp_close_connection(child->connection);

            if (pool) {
                ngx_destroy_pool(pool);
            }

        } else {
            ngx_iocp_close_owner(child);
        }
    }
}


void
ngx_iocp_close_connection(ngx_connection_t *c)
{
    ngx_err_t          err;
    ngx_socket_t       fd;
    ngx_iocp_owner_t  *owner;

    owner = c->iocp;
    if (owner == NULL || owner->closing) {
        return;
    }

    if (owner->port_owner == owner && owner->children) {
        ngx_iocp_close_children(owner);
    }

    owner->closing = 1;
    c->read->active = 0;
    c->write->active = 0;

    if (c->shared && c->type == SOCK_DGRAM && c->udp
#if (NGX_QUIC)
        && (c->listening == NULL || !c->listening->quic)
#endif
       )
    {
        ngx_delete_udp_connection(c);
    }

    if (c->read->iocp_buffer && !c->read->iocp_direct_recv
        && !c->read->iocp_direct_chain)
    {
        ngx_free(c->read->iocp_buffer);
    }
    c->read->iocp_buffer = NULL;
    c->read->iocp_buffer_size = 0;
    c->read->iocp_buffer_pos = 0;
    c->read->iocp_chain = NULL;
    c->read->iocp_direct_recv = 0;
    c->read->iocp_direct_chain = 0;

    if (c->write->iocp_buffer) {
        ngx_free(c->write->iocp_buffer);
    }
    c->write->iocp_buffer = NULL;
    c->write->iocp_buffer_size = 0;
    c->write->iocp_buffer_pos = 0;

    ngx_iocp_cancel(owner);

    fd = c->fd;

    if (ngx_cycle->files && fd != (ngx_socket_t) -1
        && ngx_cycle->files[fd] == c)
    {
        ngx_cycle->files[fd] = NULL;
    }

    c->fd = (ngx_socket_t) -1;

    if (!c->shared && fd != (ngx_socket_t) -1) {
        if (ngx_close_socket(fd) == -1) {
            err = ngx_socket_errno;
            ngx_log_error(NGX_LOG_CRIT, c->log, err,
                          ngx_close_socket_n " %d failed", fd);
        }
    }

    owner->handle = INVALID_HANDLE_VALUE;

    if (owner->pending == 0 && owner->children == 0) {
        ngx_iocp_finalize_owner(owner);
    }
}


void
ngx_iocp_close_owner(ngx_iocp_owner_t *owner)
{
    if (owner == NULL || owner->closing) {
        return;
    }

    owner->closing = 1;
    ngx_iocp_cancel(owner);

    if (!owner->shared && owner->handle != INVALID_HANDLE_VALUE) {
        if (CloseHandle(owner->handle) == 0) {
            ngx_log_error(NGX_LOG_ALERT, owner->log, ngx_errno,
                          "CloseHandle() failed");
        }
    }

    owner->handle = INVALID_HANDLE_VALUE;

    if (owner->pending == 0 && owner->children == 0) {
        ngx_iocp_finalize_owner(owner);
    }
}


static void
ngx_iocp_finalize_owner(ngx_iocp_owner_t *owner)
{
    ngx_connection_t  *c;
    ngx_iocp_owner_t  *port_owner;

    ngx_queue_remove(&owner->queue);

    port_owner = owner->port_owner;

    if (owner->cleanup) {
        owner->cleanup(owner);
    }

    c = owner->connection;
    if (c) {
        c->iocp = NULL;
        ngx_free_connection(c);
    }

    ngx_free(owner);

    if (port_owner != owner) {
        port_owner->children--;

        if (port_owner->closing && port_owner->pending == 0
            && port_owner->children == 0)
        {
            ngx_iocp_finalize_owner(port_owner);
        }
    }
}


void
ngx_iocp_event_complete(ngx_iocp_op_t *op)
{
    ngx_event_t  *ev;

    ev = op->event;
    if (ev == NULL) {
        return;
    }

    ev->iocp_op = NULL;
    ev->iocp_error = op->error;
    ev->iocp_expected = op->expected;
    ev->iocp_bytes = op->bytes;
    ev->available = (int) ngx_min(op->bytes, NGX_MAX_INT32_VALUE);
    ev->active = 0;
    ev->complete = 1;
    ev->ready = 1;

    if (ev->handler == NULL) {
        return;
    }

    ev->handler(ev);
}


static ngx_int_t
ngx_iocp_notify(ngx_event_handler_pt handler)
{
    ngx_iocp_notify_t  *notify;

    notify = ngx_calloc(sizeof(ngx_iocp_notify_t), ngx_cycle->log);
    if (notify == NULL) {
        return NGX_ERROR;
    }

    notify->handler = handler;

    (void) ngx_atomic_fetch_add(&ngx_iocp_notifications, 1);

    if (PostQueuedCompletionStatus(iocp, 0, NGX_IOCP_NOTIFY_KEY,
                                   &notify->overlapped)
        == 0)
    {
        (void) ngx_atomic_fetch_add(&ngx_iocp_notifications, -1);
        ngx_log_error(NGX_LOG_ALERT, ngx_cycle->log, ngx_errno,
                      "PostQueuedCompletionStatus() failed");
        ngx_free(notify);
        return NGX_ERROR;
    }

    return NGX_OK;
}


static ngx_int_t
ngx_iocp_process_events(ngx_cycle_t *cycle, ngx_msec_t timer, ngx_uint_t flags)
{
    BOOL              rc;
    ULONG             i, events;
    DWORD             timeout;
    ngx_err_t         err;
    OVERLAPPED_ENTRY  entries[NGX_IOCP_BATCH];

    if (timer == NGX_TIMER_INFINITE) {
        timeout = ngx_iocp_timer_resolution ? ngx_iocp_timer_resolution
                                            : INFINITE;
    } else {
        timeout = (DWORD) timer;
    }

    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, cycle->log, 0,
                   "iocp timer: %ul", timeout);

    events = 0;
    rc = GetQueuedCompletionStatusEx(iocp, entries, NGX_IOCP_BATCH, &events,
                                     timeout, FALSE);

    if (flags & NGX_UPDATE_TIME || ngx_iocp_timer_resolution) {
        ngx_time_update();
    }

    if (rc == 0) {
        err = ngx_errno;

        if (err == WAIT_TIMEOUT) {
            return NGX_OK;
        }

        ngx_log_error(NGX_LOG_ALERT, cycle->log, err,
                      "GetQueuedCompletionStatusEx() failed");
        return NGX_ERROR;
    }

    for (i = 0; i < events; i++) {
        if (ngx_iocp_process_entry(cycle, &entries[i], flags) != NGX_OK) {
            return NGX_ERROR;
        }
    }

    return NGX_OK;
}


static ngx_int_t
ngx_iocp_process_entry(ngx_cycle_t *cycle, OVERLAPPED_ENTRY *entry,
    ngx_uint_t flags)
{
    BOOL                rc;
    DWORD               bytes, op_flags;
    ngx_iocp_op_t      *op;
    ngx_iocp_owner_t   *owner;
    ngx_iocp_owner_t   *port_owner;
    ngx_iocp_notify_t  *notify;

    (void) flags;

    if (entry->lpCompletionKey == NGX_IOCP_NOTIFY_KEY) {
        notify = (ngx_iocp_notify_t *) entry->lpOverlapped;

        (void) ngx_atomic_fetch_add(&ngx_iocp_notifications, -1);

        if (notify == NULL || notify->handler == NULL) {
            ngx_log_error(NGX_LOG_ALERT, cycle->log, 0,
                          "IOCP returned an invalid notification");
            return NGX_ERROR;
        }

        notify->handler(&ngx_iocp_notify_event);
        ngx_free(notify);
        return NGX_OK;
    }

    port_owner = (ngx_iocp_owner_t *) entry->lpCompletionKey;
    op = (ngx_iocp_op_t *) entry->lpOverlapped;

    if (port_owner == NULL || op == NULL
        || op->completion_owner != port_owner || op->owner == NULL
        || op->generation != op->owner->generation || !op->linked)
    {
        ngx_log_error(NGX_LOG_ALERT, cycle->log, 0,
                      "IOCP returned a stale operation");
        return NGX_ERROR;
    }

    owner = op->owner;
    op->completing = 1;
    op->bytes = entry->dwNumberOfBytesTransferred;
    op->error = 0;

    if (owner->closing || port_owner->closing) {
        op->error = ERROR_OPERATION_ABORTED;

    } else if (op->type == NGX_IOCP_OP_WRITE_NOTIFY) {
        /* posted by the thread-pool wait callback */

    } else if (owner->socket) {
        bytes = op->bytes;
        op_flags = op->flags;

        rc = WSAGetOverlappedResult((SOCKET) owner->handle,
                                    &op->overlapped, &bytes, FALSE, &op_flags);
        if (rc == 0) {
            op->error = ngx_socket_errno;
        } else {
            op->bytes = bytes;
            op->flags = op_flags;
        }

    } else {
        bytes = op->bytes;

        rc = GetOverlappedResult(owner->handle, &op->overlapped, &bytes,
                                 FALSE);
        if (rc == 0) {
            op->error = ngx_errno;
        } else {
            op->bytes = bytes;
        }
    }

    ngx_log_debug5(NGX_LOG_DEBUG_EVENT, owner->log, op->error,
                   "iocp complete: owner:%p op:%p type:%ui bytes:%ul flags:%ul",
                   owner, op, op->type, op->bytes, op->flags);

    if (!owner->closing && op->handler) {
        op->handler(op);
    }

    ngx_iocp_finish_op(op, 1);

    return NGX_OK;
}


static void *
ngx_iocp_create_conf(ngx_cycle_t *cycle)
{
    ngx_iocp_conf_t  *cf;

    cf = ngx_palloc(cycle->pool, sizeof(ngx_iocp_conf_t));
    if (cf == NULL) {
        return NULL;
    }

    cf->threads = NGX_CONF_UNSET;
    cf->post_acceptex = NGX_CONF_UNSET;
    cf->acceptex_read = NGX_CONF_UNSET;
    cf->udp_receives = NGX_CONF_UNSET;

    return cf;
}


static char *
ngx_iocp_init_conf(ngx_cycle_t *cycle, void *conf)
{
    ngx_iocp_conf_t *cf = conf;

    ngx_conf_init_value(cf->threads, 0);
    ngx_conf_init_value(cf->post_acceptex, 10);
    ngx_conf_init_value(cf->acceptex_read, 1);
    ngx_conf_init_value(cf->udp_receives, 8);

    if (cf->threads < 0) {
        ngx_log_error(NGX_LOG_EMERG, cycle->log, 0,
                      "iocp_threads must not be negative");
        return NGX_CONF_ERROR;
    }

    if (cf->threads > 1) {
        ngx_log_error(NGX_LOG_EMERG, cycle->log, 0,
                      "iocp_threads must be 0 or 1; IOCP uses one completion "
                      "dispatcher per worker process");
        return NGX_CONF_ERROR;
    }

    if (cf->post_acceptex < 1
        || cf->post_acceptex > NGX_IOCP_MAX_ACCEPTS)
    {
        ngx_log_error(NGX_LOG_EMERG, cycle->log, 0,
                      "post_acceptex must be between 1 and %d",
                      NGX_IOCP_MAX_ACCEPTS);
        return NGX_CONF_ERROR;
    }

    if (cf->udp_receives < 1
        || cf->udp_receives > NGX_IOCP_MAX_UDP_RECV)
    {
        ngx_log_error(NGX_LOG_EMERG, cycle->log, 0,
                      "iocp_udp_receives must be between 1 and %d",
                      NGX_IOCP_MAX_UDP_RECV);
        return NGX_CONF_ERROR;
    }

    return NGX_CONF_OK;
}
