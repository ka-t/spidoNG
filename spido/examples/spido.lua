-- spido.lua — config file consumed by spido::LuaConfig
--
-- Sections are global tables: `server`, `jwt`, `routes`. Anything you
-- don't set keeps its C++ default. Handlers are looked up by name in
-- the C++ named_handlers registry (see Server::register_handler).

server = {
    port             = 8090,                       -- 0 = no plain HTTP
    tls_port         = 0,                          -- behind nginx → keep plain
    threads          = 4,
    defer_taskrun    = true,
    sqpoll           = false,
    header_timeout_s = 10,
    idle_timeout_s   = 60,
    access_log_path  = "/tmp/spido_access.log",
}

jwt = {
    enabled         = true,
    algorithm       = "HS256",
    secret          = "supersecret-spido-shared-key-change-me",
    required_claims = { "exp", "sub" },
    issuer          = "spido-test",
    audience        = "spido-api",
    cache_size      = 65536,
    leeway_seconds  = 30,
}

routes = {
    -- Public.
    { method = "GET",  path = "/health",     handler = "health",  auth = false },

    -- Auth required.
    { method = "GET",  path = "/me",         handler = "me",      auth = true  },
    { method = "GET",  path = "/users/:id",  handler = "user_get",auth = true  },
    { method = "POST", path = "/admin/wipe", handler = "wipe",    auth = true  },
}
