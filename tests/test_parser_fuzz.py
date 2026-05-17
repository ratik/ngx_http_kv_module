import os
import time

import pytest
import requests

FUZZ_BASE = os.environ.get("KV_FUZZ_URL")


def wait_fuzz_ready():
    if not FUZZ_BASE:
        return
    deadline = time.time() + 30
    while time.time() < deadline:
        try:
            requests.get(f"{FUZZ_BASE}/kv/__ready__", timeout=1)
            return
        except requests.RequestException:
            time.sleep(0.2)
    raise RuntimeError("fuzz nginx not ready")


@pytest.fixture(scope="session", autouse=True)
def _fuzz_ready():
    wait_fuzz_ready()


@pytest.mark.skipif(not FUZZ_BASE, reason="set KV_FUZZ_URL")
@pytest.mark.parametrize(
    "case",
    [
        "bad-empty",
        "bad-error",
        "bad-client-error",
        "bad-no-crlf",
        "bad-short-value-line",
        "bad-missing-length",
        "bad-nonnumeric-length",
        "bad-negative-length",
        "bad-wrong-key",
        "bad-garbage",
    ],
)
def test_malformed_memcached_response_returns_502(case):
    r = requests.get(f"{FUZZ_BASE}/kv/{case}", timeout=3)
    assert r.status_code == 502
    assert b"VALUE " not in r.content
    assert b"ERROR" not in r.content
    assert b"CLIENT_ERROR" not in r.content
