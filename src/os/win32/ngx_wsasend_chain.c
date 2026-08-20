
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>


#define NGX_WSABUFS            64
#define NGX_TRANSMIT_ELEMENTS  64
#define NGX_IOCP_FILE_BUFSIZE  65536


typedef struct {
    ngx_iocp_op_t  op;
    WSABUF         wsabufs[NGX_WSABUFS];
    u_char        *buffers[NGX_WSABUFS];
    ngx_uint_t     nbufs;
    u_long         size;
} ngx_iocp_wsasend_chain_op_t;


typedef struct {
    ngx_iocp_op_t             op;
    TRANSMIT_PACKETS_ELEMENT  elements[NGX_TRANSMIT_ELEMENTS];
    u_char                   *buffers[NGX_TRANSMIT_ELEMENTS];
    ngx_uint_t                nelements;
    u_long                    size;
} ngx_iocp_transmit_op_t;


static ngx_int_t ngx_iocp_chain_has_file(ngx_connection_t *c,
    ngx_chain_t *in, off_t limit);
static ngx_chain_t *ngx_iocp_skip_empty(ngx_chain_t *in);
static void ngx_iocp_wsasend_chain_cleanup(ngx_iocp_op_t *base);
static void ngx_iocp_transmit_cleanup(ngx_iocp_op_t *base);
static ngx_int_t ngx_iocp_transmit_chain(ngx_connection_t *c,
    ngx_chain_t *in, off_t limit, ngx_pool_t *pool, u_long *sent);
static ngx_int_t ngx_iocp_transmit_packets(ngx_connection_t *c,
    ngx_chain_t *in, off_t limit, ngx_pool_t *pool, u_long *sent);
static ngx_int_t ngx_iocp_transmit_file(ngx_connection_t *c,
    ngx_chain_t *in, off_t limit, ngx_pool_t *pool, u_long *sent);
static ngx_int_t ngx_iocp_transmit_sync_complete(ngx_connection_t *c,
    ngx_iocp_op_t *op, u_long *sent);
static ngx_int_t ngx_iocp_send_file_buffer(ngx_connection_t *c,
    ngx_chain_t *in, off_t limit, ngx_pool_t *pool, u_long *sent);


ngx_chain_t *
ngx_wsasend_chain(ngx_connection_t *c, ngx_chain_t *in, off_t limit)
{
    int           rc;
    u_char       *prev;
    u_long        size, sent, send, prev_send;
    ngx_err_t     err;
    ngx_event_t  *wev;
    ngx_array_t   vec;
    ngx_chain_t  *cl;
    LPWSABUF      wsabuf;
    WSABUF        wsabufs[NGX_WSABUFS];

    wev = c->write;

    if (limit < 0) {
        wev->error = 1;
        ngx_log_error(NGX_LOG_ALERT, c->log, 0,
                      "negative send chain limit");
        return NGX_CHAIN_ERROR;
    }

    in = ngx_iocp_skip_empty(in);
    if (in == NULL) {
        return NULL;
    }

    if (!wev->ready) {
        return in;
    }

    /* the maximum limit size is the maximum u_long value - the page size */

    if (limit == 0 || limit > (off_t) (NGX_MAX_UINT32_VALUE - ngx_pagesize)) {
        limit = NGX_MAX_UINT32_VALUE - ngx_pagesize;
    }

    send = 0;

    /*
     * WSABUFs must be 4-byte aligned otherwise
     * WSASend() will return undocumented WSAEINVAL error.
     */

    vec.elts = wsabufs;
    vec.size = sizeof(WSABUF);
    vec.nalloc = ngx_min(NGX_WSABUFS, ngx_max_wsabufs);
    vec.pool = c->pool;

    if (vec.nalloc == 0) {
        vec.nalloc = 1;
    }

    for ( ;; ) {
        prev = NULL;
        wsabuf = NULL;
        prev_send = send;

        vec.nelts = 0;

        /* create the WSABUF and coalesce the neighbouring bufs */

        for (cl = in; cl && send < limit; cl = cl->next) {

            if (cl->buf == NULL) {
                wev->error = 1;
                ngx_log_error(NGX_LOG_ALERT, c->log, 0,
                              "NULL buffer in WSASend chain");
                return NGX_CHAIN_ERROR;
            }

            if (ngx_buf_special(cl->buf)) {
                continue;
            }

            if (!ngx_buf_in_memory(cl->buf) || cl->buf->pos == NULL
                || cl->buf->last == NULL || cl->buf->last < cl->buf->pos)
            {
                wev->error = 1;
                ngx_log_error(NGX_LOG_ALERT, c->log, 0,
                              "invalid memory buffer in WSASend chain");
                return NGX_CHAIN_ERROR;
            }

            if ((size_t) (cl->buf->last - cl->buf->pos)
                > NGX_MAX_UINT32_VALUE)
            {
                size = NGX_MAX_UINT32_VALUE;

            } else {
                size = (u_long) (cl->buf->last - cl->buf->pos);
            }

            if ((off_t) size > limit - send) {
                size = (u_long) (limit - send);
            }

            if (size == 0) {
                continue;
            }

            if (prev == cl->buf->pos && wsabuf) {
                if (size > NGX_MAX_UINT32_VALUE - wsabuf->len) {
                    wev->error = 1;
                    ngx_log_error(NGX_LOG_ALERT, c->log, 0,
                                  "oversized WSASend chain");
                    return NGX_CHAIN_ERROR;
                }

                wsabuf->len += size;

            } else {
                if (vec.nelts == vec.nalloc) {
                    break;
                }

                wsabuf = ngx_array_push(&vec);
                if (wsabuf == NULL) {
                    return NGX_CHAIN_ERROR;
                }

                wsabuf->buf = (char *) cl->buf->pos;
                wsabuf->len = size;
            }

            prev = cl->buf->pos + size;
            send += size;
        }

        if (vec.nelts == 0) {
            return ngx_iocp_skip_empty(in);
        }

        sent = 0;

        rc = WSASend(c->fd, vec.elts, vec.nelts, &sent, 0, NULL, NULL);

        if (rc == -1) {
            err = ngx_errno;

            if (err == WSAEWOULDBLOCK) {
                ngx_log_debug0(NGX_LOG_DEBUG_EVENT, c->log, err,
                               "WSASend() not ready");

            } else {
                wev->error = 1;
                ngx_connection_error(c, err, "WSASend() failed");
                return NGX_CHAIN_ERROR;
            }
        }

        ngx_log_debug2(NGX_LOG_DEBUG_EVENT, c->log, 0,
                       "WSASend: fd:%d, s:%ul", c->fd, sent);

        c->sent += sent;

        in = ngx_chain_update_sent(in, sent);

        if (send - prev_send != sent) {
            wev->ready = 0;
            return in;
        }

        if (send >= limit || in == NULL) {
            return in;
        }
    }
}


