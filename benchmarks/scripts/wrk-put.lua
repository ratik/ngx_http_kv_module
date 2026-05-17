wrk.method = "PUT"
wrk.path = "/kv/bench-put?ttl=300"
wrk.body = "benchmark-payload-0123456789"
wrk.headers["Content-Type"] = "application/octet-stream"
