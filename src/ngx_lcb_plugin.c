/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2013 Couchbase, Inc.
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

/**
 * This file contains IO operations that use nginx
 *
 * @author Sergey Avseyev
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>
#include <ngx_event_connect.h>
#include <libcouchbase/couchbase.h>
#include "ngx_lcb_plugin.h"
#include "ddebug.h"

typedef void (*ngx_lcb_handler_pt)(lcb_socket_t sock, short which, void *data);

struct ngx_lcb_context_s {
    ngx_peer_connection_t *peer;
    ngx_lcb_handler_pt handler;
    short handler_mask;
    void *handler_data;
    struct addrinfo *root_ai;
    struct addrinfo *curr_ai;
    lcb_io_plugin_connect_cb conn_handler;
    void *conn_data;
};
typedef struct ngx_lcb_context_s ngx_lcb_context_t;

#define to_socket(X)    ((lcb_socket_t)(intptr_t)(X))
#define from_socket(X)  ((ngx_lcb_context_t *)(intptr_t)(X))

/* XXX it should use instance-level setting somehow */
static int common_getaddrinfo(const char *hostname,
                              const char *servname,
                              struct addrinfo **res)
{
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_flags = AI_PASSIVE;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_INET;
    return getaddrinfo(hostname, servname, &hints, res);
}

/* allocate ngx_peer_connection_t struct */
static lcb_socket_t
ngx_lcb_socket(lcb_io_opt_t io, const char *hostname, const char *servname)
{
    ngx_lcb_context_t *ctx;
    ngx_lcb_cookie_t cookie = io->v.v0.cookie;

    ctx = ngx_pcalloc(cookie->pool, sizeof(ngx_lcb_context_t));
    if (ctx == NULL) {
        return to_socket(-1);
    }
    ctx->peer = ngx_pcalloc(cookie->pool, sizeof(ngx_peer_connection_t));
    if (ctx->peer == NULL) {
        ngx_pfree(cookie->pool, ctx);
        return to_socket(-1);
    }
    ctx->peer->log = cookie->log;
    ctx->peer->log_error = NGX_ERROR_ERR;
    ctx->peer->get = ngx_event_get_peer;
    if (common_getaddrinfo(hostname, servname, &ctx->root_ai) != 0) {
        ngx_pfree(cookie->pool, ctx->peer);
        ngx_pfree(cookie->pool, ctx);
        return to_socket(-1);
    }
    ctx->curr_ai = ctx->root_ai;

    return to_socket(ctx);
}

static void
ngx_lcb_close(lcb_io_opt_t io, lcb_socket_t sock)
{
    ngx_lcb_cookie_t cookie = io->v.v0.cookie;
    ngx_lcb_context_t *ctx = from_socket(sock);
    ngx_close_connection(ctx->peer->connection);
    ngx_pfree(cookie->pool, ctx->peer);
    ctx->peer->connection = NULL;
}

static void ngx_lcb_handler_thunk(ngx_event_t *ev)
{
    ngx_connection_t *conn = ev->data;
    ngx_lcb_context_t *ctx = conn->data;
    int which = 0;

    dd("enter event handler");
    if (ev->write) {
        which |= LCB_WRITE_EVENT;
    } else {
        which |= LCB_READ_EVENT;
    }

    if (ctx->handler_mask & which) {
        dd("calling libcouchbase handler");
        ctx->handler(to_socket(ctx), which, ctx->handler_data);
    }
    dd("exit event handler");
}