ngx_chain_t *
ngx_overlapped_wsasend_chain(ngx_connection_t *c, ngx_chain_t *in, off_t limit)
{
    int                           rc;
    u_long                        expected, size, sent;
    ngx_err_t                     err;
    ngx_int_t                     trc;
    ngx_pool_t                   *pool;
    ngx_event_t                  *wev;
    ngx_chain_t                  *cl;
    ngx_uint_t                    direct;
    ngx_uint_t                    i;
    ngx_uint_t                    max_bufs;
    ngx_iocp_wsasend_chain_op_t  *op;

    wev = c->write;

    if (limit < 0) {
        wev->error = 1;
        ngx_log_error(NGX_LOG_ALERT, c->log, 0,
                      "negative send chain limit");
        return NGX_CHAIN_ERROR;
    }

    in = ngx_iocp_skip_empty(in);
    if (in == NULL) {
        return NULL;
    }

    if (!wev->complete && wev->iocp_error) {
        err = wev->iocp_error;
        wev->iocp_error = 0;
        wev->error = 1;
        ngx_connection_error(c, err, "IOCP write failed");
        return NGX_CHAIN_ERROR;
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
            ngx_connection_error(c, err, "IOCP send chain failed");
            return NGX_CHAIN_ERROR;
        }

        if (sent > expected || (sent == 0 && expected != 0)) {
            wev->error = 1;
            ngx_connection_error(c, WSAECONNRESET,
                                 "IOCP send chain completed without progress");
            return NGX_CHAIN_ERROR;
        }

        ngx_log_debug2(NGX_LOG_DEBUG_EVENT, c->log, 0,
                       "WSASend chain ovlp: fd:%d sent:%ul", c->fd, sent);

        c->sent += sent;
        wev->ready = 1;

        return ngx_chain_update_sent(in, sent);
    }

    if (wev->iocp_op) {
        return in;
    }

    if (!wev->ready) {
        return in;
    }

    if (limit == 0 || limit > NGX_IOCP_SEND_LIMIT) {
        limit = NGX_IOCP_SEND_LIMIT;
    }

    if (c->iocp == NULL && ngx_iocp_add_connection(c) != NGX_OK) {
        wev->error = 1;
        return NGX_CHAIN_ERROR;
    }

    pool = wev->iocp_pool ? wev->iocp_pool : c->pool;

    trc = ngx_iocp_chain_has_file(c, in, limit);

    if (trc == NGX_ERROR) {
        wev->error = 1;
        return NGX_CHAIN_ERROR;
    }

    if (trc == NGX_OK) {
        sent = 0;
        trc = ngx_iocp_transmit_chain(c, in, limit, pool, &sent);

        if (trc == NGX_OK) {
            return in;
        }

        if (trc == NGX_DONE) {
            c->sent += sent;
            return ngx_chain_update_sent(in, sent);
        }

        if (trc == NGX_ERROR) {
            return NGX_CHAIN_ERROR;
        }
    }

    op = (ngx_iocp_wsasend_chain_op_t *)
         ngx_iocp_op_prepare(sizeof(ngx_iocp_wsasend_chain_op_t), c->iocp,
                             wev, pool, NGX_IOCP_OP_SEND_CHAIN,
                             ngx_iocp_event_complete,
                             ngx_iocp_wsasend_chain_cleanup);
    if (op == NULL) {
        op = (ngx_iocp_wsasend_chain_op_t *)
             ngx_iocp_op_create(sizeof(ngx_iocp_wsasend_chain_op_t), c->iocp,
                                wev, pool, NGX_IOCP_OP_SEND_CHAIN,
                                ngx_iocp_event_complete,
                                ngx_iocp_wsasend_chain_cleanup);
    }
    if (op == NULL) {
        wev->error = 1;
        return NGX_CHAIN_ERROR;
    }

    max_bufs = ngx_min(NGX_WSABUFS, ngx_max_wsabufs);
    if (max_bufs == 0) {
        max_bufs = 1;
    }

    for (cl = in; cl && op->size < (u_long) limit; cl = cl->next) {

        if (cl->buf == NULL) {
            wev->error = 1;
            ngx_log_error(NGX_LOG_ALERT, c->log, 0,
                          "NULL buffer in an IOCP send chain");
            ngx_iocp_op_abort(&op->op);
            return NGX_CHAIN_ERROR;
        }

        if (ngx_buf_special(cl->buf)) {
            continue;
        }

        if (cl->buf->in_file) {
            break;
        }

        if (!ngx_buf_in_memory(cl->buf)) {
            wev->error = 1;
            ngx_log_error(NGX_LOG_ALERT, c->log, 0,
                          "invalid buffer in an IOCP send chain");
            ngx_iocp_op_abort(&op->op);
            return NGX_CHAIN_ERROR;
        }

        if (cl->buf->pos == NULL || cl->buf->last == NULL
            || cl->buf->last < cl->buf->pos)
        {
            wev->error = 1;
            ngx_log_error(NGX_LOG_ALERT, c->log, 0,
                          "invalid memory range in an IOCP send chain");
            ngx_iocp_op_abort(&op->op);
            return NGX_CHAIN_ERROR;
        }

        if ((size_t) (cl->buf->last - cl->buf->pos)
            > NGX_MAX_UINT32_VALUE)
        {
            size = NGX_MAX_UINT32_VALUE;

        } else {
            size = (u_long) (cl->buf->last - cl->buf->pos);
        }

        if (size > (u_long) limit - op->size) {
            size = (u_long) limit - op->size;
        }

        if (size == 0) {
            continue;
        }

        if (op->nbufs == max_bufs) {
            break;
        }

        op->wsabufs[op->nbufs].buf = (char *) cl->buf->pos;
        op->wsabufs[op->nbufs].len = size;
        op->nbufs++;
        op->size += size;
    }

    if (op->nbufs == 0) {
        ngx_iocp_op_abort(&op->op);
        return ngx_iocp_skip_empty(in);
    }

    direct = NGX_IOCP_DIRECT_SEND && wev->iocp_pool
             && op->size >= NGX_IOCP_DIRECT_SEND_MIN_SIZE;

    if (!direct) {
        for (i = 0; i < op->nbufs; i++) {
            op->buffers[i] = ngx_alloc(op->wsabufs[i].len, c->log);
            if (op->buffers[i] == NULL) {
                wev->error = 1;
                ngx_iocp_op_abort(&op->op);
                return NGX_CHAIN_ERROR;
            }

            ngx_memcpy(op->buffers[i], op->wsabufs[i].buf,
                       op->wsabufs[i].len);
            op->wsabufs[i].buf = (char *) op->buffers[i];
        }
    }

    op->op.bytes = 0;
    op->op.expected = op->size;

    rc = WSASend(c->fd, op->wsabufs, (DWORD) op->nbufs, &op->op.bytes, 0,
                 &op->op.overlapped, NULL);

    wev->complete = 0;
    wev->active = 1;
    wev->ready = 0;

    ngx_log_debug4(NGX_LOG_DEBUG_EVENT, c->log, 0,
                   "WSASend chain ovlp: fd:%d rc:%d bufs:%ui size:%ul",
                   c->fd, rc, op->nbufs, op->size);

    if (rc == 0 && c->iocp->skip_completion) {
        sent = op->op.bytes;
        wev->active = 0;

        if (op->op.prepared) {
            (void) ngx_iocp_op_arm(&op->op, 0);
        }

        if (sent > op->op.expected || (sent == 0 && op->op.expected != 0)) {
            wev->ready = 0;
            wev->error = 1;
            ngx_iocp_op_abort(&op->op);
            ngx_connection_error(c, WSAECONNRESET,
                                 "IOCP send chain completed without progress");
            return NGX_CHAIN_ERROR;
        }

        /* A synchronous short write has no completion packet to wake us. */
        wev->ready = 1;
        ngx_iocp_op_abort(&op->op);
        c->sent += sent;

        return ngx_chain_update_sent(in, sent);
    }

    if (rc == -1) {
        err = ngx_socket_errno;

        if (err == WSA_IO_PENDING) {
            if (op->op.prepared) {
                (void) ngx_iocp_op_arm(&op->op, 1);
            }
            return in;
        }

        wev->active = 0;
        wev->ready = 1;
        wev->error = 1;
        if (op->op.prepared) {
            (void) ngx_iocp_op_arm(&op->op, 0);
        }
        ngx_iocp_op_abort(&op->op);
        ngx_connection_error(c, err, "WSASend() failed");

        return NGX_CHAIN_ERROR;
    }

    return in;
}


