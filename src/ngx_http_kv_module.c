#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>


typedef enum {
    NGX_HTTP_KV_OP_GET = 0,
    NGX_HTTP_KV_OP_PUT,
    NGX_HTTP_KV_OP_DELETE
} ngx_http_kv_op_e;

#define NGX_HTTP_KV_END_LEN (sizeof(ngx_http_kv_end) - 1)
static u_char ngx_http_kv_end[] = CRLF "END" CRLF;


typedef struct {
    ngx_http_upstream_conf_t   upstream;
    ngx_str_t                  backend;
    ngx_uint_t                 default_ttl;
    size_t                     max_value_size;
    ngx_str_t                  key_prefix;
    ngx_uint_t                 allow_methods;
    ngx_uint_t                 not_found_status;
} ngx_http_kv_loc_conf_t;


typedef struct {
    ngx_http_request_t        *request;
    ngx_http_kv_op_e           op;
    ngx_str_t                  key;
    ngx_uint_t                 ttl;
    off_t                      body_len;
    size_t                     value_len;
    size_t                     value_left;
    size_t                     discard_left;
    unsigned                   header_done:1;
} ngx_http_kv_ctx_t;


static ngx_int_t ngx_http_kv_handler(ngx_http_request_t *r);
static void ngx_http_kv_put_body_handler(ngx_http_request_t *r);
static ngx_int_t ngx_http_kv_start_upstream(ngx_http_request_t *r,
    ngx_http_kv_ctx_t *ctx);
static ngx_int_t ngx_http_kv_create_request(ngx_http_request_t *r);
static ngx_int_t ngx_http_kv_reinit_request(ngx_http_request_t *r);
static ngx_int_t ngx_http_kv_process_header(ngx_http_request_t *r);
static ngx_int_t ngx_http_kv_filter_init(void *data);
static ngx_int_t ngx_http_kv_filter(void *data, ssize_t bytes);
static void ngx_http_kv_abort_request(ngx_http_request_t *r);
static void ngx_http_kv_finalize_request(ngx_http_request_t *r, ngx_int_t rc);
static void *ngx_http_kv_create_loc_conf(ngx_conf_t *cf);
static char *ngx_http_kv_merge_loc_conf(ngx_conf_t *cf, void *parent,
    void *child);
static char *ngx_http_kv_pass(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static char *ngx_http_kv_allow_methods(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static ngx_int_t ngx_http_kv_parse_key(ngx_http_request_t *r,
    ngx_http_kv_ctx_t *ctx, ngx_http_kv_loc_conf_t *klcf);
static ngx_int_t ngx_http_kv_parse_ttl(ngx_http_request_t *r,
    ngx_http_kv_ctx_t *ctx, ngx_http_kv_loc_conf_t *klcf);
static ngx_int_t ngx_http_kv_body_length(ngx_http_request_t *r, off_t *len);


static ngx_command_t ngx_http_kv_commands[] = {

    { ngx_string("kv_memcached_pass"),
      NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_http_kv_pass,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("kv_default_ttl"),
      NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_num_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_kv_loc_conf_t, default_ttl),
      NULL },

    { ngx_string("kv_max_value_size"),
      NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_size_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_kv_loc_conf_t, max_value_size),
      NULL },

    { ngx_string("kv_key_prefix"),
      NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_str_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_kv_loc_conf_t, key_prefix),
      NULL },

    { ngx_string("kv_allow_methods"),
      NGX_HTTP_LOC_CONF|NGX_CONF_1MORE,
      ngx_http_kv_allow_methods,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("kv_not_found_status"),
      NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_num_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_kv_loc_conf_t, not_found_status),
      NULL },

    { ngx_string("kv_connect_timeout"),
      NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_msec_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_kv_loc_conf_t, upstream.connect_timeout),
      NULL },

    { ngx_string("kv_send_timeout"),
      NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_msec_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_kv_loc_conf_t, upstream.send_timeout),
      NULL },

    { ngx_string("kv_read_timeout"),
      NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_msec_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_kv_loc_conf_t, upstream.read_timeout),
      NULL },

    ngx_null_command
};


