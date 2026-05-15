#include "server/access_log.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/uio.h>
#include <thread>
#include <unistd.h>

namespace spido {

namespace {
size_t round_pow2(size_t n) noexcept {
    size_t p = 1;
    while (p < n) p <<= 1;
    return p;
}
} // namespace

AccessLogRing::AccessLogRing(size_t entries)
    : capacity_mask_(round_pow2(entries) - 1),
      buf_((capacity_mask_ + 1) * kLineMax),
      lens_(capacity_mask_ + 1, 0) {}

char* AccessLogRing::reserve() noexcept {
    const size_t head = head_.load(std::memory_order_relaxed);
    const size_t tail = tail_.load(std::memory_order_acquire);
    if (head - tail > capacity_mask_) return nullptr; // full
    return buf_.data() + (head & capacity_mask_) * kLineMax;
}

void AccessLogRing::commit() noexcept {
    const size_t head = head_.load(std::memory_order_relaxed);
    const size_t idx  = head & capacity_mask_;
    // Length is figured from buffer content — caller wrote a NUL-terminated
    // string but we record actual length here to avoid strlen in consumer.
    char* p = buf_.data() + idx * kLineMax;
    size_t n = 0;
    while (n < kLineMax && p[n] != '\0') ++n;
    lens_[idx] = uint16_t(n);
    head_.store(head + 1, std::memory_order_release);
}

const char* AccessLogRing::peek_at(size_t i, size_t& len) noexcept {
    const size_t tail = tail_.load(std::memory_order_relaxed);
    const size_t head = head_.load(std::memory_order_acquire);
    if (tail + i >= head) return nullptr;
    const size_t idx = (tail + i) & capacity_mask_;
    len = lens_[idx];
    return buf_.data() + idx * kLineMax;
}

size_t AccessLogRing::pending() const noexcept {
    const size_t tail = tail_.load(std::memory_order_relaxed);
    const size_t head = head_.load(std::memory_order_acquire);
    return head - tail;
}

void AccessLogRing::consume(size_t n) noexcept {
    tail_.store(tail_.load(std::memory_order_relaxed) + n,
                std::memory_order_release);
}

std::unique_ptr<AccessLogger>
AccessLogger::create(const std::string& path, size_t workers, bool drop_on_full) {
    auto self = std::unique_ptr<AccessLogger>(new AccessLogger());
    self->drop_on_full_ = drop_on_full;

    if (path == "-" || path == "/dev/stdout" || path == "/dev/stderr") {
        self->fd_ = ::dup(path == "/dev/stdout" ? 1 : 2);
    } else {
        self->fd_ = ::open(path.c_str(),
                           O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    }
    if (self->fd_ < 0) {
        std::fprintf(stderr,
                     "spido: access log open(%s) failed: %s\n",
                     path.c_str(), std::strerror(errno));
        return nullptr;
    }

    self->rings_.reserve(workers);
    for (size_t i = 0; i < workers; ++i)
        self->rings_.push_back(std::make_unique<AccessLogRing>(4096));

    self->thread_ = std::thread([s = self.get()] { s->writer_loop(); });
    return self;
}

AccessLogger::~AccessLogger() {
    stop();
    if (fd_ >= 0) ::close(fd_);
}

void AccessLogger::stop() {
    stop_.store(true, std::memory_order_release);
    if (thread_.joinable()) thread_.join();
}

void AccessLogger::writer_loop() {
    // Coalesce up to N lines per writev. iovec is reset each round.
    constexpr size_t kBatch = 64;
    constexpr useconds_t kIdleUs = 1000;  // 1 ms back-off when no work

    std::vector<iovec> iov;
    iov.reserve(kBatch);

    while (!stop_.load(std::memory_order_acquire)) {
        bool any = false;
        for (auto& rp : rings_) {
            auto& r = *rp;
            iov.clear();
            for (size_t i = 0; i < kBatch; ++i) {
                size_t len = 0;
                const char* p = r.peek_at(i, len);
                if (!p) break;
                iov.push_back({const_cast<char*>(p), len});
            }
            if (iov.empty()) continue;
            any = true;
            ssize_t w = ::writev(fd_, iov.data(), int(iov.size()));
            if (w < 0) {
                if (errno == EINTR) continue;
                static thread_local int complained = 0;
                if (!complained++) std::perror("spido: access log writev");
            }
            r.consume(iov.size());
        }
        if (!any) ::usleep(kIdleUs);
    }

    // Final drain on shutdown.
    for (auto& rp : rings_) {
        auto& r = *rp;
        for (;;) {
            size_t len = 0;
            const char* p = r.peek_at(0, len);
            if (!p) break;
            ssize_t w = ::write(fd_, p, len);
            (void)w;
            r.consume(1);
        }
    }
}

} // namespace spido
