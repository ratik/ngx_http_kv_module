.PHONY: test docker-test build clean

NGINX_VERSION ?= 1.27.4

build:
	docker build --build-arg NGINX_VERSION=$(NGINX_VERSION) -f docker/Dockerfile -t ngx-http-kv-module:dev .

test:
	COMPOSE_PROFILES=bad-backend docker compose up --build --abort-on-container-exit --exit-code-from tests tests

docker-test: test

clean:
	docker compose down -v --remove-orphans
