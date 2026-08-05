
/*
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>


typedef struct {
    ngx_iocp_op_t     op;
    ngx_event_aio_t  *aio;
    u_char           *buffer;
    DWORD             size;
} ngx_iocp_file_op_t;


static ngx_int_t ngx_file_aio_open(ngx_file_t *file, ngx_pool_t *pool,
    ngx_uint_t access);
static void ngx_file_aio_cleanup(void *data);
static void ngx_file_aio_owner_cleanup(ngx_iocp_owner_t *owner);
static void ngx_file_aio_event_handler(ngx_event_t *ev);
static void ngx_file_aio_read_complete(ngx_iocp_op_t *base);
static void ngx_file_aio_write_complete(ngx_iocp_op_t *base);
static void ngx_file_aio_complete(ngx_event_aio_t *aio, ngx_err_t err,
    size_t nbytes);
static ssize_t ngx_file_aio_result(ngx_file_t *file, ngx_event_aio_t *aio,
    ngx_uint_t writing);
static ngx_int_t ngx_file_aio_write_next(ngx_event_aio_t *aio);
static ngx_int_t ngx_file_aio_write_normalize(ngx_event_aio_t *aio);
static void ngx_file_aio_write_advance(ngx_event_aio_t *aio, size_t n);


ngx_uint_t  ngx_file_aio;


ngx_int_t
ngx_file_aio_init(ngx_file_t *file, ngx_pool_t *pool)
{
    return ngx_file_aio_open(file, pool, GENERIC_READ);
}


static ngx_int_t
ngx_file_aio_open(ngx_file_t *file, ngx_pool_t *pool, ngx_uint_t access)
{
    HANDLE                fd;
    ngx_event_aio_t      *aio;
    ngx_iocp_owner_t     *owner;
    ngx_pool_cleanup_t   *cln;

    if (file->fd == NGX_INVALID_FILE || pool == NULL) {
        return NGX_ERROR;
    }

    aio = file->aio;

    if (aio == NULL) {
        aio = ngx_pcalloc(pool, sizeof(ngx_event_aio_t));
        if (aio == NULL) {
            return NGX_ERROR;
        }

        aio->file = file;
        aio->fd = NGX_INVALID_FILE;
        aio->owner_pool = pool;
        aio->pool = pool;
        aio->event.data = aio;
        aio->event.handler = ngx_file_aio_event_handler;
        aio->event.ready = 1;
        aio->event.log = file->log;

        cln = ngx_pool_cleanup_add(pool, 0);
        if (cln == NULL) {
            return NGX_ERROR;
        }

        cln->handler = ngx_file_aio_cleanup;
        cln->data = aio;
        file->aio = aio;

    } else if (aio->iocp && aio->fd != NGX_INVALID_FILE
               && (aio->access & access) == access)
    {
        return NGX_OK;

    } else if (!aio->event.ready || aio->event.complete) {
        ngx_log_error(NGX_LOG_ALERT, file->log, 0,
                      "cannot change access for active file AIO \"%V\"",
                      &file->name);
        return NGX_ERROR;

    } else if (aio->iocp) {
        ngx_iocp_close_owner(aio->iocp);
        aio->iocp = NULL;
        aio->fd = NGX_INVALID_FILE;
    }

    access |= aio->access;

    fd = ReOpenFile(file->fd, (DWORD) access,
                    FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                    FILE_FLAG_OVERLAPPED);

    if (fd == NGX_INVALID_FILE) {
        ngx_log_error(NGX_LOG_ERR, file->log, ngx_errno,
                      "ReOpenFile() \"%V\" failed", &file->name);
        return NGX_ERROR;
    }

    owner = ngx_iocp_create_owner(fd, NULL, file->log, 0, 0);
    if (owner == NULL) {
        (void) CloseHandle(fd);
        return NGX_ERROR;
    }

    owner->pool = aio->owner_pool;
    owner->data = aio;
    owner->cleanup = ngx_file_aio_owner_cleanup;

    if (ngx_iocp_associate(owner) != NGX_OK) {
        ngx_iocp_close_owner(owner);
        return NGX_ERROR;
    }

    aio->fd = fd;
    aio->iocp = owner;
    aio->access = access;

    return NGX_OK;
}


static void
ngx_file_aio_owner_cleanup(ngx_iocp_owner_t *owner)
{
    ngx_event_aio_t  *aio;

    aio = owner->data;

    if (aio && aio->iocp == owner) {
        aio->iocp = NULL;
        aio->fd = NGX_INVALID_FILE;
    }
}


static void
ngx_file_aio_cleanup(void *data)
{
    ngx_event_aio_t  *aio;

    aio = data;

    if (aio->event.timer_set) {
        ngx_del_timer(&aio->event);
    }

    if (aio->iocp) {
        ngx_iocp_close_owner(aio->iocp);
        aio->iocp = NULL;
        aio->fd = NGX_INVALID_FILE;
    }
}


ssize_t
ngx_file_aio_read(ngx_file_t *file, u_char *buf, size_t size, off_t offset,
    ngx_pool_t *pool)
{
    BOOL                 rc;
    DWORD                bytes;
    ngx_err_t            err;
    ngx_event_t         *ev;
    ngx_event_aio_t     *aio;
    ngx_iocp_file_op_t  *op;

    if (!ngx_file_aio) {
        return ngx_read_file(file, buf, size, offset);
    }

    if (offset < 0) {
        ngx_log_error(NGX_LOG_ALERT, file->log, 0,
                      "negative file AIO read offset for \"%V\"",
                      &file->name);
        return NGX_ERROR;
    }

    if (size && buf == NULL) {
        ngx_log_error(NGX_LOG_ALERT, file->log, 0,
                      "NULL file AIO read buffer for \"%V\"", &file->name);
        return NGX_ERROR;
    }

    aio = file->aio;

    if (aio && aio->event.complete) {
        return ngx_file_aio_result(file, aio, 0);
    }

    if (ngx_file_aio_open(file, pool, GENERIC_READ) != NGX_OK) {
        return NGX_ERROR;
    }

    aio = file->aio;
    ev = &aio->event;

    if (ev->complete) {
        return ngx_file_aio_result(file, aio, 0);
    }

    if (!ev->ready || ev->iocp_op) {
        ngx_log_error(NGX_LOG_ALERT, file->log, 0,
                      "second file AIO post for \"%V\"", &file->name);
        return NGX_AGAIN;
    }

    if (size == 0) {
        return 0;
    }

    size = ngx_min(size, (size_t) NGX_MAX_UINT32_VALUE);
    size = ngx_min(size, (size_t) NGX_MAX_SIZE_T_VALUE);
    size = ngx_min(size, (size_t) (NGX_MAX_OFF_T_VALUE - offset));

    if (size == 0) {
        return 0;
    }

    op = (ngx_iocp_file_op_t *)
         ngx_iocp_op_create(sizeof(ngx_iocp_file_op_t), aio->iocp, ev,
                            pool, NGX_IOCP_OP_FILE_READ,
                            ngx_file_aio_read_complete, NULL);
    if (op == NULL) {
        return NGX_ERROR;
    }

    op->aio = aio;
    op->buffer = buf;
    op->size = (DWORD) size;
    op->op.overlapped.Offset = (DWORD) offset;
    op->op.overlapped.OffsetHigh = (DWORD) (offset >> 32);
    op->op.expected = op->size;

    ev->active = 1;
    ev->ready = 0;
    ev->complete = 0;
    aio->writing = 0;
    bytes = 0;

    rc = ReadFile(aio->fd, buf, (DWORD) size, &bytes,
                  &op->op.overlapped);

    if (rc == 0) {
        err = ngx_errno;

        if (err == ERROR_IO_PENDING) {
            return NGX_AGAIN;
        }

        ngx_iocp_op_abort(&op->op);
        ev->active = 0;
        ev->ready = 1;

        if (err == ERROR_HANDLE_EOF) {
            return 0;
        }

        ngx_log_error(NGX_LOG_ERR, file->log, err,
                      "ReadFile() \"%V\" failed", &file->name);
        return NGX_ERROR;
    }

    return NGX_AGAIN;
}


static void
ngx_file_aio_read_complete(ngx_iocp_op_t *base)
{
    ngx_err_t            err;
    ngx_iocp_file_op_t  *op;

    op = (ngx_iocp_file_op_t *) base;
    err = base->error;

    if (err == ERROR_HANDLE_EOF) {
        err = 0;
        base->bytes = 0;
    }

    if (err == 0 && base->bytes > op->size) {
        err = ERROR_INVALID_DATA;
    }

    ngx_file_aio_complete(op->aio, err, base->bytes);
}


ssize_t
ngx_file_aio_write(ngx_file_t *file, u_char *buf, size_t size, off_t offset,
    ngx_pool_t *pool)
{
    ngx_int_t         rc;
    ngx_event_t      *ev;
    ngx_event_aio_t  *aio;

    if (!ngx_file_aio) {
        return ngx_write_file(file, buf, size, offset);
    }

    if (offset < 0) {
        ngx_log_error(NGX_LOG_ALERT, file->log, 0,
                      "negative file AIO write offset for \"%V\"",
                      &file->name);
        return NGX_ERROR;
    }

    if (size && buf == NULL) {
        ngx_log_error(NGX_LOG_ALERT, file->log, 0,
                      "NULL file AIO write buffer for \"%V\"", &file->name);
        return NGX_ERROR;
    }

    if (size > (size_t) NGX_MAX_SIZE_T_VALUE
        || size > (size_t) (NGX_MAX_OFF_T_VALUE - offset))
    {
        ngx_log_error(NGX_LOG_ALERT, file->log, 0,
                      "file AIO write is too large for \"%V\"", &file->name);
        return NGX_ERROR;
    }

    aio = file->aio;

    if (aio && aio->event.complete) {
        return ngx_file_aio_result(file, aio, 1);
    }

    if (ngx_file_aio_open(file, pool, GENERIC_WRITE) != NGX_OK) {
        return NGX_ERROR;
    }

    aio = file->aio;
    ev = &aio->event;

    if (ev->complete) {
        return ngx_file_aio_result(file, aio, 1);
    }

    if (!ev->ready || ev->iocp_op) {
        ngx_log_error(NGX_LOG_ALERT, file->log, 0,
                      "second file AIO post for \"%V\"", &file->name);
        return NGX_AGAIN;
    }

    if (size == 0) {
        return 0;
    }

    aio->writing = 1;
    aio->scalar = 1;
    aio->write_chain = NULL;
    aio->write_pos = buf;
    aio->write_offset = offset;
    aio->write_remaining = size;
    aio->write_total = 0;
    aio->pool = pool;
    aio->err = 0;

    ev->active = 1;
    ev->ready = 0;
    ev->complete = 0;

    rc = ngx_file_aio_write_next(aio);

    if (rc == NGX_AGAIN) {
        return NGX_AGAIN;
    }

    ev->active = 0;
    ev->ready = 1;
    aio->writing = 0;

    return (rc == NGX_OK || rc == NGX_DONE) ? 0 : NGX_ERROR;
}


ssize_t
ngx_file_aio_write_chain(ngx_file_t *file, ngx_chain_t *chain, off_t offset,
    ngx_pool_t *pool)
{
    ngx_int_t         rc;
    ngx_event_t      *ev;
    ngx_event_aio_t  *aio;

    if (!ngx_file_aio) {
        return ngx_write_chain_to_file(file, chain, offset, pool);
    }

    if (offset < 0) {
        ngx_log_error(NGX_LOG_ALERT, file->log, 0,
                      "negative file AIO write offset for \"%V\"",
                      &file->name);
        return NGX_ERROR;
    }

    aio = file->aio;

    if (aio && aio->event.complete) {
        return ngx_file_aio_result(file, aio, 1);
    }

    if (ngx_file_aio_open(file, pool, GENERIC_WRITE) != NGX_OK) {
        return NGX_ERROR;
    }

    aio = file->aio;
    ev = &aio->event;

    if (ev->complete) {
        return ngx_file_aio_result(file, aio, 1);
    }

    if (!ev->ready || ev->iocp_op) {
        ngx_log_error(NGX_LOG_ALERT, file->log, 0,
                      "second file AIO post for \"%V\"", &file->name);
        return NGX_AGAIN;
    }

    if (chain == NULL) {
        ngx_log_error(NGX_LOG_ALERT, file->log, 0,
                      "file AIO write retry without a completion for \"%V\"",
                      &file->name);
        return NGX_ERROR;
    }

    aio->writing = 1;
    aio->scalar = 0;
    aio->write_chain = chain;
    aio->write_pos = NULL;
    aio->write_offset = offset;
    aio->write_remaining = 0;
    aio->write_total = 0;
    aio->pool = pool;
    aio->err = 0;

    ev->active = 1;
    ev->ready = 0;
    ev->complete = 0;

    rc = ngx_file_aio_write_next(aio);

    if (rc == NGX_AGAIN) {
        return NGX_AGAIN;
    }

    ev->active = 0;
    ev->ready = 1;
    aio->writing = 0;

    return (rc == NGX_OK || rc == NGX_DONE) ? 0 : NGX_ERROR;
}


static ngx_int_t
ngx_file_aio_write_next(ngx_event_aio_t *aio)
{
    BOOL                 rc;
    DWORD                bytes, size;
    u_char              *buffer, *prev;
    ngx_buf_t           *b;
    ngx_err_t            err;
    ngx_int_t            nrc;
    ngx_chain_t         *cl;
    ngx_iocp_file_op_t  *op;

    if (aio->scalar) {
        if (aio->write_remaining == 0) {
            return NGX_OK;
        }

        buffer = aio->write_pos;
        size = (DWORD) ngx_min(aio->write_remaining,
                               (size_t) NGX_MAX_UINT32_VALUE);

    } else {
        nrc = ngx_file_aio_write_normalize(aio);

        if (nrc != NGX_OK) {
            return nrc;
        }

        cl = aio->write_chain;
        b = cl->buf;
        buffer = aio->write_pos;
        size = (DWORD) ngx_min((size_t) (b->last - buffer),
                               (size_t) NGX_MAX_UINT32_VALUE);
        prev = b->last;

        for (cl = cl->next; cl && size < NGX_MAX_UINT32_VALUE;
             cl = cl->next)
        {
            b = cl->buf;

            if (b == NULL) {
                aio->err = ERROR_INVALID_DATA;
                ngx_log_error(NGX_LOG_ALERT, aio->file->log, 0,
                              "NULL buffer in file AIO write for \"%V\"",
                              &aio->file->name);
                return NGX_ERROR;
            }

            if (ngx_buf_special(b)) {
                continue;
            }

            if (b->in_file || !ngx_buf_in_memory(b)) {
                break;
            }

            if (b->pos == NULL || b->last == NULL || b->last < b->pos) {
                aio->err = ERROR_INVALID_DATA;
                ngx_log_error(NGX_LOG_ALERT, aio->file->log, 0,
                              "invalid buffer range in file AIO write for "
                              "\"%V\"", &aio->file->name);
                return NGX_ERROR;
            }

            if (prev != b->pos) {
                break;
            }

            bytes = (DWORD) ngx_min((size_t) (b->last - b->pos),
                                    (size_t) (NGX_MAX_UINT32_VALUE - size));
            size += bytes;
            prev = b->pos + bytes;

            if (prev != b->last) {
                break;
            }
        }
    }

    if (aio->write_total > (size_t) NGX_MAX_SIZE_T_VALUE
        || aio->write_total
           > (size_t) (NGX_MAX_OFF_T_VALUE - aio->write_offset)
        || size > (size_t) NGX_MAX_SIZE_T_VALUE - aio->write_total
        || size > (size_t) (NGX_MAX_OFF_T_VALUE - aio->write_offset)
                  - aio->write_total)
    {
        aio->err = ERROR_FILE_TOO_LARGE;
        ngx_log_error(NGX_LOG_ALERT, aio->file->log, 0,
                      "file AIO write offset overflow for \"%V\"",
                      &aio->file->name);
        return NGX_ERROR;
    }

    op = (ngx_iocp_file_op_t *)
         ngx_iocp_op_create(sizeof(ngx_iocp_file_op_t), aio->iocp,
                            &aio->event, aio->pool,
                            NGX_IOCP_OP_FILE_WRITE,
                            ngx_file_aio_write_complete, NULL);
    if (op == NULL) {
        aio->err = ERROR_NOT_ENOUGH_MEMORY;
        return NGX_ERROR;
    }

    op->aio = aio;
    op->buffer = buffer;
    op->size = size;
    op->op.overlapped.Offset = (DWORD) (aio->write_offset
                                        + aio->write_total);
    op->op.overlapped.OffsetHigh = (DWORD) ((aio->write_offset
                                             + aio->write_total) >> 32);
    op->op.expected = size;
    aio->write_expected = size;
    bytes = 0;

    rc = WriteFile(aio->fd, buffer, size, &bytes, &op->op.overlapped);

    if (rc == 0) {
        err = ngx_errno;

        if (err == ERROR_IO_PENDING) {
            return NGX_AGAIN;
        }

        ngx_iocp_op_abort(&op->op);
        aio->err = err;

        ngx_log_error(NGX_LOG_ERR, aio->file->log, err,
                      "WriteFile() \"%V\" failed", &aio->file->name);
        return NGX_ERROR;
    }

    return NGX_AGAIN;
}


static ngx_int_t
ngx_file_aio_write_normalize(ngx_event_aio_t *aio)
{
    ngx_buf_t  *b;

    while (aio->write_chain) {
        b = aio->write_chain->buf;

        if (b == NULL) {
            aio->err = ERROR_INVALID_DATA;
            ngx_log_error(NGX_LOG_ALERT, aio->file->log, 0,
                          "NULL buffer in file AIO write for \"%V\"",
                          &aio->file->name);
            return NGX_ERROR;
        }

        if (ngx_buf_special(b)) {
            aio->write_chain = aio->write_chain->next;
            aio->write_pos = NULL;
            continue;
        }

        if (b->in_file || !ngx_buf_in_memory(b)) {
            aio->err = ERROR_INVALID_DATA;
            ngx_log_error(NGX_LOG_ALERT, aio->file->log, 0,
                          "invalid buffer in file AIO write for \"%V\"",
                          &aio->file->name);
            return NGX_ERROR;
        }

        if (b->pos == NULL || b->last == NULL || b->last < b->pos) {
            aio->err = ERROR_INVALID_DATA;
            ngx_log_error(NGX_LOG_ALERT, aio->file->log, 0,
                          "invalid buffer range in file AIO write for \"%V\"",
                          &aio->file->name);
            return NGX_ERROR;
        }

        if (aio->write_pos == NULL) {
            aio->write_pos = b->pos;
        }

        if (aio->write_pos < b->pos || aio->write_pos > b->last) {
            aio->err = ERROR_INVALID_DATA;
            ngx_log_error(NGX_LOG_ALERT, aio->file->log, 0,
                          "invalid write position in file AIO for \"%V\"",
                          &aio->file->name);
            return NGX_ERROR;
        }

        if (aio->write_pos < b->last) {
            return NGX_OK;
        }

        aio->write_chain = aio->write_chain->next;
        aio->write_pos = NULL;
    }

    return NGX_DONE;
}


static void
ngx_file_aio_write_advance(ngx_event_aio_t *aio, size_t n)
{
    size_t      size;
    ngx_buf_t  *b;

    if (aio->scalar) {
        aio->write_pos += n;
        aio->write_remaining -= n;
        return;
    }

    while (n && aio->write_chain) {
        b = aio->write_chain->buf;

        if (ngx_buf_special(b)) {
            aio->write_chain = aio->write_chain->next;
            aio->write_pos = NULL;
            continue;
        }

        if (aio->write_pos == NULL) {
            aio->write_pos = b->pos;
        }

        size = b->last - aio->write_pos;

        if (n < size) {
            aio->write_pos += n;
            return;
        }

        n -= size;
        aio->write_chain = aio->write_chain->next;
        aio->write_pos = NULL;
    }
}


static void
ngx_file_aio_write_complete(ngx_iocp_op_t *base)
{
    ngx_int_t            rc;
    ngx_err_t            err;
    ngx_event_aio_t     *aio;
    ngx_iocp_file_op_t  *op;

    op = (ngx_iocp_file_op_t *) base;
    aio = op->aio;
    err = base->error;

    if (err == 0) {
        if (base->bytes == 0 && aio->write_expected != 0) {
            err = ERROR_WRITE_FAULT;

        } else if (base->bytes > aio->write_expected) {
            err = ERROR_INVALID_DATA;
        }
    }

    if (err) {
        if (base->bytes && base->bytes <= aio->write_expected) {
            aio->file->offset += base->bytes;
            ngx_file_aio_write_advance(aio, base->bytes);
            aio->write_total += base->bytes;
        }

        ngx_file_aio_complete(aio, err, aio->write_total);
        return;
    }

    aio->file->offset += base->bytes;
    ngx_file_aio_write_advance(aio, base->bytes);
    aio->write_total += base->bytes;

    rc = ngx_file_aio_write_next(aio);

    if (rc == NGX_AGAIN) {
        return;
    }

    if (rc == NGX_OK || rc == NGX_DONE) {
        ngx_file_aio_complete(aio, 0, aio->write_total);

    } else {
        ngx_file_aio_complete(aio, aio->err ? aio->err : ERROR_WRITE_FAULT,
                              aio->write_total);
    }
}


static void
ngx_file_aio_complete(ngx_event_aio_t *aio, ngx_err_t err, size_t nbytes)
{
    ngx_event_t  *ev;

    ev = &aio->event;
    aio->err = err;
    aio->nbytes = nbytes;
    ev->iocp_op = NULL;
    ev->active = 0;
    ev->ready = 1;
    ev->complete = 1;

    if (ev->handler) {
        ev->handler(ev);
    }
}


static ssize_t
ngx_file_aio_result(ngx_file_t *file, ngx_event_aio_t *aio,
    ngx_uint_t writing)
{
    size_t     nbytes;
    ngx_err_t  err;

    if ((aio->writing ? 1 : 0) != (writing ? 1 : 0)) {
        ngx_log_error(NGX_LOG_ALERT, file->log, 0,
                      "mismatched file AIO completion for \"%V\"",
                      &file->name);
        return NGX_ERROR;
    }

    aio->event.complete = 0;
    err = aio->err;
    nbytes = aio->nbytes;
    aio->err = 0;
    aio->nbytes = 0;

    if (writing) {
        aio->writing = 0;
        aio->scalar = 0;
        aio->write_chain = NULL;
        aio->write_pos = NULL;
        aio->write_remaining = 0;
        aio->write_total = 0;
        aio->write_expected = 0;

    } else {
        aio->write_pos = NULL;
        aio->write_offset = 0;
        aio->write_expected = 0;
    }

    if (err) {
        ngx_set_errno(err);
        ngx_log_error(NGX_LOG_ERR, file->log, err,
                      "file AIO %s \"%V\" failed",
                      writing ? "write" : "read", &file->name);
        return NGX_ERROR;
    }

    if (nbytes > (size_t) NGX_MAX_SIZE_T_VALUE) {
        ngx_log_error(NGX_LOG_ALERT, file->log, 0,
                      "file AIO result is too large for \"%V\"",
                      &file->name);
        return NGX_ERROR;
    }

    return (ssize_t) nbytes;
}


static void
ngx_file_aio_event_handler(ngx_event_t *ev)
{
    ngx_event_aio_t  *aio;

    aio = ev->data;

    if (aio->handler) {
        aio->handler(ev);
    }
}
