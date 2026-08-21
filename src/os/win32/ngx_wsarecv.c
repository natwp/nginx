
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
    unsigned       allocated:1;
} ngx_iocp_wsarecv_op_t;


static void ngx_iocp_wsarecv_complete(ngx_iocp_op_t *base);
static void ngx_iocp_wsarecv_cleanup(ngx_iocp_op_t *base);
static ngx_int_t ngx_iocp_wsarecv_defer(ngx_iocp_op_t *base);
static ssize_t ngx_iocp_wsarecv_copy(ngx_event_t *rev, u_char *buf,
    size_t size);


ssize_t
ngx_wsarecv(ngx_connection_t *c, u_char *buf, size_t size)
{
    int           rc;
    u_long        bytes, flags;
    WSABUF        wsabuf[1];
    ngx_err_t     err;
    ngx_int_t     n;
    ngx_event_t  *rev;

    if (size && buf == NULL) {
        c->read->error = 1;
        ngx_log_error(NGX_LOG_ALERT, c->log, 0,
                      "NULL buffer passed to WSARecv()");
        return NGX_ERROR;
    }

    size = ngx_min(size, (size_t) NGX_MAX_UINT32_VALUE);
    size = ngx_min(size, (size_t) NGX_MAX_SIZE_T_VALUE);

    if (size == 0) {
        return 0;
    }

    wsabuf[0].buf = (char *) buf;
    wsabuf[0].len = (ULONG) size;
    flags = 0;
    bytes = 0;

    rc = WSARecv(c->fd, wsabuf, 1, &bytes, &flags, NULL, NULL);

    ngx_log_debug4(NGX_LOG_DEBUG_EVENT, c->log, 0,
                   "WSARecv: fd:%d rc:%d %ul of %z", c->fd, rc, bytes, size);

    rev = c->read;

    if (rc == -1) {
        rev->ready = 0;
        err = ngx_socket_errno;

        if (err == WSAEWOULDBLOCK) {
            ngx_log_debug0(NGX_LOG_DEBUG_EVENT, c->log, err,
                           "WSARecv() not ready");
            return NGX_AGAIN;
        }

        n = ngx_connection_error(c, err, "WSARecv() failed");

        if (n == NGX_ERROR) {
            rev->error = 1;
        }

        return n;
    }

#if (NGX_HAVE_FIONREAD)

    if (rev->available >= 0 && bytes > 0) {
        rev->available -= bytes;

        /*
         * negative rev->available means some additional bytes
         * were received between kernel notification and WSARecv(),
         * and therefore ev->ready can be safely reset even for
         * edge-triggered event methods
         */

        if (rev->available < 0) {
            rev->available = 0;
            rev->ready = 0;
        }

        ngx_log_debug1(NGX_LOG_DEBUG_EVENT, c->log, 0,
                       "WSARecv: avail:%d", rev->available);

    } else if (bytes == size) {

        if (ngx_socket_nread(c->fd, &rev->available) == -1) {
            n = ngx_connection_error(c, ngx_socket_errno,
                                     ngx_socket_nread_n " failed");

            if (n == NGX_ERROR) {
                rev->ready = 0;
                rev->error = 1;
            }

            return n;
        }

        ngx_log_debug1(NGX_LOG_DEBUG_EVENT, c->log, 0,
                       "WSARecv: avail:%d", rev->available);
    }

#endif

    if (bytes < size) {
        rev->ready = 0;
    }

    if (bytes == 0) {
        rev->ready = 0;
        rev->eof = 1;
    }

    return bytes;
}


