#include "spido_pg/notify.h"
#include "spido_pg/connection.h"
#include "wire.h"

#include <chrono>
#include <cstring>
#include <thread>
#include <unistd.h>

#include <liburing.h>
#include <sys/socket.h>

namespace spido_pg {

PgNotify::PgNotify(std::string socket_path,
                   std::string user,
                   std::string password,
                   std::string dbname)
    : socket_(std::move(socket_path)),
      user_(std::move(user)),
      password_(std::move(password)),
      dbname_(std::move(dbname))
{}

PgNotify::~PgNotify() { stop(); }

void PgNotify::listen(std::string_view channel, Handler h) {
    {
        std::lock_guard lk(mtx_);
        handlers_[std::string(channel)].push_back(std::move(h));
    }
    if (running_.load(std::memory_order_acquire) && conn_ && conn_->is_ready()) {
        std::string sql;
        sql.reserve(10 + channel.size());
        sql.append("LISTEN \"").append(channel).push_back('"');
        conn_->exec(sql);
    }
}

void PgNotify::unlisten(std::string_view channel) {
    {
        std::lock_guard lk(mtx_);
        handlers_.erase(std::string(channel));
    }
    if (running_.load(std::memory_order_acquire) && conn_ && conn_->is_ready()) {
        std::string sql;
        sql.reserve(12 + channel.size());
        sql.append("UNLISTEN \"").append(channel).push_back('"');
        conn_->exec(sql);
    }
}

bool PgNotify::connect_and_subscribe() {
    conn_ = std::make_unique<PgConnection>();
    if (!conn_->connect(socket_, user_, password_, dbname_)) {
        conn_.reset();
        return false;
    }
    std::lock_guard lk(mtx_);
    for (const auto& [chan, _] : handlers_) {
        std::string sql;
        sql.reserve(10 + chan.size());
        sql.append("LISTEN \"").append(chan).push_back('"');
        auto r = conn_->exec(sql);
        if (!r.ok()) return false;
    }
    return true;
}

void PgNotify::start() {
    if (running_.exchange(true, std::memory_order_acq_rel)) return;
    thread_ = std::thread([this]{ loop(); });
}

void PgNotify::stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) return;
    if (conn_) {
        // Forcing the recv side to wake. PgConnection's destructor will
        // emit Terminate and close the fd; the loop's recv will return
        // -ECONNRESET and exit.
        conn_->close();
    }
    if (thread_.joinable()) thread_.join();
    conn_.reset();
}

void PgNotify::loop() {
    using namespace std::chrono;
    auto backoff = milliseconds(1000);
    const auto max_backoff = milliseconds(60000);

    while (running_.load(std::memory_order_acquire)) {
        if (!conn_ || !conn_->is_ready()) {
            if (!connect_and_subscribe()) {
                std::this_thread::sleep_for(backoff);
                backoff = std::min(backoff * 2, max_backoff);
                continue;
            }
            backoff = milliseconds(1000);
        }

        // Drive a raw recv against the connection's fd. We bypass the public
        // PgConnection API here because exec() expects to drive a full
        // request/response — for LISTEN we just want to read async server
        // pushes ('A' frames) until the connection dies.
        int fd = conn_->fd();
        uint8_t hdr[5];
        ssize_t got = ::recv(fd, hdr, 5, MSG_WAITALL);
        if (got != 5) { conn_->close(); continue; }

        char type = static_cast<char>(hdr[0]);
        int32_t total = wire::get_i32(hdr + 1);
        if (total < 4 || total > 8 * 1024 * 1024) { conn_->close(); continue; }
        std::vector<uint8_t> body(static_cast<size_t>(total - 4));
        if (!body.empty()) {
            ssize_t bgot = ::recv(fd, body.data(), body.size(), MSG_WAITALL);
            if (bgot != static_cast<ssize_t>(body.size())) { conn_->close(); continue; }
        }

        if (type == wire::kNotification) {
            // int32 pid, string channel, string payload
            if (body.size() < 4) continue;
            size_t p = 4;
            size_t cl = ::strnlen(reinterpret_cast<const char*>(body.data() + p),
                                  body.size() - p);
            std::string chan(reinterpret_cast<const char*>(body.data() + p), cl);
            p += cl + 1;
            if (p > body.size()) continue;
            size_t pl = ::strnlen(reinterpret_cast<const char*>(body.data() + p),
                                  body.size() - p);
            std::string payload(reinterpret_cast<const char*>(body.data() + p), pl);

            std::vector<Handler> handlers;
            {
                std::lock_guard lk(mtx_);
                auto it = handlers_.find(chan);
                if (it != handlers_.end()) handlers = it->second;
            }
            for (auto& h : handlers) h(payload);
        }
        // Other frames (ParameterStatus, NoticeResponse, ErrorResponse on
        // disconnect, etc.) → drop. On ErrorResponse the next recv will
        // typically fail and we'll reconnect.
    }
}

} // namespace spido_pg
