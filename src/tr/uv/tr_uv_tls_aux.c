/**
 * Copyright (c) 2014 NetEase, Inc. and other Pomelo contributors
 * MIT Licensed.
 */

#include <assert.h>

#include "tr_uv_tcp_aux.h"
#include "tr_uv_tls_aux.h"
#include "pc_pomelo_i.h"
#include "pc_lib.h"

#define GET_TLS(x) tr_uv_tls_transport_t* tls; \
    tr_uv_tcp_transport_t * tt;          \
    tls = (tr_uv_tls_transport_t* )x->data; \
    tt = &tls->base; assert(tt && tls)

#define GET_TT tr_uv_tcp_transport_t* tt = &tls->base; assert(tt && tls)

static void tls__read_from_bio(tr_uv_tls_transport_t* tls);
static int tls__get_error(SSL* ssl, int status);
static void tls__write_to_tcp(tr_uv_tls_transport_t* tls);
static void tls__cycle(tr_uv_tls_transport_t* tls);

void tls__reset(tr_uv_tcp_transport_t* tt)
{
    int ret;
    QUEUE* q;

    tr_uv_tls_transport_t* tls = (tr_uv_tls_transport_t* )tt;

    if (!SSL_clear(tls->tls)) {
        pc_lib_log(PC_LOG_WARN, "tls__reset - ssl clear error: %s",
                ERR_error_string(ERR_get_error(), NULL));
    }

    ret = BIO_reset(tls->in);
    assert(ret == 1);

    ret = BIO_reset(tls->out);
    assert(ret == 1);

    // write should retry remained, insert it to writing queue
    // then tcp__reset will recycle it.
    if (tls->should_retry) {
        QUEUE_INIT(&tls->should_retry->queue);
        QUEUE_INSERT_TAIL(&tt->writing_queue, &tls->should_retry->queue);

        tls->should_retry = NULL;
    } 

    if (tls->retry_wb) {
        pc_lib_free(tls->retry_wb);
        tls->retry_wb = NULL;
        tls->retry_wb_len = 0;
    }

    // tcp reset will recycle following write item
    while(!QUEUE_EMPTY(&tls->when_tcp_is_writing_queue)) {
        q = QUEUE_HEAD(&tls->when_tcp_is_writing_queue);
        QUEUE_REMOVE(q);
        QUEUE_INIT(q);

        QUEUE_INSERT_TAIL(&tt->writing_queue, q);
    }
    
    tcp__reset(tt);
}

void tls__conn_done_cb(uv_connect_t* conn, int status)
{
    GET_TLS(conn);

    tcp__conn_done_cb(conn, status);

    // success
    if (!status) {
        // SSL_read will write tls handshake data to bio. 
        tls__read_from_bio(tls);
        
        // write tls handshake data out
        tls__write_to_tcp(tls); 
    }
}

