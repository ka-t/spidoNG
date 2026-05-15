#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "spido/request.h"
#include "spido/response.h"

namespace spido {

using Handler = std::function<void(Request&, Response&)>;

struct ServerConfig {
    uint16_t port = 8080;
    std::string bind = "0.0.0.0";
    int backlog = 4096;

    // 0 = autodetect (one worker thread per online CPU).
    unsigned threads = 0;

    // Enable IORING_SETUP_SQPOLL so the kernel pulls SQEs without our
    // io_uring_enter syscall. Requires CAP_SYS_NICE on some kernels.
    bool sqpoll = true;
    unsigned sqpoll_idle_ms = 2000;

    // IORING_SETUP_DEFER_TASKRUN + IORING_SETUP_SINGLE_ISSUER (kernel ≥
    // 6.0/6.1, liburing ≥ 2.5). Defers io_uring task work to the next
    // io_uring_submit_and_wait call so completions batch instead of
    // firing as IPI-driven task_works. Mutually exclusive with SQPOLL —
    // we automatically disable SQPOLL when this is set.
    bool defer_taskrun = false;

    // io_uring submission queue depth per worker.
    unsigned sq_entries = 4096;

    // Per-connection inbound buffer cap (bytes). Requests larger than this
    // get a 413 and the connection is closed.
    size_t max_request_bytes = 1 << 20; // 1 MiB

    // TCP_FASTOPEN queue length on the listener. 0 disables. Allows clients
    // to piggy-back request bytes onto the SYN, eliminating a round-trip
    // for the first request. Requires net.ipv4.tcp_fastopen sysctl > 0.
    int tcp_fastopen = 256;

    // Per-connection deadlines (seconds). 0 = disabled (don't do that on a
    // public-facing listener — slowloris will keep the conn forever).
    //
    //   header_timeout_s  — accept -> full request headers parsed.
    //                       This is the slowloris protection: bytes
    //                       received before headers are complete do NOT
    //                       extend the deadline, so dripping bytes 1/sec
    //                       can't keep a conn alive.
    //   body_timeout_s    — full headers parsed -> full body received.
    //   idle_timeout_s    — between requests on keep-alive connections.
    //   write_timeout_s   — application data send must drain within this.
    unsigned header_timeout_s = 10;
    unsigned body_timeout_s   = 30;
    unsigned idle_timeout_s   = 60;
    unsigned write_timeout_s  = 30;

    // Set TCP_QUICKACK on accepted sockets to disable delayed-ACK on the
    // server side — reduces tail latency for keep-alive request pacing.
    bool tcp_quickack = true;

    // SO_BUSY_POLL microseconds. >0 makes the kernel busy-poll the socket
    // for this long before sleeping. Reduces latency but burns CPU; off by
    // default. Try 50 µs for latency-sensitive workloads.
    unsigned so_busy_poll_us = 0;

    // Emit a Date: header on responses. Cached per worker thread and
    // refreshed at most once per second — strftime stays off the hot path.
    bool emit_date_header = false;

    // Combined-style access log. When access_log_path is non-empty the
    // server spawns a background writer that drains a per-worker SPSC
    // ring; worker hot path only formats one line into the ring slot —
    // no fsync, no syscalls per request. "-" or "/dev/stdout" → stderr.
    std::string access_log_path;
    // Drop lines instead of blocking when the ring fills (default: drop).
    // For low-stake tier-1 services this is the right trade-off; for
    // audit-grade logging set false (then bursts slow the worker).
    bool        access_log_drop_on_full = true;

    // AF_XDP kernel-bypass network layer. Only compiled in when the
    // library is built with -DSPIDO_WITH_XDP=ON. When enabled at run
    // time and setup succeeds, the BPF program steers TCP traffic on
    // (xdp_iface, port) into AF_XDP sockets. Setup failure (no libbpf,
    // wrong interface, no CAP_NET_ADMIN, kernel doesn't support it) is
    // not fatal — the server falls back to the io_uring engine.
    bool        xdp             = false;
    std::string xdp_iface       = "";    // e.g. "eth0", "lo"
    int         xdp_queues      = 1;
    bool        xdp_zerocopy    = true;  // prefer ZC; fall back to COPY

    // Kernel TLS (kTLS, Linux ≥ 4.17). When tls_port > 0 each worker
    // also opens a SO_REUSEPORT listener on that port. New connections
    // do an async OpenSSL handshake driven by io_uring POLL_ADD; once
    // the handshake completes the socket is upgraded to kTLS (kernel
    // does the AES-GCM AEAD) and subsequent io_uring recv/send sees
    // plaintext. Requires SPIDO_WITH_KTLS=ON at build time.
    uint16_t    tls_port        = 0;
    std::string tls_cert_path;
    std::string tls_key_path;

    // JWT bearer-token authentication. Off by default — when `jwt.enabled`
    // is false the verifier is never even constructed and the hot path
    // checks one bool per request.
    struct Jwt {
        bool        enabled         = false;
        std::string algorithm       = "HS256";  // HS256 or RS256
        std::string secret;                     // HS256 shared key
        std::string public_key_pem;             // RS256 verify key
        std::vector<std::string> required_claims = {"exp"};
        std::string issuer;
        std::string audience;
        size_t      cache_size      = 65536;
        unsigned    leeway_seconds  = 30;
    } jwt;
};

class Server {
public:
    Server();                            // default — overwrite cfg via config()
    explicit Server(uint16_t port);
    explicit Server(ServerConfig cfg);
    ~Server();

    Server(const Server&)            = delete;
    Server& operator=(const Server&) = delete;

    Server& get   (std::string_view path, Handler h, bool require_auth = false);
    Server& post  (std::string_view path, Handler h, bool require_auth = false);
    Server& put   (std::string_view path, Handler h, bool require_auth = false);
    Server& del   (std::string_view path, Handler h, bool require_auth = false);
    Server& route (Method m, std::string_view path, Handler h,
                   bool require_auth = false);

    // Register a handler by name — used by the Lua config to wire routes
    // defined in spido.lua against C++ implementations.
    Server& register_handler(std::string_view name, Handler h);
    Server& route_by_name   (Method m, std::string_view path,
                             std::string_view handler_name,
                             bool require_auth = false);

    // Mutable access to the live ServerConfig, used by LuaConfig::apply()
    // to splat parsed fields back into the server before listen() runs.
    ServerConfig& config() noexcept;
    const ServerConfig& config() const noexcept;

    // Pre-serializes a 200 response for routes whose body never changes.
    // The worker bypasses the parser→handler→serialize chain entirely and
    // appends the wire bytes directly — typically 4-8x throughput vs a
    // handler. Default content_type is text/plain; pass "application/json"
    // for JSON literals etc. Response carries Connection: keep-alive — if
    // the client requested close, the server still closes after sending.
    Server& serve_static(Method m, std::string_view path,
                         std::string_view body,
                         std::string_view content_type = "text/plain");

    // Blocks until SIGINT/SIGTERM, then drains in-flight requests and returns.
    void listen();

    // Trigger graceful shutdown from another thread or signal handler.
    void stop();

    // Defined in server.cpp; exposed so static helpers (signal handler) can
    // reach the live instance. Not part of the public API.
    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace spido
