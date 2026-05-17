#!/usr/bin/env python3
import argparse
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path
from urllib import request, error

ROOT = Path(__file__).resolve().parents[1]
BENCH = ROOT / "benchmarks"
SCRIPTS = BENCH / "scripts"
RESULTS = BENCH / "results"
BASELINE = BENCH / "baseline" / "main.json"

DEFAULT_BASE = os.environ.get("KV_BENCH_URL", "http://kv-nginx:8080")

BENCHMARKS = [
    {
        "name": "wrk-get-existing",
        "tool": "wrk",
        "cmd": ["wrk", "-t2", "-c32", "-d15s", "--latency", "-s", str(SCRIPTS / "wrk-get.lua"), DEFAULT_BASE],
    },
    {
        "name": "wrk-put-small",
        "tool": "wrk",
        "cmd": ["wrk", "-t2", "-c16", "-d15s", "--latency", "-s", str(SCRIPTS / "wrk-put.lua"), DEFAULT_BASE],
    },
    {
        "name": "wrk2-get-existing",
        "tool": "wrk2",
        "cmd": ["wrk2", "-t2", "-c32", "-d15s", "-R1000", "--latency", "-s", str(SCRIPTS / "wrk2-get.lua"), DEFAULT_BASE],
        "binary": "wrk2",
    },
    {
        "name": "vegeta-get-existing",
        "tool": "vegeta",
        "cmd": ["sh", "-lc", "vegeta attack -duration=15s -rate=200 -targets={targets} | vegeta report -type=json"],
    },
]


