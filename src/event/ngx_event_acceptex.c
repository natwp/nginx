
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>


typedef struct {
    ngx_iocp_op_t    op;
    ngx_listening_t *listening;
    ngx_socket_t     socket;
    size_t           buffer_size;
    u_char           buffer[1];
} ngx_iocp_accept_op_t;


static void ngx_event_acceptex_complete(ngx_iocp_op_t *base);
static void ngx_event_acceptex_cleanup(ngx_iocp_op_t *base);
static void ngx_close_iocp_accepted_connection(ngx_connection_t *c);
static ngx_int_t ngx_acceptex_sockaddr_valid(struct sockaddr *sockaddr,
    socklen_t socklen, int family);
static ngx_int_t ngx_acceptex_address_valid(ngx_iocp_accept_op_t *op,
    struct sockaddr *sockaddr, int socklen, int family);


void
ngx_event_acceptex(ngx_event_t *rev)
{
    ngx_log_error(NGX_LOG_ALERT, rev->log, 0,
                  "unexpected direct AcceptEx event");
}


ngx_int_t
ngx_event_post_acceptex(ngx_listening_t *ls, ngx_uint_t n)
{
    int                    rc;
    size_t                 size;
    ngx_err_t              err;
    ngx_uint_t             i;
    ngx_socket_t           s;
    ngx_connection_t      *lc;
    ngx_iocp_accept_op_t  *op;

    lc = ls->connection;

    if (lc == NULL) {
        return NGX_ERROR;
    }

    if (lc->iocp == NULL && ngx_iocp_add_connection(lc) != NGX_OK) {
        return NGX_ERROR;
    }

    if (lc->iocp->acceptex == NULL
        || lc->iocp->getacceptexsockaddrs == NULL)
    {
        ngx_log_error(NGX_LOG_EMERG, &ls->log, 0,
                      "AcceptEx extension functions are unavailable");
        return NGX_ERROR;
    }

    if (ls->sockaddr == NULL || ls->type != SOCK_STREAM
        || ls->socklen <= 0
        || (size_t) ls->socklen > sizeof(ngx_sockaddr_t)
        || (size_t) ls->socklen > NGX_MAX_UINT32_VALUE - 16
        || ls->post_accept_buffer_size > (size_t) NGX_MAX_UINT32_VALUE
        || ls->post_accept_buffer_size
           > NGX_MAX_SIZE_T_VALUE - 2 * ((size_t) ls->socklen + 16)
        || offsetof(ngx_iocp_accept_op_t, buffer)
           > NGX_MAX_SIZE_T_VALUE
              - (ls->post_accept_buffer_size
                 + 2 * ((size_t) ls->socklen + 16)))
    {
        ngx_log_error(NGX_LOG_EMERG, &ls->log, 0,
                      "invalid AcceptEx buffer size for %V",
                      &ls->addr_text);
        return NGX_ERROR;
    }

    switch (ls->sockaddr->sa_family) {

    case AF_INET:
        if (ls->socklen < (socklen_t) sizeof(struct sockaddr_in)) {
            ngx_log_error(NGX_LOG_EMERG, &ls->log, 0,
                          "invalid IPv4 AcceptEx address for %V",
                          &ls->addr_text);
            return NGX_ERROR;
        }
        break;

#if (NGX_HAVE_INET6)
    case AF_INET6:
        if (ls->socklen < (socklen_t) sizeof(struct sockaddr_in6)) {
            ngx_log_error(NGX_LOG_EMERG, &ls->log, 0,
                          "invalid IPv6 AcceptEx address for %V",
                          &ls->addr_text);
            return NGX_ERROR;
        }
        break;
#endif

    default:
        ngx_log_error(NGX_LOG_EMERG, &ls->log, 0,
                      "unsupported AcceptEx address family %d for %V",
                      ls->sockaddr->sa_family, &ls->addr_text);
        return NGX_ERROR;
    }

    size = ls->post_accept_buffer_size + 2 * ((size_t) ls->socklen + 16);

    for (i = 0; i < n; i++) {
        s = ngx_socket(ls->sockaddr->sa_family, ls->type, ls->protocol);

        ngx_log_debug1(NGX_LOG_DEBUG_EVENT, &ls->log, 0,
                       ngx_socket_n " s:%d", s);

        if (s == (ngx_socket_t) -1) {
            ngx_log_error(NGX_LOG_ALERT, &ls->log, ngx_socket_errno,
                          ngx_socket_n " failed");
            return NGX_ERROR;
        }

        op = (ngx_iocp_accept_op_t *)
             ngx_iocp_op_create(offsetof(ngx_iocp_accept_op_t, buffer) + size,
                                lc->iocp, NULL, NULL, NGX_IOCP_OP_ACCEPT,
                                ngx_event_acceptex_complete,
                                ngx_event_acceptex_cleanup);
        if (op == NULL) {
            if (ngx_close_socket(s) == -1) {
                ngx_log_error(NGX_LOG_ALERT, &ls->log, ngx_socket_errno,
                              ngx_close_socket_n " failed");
            }

            return NGX_ERROR;
        }

        op->listening = ls;
        op->socket = s;
        op->buffer_size = size;
        op->op.bytes = 0;

        rc = lc->iocp->acceptex(ls->fd, s, op->buffer,
                          (DWORD) ls->post_accept_buffer_size,
                          (DWORD) ls->socklen + 16,
                          (DWORD) ls->socklen + 16, &op->op.bytes,
                          &op->op.overlapped);

        if (rc == 0) {
            err = ngx_socket_errno;

            if (err != WSA_IO_PENDING) {
                ngx_log_error(NGX_LOG_ALERT, &ls->log, err,
                              "AcceptEx() %V failed", &ls->addr_text);
                ngx_iocp_op_abort(&op->op);
                return NGX_ERROR;
            }
        }
    }

    return NGX_OK;
}