static ngx_http_module_t ngx_http_kv_module_ctx = {
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    ngx_http_kv_create_loc_conf,
    ngx_http_kv_merge_loc_conf
};


ngx_module_t ngx_http_kv_module = {
    NGX_MODULE_V1,
    &ngx_http_kv_module_ctx,
    ngx_http_kv_commands,
    NGX_HTTP_MODULE,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NGX_MODULE_V1_PADDING
};


static ngx_int_t
ngx_http_kv_handler(ngx_http_request_t *r)
{
    ngx_int_t                rc;
    ngx_http_kv_ctx_t      *ctx;
    ngx_http_kv_loc_conf_t *klcf;

    klcf = ngx_http_get_module_loc_conf(r, ngx_http_kv_module);

    if (klcf->upstream.upstream == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    if (r->method == NGX_HTTP_GET) {
        if (!(klcf->allow_methods & NGX_HTTP_GET)) {
            return NGX_HTTP_NOT_ALLOWED;
        }
    } else if (r->method == NGX_HTTP_PUT) {
        if (!(klcf->allow_methods & NGX_HTTP_PUT)) {
            return NGX_HTTP_NOT_ALLOWED;
        }
    } else if (r->method == NGX_HTTP_DELETE) {
        if (!(klcf->allow_methods & NGX_HTTP_DELETE)) {
            return NGX_HTTP_NOT_ALLOWED;
        }
    } else {
        return NGX_HTTP_NOT_ALLOWED;
    }

    ctx = ngx_pcalloc(r->pool, sizeof(ngx_http_kv_ctx_t));
    if (ctx == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    ctx->request = r;
    ctx->op = (r->method == NGX_HTTP_GET) ? NGX_HTTP_KV_OP_GET:
              (r->method == NGX_HTTP_PUT) ? NGX_HTTP_KV_OP_PUT:
                                            NGX_HTTP_KV_OP_DELETE;
    ngx_http_set_ctx(r, ctx, ngx_http_kv_module);

    if (ngx_http_kv_parse_key(r, ctx, klcf) != NGX_OK) {
        return NGX_HTTP_BAD_REQUEST;
    }

    if (ngx_http_kv_parse_ttl(r, ctx, klcf) != NGX_OK) {
        return NGX_HTTP_BAD_REQUEST;
    }

    if (ctx->op != NGX_HTTP_KV_OP_PUT) {
        rc = ngx_http_discard_request_body(r);
        if (rc != NGX_OK) {
            return rc;
        }
        return ngx_http_kv_start_upstream(r, ctx);
    }

    if (r->headers_in.content_length_n == 0) {
        return NGX_HTTP_BAD_REQUEST;
    }

    if (r->headers_in.content_length_n > (off_t) klcf->max_value_size) {
        return NGX_HTTP_REQUEST_ENTITY_TOO_LARGE;
    }

    r->request_body_in_single_buf = 0;
    r->request_body_in_persistent_file = 0;
    r->request_body_in_clean_file = 1;

    rc = ngx_http_read_client_request_body(r, ngx_http_kv_put_body_handler);
    if (rc >= NGX_HTTP_SPECIAL_RESPONSE) {
        return rc;
    }

    return NGX_DONE;
}


static void
ngx_http_kv_put_body_handler(ngx_http_request_t *r)
{
    off_t                    len;
    ngx_http_kv_ctx_t      *ctx;
    ngx_http_kv_loc_conf_t *klcf;

    ctx = ngx_http_get_module_ctx(r, ngx_http_kv_module);
    klcf = ngx_http_get_module_loc_conf(r, ngx_http_kv_module);

    if (ctx == NULL || klcf == NULL) {
        ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
        return;
    }

    if (ngx_http_kv_body_length(r, &len) != NGX_OK || len <= 0) {
        ngx_http_finalize_request(r, NGX_HTTP_BAD_REQUEST);
        return;
    }

    if (len > (off_t) klcf->max_value_size) {
        ngx_http_finalize_request(r, NGX_HTTP_REQUEST_ENTITY_TOO_LARGE);
        return;
    }

    ctx->body_len = len;

    if (ngx_http_kv_start_upstream(r, ctx) == NGX_DONE) {
        return;
    }

    ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
}


static ngx_int_t
ngx_http_kv_start_upstream(ngx_http_request_t *r, ngx_http_kv_ctx_t *ctx)
{
    ngx_http_upstream_t     *u;
    ngx_http_kv_loc_conf_t *klcf;

    if (ngx_http_upstream_create(r) != NGX_OK) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    u = r->upstream;
    klcf = ngx_http_get_module_loc_conf(r, ngx_http_kv_module);

    ngx_str_set(&u->schema, "memcached://");
    u->output.tag = (ngx_buf_tag_t) &ngx_http_kv_module;
    u->conf = &klcf->upstream;

    u->create_request = ngx_http_kv_create_request;
    u->reinit_request = ngx_http_kv_reinit_request;
    u->process_header = ngx_http_kv_process_header;
    u->abort_request = ngx_http_kv_abort_request;
    u->finalize_request = ngx_http_kv_finalize_request;
    u->input_filter_init = ngx_http_kv_filter_init;
    u->input_filter = ngx_http_kv_filter;
    u->input_filter_ctx = ctx;

    r->main->count++;
    ngx_http_upstream_init(r);

    return NGX_DONE;
}


static ngx_int_t
ngx_http_kv_create_request(ngx_http_request_t *r)
{
    size_t              len;
    ngx_buf_t          *b, *tail;
    ngx_chain_t        *cl, *last, *body, *tl;
    ngx_http_kv_ctx_t  *ctx;

    ctx = ngx_http_get_module_ctx(r, ngx_http_kv_module);
    if (ctx == NULL) {
        return NGX_ERROR;
    }

    if (ctx->op == NGX_HTTP_KV_OP_GET) {
        len = sizeof("get ") - 1 + ctx->key.len + sizeof(CRLF) - 1;
    } else if (ctx->op == NGX_HTTP_KV_OP_DELETE) {
        len = sizeof("delete ") - 1 + ctx->key.len + sizeof(CRLF) - 1;
    } else {
        len = sizeof("set ") - 1 + ctx->key.len + sizeof(" 0 ") - 1
              + NGX_INT_T_LEN + 1 + NGX_OFF_T_LEN + sizeof(CRLF) - 1;
    }

    b = ngx_create_temp_buf(r->pool, len);
    if (b == NULL) {
        return NGX_ERROR;
    }

    if (ctx->op == NGX_HTTP_KV_OP_GET) {
        b->last = ngx_cpymem(b->last, "get ", sizeof("get ") - 1);
        b->last = ngx_copy(b->last, ctx->key.data, ctx->key.len);
        *b->last++ = CR; *b->last++ = LF;
    } else if (ctx->op == NGX_HTTP_KV_OP_DELETE) {
        b->last = ngx_cpymem(b->last, "delete ", sizeof("delete ") - 1);
        b->last = ngx_copy(b->last, ctx->key.data, ctx->key.len);
        *b->last++ = CR; *b->last++ = LF;
    } else {
        b->last = ngx_sprintf(b->last, "set %V 0 %ui %O" CRLF,
                              &ctx->key, ctx->ttl, ctx->body_len);
    }

    cl = ngx_alloc_chain_link(r->pool);
    if (cl == NULL) {
        return NGX_ERROR;
    }
    cl->buf = b;
    cl->next = NULL;
    last = cl;

    if (ctx->op == NGX_HTTP_KV_OP_PUT) {
        for (body = r->request_body->bufs; body; body = body->next) {
            tl = ngx_alloc_chain_link(r->pool);
            if (tl == NULL) {
                return NGX_ERROR;
            }
            tl->buf = body->buf;
            tl->next = NULL;
            last->next = tl;
            last = tl;
        }

        tail = ngx_create_temp_buf(r->pool, sizeof(CRLF) - 1);
        if (tail == NULL) {
            return NGX_ERROR;
        }
        *tail->last++ = CR; *tail->last++ = LF;

        tl = ngx_alloc_chain_link(r->pool);
        if (tl == NULL) {
            return NGX_ERROR;
        }
        tl->buf = tail;
        tl->next = NULL;
        last->next = tl;
    }

    r->upstream->request_bufs = cl;

    ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "kv memcached op:%ui key:%V", ctx->op, &ctx->key);

    return NGX_OK;
}


static ngx_int_t
ngx_http_kv_reinit_request(ngx_http_request_t *r)
{
    ngx_http_kv_ctx_t *ctx = ngx_http_get_module_ctx(r, ngx_http_kv_module);
    if (ctx) {
        ctx->value_left = 0;
        ctx->discard_left = 0;
        ctx->header_done = 0;
    }
    return NGX_OK;
}


static ngx_int_t
ngx_http_kv_process_header(ngx_http_request_t *r)
{
    u_char                  *p, *start, *sp, *end;
    ngx_int_t                n;
    ngx_str_t                line;
    ngx_http_upstream_t     *u;
    ngx_http_kv_ctx_t       *ctx;
    ngx_http_kv_loc_conf_t  *klcf;

    u = r->upstream;
    ctx = ngx_http_get_module_ctx(r, ngx_http_kv_module);
    klcf = ngx_http_get_module_loc_conf(r, ngx_http_kv_module);

    for (p = u->buffer.pos; p < u->buffer.last; p++) {
        if (*p == LF) {
            goto found;
        }
    }

    return NGX_AGAIN;

found:
    start = u->buffer.pos;
    end = p;
    if (end > start && end[-1] == CR) {
        end--;
    }

    line.data = start;
    line.len = end - start;
    u->buffer.pos = p + 1;

    if (ctx->op == NGX_HTTP_KV_OP_GET) {
        if (line.len == sizeof("END") - 1
            && ngx_strncmp(line.data, "END", sizeof("END") - 1) == 0)
        {
            u->headers_in.status_n = klcf->not_found_status;
            u->headers_in.content_length_n = 0;
            r->headers_out.status = klcf->not_found_status;
            r->headers_out.content_length_n = 0;
            return NGX_OK;
        }

        if (line.len < sizeof("VALUE  0 0") - 1
            || ngx_strncmp(line.data, "VALUE ", sizeof("VALUE ") - 1) != 0)
        {
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                          "memcached invalid get response: \"%V\"", &line);
            return NGX_HTTP_UPSTREAM_INVALID_HEADER;
        }

        sp = line.data + sizeof("VALUE ") - 1;
        while (sp < end && *sp != ' ') { sp++; }
        if (sp == end) { return NGX_HTTP_UPSTREAM_INVALID_HEADER; }
        sp++;
        while (sp < end && *sp != ' ') { sp++; }
        if (sp == end) { return NGX_HTTP_UPSTREAM_INVALID_HEADER; }
        sp++;

        n = ngx_atoi(sp, end - sp);
        if (n < 0) {
            return NGX_HTTP_UPSTREAM_INVALID_HEADER;
        }

        ctx->value_len = (size_t) n;
        ctx->value_left = (size_t) n;
        ctx->discard_left = NGX_HTTP_KV_END_LEN;

        u->headers_in.status_n = NGX_HTTP_OK;
        u->state->status = NGX_HTTP_OK;
        u->headers_in.content_length_n = n;
        r->headers_out.status = NGX_HTTP_OK;
        r->headers_out.content_length_n = n;
        ngx_str_set(&r->headers_out.content_type, "application/octet-stream");
        return NGX_OK;
    }

    if (ctx->op == NGX_HTTP_KV_OP_PUT) {
        if (line.len == sizeof("STORED") - 1
            && ngx_strncmp(line.data, "STORED", sizeof("STORED") - 1) == 0)
        {
            u->headers_in.status_n = NGX_HTTP_NO_CONTENT;
            u->headers_in.content_length_n = 0;
            r->headers_out.status = NGX_HTTP_NO_CONTENT;
            r->headers_out.content_length_n = 0;
            return NGX_OK;
        }
        return NGX_HTTP_UPSTREAM_INVALID_HEADER;
    }

    if (line.len == sizeof("DELETED") - 1
        && ngx_strncmp(line.data, "DELETED", sizeof("DELETED") - 1) == 0)
    {
        u->headers_in.status_n = NGX_HTTP_NO_CONTENT;
        u->headers_in.content_length_n = 0;
        r->headers_out.status = NGX_HTTP_NO_CONTENT;
        r->headers_out.content_length_n = 0;
        return NGX_OK;
    }

    if (line.len == sizeof("NOT_FOUND") - 1
        && ngx_strncmp(line.data, "NOT_FOUND", sizeof("NOT_FOUND") - 1) == 0)
    {
        u->headers_in.status_n = klcf->not_found_status;
        u->headers_in.content_length_n = 0;
        r->headers_out.status = klcf->not_found_status;
        r->headers_out.content_length_n = 0;
        return NGX_OK;
    }

    return NGX_HTTP_UPSTREAM_INVALID_HEADER;
}


