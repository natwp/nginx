
/*
 * Windows IOCP datagram transport.
 *
 * The listener owns a queue of WSARecvMsg/WSARecvFrom operations.  A
 * completion is decoded on the nginx worker thread and then delivered to the
 * existing per-peer UDP/QUIC connection machinery.  No pseudo connection is
 * allocated while a receive is outstanding, so datagram prefetch cannot
 * consume worker_connections.
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>
#include <ngx_win32_worker.h>

#if (NGX_QUIC)
#include <ngx_event_quic.h>
#include <ngx_event_quic_connection.h>
#endif


#define NGX_IOCP_UDP_RECEIVE_SIZE  65535
#define NGX_IOCP_UDP_CONTROL_SIZE  512
#define NGX_IOCP_UDP_BUFS          64


typedef struct {
    ngx_iocp_op_t  op;
    ngx_listening_t *listening;
    WSAMSG         msg;
    WSABUF         wsabuf;
    ngx_sockaddr_t remote;
    int            remote_len;
    u_char         control[NGX_IOCP_UDP_CONTROL_SIZE];
    u_char         data[NGX_IOCP_UDP_RECEIVE_SIZE];
    unsigned       recvmsg:1;
} ngx_iocp_udp_recv_op_t;


typedef struct {
    ngx_iocp_op_t  op;
    WSAMSG         msg;
    WSABUF         bufs[NGX_IOCP_UDP_BUFS];
    u_char        *buffers[NGX_IOCP_UDP_BUFS];
    ngx_sockaddr_t sockaddr;
    u_char         control[NGX_IOCP_UDP_CONTROL_SIZE];
    ngx_uint_t     nbufs;
} ngx_iocp_udp_send_op_t;


static void ngx_iocp_udp_recv_complete(ngx_iocp_op_t *base);
static void ngx_iocp_udp_recv_cleanup(ngx_iocp_op_t *base);
static void ngx_iocp_udp_send_complete(ngx_iocp_op_t *base);
static void ngx_iocp_udp_send_cleanup(ngx_iocp_op_t *base);
static ngx_int_t ngx_iocp_udp_dispatch(ngx_listening_t *ls, u_char *data,
    size_t size, struct sockaddr *sockaddr, socklen_t socklen,
    struct sockaddr *local_sockaddr, socklen_t local_socklen);
static ngx_int_t ngx_iocp_udp_enable_pktinfo(ngx_listening_t *ls);
static void ngx_iocp_udp_disable_connreset(ngx_listening_t *ls);
static ngx_int_t ngx_iocp_udp_sockaddr_valid(struct sockaddr *sockaddr,
    socklen_t socklen);
static void ngx_iocp_close_accepted_udp_connection(ngx_connection_t *c);
static ngx_int_t ngx_udp_insert_connection(ngx_connection_t *c);
static ngx_connection_t *ngx_udp_lookup_connection(ngx_listening_t *ls,
    struct sockaddr *sockaddr, socklen_t socklen,
    struct sockaddr *local_sockaddr, socklen_t local_socklen);
static ssize_t ngx_udp_shared_recv_win32(ngx_connection_t *c, u_char *buf,
    size_t size);
static ngx_int_t ngx_iocp_udp_build_msg(ngx_iocp_udp_send_op_t *op,
    ngx_connection_t *c, struct msghdr *msg);
static ngx_chain_t *ngx_iocp_udp_update_sent(ngx_chain_t *in, off_t sent);


ngx_int_t
ngx_iocp_post_udp_receives(ngx_listening_t *ls, ngx_uint_t n)
{
    ngx_connection_t       *lc;
    ngx_iocp_udp_recv_op_t  *op;
    ngx_uint_t               i;
    int                      rc;
    ngx_err_t                err;

    lc = ls->connection;
    if (lc == NULL || lc->iocp == NULL) {
        return NGX_ERROR;
    }

    if (ls->sockaddr == NULL
        || ls->socklen > (socklen_t) sizeof(ngx_sockaddr_t)
        || ngx_iocp_udp_sockaddr_valid(ls->sockaddr, ls->socklen) != NGX_OK)
    {
        ngx_log_error(NGX_LOG_EMERG, &ls->log, 0,
                      "invalid UDP listener address for %V", &ls->addr_text);
        return NGX_ERROR;
    }

    if (ls->wildcard && lc->iocp->recvmsg == NULL) {
        ngx_log_error(NGX_LOG_EMERG, &ls->log, 0,
                      "WSARecvMsg() is required for wildcard UDP listener %V",
                      &ls->addr_text);
        return NGX_ERROR;
    }

    ngx_iocp_udp_disable_connreset(ls);

    if (ngx_iocp_udp_enable_pktinfo(ls) != NGX_OK) {
        return NGX_ERROR;
    }

    for (i = 0; i < n; i++) {
        op = (ngx_iocp_udp_recv_op_t *)
             ngx_iocp_op_create(sizeof(ngx_iocp_udp_recv_op_t), lc->iocp,
                                NULL, NULL, NGX_IOCP_OP_UDP_RECV,
                                ngx_iocp_udp_recv_complete,
                                ngx_iocp_udp_recv_cleanup);
        if (op == NULL) {
            return NGX_ERROR;
        }

        op->listening = ls;
        op->remote_len = sizeof(op->remote);
        op->wsabuf.buf = (char *) op->data;
        op->wsabuf.len = sizeof(op->data);
        op->msg.name = (LPSOCKADDR) &op->remote;
        op->msg.namelen = sizeof(op->remote);
        op->msg.lpBuffers = &op->wsabuf;
        op->msg.dwBufferCount = 1;
        op->msg.Control.buf = (char *) op->control;
        op->msg.Control.len = sizeof(op->control);
        op->msg.dwFlags = 0;
        op->op.bytes = 0;
        op->op.expected = sizeof(op->data);
        op->op.flags = 0;

        if (lc->iocp->recvmsg) {
            op->recvmsg = 1;
            rc = lc->iocp->recvmsg(lc->fd, &op->msg, &op->op.bytes,
                                   &op->op.overlapped, NULL);
        } else {
            op->recvmsg = 0;
            rc = WSARecvFrom(lc->fd, &op->wsabuf, 1, &op->op.bytes,
                             &op->op.flags, (LPSOCKADDR) &op->remote,
                             &op->remote_len, &op->op.overlapped, NULL);
        }

        if (rc == SOCKET_ERROR) {
            err = ngx_socket_errno;

            if (err == WSA_IO_PENDING) {
                continue;
            }

            ngx_log_error(NGX_LOG_ALERT, &ls->log, err,
                          "%s failed for %V",
                          op->recvmsg ? "WSARecvMsg()" : "WSARecvFrom()",
                          &ls->addr_text);
            ngx_iocp_op_abort(&op->op);
            return NGX_ERROR;
        }
    }

    return NGX_OK;
}


static void
ngx_iocp_udp_disable_connreset(ngx_listening_t *ls)
{
    BOOL               enabled;
    DWORD              bytes;
    ngx_err_t          err;
    ngx_connection_t  *c;

    c = ls->connection;

    if (c->iocp->udp_connreset) {
        return;
    }

    c->iocp->udp_connreset = 1;
    enabled = FALSE;
    bytes = 0;

    if (WSAIoctl(c->fd, SIO_UDP_CONNRESET, &enabled, sizeof(enabled),
                 NULL, 0, &bytes, NULL, NULL)
        == SOCKET_ERROR)
    {
        err = ngx_socket_errno;
        ngx_log_error(NGX_LOG_WARN, &ls->log, err,
                      "WSAIoctl(SIO_UDP_CONNRESET) failed for %V, ignored",
                      &ls->addr_text);
    }
}


static ngx_int_t
ngx_iocp_udp_enable_pktinfo(ngx_listening_t *ls)
{
    int                on;
    ngx_err_t          err;
    ngx_connection_t  *c;

    c = ls->connection;

    if (!ls->wildcard || c->iocp->udp_pktinfo) {
        return NGX_OK;
    }

    on = 1;

    switch (ls->sockaddr->sa_family) {

    case AF_INET:
        if (setsockopt(c->fd, IPPROTO_IP, IP_PKTINFO, (const char *) &on,
                       sizeof(on)) == -1)
        {
            err = ngx_socket_errno;
            ngx_log_error(NGX_LOG_EMERG, &ls->log, err,
                          "setsockopt(IP_PKTINFO) failed for %V",
                          &ls->addr_text);
            return NGX_ERROR;
        }
        break;

#if (NGX_HAVE_INET6)
    case AF_INET6:
        if (setsockopt(c->fd, IPPROTO_IPV6, IPV6_PKTINFO, (const char *) &on,
                       sizeof(on)) == -1)
        {
            err = ngx_socket_errno;
            ngx_log_error(NGX_LOG_EMERG, &ls->log, err,
                          "setsockopt(IPV6_PKTINFO) failed for %V",
                          &ls->addr_text);
            return NGX_ERROR;
        }
        break;
#endif

    default:
        ngx_log_error(NGX_LOG_EMERG, &ls->log, 0,
                      "unsupported UDP address family %d for %V",
                      ls->sockaddr->sa_family, &ls->addr_text);
        return NGX_ERROR;
    }

    c->iocp->udp_pktinfo = 1;

    return NGX_OK;
}


static ngx_int_t
ngx_iocp_udp_sockaddr_valid(struct sockaddr *sockaddr, socklen_t socklen)
{
    if (sockaddr == NULL
        || socklen < (socklen_t) sizeof(sockaddr->sa_family))
    {
        return NGX_ERROR;
    }

    switch (sockaddr->sa_family) {

    case AF_INET:
        return (socklen >= (socklen_t) sizeof(struct sockaddr_in))
               ? NGX_OK : NGX_ERROR;

#if (NGX_HAVE_INET6)
    case AF_INET6:
        return (socklen >= (socklen_t) sizeof(struct sockaddr_in6))
               ? NGX_OK : NGX_ERROR;
#endif

    default:
        return NGX_ERROR;
    }
}


void
ngx_event_recvmsg(ngx_event_t *ev)
{
    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, ev->log, 0,
                   "IOCP datagram event for fd:%d",
                   ((ngx_connection_t *) ev->data)->fd);
}


static void
ngx_iocp_udp_recv_complete(ngx_iocp_op_t *base)
{
    size_t                  len, remaining, step;
    u_char                 *control, *end;
    struct cmsghdr         *cmsg;
    struct msghdr           msg;
    ngx_iocp_udp_recv_op_t  *op;
    ngx_listening_t         *ls;
    ngx_sockaddr_t           local;
    ngx_uint_t               local_found;
    socklen_t                socklen, local_socklen;
    struct sockaddr         *local_sockaddr;

    op = (ngx_iocp_udp_recv_op_t *) base;
    ls = op->listening;

    if (ngx_iocp_post_udp_receives(ls, 1) != NGX_OK) {
        ngx_log_error(NGX_LOG_ALERT, &ls->log, 0,
                      "could not replenish UDP receive queue for %V",
                      &ls->addr_text);
    }

    if (base->error) {
        ngx_log_error(NGX_LOG_ERR, &ls->log, base->error,
                      "%s completed with an error for %V",
                      op->recvmsg ? "WSARecvMsg()" : "WSARecvFrom()",
                      &ls->addr_text);
        return;
    }

    if (base->bytes > sizeof(op->data)) {
        ngx_log_error(NGX_LOG_ERR, &ls->log, 0,
                      "IOCP received an oversized UDP datagram for %V",
                      &ls->addr_text);
        return;
    }

    if (op->recvmsg && op->msg.dwFlags & (MSG_TRUNC|MSG_CTRUNC)) {
        ngx_log_error(NGX_LOG_ERR, &ls->log, 0,
                      "WSARecvMsg() truncated a datagram for %V",
                      &ls->addr_text);
        return;
    }

    if (op->recvmsg) {
        socklen = (socklen_t) op->msg.namelen;

    } else {
        socklen = (socklen_t) op->remote_len;
    }

    if (socklen <= 0 || socklen > (socklen_t) sizeof(op->remote)
        || ngx_iocp_udp_sockaddr_valid(&op->remote.sockaddr, socklen)
           != NGX_OK)
    {
        ngx_log_error(NGX_LOG_ERR, &ls->log, 0,
                      "IOCP received an invalid UDP peer address for %V",
                      &ls->addr_text);
        return;
    }

    local_sockaddr = ls->sockaddr;
    local_socklen = ls->socklen;
    local_found = ls->wildcard ? 0 : 1;

    if (ls->wildcard && op->recvmsg) {
        if (op->msg.Control.len > sizeof(op->control)) {
            ngx_log_error(NGX_LOG_ERR, &ls->log, 0,
                          "WSARecvMsg() returned invalid control data for %V",
                          &ls->addr_text);
            return;
        }

        ngx_memcpy(&local, ls->sockaddr, ls->socklen);
        local_sockaddr = &local.sockaddr;

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
                ngx_log_error(NGX_LOG_ERR, &ls->log, 0,
                              "WSARecvMsg() returned malformed control data "
                              "for %V", &ls->addr_text);
                return;
            }

            if (ngx_get_srcaddr_cmsg(cmsg, local_sockaddr) == NGX_OK) {
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

        if (!local_found) {
            ngx_log_error(NGX_LOG_ERR, &ls->log, 0,
                          "WSARecvMsg() returned no destination address for %V",
                          &ls->addr_text);
            return;
        }
    }

    (void) ngx_iocp_udp_dispatch_datagram(ls, op->data, base->bytes,
                                 (struct sockaddr *) &op->remote, socklen,
                                 local_sockaddr, local_socklen);
}


static void
ngx_iocp_udp_recv_cleanup(ngx_iocp_op_t *base)
{
    /* The receive buffer and WSAMSG are part of the operation allocation. */
    (void) base;
}


