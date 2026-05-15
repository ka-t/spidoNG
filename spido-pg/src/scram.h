#pragma once

// SCRAM-SHA-256 client for PG's SASL authentication exchange. We
// implement the minimal subset RFC 5802 / 7677 requires for talking to
// a stock PostgreSQL ≥ 14 with scram-sha-256 in pg_hba.conf. Channel
// binding (tls-server-end-point) is NOT implemented — we connect over
// Unix sockets where channel binding is moot.
//
// Wire flow inside the PG protocol's 'R' frames:
//   server → R + AuthSASL    + "SCRAM-SHA-256\0\0"
//   client → 'p' + SASLInitialResponse
//                ("SCRAM-SHA-256" \0 i32(len) "n,,n=,r=<nonce>")
//   server → R + AuthSASLContinue + "r=<server-nonce>,s=<salt-b64>,i=<iters>"
//   client → 'p' + "c=biws,r=<nonce>,p=<proof-b64>"
//   server → R + AuthSASLFinal + "v=<server-sig-b64>"
//   server → R + AuthOk

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace spido_pg::scram {

inline std::string b64_encode(const uint8_t* data, size_t len) {
    static const char kT[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t n = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < len) n |= static_cast<uint32_t>(data[i+1]) << 8;
        if (i + 2 < len) n |= static_cast<uint32_t>(data[i+2]);
        out.push_back(kT[(n >> 18) & 0x3F]);
        out.push_back(kT[(n >> 12) & 0x3F]);
        out.push_back(i + 1 < len ? kT[(n >> 6) & 0x3F] : '=');
        out.push_back(i + 2 < len ? kT[(n     ) & 0x3F] : '=');
    }
    return out;
}

inline std::vector<uint8_t> b64_decode(std::string_view s) {
    auto v = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+')             return 62;
        if (c == '/')             return 63;
        return -1;
    };
    std::vector<uint8_t> out;
    out.reserve((s.size() / 4) * 3);
    uint32_t buf = 0; int nbits = 0;
    for (char c : s) {
        if (c == '=' || c == '\n' || c == '\r') continue;
        int n = v(c);
        if (n < 0) continue;
        buf = (buf << 6) | static_cast<uint32_t>(n);
        nbits += 6;
        if (nbits >= 8) {
            nbits -= 8;
            out.push_back(static_cast<uint8_t>((buf >> nbits) & 0xff));
        }
    }
    return out;
}

inline std::vector<uint8_t> hmac_sha256(const uint8_t* key, size_t klen,
                                         const uint8_t* msg, size_t mlen)
{
    unsigned int olen = 0;
    std::vector<uint8_t> out(32);
    HMAC(EVP_sha256(), key, static_cast<int>(klen),
         msg, mlen, out.data(), &olen);
    out.resize(olen);
    return out;
}

inline std::vector<uint8_t> sha256(const uint8_t* msg, size_t mlen) {
    std::vector<uint8_t> out(32);
    SHA256(msg, mlen, out.data());
    return out;
}

// PBKDF2-HMAC-SHA256 (the SCRAM "Hi" function). Iterates HMAC over a
// counter-suffixed salt; OpenSSL has a one-call helper that handles
// the counter machinery for us.
inline std::vector<uint8_t> Hi(std::string_view password,
                                const std::vector<uint8_t>& salt,
                                int iters)
{
    std::vector<uint8_t> out(32);
    PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()),
                      salt.data(), static_cast<int>(salt.size()),
                      iters, EVP_sha256(),
                      32, out.data());
    return out;
}

inline std::string random_nonce(size_t bytes = 18) {
    std::vector<uint8_t> raw(bytes);
    RAND_bytes(raw.data(), static_cast<int>(raw.size()));
    return b64_encode(raw.data(), raw.size());
}

// Build the client-first message bare ("n=,r=<nonce>") and the GS2 header
// prepended ("n,," means no channel binding, no authzid). Returns the
// concatenated client-first-message PG expects in the SASLInitialResponse.
struct ClientFirst {
    std::string gs2_header;        // "n,,"
    std::string bare;              // "n=,r=<nonce>"
    std::string client_nonce;
    std::string full;              // gs2_header + bare
};