static ngx_int_t
ngx_http_kv_filter_init(void *data)
{
    ngx_http_kv_ctx_t   *ctx = data;
    ngx_http_upstream_t *u;

    if (ctx == NULL || ctx->request == NULL) {
        return NGX_ERROR;
    }

    u = ctx->request->upstream;

    if (ctx->op == NGX_HTTP_KV_OP_GET && u->headers_in.status_n == NGX_HTTP_OK) {
        u->length = u->headers_in.content_length_n + NGX_HTTP_KV_END_LEN;
        ctx->discard_left = NGX_HTTP_KV_END_LEN;
    } else {
        u->length = 0;
    }

    return NGX_OK;
}


static ngx_int_t
ngx_http_kv_filter(void *data, ssize_t bytes)
{
    ngx_http_kv_ctx_t    *ctx = data;
    ngx_http_request_t   *r;
    u_char               *last;
    ngx_buf_t            *b;
    ngx_chain_t          *cl, **ll;
    ngx_http_upstream_t  *u;
    size_t                value_part, trailer_part;

    if (ctx == NULL || ctx->request == NULL || bytes == 0) {
        return NGX_OK;
    }

    r = ctx->request;
    u = r->upstream;
    b = &u->buffer;

    if (ctx->op != NGX_HTTP_KV_OP_GET || u->headers_in.status_n != NGX_HTTP_OK) {
        b->last += bytes;
        b->pos = b->last;
        return NGX_OK;
    }

    for (cl = u->out_bufs, ll = &u->out_bufs; cl; cl = cl->next) {
        ll = &cl->next;
    }

    if (ctx->value_left > 0) {
        value_part = ngx_min((size_t) bytes, ctx->value_left);

        cl = ngx_chain_get_free_buf(r->pool, &u->free_bufs);
        if (cl == NULL) {
            return NGX_ERROR;
        }

        cl->buf->flush = 1;
        cl->buf->memory = 1;
        cl->buf->pos = b->last;
        cl->buf->last = b->last + value_part;
        cl->buf->tag = u->output.tag;
        cl->next = NULL;
        *ll = cl;

        ctx->value_left -= value_part;
        u->length -= value_part;
        bytes -= value_part;
        b->last += value_part;
    }

    if (bytes > 0 && ctx->value_left == 0) {
        last = b->last;
        trailer_part = ngx_min((size_t) bytes, ctx->discard_left);

        if (ngx_strncmp(last,
                        ngx_http_kv_end + NGX_HTTP_KV_END_LEN - ctx->discard_left,
                        trailer_part) != 0)
        {
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                          "memcached sent invalid trailer");
            u->length = 0;
            ctx->discard_left = 0;
            b->last += bytes;
            b->pos = b->last;
            return NGX_OK;
        }

        ctx->discard_left -= trailer_part;
        u->length -= trailer_part;
        b->last += trailer_part;

        if (ctx->discard_left == 0) {
            u->keepalive = 1;
        }
    }

    b->pos = b->last;
    return NGX_OK;
}


