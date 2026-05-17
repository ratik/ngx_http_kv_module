ARG NGINX_VERSION=1.27.4
FROM debian:bookworm AS build
ARG NGINX_VERSION
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential ca-certificates wget libpcre3-dev zlib1g-dev libssl-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /build
RUN wget -q https://nginx.org/download/nginx-${NGINX_VERSION}.tar.gz \
    && tar xzf nginx-${NGINX_VERSION}.tar.gz
COPY . /module
WORKDIR /build/nginx-${NGINX_VERSION}
RUN ./configure --prefix=/usr/local/nginx --with-compat --add-dynamic-module=/module \
    && make modules \
    && make install \
    && cp objs/ngx_http_kv_module.so /usr/local/nginx/modules/

FROM debian:bookworm
RUN apt-get update && apt-get install -y --no-install-recommends \
    memcached curl ca-certificates libpcre3 zlib1g openssl perl \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /usr/local/nginx /usr/local/nginx
COPY examples/nginx.conf /usr/local/nginx/conf/nginx.conf
COPY docker/entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh && mkdir -p /run/memcached /tmp/nginx-client-body /var/log/nginx
EXPOSE 8080
ENTRYPOINT ["/entrypoint.sh"]