static void
ngx_iocp_wsasend_chain_cleanup(ngx_iocp_op_t *base)
{
    ngx_uint_t                      i;
    ngx_iocp_wsasend_chain_op_t   *op;

    op = (ngx_iocp_wsasend_chain_op_t *) base;

    for (i = 0; i < op->nbufs; i++) {
        if (op->buffers[i]) {
            ngx_free(op->buffers[i]);
            op->buffers[i] = NULL;
        }
    }
}


static ngx_chain_t *
ngx_iocp_skip_empty(ngx_chain_t *in)
{
    ngx_buf_t  *b;

    for ( /* void */ ; in; in = in->next) {
        b = in->buf;

        if (b == NULL) {
            break;
        }

        if (ngx_buf_special(b)) {
            continue;
        }

        if (ngx_buf_in_memory(b) && b->pos == b->last
            && (!b->in_file || b->file_pos == b->file_last))
        {
            continue;
        }

        if (!ngx_buf_in_memory(b) && b->in_file
            && b->file_pos == b->file_last)
        {
            continue;
        }

        break;
    }

    return in;
}


static ngx_int_t
ngx_iocp_chain_has_file(ngx_connection_t *c, ngx_chain_t *in, off_t limit)
{
    off_t        send, size;
    ngx_buf_t   *b;
    ngx_chain_t *cl;

    send = 0;

    for (cl = in; cl && send < limit; cl = cl->next) {
        b = cl->buf;

        if (b == NULL) {
            ngx_log_error(NGX_LOG_ALERT, c->log, 0,
                          "NULL buffer in an IOCP send chain");
            return NGX_ERROR;
        }

        if (ngx_buf_special(b)) {
            continue;
        }

        if (b->in_file) {
            if (b->file == NULL || b->file->fd == NGX_INVALID_FILE
                || b->file_pos < 0 || b->file_last < b->file_pos)
            {
                ngx_log_error(NGX_LOG_ALERT, c->log, 0,
                              "invalid file buffer in an IOCP send chain");
                return NGX_ERROR;
            }

            if (ngx_buf_in_memory(b)
                && (b->pos == NULL || b->last == NULL || b->last < b->pos
                    || (off_t) (b->last - b->pos)
                       != b->file_last - b->file_pos))
            {
                ngx_log_error(NGX_LOG_ALERT, c->log, 0,
                              "mismatched file buffer in an IOCP send chain");
                return NGX_ERROR;
            }

            if (b->file_last != b->file_pos) {
                return NGX_OK;
            }

            continue;
        }

        if (!ngx_buf_in_memory(b) || b->pos == NULL || b->last == NULL
            || b->last < b->pos)
        {
            ngx_log_error(NGX_LOG_ALERT, c->log, 0,
                          "invalid memory buffer in an IOCP send chain");
            return NGX_ERROR;
        }

        size = (off_t) (b->last - b->pos);
        if (size > limit - send) {
            size = limit - send;
        }

        send += size;
    }

    return NGX_DECLINED;
}