static void
ngx_http_kv_abort_request(ngx_http_request_t *r)
{
    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "abort kv upstream request");
}


static void
ngx_http_kv_finalize_request(ngx_http_request_t *r, ngx_int_t rc)
{
    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "finalize kv upstream request: %i", rc);
}


static void *
ngx_http_kv_create_loc_conf(ngx_conf_t *cf)
{
    ngx_http_kv_loc_conf_t *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_kv_loc_conf_t));
    if (conf == NULL) {
        return NULL;
    }

    conf->default_ttl = NGX_CONF_UNSET_UINT;
    conf->max_value_size = NGX_CONF_UNSET_SIZE;
    conf->allow_methods = NGX_CONF_UNSET_UINT;
    conf->not_found_status = NGX_CONF_UNSET_UINT;

    conf->upstream.connect_timeout = NGX_CONF_UNSET_MSEC;
    conf->upstream.send_timeout = NGX_CONF_UNSET_MSEC;
    conf->upstream.read_timeout = NGX_CONF_UNSET_MSEC;
    conf->upstream.buffer_size = NGX_CONF_UNSET_SIZE;

    conf->upstream.next_upstream = NGX_CONF_UNSET_UINT;
    conf->upstream.next_upstream_tries = NGX_CONF_UNSET_UINT;
    conf->upstream.next_upstream_timeout = NGX_CONF_UNSET_MSEC;

    return conf;
}


