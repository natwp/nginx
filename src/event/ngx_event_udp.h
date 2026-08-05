
/*
 * Copyright (C) Nginx, Inc.
 */


#ifndef _NGX_EVENT_UDP_H_INCLUDED_
#define _NGX_EVENT_UDP_H_INCLUDED_


#include <ngx_config.h>
#include <ngx_core.h>


#if (NGX_WIN32)

struct iovec {
    void    *iov_base;
    size_t   iov_len;
};

struct msghdr {
    void           *msg_name;
    socklen_t       msg_namelen;
    struct iovec   *msg_iov;
    size_t          msg_iovlen;
    void           *msg_control;
    size_t          msg_controllen;
    int             msg_flags;
};

#undef CMSG_FIRSTHDR
#undef CMSG_NXTHDR
#undef CMSG_DATA
#undef CMSG_SPACE
#undef CMSG_LEN

#define NGX_CMSG_ALIGN(n)        WSA_CMSGHDR_ALIGN(n)
#define CMSG_FIRSTHDR(msg)                                                    \
    ((msg)->msg_controllen >= sizeof(WSACMSGHDR)                              \
     ? (struct cmsghdr *) (msg)->msg_control : NULL)
#define CMSG_NXTHDR(msg, cmsg)                                                \
    ((u_char *) (cmsg) + NGX_CMSG_ALIGN((cmsg)->cmsg_len)                     \
       + sizeof(WSACMSGHDR)                                                   \
     > (u_char *) (msg)->msg_control + (msg)->msg_controllen                  \
     ? NULL                                                                   \
     : (struct cmsghdr *) ((u_char *) (cmsg)                                  \
                           + NGX_CMSG_ALIGN((cmsg)->cmsg_len)))
#define CMSG_DATA(cmsg)                                                       \
    ((u_char *) (cmsg) + WSA_CMSGDATA_ALIGN(sizeof(WSACMSGHDR)))
#define CMSG_SPACE(n)             WSA_CMSG_SPACE(n)
#define CMSG_LEN(n)               WSA_CMSG_LEN(n)

#undef NGX_HAVE_ADDRINFO_CMSG
#define NGX_HAVE_ADDRINFO_CMSG  1

typedef union {
    IN_PKTINFO       pkt;
#if (NGX_HAVE_INET6)
    IN6_PKTINFO      pkt6;
#endif
} ngx_addrinfo_t;

#endif


struct ngx_udp_connection_s {
    ngx_rbtree_node_t   node;
    ngx_connection_t   *connection;
    ngx_buf_t           *buffer;
    ngx_str_t            key;
};


#if !(NGX_WIN32)

#if ((NGX_HAVE_MSGHDR_MSG_CONTROL)                                            \
     && (NGX_HAVE_IP_SENDSRCADDR || NGX_HAVE_IP_RECVDSTADDR                   \
         || NGX_HAVE_IP_PKTINFO                                               \
         || (NGX_HAVE_INET6 && NGX_HAVE_IPV6_RECVPKTINFO)))
#define NGX_HAVE_ADDRINFO_CMSG  1

#endif


#if (NGX_HAVE_ADDRINFO_CMSG)

typedef union {
#if (NGX_HAVE_IP_SENDSRCADDR || NGX_HAVE_IP_RECVDSTADDR)
    struct in_addr        addr;
#endif

#if (NGX_HAVE_IP_PKTINFO)
    struct in_pktinfo     pkt;
#endif

#if (NGX_HAVE_INET6 && NGX_HAVE_IPV6_RECVPKTINFO)
    struct in6_pktinfo    pkt6;
#endif
} ngx_addrinfo_t;

size_t ngx_set_srcaddr_cmsg(struct cmsghdr *cmsg,
    struct sockaddr *local_sockaddr);
ngx_int_t ngx_get_srcaddr_cmsg(struct cmsghdr *cmsg,
    struct sockaddr *local_sockaddr);

#endif

#endif


#if (NGX_HAVE_ADDRINFO_CMSG)

#if (NGX_WIN32)
size_t ngx_set_srcaddr_cmsg(struct cmsghdr *cmsg,
    struct sockaddr *local_sockaddr);
ngx_int_t ngx_get_srcaddr_cmsg(struct cmsghdr *cmsg,
    struct sockaddr *local_sockaddr);
#endif

#endif

void ngx_event_recvmsg(ngx_event_t *ev);
ssize_t ngx_sendmsg(ngx_connection_t *c, struct msghdr *msg, int flags);
void ngx_udp_rbtree_insert_value(ngx_rbtree_node_t *temp,
    ngx_rbtree_node_t *node, ngx_rbtree_node_t *sentinel);

#if (NGX_WIN32)
ngx_int_t ngx_iocp_post_udp_receives(ngx_listening_t *ls, ngx_uint_t n);
#endif

void ngx_delete_udp_connection(void *data);


#endif /* _NGX_EVENT_UDP_H_INCLUDED_ */