static void tls__write_to_bio(tr_uv_tls_transport_t* tls)
{
    int ret = 0;
    QUEUE* head;
    QUEUE* q;
    tr_uv_wi_t* wi = NULL;
    int flag = 0;

    tr_uv_tcp_transport_t* tt = (tr_uv_tcp_transport_t* )tls;

    assert(tt->state == TR_UV_TCP_CONNECTING || tt->state == TR_UV_TCP_HANDSHAKEING || tt->state == TR_UV_TCP_DONE);

    if (tt->is_writing) {
        // pending write item
        head = &tls->when_tcp_is_writing_queue;
    } else {
        head = &tt->writing_queue;
    }

    if (tt->state == TR_UV_TCP_DONE) {
        while (!QUEUE_EMPTY(&tt->conn_pending_queue)) {
            q = QUEUE_HEAD(&tt->conn_pending_queue);
            QUEUE_REMOVE(q);
            QUEUE_INIT(q);
            QUEUE_INSERT_TAIL(&tt->write_wait_queue, q);
        }
    }

    if (tls->retry_wb) {
        ret = SSL_write(tls->tls, tls->retry_wb, tls->retry_wb_len);
        assert(ret == -1 || ret == tls->retry_wb_len);

        if (ret == -1) {
            if (tls__get_error(tls->tls, ret)) {
                pc_trans_fire_event(tt->client, PC_EV_UNEXPECTED_DISCONNECT, "TLS Error", NULL);
                tt->reconn_fn(tt);
                return ;
            } else {
                // retry fails, do nothing.
            }
        } else {
            // retry succeeds
            if (tls->should_retry) {
                QUEUE_INIT(&tls->should_retry->queue);
                QUEUE_INSERT_TAIL(head, &tls->should_retry->queue);

                tls->should_retry = NULL;
            }
            pc_lib_free(tls->retry_wb);
            tls->retry_wb = NULL;
            tls->retry_wb_len = 0;
            // write to bio success.
            flag = 1;
        }
    }

    // retry write buf has been written, try to write more data to bio.
    if (!tls->retry_wb) {
        while(!QUEUE_EMPTY(&tt->write_wait_queue)) {
            q = QUEUE_HEAD(&tt->write_wait_queue);
            QUEUE_REMOVE(q);
            QUEUE_INIT(q);

            wi = (tr_uv_wi_t* )QUEUE_DATA(q, tr_uv_wi_t, queue);
            ret = SSL_write(tls->tls, wi->buf.base, wi->buf.len);
            assert(ret == -1 || ret == wi->buf.len);
            if (ret == -1) {
                tls->should_retry = wi;
                if (tls__get_error(tls->tls, ret)) {
                    pc_trans_fire_event(tt->client, PC_EV_UNEXPECTED_DISCONNECT, "TLS Error", NULL);
                    tt->reconn_fn(tt);
                    return ;
                } else {
                    tls->retry_wb = (char* )pc_lib_malloc(wi->buf.len);
                    memcpy(tls->retry_wb, wi->buf.base, wi->buf.len);
                    tls->retry_wb_len = tls->should_retry->buf.len;
                    break;
                }
            } else {
                // write to bio success.
                flag = 1;
            }
        }
    }
    
    // enable check timeout timer
    if (!uv_is_active((uv_handle_t* )&tt->check_timeout)) {
        uv_timer_start(&tt->check_timeout, tt->write_check_timeout_cb, 
                PC_TIMEOUT_CHECK_INTERVAL * 1000, 0);
    }
    if (flag)
        tls__write_to_tcp(tls);
}

static void tls__read_from_bio(tr_uv_tls_transport_t* tls) 
{
    int read;
    tr_uv_tcp_transport_t* tt = (tr_uv_tcp_transport_t* )tls;

    do {
        read = SSL_read(tls->tls, tls->rb, PC_TLS_READ_BUF_SIZE);
        if (read > 0) {
            pc_pkg_parser_feed(&tt->pkg_parser, tls->rb, read);
        }
    } while (read > 0);
   
    if (tls__get_error(tls->tls, read)) {
        pc_trans_fire_event(tt->client, PC_EV_UNEXPECTED_DISCONNECT, "TLS Error", NULL);
        tt->reconn_fn(tt);
    }
}

/*
 * return 0 if it is not an error
 * otherwise return 1
 */
static int tls__get_error(SSL* ssl, int status)
{
    int err;

    err = SSL_get_error(ssl, status);
    switch (err) {
        case SSL_ERROR_NONE:
        case SSL_ERROR_WANT_READ:
        case SSL_ERROR_WANT_WRITE:
            return 0;
        case SSL_ERROR_ZERO_RETURN:
            pc_lib_log(PC_LOG_WARN, "tls_get_error - tls detect shutdown, reconn");
            return 1;
        default:
            assert(err == SSL_ERROR_SSL || err == SSL_ERROR_SYSCALL);
            pc_lib_log(PC_LOG_ERROR, "tls_get_error - tls error: %s", ERR_error_string(ERR_get_error(), NULL));
            break;
    }
    return 1;
}

static void tls__write_to_tcp(tr_uv_tls_transport_t* tls)
{
    QUEUE* q;
    char* ptr;
    size_t len;
    uv_buf_t buf;
    tr_uv_tcp_transport_t* tt = (tr_uv_tcp_transport_t*)tls;

    if (tt->is_writing)
        return;

    len = BIO_pending(tls->out);

    if (len == 0) {
        assert(QUEUE_EMPTY(&tls->when_tcp_is_writing_queue));
        uv_async_send(&tt->write_async);
        return ;
    } 

    while(!QUEUE_EMPTY(&tls->when_tcp_is_writing_queue)) {
        q = QUEUE_HEAD(&tls->when_tcp_is_writing_queue);
        QUEUE_REMOVE(q);
        QUEUE_INIT(q);

        QUEUE_INSERT_TAIL(&tt->writing_queue, q);
    }

    BIO_get_mem_data(tls->out, &ptr);

    buf.base = ptr;
    buf.len = len;

    // TODO: error handling
    tt->write_req.data = tls;
    uv_write(&tt->write_req, (uv_stream_t* )&tt->socket, &buf, 1, tls__write_done_cb);
    BIO_reset(tls->out);
    tt->is_writing = 1;
}