ngx_int_t
ngx_iocp_udp_dispatch_datagram(ngx_listening_t *ls, u_char *data, size_t size,
    struct sockaddr *sockaddr, socklen_t socklen,
    struct sockaddr *local_sockaddr, socklen_t local_socklen)
{
#if (NGX_QUIC)
    if (ls->quic) {
        ngx_quic_iocp_dispatch(ls, data, size, sockaddr, socklen,
                               local_sockaddr, local_socklen);
        return NGX_OK;
    }
#endif

    return ngx_iocp_udp_dispatch(ls, data, size, sockaddr, socklen,
                                 local_sockaddr, local_socklen);
}


static ngx_int_t
ngx_iocp_udp_dispatch(ngx_listening_t *ls, u_char *data, size_t size,
    struct sockaddr *sockaddr, socklen_t socklen,
    struct sockaddr *local_sockaddr, socklen_t local_socklen)
{
    ngx_buf_t          buf;
    ngx_log_t          *log;
    ngx_event_t        *rev, *wev;
    ngx_uint_t          instance;
    ngx_connection_t   *c, *lc;
#if (NGX_DEBUG)
    ngx_event_conf_t   *ecf;
#endif

    lc = ls->connection;

    if (lc == NULL || lc->fd == (ngx_socket_t) -1) {
        ngx_log_debug0(NGX_LOG_DEBUG_EVENT, &ls->log, 0,
                       "dropping UDP datagram for closed listener");
        return NGX_OK;
    }

#if (NGX_DEBUG)
    ecf = ngx_event_get_conf(ngx_cycle->conf_ctx, ngx_event_core_module);
#endif

