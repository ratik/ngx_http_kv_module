.PHONY: test docker-test sanitizer-test bench bench-compare bench-update-baseline build clean

NGINX_VERSION ?= 1.27.4
SANITIZERS ?= 0

build:
	docker build --build-arg NGINX_VERSION=$(NGINX_VERSION) --build-arg SANITIZERS=$(SANITIZERS) -f docker/Dockerfile -t ngx-http-kv-module:dev .

test:
	NGINX_VERSION=$(NGINX_VERSION) SANITIZERS=$(SANITIZERS) COMPOSE_PROFILES=bad-backend docker compose up --build --abort-on-container-exit --exit-code-from tests tests

sanitizer-test:
	NGINX_VERSION=$(NGINX_VERSION) SANITIZERS=1 COMPOSE_PROFILES=bad-backend docker compose up --build --abort-on-container-exit --exit-code-from tests tests

bench:
	NGINX_VERSION=$(NGINX_VERSION) COMPOSE_PROFILES=bench docker compose up --build --abort-on-container-exit --exit-code-from bench bench

bench-compare:
	COMPOSE_PROFILES=bench docker compose run --rm bench-compare

bench-update-baseline:
	COMPOSE_PROFILES=bench docker compose run --rm bench-update-baseline

docker-test: test

clean:
	docker compose down -v --remove-orphans