static char *
ngx_http_kv_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_kv_loc_conf_t *prev = parent;
    ngx_http_kv_loc_conf_t *conf = child;

    ngx_conf_merge_uint_value(conf->default_ttl, prev->default_ttl, 0);
    ngx_conf_merge_size_value(conf->max_value_size, prev->max_value_size,
                              1024 * 1024);
    ngx_conf_merge_str_value(conf->key_prefix, prev->key_prefix, "");
    ngx_conf_merge_uint_value(conf->allow_methods, prev->allow_methods,
                              NGX_HTTP_GET|NGX_HTTP_PUT|NGX_HTTP_DELETE);
    ngx_conf_merge_uint_value(conf->not_found_status, prev->not_found_status,
                              NGX_HTTP_NOT_FOUND);

    ngx_conf_merge_msec_value(conf->upstream.connect_timeout,
                              prev->upstream.connect_timeout, 60000);
    ngx_conf_merge_msec_value(conf->upstream.send_timeout,
                              prev->upstream.send_timeout, 60000);
    ngx_conf_merge_msec_value(conf->upstream.read_timeout,
                              prev->upstream.read_timeout, 60000);
    ngx_conf_merge_size_value(conf->upstream.buffer_size,
                              prev->upstream.buffer_size,
                              (size_t) ngx_pagesize);
    ngx_conf_merge_uint_value(conf->upstream.next_upstream,
                              prev->upstream.next_upstream,
                              NGX_HTTP_UPSTREAM_FT_ERROR
                              |NGX_HTTP_UPSTREAM_FT_TIMEOUT);
    ngx_conf_merge_uint_value(conf->upstream.next_upstream_tries,
                              prev->upstream.next_upstream_tries, 1);
    ngx_conf_merge_msec_value(conf->upstream.next_upstream_timeout,
                              prev->upstream.next_upstream_timeout, 0);

    conf->upstream.pass_request_headers = 0;
    conf->upstream.pass_request_body = 1;
    conf->upstream.intercept_errors = 1;

    if (conf->upstream.upstream == NULL) {
        conf->upstream.upstream = prev->upstream.upstream;
    }

    return NGX_CONF_OK;
}