    c = ngx_udp_lookup_connection(ls, sockaddr, socklen, local_sockaddr,
                                  local_socklen);

    if (c) {
        ngx_memzero(&buf, sizeof(ngx_buf_t));
        buf.pos = data;
        buf.last = data + size;
        buf.start = data;
        buf.end = data + NGX_IOCP_UDP_RECEIVE_SIZE;

        c->udp->buffer = &buf;
        rev = c->read;
        instance = rev->instance;
        rev->ready = 1;
        rev->active = 0;
        rev->handler(rev);

        if (c->fd == (ngx_socket_t) -1 || rev->instance != instance) {
            return NGX_OK;
        }

        if (c->udp) {
            c->udp->buffer = NULL;
        }

        rev->ready = 0;
        rev->active = 1;

        return NGX_OK;
    }

#if (NGX_STAT_STUB)
    (void) ngx_atomic_fetch_add(ngx_stat_accepted, 1);
#endif

    ngx_accept_disabled = ngx_cycle->connection_n / 8
                          - ngx_cycle->free_connection_n;

    c = ngx_get_connection(lc->fd, &ls->log);
    if (c == NULL) {
        return NGX_ERROR;
    }

    c->shared = 1;
    c->type = SOCK_DGRAM;
    c->socklen = socklen;

#if (NGX_STAT_STUB)
    (void) ngx_atomic_fetch_add(ngx_stat_active, 1);
#endif

