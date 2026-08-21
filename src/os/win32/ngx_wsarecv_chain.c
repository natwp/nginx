
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>


#define NGX_WSABUFS  64


typedef struct {
    ngx_iocp_op_t  op;
    ngx_chain_t   *chain;
    u_char        *buffer;
    unsigned       direct:1;
} ngx_iocp_wsarecv_chain_op_t;


static void ngx_iocp_wsarecv_chain_complete(ngx_iocp_op_t *base);
static void ngx_iocp_wsarecv_chain_cleanup(ngx_iocp_op_t *base);
static ngx_int_t ngx_iocp_wsarecv_chain_defer(ngx_iocp_op_t *base);
static ssize_t ngx_iocp_wsarecv_chain_copy(ngx_event_t *rev,
    ngx_chain_t *chain, off_t limit);


ssize_t
ngx_wsarecv_chain(ngx_connection_t *c, ngx_chain_t *chain, off_t limit)
{
    int           rc;
    u_char       *prev;
    u_long        bytes, flags;
    size_t        max, n, size;
    ngx_err_t     err;
    ngx_array_t   vec;
    ngx_event_t  *rev;
    LPWSABUF      wsabuf;
    WSABUF        wsabufs[NGX_WSABUFS];

    if (limit < 0) {
        c->read->error = 1;
        ngx_log_error(NGX_LOG_ALERT, c->log, 0,
                      "negative receive chain limit");
        return NGX_ERROR;
    }

    max = ngx_min((size_t) NGX_MAX_UINT32_VALUE,
                  (size_t) NGX_MAX_SIZE_T_VALUE);
    if (limit == 0 || (uint64_t) limit > (uint64_t) max) {
        limit = (off_t) max;
    }

    prev = NULL;
    wsabuf = NULL;
    flags = 0;
    size = 0;
    bytes = 0;

    vec.elts = wsabufs;
    vec.nelts = 0;
    vec.size = sizeof(WSABUF);
    vec.nalloc = NGX_WSABUFS;
    vec.pool = c->pool;

    /* coalesce the neighbouring bufs */

    while (chain) {
        if (chain->buf == NULL || chain->buf->last == NULL
            || chain->buf->end == NULL || chain->buf->end < chain->buf->last)
        {
            c->read->error = 1;
            ngx_log_error(NGX_LOG_ALERT, c->log, 0,
                          "invalid buffer range in WSARecv chain");
            return NGX_ERROR;
        }

        n = (size_t) (chain->buf->end - chain->buf->last);

        if (limit) {
            if (size >= (size_t) limit) {
                break;
            }

            if (n > (size_t) limit - size) {
                n = (size_t) limit - size;
            }
        }

        if (n == 0) {
            chain = chain->next;
            continue;
        }

        if (prev == chain->buf->last) {
            if (n > (size_t) NGX_MAX_UINT32_VALUE - wsabuf->len) {
                c->read->error = 1;
                ngx_log_error(NGX_LOG_ALERT, c->log, 0,
                              "oversized WSARecv chain");
                return NGX_ERROR;
            }

            wsabuf->len += n;

        } else {
            if (vec.nelts == vec.nalloc) {
                break;
            }

            wsabuf = ngx_array_push(&vec);
            if (wsabuf == NULL) {
                return NGX_ERROR;
            }

            wsabuf->buf = (char *) chain->buf->last;
            wsabuf->len = n;
        }

        size += n;
        prev = chain->buf->last + n;
        chain = chain->next;
    }

    if (vec.nelts == 0) {
        return 0;
    }

    ngx_log_debug2(NGX_LOG_DEBUG_EVENT, c->log, 0,
                   "WSARecv: %ui:%ul", vec.nelts, wsabuf->len);


    rc = WSARecv(c->fd, vec.elts, vec.nelts, &bytes, &flags, NULL, NULL);

    rev = c->read;

    if (rc == -1) {
        rev->ready = 0;
        err = ngx_socket_errno;

        if (err == WSAEWOULDBLOCK) {
            ngx_log_debug0(NGX_LOG_DEBUG_EVENT, c->log, err,
                           "WSARecv() not ready");
            return NGX_AGAIN;
        }

        rev->error = 1;
        ngx_connection_error(c, err, "WSARecv() failed");
        return NGX_ERROR;
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
            rev->ready = 0;
            rev->error = 1;
            ngx_connection_error(c, ngx_socket_errno,
                                 ngx_socket_nread_n " failed");
            return NGX_ERROR;
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
ngx_overlapped_wsarecv_chain(ngx_connection_t *c, ngx_chain_t *chain,
    off_t limit)
{
    int                            rc;
    u_long                         bytes, expected;
    size_t                         max, n, size;
    ngx_err_t                      err;
    ngx_pool_t                    *pool;
    ngx_chain_t                    *start;
    ngx_chain_t                    *cl;
    u_char                         *prev;
    WSABUF                         *wsabuf;
    size_t                         remaining;
    u_long                         posted;
    ngx_uint_t                     nbufs, unlimited;
    ngx_event_t                   *rev;
    ngx_iocp_wsarecv_chain_op_t   *op;
    WSABUF                         wsabufs[NGX_WSABUFS];

    if (limit < 0) {
        rev = c->read;
        rev->error = 1;
        ngx_log_error(NGX_LOG_ALERT, c->log, 0,
                      "negative receive chain limit");
        return NGX_ERROR;
    }

    unlimited = (limit == 0);

    max = ngx_min((size_t) NGX_MAX_UINT32_VALUE,
                  (size_t) NGX_MAX_SIZE_T_VALUE);
    max = ngx_min(max, (size_t) NGX_IOCP_READ_LIMIT);
    if (limit == 0 || (uint64_t) limit > (uint64_t) max) {
        limit = (off_t) max;
    }

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
            rev->error = 1;
            ngx_connection_error(c, err, "WSARecv() failed");
            return NGX_ERROR;
        }

        if (bytes > expected) {
            rev->error = 1;
            ngx_connection_error(c, WSAEMSGSIZE,
                                 "WSARecv() chain returned too much data");
            return NGX_ERROR;
        }

        if (bytes == 0) {
            rev->ready = 0;
            rev->eof = 1;

            rev->iocp_chain = NULL;
            rev->iocp_direct_chain = 0;
            rev->iocp_buffer_size = 0;
            rev->iocp_buffer_pos = 0;

        } else if (rev->iocp_direct_chain) {
            if (!unlimited || rev->iocp_chain != chain
                || rev->iocp_buffer_size != bytes)
            {
                rev->iocp_chain = NULL;
                rev->iocp_direct_chain = 0;
                rev->iocp_buffer_size = 0;
                rev->iocp_buffer_pos = 0;
                rev->error = 1;
                ngx_connection_error(c, WSAEINVAL,
                                     "WSARecv() chain changed while "
                                     "IOCP read was pending");
                return NGX_ERROR;
            }

            rev->iocp_chain = NULL;
            rev->iocp_direct_chain = 0;
            rev->iocp_buffer_size = 0;
            rev->iocp_buffer_pos = 0;
            rev->ready = 0;
            return (ssize_t) bytes;

        } else {
            rev->ready = 1;

            if (rev->iocp_buffer == NULL
                || rev->iocp_buffer_size != bytes)
            {
                rev->error = 1;
                ngx_connection_error(c, WSAEINVAL,
                                     "WSARecv() chain lost completed data");
                return NGX_ERROR;
            }
        }

        return ngx_iocp_wsarecv_chain_copy(rev, chain, limit);
    }

    if (rev->iocp_direct_chain) {
        if (!unlimited || rev->iocp_chain != chain) {
            rev->iocp_chain = NULL;
            rev->iocp_direct_chain = 0;
            rev->iocp_buffer_size = 0;
            rev->iocp_buffer_pos = 0;
            rev->error = 1;
            ngx_connection_error(c, WSAEINVAL,
                                 "WSARecv() chain changed while "
                                 "IOCP read was pending");
            return NGX_ERROR;
        }

        n = rev->iocp_buffer_size;
        rev->iocp_chain = NULL;
        rev->iocp_direct_chain = 0;
        rev->iocp_buffer_size = 0;
        rev->iocp_buffer_pos = 0;
        rev->ready = 0;
        return (ssize_t) n;
    }

    if (rev->iocp_buffer) {
        return ngx_iocp_wsarecv_chain_copy(rev, chain, limit);
    }

    if (rev->iocp_op) {
        return NGX_AGAIN;
    }

    /*
     * Consume readiness reported by a zero-byte IOCP receive directly.  A
     * second overlapped receive would add an operation allocation and another
     * completion-port round trip before the caller can use the available
     * bytes.
     */
    if (rev->ready) {
        return ngx_wsarecv_chain(c, chain, limit);
    }

    if (c->iocp == NULL && ngx_iocp_add_connection(c) != NGX_OK) {
        rev->error = 1;
        return NGX_ERROR;
    }

    start = chain;
    size = 0;

    while (chain && size < max) {
        if (chain->buf == NULL || chain->buf->last == NULL
            || chain->buf->end == NULL || chain->buf->end < chain->buf->last)
        {
            rev->error = 1;
            ngx_log_error(NGX_LOG_ALERT, c->log, 0,
                          "invalid buffer range in overlapped WSARecv chain");
            return NGX_ERROR;
        }

        n = (size_t) (chain->buf->end - chain->buf->last);

        if (limit) {
            if (size >= (size_t) limit) {
                break;
            }

            if (n > (size_t) limit - size) {
                n = (size_t) limit - size;
            }
        }

        if (n == 0) {
            chain = chain->next;
            continue;
        }

        if (n > max - size) {
            n = max - size;
        }

        size += n;
        chain = chain->next;
    }

    if (size == 0) {
        return 0;
    }

    pool = rev->iocp_pool ? rev->iocp_pool : c->pool;

    op = (ngx_iocp_wsarecv_chain_op_t *)
         ngx_iocp_op_create(sizeof(ngx_iocp_wsarecv_chain_op_t), c->iocp,
                            rev, pool, NGX_IOCP_OP_RECV_CHAIN,
                            ngx_iocp_wsarecv_chain_complete,
                            ngx_iocp_wsarecv_chain_cleanup);
    if (op == NULL) {
        rev->error = 1;
        return NGX_ERROR;
    }

    if (unlimited && rev->iocp_pool && rev->iocp_pool != c->pool) {
        op->chain = start;
        op->direct = 1;
        remaining = size;
        posted = 0;
        nbufs = 0;
        prev = NULL;
        wsabuf = NULL;

        for (cl = start; cl && remaining; cl = cl->next) {
            n = (size_t) (cl->buf->end - cl->buf->last);

            if (n > remaining) {
                n = remaining;
            }

            if (n == 0) {
                continue;
            }

            if (prev == cl->buf->last) {
                if (n > (size_t) NGX_MAX_UINT32_VALUE - wsabuf->len) {
                    rev->error = 1;
                    ngx_log_error(NGX_LOG_ALERT, c->log, 0,
                                  "oversized WSARecv chain");
                    ngx_iocp_op_abort(&op->op);
                    return NGX_ERROR;
                }

                wsabuf->len += (ULONG) n;

            } else {
                if (nbufs == NGX_WSABUFS) {
                    break;
                }

                wsabuf = &wsabufs[nbufs++];
                wsabuf->buf = (char *) cl->buf->last;
                wsabuf->len = (ULONG) n;
            }

            posted += (u_long) n;
            remaining -= n;
            prev = cl->buf->last + n;
        }

        if (nbufs == 0) {
            ngx_iocp_op_abort(&op->op);
            return 0;
        }

    } else {
        op->buffer = ngx_alloc(size, c->log);
        if (op->buffer == NULL) {
            ngx_iocp_op_abort(&op->op);
            rev->error = 1;
            return NGX_ERROR;
        }

        wsabufs[0].buf = (char *) op->buffer;
        wsabufs[0].len = (ULONG) size;
        posted = (u_long) size;
        nbufs = 1;
    }

    op->op.flags = 0;
    op->op.bytes = 0;
    op->op.expected = (DWORD) posted;

    rc = WSARecv(c->fd, wsabufs, (DWORD) nbufs, &op->op.bytes,
                 &op->op.flags, &op->op.overlapped, NULL);

    rev->complete = 0;
    rev->active = 1;
    rev->ready = 0;

    ngx_log_debug3(NGX_LOG_DEBUG_EVENT, c->log, 0,
                   "WSARecv chain ovlp: fd:%d rc:%d size:%ul",
                   c->fd, rc, posted);

    if (rc == -1) {
        err = ngx_socket_errno;

        if (err == WSA_IO_PENDING) {
            return NGX_AGAIN;
        }

        rev->active = 0;
        rev->ready = 1;
        rev->error = 1;
        ngx_iocp_op_abort(&op->op);
        ngx_connection_error(c, err, "WSARecv() failed");

        return NGX_ERROR;
    }

    return NGX_AGAIN;
}


