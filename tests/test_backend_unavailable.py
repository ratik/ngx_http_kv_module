"""
Backend-unavailable test needs nginx running with bad socket path.
Run manually or from CI by pointing KV_BASE_URL at such instance.
"""
import os
import pytest
import requests

BASE = os.environ.get("KV_BAD_BACKEND_URL")


@pytest.mark.skipif(not BASE, reason="set KV_BAD_BACKEND_URL")
def test_backend_unavailable_returns_502():
    assert requests.get(f"{BASE}/kv/missing").status_code == 502
