#include <cstdio>

#include "spido/server.h"

using namespace spido;

int main() {
    Server server(8080);

    server.get("/health", [](Request&, Response& res) {
        res.json({{"status", "ok"}});
    });

    server.get("/", [](Request&, Response& res) {
        res.text("spido");
    });

    server.post("/upload", [](Request& req, Response& res) {
        res.json({{"received", static_cast<int64_t>(req.body.size())}});
    });

    server.put("/echo", [](Request& req, Response& res) {
        res.body.assign(req.body.data(), req.body.size());
        res.header("Content-Type", "application/octet-stream");
    });

    server.del("/items/42", [](Request&, Response& res) {
        res.set_status(204);
    });

    server.listen();
    return 0;
}
