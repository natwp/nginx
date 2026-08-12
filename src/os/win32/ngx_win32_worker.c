
/*
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>
#include <ngx_win32_router.h>
#include <ngx_win32_worker.h>
#include <ntsecapi.h>
#include <sddl.h>


#ifndef PIPE_REJECT_REMOTE_CLIENTS
#define PIPE_REJECT_REMOTE_CLIENTS  0x00000008
#endif


#define NGX_WIN32_CHANNEL_UDP_BUFS     64
#define NGX_WIN32_CHANNEL_WRITE_LIMIT  (8 * 1024 * 1024)
#define NGX_WIN32_CHANNEL_WRITE_RESERVE (64 * 1024)
#define NGX_WIN32_CHANNEL_WRITE_MESSAGES 4096
#define NGX_WIN32_CHANNEL_WRITE_RESERVE_MESSAGES 64
#define NGX_WIN32_CHANNEL_READ_LIMIT   (8 * 1024 * 1024)
#define NGX_WIN32_CHANNEL_QUEUE_LIMIT  4096
#define NGX_WIN32_CHANNEL_DISPATCH_MAX 64
#define NGX_WIN32_CHANNEL_DISPATCH_SIZE (1024 * 1024)

#define NGX_WIN32_QUIC_ROUTE_TAG_OFFSET  6
#define NGX_WIN32_QUIC_ROUTE_TAG_LEN     6
#define NGX_WIN32_QUIC_ROUTE_TAIL_OFFSET 12


#define NGX_WIN32_LISTENER_STREAM  0x00000001
#define NGX_WIN32_LISTENER_QUIC    0x00000002


ngx_uint_t  ngx_win32_worker_slot;
ngx_uint_t  ngx_win32_worker_generation;
ngx_uint_t  ngx_win32_worker_role;
ngx_uint_t  ngx_win32_worker_bootstrap_active;
ngx_uint_t  ngx_win32_worker_expected_listeners;
ngx_uint_t  ngx_win32_worker_routed;


static HANDLE  ngx_win32_worker_pipe;
static HANDLE  ngx_win32_worker_read_event;
static HANDLE  ngx_win32_worker_write_event;
static HANDLE  ngx_win32_accept_mutex;
static HANDLE  ngx_win32_channel_thread;
static HANDLE  ngx_win32_channel_write_thread;
static HANDLE  ngx_win32_channel_write_ready;
static volatile LONG ngx_win32_channel_stopping;
static ngx_queue_t ngx_win32_channel_queue;
static ngx_queue_t ngx_win32_channel_write_queue;
static ngx_queue_t ngx_win32_channel_write_priority_queue;
static ngx_uint_t ngx_win32_channel_queued;
static size_t ngx_win32_channel_read_queued;
static size_t ngx_win32_channel_write_queued;
static ngx_uint_t ngx_win32_channel_write_messages;
static CRITICAL_SECTION ngx_win32_channel_lock;
static CRITICAL_SECTION ngx_win32_channel_write_lock;
static ngx_uint_t ngx_win32_channel_initialized;
static ngx_uint_t ngx_win32_channel_posted;
static uint64_t ngx_win32_quic_route_key[2];
static ngx_uint_t ngx_win32_quic_route_key_initialized;


static ngx_inline ngx_uint_t
ngx_win32_channel_is_stopping(void)
{
    return ngx_win32_channel_stopping != 0;
}


static ngx_inline void
ngx_win32_channel_set_stopping(LONG value)
{
    (void) InterlockedExchange(&ngx_win32_channel_stopping, value);
}


typedef struct {
    ngx_queue_t      queue;
    ngx_uint_t       type;
    ngx_socket_t     socket;
    ngx_uint_t       listener;
    ngx_sockaddr_t   local;
    ngx_sockaddr_t   remote;
    socklen_t        local_socklen;
    socklen_t        remote_socklen;
    size_t           size;
    u_char          *data;
} ngx_win32_channel_accept_node_t;


typedef struct {
    ngx_queue_t  queue;
    size_t       size;
    u_char      *data;
} ngx_win32_channel_write_node_t;


static ngx_int_t ngx_win32_bootstrap_read(HANDLE pipe, void *data,
    size_t size, ngx_log_t *log);
static ngx_int_t ngx_win32_bootstrap_write(HANDLE pipe, const void *data,
    size_t size, ngx_log_t *log);
static ngx_int_t ngx_win32_worker_pipe_io(HANDLE pipe, void *data,
    size_t size, ngx_uint_t write, ngx_log_t *log);
static ngx_int_t ngx_win32_master_pipe_io(HANDLE pipe, void *data,
    size_t size, ngx_uint_t write, ngx_log_t *log);
static ngx_int_t ngx_win32_bootstrap_read_header(HANDLE pipe,
    ngx_win32_bootstrap_header_t *header, ngx_log_t *log);
static ngx_int_t ngx_win32_bootstrap_get_uint(char *name, ngx_uint_t *value,
    ngx_log_t *log);
static ngx_int_t ngx_win32_master_connect(ngx_cycle_t *cycle,
    ngx_win32_worker_bootstrap_t *bootstrap, HANDLE process);
static ngx_int_t ngx_win32_master_read_header(ngx_cycle_t *cycle,
    ngx_win32_worker_bootstrap_t *bootstrap,
    ngx_win32_bootstrap_header_t *header, HANDLE process);
static ngx_thread_value_t __stdcall ngx_win32_worker_channel_thread(void *data);
static ngx_thread_value_t __stdcall ngx_win32_worker_channel_write_thread(
    void *data);
static ngx_int_t ngx_win32_worker_channel_write(const void *data, size_t size,
    ngx_uint_t priority, ngx_log_t *log);
static ngx_win32_channel_write_node_t *ngx_win32_worker_channel_alloc_write(
    size_t size, ngx_log_t *log);
static ngx_int_t ngx_win32_worker_channel_queue_write(
    ngx_win32_channel_write_node_t *node, ngx_uint_t priority,
    ngx_log_t *log);
static ngx_int_t ngx_win32_worker_channel_ack(uint64_t id,
    ngx_uint_t status);
static ngx_listening_t *ngx_win32_worker_find_listener(ngx_cycle_t *cycle,
    ngx_uint_t index, int type, struct sockaddr *local,
    socklen_t local_socklen);
static ngx_uint_t ngx_win32_worker_listener_matches(ngx_listening_t *ls,
    int type, struct sockaddr *local, socklen_t local_socklen);
static void ngx_win32_worker_channel_handler(ngx_event_t *ev);
static uint64_t ngx_win32_quic_route_tag(u_char *id, size_t len);


ngx_int_t
ngx_win32_worker_bootstrap_init(ngx_log_t *log)
{
    DWORD  mode, n;
    char   name[NGX_PROCESS_SYNC_NAME];

    n = GetEnvironmentVariable(NGX_WIN32_WORKER_PIPE_ENV, name,
                               NGX_PROCESS_SYNC_NAME);

    if (n == 0) {
        if (ngx_errno == ERROR_ENVVAR_NOT_FOUND) {
            return NGX_DECLINED;
        }

        ngx_log_error(NGX_LOG_EMERG, log, ngx_errno,
                      "GetEnvironmentVariable(\"%s\") failed",
                      NGX_WIN32_WORKER_PIPE_ENV);
        return NGX_ERROR;
    }

    if (n >= NGX_PROCESS_SYNC_NAME) {
        ngx_log_error(NGX_LOG_EMERG, log, 0,
                      "the worker bootstrap pipe name is too long");
        return NGX_ERROR;
    }

    if (ngx_win32_bootstrap_get_uint(NGX_WIN32_WORKER_SLOT_ENV,
                                     &ngx_win32_worker_slot, log)
        != NGX_OK
        || ngx_win32_bootstrap_get_uint(NGX_WIN32_WORKER_GENERATION_ENV,
                                        &ngx_win32_worker_generation, log)
           != NGX_OK
        || ngx_win32_bootstrap_get_uint(NGX_WIN32_WORKER_ROLE_ENV,
                                        &ngx_win32_worker_role, log)
           != NGX_OK)
    {
        return NGX_ERROR;
    }

    for ( ;; ) {
        ngx_win32_worker_pipe = CreateFile(name,
                                           GENERIC_READ | GENERIC_WRITE,
                                           0, NULL, OPEN_EXISTING,
                                           FILE_FLAG_OVERLAPPED, NULL);

        if (ngx_win32_worker_pipe != INVALID_HANDLE_VALUE) {
            break;
        }

        if (ngx_errno != ERROR_PIPE_BUSY) {
            ngx_log_error(NGX_LOG_EMERG, log, ngx_errno,
                          "CreateFile(\"%s\") failed", name);
            return NGX_ERROR;
        }

        if (WaitNamedPipe(name, NGX_WIN32_BOOTSTRAP_TIMEOUT) == 0) {
            ngx_log_error(NGX_LOG_EMERG, log, ngx_errno,
                          "WaitNamedPipe(\"%s\") failed", name);
            return NGX_ERROR;
        }
    }

    ngx_win32_worker_read_event = CreateEvent(NULL, 1, 0, NULL);
    if (ngx_win32_worker_read_event == NULL) {
        ngx_log_error(NGX_LOG_EMERG, log, ngx_errno,
                      "CreateEvent(worker pipe read) failed");
        ngx_close_handle(ngx_win32_worker_pipe);
        ngx_win32_worker_pipe = NULL;
        return NGX_ERROR;
    }

    ngx_win32_worker_write_event = CreateEvent(NULL, 1, 0, NULL);
    if (ngx_win32_worker_write_event == NULL) {
        ngx_log_error(NGX_LOG_EMERG, log, ngx_errno,
                      "CreateEvent(worker pipe write) failed");
        ngx_close_handle(ngx_win32_worker_read_event);
        ngx_win32_worker_read_event = NULL;
        ngx_close_handle(ngx_win32_worker_pipe);
        ngx_win32_worker_pipe = NULL;
        return NGX_ERROR;
    }

    mode = PIPE_READMODE_BYTE | PIPE_WAIT;

    if (SetNamedPipeHandleState(ngx_win32_worker_pipe, &mode, NULL, NULL)
        == 0)
    {
        ngx_log_error(NGX_LOG_EMERG, log, ngx_errno,
                      "SetNamedPipeHandleState(\"%s\") failed", name);
        ngx_close_handle(ngx_win32_worker_write_event);
        ngx_win32_worker_write_event = NULL;
        ngx_close_handle(ngx_win32_worker_read_event);
        ngx_win32_worker_read_event = NULL;
        ngx_close_handle(ngx_win32_worker_pipe);
        ngx_win32_worker_pipe = NULL;
        return NGX_ERROR;
    }

    ngx_win32_worker_bootstrap_active = 1;

    if (ngx_win32_worker_send_status(NGX_WIN32_BOOTSTRAP_HELLO, 0)
        != NGX_OK)
    {
        return NGX_ERROR;
    }

    return NGX_OK;
}


ngx_int_t
ngx_win32_worker_import_listeners(ngx_cycle_t *cycle)
{
    ngx_uint_t                         i;
    ngx_socket_t                       s;
    ngx_listening_t                   *ls;
    ngx_win32_bootstrap_header_t       header;
    ngx_win32_bootstrap_listener_t     listener;
    ngx_win32_bootstrap_listeners_t    listeners;

    if (!ngx_win32_worker_bootstrap_active) {
        return NGX_DECLINED;
    }

    if (ngx_win32_bootstrap_read_header(ngx_win32_worker_pipe, &header,
                                        cycle->log)
        != NGX_OK)
    {
        return NGX_ERROR;
    }

    if (header.type != NGX_WIN32_BOOTSTRAP_LISTENERS
        || header.slot != ngx_win32_worker_slot
        || header.generation != ngx_win32_worker_generation
        || header.length < sizeof(ngx_win32_bootstrap_listeners_t))
    {
        ngx_log_error(NGX_LOG_EMERG, cycle->log, 0,
                      "invalid worker listener bootstrap header");
        (void) ngx_win32_worker_send_status(NGX_WIN32_BOOTSTRAP_FAIL,
                                             ERROR_INVALID_DATA);
        return NGX_ERROR;
    }

    if (ngx_win32_bootstrap_read(ngx_win32_worker_pipe, &listeners,
                                 sizeof(ngx_win32_bootstrap_listeners_t),
                                 cycle->log)
        != NGX_OK)
    {
        return NGX_ERROR;
    }

    if (header.length
        != sizeof(ngx_win32_bootstrap_listeners_t)
           + listeners.count * sizeof(ngx_win32_bootstrap_listener_t))
    {
        ngx_log_error(NGX_LOG_EMERG, cycle->log, 0,
                      "invalid worker listener bootstrap length");
        (void) ngx_win32_worker_send_status(NGX_WIN32_BOOTSTRAP_FAIL,
                                             ERROR_INVALID_DATA);
        return NGX_ERROR;
    }

    ngx_win32_worker_routed =
        (listeners.flags & NGX_WIN32_LISTENERS_ROUTED) ? 1 : 0;

    if (ngx_win32_worker_routed) {
        ngx_memcpy(ngx_win32_quic_route_key, listeners.route_key,
                   sizeof(ngx_win32_quic_route_key));
        ngx_win32_quic_route_key_initialized = 1;
    }

    if (ngx_array_init(&cycle->listening, cycle->pool,
                       listeners.count ? listeners.count : 1,
                       sizeof(ngx_listening_t))
        != NGX_OK)
    {
        (void) ngx_win32_worker_send_status(NGX_WIN32_BOOTSTRAP_FAIL,
                                             ERROR_NOT_ENOUGH_MEMORY);
        return NGX_ERROR;
    }

    for (i = 0; i < listeners.count; i++) {
        if (ngx_win32_bootstrap_read(ngx_win32_worker_pipe, &listener,
                                     sizeof(ngx_win32_bootstrap_listener_t),
                                     cycle->log)
            != NGX_OK)
        {
            goto failed;
        }

        if (listener.index != i) {
            ngx_log_error(NGX_LOG_EMERG, cycle->log, 0,
                          "out-of-order worker listener bootstrap record");
            goto failed;
        }

        s = WSASocket(FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO,
                      FROM_PROTOCOL_INFO, &listener.info, 0,
                      WSA_FLAG_OVERLAPPED);

        if (s == (ngx_socket_t) -1) {
            ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_socket_errno,
                          "WSASocket(FROM_PROTOCOL_INFO) failed");
            goto failed;
        }

        ls = ngx_array_push(&cycle->listening);
        if (ls == NULL) {
            ngx_close_socket(s);
            goto failed;
        }

        ngx_memzero(ls, sizeof(ngx_listening_t));

        ls->fd = s;
        ls->inherited = 1;
        ls->shared = 1;
        ls->worker = ngx_win32_worker_slot;
        ls->quic = (listener.flags & NGX_WIN32_LISTENER_QUIC) ? 1 : 0;
    }

    ngx_win32_worker_expected_listeners = listeners.count;
    ngx_inherited = 1;

    if (ngx_set_inherited_sockets(cycle) != NGX_OK) {
        goto failed;
    }

    if (ngx_win32_worker_send_status(NGX_WIN32_BOOTSTRAP_IMPORT_OK, 0)
        != NGX_OK)
    {
        goto failed;
    }

    return NGX_OK;

failed:

    ls = cycle->listening.elts;

    for (i = 0; i < cycle->listening.nelts; i++) {
        if (ls[i].fd != (ngx_socket_t) -1) {
            ngx_close_socket(ls[i].fd);
            ls[i].fd = (ngx_socket_t) -1;
        }
    }

    (void) ngx_win32_worker_send_status(NGX_WIN32_BOOTSTRAP_FAIL,
                                         ngx_socket_errno);

    return NGX_ERROR;
}


ngx_int_t
ngx_win32_worker_validate_listeners(ngx_cycle_t *cycle)
{
    ngx_uint_t        i, n;
    ngx_listening_t  *ls;

    if (!ngx_win32_worker_bootstrap_active) {
        return NGX_OK;
    }

    n = 0;
    ls = cycle->listening.elts;

    for (i = 0; i < cycle->listening.nelts; i++) {
        if (ls[i].ignore) {
            continue;
        }

        if (ls[i].fd == (ngx_socket_t) -1 || !ls[i].inherited) {
            ngx_log_error(NGX_LOG_EMERG, cycle->log, 0,
                          "worker %ui generation %ui has no imported "
                          "listener for %V", ngx_win32_worker_slot,
                          ngx_win32_worker_generation, &ls[i].addr_text);
            return NGX_ERROR;
        }

        n++;
    }

    if (n != ngx_win32_worker_expected_listeners) {
        ngx_log_error(NGX_LOG_EMERG, cycle->log, 0,
                      "worker %ui generation %ui imported %ui listeners "
                      "but configuration requires %ui",
                      ngx_win32_worker_slot,
                      ngx_win32_worker_generation,
                      ngx_win32_worker_expected_listeners, n);
        return NGX_ERROR;
    }

    return NGX_OK;
}


ngx_int_t
ngx_win32_worker_send_status(ngx_uint_t type, ngx_uint_t status)
{
    ngx_win32_bootstrap_header_t  header;

    if (ngx_win32_worker_pipe == NULL) {
        return NGX_DECLINED;
    }

    ngx_memzero(&header, sizeof(ngx_win32_bootstrap_header_t));

    header.magic = NGX_WIN32_BOOTSTRAP_MAGIC;
    header.version = NGX_WIN32_BOOTSTRAP_VERSION;
    header.type = (uint32_t) type;
    header.slot = (uint32_t) ngx_win32_worker_slot;
    header.generation = (uint32_t) ngx_win32_worker_generation;
    header.pid = (uint32_t) ngx_pid;
    header.status = (uint32_t) status;

    return ngx_win32_bootstrap_write(ngx_win32_worker_pipe, &header,
                                     sizeof(ngx_win32_bootstrap_header_t),
                                     ngx_cycle->log);
}


ngx_int_t
ngx_win32_worker_channel_init(ngx_cycle_t *cycle)
{
    ngx_tid_t  rtid, wtid;

    if (!ngx_win32_worker_routed) {
        return NGX_OK;
    }

    if (!(ngx_event_flags & NGX_USE_IOCP_EVENT)) {
        ngx_log_error(NGX_LOG_EMERG, cycle->log, 0,
                      "the routed Win32 worker channel requires IOCP");
        return NGX_ERROR;
    }

    ngx_queue_init(&ngx_win32_channel_queue);
    ngx_queue_init(&ngx_win32_channel_write_queue);
    ngx_queue_init(&ngx_win32_channel_write_priority_queue);
    InitializeCriticalSection(&ngx_win32_channel_lock);
    InitializeCriticalSection(&ngx_win32_channel_write_lock);
    ngx_win32_channel_initialized = 1;
    ngx_win32_channel_set_stopping(0);
    ngx_win32_channel_posted = 0;
    ngx_win32_channel_queued = 0;
    ngx_win32_channel_read_queued = 0;
    ngx_win32_channel_write_queued = 0;
    ngx_win32_channel_write_messages = 0;

    ngx_win32_channel_write_ready = CreateEvent(NULL, 1, 0, NULL);
    if (ngx_win32_channel_write_ready == NULL) {
        ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_errno,
                      "CreateEvent(worker channel write) failed");
        goto failed;
    }

    if (ngx_create_thread(&wtid, ngx_win32_worker_channel_write_thread,
                          cycle, cycle->log)
        != 0)
    {
        goto failed;
    }

    ngx_win32_channel_write_thread = wtid;

    if (ngx_create_thread(&rtid, ngx_win32_worker_channel_thread, cycle,
                          cycle->log)
        != 0)
    {
        goto failed;
    }

    ngx_win32_channel_thread = rtid;

    return NGX_OK;

failed:

    ngx_win32_channel_set_stopping(1);

    if (ngx_win32_worker_pipe) {
        (void) CancelIoEx(ngx_win32_worker_pipe, NULL);
    }

    if (ngx_win32_channel_write_ready) {
        (void) SetEvent(ngx_win32_channel_write_ready);
    }

    if (ngx_win32_channel_write_thread) {
        if (WaitForSingleObject(ngx_win32_channel_write_thread,
                                NGX_WIN32_BOOTSTRAP_TIMEOUT)
            != WAIT_OBJECT_0)
        {
            ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                          "could not stop the Win32 worker channel writer");
            exit(2);
        }

        ngx_close_handle(ngx_win32_channel_write_thread);
        ngx_win32_channel_write_thread = NULL;
    }

    if (ngx_win32_channel_write_ready) {
        ngx_close_handle(ngx_win32_channel_write_ready);
        ngx_win32_channel_write_ready = NULL;
    }

    DeleteCriticalSection(&ngx_win32_channel_write_lock);
    DeleteCriticalSection(&ngx_win32_channel_lock);
    ngx_win32_channel_initialized = 0;

    return NGX_ERROR;
}


void
ngx_win32_worker_channel_done(void)
{
    size_t                           read_bytes, write_bytes;
    u_long                           wait;
    ngx_uint_t                       read_messages, write_messages;
    ngx_queue_t                     *q;
    ngx_win32_channel_accept_node_t *node;
    ngx_win32_channel_write_node_t  *write;

    if (!ngx_win32_channel_initialized) {
        return;
    }

    ngx_win32_channel_set_stopping(1);

    if (ngx_win32_channel_write_ready) {
        (void) SetEvent(ngx_win32_channel_write_ready);
    }

    if (ngx_win32_channel_thread) {
        if (ngx_win32_worker_pipe) {
            (void) CancelIoEx(ngx_win32_worker_pipe, NULL);
        }

        wait = WaitForSingleObject(ngx_win32_channel_thread,
                                   NGX_WIN32_BOOTSTRAP_TIMEOUT);
        if (wait != WAIT_OBJECT_0) {
            ngx_log_error(NGX_LOG_ALERT, ngx_cycle->log,
                          wait == WAIT_FAILED ? ngx_errno : 0,
                          "could not stop the Win32 worker channel reader");
            exit(2);
        }

        ngx_close_handle(ngx_win32_channel_thread);
        ngx_win32_channel_thread = NULL;
    }

    if (ngx_win32_channel_write_thread) {
        wait = WaitForSingleObject(ngx_win32_channel_write_thread,
                                   NGX_WIN32_BOOTSTRAP_TIMEOUT);
        if (wait != WAIT_OBJECT_0) {
            ngx_log_error(NGX_LOG_ALERT, ngx_cycle->log,
                          wait == WAIT_FAILED ? ngx_errno : 0,
                          "could not stop the Win32 worker channel writer");
            exit(2);
        }

        ngx_close_handle(ngx_win32_channel_write_thread);
        ngx_win32_channel_write_thread = NULL;
    }

    EnterCriticalSection(&ngx_win32_channel_lock);

    read_bytes = 0;
    read_messages = 0;

    while (!ngx_queue_empty(&ngx_win32_channel_queue)) {
        q = ngx_queue_head(&ngx_win32_channel_queue);
        ngx_queue_remove(q);
        node = ngx_queue_data(q, ngx_win32_channel_accept_node_t, queue);

        if (node->socket != (ngx_socket_t) -1) {
            (void) ngx_close_socket(node->socket);
        }

        read_messages++;
        read_bytes += sizeof(ngx_win32_channel_accept_node_t) + node->size;
        ngx_free(node);
    }

    if (read_messages != ngx_win32_channel_queued
        || read_bytes != ngx_win32_channel_read_queued)
    {
        ngx_log_error(NGX_LOG_ALERT, ngx_cycle->log, 0,
                      "Win32 worker channel read queue counters differ "
                      "at shutdown (tracked:%ui/%uz, actual:%ui/%uz)",
                      ngx_win32_channel_queued,
                      ngx_win32_channel_read_queued,
                      read_messages, read_bytes);
    }

    ngx_win32_channel_queued = 0;
    ngx_win32_channel_read_queued = 0;

    LeaveCriticalSection(&ngx_win32_channel_lock);

    EnterCriticalSection(&ngx_win32_channel_write_lock);

    write_bytes = 0;
    write_messages = 0;

    while (!ngx_queue_empty(&ngx_win32_channel_write_priority_queue)
           || !ngx_queue_empty(&ngx_win32_channel_write_queue))
    {
        q = !ngx_queue_empty(&ngx_win32_channel_write_priority_queue)
            ? ngx_queue_head(&ngx_win32_channel_write_priority_queue)
            : ngx_queue_head(&ngx_win32_channel_write_queue);
        ngx_queue_remove(q);
        write = ngx_queue_data(q, ngx_win32_channel_write_node_t, queue);
        write_messages++;
        write_bytes += write->size;
        ngx_free(write);
    }

    if (write_messages != ngx_win32_channel_write_messages
        || write_bytes != ngx_win32_channel_write_queued)
    {
        ngx_log_error(NGX_LOG_ALERT, ngx_cycle->log, 0,
                      "Win32 worker channel write queue counters differ "
                      "at shutdown (tracked:%ui/%uz, actual:%ui/%uz)",
                      ngx_win32_channel_write_messages,
                      ngx_win32_channel_write_queued,
                      write_messages, write_bytes);
    }

    ngx_win32_channel_write_queued = 0;
    ngx_win32_channel_write_messages = 0;

    LeaveCriticalSection(&ngx_win32_channel_write_lock);

    DeleteCriticalSection(&ngx_win32_channel_write_lock);
    DeleteCriticalSection(&ngx_win32_channel_lock);
    ngx_win32_channel_initialized = 0;

    if (ngx_win32_channel_write_ready) {
        ngx_close_handle(ngx_win32_channel_write_ready);
        ngx_win32_channel_write_ready = NULL;
    }

    if (ngx_win32_worker_write_event) {
        ngx_close_handle(ngx_win32_worker_write_event);
        ngx_win32_worker_write_event = NULL;
    }

    if (ngx_win32_worker_read_event) {
        ngx_close_handle(ngx_win32_worker_read_event);
        ngx_win32_worker_read_event = NULL;
    }

    if (ngx_win32_worker_pipe) {
        ngx_close_handle(ngx_win32_worker_pipe);
        ngx_win32_worker_pipe = NULL;
    }
}


static ngx_thread_value_t __stdcall
ngx_win32_worker_channel_write_thread(void *data)
{
    u_long                           wait;
    ngx_queue_t                     *q;
    ngx_cycle_t                     *cycle;
    ngx_win32_channel_write_node_t  *node;

    cycle = data;

    for ( ;; ) {
        wait = WaitForSingleObject(ngx_win32_channel_write_ready, INFINITE);

        if (wait != WAIT_OBJECT_0) {
            ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                          "WaitForSingleObject(worker channel write) failed");

            if (!ngx_win32_channel_is_stopping()) {
                exit(2);
            }

            return 0;
        }

        for ( ;; ) {
            EnterCriticalSection(&ngx_win32_channel_write_lock);

            if (ngx_win32_channel_is_stopping()) {
                LeaveCriticalSection(&ngx_win32_channel_write_lock);
                return 0;
            }

            if (ngx_queue_empty(&ngx_win32_channel_write_priority_queue)
                && ngx_queue_empty(&ngx_win32_channel_write_queue))
            {
                (void) ResetEvent(ngx_win32_channel_write_ready);
                LeaveCriticalSection(&ngx_win32_channel_write_lock);
                break;
            }

            q = !ngx_queue_empty(&ngx_win32_channel_write_priority_queue)
                ? ngx_queue_head(&ngx_win32_channel_write_priority_queue)
                : ngx_queue_head(&ngx_win32_channel_write_queue);
            ngx_queue_remove(q);
            node = ngx_queue_data(q, ngx_win32_channel_write_node_t, queue);
            ngx_win32_channel_write_queued -= node->size;
            ngx_win32_channel_write_messages--;

            LeaveCriticalSection(&ngx_win32_channel_write_lock);

            if (ngx_win32_bootstrap_write(ngx_win32_worker_pipe, node->data,
                                          node->size, cycle->log)
                != NGX_OK)
            {
                ngx_free(node);

                if (!ngx_win32_channel_is_stopping()) {
                    ngx_log_error(NGX_LOG_ALERT, cycle->log, 0,
                                  "Win32 worker channel writer stopped "
                                  "unexpectedly for slot %ui generation %ui",
                                  ngx_win32_worker_slot,
                                  ngx_win32_worker_generation);
                    exit(2);
                }

                return 0;
            }

            ngx_free(node);
        }
    }
}


static ngx_int_t
ngx_win32_worker_channel_write(const void *data, size_t size,
    ngx_uint_t priority, ngx_log_t *log)
{
    ngx_win32_channel_write_node_t *node;

    if (!ngx_win32_channel_initialized || data == NULL || size == 0) {
        return NGX_ERROR;
    }

    node = ngx_win32_worker_channel_alloc_write(size, log);
    if (node == NULL) {
        return NGX_ERROR;
    }

    ngx_memcpy(node->data, data, size);

    return ngx_win32_worker_channel_queue_write(node, priority, log);
}


static ngx_win32_channel_write_node_t *
ngx_win32_worker_channel_alloc_write(size_t size, ngx_log_t *log)
{
    ngx_win32_channel_write_node_t *node;

    node = ngx_alloc(sizeof(ngx_win32_channel_write_node_t) + size, log);
    if (node == NULL) {
        return NULL;
    }

    ngx_memzero(node, sizeof(ngx_win32_channel_write_node_t));
    node->size = size;
    node->data = (u_char *) (node + 1);

    return node;
}


static ngx_int_t
ngx_win32_worker_channel_queue_write(ngx_win32_channel_write_node_t *node,
    ngx_uint_t priority, ngx_log_t *log)
{
    ngx_uint_t  full;

    EnterCriticalSection(&ngx_win32_channel_write_lock);

    if (ngx_win32_channel_is_stopping()) {
        LeaveCriticalSection(&ngx_win32_channel_write_lock);
        ngx_free(node);

        return NGX_ERROR;
    }

    if ((!priority && node->size > NGX_WIN32_CHANNEL_WRITE_LIMIT)
        || (priority
            && node->size > NGX_WIN32_CHANNEL_WRITE_LIMIT
                      + NGX_WIN32_CHANNEL_WRITE_RESERVE))
    {
        LeaveCriticalSection(&ngx_win32_channel_write_lock);
        ngx_free(node);
        return NGX_ERROR;
    }

    full = ngx_win32_channel_write_messages
           >= NGX_WIN32_CHANNEL_WRITE_MESSAGES
              + (priority
                 ? NGX_WIN32_CHANNEL_WRITE_RESERVE_MESSAGES : 0)
           || ngx_win32_channel_write_queued
              > NGX_WIN32_CHANNEL_WRITE_LIMIT
                + (priority ? NGX_WIN32_CHANNEL_WRITE_RESERVE : 0)
                - node->size;

    if (full) {
        LeaveCriticalSection(&ngx_win32_channel_write_lock);
        ngx_free(node);

        ngx_log_error(NGX_LOG_WARN, log, 0,
                      "Win32 worker channel write queue is full");

        return priority ? NGX_ERROR : NGX_AGAIN;
    }

    if (priority) {
        ngx_queue_insert_tail(&ngx_win32_channel_write_priority_queue,
                              &node->queue);

    } else {
        ngx_queue_insert_tail(&ngx_win32_channel_write_queue, &node->queue);
    }

    ngx_win32_channel_write_queued += node->size;
    ngx_win32_channel_write_messages++;

    if (SetEvent(ngx_win32_channel_write_ready) == 0) {
        ngx_queue_remove(&node->queue);
        ngx_win32_channel_write_queued -= node->size;
        ngx_win32_channel_write_messages--;
        LeaveCriticalSection(&ngx_win32_channel_write_lock);
        ngx_log_error(NGX_LOG_ALERT, log, ngx_errno,
                      "SetEvent(worker channel write) failed");
        ngx_free(node);
        return NGX_ERROR;
    }

    LeaveCriticalSection(&ngx_win32_channel_write_lock);

    return NGX_OK;
}


static ngx_thread_value_t __stdcall
ngx_win32_worker_channel_thread(void *data)
{
    size_t                           queued_size;
    ngx_int_t                        notify;
    ngx_socket_t                     s;
    ngx_cycle_t                     *cycle;
    ngx_win32_channel_accept_t       accept;
    ngx_win32_channel_header_t       header;
    ngx_win32_channel_udp_t          udp;
    ngx_win32_channel_accept_node_t *node;

    cycle = data;

    for ( ;; ) {
        if (ngx_win32_bootstrap_read(ngx_win32_worker_pipe, &header,
                                     sizeof(ngx_win32_channel_header_t),
                                     cycle->log)
            != NGX_OK)
        {
            if (!ngx_win32_channel_is_stopping()) {
                ngx_log_error(NGX_LOG_ALERT, cycle->log, 0,
                              "Win32 worker channel stopped unexpectedly for "
                              "slot %ui generation %ui",
                              ngx_win32_worker_slot,
                              ngx_win32_worker_generation);
                exit(2);
            }

            return 0;
        }

        if (header.magic != NGX_WIN32_CHANNEL_MAGIC
            || header.version != NGX_WIN32_CHANNEL_VERSION
            || header.slot != ngx_win32_worker_slot
            || header.generation != ngx_win32_worker_generation)
        {
            ngx_log_error(NGX_LOG_ALERT, cycle->log, 0,
                          "invalid Win32 worker channel message");
            goto failed;
        }

        if (header.type == NGX_WIN32_CHANNEL_UDP_RECV) {
            if (header.length < sizeof(ngx_win32_channel_udp_t)
                || header.length > sizeof(ngx_win32_channel_udp_t)
                                    + NGX_WIN32_CHANNEL_UDP_MAX)
            {
                ngx_log_error(NGX_LOG_ALERT, cycle->log, 0,
                              "invalid Win32 UDP channel message length");
                goto failed;
            }

            ngx_memzero(&udp, sizeof(ngx_win32_channel_udp_t));
            udp.header = header;

            if (ngx_win32_bootstrap_read(ngx_win32_worker_pipe,
                         (u_char *) &udp
                         + sizeof(ngx_win32_channel_header_t),
                         sizeof(ngx_win32_channel_udp_t)
                         - sizeof(ngx_win32_channel_header_t), cycle->log)
                != NGX_OK)
            {
                goto failed;
            }

            if (udp.local_socklen > sizeof(ngx_sockaddr_t)
                || udp.remote_socklen > sizeof(ngx_sockaddr_t)
                || udp.data_len > NGX_WIN32_CHANNEL_UDP_MAX
                || header.length != sizeof(ngx_win32_channel_udp_t)
                                    + udp.data_len)
            {
                ngx_log_error(NGX_LOG_ALERT, cycle->log, 0,
                              "invalid Win32 UDP channel message");
                goto failed;
            }

            node = ngx_alloc(sizeof(ngx_win32_channel_accept_node_t)
                             + udp.data_len, cycle->log);
            if (node == NULL) {
                goto failed;
            }

            ngx_memzero(node, sizeof(ngx_win32_channel_accept_node_t));
            node->type = NGX_WIN32_CHANNEL_UDP_RECV;
            node->socket = (ngx_socket_t) -1;
            node->listener = header.listener;
            node->local_socklen = (socklen_t) udp.local_socklen;
            node->remote_socklen = (socklen_t) udp.remote_socklen;
            node->size = udp.data_len;
            node->data = (u_char *) (node + 1);
            ngx_memcpy(&node->local, &udp.local, udp.local_socklen);
            ngx_memcpy(&node->remote, &udp.remote, udp.remote_socklen);

            if (udp.data_len
                && ngx_win32_bootstrap_read(ngx_win32_worker_pipe,
                                            node->data, udp.data_len,
                                            cycle->log)
                   != NGX_OK)
            {
                ngx_free(node);
                goto failed;
            }

            queued_size = sizeof(ngx_win32_channel_accept_node_t)
                          + udp.data_len;
            goto queue;
        }

        if (header.type != NGX_WIN32_CHANNEL_ACCEPT
            || header.length != sizeof(ngx_win32_channel_accept_t))
        {
            ngx_log_error(NGX_LOG_ALERT, cycle->log, 0,
                          "unsupported Win32 worker channel message");
            goto failed;
        }

        ngx_memzero(&accept, sizeof(ngx_win32_channel_accept_t));
        accept.header = header;

        if (ngx_win32_bootstrap_read(ngx_win32_worker_pipe,
                         (u_char *) &accept
                         + sizeof(ngx_win32_channel_header_t),
                         sizeof(ngx_win32_channel_accept_t)
                         - sizeof(ngx_win32_channel_header_t), cycle->log)
            != NGX_OK)
        {
            goto failed;
        }

        if (accept.local_socklen > sizeof(ngx_sockaddr_t)
            || accept.remote_socklen > sizeof(ngx_sockaddr_t)
            || ngx_win32_exiting_requested()
            || ngx_win32_quit_requested()
            || ngx_win32_terminate_requested())
        {
            (void) ngx_win32_worker_channel_ack(header.id,
                         ngx_win32_exiting_requested()
                         || ngx_win32_quit_requested()
                         || ngx_win32_terminate_requested()
                         ? ERROR_OPERATION_ABORTED : ERROR_INVALID_DATA);
            continue;
        }

        s = WSASocket(FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO,
                      FROM_PROTOCOL_INFO, &accept.info, 0,
                      WSA_FLAG_OVERLAPPED);

        if (s == (ngx_socket_t) -1) {
            (void) ngx_win32_worker_channel_ack(header.id,
                                                ngx_socket_errno);
            continue;
        }

        node = ngx_alloc(sizeof(ngx_win32_channel_accept_node_t), cycle->log);
        if (node == NULL) {
            (void) ngx_close_socket(s);
            (void) ngx_win32_worker_channel_ack(header.id,
                                                ERROR_NOT_ENOUGH_MEMORY);
            continue;
        }

        ngx_memzero(node, sizeof(ngx_win32_channel_accept_node_t));
        node->type = NGX_WIN32_CHANNEL_ACCEPT;
        node->socket = s;
        node->listener = header.listener;
        node->local_socklen = (socklen_t) accept.local_socklen;
        node->remote_socklen = (socklen_t) accept.remote_socklen;
        ngx_memcpy(&node->local, &accept.local, accept.local_socklen);
        ngx_memcpy(&node->remote, &accept.remote, accept.remote_socklen);
        queued_size = sizeof(ngx_win32_channel_accept_node_t);

    queue:

        notify = 0;

        EnterCriticalSection(&ngx_win32_channel_lock);

        if (ngx_win32_channel_is_stopping()
            || ngx_win32_channel_queued >= NGX_WIN32_CHANNEL_QUEUE_LIMIT
            || queued_size > NGX_WIN32_CHANNEL_READ_LIMIT
            || ngx_win32_channel_read_queued
               > NGX_WIN32_CHANNEL_READ_LIMIT - queued_size)
        {
            LeaveCriticalSection(&ngx_win32_channel_lock);

            if (node->socket != (ngx_socket_t) -1) {
                (void) ngx_win32_worker_channel_ack(header.id,
                                                    ERROR_NOT_ENOUGH_QUOTA);
                (void) ngx_close_socket(node->socket);
            }

            ngx_free(node);

            if (!ngx_win32_channel_is_stopping()) {
                ngx_log_error(NGX_LOG_WARN, cycle->log, 0,
                              "Win32 worker channel read queue is full");
            }

            continue;
        }

        if (node->socket != (ngx_socket_t) -1
            && ngx_win32_worker_channel_ack(header.id, 0) != NGX_OK)
        {
            LeaveCriticalSection(&ngx_win32_channel_lock);
            (void) ngx_close_socket(node->socket);
            ngx_free(node);
            goto failed;
        }

        ngx_queue_insert_tail(&ngx_win32_channel_queue, &node->queue);
        ngx_win32_channel_queued++;
        ngx_win32_channel_read_queued += queued_size;

        if (!ngx_win32_channel_posted) {
            ngx_win32_channel_posted = 1;
            notify = 1;
        }

        LeaveCriticalSection(&ngx_win32_channel_lock);

        if (notify && ngx_notify(ngx_win32_worker_channel_handler) != NGX_OK) {
            ngx_log_error(NGX_LOG_ALERT, cycle->log, 0,
                          "could not notify the Win32 worker channel");
            goto failed;
        }
    }

failed:

    if (!ngx_win32_channel_is_stopping()) {
        exit(2);
    }

    return 0;
}


static ngx_int_t
ngx_win32_worker_channel_ack(uint64_t id, ngx_uint_t status)
{
    ngx_int_t                   rc;
    ngx_win32_channel_header_t  header;

    ngx_memzero(&header, sizeof(ngx_win32_channel_header_t));
    header.magic = NGX_WIN32_CHANNEL_MAGIC;
    header.version = NGX_WIN32_CHANNEL_VERSION;
    header.type = NGX_WIN32_CHANNEL_ACCEPT_ACK;
    header.length = sizeof(ngx_win32_channel_header_t);
    header.slot = (uint32_t) ngx_win32_worker_slot;
    header.generation = (uint32_t) ngx_win32_worker_generation;
    header.status = (uint32_t) status;
    header.id = id;

    rc = ngx_win32_worker_channel_write(&header,
                                        sizeof(ngx_win32_channel_header_t),
                                        1, ngx_cycle->log);

    return rc;
}


static ngx_listening_t *
ngx_win32_worker_find_listener(ngx_cycle_t *cycle, ngx_uint_t index, int type,
    struct sockaddr *local, socklen_t local_socklen)
{
    ngx_uint_t        i, n;
    ngx_listening_t  *ls;

    if (local == NULL
        || local_socklen < (socklen_t) sizeof(local->sa_family)
        || local_socklen > (socklen_t) sizeof(ngx_sockaddr_t))
    {
        return NULL;
    }

    ls = cycle->listening.elts;
    n = ngx_win32_worker_routed ? ngx_win32_worker_expected_listeners
                                : cycle->listening.nelts;

    if (index < n
        && ngx_win32_worker_listener_matches(&ls[index], type, local,
                                              local_socklen))
    {
        return &ls[index];
    }

    for (i = 0; i < n; i++) {
        if (i != index
            && ngx_win32_worker_listener_matches(&ls[i], type, local,
                                                  local_socklen))
        {
            return &ls[i];
        }
    }

    return NULL;
}


static ngx_uint_t
ngx_win32_worker_listener_matches(ngx_listening_t *ls, int type,
    struct sockaddr *local, socklen_t local_socklen)
{
    return !ls->ignore && ls->sockaddr && ls->type == type
           && ls->sockaddr->sa_family == local->sa_family
           && ngx_inet_get_port(ls->sockaddr) == ngx_inet_get_port(local)
           && (ls->wildcard
               || ngx_cmp_sockaddr(ls->sockaddr, ls->socklen,
                                   local, local_socklen, 1)
                  == NGX_OK);
}


static void
ngx_win32_worker_channel_handler(ngx_event_t *ev)
{
    size_t                           dispatched, node_size;
    ngx_int_t                        notify;
    ngx_uint_t                       messages;
    ngx_queue_t                     *q;
    ngx_cycle_t                     *cycle;
    ngx_listening_t                 *ls;
    ngx_win32_channel_accept_node_t *node;

    (void) ev;

    if (!ngx_win32_channel_initialized) {
        return;
    }

    cycle = (ngx_cycle_t *) ngx_cycle;
    dispatched = 0;
    messages = 0;

    for ( ;; ) {
        EnterCriticalSection(&ngx_win32_channel_lock);

        if (ngx_queue_empty(&ngx_win32_channel_queue)) {
            ngx_win32_channel_posted = 0;
            LeaveCriticalSection(&ngx_win32_channel_lock);
            return;
        }

        if (messages >= NGX_WIN32_CHANNEL_DISPATCH_MAX
            || dispatched >= NGX_WIN32_CHANNEL_DISPATCH_SIZE)
        {
            notify = 1;
            LeaveCriticalSection(&ngx_win32_channel_lock);

            if (notify
                && ngx_notify(ngx_win32_worker_channel_handler) != NGX_OK)
            {
                ngx_log_error(NGX_LOG_ALERT, cycle->log, 0,
                              "could not continue the Win32 worker channel");

                EnterCriticalSection(&ngx_win32_channel_lock);
                ngx_win32_channel_posted = 0;
                LeaveCriticalSection(&ngx_win32_channel_lock);
            }

            return;
        }

        q = ngx_queue_head(&ngx_win32_channel_queue);
        ngx_queue_remove(q);
        node = ngx_queue_data(q, ngx_win32_channel_accept_node_t, queue);
        node_size = sizeof(ngx_win32_channel_accept_node_t) + node->size;
        ngx_win32_channel_queued--;
        ngx_win32_channel_read_queued -= node_size;
        LeaveCriticalSection(&ngx_win32_channel_lock);

        messages++;
        dispatched += node_size;

        ls = ngx_win32_worker_find_listener(cycle, node->listener,
                           node->type == NGX_WIN32_CHANNEL_UDP_RECV
                           ? SOCK_DGRAM : SOCK_STREAM,
                           &node->local.sockaddr, node->local_socklen);

        if (ls == NULL) {
            ngx_log_error(NGX_LOG_ALERT, cycle->log, 0,
                          "Win32 channel returned no matching listener %ui",
                          node->listener);

            if (node->socket != (ngx_socket_t) -1) {
                (void) ngx_close_socket(node->socket);
            }

            ngx_free(node);
            continue;
        }

        if (node->type == NGX_WIN32_CHANNEL_UDP_RECV) {
            (void) ngx_iocp_udp_dispatch_datagram(ls, node->data, node->size,
                                          &node->remote.sockaddr,
                                          node->remote_socklen,
                                          &node->local.sockaddr,
                                          node->local_socklen);
            ngx_free(node);
            continue;
        }

        if (ngx_event_acceptex_import(ls, node->socket,
                                      &node->local.sockaddr,
                                      node->local_socklen,
                                      &node->remote.sockaddr,
                                      node->remote_socklen, NULL, 0)
            != NGX_OK)
        {
            ngx_log_error(NGX_LOG_ERR, cycle->log, 0,
                          "could not import a routed Win32 connection");
        }

        ngx_free(node);
    }
}


ssize_t
ngx_win32_worker_udp_sendmsg(ngx_connection_t *c, struct msghdr *msg,
    int flags)
{
    size_t                      size;
    ngx_int_t                   rc;
    ngx_uint_t                  i, listener;
    ngx_listening_t            *ls;
    ngx_win32_channel_write_node_t *node;
    ngx_win32_channel_udp_t    *udp;
    ngx_win32_channel_header_t *header;
    u_char                     *p;
    struct sockaddr            *local, *remote;
    socklen_t                   local_socklen, remote_socklen;

    if (!ngx_win32_channel_initialized || ngx_win32_channel_is_stopping()
        || ngx_win32_worker_pipe == NULL || c == NULL || msg == NULL
        || c->listening == NULL
        || msg->msg_iovlen > NGX_WIN32_CHANNEL_UDP_BUFS)
    {
        return NGX_ERROR;
    }

    size = 0;

    for (i = 0; i < msg->msg_iovlen; i++) {
        if ((msg->msg_iov[i].iov_len && msg->msg_iov[i].iov_base == NULL)
            || msg->msg_iov[i].iov_len > NGX_WIN32_CHANNEL_UDP_MAX - size)
        {
            return NGX_ERROR;
        }

        size += msg->msg_iov[i].iov_len;
    }

    remote = msg->msg_name ? msg->msg_name : c->sockaddr;
    remote_socklen = msg->msg_name ? msg->msg_namelen : c->socklen;
    local = c->local_sockaddr ? c->local_sockaddr : c->listening->sockaddr;
    local_socklen = c->local_sockaddr ? c->local_socklen
                                      : c->listening->socklen;

    if (remote == NULL || local == NULL
        || remote_socklen <= 0
        || remote_socklen > (socklen_t) sizeof(ngx_sockaddr_t)
        || local_socklen <= 0
        || local_socklen > (socklen_t) sizeof(ngx_sockaddr_t))
    {
        return NGX_ERROR;
    }

    ls = ((ngx_cycle_t *) ngx_cycle)->listening.elts;
    listener = (ngx_uint_t) (c->listening - ls);

    if (listener >= ngx_win32_worker_expected_listeners) {
        return NGX_ERROR;
    }

    node = ngx_win32_worker_channel_alloc_write(
        sizeof(ngx_win32_channel_udp_t) + size, c->log);
    if (node == NULL) {
        return NGX_ERROR;
    }

    udp = (ngx_win32_channel_udp_t *) node->data;
    ngx_memzero(udp, sizeof(ngx_win32_channel_udp_t));
    header = &udp->header;
    header->magic = NGX_WIN32_CHANNEL_MAGIC;
    header->version = NGX_WIN32_CHANNEL_VERSION;
    header->type = NGX_WIN32_CHANNEL_UDP_SEND;
    header->length = (uint32_t) (sizeof(ngx_win32_channel_udp_t) + size);
    header->slot = (uint32_t) ngx_win32_worker_slot;
    header->generation = (uint32_t) ngx_win32_worker_generation;
    header->listener = (uint32_t) listener;
    header->status = (uint32_t) flags;
    udp->local_socklen = (uint32_t) local_socklen;
    udp->remote_socklen = (uint32_t) remote_socklen;
    udp->data_len = (uint32_t) size;
    udp->flags = (uint32_t) flags;
    ngx_memcpy(&udp->local, local, local_socklen);
    ngx_memcpy(&udp->remote, remote, remote_socklen);

    p = (u_char *) udp + sizeof(ngx_win32_channel_udp_t);

    for (i = 0; i < msg->msg_iovlen; i++) {
        p = ngx_cpymem(p, msg->msg_iov[i].iov_base,
                       msg->msg_iov[i].iov_len);
    }

    rc = ngx_win32_worker_channel_queue_write(node, 0, c->log);

    return rc == NGX_OK ? (ssize_t) size
                        : rc == NGX_AGAIN ? NGX_AGAIN : NGX_ERROR;
}


void
ngx_win32_worker_quic_route_id(u_char *id, size_t len)
{
    uint64_t  tag;

    if (!ngx_win32_worker_routed
        || !ngx_win32_quic_route_key_initialized
        || id == NULL || len != NGX_WIN32_QUIC_ROUTE_CID_LEN
        || ngx_win32_worker_slot > 0xffff)
    {
        return;
    }

    id[0] = (u_char) (NGX_WIN32_QUIC_ROUTE_MAGIC >> 8);
    id[1] = (u_char) NGX_WIN32_QUIC_ROUTE_MAGIC;
    id[2] = (u_char) (ngx_win32_worker_slot >> 8);
    id[3] = (u_char) ngx_win32_worker_slot;
    id[4] = (u_char) (ngx_win32_worker_generation >> 8);
    id[5] = (u_char) ngx_win32_worker_generation;

    tag = ngx_win32_quic_route_tag(id, len);
    id[6] = (u_char) (tag >> 56);
    id[7] = (u_char) (tag >> 48);
    id[8] = (u_char) (tag >> 40);
    id[9] = (u_char) (tag >> 32);
    id[10] = (u_char) (tag >> 24);
    id[11] = (u_char) (tag >> 16);
}


ngx_int_t
ngx_win32_quic_route_decode(u_char *id, size_t len, ngx_uint_t *slot,
    ngx_uint_t *generation)
{
    uint64_t  tag;

    if (!ngx_win32_quic_route_key_initialized
        || id == NULL || len != NGX_WIN32_QUIC_ROUTE_CID_LEN
        || id[0] != (u_char) (NGX_WIN32_QUIC_ROUTE_MAGIC >> 8)
        || id[1] != (u_char) NGX_WIN32_QUIC_ROUTE_MAGIC)
    {
        return NGX_DECLINED;
    }

    tag = ngx_win32_quic_route_tag(id, len);

    if (id[6] != (u_char) (tag >> 56)
        || id[7] != (u_char) (tag >> 48)
        || id[8] != (u_char) (tag >> 40)
        || id[9] != (u_char) (tag >> 32)
        || id[10] != (u_char) (tag >> 24)
        || id[11] != (u_char) (tag >> 16))
    {
        return NGX_DECLINED;
    }

    *slot = ((ngx_uint_t) id[2] << 8) | id[3];
    *generation = ((ngx_uint_t) id[4] << 8) | id[5];

    return NGX_OK;
}


static uint64_t
ngx_win32_quic_route_tag(u_char *id, size_t len)
{
    size_t  tail;
    u_char  input[NGX_WIN32_QUIC_ROUTE_CID_LEN
                  - NGX_WIN32_QUIC_ROUTE_TAG_LEN];

    ngx_memcpy(input, id, NGX_WIN32_QUIC_ROUTE_TAG_OFFSET);
    tail = len - NGX_WIN32_QUIC_ROUTE_TAIL_OFFSET;
    ngx_memcpy(input + NGX_WIN32_QUIC_ROUTE_TAG_OFFSET,
               id + NGX_WIN32_QUIC_ROUTE_TAIL_OFFSET, tail);

    return ngx_siphash(ngx_win32_quic_route_key[0],
                       ngx_win32_quic_route_key[1], input,
                       NGX_WIN32_QUIC_ROUTE_TAG_OFFSET + tail);
}


ngx_int_t
ngx_win32_master_create_bootstrap(ngx_cycle_t *cycle,
    ngx_win32_worker_bootstrap_t *bootstrap, ngx_uint_t slot,
    ngx_uint_t generation, ngx_uint_t role)
{
    u_char                *p;
    u_char                 random[16];
    SECURITY_ATTRIBUTES    sa;
    PSECURITY_DESCRIPTOR   descriptor;

    ngx_memzero(bootstrap, sizeof(ngx_win32_worker_bootstrap_t));

    bootstrap->slot = slot;
    bootstrap->generation = generation;
    bootstrap->role = role;

    if (RtlGenRandom(random, sizeof(random)) == 0) {
        ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_errno,
                      "RtlGenRandom(worker pipe name) failed");
        return NGX_ERROR;
    }

    p = ngx_sprintf(bootstrap->name,
                    "\\\\.\\pipe\\nginx_worker_%s_%ui_%ui_",
                    ngx_unique, generation, slot);
    p = ngx_hex_dump(p, random, sizeof(random));
    *p = '\0';

    descriptor = NULL;

    if (ConvertStringSecurityDescriptorToSecurityDescriptorA(
            "D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;OW)",
            SDDL_REVISION_1, &descriptor, NULL)
        == 0)
    {
        ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_errno,
                      "could not create worker pipe security descriptor");
        return NGX_ERROR;
    }

    ngx_memzero(&sa, sizeof(SECURITY_ATTRIBUTES));
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.lpSecurityDescriptor = descriptor;

    bootstrap->pipe = CreateNamedPipe((char *) bootstrap->name,
                                      PIPE_ACCESS_DUPLEX
                                      | FILE_FLAG_OVERLAPPED
                                      | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE
                                      | PIPE_WAIT
                                      | PIPE_REJECT_REMOTE_CLIENTS,
                                      1, 65536, 65536,
                                      NGX_WIN32_BOOTSTRAP_TIMEOUT, &sa);

    LocalFree(descriptor);

    if (bootstrap->pipe == INVALID_HANDLE_VALUE) {
        ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_errno,
                      "CreateNamedPipe(\"%s\") failed", bootstrap->name);
        bootstrap->pipe = NULL;
        return NGX_ERROR;
    }

    return NGX_OK;
}


ngx_int_t
ngx_win32_master_bootstrap_worker(ngx_cycle_t *cycle,
    ngx_win32_worker_bootstrap_t *bootstrap, ngx_pid_t pid, HANDLE process)
{
    ngx_uint_t                         i, n;
    ngx_listening_t                   *ls;
    ngx_win32_bootstrap_header_t       header;
    ngx_win32_bootstrap_listener_t     listener;
    ngx_win32_bootstrap_listeners_t    listeners;

    if (ngx_win32_master_connect(cycle, bootstrap, process) != NGX_OK) {
        return NGX_ERROR;
    }

    if (ngx_win32_master_read_header(cycle, bootstrap, &header, process)
        != NGX_OK)
    {
        return NGX_ERROR;
    }

    if (header.type != NGX_WIN32_BOOTSTRAP_HELLO
        || header.pid != pid
        || header.slot != bootstrap->slot
        || header.generation != bootstrap->generation)
    {
        ngx_log_error(NGX_LOG_EMERG, cycle->log, 0,
                      "invalid hello from worker process %P", pid);
        return NGX_ERROR;
    }

    n = 0;
    ls = cycle->listening.elts;

    for (i = 0; i < cycle->listening.nelts; i++) {
        if (!ls[i].ignore) {
            n++;
        }
    }

    ngx_memzero(&header, sizeof(ngx_win32_bootstrap_header_t));
    header.magic = NGX_WIN32_BOOTSTRAP_MAGIC;
    header.version = NGX_WIN32_BOOTSTRAP_VERSION;
    header.type = NGX_WIN32_BOOTSTRAP_LISTENERS;
    header.length = (uint32_t) (sizeof(ngx_win32_bootstrap_listeners_t)
                                + n * sizeof(ngx_win32_bootstrap_listener_t));
    header.slot = (uint32_t) bootstrap->slot;
    header.generation = (uint32_t) bootstrap->generation;
    header.pid = (uint32_t) pid;

    ngx_memzero(&listeners, sizeof(ngx_win32_bootstrap_listeners_t));
    listeners.count = (uint32_t) n;

    if (ngx_win32_router_required(cycle)) {
        if (!ngx_win32_quic_route_key_initialized) {
            if (RtlGenRandom(ngx_win32_quic_route_key,
                             sizeof(ngx_win32_quic_route_key)) == 0)
            {
                ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_errno,
                              "RtlGenRandom(QUIC route key) failed");
                return NGX_ERROR;
            }

            ngx_win32_quic_route_key_initialized = 1;
        }

        listeners.flags |= NGX_WIN32_LISTENERS_ROUTED;
        ngx_memcpy(listeners.route_key, ngx_win32_quic_route_key,
                   sizeof(listeners.route_key));
        bootstrap->routed = 1;
    }

    if (ngx_win32_bootstrap_write(bootstrap->pipe, &header,
                                  sizeof(ngx_win32_bootstrap_header_t),
                                  cycle->log)
        != NGX_OK
        || ngx_win32_bootstrap_write(bootstrap->pipe, &listeners,
                                     sizeof(ngx_win32_bootstrap_listeners_t),
                                     cycle->log)
           != NGX_OK)
    {
        return NGX_ERROR;
    }

    n = 0;

    for (i = 0; i < cycle->listening.nelts; i++) {
        if (ls[i].ignore) {
            continue;
        }

        ngx_memzero(&listener, sizeof(ngx_win32_bootstrap_listener_t));

        listener.index = (uint32_t) n++;

        if (ls[i].type == SOCK_STREAM) {
            listener.flags |= NGX_WIN32_LISTENER_STREAM;
        }

        if (ls[i].quic) {
            listener.flags |= NGX_WIN32_LISTENER_QUIC;
        }

        if (WSADuplicateSocket(ls[i].fd, pid, &listener.info) == -1) {
            ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_socket_errno,
                          "WSADuplicateSocket(%V, %P) failed",
                          &ls[i].addr_text, pid);
            return NGX_ERROR;
        }

        if (ngx_win32_bootstrap_write(bootstrap->pipe, &listener,
                                      sizeof(ngx_win32_bootstrap_listener_t),
                                      cycle->log)
            != NGX_OK)
        {
            return NGX_ERROR;
        }
    }

    bootstrap->expected = n;

    if (ngx_win32_master_wait_status(cycle, bootstrap,
                                     NGX_WIN32_BOOTSTRAP_IMPORT_OK, process)
        != NGX_OK)
    {
        return NGX_ERROR;
    }

    bootstrap->imported = 1;

    return NGX_OK;
}


ngx_int_t
ngx_win32_master_wait_status(ngx_cycle_t *cycle,
    ngx_win32_worker_bootstrap_t *bootstrap, ngx_uint_t type, HANDLE process)
{
    ngx_win32_bootstrap_header_t  header;

    if (ngx_win32_master_read_header(cycle, bootstrap, &header, process)
        != NGX_OK)
    {
        return NGX_ERROR;
    }

    if (header.type == NGX_WIN32_BOOTSTRAP_FAIL) {
        ngx_log_error(NGX_LOG_EMERG, cycle->log, header.status,
                      "worker %ui generation %ui bootstrap failed",
                      bootstrap->slot, bootstrap->generation);
        return NGX_ERROR;
    }

    if (header.type != type
        || header.slot != bootstrap->slot
        || header.generation != bootstrap->generation)
    {
        ngx_log_error(NGX_LOG_EMERG, cycle->log, 0,
                      "unexpected worker %ui generation %ui bootstrap "
                      "message %ud, expected %ui", bootstrap->slot,
                      bootstrap->generation, header.type, type);
        return NGX_ERROR;
    }

    return NGX_OK;
}


void
ngx_win32_master_close_bootstrap(ngx_win32_worker_bootstrap_t *bootstrap)
{
    if (bootstrap->pipe) {
        FlushFileBuffers(bootstrap->pipe);
        DisconnectNamedPipe(bootstrap->pipe);
        ngx_close_handle(bootstrap->pipe);
        bootstrap->pipe = NULL;
    }
}


ngx_int_t
ngx_win32_accept_mutex_init(ngx_cycle_t *cycle)
{
    u_char                name[NGX_PROCESS_SYNC_NAME];
    SECURITY_ATTRIBUTES   sa;
    PSECURITY_DESCRIPTOR  descriptor;

    if (ngx_win32_accept_mutex) {
        return NGX_OK;
    }

    ngx_sprintf(name, "Local\\ngx_accept_%s%Z", ngx_unique);

    descriptor = NULL;

    if (ConvertStringSecurityDescriptorToSecurityDescriptorA(
            "D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;OW)",
            SDDL_REVISION_1, &descriptor, NULL)
        == 0)
    {
        ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_errno,
                      "could not create accept mutex security descriptor");
        return NGX_ERROR;
    }

    ngx_memzero(&sa, sizeof(SECURITY_ATTRIBUTES));
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.lpSecurityDescriptor = descriptor;

    ngx_win32_accept_mutex = CreateMutex(&sa, 0, (char *) name);
    LocalFree(descriptor);
    if (ngx_win32_accept_mutex == NULL) {
        ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_errno,
                      "CreateMutex(\"%s\") failed", name);
        return NGX_ERROR;
    }

    return NGX_OK;
}


ngx_int_t
ngx_win32_accept_mutex_trylock(ngx_cycle_t *cycle)
{
    u_long  rc;

    rc = WaitForSingleObject(ngx_win32_accept_mutex, 0);

    if (rc == WAIT_OBJECT_0 || rc == WAIT_ABANDONED) {
        if (rc == WAIT_ABANDONED) {
            ngx_log_error(NGX_LOG_WARN, cycle->log, 0,
                          "recovered an abandoned accept mutex");
        }

        return 1;
    }

    if (rc == WAIT_TIMEOUT) {
        return 0;
    }

    ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                  "WaitForSingleObject(accept mutex) failed");

    return NGX_ERROR;
}


void
ngx_win32_accept_mutex_unlock(void)
{
    if (ngx_win32_accept_mutex && ReleaseMutex(ngx_win32_accept_mutex) == 0) {
        ngx_log_error(NGX_LOG_ALERT, ngx_cycle->log, ngx_errno,
                      "ReleaseMutex(accept mutex) failed");
    }
}


void
ngx_win32_accept_mutex_done(void)
{
    if (ngx_win32_accept_mutex) {
        ngx_close_handle(ngx_win32_accept_mutex);
        ngx_win32_accept_mutex = NULL;
    }
}


static ngx_int_t
ngx_win32_bootstrap_read(HANDLE pipe, void *data, size_t size, ngx_log_t *log)
{
    if (pipe == ngx_win32_worker_pipe) {
        return ngx_win32_worker_pipe_io(pipe, data, size, 0, log);
    }

    return ngx_win32_master_pipe_io(pipe, data, size, 0, log);
}


static ngx_int_t
ngx_win32_bootstrap_write(HANDLE pipe, const void *data, size_t size,
    ngx_log_t *log)
{
    if (pipe == ngx_win32_worker_pipe) {
        return ngx_win32_worker_pipe_io(pipe, (void *) data, size, 1, log);
    }

    return ngx_win32_master_pipe_io(pipe, (void *) data, size, 1, log);
}


static ngx_int_t
ngx_win32_master_pipe_io(HANDLE pipe, void *data, size_t size,
    ngx_uint_t write, ngx_log_t *log)
{
    BOOL        rc;
    DWORD       chunk, error, n;
    HANDLE      event;
    u_long      wait;
    u_char     *p;
    OVERLAPPED  overlapped;

    event = CreateEvent(NULL, 1, 0, NULL);
    if (event == NULL) {
        ngx_log_error(NGX_LOG_EMERG, log, ngx_errno,
                      "CreateEvent(worker bootstrap pipe) failed");
        return NGX_ERROR;
    }

    p = data;

    while (size) {
        ngx_memzero(&overlapped, sizeof(OVERLAPPED));
        overlapped.hEvent = event;
        (void) ResetEvent(event);
        chunk = (DWORD) ngx_min(size, 0xffffffff);
        n = 0;

        rc = write ? WriteFile(pipe, p, chunk, &n, &overlapped)
                   : ReadFile(pipe, p, chunk, &n, &overlapped);

        if (rc == 0) {
            error = ngx_errno;

            if (error != ERROR_IO_PENDING) {
                ngx_log_error(NGX_LOG_EMERG, log, error,
                              "%s(worker bootstrap) failed",
                              write ? "WriteFile" : "ReadFile");
                ngx_close_handle(event);
                return NGX_ERROR;
            }

            wait = WaitForSingleObject(event, NGX_WIN32_BOOTSTRAP_TIMEOUT);
            if (wait != WAIT_OBJECT_0) {
                error = wait == WAIT_FAILED ? ngx_errno : WAIT_TIMEOUT;
                (void) CancelIoEx(pipe, &overlapped);
                (void) GetOverlappedResult(pipe, &overlapped, &n, TRUE);
                ngx_log_error(NGX_LOG_EMERG, log, error,
                              "%s(worker bootstrap) failed",
                              write ? "WriteFile" : "ReadFile");
                ngx_close_handle(event);
                return NGX_ERROR;
            }

            if (GetOverlappedResult(pipe, &overlapped, &n, FALSE) == 0) {
                ngx_log_error(NGX_LOG_EMERG, log, ngx_errno,
                              "%s(worker bootstrap) failed",
                              write ? "WriteFile" : "ReadFile");
                ngx_close_handle(event);
                return NGX_ERROR;
            }
        }

        if (n == 0) {
            ngx_log_error(NGX_LOG_EMERG, log, 0,
                          "worker bootstrap pipe accepted no data");
            ngx_close_handle(event);
            return NGX_ERROR;
        }

        p += n;
        size -= n;
    }

    ngx_close_handle(event);

    return NGX_OK;
}


static ngx_int_t
ngx_win32_worker_pipe_io(HANDLE pipe, void *data, size_t size,
    ngx_uint_t write, ngx_log_t *log)
{
    BOOL        rc;
    DWORD       chunk, error, n;
    HANDLE      event;
    u_long      wait;
    u_char     *p;
    OVERLAPPED  overlapped;

    event = write ? ngx_win32_worker_write_event
                  : ngx_win32_worker_read_event;

    if (event == NULL) {
        ngx_log_error(NGX_LOG_EMERG, log, 0,
                      "worker pipe event is not initialized");
        return NGX_ERROR;
    }

    p = data;

    while (size) {
        ngx_memzero(&overlapped, sizeof(OVERLAPPED));
        overlapped.hEvent = event;

        if (ResetEvent(event) == 0) {
            ngx_log_error(NGX_LOG_EMERG, log, ngx_errno,
                          "ResetEvent(worker pipe) failed");
            return NGX_ERROR;
        }

        chunk = (DWORD) ngx_min(size, 0xffffffff);
        n = 0;

        rc = write ? WriteFile(pipe, p, chunk, &n, &overlapped)
                   : ReadFile(pipe, p, chunk, &n, &overlapped);

        if (rc == 0) {
            error = ngx_errno;

            if (error != ERROR_IO_PENDING) {
                if (error != ERROR_OPERATION_ABORTED
                    || !ngx_win32_channel_is_stopping())
                {
                    ngx_log_error(NGX_LOG_EMERG, log, error,
                                  "%s(worker pipe) failed",
                                  write ? "WriteFile" : "ReadFile");
                }

                return NGX_ERROR;
            }

            wait = WaitForSingleObject(event, INFINITE);
            if (wait != WAIT_OBJECT_0) {
                ngx_log_error(NGX_LOG_EMERG, log, ngx_errno,
                              "WaitForSingleObject(worker pipe) failed");
                return NGX_ERROR;
            }

            if (GetOverlappedResult(pipe, &overlapped, &n, 0) == 0) {
                error = ngx_errno;

                if (error != ERROR_OPERATION_ABORTED
                    || !ngx_win32_channel_is_stopping())
                {
                    ngx_log_error(NGX_LOG_EMERG, log, error,
                                  "GetOverlappedResult(worker pipe) failed");
                }

                return NGX_ERROR;
            }
        }

        if (n == 0) {
            ngx_log_error(NGX_LOG_EMERG, log, 0,
                          "worker pipe accepted no data");
            return NGX_ERROR;
        }

        p += n;
        size -= n;
    }

    return NGX_OK;
}


static ngx_int_t
ngx_win32_bootstrap_read_header(HANDLE pipe,
    ngx_win32_bootstrap_header_t *header, ngx_log_t *log)
{
    if (ngx_win32_bootstrap_read(pipe, header,
                                 sizeof(ngx_win32_bootstrap_header_t), log)
        != NGX_OK)
    {
        return NGX_ERROR;
    }

    if (header->magic != NGX_WIN32_BOOTSTRAP_MAGIC
        || header->version != NGX_WIN32_BOOTSTRAP_VERSION)
    {
        ngx_log_error(NGX_LOG_EMERG, log, 0,
                      "unsupported worker bootstrap protocol");
        return NGX_ERROR;
    }

    return NGX_OK;
}


static ngx_int_t
ngx_win32_bootstrap_get_uint(char *name, ngx_uint_t *value, ngx_log_t *log)
{
    DWORD      n;
    ngx_int_t  parsed;
    u_char     text[NGX_INT_T_LEN + 1];

    n = GetEnvironmentVariable(name, (char *) text, NGX_INT_T_LEN + 1);

    if (n == 0 || n > NGX_INT_T_LEN) {
        ngx_log_error(NGX_LOG_EMERG, log, n == 0 ? ngx_errno : 0,
                      "invalid worker environment variable \"%s\"", name);
        return NGX_ERROR;
    }

    parsed = ngx_atoi(text, n);
    if (parsed == NGX_ERROR) {
        ngx_log_error(NGX_LOG_EMERG, log, 0,
                      "invalid worker environment variable \"%s\"", name);
        return NGX_ERROR;
    }

    *value = (ngx_uint_t) parsed;

    return NGX_OK;
}


static ngx_int_t
ngx_win32_master_connect(ngx_cycle_t *cycle,
    ngx_win32_worker_bootstrap_t *bootstrap, HANDLE process)
{
    BOOL        connected;
    DWORD       bytes, err, mode;
    HANDLE      event, events[2];
    OVERLAPPED  overlapped;
    u_long      rc;

    event = CreateEvent(NULL, 1, 0, NULL);
    if (event == NULL) {
        ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_errno,
                      "CreateEvent(worker pipe connect) failed");
        return NGX_ERROR;
    }

    ngx_memzero(&overlapped, sizeof(OVERLAPPED));
    overlapped.hEvent = event;
    connected = ConnectNamedPipe(bootstrap->pipe, &overlapped);

    if (!connected) {
        err = ngx_errno;

        if (err == ERROR_IO_PENDING) {
            events[0] = event;
            events[1] = process;
            rc = WaitForMultipleObjects(2, events, FALSE,
                                        NGX_WIN32_BOOTSTRAP_TIMEOUT);

            if (rc != WAIT_OBJECT_0) {
                err = rc == WAIT_FAILED ? ngx_errno : 0;
                (void) CancelIoEx(bootstrap->pipe, &overlapped);
                (void) GetOverlappedResult(bootstrap->pipe, &overlapped,
                                           &bytes, TRUE);
                ngx_log_error(NGX_LOG_EMERG, cycle->log, err,
                              "worker %ui generation %ui did not connect "
                              "its bootstrap pipe", bootstrap->slot,
                              bootstrap->generation);
                ngx_close_handle(event);
                return NGX_ERROR;
            }

            if (GetOverlappedResult(bootstrap->pipe, &overlapped, &err,
                                    FALSE)
                == 0)
            {
                ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_errno,
                              "ConnectNamedPipe(\"%s\") failed",
                              bootstrap->name);
                ngx_close_handle(event);
                return NGX_ERROR;
            }

        } else if (err != ERROR_PIPE_CONNECTED) {
            ngx_log_error(NGX_LOG_EMERG, cycle->log, err,
                          "ConnectNamedPipe(\"%s\") failed",
                          bootstrap->name);
            ngx_close_handle(event);
            return NGX_ERROR;
        }
    }

    ngx_close_handle(event);

    mode = PIPE_READMODE_BYTE | PIPE_WAIT;

    if (SetNamedPipeHandleState(bootstrap->pipe, &mode, NULL, NULL) == 0) {
        ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_errno,
                      "SetNamedPipeHandleState(\"%s\") failed",
                      bootstrap->name);
        return NGX_ERROR;
    }

    return NGX_OK;
}


static ngx_int_t
ngx_win32_master_read_header(ngx_cycle_t *cycle,
    ngx_win32_worker_bootstrap_t *bootstrap,
    ngx_win32_bootstrap_header_t *header, HANDLE process)
{
    DWORD   available, start;
    u_long  rc;

    start = GetTickCount();

    for ( ;; ) {
        available = 0;

        if (PeekNamedPipe(bootstrap->pipe, NULL, 0, NULL, &available, NULL)
            == 0)
        {
            ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_errno,
                          "PeekNamedPipe(\"%s\") failed", bootstrap->name);
            return NGX_ERROR;
        }

        if (available >= sizeof(ngx_win32_bootstrap_header_t)) {
            break;
        }

        rc = WaitForSingleObject(process, 0);
        if (rc == WAIT_OBJECT_0) {
            ngx_log_error(NGX_LOG_EMERG, cycle->log, 0,
                          "worker %ui generation %ui exited during "
                          "bootstrap", bootstrap->slot,
                          bootstrap->generation);
            return NGX_ERROR;
        }

        if ((DWORD) (GetTickCount() - start) >= NGX_WIN32_BOOTSTRAP_TIMEOUT) {
            ngx_log_error(NGX_LOG_EMERG, cycle->log, 0,
                          "worker %ui generation %ui bootstrap timed out "
                          "after %dms", bootstrap->slot,
                          bootstrap->generation,
                          NGX_WIN32_BOOTSTRAP_TIMEOUT);
            return NGX_ERROR;
        }

        Sleep(10);
    }

    return ngx_win32_bootstrap_read_header(bootstrap->pipe, header,
                                           cycle->log);
}