inline ClientFirst build_client_first() {
    ClientFirst c;
    c.gs2_header   = "n,,";
    c.client_nonce = random_nonce();
    c.bare         = "n=,r=" + c.client_nonce;
    c.full         = c.gs2_header + c.bare;
    return c;
}

// Parse "r=...,s=...,i=..." server-first message.
struct ServerFirst {
    std::string          r_nonce;
    std::vector<uint8_t> salt;
    int                  iters = 0;
};

inline bool parse_server_first(std::string_view msg, ServerFirst& out) {
    size_t i = 0;
    auto next_field = [&]() -> std::string_view {
        size_t start = i;
        while (i < msg.size() && msg[i] != ',') ++i;
        auto f = msg.substr(start, i - start);
        if (i < msg.size()) ++i;
        return f;
    };
    while (i < msg.size()) {
        auto f = next_field();
        if (f.size() < 2 || f[1] != '=') continue;
        char tag = f[0];
        auto val = f.substr(2);
        switch (tag) {
            case 'r': out.r_nonce = std::string(val); break;
            case 's': out.salt    = b64_decode(val);  break;
            case 'i': try { out.iters = std::stoi(std::string(val)); }
                      catch (...) { return false; } break;
            default: break;
        }
    }
    return !out.r_nonce.empty() && !out.salt.empty() && out.iters > 0;
}

// Compute the client final message ("c=biws,r=<r>,p=<proof>") and the
// expected server signature so we can verify the SASLFinal frame.
struct FinalArtifacts {
    std::string client_final;       // to send
    std::string expected_server_sig_b64;
};

inline FinalArtifacts build_client_final(std::string_view password,
                                          const ClientFirst& cf,
                                          const ServerFirst& sf)
{
    FinalArtifacts art;
    // SaltedPassword = Hi(password, salt, i)
    auto salted = Hi(password, sf.salt, sf.iters);
    // ClientKey  = HMAC(SaltedPassword, "Client Key")
    auto client_key = hmac_sha256(salted.data(), salted.size(),
        reinterpret_cast<const uint8_t*>("Client Key"), 10);
    // StoredKey  = SHA-256(ClientKey)
    auto stored_key = sha256(client_key.data(), client_key.size());
    // AuthMessage = client-first-bare + "," + server-first + "," + client-final-without-proof
    // c=biws → base64("n,,")
    std::string client_final_no_proof = "c=biws,r=" + sf.r_nonce;
    std::string server_first(
        "r=" + sf.r_nonce + ",s=" + b64_encode(sf.salt.data(), sf.salt.size()) +
        ",i=" + std::to_string(sf.iters));
    std::string auth_msg = cf.bare + "," + server_first + "," + client_final_no_proof;
    // ClientSignature = HMAC(StoredKey, AuthMessage)
    auto client_sig = hmac_sha256(stored_key.data(), stored_key.size(),
        reinterpret_cast<const uint8_t*>(auth_msg.data()), auth_msg.size());
    // ClientProof = ClientKey XOR ClientSignature
    std::vector<uint8_t> proof(client_key.size());
    for (size_t k = 0; k < client_key.size(); ++k)
        proof[k] = client_key[k] ^ client_sig[k];
    art.client_final = client_final_no_proof + ",p=" +
                       b64_encode(proof.data(), proof.size());
    // ServerSignature = HMAC(ServerKey, AuthMessage)
    // ServerKey = HMAC(SaltedPassword, "Server Key")
    auto server_key = hmac_sha256(salted.data(), salted.size(),
        reinterpret_cast<const uint8_t*>("Server Key"), 10);
    auto server_sig = hmac_sha256(server_key.data(), server_key.size(),
        reinterpret_cast<const uint8_t*>(auth_msg.data()), auth_msg.size());
    art.expected_server_sig_b64 = b64_encode(server_sig.data(), server_sig.size());
    return art;
}

inline bool verify_server_final(std::string_view server_final,
                                 std::string_view expected_sig_b64)
{
    // Expect "v=<sig>"
    if (server_final.size() < 2 || server_final.substr(0, 2) != "v=") return false;
    auto sig = server_final.substr(2);
    return sig == expected_sig_b64;
}

} // namespace spido_pg::scram
