#pragma once

// SHA-256 — used to build cache keys from (sql, params). No OpenSSL dep so
// the cache layer compiles even when callers don't link libcrypto.

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace spido_pg::sha256 {

struct Ctx {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t  buf[64];
    size_t   bufused;
};

inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

inline void init(Ctx& c) {
    static const uint32_t H[8] = {
        0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
        0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19
    };
    std::memcpy(c.state, H, sizeof H);
    c.bitlen = 0; c.bufused = 0;
}

inline void transform(Ctx& c, const uint8_t* blk) {
    static const uint32_t K[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
    };
    uint32_t W[64];
    for (int i = 0; i < 16; ++i)
        W[i] = (uint32_t(blk[i*4])<<24) | (uint32_t(blk[i*4+1])<<16) |
               (uint32_t(blk[i*4+2])<<8) | uint32_t(blk[i*4+3]);
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = rotr(W[i-15],7) ^ rotr(W[i-15],18) ^ (W[i-15]>>3);
        uint32_t s1 = rotr(W[i-2],17) ^ rotr(W[i-2],19)  ^ (W[i-2]>>10);
        W[i] = W[i-16] + s0 + W[i-7] + s1;
    }
    uint32_t a=c.state[0],b=c.state[1],cc=c.state[2],d=c.state[3];
    uint32_t e=c.state[4],f=c.state[5],g=c.state[6],h=c.state[7];
    for (int i = 0; i < 64; ++i) {
        uint32_t S1 = rotr(e,6) ^ rotr(e,11) ^ rotr(e,25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = h + S1 + ch + K[i] + W[i];
        uint32_t S0 = rotr(a,2) ^ rotr(a,13) ^ rotr(a,22);
        uint32_t mj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1;
        d = cc; cc = b; b = a; a = t1 + t2;
    }
    c.state[0]+=a; c.state[1]+=b; c.state[2]+=cc; c.state[3]+=d;
    c.state[4]+=e; c.state[5]+=f; c.state[6]+=g;  c.state[7]+=h;
}

inline void update(Ctx& c, const void* data, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    c.bitlen += len * 8;
    while (len > 0) {
        size_t take = 64 - c.bufused;
        if (take > len) take = len;
        std::memcpy(c.buf + c.bufused, p, take);
        c.bufused += take; p += take; len -= take;
        if (c.bufused == 64) { transform(c, c.buf); c.bufused = 0; }
    }
}

inline void finish(Ctx& c, uint8_t out[32]) {
    c.buf[c.bufused++] = 0x80;
    if (c.bufused > 56) {
        while (c.bufused < 64) c.buf[c.bufused++] = 0;
        transform(c, c.buf); c.bufused = 0;
    }
    while (c.bufused < 56) c.buf[c.bufused++] = 0;
    uint64_t bl = c.bitlen;
    for (int i = 7; i >= 0; --i) c.buf[56 + (7 - i)] = (bl >> (i*8)) & 0xff;
    transform(c, c.buf);
    for (int i = 0; i < 8; ++i) {
        out[i*4+0] = (c.state[i] >> 24) & 0xff;
        out[i*4+1] = (c.state[i] >> 16) & 0xff;
        out[i*4+2] = (c.state[i] >>  8) & 0xff;
        out[i*4+3] = (c.state[i]      ) & 0xff;
    }
}

inline std::string hex(std::string_view a, std::string_view b = {}) {
    Ctx c; init(c);
    update(c, a.data(), a.size());
    if (!b.empty()) update(c, b.data(), b.size());
    uint8_t out[32]; finish(c, out);
    static const char* H = "0123456789abcdef";
    std::string s; s.resize(64);
    for (int i = 0; i < 32; ++i) {
        s[i*2+0] = H[out[i] >> 4];
        s[i*2+1] = H[out[i] & 0xf];
    }
    return s;
}

} // namespace spido_pg::sha256
