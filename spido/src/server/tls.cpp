#ifdef SPIDO_WITH_KTLS

#include "server/tls.h"

#include <cstdio>
#include <fcntl.h>

#include <openssl/bio.h>
#include <openssl/err.h>

namespace spido {

namespace {
void log_ssl_errors(const char* prefix) {
    unsigned long e;
    while ((e = ERR_get_error()) != 0) {
        char buf[256];
        ERR_error_string_n(e, buf, sizeof(buf));
        std::fprintf(stderr, "spido/tls: %s: %s\n", prefix, buf);
    }
}
} // namespace

std::unique_ptr<TlsContext>
TlsContext::create(const std::string& cert_path, const std::string& key_path) {
    auto self = std::unique_ptr<TlsContext>(new TlsContext());

    self->ctx_ = ::SSL_CTX_new(TLS_server_method());
    if (!self->ctx_) { log_ssl_errors("SSL_CTX_new"); return nullptr; }

    // For this build we pin to TLS 1.2 so the server can SSL_free as soon
    // as SSL_do_handshake returns success — in TLS 1.2 both TX and RX kTLS
    // keys are installed at that point. TLS 1.3 needs an extra SSL_read
    // round-trip after handshake to consume the client's Finished before
    // RX kTLS arms; that's a future enhancement.
    ::SSL_CTX_set_min_proto_version(self->ctx_, TLS1_2_VERSION);
    ::SSL_CTX_set_max_proto_version(self->ctx_, TLS1_2_VERSION);

    // Limit to AES-GCM / CHACHA20 because that's what kTLS handles in the
    // kernel. Anything else would silently fall back to userspace crypto.
    ::SSL_CTX_set_ciphersuites(self->ctx_,
        "TLS_AES_128_GCM_SHA256:TLS_AES_256_GCM_SHA384:TLS_CHACHA20_POLY1305_SHA256");
    ::SSL_CTX_set_cipher_list(self->ctx_,
        "ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256:"
        "ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384:"
        "ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-RSA-CHACHA20-POLY1305");

    // Tell OpenSSL to flip the socket into kTLS mode once keys are ready.
    // Combined with the SSL_OP_ENABLE_KTLS option, OpenSSL itself calls
    // setsockopt(TCP_ULP="tls") + setsockopt(SOL_TLS, TLS_TX/TLS_RX)
    // right after the handshake completes.
    ::SSL_CTX_set_options(self->ctx_, SSL_OP_ENABLE_KTLS);
    // Don't let OpenSSL retry recv/send transparently — we want clean
    // WANT_READ/WANT_WRITE signals so our io_uring driver can park.
    ::SSL_CTX_clear_mode(self->ctx_, SSL_MODE_AUTO_RETRY);

    if (::SSL_CTX_use_certificate_chain_file(self->ctx_, cert_path.c_str()) <= 0) {
        log_ssl_errors("use_certificate_chain_file");
        return nullptr;
    }
    if (::SSL_CTX_use_PrivateKey_file(self->ctx_,
                                       key_path.c_str(),
                                       SSL_FILETYPE_PEM) <= 0) {
        log_ssl_errors("use_PrivateKey_file");
        return nullptr;
    }
    if (!::SSL_CTX_check_private_key(self->ctx_)) {
        std::fprintf(stderr, "spido/tls: private key does not match cert\n");
        return nullptr;
    }

    // Session cache off — we don't need session resumption to demonstrate
    // kTLS, and the cache adds a global lock on the hot path.
    ::SSL_CTX_set_session_cache_mode(self->ctx_, SSL_SESS_CACHE_OFF);
    return self;
}

TlsContext::~TlsContext() {
    if (ctx_) ::SSL_CTX_free(ctx_);
}

SSL* TlsContext::new_session(int fd) noexcept {
    SSL* ssl = ::SSL_new(ctx_);
    if (!ssl) { log_ssl_errors("SSL_new"); return nullptr; }
    if (::SSL_set_fd(ssl, fd) != 1) {
        log_ssl_errors("SSL_set_fd");
        ::SSL_free(ssl);
        return nullptr;
    }
    // Server side accepts the handshake; the first SSL_do_handshake call
    // will read ClientHello and write ServerHello/etc.
    ::SSL_set_accept_state(ssl);
    return ssl;
}

} // namespace spido

#endif // SPIDO_WITH_KTLS