static ngx_int_t
ngx_iocp_transmit_chain(ngx_connection_t *c, ngx_chain_t *in, off_t limit,
    ngx_pool_t *pool, u_long *sent)
{
    ngx_int_t  rc;

    *sent = 0;

    if (ngx_win32_workstation) {
        return ngx_iocp_send_file_buffer(c, in, limit, pool, sent);
    }

    if (c->iocp && c->iocp->transmitpackets) {
        rc = ngx_iocp_transmit_packets(c, in, limit, pool, sent);

        if (rc != NGX_DECLINED) {
            return rc;
        }
    }

    rc = ngx_iocp_transmit_file(c, in, limit, pool, sent);

    if (rc != NGX_DECLINED) {
        return rc;
    }

    if (c->iocp && c->iocp->transmitpackets == NULL
        && c->iocp->transmitfile == NULL)
    {
        c->sendfile = 0;
    }

    return ngx_iocp_send_file_buffer(c, in, limit, pool, sent);
}


static ngx_int_t
ngx_iocp_transmit_packets(ngx_connection_t *c, ngx_chain_t *in, off_t limit,
    ngx_pool_t *pool, u_long *sent)
{
    BOOL                         rc;
    u_long                       size, total;
    ngx_err_t                    err;
    ngx_event_t                 *wev;
    ngx_chain_t                 *cl;
    ngx_buf_t                   *b;
    ngx_uint_t                   direct;
    ngx_uint_t                   i;
    ngx_iocp_transmit_op_t      *op;
    TRANSMIT_PACKETS_ELEMENT   *element;

    wev = c->write;
    op = (ngx_iocp_transmit_op_t *)
         ngx_iocp_op_create(sizeof(ngx_iocp_transmit_op_t), c->iocp, wev,
                            pool, NGX_IOCP_OP_TRANSMIT,
                            ngx_iocp_event_complete,
                            ngx_iocp_transmit_cleanup);
    if (op == NULL) {
        wev->error = 1;
        return NGX_ERROR;
    }

    total = 0;
    element = NULL;

    for (cl = in; cl && total < (u_long) limit; cl = cl->next) {
        if (cl->buf == NULL) {
            ngx_iocp_op_abort(&op->op);
            wev->error = 1;
            ngx_log_error(NGX_LOG_ALERT, c->log, 0,
                          "NULL buffer in TransmitPackets()");
            return NGX_ERROR;
        }

        if (ngx_buf_special(cl->buf)) {
            continue;
        }

        b = cl->buf;

        if (b->in_file) {
            if (b->file == NULL || b->file->fd == NGX_INVALID_FILE) {
                ngx_iocp_op_abort(&op->op);
                wev->error = 1;
                ngx_log_error(NGX_LOG_ALERT, c->log, 0,
                              "invalid file buffer in TransmitPackets()");
                return NGX_ERROR;
            }

            if (b->file_pos < 0 || b->file_last < b->file_pos) {
                ngx_iocp_op_abort(&op->op);
                wev->error = 1;
                ngx_log_error(NGX_LOG_ALERT, c->log, 0,
                              "invalid file range in TransmitPackets()");
                return NGX_ERROR;
            }

            if (ngx_buf_in_memory(b)
                && (b->pos == NULL || b->last == NULL || b->last < b->pos
                    || (off_t) (b->last - b->pos)
                       != b->file_last - b->file_pos))
            {
                ngx_iocp_op_abort(&op->op);
                wev->error = 1;
                ngx_log_error(NGX_LOG_ALERT, c->log, 0,
                              "mismatched file buffer in TransmitPackets()");
                return NGX_ERROR;
            }

            size = (u_long) ngx_min((off_t) (b->file_last - b->file_pos),
                                    (off_t) (NGX_MAX_UINT32_VALUE - total));
            if (size > (u_long) limit - total) {
                size = (u_long) limit - total;
            }

            if (size == 0) {
                continue;
            }

            if (element && (element->dwElFlags & TP_ELEMENT_FILE)
                && element->hFile == b->file->fd
                && element->nFileOffset.QuadPart
                   <= NGX_MAX_OFF_T_VALUE - element->cLength
                && element->nFileOffset.QuadPart + element->cLength
                   == b->file_pos)
            {
                element->cLength += size;

            } else {
                if (op->nelements == NGX_TRANSMIT_ELEMENTS) {
                    break;
                }

                element = &op->elements[op->nelements++];
                ngx_memzero(element, sizeof(*element));
                element->dwElFlags = TP_ELEMENT_FILE;
                element->cLength = size;
                element->nFileOffset.QuadPart = b->file_pos;
                element->hFile = b->file->fd;
            }

        } else {
            if (!ngx_buf_in_memory(b)) {
                ngx_iocp_op_abort(&op->op);
                wev->error = 1;
                ngx_log_error(NGX_LOG_ALERT, c->log, 0,
                              "invalid memory buffer in TransmitPackets()");
                return NGX_ERROR;
            }

            if (b->pos == NULL || b->last == NULL || b->last < b->pos) {
                ngx_iocp_op_abort(&op->op);
                wev->error = 1;
                ngx_log_error(NGX_LOG_ALERT, c->log, 0,
                              "invalid memory range in TransmitPackets()");
                return NGX_ERROR;
            }

            size = (u_long) ngx_min((off_t) (b->last - b->pos),
                                    (off_t) (NGX_MAX_UINT32_VALUE - total));
            if (size > (u_long) limit - total) {
                size = (u_long) limit - total;
            }

            if (size == 0) {
                continue;
            }

            if (op->nelements == NGX_TRANSMIT_ELEMENTS) {
                break;
            }

            element = &op->elements[op->nelements];
            ngx_memzero(element, sizeof(*element));
            element->dwElFlags = TP_ELEMENT_MEMORY;
            element->cLength = size;
            element->pBuffer = b->pos;

            op->nelements++;
        }

        total += size;
    }

    if (op->nelements == 0 || total == 0) {
        ngx_iocp_op_abort(&op->op);
        return NGX_DECLINED;
    }

    direct = NGX_IOCP_DIRECT_SEND && wev->iocp_pool
             && total >= NGX_IOCP_DIRECT_SEND_MIN_SIZE;

    if (!direct) {
        for (i = 0; i < op->nelements; i++) {
            element = &op->elements[i];

            if (!(element->dwElFlags & TP_ELEMENT_MEMORY)) {
                continue;
            }

            op->buffers[i] = ngx_alloc(element->cLength, c->log);
            if (op->buffers[i] == NULL) {
                ngx_iocp_op_abort(&op->op);
                wev->error = 1;
                return NGX_ERROR;
            }

            ngx_memcpy(op->buffers[i], element->pBuffer,
                       element->cLength);
            element->pBuffer = op->buffers[i];
        }
    }

    op->elements[op->nelements - 1].dwElFlags |= TP_ELEMENT_EOP;
    op->size = total;
    op->op.expected = total;

    rc = c->iocp->transmitpackets(c->fd, op->elements,
                                  (DWORD) op->nelements, 0,
                                  &op->op.overlapped, TP_USE_DEFAULT_WORKER);
    wev->complete = 0;
    wev->active = 1;
    wev->ready = 0;

    if (rc != 0) {
        if (c->iocp->skip_completion) {
            return ngx_iocp_transmit_sync_complete(c, &op->op, sent);
        }

        return NGX_OK;
    }

    err = ngx_socket_errno;
    if (err == WSA_IO_PENDING) {
        return NGX_OK;
    }

    ngx_iocp_op_abort(&op->op);
    wev->active = 0;
    wev->ready = 1;

    if (err == WSAEINVAL || err == WSAEOPNOTSUPP) {
        c->iocp->transmitpackets = NULL;
        c->iocp->port_owner->transmitpackets = NULL;
        return NGX_DECLINED;
    }

    wev->error = 1;
    ngx_connection_error(c, err, "TransmitPackets() failed");
    return NGX_ERROR;
}