LIBCOUCHBASE_API
int ngx_lcb_connect(lcb_io_opt_t io,
                    lcb_socket_t sock,
                    const struct sockaddr *name,
                    unsigned int namelen)
{
    ngx_lcb_cookie_t cookie = io->v.v0.cookie;
    ngx_lcb_context_t *ctx = from_socket(sock);
    ngx_peer_connection_t *peer = ctx->peer;
    size_t len;

    dd("initialize peer");
    peer->sockaddr = (struct sockaddr *)name;
    peer->socklen = namelen;
    /* FIXME free peer->name later */
    peer->name = ngx_pnalloc(cookie->pool, sizeof(ngx_str_t));
    if (peer->name == NULL) {
        return -1;
    }
    len = NGX_INET_ADDRSTRLEN + sizeof(":65535") - 1;
    peer->name->data = ngx_pnalloc(cookie->pool, len);
    if (peer->name->data == NULL) {
        ngx_pfree(cookie->pool, peer->name);
        return -1;
    }
    peer->name->len = ngx_sock_ntop(peer->sockaddr, peer->name->data, len, 1);
    dd("peer initialized");
    return 0;
}

static void ngx_lcb_connect_handler_thunk(ngx_event_t *ev)
{
    ngx_connection_t *conn = ev->data;
    ngx_lcb_context_t *ctx = conn->data;

    conn->read->handler = ngx_lcb_handler_thunk;
    conn->write->handler = ngx_lcb_handler_thunk;
    ctx->conn_handler(LCB_SUCCESS, ctx->conn_data);

    /* check for broken connection */
    dd("exit connect handler");
}

LIBCOUCHBASE_API
void ngx_lcb_connect_peer(lcb_io_opt_t io,
                          lcb_socket_t sock,
                          void *event,
                          void *cb_data,
                          lcb_io_plugin_connect_cb handler)
{
    ngx_lcb_cookie_t cookie = io->v.v0.cookie;
    ngx_lcb_context_t *ctx = from_socket(sock);
    ngx_peer_connection_t *peer = ctx->peer;
    ngx_int_t rc;

    dd("connecting the peer");
    if (peer->sockaddr == NULL) {
        if (ngx_lcb_connect(io, sock, ctx->curr_ai->ai_addr,
                            ctx->curr_ai->ai_addrlen) != 0) {
            handler(LCB_CONNECT_ERROR, cb_data);
            return;
        }
    }
    dd("setup flags");
    rc = ngx_event_connect_peer(peer);
    dd("peer->connection = %p", (void *)peer->connection);
    dd("peer->connection->write->data = %p", (void *)peer->connection->write->data);
    dd("rc = %d", (int)rc);
    if (rc == NGX_ERROR || rc == NGX_BUSY || rc == NGX_DECLINED) {
        io->v.v0.error = ngx_socket_errno;
        dd("ngx_event_connect_peer(\"%s\") error %d, %s\n",
           peer->name->data, (int)ngx_socket_errno, strerror(ngx_socket_errno));
        handler(LCB_CONNECT_ERROR, cb_data);
        return;
    }
    if (ngx_event_flags & NGX_USE_CLEAR_EVENT) {
        ngx_add_event(peer->connection->write, NGX_WRITE_EVENT, NGX_CLEAR_EVENT);
        /* FIXME handle error code */
    }
    peer->connection->data = ctx;
    if (rc == NGX_AGAIN) {
        dd("will retry");
        peer->connection->write->handler = ngx_lcb_connect_handler_thunk;
        peer->connection->read->handler = ngx_lcb_connect_handler_thunk;
        ctx->conn_data = cb_data;
        ctx->conn_handler = handler;
        ngx_add_timer(peer->connection->write, 300); /* ms */
        dd("return");
        return;
    }
    peer->connection->read->handler = ngx_lcb_handler_thunk;
    peer->connection->write->handler = ngx_lcb_handler_thunk;
    handler(LCB_SUCCESS, cb_data);
    dd("connected\n");
    dd("set event data to %p\n", (void *)ctx);
}

static lcb_ssize_t
ngx_lcb_recv(lcb_io_opt_t io, lcb_socket_t sock, void *buf, lcb_size_t nbuf, int flags)
{
    ngx_lcb_cookie_t cookie = io->v.v0.cookie;
    ngx_lcb_context_t *ctx = from_socket(sock);
    ssize_t ret;

    ret = ctx->peer->connection->recv(ctx->peer->connection, buf, nbuf);
    if (ret < 0) {
        io->v.v0.error = ngx_socket_errno;
    }

    (void)flags;
    return ret;
}

