-- wrk Lua script for POST /firmware. The generated handler reads row
-- columns from x-device_id / x-version / x-payload headers (the template
-- doesn't parse JSON bodies yet), so the load-gen sets those headers per
-- request. We vary the device_id per connection to avoid collisions and
-- keep the batch writer's flusher fed.

wrk.method = "POST"
wrk.body   = ""
wrk.headers["Content-Type"] = "application/octet-stream"

-- 16-char hex payload — large enough to round-trip a non-trivial cell
-- through binary BIND but small enough not to bottleneck the HTTP parser.
local function rand_hex(n)
    local t = {}
    for i = 1, n do t[i] = string.format("%x", math.random(0, 15)) end
    return table.concat(t)
end

math.randomseed(os.time() + (os.getenv("WRK_SEED") or 0))

function init(args)
    -- Connection-local counter so device_ids don't collide across threads.
    counter = 0
    seed    = math.random(0, 2^30)
end

function request()
    counter = counter + 1
    wrk.headers["x-device_id"] = string.format("dev-%d-%d", seed, counter)
    wrk.headers["x-version"]   = "1.2.3"
    wrk.headers["x-payload"]   = rand_hex(32)
    return wrk.format(nil, "/firmware")
end

-- Summary: report write success rate distinctly so a flood of 4xx/5xx
-- doesn't get hidden by raw QPS numbers.
function done(summary, latency, requests)
    io.write(string.format("\n-- writes --\n"))
    io.write(string.format("requests          %d\n",   summary.requests))
    io.write(string.format("non-2xx/3xx       %d\n",   summary.errors.status))
    io.write(string.format("socket errors     %d\n",
        summary.errors.connect + summary.errors.read
        + summary.errors.write + summary.errors.timeout))
    io.write(string.format("p50 latency       %.2f ms\n", latency:percentile(50)/1000))
    io.write(string.format("p99 latency       %.2f ms\n", latency:percentile(99)/1000))
    io.write(string.format("max latency       %.2f ms\n", latency.max/1000))
end