    c->pool = ngx_create_pool(ls->pool_size, &ls->log);
    if (c->pool == NULL) {
        ngx_iocp_close_accepted_udp_connection(c);
        return NGX_ERROR;
    }

    c->sockaddr = ngx_palloc(c->pool, socklen);
    if (c->sockaddr == NULL) {
        ngx_iocp_close_accepted_udp_connection(c);
        return NGX_ERROR;
    }

    ngx_memcpy(c->sockaddr, sockaddr, socklen);

    log = ngx_palloc(c->pool, sizeof(ngx_log_t));
    if (log == NULL) {
        ngx_iocp_close_accepted_udp_connection(c);
        return NGX_ERROR;
    }

    *log = ls->log;

    c->recv = ngx_udp_shared_recv_win32;
    c->send = ngx_udp_send;
    c->send_chain = ngx_udp_send_chain;
    c->need_flush_buf = 1;
    c->log = log;
    c->pool->log = log;
    c->listening = ls;

    if (local_sockaddr != ls->sockaddr) {
        c->local_sockaddr = ngx_palloc(c->pool, local_socklen);
        if (c->local_sockaddr == NULL) {
            ngx_iocp_close_accepted_udp_connection(c);
            return NGX_ERROR;
        }

        ngx_memcpy(c->local_sockaddr, local_sockaddr, local_socklen);

    } else {
        c->local_sockaddr = local_sockaddr;
    }

    c->local_socklen = local_socklen;

    if (!ngx_win32_worker_routed
        && ngx_iocp_create_shared_owner(c, lc->iocp) == NULL)
    {
        ngx_iocp_close_accepted_udp_connection(c);
        return NGX_ERROR;
    }

    c->buffer = ngx_create_temp_buf(c->pool, size ? size : 1);
    if (c->buffer == NULL) {
        ngx_iocp_close_accepted_udp_connection(c);
        return NGX_ERROR;
    }

    c->buffer->last = ngx_cpymem(c->buffer->last, data, size);

    rev = c->read;
    wev = c->write;
    rev->active = 1;
    wev->ready = 1;
    rev->log = log;
    wev->log = log;

    if (ls->addr_ntop) {
        c->addr_text.data = ngx_pnalloc(c->pool, ls->addr_text_max_len);
        if (c->addr_text.data == NULL) {
            ngx_iocp_close_accepted_udp_connection(c);
            return NGX_ERROR;
        }

        c->addr_text.len = ngx_sock_ntop(c->sockaddr, c->socklen,
                                         c->addr_text.data,
                                         ls->addr_text_max_len, 0);
        if (c->addr_text.len == 0) {
            ngx_iocp_close_accepted_udp_connection(c);
            return NGX_ERROR;
        }
    }

    c->number = ngx_atomic_fetch_add(ngx_connection_counter, 1);
    c->start_time = ngx_current_msec;

#if (NGX_STAT_STUB)
    (void) ngx_atomic_fetch_add(ngx_stat_handled, 1);
#endif

#if (NGX_DEBUG)
    {
    ngx_str_t  addr;
    u_char     text[NGX_SOCKADDR_STRLEN];

    ngx_debug_accepted_connection(ecf, c);

    if (log->log_level & NGX_LOG_DEBUG_EVENT) {
        addr.data = text;
        addr.len = ngx_sock_ntop(c->sockaddr, c->socklen, text,
                                 NGX_SOCKADDR_STRLEN, 1);

        ngx_log_debug4(NGX_LOG_DEBUG_EVENT, log, 0,
                       "*%uA IOCP recvmsg: %V fd:%d n:%uz",
                       c->number, &addr, c->fd, size);
    }

    }
#endif

    if (ngx_udp_insert_connection(c) != NGX_OK) {
        ngx_iocp_close_accepted_udp_connection(c);
        return NGX_ERROR;
    }

    log->data = NULL;
    log->handler = NULL;
    ls->handler(c);

    return NGX_OK;
}


static void
ngx_iocp_close_accepted_udp_connection(ngx_connection_t *c)
{
    ngx_pool_t  *pool;

    pool = c->pool;

    if (c->iocp) {
        ngx_close_connection(c);

    } else {
        ngx_free_connection(c);
        c->fd = (ngx_socket_t) -1;
    }

    if (pool) {
        ngx_destroy_pool(pool);
    }

#if (NGX_STAT_STUB)
    (void) ngx_atomic_fetch_add(ngx_stat_active, -1);
#endif
}


static ssize_t
ngx_udp_shared_recv_win32(ngx_connection_t *c, u_char *buf, size_t size)
{
    size_t       n;
    ngx_buf_t   *b;

    if (c->udp == NULL || c->udp->buffer == NULL) {
        return NGX_AGAIN;
    }

    b = c->udp->buffer;

    if ((size && buf == NULL) || b->last < b->pos) {
        c->read->error = 1;
        ngx_log_error(NGX_LOG_ALERT, c->log, 0,
                      "invalid buffer in an IOCP UDP receive");
        return NGX_ERROR;
    }

    n = ngx_min((size_t) (b->last - b->pos), size);

    if (n) {
        ngx_memcpy(buf, b->pos, n);
    }

    c->udp->buffer = NULL;
    c->read->ready = 0;
    c->read->active = 1;

    return (ssize_t) n;
}


