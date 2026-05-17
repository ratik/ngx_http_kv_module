# ngx_http_kv_module

Production-oriented Nginx HTTP dynamic module exposing a small REST KV API backed by Memcached text protocol.

## API

- `PUT /kv/<key>` -> `set <key> 0 <ttl> <bytes>` -> `204`
- `GET /kv/<key>` -> `get <key>` -> `200 application/octet-stream` or `404`
- `DELETE /kv/<key>` -> `delete <key>` -> `204` or `404`

Keys come from URI path after location prefix. Query string is ignored except `ttl=<seconds>`.

## Directives

```nginx
location /kv/ {
    kv_memcached_pass unix:/run/memcached/memcached.sock;
    kv_default_ttl 300;
    kv_max_value_size 1m;
    kv_key_prefix "app:";
    kv_allow_methods GET PUT DELETE;
    kv_not_found_status 404;
    kv_connect_timeout 2s;
    kv_send_timeout 2s;
    kv_read_timeout 2s;
}
```

## Build dynamic module

```sh
wget https://nginx.org/download/nginx-1.27.4.tar.gz
tar xzf nginx-1.27.4.tar.gz
cd nginx-1.27.4
./configure --with-compat --add-dynamic-module=/path/to/ngx_http_kv_module
make modules
```

Then load:

```nginx
load_module modules/ngx_http_kv_module.so;
```

## Memcached Unix socket

```sh
mkdir -p /run/memcached
memcached -u nobody -s /run/memcached/memcached.sock -a 777
```

## Dev/test with Docker

```sh
make test
```

Compose builds Nginx with this module, starts Memcached on Unix socket, then runs pytest integration tests.

## Implementation notes

- Uses Nginx upstream/event APIs; no blocking socket calls.
- Uses Memcached text protocol only.
- No in-process storage and no custom allocator; request state uses request pool.
- PUT uses `ngx_http_read_client_request_body` and chains Nginx body buffers/files to upstream request.
- Keys are URL-decoded and strictly rejected if empty, over 250 bytes after prefix, or containing spaces/control chars/CR/LF/NUL.
- TTL is defaulted from config and can be overridden with `?ttl=<seconds>`.

## Pragmatic score

Current: 7/10.

To reach 10/10:
- Add parser fuzz tests for malformed Memcached responses.
- Add explicit bad-backend compose profile to automate 502 case.
- Add CI matrix across Nginx versions.
- Harden GET trailer validation (`\r\nEND\r\n`) instead of discard-only.