ssize_t
ngx_overlapped_wsarecv(ngx_connection_t *c, u_char *buf, size_t size)
{
    int                       rc;
    u_long                    bytes, expected;
    ngx_err_t                 err;
    ngx_int_t                 n;
    ngx_pool_t               *pool;
    ngx_event_t              *rev;
    ngx_iocp_wsarecv_op_t    *op;

    rev = c->read;

    if (!rev->complete && rev->iocp_error) {
        err = rev->iocp_error;
        rev->iocp_error = 0;
        rev->error = 1;
        (void) ngx_connection_error(c, err, "overlapped read failed");
        return NGX_ERROR;
    }

    if (rev->complete) {
        rev->complete = 0;
        rev->active = 0;

        err = rev->iocp_error;
        rev->iocp_error = 0;

        bytes = (u_long) rev->iocp_bytes;
        rev->iocp_bytes = 0;
        rev->available = 0;
        expected = (u_long) rev->iocp_expected;
        rev->iocp_expected = 0;

        if (err) {
            n = ngx_connection_error(c, err, "WSARecv() failed");
            if (n == NGX_ERROR) {
                rev->error = 1;
            }

            return n;
        }

        if (bytes > expected) {
            rev->error = 1;
            ngx_connection_error(c, WSAEMSGSIZE,
                                 "WSARecv() returned too much data");
            return NGX_ERROR;
        }

        ngx_log_debug3(NGX_LOG_DEBUG_EVENT, c->log, 0,
                       "WSARecv ovlp: fd:%d %ul of %uz",
                       c->fd, bytes, size);

        if (bytes == 0) {
            rev->ready = 0;
            rev->eof = 1;
        } else {
            rev->ready = 1;

            if (rev->iocp_buffer == NULL
                || rev->iocp_buffer_size != bytes)
            {
                rev->error = 1;
                ngx_connection_error(c, WSAEINVAL,
                                     "WSARecv() lost completed data");
                return NGX_ERROR;
            }
        }

        return ngx_iocp_wsarecv_copy(rev, buf, size);
    }

    if (rev->iocp_buffer) {
        return ngx_iocp_wsarecv_copy(rev, buf, size);
    }

    if (rev->iocp_op) {
        return NGX_AGAIN;
    }

    if (size == 0) {
        return 0;
    }

    if (buf == NULL) {
        rev->error = 1;
        ngx_log_error(NGX_LOG_ALERT, c->log, 0,
                      "NULL buffer passed to overlapped WSARecv()");
        return NGX_ERROR;
    }

    size = ngx_min(size, (size_t) NGX_MAX_UINT32_VALUE);
    size = ngx_min(size, (size_t) NGX_MAX_SIZE_T_VALUE);
    size = ngx_min(size, (size_t) NGX_IOCP_READ_LIMIT);

    if (c->iocp == NULL && ngx_iocp_add_connection(c) != NGX_OK) {
        rev->error = 1;
        return NGX_ERROR;
    }

    pool = rev->iocp_pool ? rev->iocp_pool : c->pool;

    op = (ngx_iocp_wsarecv_op_t *)
         ngx_iocp_op_create(sizeof(ngx_iocp_wsarecv_op_t), c->iocp, rev,
                            pool, NGX_IOCP_OP_RECV,
                            ngx_iocp_wsarecv_complete,
                            ngx_iocp_wsarecv_cleanup);
    if (op == NULL) {
        rev->error = 1;
        return NGX_ERROR;
    }

    op->wsabuf.len = (ULONG) size;

    if (rev->iocp_pool && rev->iocp_pool != c->pool) {
        op->buffer = buf;
        op->allocated = 0;
        rev->iocp_direct_recv = 1;

    } else {
        rev->iocp_direct_recv = 0;
        op->buffer = ngx_alloc(op->wsabuf.len, c->log);
        if (op->buffer == NULL) {
            ngx_iocp_op_abort(&op->op);
            rev->error = 1;
            return NGX_ERROR;
        }

        op->allocated = 1;
    }

    op->wsabuf.buf = (char *) op->buffer;
    op->op.flags = 0;
    op->op.bytes = 0;
    op->op.expected = op->wsabuf.len;

    rc = WSARecv(c->fd, &op->wsabuf, 1, &op->op.bytes, &op->op.flags,
                 &op->op.overlapped, NULL);

    rev->complete = 0;
    rev->active = 1;
    rev->ready = 0;

    ngx_log_debug3(NGX_LOG_DEBUG_EVENT, c->log, 0,
                   "WSARecv ovlp: fd:%d rc:%d of %uz", c->fd, rc, size);

    if (rc == -1) {
        err = ngx_socket_errno;
        if (err == WSA_IO_PENDING) {
            ngx_log_debug0(NGX_LOG_DEBUG_EVENT, c->log, err,
                           "WSARecv() posted");
            return NGX_AGAIN;
        }

        rev->active = 0;
        rev->ready = 1;
        rev->iocp_direct_recv = 0;
        ngx_iocp_op_abort(&op->op);

        n = ngx_connection_error(c, err, "WSARecv() failed");

        if (n == NGX_ERROR) {
            rev->error = 1;
        }

        return n;
    }

    return NGX_AGAIN;
}