void
ngx_udp_rbtree_insert_value(ngx_rbtree_node_t *temp,
    ngx_rbtree_node_t *node, ngx_rbtree_node_t *sentinel)
{
    ngx_int_t               rc;
    ngx_connection_t       *c, *ct;
    ngx_rbtree_node_t      **p;
    ngx_udp_connection_t    *udp, *udpt;

    for ( ;; ) {
        if (node->key < temp->key) {
            p = &temp->left;

        } else if (node->key > temp->key) {
            p = &temp->right;

        } else {
            udp = (ngx_udp_connection_t *) node;
            c = udp->connection;
            udpt = (ngx_udp_connection_t *) temp;
            ct = udpt->connection;

            rc = ngx_memn2cmp(udp->key.data, udpt->key.data,
                              udp->key.len, udpt->key.len);

            if (rc == 0 && c->listening->wildcard) {
                rc = ngx_cmp_sockaddr(c->local_sockaddr, c->local_socklen,
                                      ct->local_sockaddr, ct->local_socklen,
                                      1);
            }

            p = (rc < 0) ? &temp->left : &temp->right;
        }

        if (*p == sentinel) {
            break;
        }

        temp = *p;
    }

    *p = node;
    node->parent = temp;
    node->left = sentinel;
    node->right = sentinel;
    ngx_rbt_red(node);
}


static ngx_int_t
ngx_udp_insert_connection(ngx_connection_t *c)
{
    uint32_t             hash;
    ngx_pool_cleanup_t  *cln;
    ngx_udp_connection_t *udp;

    if (c->udp) {
        return NGX_OK;
    }

    udp = ngx_pcalloc(c->pool, sizeof(ngx_udp_connection_t));
    if (udp == NULL) {
        return NGX_ERROR;
    }

    udp->connection = c;
    ngx_crc32_init(hash);
    ngx_crc32_update(&hash, (u_char *) c->sockaddr, c->socklen);

    if (c->listening->wildcard) {
        ngx_crc32_update(&hash, (u_char *) c->local_sockaddr,
                         c->local_socklen);
    }

    ngx_crc32_final(hash);
    udp->node.key = hash;
    udp->key.data = (u_char *) c->sockaddr;
    udp->key.len = c->socklen;

    cln = ngx_pool_cleanup_add(c->pool, 0);
    if (cln == NULL) {
        return NGX_ERROR;
    }

    cln->data = c;
    cln->handler = ngx_delete_udp_connection;
    ngx_rbtree_insert(&c->listening->rbtree, &udp->node);
    c->udp = udp;

    return NGX_OK;
}


void
ngx_delete_udp_connection(void *data)
{
    ngx_connection_t  *c;

    c = data;
    if (c->udp == NULL) {
        return;
    }

    ngx_rbtree_delete(&c->listening->rbtree, &c->udp->node);
    c->udp = NULL;
}


static ngx_connection_t *
ngx_udp_lookup_connection(ngx_listening_t *ls, struct sockaddr *sockaddr,
    socklen_t socklen, struct sockaddr *local_sockaddr,
    socklen_t local_socklen)
{
    uint32_t             hash;
    ngx_int_t            rc;
    ngx_connection_t    *c;
    ngx_rbtree_node_t   *node, *sentinel;
    ngx_udp_connection_t *udp;

    node = ls->rbtree.root;
    sentinel = ls->rbtree.sentinel;
    ngx_crc32_init(hash);
    ngx_crc32_update(&hash, (u_char *) sockaddr, socklen);

    if (ls->wildcard) {
        ngx_crc32_update(&hash, (u_char *) local_sockaddr, local_socklen);
    }

    ngx_crc32_final(hash);

    while (node != sentinel) {
        if (hash < node->key) {
            node = node->left;
            continue;
        }
        if (hash > node->key) {
            node = node->right;
            continue;
        }

        udp = (ngx_udp_connection_t *) node;
        c = udp->connection;
        rc = ngx_cmp_sockaddr(sockaddr, socklen, c->sockaddr, c->socklen, 1);

        if (rc == 0 && ls->wildcard) {
            rc = ngx_cmp_sockaddr(local_sockaddr, local_socklen,
                                  c->local_sockaddr, c->local_socklen, 1);
        }

        if (rc == 0) {
            return c;
        }

        node = (rc < 0) ? node->left : node->right;
    }

    return NULL;
}


size_t
ngx_set_srcaddr_cmsg(struct cmsghdr *cmsg, struct sockaddr *local_sockaddr)
{
    IN_PKTINFO          *pkt;
    struct sockaddr_in *sin;
#if (NGX_HAVE_INET6)
    IN6_PKTINFO          *pkt6;
    struct sockaddr_in6 *sin6;
#endif

    if (local_sockaddr->sa_family == AF_INET) {
        cmsg->cmsg_level = IPPROTO_IP;
        cmsg->cmsg_type = IP_PKTINFO;
        cmsg->cmsg_len = CMSG_LEN(sizeof(IN_PKTINFO));

        sin = (struct sockaddr_in *) local_sockaddr;
        pkt = (IN_PKTINFO *) CMSG_DATA(cmsg);
        ngx_memzero(pkt, sizeof(IN_PKTINFO));
        pkt->ipi_addr = sin->sin_addr;

        return CMSG_SPACE(sizeof(IN_PKTINFO));
    }

#if (NGX_HAVE_INET6)
    if (local_sockaddr->sa_family == AF_INET6) {
        cmsg->cmsg_level = IPPROTO_IPV6;
        cmsg->cmsg_type = IPV6_PKTINFO;
        cmsg->cmsg_len = CMSG_LEN(sizeof(IN6_PKTINFO));

        sin6 = (struct sockaddr_in6 *) local_sockaddr;
        pkt6 = (IN6_PKTINFO *) CMSG_DATA(cmsg);
        ngx_memzero(pkt6, sizeof(IN6_PKTINFO));
        pkt6->ipi6_addr = sin6->sin6_addr;
        pkt6->ipi6_ifindex = sin6->sin6_scope_id;

        return CMSG_SPACE(sizeof(IN6_PKTINFO));
    }
#endif

    return 0;
}