static char *
ngx_http_kv_pass(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_kv_loc_conf_t *klcf = conf;
    ngx_str_t             *value, url;
    ngx_url_t              u;
    ngx_http_core_loc_conf_t *clcf;

    if (klcf->upstream.upstream) {
        return "is duplicate";
    }

    value = cf->args->elts;
    url = value[1];

    ngx_memzero(&u, sizeof(ngx_url_t));
    u.url = url;
    u.no_resolve = 1;

    klcf->upstream.upstream = ngx_http_upstream_add(cf, &u, 0);
    if (klcf->upstream.upstream == NULL) {
        return NGX_CONF_ERROR;
    }

    klcf->backend = url;

    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
    clcf->handler = ngx_http_kv_handler;

    return NGX_CONF_OK;
}


static char *
ngx_http_kv_allow_methods(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_kv_loc_conf_t *klcf = conf;
    ngx_str_t             *value;
    ngx_uint_t             i, methods = 0;

    value = cf->args->elts;

    for (i = 1; i < cf->args->nelts; i++) {
        if (value[i].len == 3 && ngx_strncmp(value[i].data, "GET", 3) == 0) {
            methods |= NGX_HTTP_GET;
        } else if (value[i].len == 3 && ngx_strncmp(value[i].data, "PUT", 3) == 0) {
            methods |= NGX_HTTP_PUT;
        } else if (value[i].len == 6 && ngx_strncmp(value[i].data, "DELETE", 6) == 0) {
            methods |= NGX_HTTP_DELETE;
        } else {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "invalid method \"%V\"", &value[i]);
            return NGX_CONF_ERROR;
        }
    }

    klcf->allow_methods = methods;
    return NGX_CONF_OK;
}


