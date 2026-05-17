-- Intentionally unused by the default benchmark suite.
-- wrk does not handle this module's 204 PUT responses reliably enough for regression tracking.
local body = "benchmark-payload-0123456789"
local headers = {
  ["Content-Type"] = "application/octet-stream",
  ["Content-Length"] = tostring(#body),
}

request = function()
  return wrk.format("PUT", "/kv/bench-put?ttl=300", headers, body)
end