ngx_int_t
ngx_get_srcaddr_cmsg(struct cmsghdr *cmsg, struct sockaddr *local_sockaddr)
{
    IN_PKTINFO          *pkt;
    struct sockaddr_in *sin;
#if (NGX_HAVE_INET6)
    IN6_PKTINFO          *pkt6;
    struct sockaddr_in6 *sin6;
#endif

    if (cmsg->cmsg_level == IPPROTO_IP && cmsg->cmsg_type == IP_PKTINFO
        && cmsg->cmsg_len >= CMSG_LEN(sizeof(IN_PKTINFO))
        && local_sockaddr->sa_family == AF_INET)
    {
        pkt = (IN_PKTINFO *) CMSG_DATA(cmsg);
        sin = (struct sockaddr_in *) local_sockaddr;
        sin->sin_addr = pkt->ipi_addr;

        return NGX_OK;
    }

#if (NGX_HAVE_INET6)
    if (cmsg->cmsg_level == IPPROTO_IPV6
        && cmsg->cmsg_type == IPV6_PKTINFO
        && cmsg->cmsg_len >= CMSG_LEN(sizeof(IN6_PKTINFO))
        && local_sockaddr->sa_family == AF_INET6)
    {
        pkt6 = (IN6_PKTINFO *) CMSG_DATA(cmsg);
        sin6 = (struct sockaddr_in6 *) local_sockaddr;
        sin6->sin6_addr = pkt6->ipi6_addr;
        sin6->sin6_scope_id = pkt6->ipi6_ifindex;

        return NGX_OK;
    }
#endif

    return NGX_DECLINED;
}


ssize_t
ngx_sendmsg(ngx_connection_t *c, struct msghdr *msg, int flags)
{
    DWORD                    bytes;
    int                      rc;
    ngx_err_t                err;
    ngx_pool_t              *pool;
    ngx_iocp_owner_t        *owner;
    ngx_iocp_udp_send_op_t  *op;

    if (ngx_win32_worker_routed && c->listening
        && c->listening->type == SOCK_DGRAM)
    {
        return ngx_win32_worker_udp_sendmsg(c, msg, flags);
    }

    owner = c->iocp;

    if (owner == NULL) {
        c->write->error = 1;
        ngx_log_error(NGX_LOG_ALERT, c->log, 0,
                      "UDP socket is not associated with IOCP");
        return NGX_ERROR;
    }

    if (c->write->complete) {
        DWORD  expected;

        c->write->complete = 0;
        c->write->active = 0;
        err = c->write->iocp_error;
        c->write->iocp_error = 0;
        bytes = (DWORD) c->write->iocp_bytes;
        c->write->iocp_bytes = 0;
        c->write->available = 0;
        expected = (DWORD) c->write->iocp_expected;
        c->write->iocp_expected = 0;

        if (err) {
            c->write->error = 1;
            ngx_connection_error(c, err, "WSASendMsg() failed");
            return NGX_ERROR;
        }

        if (bytes != expected) {
            c->write->error = 1;
            ngx_connection_error(c, WSAEMSGSIZE,
                                 "WSASendMsg() sent a partial datagram");
            return NGX_ERROR;
        }

        c->write->ready = 1;

        if (msg == NULL) {
            return 0;
        }
    }

    if (c->write->iocp_op) {
        return NGX_AGAIN;
    }

    if (!c->write->ready) {
        return NGX_AGAIN;
    }

    pool = c->write->iocp_pool ? c->write->iocp_pool : c->pool;

    op = (ngx_iocp_udp_send_op_t *)
         ngx_iocp_op_create(sizeof(ngx_iocp_udp_send_op_t), owner,
                            c->write, pool, NGX_IOCP_OP_UDP_SEND,
                            ngx_iocp_udp_send_complete,
                            ngx_iocp_udp_send_cleanup);
    if (op == NULL || ngx_iocp_udp_build_msg(op, c, msg) != NGX_OK) {
        if (op) {
            ngx_iocp_op_abort(&op->op);
        }
        ngx_log_error(NGX_LOG_ALERT, c->log, 0,
                      "invalid UDP message for IOCP send");
        c->write->error = 1;
        return NGX_ERROR;
    }

    if (op->msg.Control.len && owner->sendmsg == NULL) {
        ngx_log_error(NGX_LOG_ALERT, c->log, 0,
                      "WSASendMsg() is required to select a UDP source address");
        ngx_iocp_op_abort(&op->op);
        c->write->error = 1;
        return NGX_ERROR;
    }

    bytes = 0;
    if (owner->sendmsg) {
        rc = owner->sendmsg((SOCKET) owner->handle, &op->msg, (DWORD) flags,
                            &bytes,
                            &op->op.overlapped, NULL);

    } else if (op->msg.name) {
        rc = WSASendTo((SOCKET) owner->handle, op->bufs,
                       (DWORD) op->nbufs, &bytes,
                       (DWORD) flags, op->msg.name, op->msg.namelen,
                       &op->op.overlapped, NULL);

    } else {
        rc = WSASend((SOCKET) owner->handle, op->bufs,
                     (DWORD) op->nbufs, &bytes,
                     (DWORD) flags, &op->op.overlapped, NULL);
    }
    c->write->active = 1;
    c->write->ready = 0;

    if (rc == SOCKET_ERROR) {
        err = ngx_socket_errno;
        if (err == WSA_IO_PENDING) {
            return (ssize_t) op->op.expected;
        }

        ngx_iocp_op_abort(&op->op);
        c->write->active = 0;
        c->write->ready = 1;
        c->write->error = 1;
        ngx_connection_error(c, err, "WSASendMsg() failed");
        return NGX_ERROR;
    }

    return (ssize_t) op->op.expected;
}


