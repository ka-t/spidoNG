#ifdef SPIDO_WITH_LUA

#include "server/lua_config.h"

#include <cstdio>
#include <cstring>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

#include "spido/request.h"
#include "spido/response.h"

namespace spido {

struct LuaConfig::State { lua_State* L = nullptr; };

namespace {

// Pull `table.key` as string. Returns true if present and is a string,
// writing the result to `out`. Leaves the stack unchanged on return.
bool field_str(lua_State* L, int tbl, const char* key, std::string& out) {
    lua_getfield(L, tbl, key);
    bool ok = (lua_type(L, -1) == LUA_TSTRING);
    if (ok) {
        size_t n;
        const char* s = lua_tolstring(L, -1, &n);
        out.assign(s, n);
    }
    lua_pop(L, 1);
    return ok;
}

bool field_int(lua_State* L, int tbl, const char* key, long long& out) {
    lua_getfield(L, tbl, key);
    bool ok = (lua_type(L, -1) == LUA_TNUMBER);
    if (ok) out = static_cast<long long>(lua_tointeger(L, -1));
    lua_pop(L, 1);
    return ok;
}

bool field_bool(lua_State* L, int tbl, const char* key, bool& out) {
    lua_getfield(L, tbl, key);
    bool ok = (lua_type(L, -1) == LUA_TBOOLEAN);
    if (ok) out = bool(lua_toboolean(L, -1));
    lua_pop(L, 1);
    return ok;
}

// Pull a sequence of strings from `table.key`.
bool field_strvec(lua_State* L, int tbl, const char* key,
                  std::vector<std::string>& out) {
    lua_getfield(L, tbl, key);
    if (lua_type(L, -1) != LUA_TTABLE) { lua_pop(L, 1); return false; }
    out.clear();
    lua_pushnil(L);
    while (lua_next(L, -2)) {
        if (lua_type(L, -1) == LUA_TSTRING) {
            size_t n; const char* s = lua_tolstring(L, -1, &n);
            out.emplace_back(s, n);
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    return true;
}

Method method_from_str(std::string_view s) {
    if (s == "GET")     return Method::GET;
    if (s == "POST")    return Method::POST;
    if (s == "PUT")     return Method::PUT;
    if (s == "DELETE")  return Method::DELETE;
    if (s == "HEAD")    return Method::HEAD;
    if (s == "OPTIONS") return Method::OPTIONS;
    if (s == "PATCH")   return Method::PATCH;
    return Method::Unknown;
}

} // namespace

LuaConfig::LuaConfig() : st_(new State) {
    st_->L = ::luaL_newstate();
    if (st_->L) ::luaL_openlibs(st_->L);
}

LuaConfig::~LuaConfig() {
    if (st_) {
        if (st_->L) lua_close(st_->L);
        delete st_;
    }
}

bool LuaConfig::load_file(const std::string& path) {
    if (!st_->L) return false;
    int rc = ::luaL_loadfile(st_->L, path.c_str());
    if (rc != LUA_OK) {
        std::fprintf(stderr, "spido/lua: load %s: %s\n",
                     path.c_str(), lua_tostring(st_->L, -1));
        lua_pop(st_->L, 1);
        return false;
    }
    rc = lua_pcall(st_->L, 0, 0, 0);
    if (rc != LUA_OK) {
        std::fprintf(stderr, "spido/lua: exec %s: %s\n",
                     path.c_str(), lua_tostring(st_->L, -1));
        lua_pop(st_->L, 1);
        return false;
    }
    return true;
}

bool LuaConfig::apply(Server& out, std::string& err) {
    lua_State* L = st_->L;
    if (!L) { err = "no Lua state"; return false; }

    // ---- server table ----
    lua_getglobal(L, "server");
    if (lua_type(L, -1) == LUA_TTABLE) {
        long long iv; std::string sv; bool bv;
        if (field_int (L, -1, "port",          iv)) out.config().port          = uint16_t(iv);
        if (field_int (L, -1, "tls_port",      iv)) out.config().tls_port      = uint16_t(iv);
        if (field_str (L, -1, "tls_cert_path", sv)) out.config().tls_cert_path = sv;
        if (field_str (L, -1, "tls_key_path",  sv)) out.config().tls_key_path  = sv;
        if (field_int (L, -1, "threads",       iv)) out.config().threads       = unsigned(iv);
        if (field_bool(L, -1, "defer_taskrun", bv)) out.config().defer_taskrun = bv;
        if (field_bool(L, -1, "sqpoll",        bv)) out.config().sqpoll        = bv;
        if (field_int (L, -1, "header_timeout_s", iv))
            out.config().header_timeout_s = unsigned(iv);
        if (field_int (L, -1, "idle_timeout_s",   iv))
            out.config().idle_timeout_s   = unsigned(iv);
        if (field_str (L, -1, "access_log_path", sv))
            out.config().access_log_path = sv;
    }
    lua_pop(L, 1);

    // ---- jwt table ----
    lua_getglobal(L, "jwt");
    if (lua_type(L, -1) == LUA_TTABLE) {
        auto& j = out.config().jwt;
        long long iv; std::string sv; bool bv;
        if (field_bool(L, -1, "enabled",         bv)) j.enabled         = bv;
        if (field_str (L, -1, "algorithm",       sv)) j.algorithm       = sv;
        if (field_str (L, -1, "secret",          sv)) j.secret          = sv;
        if (field_str (L, -1, "public_key_pem",  sv)) j.public_key_pem  = sv;
        if (field_str (L, -1, "issuer",          sv)) j.issuer          = sv;
        if (field_str (L, -1, "audience",        sv)) j.audience        = sv;
        if (field_int (L, -1, "cache_size",      iv)) j.cache_size      = size_t(iv);
        if (field_int (L, -1, "leeway_seconds",  iv)) j.leeway_seconds  = unsigned(iv);
        field_strvec(L, -1, "required_claims", j.required_claims);
    }
    lua_pop(L, 1);

    // ---- routes ----
    lua_getglobal(L, "routes");
    if (lua_type(L, -1) == LUA_TTABLE) {
        size_t n = lua_rawlen(L, -1);
        for (size_t i = 1; i <= n; ++i) {
            lua_rawgeti(L, -1, lua_Integer(i));
            if (lua_type(L, -1) == LUA_TTABLE) {
                std::string method, path, handler;
                bool auth = false;
                if (!field_str(L, -1, "method",  method) ||
                    !field_str(L, -1, "path",    path)   ||
                    !field_str(L, -1, "handler", handler)) {
                    err = "route missing method/path/handler";
                    lua_pop(L, 2);
                    return false;
                }
                field_bool(L, -1, "auth", auth);
                Method m = method_from_str(method);
                if (m == Method::Unknown) {
                    err = "unknown HTTP method: " + method;
                    lua_pop(L, 2);
                    return false;
                }
                out.route_by_name(m, path, handler, auth);
            }
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    return true;
}

} // namespace spido

#endif // SPIDO_WITH_LUA