void tls__write_done_cb(uv_write_t* w, int status)
{
    tr_uv_wi_t* wi = NULL;
    int i;
    QUEUE* q;
    GET_TLS(w);

    tt->is_writing = 0;

    if (status) {
        pc_lib_log(PC_LOG_ERROR, "tcp__write_done_cb - uv_write callback error: %s", uv_strerror(status));
    }

    status = status ? PC_RC_ERROR : PC_RC_OK;

    uv_mutex_lock(&tt->wq_mutex);
    while(!QUEUE_EMPTY(&tt->writing_queue)) {
        q = QUEUE_HEAD(&tt->writing_queue); 
        QUEUE_REMOVE(q);
        QUEUE_INIT(q);

        wi = (tr_uv_wi_t* )QUEUE_DATA(q, tr_uv_wi_t, queue);

        if (!status && TR_UV_WI_IS_RESP(wi->type)) {
            QUEUE_INSERT_TAIL(&tt->resp_pending_queue, q);
            continue;
        };

        pc_lib_free(wi->buf.base);
        wi->buf.base = NULL;
        wi->buf.len = 0;

        if (TR_UV_WI_IS_NOTIFY(wi->type)) {
            pc_trans_sent(tt->client, wi->seq_num, status);
        }

        if (TR_UV_WI_IS_RESP(wi->type)) {
            pc_trans_resp(tt->client, wi->req_id, status, NULL);
        }
        // if internal, do nothing here.

        if (PC_IS_PRE_ALLOC(wi->type)) {
            PC_PRE_ALLOC_SET_IDLE(wi->type);
        } else {
            pc_lib_free(wi);
        }
    }
    uv_mutex_unlock(&tt->wq_mutex);
    tls__write_to_tcp(tls);
}

static void tls__cycle(tr_uv_tls_transport_t* tls)
{
    tls__write_to_bio(tls);
    tls__read_from_bio(tls);
    tls__write_to_tcp(tls);
}

void tls__write_async_cb(uv_async_t* a)
{
    GET_TLS(a);

    tls__write_to_bio(tls);
}

void tls__write_timeout_check_cb(uv_timer_t* t)
{
    tr_uv_wi_t* wi = NULL;
    int cont = 0;
    time_t ct = time(0); // TODO:
    GET_TLS(t);

    wi = tls->should_retry;
    if (wi && wi->timeout != PC_WITHOUT_TIMEOUT && ct > wi->ts + wi->timeout) {
        if (TR_UV_WI_IS_NOTIFY(wi->type)) {
            pc_trans_sent(tt->client, wi->seq_num, PC_RC_TIMEOUT);
            pc_lib_log(PC_LOG_WARN, "checkout_timeout_queue - notify timeout, seq num: %u", wi->seq_num);
        } else if (TR_UV_WI_IS_RESP(wi->type)) {
            pc_trans_resp(tt->client, wi->req_id, PC_RC_TIMEOUT, NULL);
            pc_lib_log(PC_LOG_WARN, "checkout_timeout_queue - request timeout, req id: %u", wi->req_id);
        }

        // if internal, just drop it.

        pc_lib_free(wi->buf.base);
        wi->buf.base = NULL;
        wi->buf.len = 0;

        if (PC_IS_PRE_ALLOC(wi->type)) { 
            PC_PRE_ALLOC_SET_IDLE(wi->type);
        } else {
            pc_lib_free(wi);
        }
        tls->should_retry = NULL;
    }

    uv_mutex_lock(&tt->wq_mutex);
    cont = tcp__check_queue_timeout(&tls->when_tcp_is_writing_queue, tt->client, cont);
    uv_mutex_unlock(&tt->wq_mutex);

    if (cont && !uv_is_active((uv_handle_t* )t)) {
        uv_timer_start(t, tt->write_check_timeout_cb, PC_TIMEOUT_CHECK_INTERVAL* 1000, 0);
    }

    tcp__write_check_timeout_cb(t);
}

void tls__cleanup_async_cb(uv_async_t* a)
{
    GET_TLS(a);

    tcp__cleanup_async_cb(a);

    if (tls->tls) {
        SSL_free(tls->tls);
        tls->tls = NULL;
    }

    if (tls->in) {
        BIO_free(tls->in);
        tls->in = NULL;
    }

    if (tls->out) {
        BIO_free(tls->out);
        tls->out = NULL;
    }
}

void tls__on_tcp_read_cb(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) 
{
    GET_TLS(stream);

    if ( nread < 0) {
        tcp__on_tcp_read_cb(stream, nread, buf);
        return ;
    }

    BIO_write(tls->in, buf->base, nread);
    tls__cycle(tls);
}