static ngx_int_t
ngx_iocp_udp_build_msg(ngx_iocp_udp_send_op_t *op, ngx_connection_t *c,
    struct msghdr *msg)
{
    ngx_uint_t  i;

    if (msg == NULL || msg->msg_iovlen > NGX_IOCP_UDP_BUFS
        || (msg->msg_iovlen && msg->msg_iov == NULL)
        || (msg->msg_name == NULL && msg->msg_namelen != 0)
        || (msg->msg_name != NULL && msg->msg_namelen <= 0)
        || (msg->msg_control == NULL && msg->msg_controllen != 0))
    {
        return NGX_ERROR;
    }

    if (msg->msg_name && msg->msg_namelen > 0) {
        if (msg->msg_namelen > (socklen_t) sizeof(op->sockaddr)) {
            return NGX_ERROR;
        }

        if (ngx_iocp_udp_sockaddr_valid(msg->msg_name, msg->msg_namelen)
            != NGX_OK)
        {
            return NGX_ERROR;
        }

        ngx_memcpy(&op->sockaddr, msg->msg_name,
                   (size_t) msg->msg_namelen);
        op->msg.name = (LPSOCKADDR) &op->sockaddr;
        op->msg.namelen = (INT) msg->msg_namelen;
    }

    op->nbufs = (ngx_uint_t) msg->msg_iovlen;

    for (i = 0; i < msg->msg_iovlen; i++) {
        if (msg->msg_iov[i].iov_len && msg->msg_iov[i].iov_base == NULL) {
            return NGX_ERROR;
        }

        if (msg->msg_iov[i].iov_len > NGX_MAX_UINT32_VALUE
            || msg->msg_iov[i].iov_len
               > NGX_MAX_UINT32_VALUE - op->op.expected
            || msg->msg_iov[i].iov_len
               > NGX_IOCP_UDP_RECEIVE_SIZE - op->op.expected)
        {
            return NGX_ERROR;
        }

        op->bufs[i].buf = (char *) msg->msg_iov[i].iov_base;
        op->bufs[i].len = (ULONG) msg->msg_iov[i].iov_len;

        if (op->bufs[i].len) {
            op->buffers[i] = ngx_alloc(op->bufs[i].len, c->log);
            if (op->buffers[i] == NULL) {
                return NGX_ERROR;
            }

            ngx_memcpy(op->buffers[i], msg->msg_iov[i].iov_base,
                       op->bufs[i].len);
            op->bufs[i].buf = (char *) op->buffers[i];

        } else {
            op->bufs[i].buf = (char *) op->control;
        }

        op->op.expected += op->bufs[i].len;
    }

    op->msg.lpBuffers = op->bufs;

    if (op->nbufs == 0) {
        op->bufs[0].buf = (char *) op->control;
        op->bufs[0].len = 0;
        op->nbufs = 1;
    }

    op->msg.dwBufferCount = (DWORD) op->nbufs;

    if (msg->msg_control && msg->msg_controllen) {
        if (msg->msg_controllen > sizeof(op->control)) {
            return NGX_ERROR;
        }

        ngx_memcpy(op->control, msg->msg_control, msg->msg_controllen);
        op->msg.Control.buf = (char *) op->control;
        op->msg.Control.len = (DWORD) msg->msg_controllen;
    }

    op->msg.dwFlags = 0;

    return NGX_OK;
}


static void
ngx_iocp_udp_send_complete(ngx_iocp_op_t *base)
{
    ngx_iocp_event_complete(base);
}


static void
ngx_iocp_udp_send_cleanup(ngx_iocp_op_t *base)
{
    ngx_uint_t               i;
    ngx_iocp_udp_send_op_t  *op;

    op = (ngx_iocp_udp_send_op_t *) base;

    for (i = 0; i < op->nbufs; i++) {
        if (op->buffers[i]) {
            ngx_free(op->buffers[i]);
            op->buffers[i] = NULL;
        }
    }
}


ssize_t
ngx_iocp_udp_send(ngx_connection_t *c, u_char *buf, size_t size)
{
    struct cmsghdr *cmsg;
    struct iovec  iov;
    struct msghdr msg;
    ssize_t       n;
    u_char        control[CMSG_SPACE(sizeof(ngx_addrinfo_t))];

    if ((size && buf == NULL) || size > NGX_IOCP_UDP_RECEIVE_SIZE) {
        c->write->error = 1;
        ngx_log_error(NGX_LOG_ALERT, c->log, 0,
                      "invalid buffer in an IOCP UDP send");
        return NGX_ERROR;
    }

    ngx_memzero(&iov, sizeof(iov));
    iov.iov_base = buf;
    iov.iov_len = size;
    ngx_memzero(&msg, sizeof(msg));
    msg.msg_name = c->sockaddr;
    msg.msg_namelen = c->socklen;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    if (c->listening && c->listening->wildcard && c->local_sockaddr) {
        ngx_memzero(control, sizeof(control));
        msg.msg_control = control;
        msg.msg_controllen = sizeof(control);
        cmsg = CMSG_FIRSTHDR(&msg);
        msg.msg_controllen = ngx_set_srcaddr_cmsg(cmsg,
                                                  c->local_sockaddr);
        if (msg.msg_controllen == 0) {
            c->write->error = 1;
            ngx_log_error(NGX_LOG_ALERT, c->log, 0,
                          "cannot select UDP source address family %d",
                          c->local_sockaddr->sa_family);
            return NGX_ERROR;
        }
    }

    n = ngx_sendmsg(c, &msg, 0);
    if (n >= 0) {
        c->sent += n;
    }

    return n;
}


