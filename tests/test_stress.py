import concurrent.futures
import hashlib
import os
import threading
import time
import uuid

import pytest
import requests

BASE = os.environ.get("KV_BASE_URL", "http://127.0.0.1:8080")
STRESS_ITEMS = int(os.environ.get("KV_STRESS_ITEMS", "16"))
STRESS_WORKERS = int(os.environ.get("KV_STRESS_WORKERS", "8"))
LARGE_VALUE_SIZE = int(os.environ.get("KV_STRESS_VALUE_SIZE", str(512 * 1024)))
REQUEST_TIMEOUT = float(os.environ.get("KV_STRESS_TIMEOUT", "20"))


def key():
    return f"stress-{uuid.uuid4().hex}"


def url(k):
    return f"{BASE}/kv/{k}"


def wait_ready():
    deadline = time.time() + 30
    while time.time() < deadline:
        try:
            requests.get(url("__stress_ready__"), timeout=1)
            return
        except requests.RequestException:
            time.sleep(0.2)
    raise RuntimeError("nginx not ready")


@pytest.fixture(scope="session", autouse=True)
def _ready():
    wait_ready()


def payload(seed, size=LARGE_VALUE_SIZE):
    out = bytearray()
    counter = 0
    while len(out) < size:
        out.extend(hashlib.sha256(f"{seed}:{counter}".encode()).digest())
        counter += 1
    return bytes(out[:size])


def test_concurrent_large_put_get_delete_roundtrips():
    """Exercise concurrent large request bodies, large responses, and deletes."""
    items = [(key(), payload(i)) for i in range(STRESS_ITEMS)]
    start = threading.Event()

    def roundtrip(item):
        k, value = item
        start.wait()

        put = requests.put(url(k) + "?ttl=300", data=value, timeout=REQUEST_TIMEOUT)
        if put.status_code != 204:
            return (k, "put", put.status_code, len(value), None)

        get = requests.get(url(k), timeout=REQUEST_TIMEOUT)
        if get.status_code != 200 or get.content != value:
            return (k, "get", get.status_code, len(value), len(get.content))

        delete = requests.delete(url(k), timeout=REQUEST_TIMEOUT)
        if delete.status_code != 204:
            return (k, "delete", delete.status_code, len(value), None)

        missing = requests.get(url(k), timeout=REQUEST_TIMEOUT)
        if missing.status_code != 404:
            return (k, "missing", missing.status_code, len(value), len(missing.content))

        return None

    with concurrent.futures.ThreadPoolExecutor(max_workers=STRESS_WORKERS) as ex:
        futures = [ex.submit(roundtrip, item) for item in items]
        start.set()
        failures = [f.result() for f in futures]

    assert [f for f in failures if f is not None] == []


def test_concurrent_large_mixed_method_load():
    """Run overlapping PUT/GET/DELETE operations on large values without response corruption."""
    items = [(key(), payload(f"mixed-{i}", LARGE_VALUE_SIZE // 2)) for i in range(max(4, STRESS_ITEMS // 2))]

    for k, value in items:
        assert requests.put(url(k) + "?ttl=300", data=value, timeout=REQUEST_TIMEOUT).status_code == 204

    start = threading.Event()

    def cycle(index):
        k, value = items[index % len(items)]
        replacement = payload(f"replacement-{index}", LARGE_VALUE_SIZE // 2)
        start.wait()

        # Unique replacement key avoids race-sensitive assertions while still mixing methods concurrently.
        rk = f"{k}-{index}"
        put = requests.put(url(rk) + "?ttl=300", data=replacement, timeout=REQUEST_TIMEOUT)
        get_existing = requests.get(url(k), timeout=REQUEST_TIMEOUT)
        delete = requests.delete(url(rk), timeout=REQUEST_TIMEOUT)
        get_deleted = requests.get(url(rk), timeout=REQUEST_TIMEOUT)

        return {
            "put": put.status_code,
            "get_existing": get_existing.status_code,
            "get_existing_ok": get_existing.content == value,
            "delete": delete.status_code,
            "get_deleted": get_deleted.status_code,
        }

    operations = STRESS_ITEMS * 2
    with concurrent.futures.ThreadPoolExecutor(max_workers=STRESS_WORKERS) as ex:
        futures = [ex.submit(cycle, i) for i in range(operations)]
        start.set()
        results = [f.result() for f in futures]

    assert all(r == {
        "put": 204,
        "get_existing": 200,
        "get_existing_ok": True,
        "delete": 204,
        "get_deleted": 404,
    } for r in results)