static void
ngx_event_acceptex_complete(ngx_iocp_op_t *base)
{
    int                    local_socklen, socklen;
    size_t                 n;
    ngx_log_t             *log;
    ngx_socket_t           s;
    ngx_connection_t      *c;
    ngx_event_t           *rev;
    ngx_listening_t       *ls;
    struct sockaddr       *local_sockaddr, *sockaddr;
    ngx_iocp_accept_op_t  *op;

    op = (ngx_iocp_accept_op_t *) base;
    ls = op->listening;

    if (ngx_event_post_acceptex(ls, 1) != NGX_OK) {
        ngx_log_error(NGX_LOG_ALERT, &ls->log, 0,
                      "could not replenish AcceptEx queue for %V",
                      &ls->addr_text);
    }

    if (base->error) {
        ngx_log_error(NGX_LOG_ERR, &ls->log, base->error,
                      "AcceptEx() %V failed", &ls->addr_text);
        return;
    }

    if ((size_t) base->bytes > ls->post_accept_buffer_size) {
        ngx_log_error(NGX_LOG_ALERT, &ls->log, 0,
                      "AcceptEx() returned too much preread data for %V",
                      &ls->addr_text);
        return;
    }

    if (setsockopt(op->socket, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
                   (char *) &ls->fd, sizeof(ngx_socket_t))
        == -1)
    {
        ngx_log_error(NGX_LOG_CRIT, &ls->log, ngx_socket_errno,
                      "setsockopt(SO_UPDATE_ACCEPT_CONTEXT) failed for %V",
                      &ls->addr_text);
        return;
    }

    if (ngx_nonblocking(op->socket) == -1) {
        ngx_log_error(NGX_LOG_CRIT, &ls->log, ngx_socket_errno,
                      ngx_nonblocking_n " failed for %V", &ls->addr_text);
        return;
    }

    if (SetHandleInformation((HANDLE) op->socket, HANDLE_FLAG_INHERIT, 0)
        == 0)
    {
        ngx_log_error(NGX_LOG_CRIT, &ls->log, ngx_errno,
                      "SetHandleInformation() failed for %V",
                      &ls->addr_text);
        return;
    }

    local_sockaddr = NULL;
    sockaddr = NULL;
    local_socklen = 0;
    socklen = 0;

    op->op.owner->getacceptexsockaddrs(op->buffer,
                             (DWORD) ls->post_accept_buffer_size,
                             (DWORD) ls->socklen + 16,
                             (DWORD) ls->socklen + 16,
                             &local_sockaddr, &local_socklen,
                             &sockaddr, &socklen);

    s = op->socket;

#if (NGX_STAT_STUB)
    (void) ngx_atomic_fetch_add(ngx_stat_accepted, 1);
#endif

    ngx_accept_disabled = ngx_cycle->connection_n / 8
                          - ngx_cycle->free_connection_n;

    c = ngx_get_connection(s, &ls->log);

    if (c == NULL) {
        return;
    }

    op->socket = (ngx_socket_t) -1;

    c->type = SOCK_STREAM;
    c->pool = ngx_create_pool(ls->pool_size, &ls->log);
    if (c->pool == NULL) {
        ngx_close_iocp_accepted_connection(c);
        return;
    }

    log = ngx_palloc(c->pool, sizeof(ngx_log_t));
    if (log == NULL) {
        ngx_close_iocp_accepted_connection(c);
        return;
    }

    *log = ls->log;
    c->log = log;
    c->pool->log = log;

    if (ngx_acceptex_address_valid(op, local_sockaddr, local_socklen,
                                   ls->sockaddr->sa_family)
        != NGX_OK
        || ngx_acceptex_address_valid(op, sockaddr, socklen,
                                      ls->sockaddr->sa_family)
           != NGX_OK)
    {
        ngx_log_error(NGX_LOG_ALERT, log, 0,
                      "GetAcceptExSockaddrs() returned invalid addresses");
        ngx_close_iocp_accepted_connection(c);
        return;
    }

    c->local_sockaddr = ngx_palloc(c->pool, (size_t) local_socklen);
    c->sockaddr = ngx_palloc(c->pool, (size_t) socklen);

    if (c->local_sockaddr == NULL || c->sockaddr == NULL) {
        ngx_close_iocp_accepted_connection(c);
        return;
    }

    ngx_memcpy(c->local_sockaddr, local_sockaddr, (size_t) local_socklen);
    ngx_memcpy(c->sockaddr, sockaddr, (size_t) socklen);

    c->local_socklen = local_socklen;
    c->socklen = socklen;
    c->listening = ls;

    if (ls->post_accept_buffer_size) {
        c->buffer = ngx_create_temp_buf(c->pool,
                                        ls->post_accept_buffer_size);
        if (c->buffer == NULL) {
            ngx_close_iocp_accepted_connection(c);
            return;
        }

        n = ngx_min((size_t) base->bytes, ls->post_accept_buffer_size);

        if (n) {
            rev = c->read;
            rev->iocp_buffer = ngx_alloc(n, log);
            if (rev->iocp_buffer == NULL) {
                ngx_close_iocp_accepted_connection(c);
                return;
            }

            ngx_memcpy(rev->iocp_buffer, op->buffer, n);
            rev->iocp_buffer_size = n;
            rev->iocp_buffer_pos = 0;
            rev->available = (int) ngx_min(n, NGX_MAX_INT32_VALUE);
        }
    }

    c->recv = ngx_recv;
    c->send = ngx_send;
    c->recv_chain = ngx_recv_chain;
    c->send_chain = ngx_send_chain;
    c->sendfile = 1;

    c->read->ready = 1;
    c->write->ready = 1;
    c->read->log = log;
    c->write->log = log;

    if (ls->addr_ntop) {
        c->addr_text.data = ngx_pnalloc(c->pool, ls->addr_text_max_len);
        if (c->addr_text.data == NULL) {
            ngx_close_iocp_accepted_connection(c);
            return;
        }

        c->addr_text.len = ngx_sock_ntop(c->sockaddr, c->socklen,
                                         c->addr_text.data,
                                         ls->addr_text_max_len, 0);
        if (c->addr_text.len == 0) {
            ngx_close_iocp_accepted_connection(c);
            return;
        }
    }

    if (ngx_iocp_add_connection(c) != NGX_OK) {
        ngx_close_iocp_accepted_connection(c);
        return;
    }

    (void) ngx_iocp_enable_skip_completion(c);

    c->number = ngx_atomic_fetch_add(ngx_connection_counter, 1);
    c->start_time = ngx_current_msec;

#if (NGX_DEBUG)
    {
        ngx_event_conf_t  *ecf;

        ecf = ngx_event_get_conf(ngx_cycle->conf_ctx, ngx_event_core_module);
        ngx_debug_accepted_connection(ecf, c);
    }
#endif

#if (NGX_STAT_STUB)
    (void) ngx_atomic_fetch_add(ngx_stat_handled, 1);
    (void) ngx_atomic_fetch_add(ngx_stat_active, 1);
#endif

    log->data = NULL;
    log->handler = NULL;

    ls->handler(c);
}


