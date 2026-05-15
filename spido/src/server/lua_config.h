#pragma once

#ifdef SPIDO_WITH_LUA

#include <string>

#include "spido/server.h"

namespace spido {

// Loads spido.lua, extracts the global tables `server`, `jwt`, and the
// `routes` array, and applies them to a Server. Handlers are bound by
// name — the caller must have registered each `handler` field via
// Server::register_handler() before calling apply().
//
// Lua schema (all fields optional unless flagged required):
//
//   server = {
//       port          = 8080,           -- 0 → no plaintext listener
//       tls_port      = 8443,           -- 0 → no TLS listener
//       tls_cert_path = "cert.pem",
//       tls_key_path  = "key.pem",
//       threads       = 0,              -- 0 = nproc
//       defer_taskrun = false,
//       sqpoll        = true,
//       header_timeout_s = 10,
//       idle_timeout_s   = 60,
//       access_log_path  = "/var/log/spido/access.log",
//   }
//
//   jwt = {
//       enabled         = false,
//       algorithm       = "HS256",      -- or "RS256"
//       secret          = "...",        -- HS256
//       public_key_pem  = "----…",      -- RS256
//       required_claims = {"exp", "sub"},
//       issuer          = "...",
//       audience        = "...",
//       cache_size      = 65536,
//   }
//
//   routes = {
//       { method="GET",  path="/health",     handler="health",      auth=false },
//       { method="GET",  path="/users/:id",  handler="user_get",    auth=true  },
//   }
class LuaConfig {
public:
    LuaConfig();
    ~LuaConfig();
    LuaConfig(const LuaConfig&) = delete;
    LuaConfig& operator=(const LuaConfig&) = delete;

    // Reads `path`. Returns false on Lua syntax/runtime error.
    bool load_file(const std::string& path);

    // Apply the parsed config to `out`. Returns false on type mismatches
    // (e.g. routes expects a table but got a string). Already-registered
    // server fields are overwritten; missing fields keep their defaults.
    bool apply(Server& out, std::string& err);

private:
    struct State;
    State* st_;
};

} // namespace spido

#endif // SPIDO_WITH_LUA