static void
ngx_iocp_transmit_cleanup(ngx_iocp_op_t *base)
{
    ngx_uint_t               i;
    ngx_iocp_transmit_op_t  *op;

    op = (ngx_iocp_transmit_op_t *) base;

    for (i = 0; i < op->nelements; i++) {
        if (op->buffers[i]) {
            ngx_free(op->buffers[i]);
            op->buffers[i] = NULL;
        }
    }
}


static ngx_int_t
ngx_iocp_transmit_file(ngx_connection_t *c, ngx_chain_t *in, off_t limit,
    ngx_pool_t *pool, u_long *sent)
{
    BOOL                         rc;
    u_long                       size;
    ngx_err_t                    err;
    ngx_event_t                 *wev;
    ngx_chain_t                 *cl, *first;
    ngx_buf_t                   *b;
    ngx_fd_t                     fd;
    off_t                        offset, total;
    ngx_iocp_transmit_op_t      *op;

    wev = c->write;
    first = NULL;
    for (cl = in; cl; cl = cl->next) {
        if (cl->buf == NULL) {
            wev = c->write;
            wev->error = 1;
            ngx_log_error(NGX_LOG_ALERT, c->log, 0,
                          "NULL buffer in TransmitFile()");
            return NGX_ERROR;
        }

        if (ngx_buf_special(cl->buf)) {
            continue;
        }

        first = cl;
        break;
    }

    if (first == NULL || !first->buf->in_file) {
        return NGX_DECLINED;
    }

    if (c->iocp == NULL || c->iocp->transmitfile == NULL) {
        return NGX_DECLINED;
    }

    b = first->buf;
    if (b->file == NULL || b->file->fd == NGX_INVALID_FILE) {
        c->write->error = 1;
        ngx_log_error(NGX_LOG_ALERT, c->log, 0,
                      "invalid file buffer in TransmitFile()");
        return NGX_ERROR;
    }

    fd = b->file->fd;
    offset = b->file_pos;

    if (offset < 0) {
        c->write->error = 1;
        ngx_log_error(NGX_LOG_ALERT, c->log, 0,
                      "negative file offset in TransmitFile()");
        return NGX_ERROR;
    }

    total = 0;

    for (cl = first; cl && total < limit; cl = cl->next) {
        if (cl->buf == NULL) {
            wev->error = 1;
            ngx_log_error(NGX_LOG_ALERT, c->log, 0,
                          "NULL buffer in TransmitFile()");
            return NGX_ERROR;
        }

        if (ngx_buf_special(cl->buf)) {
            continue;
        }

        b = cl->buf;
        if (!b->in_file || b->file == NULL || b->file->fd != fd)
        {
            break;
        }

        if (b->file_pos < 0 || b->file_last < b->file_pos) {
            wev->error = 1;
            ngx_log_error(NGX_LOG_ALERT, c->log, 0,
                          "invalid file range in TransmitFile()");
            return NGX_ERROR;
        }

        if (total > NGX_MAX_OFF_T_VALUE - offset
            || b->file_pos != offset + total)
        {
            break;
        }

        if (ngx_buf_in_memory(b)
            && (b->pos == NULL || b->last == NULL || b->last < b->pos
                || (off_t) (b->last - b->pos)
                   != b->file_last - b->file_pos))
        {
            wev->error = 1;
            ngx_log_error(NGX_LOG_ALERT, c->log, 0,
                          "mismatched file buffer in TransmitFile()");
            return NGX_ERROR;
        }

        if (b->file_last - b->file_pos > limit - total) {
            total = limit;
            break;
        }

        total += b->file_last - b->file_pos;
    }

    if (total <= 0) {
        return NGX_DECLINED;
    }

    size = (u_long) ngx_min(total, (off_t) NGX_MAX_UINT32_VALUE);
    op = (ngx_iocp_transmit_op_t *)
         ngx_iocp_op_create(sizeof(ngx_iocp_transmit_op_t), c->iocp, wev,
                            pool, NGX_IOCP_OP_TRANSMIT,
                            ngx_iocp_event_complete,
                            ngx_iocp_transmit_cleanup);
    if (op == NULL) {
        wev->error = 1;
        return NGX_ERROR;
    }

    op->size = size;
    op->op.expected = size;
    op->op.overlapped.Offset = (DWORD) offset;
    op->op.overlapped.OffsetHigh = (DWORD) (offset >> 32);

    rc = c->iocp->transmitfile(c->fd, fd, size, 0, &op->op.overlapped,
                               NULL, TF_USE_DEFAULT_WORKER);
    wev->complete = 0;
    wev->active = 1;
    wev->ready = 0;

    if (rc != 0) {
        if (c->iocp->skip_completion) {
            return ngx_iocp_transmit_sync_complete(c, &op->op, sent);
        }

        return NGX_OK;
    }

    err = ngx_socket_errno;
    if (err == WSA_IO_PENDING) {
        return NGX_OK;
    }

    ngx_iocp_op_abort(&op->op);
    wev->active = 0;
    wev->ready = 1;

    if (err == WSAEINVAL || err == WSAEOPNOTSUPP) {
        c->iocp->transmitfile = NULL;
        c->iocp->port_owner->transmitfile = NULL;
        c->sendfile = 0;
        return NGX_DECLINED;
    }

    wev->error = 1;
    ngx_connection_error(c, err, "TransmitFile() failed");
    return NGX_ERROR;
}