static void
ngx_iocp_wsarecv_chain_complete(ngx_iocp_op_t *base)
{
    ngx_event_t                   *rev;
    ngx_iocp_wsarecv_chain_op_t   *op;

    op = (ngx_iocp_wsarecv_chain_op_t *) base;
    rev = base->event;

    if (base->error == 0 && base->bytes) {
        if (rev->iocp_buffer || rev->iocp_direct_chain) {
            base->error = WSAEINVAL;

        } else if (op->direct) {
            rev->iocp_chain = op->chain;
            rev->iocp_buffer_size = base->bytes;
            rev->iocp_buffer_pos = 0;
            rev->iocp_direct_chain = 1;

        } else {
            rev->iocp_buffer = op->buffer;
            rev->iocp_buffer_size = base->bytes;
            rev->iocp_buffer_pos = 0;
            op->buffer = NULL;
        }
    }

    ngx_iocp_event_complete(base);

    if (rev->iocp_direct_chain && rev->iocp_chain
        && ngx_iocp_wsarecv_chain_defer(base) != NGX_OK)
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
ngx_iocp_wsarecv_chain_cleanup(ngx_iocp_op_t *base)
{
    ngx_iocp_wsarecv_chain_op_t  *op;

    op = (ngx_iocp_wsarecv_chain_op_t *) base;

    if (op->buffer) {
        ngx_free(op->buffer);
        op->buffer = NULL;
    }
}


static ngx_int_t
ngx_iocp_wsarecv_chain_defer(ngx_iocp_op_t *base)
{
    size_t        copied, n, size;
    u_char       *buffer;
    ngx_buf_t    *b;
    ngx_chain_t  *cl;
    ngx_event_t  *rev;

    rev = base->event;

    if (!rev->iocp_direct_chain || rev->iocp_chain == NULL) {
        return NGX_OK;
    }

    size = rev->iocp_buffer_size;
    buffer = ngx_alloc(size, base->owner->log);
    if (buffer == NULL) {
        rev->iocp_chain = NULL;
        rev->iocp_buffer_size = 0;
        rev->iocp_buffer_pos = 0;
        rev->iocp_direct_chain = 0;
        rev->iocp_error = WSAENOBUFS;
        return NGX_ERROR;
    }

    copied = 0;

    for (cl = rev->iocp_chain; cl && copied < size; cl = cl->next) {
        b = cl->buf;

        if (b == NULL || b->last == NULL || b->end == NULL
            || b->end < b->last)
        {
            ngx_free(buffer);
            rev->iocp_chain = NULL;
            rev->iocp_buffer_size = 0;
            rev->iocp_buffer_pos = 0;
            rev->iocp_direct_chain = 0;
            rev->iocp_error = WSAEINVAL;
            return NGX_ERROR;
        }

        n = ngx_min((size_t) (b->end - b->last), size - copied);
        ngx_memcpy(buffer + copied, b->last, n);
        copied += n;
    }

    if (copied != size) {
        ngx_free(buffer);
        rev->iocp_chain = NULL;
        rev->iocp_buffer_size = 0;
        rev->iocp_buffer_pos = 0;
        rev->iocp_direct_chain = 0;
        rev->iocp_error = WSAEINVAL;
        return NGX_ERROR;
    }

    rev->iocp_chain = NULL;
    rev->iocp_buffer = buffer;
    rev->iocp_buffer_pos = 0;
    rev->iocp_direct_chain = 0;

    return NGX_OK;
}


static ssize_t
ngx_iocp_wsarecv_chain_copy(ngx_event_t *rev, ngx_chain_t *chain,
    off_t limit)
{
    size_t       available, copied, n, size;
    ngx_buf_t   *b;

    if (rev->iocp_buffer_pos > rev->iocp_buffer_size) {
        rev->error = 1;
        return NGX_ERROR;
    }

    available = rev->iocp_buffer_size - rev->iocp_buffer_pos;

    if (available == 0) {
        return rev->eof ? 0 : NGX_AGAIN;
    }
    copied = 0;

    while (chain && copied < available) {
        b = chain->buf;

        if (b == NULL || b->last == NULL || b->end == NULL
            || b->end < b->last)
        {
            rev->error = 1;
            return NGX_ERROR;
        }

        size = (size_t) (b->end - b->last);

        if (limit) {
            if ((off_t) copied >= limit) {
                break;
            }

            if (size > (size_t) (limit - (off_t) copied)) {
                size = (size_t) (limit - copied);
            }
        }

        n = ngx_min(size, available - copied);

        if (n) {
            ngx_memcpy(b->last,
                       rev->iocp_buffer + rev->iocp_buffer_pos + copied, n);
            copied += n;
        }

        chain = chain->next;
    }

    rev->iocp_buffer_pos += copied;

    if (rev->iocp_buffer_pos == rev->iocp_buffer_size) {
        ngx_free(rev->iocp_buffer);
        rev->iocp_buffer = NULL;
        rev->iocp_buffer_size = 0;
        rev->iocp_buffer_pos = 0;
        rev->ready = 0;
    }

    return copied ? (ssize_t) copied : (rev->eof ? 0 : NGX_AGAIN);
}