def run(cmd, check=False):
    p = subprocess.run(cmd, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    if check and p.returncode != 0:
        raise RuntimeError(f"command failed {cmd}:\n{p.stdout}\n{p.stderr}")
    return p


def git_sha():
    p = run(["git", "-c", f"safe.directory={ROOT}", "rev-parse", "--short=12", "HEAD"])
    return p.stdout.strip() if p.returncode == 0 else "unknown"


def full_git_sha():
    p = run(["git", "-c", f"safe.directory={ROOT}", "rev-parse", "HEAD"])
    return p.stdout.strip() if p.returncode == 0 else "unknown"


def http(method, url, data=None, timeout=5):
    req = request.Request(url, data=data, method=method)
    if data is not None:
        req.add_header("Content-Type", "application/octet-stream")
    try:
        with request.urlopen(req, timeout=timeout) as r:
            return r.status, r.read()
    except error.HTTPError as e:
        return e.code, e.read()


def wait_ready(base):
    deadline = time.time() + 60
    while time.time() < deadline:
        try:
            http("GET", f"{base}/kv/__bench_ready__", timeout=1)
            return
        except Exception:
            time.sleep(0.25)
    raise RuntimeError("bench target not ready")


def seed(base):
    status, _ = http("PUT", f"{base}/kv/bench-existing?ttl=300", b"benchmark-value")
    if status != 204:
        raise RuntimeError(f"seed failed: HTTP {status}")


def parse_num(s):
    s = s.strip()
    mult = 1.0
    if s.endswith("k") or s.endswith("K"):
        mult, s = 1_000.0, s[:-1]
    elif s.endswith("M"):
        mult, s = 1_000_000.0, s[:-1]
    return float(s) * mult


def parse_latency_ms(s):
    s = s.strip()
    m = re.match(r"([0-9.]+)(us|ms|s)$", s)
    if not m:
        return None
    v, unit = float(m.group(1)), m.group(2)
    return v / 1000.0 if unit == "us" else v * 1000.0 if unit == "s" else v


def parse_wrk(out, rc):
    rps = None
    p99 = None
    errors = 0
    requests_total = None
    timeouts = 0
    m = re.search(r"Requests/sec:\s*([0-9.]+)", out)
    if m:
        rps = float(m.group(1))
    m = re.search(r"(\d+) requests in", out)
    if m:
        requests_total = int(m.group(1))
    m = re.search(r"99%\s+([0-9.]+(?:us|ms|s))", out)
    if m:
        p99 = parse_latency_ms(m.group(1))
    m = re.search(r"Socket errors: connect (\d+), read (\d+), write (\d+), timeout (\d+)", out)
    if m:
        nums = [int(x) for x in m.groups()]
        errors += sum(nums)
        timeouts += nums[3]
    m = re.search(r"Non-2xx or 3xx responses: (\d+)", out)
    if m:
        errors += int(m.group(1))
    error_rate = (errors / requests_total) if requests_total else (1.0 if rc else 0.0)
    return {"rps": rps, "p99_ms": p99, "errors": errors, "error_rate": error_rate, "timeouts": timeouts}


def parse_vegeta(out, rc):
    try:
        j = json.loads(out)
    except json.JSONDecodeError:
        return {"rps": None, "p99_ms": None, "errors": 1, "error_rate": 1.0, "timeouts": 1 if rc else 0}
    total = int(j.get("requests", 0))
    status_codes = j.get("status_codes", {}) or {}
    good = sum(int(v) for k, v in status_codes.items() if str(k).startswith(("2", "3", "4")))
    # 4xx can be expected for DELETE missing in mixed benchmark. Network/timeouts appear in errors.
    tool_errors = [e for e in (j.get("errors", []) or []) if not str(e).startswith("404 ")]
    err_count = len(tool_errors) + max(0, total - good)
    lat = j.get("latencies", {}) or {}
    p99_ns = lat.get("99th")
    return {
        "rps": float(j.get("rate", 0.0)),
        "p99_ms": (float(p99_ns) / 1_000_000.0) if p99_ns is not None else None,
        "errors": err_count,
        "error_rate": (err_count / total) if total else 1.0,
        "timeouts": sum(1 for e in tool_errors if "timeout" in str(e).lower()),
    }


def extract_worker_settings(conf):
    wp = re.search(r"worker_processes\s+([^;]+);", conf)
    wc = re.search(r"worker_connections\s+([^;]+);", conf)
    return {
        "worker_processes": wp.group(1).strip() if wp else "unknown",
        "worker_connections": wc.group(1).strip() if wc else "unknown",
    }


def metadata(base):
    nginx_p = run(["sh", "-lc", "/usr/local/nginx/sbin/nginx -v 2>&1"])
    nginx_v = (nginx_p.stderr.strip() or nginx_p.stdout.strip()) if nginx_p.returncode == 0 else f"nginx/{os.environ.get('NGINX_VERSION', 'unknown')}"
    mem_v = run(["memcached", "-h"]).stdout.splitlines()[0] if shutil.which("memcached") else "unknown"
    cpu = Path("/proc/cpuinfo").read_text(errors="ignore") if Path("/proc/cpuinfo").exists() else ""
    model = next((l.split(":", 1)[1].strip() for l in cpu.splitlines() if "model name" in l or "Processor" in l), platform.processor())
    cfg = Path(os.environ.get("NGINX_CONF_PATH", "/usr/local/nginx/conf/nginx.conf"))
    if not cfg.exists():
        cfg = ROOT / "examples" / "nginx.conf"
    return {
        "timestamp": datetime.now(timezone.utc).isoformat(),
        "git_sha": full_git_sha(),
        "git_sha_short": git_sha(),
        "os": platform.platform(),
        "cpu": model,
        "cpu_count": os.cpu_count(),
        "nginx_version": nginx_v,
        "memcached_version": mem_v,
        "module_config": cfg.read_text(errors="ignore") if cfg.exists() else "unknown",
        "worker_settings": extract_worker_settings(cfg.read_text(errors="ignore") if cfg.exists() else ""),
        "base_url": base,
    }


def run_bench(args):
    base = args.base_url
    wait_ready(base)
    seed(base)
    sha = git_sha()
    ts = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    outdir = RESULTS / f"{ts}-{sha}"
    outdir.mkdir(parents=True, exist_ok=False)
    vegeta_targets = outdir / "vegeta-targets.txt"
    vegeta_targets.write_text((SCRIPTS / "vegeta-targets.txt").read_text().replace("http://kv-nginx:8080", base))

    results = []
    for b in BENCHMARKS:
        binary = b.get("binary", b["tool"])
        if shutil.which(binary) is None:
            raise RuntimeError(f"missing benchmark tool: {binary} for {b['name']}")
        cmd = [part.format(targets=vegeta_targets) for part in b["cmd"]]
        p = run(cmd)
        raw = p.stdout + p.stderr
        (outdir / f"{b['name']}.txt").write_text(raw)
        metrics = parse_vegeta(p.stdout, p.returncode) if b["tool"] == "vegeta" else parse_wrk(raw, p.returncode)
        results.append({"name": b["name"], "tool": b["tool"], "command": cmd, "returncode": p.returncode, **metrics})

    doc = {"metadata": metadata(base), "benchmarks": results}
    (outdir / "results.json").write_text(json.dumps(doc, indent=2, sort_keys=True) + "\n")
    write_report(doc, outdir / "report.md", baseline=load_baseline(required=False))
    print(outdir)
    return 0


def load_baseline(required=True):
    if not BASELINE.exists():
        if required:
            raise RuntimeError(f"missing baseline: {BASELINE}")
        return None
    return json.loads(BASELINE.read_text())


def latest_results():
    dirs = sorted([p for p in RESULTS.iterdir() if (p / "results.json").exists()])
    if not dirs:
        raise RuntimeError("no benchmark results found; run make bench first")
    return dirs[-1] / "results.json"


def compare(args):
    current_path = Path(args.results) if args.results else latest_results()
    cur = json.loads(current_path.read_text())
    base = load_baseline(required=True)
    failures = compare_docs(cur, base)
    write_report(cur, current_path.parent / "report.md", baseline=base, failures=failures)
    if failures:
        print("performance regression detected:")
        for f in failures:
            print(f"- {f}")
        return 1
    print("performance compare passed")
    return 0


def by_name(doc):
    return {b["name"]: b for b in doc.get("benchmarks", [])}


def compare_docs(cur, base):
    failures = []
    cb, bb = by_name(cur), by_name(base)
    for name, c in cb.items():
        if name not in bb:
            failures.append(f"{name}: missing in baseline")
            continue
        b = bb[name]
        if c.get("timeouts", 0) > 0:
            failures.append(f"{name}: timeouts {c.get('timeouts')}")
        if c.get("error_rate", 1.0) > 0.001:
            failures.append(f"{name}: error rate {c.get('error_rate'):.4%} > 0.1%")
        if c.get("rps") is not None and b.get("rps"):
            drop = (b["rps"] - c["rps"]) / b["rps"]
            if drop > 0.10:
                failures.append(f"{name}: RPS drop {drop:.1%} ({c['rps']:.2f} < {b['rps']:.2f})")
        if c.get("p99_ms") is not None and b.get("p99_ms"):
            inc = (c["p99_ms"] - b["p99_ms"]) / b["p99_ms"]
            if inc > 0.15:
                failures.append(f"{name}: p99 increase {inc:.1%} ({c['p99_ms']:.2f}ms > {b['p99_ms']:.2f}ms)")
    return failures


def write_report(doc, path, baseline=None, failures=None):
    failures = failures or []
    bb = by_name(baseline) if baseline else {}
    lines = ["# Performance report", "", f"Git SHA: `{doc['metadata'].get('git_sha_short')}`", "", "| Benchmark | Tool | RPS | p99 ms | Errors | Error rate | Timeouts | vs baseline |", "|---|---:|---:|---:|---:|---:|---:|---|"]
    for b in doc.get("benchmarks", []):
        delta = "n/a"
        if b["name"] in bb and b.get("rps") and bb[b["name"]].get("rps"):
            rps_delta = (b["rps"] - bb[b["name"]]["rps"]) / bb[b["name"]]["rps"]
            p99_delta = None
            if b.get("p99_ms") and bb[b["name"]].get("p99_ms"):
                p99_delta = (b["p99_ms"] - bb[b["name"]]["p99_ms"]) / bb[b["name"]]["p99_ms"]
            delta = f"RPS {rps_delta:+.1%}" + (f", p99 {p99_delta:+.1%}" if p99_delta is not None else "")
        lines.append(f"| {b['name']} | {b['tool']} | {b.get('rps') or 0:.2f} | {b.get('p99_ms') or 0:.2f} | {b.get('errors', 0)} | {b.get('error_rate', 0):.3%} | {b.get('timeouts', 0)} | {delta} |")
    lines += ["", "## Environment", "", "```json", json.dumps(doc["metadata"], indent=2, sort_keys=True), "```"]
    if failures:
        lines += ["", "## Failures", ""] + [f"- {f}" for f in failures]
    path.write_text("\n".join(lines) + "\n")


def update_baseline(args):
    src = Path(args.results) if args.results else latest_results()
    BASELINE.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(src, BASELINE)
    print(f"updated {BASELINE} from {src}")
    return 0


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("bench")
    b.add_argument("--base-url", default=DEFAULT_BASE)
    c = sub.add_parser("compare")
    c.add_argument("--results")
    u = sub.add_parser("update-baseline")
    u.add_argument("--results")
    args = ap.parse_args()
    try:
        if args.cmd == "bench":
            return run_bench(args)
        if args.cmd == "compare":
            return compare(args)
        if args.cmd == "update-baseline":
            return update_baseline(args)
    except Exception as e:
        print(f"error: {e}", file=sys.stderr)
        return 2

if __name__ == "__main__":
    raise SystemExit(main())