ngx_int_t
ngx_event_acceptex_import(ngx_listening_t *ls, ngx_socket_t s,
    struct sockaddr *local_sockaddr, socklen_t local_socklen,
    struct sockaddr *sockaddr, socklen_t socklen, u_char *data, size_t size)
{
    ngx_log_t         *log;
    ngx_event_t       *rev;
    ngx_connection_t  *c;

    if (ls == NULL) {
        ngx_log_error(NGX_LOG_ALERT, ngx_cycle->log, 0,
                      "missing listener for a routed AcceptEx connection");

        if (s != (ngx_socket_t) -1) {
            (void) ngx_close_socket(s);
        }

        return NGX_ERROR;
    }

    if (s == (ngx_socket_t) -1 || ls->sockaddr == NULL
        || ls->type != SOCK_STREAM
        || ngx_acceptex_sockaddr_valid(local_sockaddr, local_socklen,
                                       ls->sockaddr->sa_family)
           != NGX_OK
        || ngx_acceptex_sockaddr_valid(sockaddr, socklen,
                                       ls->sockaddr->sa_family)
           != NGX_OK
        || size > ls->post_accept_buffer_size || (size && data == NULL))
    {
        ngx_log_error(NGX_LOG_ALERT, &ls->log, 0,
                      "invalid routed AcceptEx connection for %V",
                      &ls->addr_text);
        (void) ngx_close_socket(s);
        return NGX_ERROR;
    }