static void
ngx_iocp_wsarecv_complete(ngx_iocp_op_t *base)
{
    ngx_event_t             *rev;
    ngx_iocp_wsarecv_op_t   *op;

    op = (ngx_iocp_wsarecv_op_t *) base;
    rev = base->event;

    if (base->error == 0 && base->bytes) {
        if (rev->iocp_buffer) {
            base->error = WSAEINVAL;
            rev->iocp_direct_recv = 0;

        } else {
            rev->iocp_buffer = op->buffer;
            rev->iocp_buffer_size = base->bytes;
            rev->iocp_buffer_pos = 0;
            op->buffer = NULL;
        }

    } else {
        rev->iocp_direct_recv = 0;
    }

    ngx_iocp_event_complete(base);

    if (rev->iocp_direct_recv && rev->iocp_buffer
        && ngx_iocp_wsarecv_defer(base) != NGX_OK)
    {
        rev->complete = 0;
        rev->iocp_bytes = 0;
        rev->iocp_expected = 0;
        rev->error = 1;
        rev->ready = 1;

        if (rev->handler) {
            rev->handler(rev);
        }
    }
}


static void
ngx_iocp_wsarecv_cleanup(ngx_iocp_op_t *base)
{
    ngx_iocp_wsarecv_op_t  *op;

    op = (ngx_iocp_wsarecv_op_t *) base;

    if (op->allocated && op->buffer) {
        ngx_free(op->buffer);
        op->buffer = NULL;
    }
}


static ngx_int_t
ngx_iocp_wsarecv_defer(ngx_iocp_op_t *base)
{
    size_t        available;
    u_char       *buffer;
    ngx_event_t  *rev;

    rev = base->event;

    if (!rev->iocp_direct_recv || rev->iocp_buffer == NULL) {
        return NGX_OK;
    }

    if (rev->iocp_buffer_pos > rev->iocp_buffer_size) {
        rev->iocp_buffer = NULL;
        rev->iocp_buffer_size = 0;
        rev->iocp_buffer_pos = 0;
        rev->iocp_direct_recv = 0;
        rev->iocp_error = WSAEINVAL;
        return NGX_ERROR;
    }

    available = rev->iocp_buffer_size - rev->iocp_buffer_pos;

    if (available == 0) {
        rev->iocp_buffer = NULL;
        rev->iocp_buffer_size = 0;
        rev->iocp_buffer_pos = 0;
        rev->iocp_direct_recv = 0;
        return NGX_OK;
    }

    buffer = ngx_alloc(available, base->owner->log);
    if (buffer == NULL) {
        rev->iocp_buffer = NULL;
        rev->iocp_buffer_size = 0;
        rev->iocp_buffer_pos = 0;
        rev->iocp_direct_recv = 0;
        rev->iocp_error = WSAENOBUFS;
        return NGX_ERROR;
    }

    ngx_memcpy(buffer, rev->iocp_buffer + rev->iocp_buffer_pos, available);

    rev->iocp_buffer = buffer;
    rev->iocp_buffer_size = available;
    rev->iocp_buffer_pos = 0;
    rev->iocp_direct_recv = 0;

    return NGX_OK;
}


static ssize_t
ngx_iocp_wsarecv_copy(ngx_event_t *rev, u_char *buf, size_t size)
{
    size_t  available, n;

    if (rev->iocp_buffer_pos > rev->iocp_buffer_size) {
        rev->error = 1;
        return NGX_ERROR;
    }

    available = rev->iocp_buffer_size - rev->iocp_buffer_pos;
    n = ngx_min(available, size);

    if (n) {
        if (buf == NULL || rev->iocp_buffer == NULL) {
            rev->error = 1;
            return NGX_ERROR;
        }

        if (buf != rev->iocp_buffer + rev->iocp_buffer_pos) {
            ngx_memcpy(buf, rev->iocp_buffer + rev->iocp_buffer_pos, n);
        }
        rev->iocp_buffer_pos += n;
    }

    if (rev->iocp_buffer_pos == rev->iocp_buffer_size) {
        if (!rev->iocp_direct_recv) {
            ngx_free(rev->iocp_buffer);
        }
        rev->iocp_buffer = NULL;
        rev->iocp_buffer_size = 0;
        rev->iocp_buffer_pos = 0;
        rev->iocp_direct_recv = 0;
        rev->ready = 0;
    }

    return (ssize_t) n;
}
