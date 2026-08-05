
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>


typedef struct {
    ngx_iocp_op_t  op;
    WSABUF         wsabuf;
    u_char        *buffer;
} ngx_iocp_wsasend_op_t;


static void ngx_iocp_wsasend_cleanup(ngx_iocp_op_t *base);


ssize_t
ngx_wsasend(ngx_connection_t *c, u_char *buf, size_t size)
{
    int           n;
    u_long        sent;
    ngx_err_t     err;
    ngx_event_t  *wev;
    WSABUF        wsabuf;

    wev = c->write;

    if (!wev->ready) {
        return NGX_AGAIN;
    }

    if (size && buf == NULL) {
        wev->error = 1;
        ngx_log_error(NGX_LOG_ALERT, c->log, 0,
                      "NULL buffer passed to WSASend()");
        return NGX_ERROR;
    }

    size = ngx_min(size, (size_t) NGX_MAX_UINT32_VALUE);
    size = ngx_min(size, (size_t) NGX_MAX_SIZE_T_VALUE);

    if (size == 0) {
        return 0;
    }

    /*
     * WSABUF must be 4-byte aligned otherwise
     * WSASend() will return undocumented WSAEINVAL error.
     */

    wsabuf.buf = (char *) buf;
    wsabuf.len = (ULONG) size;

    sent = 0;

    n = WSASend(c->fd, &wsabuf, 1, &sent, 0, NULL, NULL);

    ngx_log_debug4(NGX_LOG_DEBUG_EVENT, c->log, 0,
                   "WSASend: fd:%d, %d, %ul of %uz", c->fd, n, sent, size);

    if (n == 0) {
        if (sent < size) {
            wev->ready = 0;
        }

        c->sent += sent;

        return sent;
    }

    err = ngx_socket_errno;

    if (err == WSAEWOULDBLOCK) {
        ngx_log_debug0(NGX_LOG_DEBUG_EVENT, c->log, err, "WSASend() not ready");
        wev->ready = 0;
        return NGX_AGAIN;
    }

    wev->error = 1;
    ngx_connection_error(c, err, "WSASend() failed");

    return NGX_ERROR;
}


ssize_t
ngx_overlapped_wsasend(ngx_connection_t *c, u_char *buf, size_t size)
{
    int                       n;
    u_long                    expected, sent;
    ngx_err_t                 err;
    ngx_pool_t               *pool;
    ngx_event_t              *wev;
    ngx_iocp_wsasend_op_t    *op;

    wev = c->write;

    if (!wev->complete && wev->iocp_error) {
        err = wev->iocp_error;
        wev->iocp_error = 0;
        wev->error = 1;
        ngx_connection_error(c, err, "IOCP write failed");
        return NGX_ERROR;
    }

    if (wev->complete) {
        wev->complete = 0;
        wev->active = 0;

        err = wev->iocp_error;
        wev->iocp_error = 0;

        sent = (u_long) wev->iocp_bytes;
        wev->iocp_bytes = 0;
        wev->available = 0;
        expected = (u_long) wev->iocp_expected;
        wev->iocp_expected = 0;

        if (err) {
            wev->error = 1;
            ngx_connection_error(c, err, "WSASend() failed");
            return NGX_ERROR;
        }

        if (sent > expected || (sent == 0 && expected != 0)) {
            wev->error = 1;
            ngx_connection_error(c, WSAECONNRESET,
                                 "WSASend() completed without progress");
            return NGX_ERROR;
        }

        ngx_log_debug3(NGX_LOG_DEBUG_EVENT, c->log, 0,
                       "WSASend ovlp: fd:%d %ul of %uz", c->fd, sent, size);

        wev->ready = 1;
        c->sent += sent;

        return sent;
    }

    if (wev->iocp_op) {
        return NGX_AGAIN;
    }

    if (!wev->ready) {
        return NGX_AGAIN;
    }

    if (size == 0) {
        return 0;
    }

    if (buf == NULL) {
        wev->error = 1;
        ngx_log_error(NGX_LOG_ALERT, c->log, 0,
                      "NULL buffer passed to overlapped WSASend()");
        return NGX_ERROR;
    }

    size = ngx_min(size, (size_t) NGX_MAX_UINT32_VALUE);
    size = ngx_min(size, (size_t) NGX_MAX_SIZE_T_VALUE);
    size = ngx_min(size, (size_t) NGX_IOCP_SEND_LIMIT);

    if (c->iocp == NULL && ngx_iocp_add_connection(c) != NGX_OK) {
        wev->error = 1;
        return NGX_ERROR;
    }

    pool = wev->iocp_pool ? wev->iocp_pool : c->pool;

    op = (ngx_iocp_wsasend_op_t *)
         ngx_iocp_op_create(sizeof(ngx_iocp_wsasend_op_t), c->iocp, wev,
                            pool, NGX_IOCP_OP_SEND, ngx_iocp_event_complete,
                            ngx_iocp_wsasend_cleanup);
    if (op == NULL) {
        wev->error = 1;
        return NGX_ERROR;
    }

    op->wsabuf.len = (ULONG) size;
    op->buffer = ngx_alloc(op->wsabuf.len, c->log);
    if (op->buffer == NULL) {
        ngx_iocp_op_abort(&op->op);
        wev->error = 1;
        return NGX_ERROR;
    }

    ngx_memcpy(op->buffer, buf, op->wsabuf.len);
    op->wsabuf.buf = (char *) op->buffer;
    op->op.bytes = 0;
    op->op.expected = op->wsabuf.len;

    n = WSASend(c->fd, &op->wsabuf, 1, &op->op.bytes, 0,
                &op->op.overlapped, NULL);

    wev->complete = 0;
    wev->active = 1;
    wev->ready = 0;

    ngx_log_debug3(NGX_LOG_DEBUG_EVENT, c->log, 0,
                   "WSASend ovlp: fd:%d rc:%d of %uz", c->fd, n, size);

    if (n == -1) {
        err = ngx_socket_errno;

        if (err == WSA_IO_PENDING) {
            return NGX_AGAIN;
        }

        wev->active = 0;
        wev->ready = 1;
        wev->error = 1;
        ngx_iocp_op_abort(&op->op);
        ngx_connection_error(c, err, "WSASend() failed");

        return NGX_ERROR;
    }

    return NGX_AGAIN;
}


static void
ngx_iocp_wsasend_cleanup(ngx_iocp_op_t *base)
{
    ngx_iocp_wsasend_op_t  *op;

    op = (ngx_iocp_wsasend_op_t *) base;

    if (op->buffer) {
        ngx_free(op->buffer);
        op->buffer = NULL;
    }
}