    if (ngx_nonblocking(s) == -1) {
        ngx_log_error(NGX_LOG_CRIT, &ls->log, ngx_socket_errno,
                      ngx_nonblocking_n " failed for %V", &ls->addr_text);
        (void) ngx_close_socket(s);
        return NGX_ERROR;
    }

    if (SetHandleInformation((HANDLE) s, HANDLE_FLAG_INHERIT, 0) == 0) {
        ngx_log_error(NGX_LOG_CRIT, &ls->log, ngx_errno,
                      "SetHandleInformation() failed for %V",
                      &ls->addr_text);
        (void) ngx_close_socket(s);
        return NGX_ERROR;
    }

#if (NGX_STAT_STUB)
    (void) ngx_atomic_fetch_add(ngx_stat_accepted, 1);
#endif

    ngx_accept_disabled = ngx_cycle->connection_n / 8
                          - ngx_cycle->free_connection_n;

    c = ngx_get_connection(s, &ls->log);
    if (c == NULL) {
        (void) ngx_close_socket(s);
        return NGX_ERROR;
    }

    c->type = SOCK_STREAM;
    c->pool = ngx_create_pool(ls->pool_size, &ls->log);
    if (c->pool == NULL) {
        ngx_close_iocp_accepted_connection(c);
        return NGX_ERROR;
    }

    log = ngx_palloc(c->pool, sizeof(ngx_log_t));
    if (log == NULL) {
        ngx_close_iocp_accepted_connection(c);
        return NGX_ERROR;
    }

    *log = ls->log;
    c->log = log;
    c->pool->log = log;

    c->local_sockaddr = ngx_palloc(c->pool, (size_t) local_socklen);
    c->sockaddr = ngx_palloc(c->pool, (size_t) socklen);

    if (c->local_sockaddr == NULL || c->sockaddr == NULL) {
        ngx_close_iocp_accepted_connection(c);
        return NGX_ERROR;
    }

    ngx_memcpy(c->local_sockaddr, local_sockaddr, (size_t) local_socklen);
    ngx_memcpy(c->sockaddr, sockaddr, (size_t) socklen);

    c->local_socklen = local_socklen;
    c->socklen = socklen;
    c->listening = ls;

    if (ls->post_accept_buffer_size) {
        c->buffer = ngx_create_temp_buf(c->pool,
                                        ls->post_accept_buffer_size);
        if (c->buffer == NULL) {
            ngx_close_iocp_accepted_connection(c);
            return NGX_ERROR;
        }

        if (size) {
            rev = c->read;
            rev->iocp_buffer = ngx_alloc(size, log);
            if (rev->iocp_buffer == NULL) {
                ngx_close_iocp_accepted_connection(c);
                return NGX_ERROR;
            }

            ngx_memcpy(rev->iocp_buffer, data, size);
            rev->iocp_buffer_size = size;
            rev->iocp_buffer_pos = 0;
            rev->available = (int) ngx_min(size, NGX_MAX_INT32_VALUE);
        }
    }

    c->recv = ngx_recv;
    c->send = ngx_send;
    c->recv_chain = ngx_recv_chain;
    c->send_chain = ngx_send_chain;
    c->sendfile = 1;

    c->read->ready = 1;
    c->write->ready = 1;
    c->read->log = log;
    c->write->log = log;

