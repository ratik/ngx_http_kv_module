#!/bin/sh
set -eu
rm -f /run/memcached/memcached.sock
mkdir -p /run/memcached
chown nobody:nogroup /run/memcached
memcached -u nobody -s /run/memcached/memcached.sock -a 777 &
for i in $(seq 1 50); do
  [ -S /run/memcached/memcached.sock ] && break
  sleep 0.1
done
exec /usr/local/nginx/sbin/nginx -g 'daemon off;'
