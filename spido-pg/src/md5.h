#pragma once

// MD5 — used only for PostgreSQL's legacy "md5" auth method. Yes, we know.
// SCRAM-SHA-256 would be saner; many self-hosted clusters still default to
// md5 in pg_hba.conf though, so we support it.

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace spido_pg::md5 {

struct Ctx {
    uint32_t state[4];
    uint64_t bitlen;
    uint8_t  buf[64];
    size_t   bufused;
};

inline uint32_t rotl(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

inline void init(Ctx& c) {
    c.state[0] = 0x67452301; c.state[1] = 0xefcdab89;
    c.state[2] = 0x98badcfe; c.state[3] = 0x10325476;
    c.bitlen = 0; c.bufused = 0;
}

inline void transform(Ctx& c, const uint8_t* block) {
    static const uint32_t K[64] = {
        0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
        0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
        0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
        0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
        0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
        0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
        0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
        0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391
    };
    static const int S[64] = {
        7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
        5, 9,14,20,5, 9,14,20,5, 9,14,20,5, 9,14,20,
        4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
        6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21
    };
    uint32_t M[16];
    for (int i = 0; i < 16; ++i) {
        M[i] = (uint32_t)block[i*4+0] |
               ((uint32_t)block[i*4+1] << 8) |
               ((uint32_t)block[i*4+2] << 16) |
               ((uint32_t)block[i*4+3] << 24);
    }
    uint32_t a=c.state[0],b=c.state[1],cc=c.state[2],d=c.state[3];
    for (int i = 0; i < 64; ++i) {
        uint32_t f, g;
        if (i < 16)      { f = (b & cc) | ((~b) & d);                g = i; }
        else if (i < 32) { f = (d & b) | ((~d) & cc);                g = (5*i + 1) % 16; }
        else if (i < 48) { f = b ^ cc ^ d;                            g = (3*i + 5) % 16; }
        else             { f = cc ^ (b | (~d));                       g = (7*i) % 16; }
        uint32_t tmp = d;
        d  = cc;
        cc = b;
        b  = b + rotl(a + f + K[i] + M[g], S[i]);
        a  = tmp;
    }
    c.state[0]+=a; c.state[1]+=b; c.state[2]+=cc; c.state[3]+=d;
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

inline void finish(Ctx& c, uint8_t out[16]) {
    c.buf[c.bufused++] = 0x80;
    if (c.bufused > 56) {
        while (c.bufused < 64) c.buf[c.bufused++] = 0;
        transform(c, c.buf); c.bufused = 0;
    }
    while (c.bufused < 56) c.buf[c.bufused++] = 0;
    uint64_t bl = c.bitlen;
    for (int i = 0; i < 8; ++i) c.buf[56+i] = (bl >> (i*8)) & 0xff;
    transform(c, c.buf);
    for (int i = 0; i < 4; ++i) {
        out[i*4+0] = (c.state[i]      ) & 0xff;
        out[i*4+1] = (c.state[i] >>  8) & 0xff;
        out[i*4+2] = (c.state[i] >> 16) & 0xff;
        out[i*4+3] = (c.state[i] >> 24) & 0xff;
    }
}

// Lowercase hex of MD5(data). 32 chars + NUL on success.
inline std::string hex(std::string_view data) {
    Ctx c; init(c); update(c, data.data(), data.size());
    uint8_t out[16]; finish(c, out);
    static const char* H = "0123456789abcdef";
    std::string s; s.resize(32);
    for (int i = 0; i < 16; ++i) {
        s[i*2+0] = H[out[i] >> 4];
        s[i*2+1] = H[out[i] & 0xf];
    }
    return s;
}

// PG md5 auth: "md5" + md5_hex( md5_hex(password + user) + salt )
inline std::string pg_password(std::string_view password,
                               std::string_view user,
                               const uint8_t salt[4]) {
    std::string a; a.reserve(password.size() + user.size());
    a.append(password); a.append(user);
    std::string inner = hex(a);
    inner.append(reinterpret_cast<const char*>(salt), 4);
    return "md5" + hex(inner);
}

} // namespace spido_pg::md5
