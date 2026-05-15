#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace spido_pg {

class PgConnection;

// Dedicated LISTEN/NOTIFY connection. Owns its own PgConnection (not from
// the pool) and a background thread that blocks in a recv() until a
// NotificationResponse ('A') arrives.
class PgNotify {
public:
    using Handler = std::function<void(std::string_view payload)>;

    PgNotify(std::string socket_path,
             std::string user,
             std::string password,
             std::string dbname);
    ~PgNotify();

    PgNotify(const PgNotify&)            = delete;
    PgNotify& operator=(const PgNotify&) = delete;

    void listen(std::string_view channel, Handler h);
    void unlisten(std::string_view channel);

    void start();
    void stop();

private:
    void                                                       loop();
    bool                                                       connect_and_subscribe();

    std::string                                                socket_;
    std::string                                                user_;
    std::string                                                password_;
    std::string                                                dbname_;

    std::unique_ptr<PgConnection>                              conn_;

    mutable std::mutex                                         mtx_;
    std::unordered_map<std::string, std::vector<Handler>>      handlers_;

    std::thread                                                thread_;
    std::atomic<bool>                                          running_{false};
};

} // namespace spido_pg