ngx_chain_t *
ngx_iocp_udp_send_chain(ngx_connection_t *c, ngx_chain_t *in, off_t limit)
{
    struct cmsghdr *cmsg;
    struct iovec  iovs[NGX_IOCP_UDP_BUFS];
    struct iovec *iov;
    struct msghdr msg;
    u_char       *prev;
    ngx_chain_t  *cl;
    ngx_uint_t    flush, n;
    size_t        size, total;
    ssize_t       sent;
    u_char        control[CMSG_SPACE(sizeof(ngx_addrinfo_t))];

    (void) limit;

    if (c->write->complete) {
        sent = ngx_sendmsg(c, NULL, 0);
        if (sent == NGX_ERROR) {
            return NGX_CHAIN_ERROR;
        }

        if (sent == NGX_AGAIN) {
            return in;
        }
    }

    if (!c->write->ready) {
        return in;
    }

    flush = 0;
    n = 0;
    iov = NULL;
    prev = NULL;
    total = 0;

    for (cl = in; cl && !flush; cl = cl->next) {
        if (cl->buf == NULL) {
            ngx_log_error(NGX_LOG_ALERT, c->log, 0,
                          "null buffer in an IOCP UDP output chain");
            c->write->error = 1;
            return NGX_CHAIN_ERROR;
        }

        if (cl->buf->flush || cl->buf->last_buf) {
            flush = 1;
        }

        if (ngx_buf_special(cl->buf)) {
            continue;
        }

        if (cl->buf->in_file) {
            ngx_log_error(NGX_LOG_ALERT, c->log, 0,
                          "file buffer in an IOCP UDP output chain");
            c->write->error = 1;
            return NGX_CHAIN_ERROR;
        }

        if (!ngx_buf_in_memory(cl->buf)) {
            ngx_log_error(NGX_LOG_ALERT, c->log, 0,
                          "invalid buffer in an IOCP UDP output chain");
            c->write->error = 1;
            return NGX_CHAIN_ERROR;
        }

        if (cl->buf->last < cl->buf->pos) {
            ngx_log_error(NGX_LOG_ALERT, c->log, 0,
                          "invalid buffer range in an IOCP UDP output chain");
            c->write->error = 1;
            return NGX_CHAIN_ERROR;
        }

        size = (size_t) (cl->buf->last - cl->buf->pos);

        if (size > NGX_MAX_UINT32_VALUE) {
            ngx_log_error(NGX_LOG_ALERT, c->log, 0,
                          "oversized buffer in an IOCP UDP output chain");
            c->write->error = 1;
            return NGX_CHAIN_ERROR;
        }

        if (size > NGX_IOCP_UDP_RECEIVE_SIZE - total) {
            ngx_log_error(NGX_LOG_ALERT, c->log, 0,
                          "oversized IOCP UDP datagram");
            c->write->error = 1;
            return NGX_CHAIN_ERROR;
        }

        total += size;

        if (prev == cl->buf->pos && iov) {
            if (size > NGX_MAX_UINT32_VALUE - iov->iov_len) {
                ngx_log_error(NGX_LOG_ALERT, c->log, 0,
                              "oversized IOCP UDP datagram");
                c->write->error = 1;
                return NGX_CHAIN_ERROR;
            }

            iov->iov_len += size;

        } else {
            if (n == NGX_IOCP_UDP_BUFS) {
                ngx_log_error(NGX_LOG_ALERT, c->log, 0,
                              "too many parts in an IOCP UDP datagram");
                c->write->error = 1;
                return NGX_CHAIN_ERROR;
            }

            iov = &iovs[n++];
            iov->iov_base = cl->buf->pos;
            iov->iov_len = size;
        }

        prev = cl->buf->last;
    }

    if (!flush) {
        return in;
    }

    if (n == 0) {
        iovs[0].iov_base = NULL;
        iovs[0].iov_len = 0;
        n = 1;
    }

    ngx_memzero(&msg, sizeof(msg));
    msg.msg_name = c->sockaddr;
    msg.msg_namelen = c->socklen;
    msg.msg_iov = iovs;
    msg.msg_iovlen = n;

    if (c->listening && c->listening->wildcard && c->local_sockaddr) {
        ngx_memzero(control, sizeof(control));
        msg.msg_control = control;
        msg.msg_controllen = sizeof(control);
        cmsg = CMSG_FIRSTHDR(&msg);
        msg.msg_controllen = ngx_set_srcaddr_cmsg(cmsg,
                                                  c->local_sockaddr);
        if (msg.msg_controllen == 0) {
            c->write->error = 1;
            ngx_log_error(NGX_LOG_ALERT, c->log, 0,
                          "cannot select UDP source address family %d",
                          c->local_sockaddr->sa_family);
            return NGX_CHAIN_ERROR;
        }
    }

    sent = ngx_sendmsg(c, &msg, 0);
    if (sent == NGX_ERROR) {
        return NGX_CHAIN_ERROR;
    }
    if (sent == NGX_AGAIN) {
        c->write->ready = 0;
        return in;
    }

    c->sent += sent;
    return ngx_iocp_udp_update_sent(in, sent);
}


static ngx_chain_t *
ngx_iocp_udp_update_sent(ngx_chain_t *in, off_t sent)
{
    off_t       size;
    ngx_buf_t  *b;

    for ( /* void */ ; in; in = in->next) {
        b = in->buf;

        if (b == NULL) {
            return in;
        }

        if (!ngx_buf_special(b)) {
            size = ngx_buf_size(b);

            if (size < 0 || sent < size) {
                if (sent > 0 && ngx_buf_in_memory(b)) {
                    b->pos += (size_t) sent;
                }

                return in;
            }

            sent -= size;

            if (ngx_buf_in_memory(b)) {
                b->pos = b->last;
            }
        }

        if (b->flush || b->last_buf) {
            return in->next;
        }
    }

    return NULL;
}