static ngx_int_t
ngx_http_kv_parse_key(ngx_http_request_t *r, ngx_http_kv_ctx_t *ctx,
    ngx_http_kv_loc_conf_t *klcf)
{
    size_t                    raw_len, dst_len, i;
    u_char                   *src, *dst, *p;
    ngx_http_core_loc_conf_t *clcf;

    clcf = ngx_http_get_module_loc_conf(r, ngx_http_core_module);

    if (r->uri.len < clcf->name.len) {
        return NGX_ERROR;
    }

    src = r->uri.data + clcf->name.len;
    raw_len = r->uri.len - clcf->name.len;

    if (raw_len == 0) {
        return NGX_ERROR;
    }

    dst = ngx_pnalloc(r->pool, klcf->key_prefix.len + raw_len);
    if (dst == NULL) {
        return NGX_ERROR;
    }

    p = ngx_copy(dst, klcf->key_prefix.data, klcf->key_prefix.len);
    dst_len = raw_len;
    ngx_unescape_uri(&p, &src, raw_len, 0);
    dst_len = p - (dst + klcf->key_prefix.len);

    ctx->key.data = dst;
    ctx->key.len = klcf->key_prefix.len + dst_len;

    if (ctx->key.len == 0 || ctx->key.len > 250) {
        return NGX_ERROR;
    }

    for (i = 0; i < ctx->key.len; i++) {
        if (ctx->key.data[i] <= 0x20 || ctx->key.data[i] == 0x7f) {
            return NGX_ERROR;
        }
    }

    return NGX_OK;
}


static ngx_int_t
ngx_http_kv_parse_ttl(ngx_http_request_t *r, ngx_http_kv_ctx_t *ctx,
    ngx_http_kv_loc_conf_t *klcf)
{
    ngx_str_t  v;
    ngx_int_t  n;

    ctx->ttl = klcf->default_ttl;

    if (ngx_http_arg(r, (u_char *) "ttl", 3, &v) != NGX_OK) {
        return NGX_OK;
    }

    if (v.len == 0 || v.len > 10) {
        return NGX_ERROR;
    }

    n = ngx_atoi(v.data, v.len);
    if (n < 0) {
        return NGX_ERROR;
    }

    ctx->ttl = (ngx_uint_t) n;
    return NGX_OK;
}


static ngx_int_t
ngx_http_kv_body_length(ngx_http_request_t *r, off_t *len)
{
    ngx_chain_t *cl;
    ngx_buf_t   *b;

    *len = 0;

    if (r->request_body == NULL || r->request_body->bufs == NULL) {
        return NGX_ERROR;
    }

    for (cl = r->request_body->bufs; cl; cl = cl->next) {
        b = cl->buf;
        if (ngx_buf_in_memory(b)) {
            *len += b->last - b->pos;
        }
        if (b->in_file) {
            *len += b->file_last - b->file_pos;
        }
    }

    return NGX_OK;
}
