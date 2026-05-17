import concurrent.futures
import os
import time
import uuid

import pytest
import requests

BASE = os.environ.get("KV_BASE_URL", "http://127.0.0.1:8080")


def key(name=None):
    return f"t-{uuid.uuid4().hex}" if name is None else name


def url(k):
    return f"{BASE}/kv/{k}"


def wait_ready():
    deadline = time.time() + 30
    while time.time() < deadline:
        try:
            requests.get(url("__ready__"), timeout=1)
            return
        except requests.RequestException:
            time.sleep(0.2)
    raise RuntimeError("nginx not ready")


@pytest.fixture(scope="session", autouse=True)
def _ready():
    wait_ready()


def test_put_then_get_same_value():
    k = key()
    value = b"hello world"
    assert requests.put(url(k), data=value).status_code == 204
    r = requests.get(url(k))
    assert r.status_code == 200
    assert r.content == value
    assert r.headers.get("content-type", "").startswith("application/octet-stream")


def test_get_missing_returns_404():
    assert requests.get(url(key())).status_code == 404


def test_delete_existing_returns_204():
    k = key()
    requests.put(url(k), data=b"x")
    assert requests.delete(url(k)).status_code == 204
    assert requests.get(url(k)).status_code == 404


def test_delete_missing_returns_404():
    assert requests.delete(url(key())).status_code == 404


def test_invalid_method_returns_405():
    assert requests.post(url(key()), data=b"x").status_code == 405


def test_empty_key_returns_400():
    assert requests.get(f"{BASE}/kv/").status_code == 400


@pytest.mark.parametrize("bad", ["has%20space", "has%0acrlf", "has%0dcr", "nul%00byte", "tab%09x"])
def test_bad_key_rejected(bad):
    assert requests.get(url(bad)).status_code == 400


def test_long_key_rejected():
    # config prefix is app:, so 247 visible bytes gives 251 total.
    assert requests.get(url("x" * 247)).status_code == 400


def test_body_larger_than_max_returns_413():
    assert requests.put(url(key()), data=b"x" * (1024 * 1024 + 1)).status_code == 413


def test_ttl_expiration_works():
    k = key()
    assert requests.put(url(k) + "?ttl=1", data=b"gone").status_code == 204
    assert requests.get(url(k)).status_code == 200
    time.sleep(1.3)
    assert requests.get(url(k)).status_code == 404


def test_ttl_query_override_works():
    k = key()
    assert requests.put(url(k) + "?ttl=0", data=b"stay").status_code == 204
    time.sleep(1.2)
    r = requests.get(url(k))
    assert r.status_code == 200
    assert r.content == b"stay"


@pytest.mark.parametrize("ttl", ["", "abc", "-1", "1.5", "99999999999"])
def test_invalid_ttl_returns_400(ttl):
    assert requests.put(url(key()) + f"?ttl={ttl}", data=b"x").status_code == 400


def test_binary_payload_preserved():
    k = key()
    payload = bytes([0, 1, 2, 10, 13, 255]) + b"middle\x00end"
    assert requests.put(url(k), data=payload).status_code == 204
    r = requests.get(url(k))
    assert r.status_code == 200
    assert r.content == payload


def test_multiple_sequential_requests_work():
    for i in range(20):
        k = key()
        v = f"value-{i}".encode()
        assert requests.put(url(k), data=v).status_code == 204
        assert requests.get(url(k)).content == v


def test_concurrent_requests_do_not_corrupt_responses():
    items = [(key(), os.urandom(256)) for _ in range(50)]

    def roundtrip(item):
        k, v = item
        pr = requests.put(url(k), data=v, timeout=5)
        gr = requests.get(url(k), timeout=5)
        return pr.status_code, gr.status_code, gr.content == v

    with concurrent.futures.ThreadPoolExecutor(max_workers=16) as ex:
        results = list(ex.map(roundtrip, items))

    assert all(r == (204, 200, True) for r in results)