static int
iovec2chains(ngx_lcb_cookie_t cookie,
             struct lcb_iovec_st *iov, lcb_size_t niov,
             ngx_chain_t **chains, ngx_buf_t **buffers)
{
    ngx_chain_t *cc;
    ngx_buf_t *bb;
    lcb_size_t ii;

    cc = ngx_pcalloc(cookie->pool, niov * sizeof(ngx_chain_t));
    bb = ngx_pcalloc(cookie->pool, niov * sizeof(ngx_buf_t));
    if (cc == NULL || bb == NULL) {
        ngx_log_error(NGX_LOG_ERR, cookie->log, 0,
                      "Failed to allocate response buffer.");
        ngx_pfree(cookie->pool, cc);
        ngx_pfree(cookie->pool, bb);
        return -1;
    }

    for (ii = 0; ii < niov; ++ii) {
        if (iov->iov_len == 0) {
            break;
        }
        bb[ii].pos = iov->iov_base;
        bb[ii].last = iov->iov_base + iov->iov_len;
        bb[ii].memory = 1;
        cc[ii].buf = bb + ii;
        cc[ii].next = (ii + 1 < niov) ? cc + ii + 1 : NULL;
    }
    bb[ii - 1].last_buf = 1;

    *chains = cc;
    *buffers = bb;
    return 0;
}

static lcb_ssize_t
ngx_lcb_recvv(lcb_io_opt_t io, lcb_socket_t sock, struct lcb_iovec_st *iov, lcb_size_t niov)
{
    ngx_lcb_cookie_t cookie = io->v.v0.cookie;
    ngx_lcb_context_t *ctx = from_socket(sock);
    ngx_chain_t *chains;
    ngx_buf_t *buffers;
    ssize_t ret;

    if (iovec2chains(cookie, iov, niov, &chains, &buffers) != 0) {
        return -1;
    }
    ret = ctx->peer->connection->recv_chain(ctx->peer->connection, chains);
    if (ret < 0) {
        io->v.v0.error = ngx_socket_errno;
    }
    ngx_pfree(cookie->pool, chains);
    ngx_pfree(cookie->pool, buffers);
    return ret;
}

static lcb_ssize_t
ngx_lcb_send(lcb_io_opt_t io, lcb_socket_t sock, const void *buf, lcb_size_t nbuf, int flags)
{
    ngx_lcb_cookie_t cookie = io->v.v0.cookie;
    ngx_lcb_context_t *ctx = from_socket(sock);
    ssize_t ret;

    ret = ctx->peer->connection->send(ctx->peer->connection, (u_char *)buf, nbuf);
    if (ret < 0) {
        io->v.v0.error = ngx_socket_errno;
    }

    (void)flags;
    return ret;
}

static lcb_ssize_t
ngx_lcb_sendv(lcb_io_opt_t io, lcb_socket_t sock, struct lcb_iovec_st *iov, lcb_size_t niov)
{
    ngx_lcb_cookie_t cookie = io->v.v0.cookie;
    ngx_lcb_context_t *ctx = from_socket(sock);
    ngx_chain_t *chains, *rc;
    ngx_buf_t *buffers;
    ssize_t old;

    if (iovec2chains(cookie, iov, niov, &chains, &buffers) != 0) {
        return -1;
    }
    old = ctx->peer->connection->sent;
    rc = ctx->peer->connection->send_chain(ctx->peer->connection, chains, 0);
    if (rc == NGX_CHAIN_ERROR) {
        io->v.v0.error = ngx_socket_errno;
    }
    ngx_pfree(cookie->pool, chains);
    ngx_pfree(cookie->pool, buffers);

    return ctx->peer->connection->sent - old;
}

void
ngx_lcb_timer_delete(lcb_io_opt_t io, void *timer)
{
    ngx_lcb_cookie_t cookie = io->v.v0.cookie;
    (void)cookie;
    (void)timer;
}

