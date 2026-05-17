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

## Installation

### 1. Install build dependencies

Debian/Ubuntu:

```sh
sudo apt-get update
sudo apt-get install -y build-essential ca-certificates wget libpcre3-dev zlib1g-dev libssl-dev memcached
```

RHEL/Fedora:

```sh
sudo dnf install -y gcc make wget pcre-devel zlib-devel openssl-devel memcached
```

### 2. Build dynamic module

Build against same Nginx version and compatible configure flags as target Nginx. For stock source build:

```sh
NGINX_VERSION=1.27.4
wget https://nginx.org/download/nginx-${NGINX_VERSION}.tar.gz
tar xzf nginx-${NGINX_VERSION}.tar.gz
cd nginx-${NGINX_VERSION}
./configure --with-compat --add-dynamic-module=/path/to/ngx_http_kv_module
make modules
```

Output:

```sh
objs/ngx_http_kv_module.so
```

### 3. Install module

Copy module into Nginx modules directory:

```sh
sudo cp objs/ngx_http_kv_module.so /etc/nginx/modules/
# or, for source-installed nginx:
# sudo cp objs/ngx_http_kv_module.so /usr/local/nginx/modules/
```

Load it in top-level `nginx.conf` before `events {}`:

```nginx
load_module modules/ngx_http_kv_module.so;
```

### 4. Configure location

```nginx
server {
    listen 8080;

    location /kv/ {
        kv_memcached_pass unix:/run/memcached/memcached.sock;
        kv_default_ttl 300;
        kv_max_value_size 1m;
        kv_key_prefix "app:";
    }
}
```

### 5. Start Memcached on Unix socket

```sh
sudo mkdir -p /run/memcached
sudo chown memcache:memcache /run/memcached 2>/dev/null || sudo chown nobody:nogroup /run/memcached
sudo memcached -u memcache -s /run/memcached/memcached.sock -a 770
```

Ensure Nginx worker user can access socket. If using `www-data`, make socket group readable/writable by that user/group.

### 6. Validate and reload Nginx

```sh
sudo nginx -t
sudo nginx -s reload
```

Smoke test:

```sh
curl -i -X PUT --data-binary 'hello' http://127.0.0.1:8080/kv/foo
curl -i http://127.0.0.1:8080/kv/foo
curl -i -X DELETE http://127.0.0.1:8080/kv/foo
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