    if (ls->addr_ntop) {
        c->addr_text.data = ngx_pnalloc(c->pool, ls->addr_text_max_len);
        if (c->addr_text.data == NULL) {
            ngx_close_iocp_accepted_connection(c);
            return NGX_ERROR;
        }

        c->addr_text.len = ngx_sock_ntop(c->sockaddr, c->socklen,
                                         c->addr_text.data,
                                         ls->addr_text_max_len, 0);
        if (c->addr_text.len == 0) {
            ngx_close_iocp_accepted_connection(c);
            return NGX_ERROR;
        }
    }

    if (ngx_iocp_add_connection(c) != NGX_OK) {
        ngx_close_iocp_accepted_connection(c);
        return NGX_ERROR;
    }

    /*
     * This socket was recreated from WSADuplicateSocket() protocol state.
     * Some providers can still queue a completion packet after reporting a
     * synchronous result when completion-on-success is disabled.  Keep the
     * conservative completion path so an OVERLAPPED is not recycled while a
     * late packet can still reference it.
     */

    c->number = ngx_atomic_fetch_add(ngx_connection_counter, 1);
    c->start_time = ngx_current_msec;

#if (NGX_DEBUG)
    {
        ngx_event_conf_t  *ecf;

        ecf = ngx_event_get_conf(ngx_cycle->conf_ctx, ngx_event_core_module);
        ngx_debug_accepted_connection(ecf, c);
    }
#endif

#if (NGX_STAT_STUB)
    (void) ngx_atomic_fetch_add(ngx_stat_handled, 1);
    (void) ngx_atomic_fetch_add(ngx_stat_active, 1);
#endif

    log->data = NULL;
    log->handler = NULL;

    ls->handler(c);

    return NGX_OK;
}


static ngx_int_t
ngx_acceptex_sockaddr_valid(struct sockaddr *sockaddr, socklen_t socklen,
    int family)
{
    if (sockaddr == NULL || socklen <= 0
        || socklen > (socklen_t) sizeof(ngx_sockaddr_t)
        || sockaddr->sa_family != family)
    {
        return NGX_ERROR;
    }

    switch (family) {

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
ngx_acceptex_address_valid(ngx_iocp_accept_op_t *op,
    struct sockaddr *sockaddr, int socklen, int family)
{
    uintptr_t  start, address;
    size_t     offset;
    size_t     required;

    if (sockaddr == NULL || socklen <= 0
        || (size_t) socklen > sizeof(ngx_sockaddr_t)
        || socklen < (int) sizeof(sockaddr->sa_family)
        || sockaddr->sa_family != family)
    {
        return NGX_ERROR;
    }

    switch (sockaddr->sa_family) {

    case AF_INET:
        required = sizeof(struct sockaddr_in);
        break;

#if (NGX_HAVE_INET6)
    case AF_INET6:
        required = sizeof(struct sockaddr_in6);
        break;
#endif

    default:
        return NGX_ERROR;
    }

    if ((size_t) socklen < required) {
        return NGX_ERROR;
    }

    start = (uintptr_t) op->buffer;
    address = (uintptr_t) sockaddr;

    if (address < start) {
        return NGX_ERROR;
    }

    offset = (size_t) (address - start);
    if (offset > op->buffer_size
        || (size_t) socklen > op->buffer_size - offset)
    {
        return NGX_ERROR;
    }

    return NGX_OK;
}


static void
ngx_event_acceptex_cleanup(ngx_iocp_op_t *base)
{
    ngx_iocp_accept_op_t  *op;

    op = (ngx_iocp_accept_op_t *) base;

    if (op->socket == (ngx_socket_t) -1) {
        return;
    }

    if (ngx_close_socket(op->socket) == -1) {
        ngx_log_error(NGX_LOG_ALERT, &op->listening->log, ngx_socket_errno,
                      ngx_close_socket_n " failed");
    }

    op->socket = (ngx_socket_t) -1;
}


static void
ngx_close_iocp_accepted_connection(ngx_connection_t *c)
{
    ngx_pool_t  *pool;

    pool = c->pool;
    ngx_close_connection(c);

    if (pool) {
        ngx_destroy_pool(pool);
    }
}


u_char *
ngx_acceptex_log_error(ngx_log_t *log, u_char *buf, size_t len)
{
    return ngx_snprintf(buf, len, " while posting AcceptEx() on %V", log->data);
}