static int
ngx_lcb_timer_update(lcb_io_opt_t io, void *timer, lcb_uint32_t usec, void *data, ngx_lcb_handler_pt handler)
{
    ngx_lcb_cookie_t cookie = io->v.v0.cookie;
    (void)cookie;
    (void)timer;
    (void)usec;
    (void)data;
    (void)handler;
}

static int
ngx_lcb_event_update(lcb_io_opt_t io, lcb_socket_t sock, void *event, short flags, void *data, ngx_lcb_handler_pt handler)
{
    ngx_lcb_context_t *ctx = from_socket(sock);

    dd("update event(%p): name=%p handler=%p, mask=%d, data=%p",
       ctx->peer->name, (void *)ctx, (void *)handler, (int)flags, data);
    ctx->handler = handler;
    ctx->handler_mask = flags;
    ctx->handler_data = data;
    (void)event;
}

static void
ngx_lcb_event_delete(lcb_io_opt_t io, lcb_socket_t sock, void *event)
{
    ngx_lcb_context_t *ctx = from_socket(sock);

    ctx->handler = NULL;
    ctx->handler_mask = 0;
    ctx->handler_data = NULL;
    (void)event;
}

static void *
ngx_lcb_event_create_noop(lcb_io_opt_t io)
{
    (void)io;
}

static void
ngx_lcb_event_destroy_noop(lcb_io_opt_t io, void *event)
{
    (void)io;
    (void)event;
}

static void
ngx_lcb_noop(lcb_io_opt_t io)
{
    ngx_lcb_cookie_t cookie = io->v.v0.cookie;
    (void)cookie;
}

static void
ngx_lcb_destroy_io_opts(lcb_io_opt_t io)
{
    ngx_lcb_cookie_t cookie = io->v.v0.cookie;
    free(io);
}

lcb_error_t
ngx_lcb_create_io_opts(int version, lcb_io_opt_t *io, void *cookie)
{
    lcb_io_opt_t ret;

    if (version != 0) {
        return LCB_PLUGIN_VERSION_MISMATCH;
    }
    ret = calloc(1, sizeof(*ret));
    if (ret == NULL) {
        free(ret);
        return LCB_CLIENT_ENOMEM;
    }
    if (cookie == NULL) {
        return LCB_EINVAL;
    } else {
        ret->v.v0.cookie = cookie;
    }

    /* setup io iops! */
    ret->version = 0;
    ret->dlhandle = NULL;
    ret->destructor = ngx_lcb_destroy_io_opts;
    /* consider that struct isn't allocated by the library,
     * `need_cleanup' flag might be set in lcb_create() */
    ret->v.v0.need_cleanup = 0;
    ret->v.v0.recv = ngx_lcb_recv;
    ret->v.v0.send = ngx_lcb_send;
    ret->v.v0.recvv = ngx_lcb_recvv;
    ret->v.v0.sendv = ngx_lcb_sendv;
    ret->v.v0.socket = ngx_lcb_socket;
    ret->v.v0.close = ngx_lcb_close;
    ret->v.v0.connect = ngx_lcb_connect;
    ret->v.v0.connect_peer = ngx_lcb_connect_peer;

    ret->v.v0.delete_event = ngx_lcb_event_delete;
    ret->v.v0.update_event = ngx_lcb_event_update;

    ret->v.v0.delete_timer = ngx_lcb_timer_delete;
    ret->v.v0.update_timer = ngx_lcb_timer_update;

    ret->v.v0.create_event = ngx_lcb_event_create_noop;
    ret->v.v0.create_timer = ngx_lcb_event_create_noop;
    ret->v.v0.destroy_event = ngx_lcb_event_destroy_noop;
    ret->v.v0.destroy_timer = ngx_lcb_event_destroy_noop;
    ret->v.v0.run_event_loop = ngx_lcb_noop;
    ret->v.v0.stop_event_loop = ngx_lcb_noop;

    *io = ret;
    return LCB_SUCCESS;
}