static ngx_int_t
ngx_iocp_transmit_sync_complete(ngx_connection_t *c, ngx_iocp_op_t *op,
    u_long *sent)
{
    BOOL         rc;
    DWORD        bytes, flags;
    ngx_err_t    err;
    ngx_event_t *wev;

    bytes = 0;
    flags = 0;
    wev = c->write;

    rc = WSAGetOverlappedResult(c->fd, &op->overlapped, &bytes, FALSE,
                                &flags);

    wev->active = 0;

    if (rc == 0) {
        err = ngx_socket_errno;
        wev->ready = 0;
        wev->error = 1;
        ngx_iocp_op_abort(op);
        ngx_connection_error(c, err, "native file transmit failed");
        return NGX_ERROR;
    }

    if (bytes > op->expected || (bytes == 0 && op->expected != 0)) {
        wev->ready = 0;
        wev->error = 1;
        ngx_iocp_op_abort(op);
        ngx_connection_error(c, WSAECONNRESET,
                             "native file transmit completed without progress");
        return NGX_ERROR;
    }

    /* A synchronous short write has no completion packet to wake us. */
    wev->ready = 1;
    *sent = bytes;
    ngx_iocp_op_abort(op);

    return NGX_DONE;
}


static ngx_int_t
ngx_iocp_send_file_buffer(ngx_connection_t *c, ngx_chain_t *in, off_t limit,
    ngx_pool_t *pool, u_long *sent)
{
    int                           rc;
    u_long                        size;
    ssize_t                       n;
    off_t                         offset, saved_offset;
    ngx_buf_t                    *b;
    ngx_err_t                     err;
    ngx_event_t                  *wev;
    ngx_chain_t                  *cl;
    ngx_iocp_wsasend_chain_op_t  *op;

    for (cl = in; cl; cl = cl->next) {
        if (cl->buf == NULL) {
            c->write->error = 1;
            ngx_log_error(NGX_LOG_ALERT, c->log, 0,
                          "NULL buffer in IOCP send fallback");
            return NGX_ERROR;
        }

        if (!ngx_buf_special(cl->buf)) {
            break;
        }
    }

    if (cl == NULL || !cl->buf->in_file) {
        return NGX_DECLINED;
    }

    b = cl->buf;
    wev = c->write;

    if (b->file == NULL || b->file->fd == NGX_INVALID_FILE
        || b->file_pos < 0 || b->file_last <= b->file_pos)
    {
        wev->error = 1;
        ngx_log_error(NGX_LOG_ALERT, c->log, 0,
                      "invalid file buffer in IOCP send fallback");
        return NGX_ERROR;
    }

    if (ngx_buf_in_memory(b)
        && (b->pos == NULL || b->last == NULL || b->last < b->pos
            || (off_t) (b->last - b->pos)
               != b->file_last - b->file_pos))
    {
        wev->error = 1;
        ngx_log_error(NGX_LOG_ALERT, c->log, 0,
                      "mismatched file buffer in IOCP send fallback");
        return NGX_ERROR;
    }

    size = (u_long) ngx_min(b->file_last - b->file_pos,
                            (off_t) NGX_IOCP_FILE_BUFSIZE);
    size = (u_long) ngx_min((off_t) size, limit);

    op = (ngx_iocp_wsasend_chain_op_t *)
         ngx_iocp_op_prepare(sizeof(ngx_iocp_wsasend_chain_op_t), c->iocp,
                             wev, pool, NGX_IOCP_OP_SEND_CHAIN,
                             ngx_iocp_event_complete,
                             ngx_iocp_wsasend_chain_cleanup);
    if (op == NULL) {
        op = (ngx_iocp_wsasend_chain_op_t *)
             ngx_iocp_op_create(sizeof(ngx_iocp_wsasend_chain_op_t), c->iocp,
                                wev, pool, NGX_IOCP_OP_SEND_CHAIN,
                                ngx_iocp_event_complete,
                                ngx_iocp_wsasend_chain_cleanup);
    }
    if (op == NULL) {
        wev->error = 1;
        return NGX_ERROR;
    }

    op->nbufs = 1;
    op->buffers[0] = ngx_alloc(size, c->log);
    if (op->buffers[0] == NULL) {
        wev->error = 1;
        ngx_iocp_op_abort(&op->op);
        return NGX_ERROR;
    }

    offset = b->file_pos;
    saved_offset = b->file->offset;
    n = ngx_read_file(b->file, op->buffers[0], size, offset);
    b->file->offset = saved_offset;

    if (n <= 0 || (size_t) n != size) {
        if (n == 0) {
            ngx_log_error(NGX_LOG_ALERT, c->log, 0,
                          "file \"%V\" was truncated at %O",
                          &b->file->name, offset);

        } else if (n > 0) {
            ngx_log_error(NGX_LOG_ALERT, c->log, 0,
                          "file \"%V\" returned only %z of %ul bytes",
                          &b->file->name, n, size);
        }

        wev->error = 1;
        ngx_iocp_op_abort(&op->op);
        return NGX_ERROR;
    }

    op->wsabufs[0].buf = (char *) op->buffers[0];
    op->wsabufs[0].len = (u_long) n;
    op->size = (u_long) n;
    op->op.bytes = 0;
    op->op.expected = op->size;

    rc = WSASend(c->fd, op->wsabufs, 1, &op->op.bytes, 0,
                 &op->op.overlapped, NULL);

    wev->complete = 0;
    wev->active = 1;
    wev->ready = 0;

    ngx_log_debug3(NGX_LOG_DEBUG_EVENT, c->log, 0,
                   "WSASend file fallback: fd:%d size:%ul offset:%O",
                   c->fd, op->size, offset);

    if (rc == 0 && c->iocp->skip_completion) {
        *sent = op->op.bytes;
        wev->active = 0;

        if (op->op.prepared) {
            (void) ngx_iocp_op_arm(&op->op, 0);
        }

        if (*sent > op->op.expected
            || (*sent == 0 && op->op.expected != 0))
        {
            wev->ready = 0;
            wev->error = 1;
            ngx_iocp_op_abort(&op->op);
            ngx_connection_error(c, WSAECONNRESET,
                                 "WSASend() completed without progress");
            return NGX_ERROR;
        }

        /* A synchronous short write has no completion packet to wake us. */
        wev->ready = 1;
        ngx_iocp_op_abort(&op->op);

        return NGX_DONE;
    }

    if (rc == -1) {
        err = ngx_socket_errno;

        if (err == WSA_IO_PENDING) {
            if (op->op.prepared) {
                (void) ngx_iocp_op_arm(&op->op, 1);
            }
            return NGX_OK;
        }

        wev->active = 0;
        wev->ready = 1;
        wev->error = 1;
        if (op->op.prepared) {
            (void) ngx_iocp_op_arm(&op->op, 0);
        }
        ngx_iocp_op_abort(&op->op);
        ngx_connection_error(c, err, "WSASend() failed");
        return NGX_ERROR;
    }

    return NGX_OK;
}
