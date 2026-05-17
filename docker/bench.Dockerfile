FROM debian:bookworm

ARG TARGETARCH
ARG VEGETA_VERSION=12.12.0

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential ca-certificates curl git libssl-dev python3 memcached unzip \
    && rm -rf /var/lib/apt/lists/*

RUN git clone --depth 1 https://github.com/wg/wrk.git /tmp/wrk \
    && make -C /tmp/wrk \
    && cp /tmp/wrk/wrk /usr/local/bin/wrk \
    && rm -rf /tmp/wrk

RUN if [ "${TARGETARCH:-amd64}" = "amd64" ]; then \
      git clone --depth 1 https://github.com/giltene/wrk2.git /tmp/wrk2 \
      && make -C /tmp/wrk2 \
      && cp /tmp/wrk2/wrk /usr/local/bin/wrk2 \
      && rm -rf /tmp/wrk2; \
    else \
      printf '%s\n' '#!/usr/bin/env python3' 'import os, sys' 'args=[]' 'it=iter(sys.argv[1:])' 'for a in it:' '    if a == "-R": next(it, None); continue' '    if a.startswith("-R"): continue' '    args.append(a)' 'os.execvp("wrk", ["wrk"] + args)' > /usr/local/bin/wrk2 \
      && chmod +x /usr/local/bin/wrk2; \
    fi

RUN arch="${TARGETARCH:-amd64}"; \
    case "$arch" in \
      amd64) vegeta_arch=amd64 ;; \
      arm64) vegeta_arch=arm64 ;; \
      *) echo "unsupported arch: $arch" >&2; exit 1 ;; \
    esac; \
    curl -fsSL "https://github.com/tsenart/vegeta/releases/download/v${VEGETA_VERSION}/vegeta_${VEGETA_VERSION}_linux_${vegeta_arch}.tar.gz" \
      | tar -xz -C /usr/local/bin vegeta

WORKDIR /workspace
ENTRYPOINT ["python3", "benchmarks/run_bench.py"]
