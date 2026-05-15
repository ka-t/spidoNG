#pragma once

#ifdef SPIDO_WITH_KTLS

#include <memory>
#include <string>

#include <openssl/ssl.h>

namespace spido {

// Process-wide TLS context: loads cert+key once, configures TLS 1.2/1.3
// only, enables kernel-TLS auto-config so OpenSSL programs the socket
// with TCP_ULP="tls" + SOL_TLS keying material once the handshake is
// complete. After that the connection is "free" — io_uring recv/send
// on the raw fd transports plaintext, kernel does AEAD on the wire.
class TlsContext {
public:
    static std::unique_ptr<TlsContext>
    create(const std::string& cert_path, const std::string& key_path);

    ~TlsContext();
    TlsContext(const TlsContext&)            = delete;
    TlsContext& operator=(const TlsContext&) = delete;

    SSL_CTX* ctx() noexcept { return ctx_; }

    // Per-connection SSL handle. fd is bound via SSL_set_fd; OpenSSL will
    // recv/send via the socket BIO during handshake. Caller drives via
    // io_uring POLL_ADD: each readable/writable poll completion triggers
    // another SSL_do_handshake call until SSL_is_init_finished returns true.
    SSL* new_session(int fd) noexcept;

private:
    TlsContext() = default;
    SSL_CTX* ctx_ = nullptr;
};

} // namespace spido

#endif // SPIDO_WITH_KTLS
