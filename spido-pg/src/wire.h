#pragma once

// Internal header — protocol-level helpers shared between connection.cpp,
// pool.cpp, notify.cpp. Not exported.

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace spido_pg::wire {

// PostgreSQL frontend message tags.
constexpr char kQuery       = 'Q';
constexpr char kParse       = 'P';
constexpr char kBind        = 'B';
constexpr char kDescribe    = 'D';
constexpr char kExecute     = 'E';
constexpr char kSync        = 'S';
constexpr char kClose       = 'C';
constexpr char kFlush       = 'H';
constexpr char kTerminate   = 'X';
constexpr char kPassword    = 'p';

// Backend message tags.
constexpr char kAuthentication = 'R';
constexpr char kParameterStatus= 'S';
constexpr char kBackendKeyData = 'K';
constexpr char kReadyForQuery  = 'Z';
constexpr char kRowDescription = 'T';
constexpr char kDataRow        = 'D';
constexpr char kCommandComplete= 'C';
constexpr char kErrorResponse  = 'E';
constexpr char kNoticeResponse = 'N';
constexpr char kParameterDesc  = 't';
constexpr char kNoData         = 'n';
constexpr char kParseComplete  = '1';
constexpr char kBindComplete   = '2';
constexpr char kCloseComplete  = '3';
constexpr char kEmptyQuery     = 'I';
constexpr char kPortalSuspended= 's';
constexpr char kNotification   = 'A';
constexpr char kCopyInResponse = 'G';
constexpr char kCopyOutResponse= 'H';

// AuthenticationRequest subtypes (uint32_t after 'R' frame header).
constexpr uint32_t kAuthOk          = 0;
constexpr uint32_t kAuthCleartext   = 3;
constexpr uint32_t kAuthMD5         = 5;
constexpr uint32_t kAuthSASL        = 10;
constexpr uint32_t kAuthSASLContinue= 11;
constexpr uint32_t kAuthSASLFinal   = 12;

// ---------- byte writers ----------

inline void put_u8(std::vector<uint8_t>& buf, uint8_t v)        { buf.push_back(v); }
inline void put_i16(std::vector<uint8_t>& buf, int16_t v) {
    uint16_t u = static_cast<uint16_t>(v);
    buf.push_back(static_cast<uint8_t>(u >> 8));
    buf.push_back(static_cast<uint8_t>(u));
}
inline void put_i32(std::vector<uint8_t>& buf, int32_t v) {
    uint32_t u = static_cast<uint32_t>(v);
    buf.push_back(static_cast<uint8_t>(u >> 24));
    buf.push_back(static_cast<uint8_t>(u >> 16));
    buf.push_back(static_cast<uint8_t>(u >> 8));
    buf.push_back(static_cast<uint8_t>(u));
}
inline void put_i64(std::vector<uint8_t>& buf, int64_t v) {
    uint64_t u = static_cast<uint64_t>(v);
    for (int i = 7; i >= 0; --i) buf.push_back(static_cast<uint8_t>(u >> (i*8)));
}
inline void put_str(std::vector<uint8_t>& buf, std::string_view s) {
    buf.insert(buf.end(), s.begin(), s.end());
    buf.push_back(0); // NUL
}
inline void put_bytes(std::vector<uint8_t>& buf, const void* data, size_t n) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    buf.insert(buf.end(), p, p+n);
}

// Patch a previously-written 4-byte length placeholder. `len_offset` points
// at the first byte of the placeholder; written length includes the 4 bytes
// of the length itself.
inline void patch_len(std::vector<uint8_t>& buf, size_t len_offset) {
    uint32_t len = static_cast<uint32_t>(buf.size() - len_offset);
    buf[len_offset+0] = static_cast<uint8_t>(len >> 24);
    buf[len_offset+1] = static_cast<uint8_t>(len >> 16);
    buf[len_offset+2] = static_cast<uint8_t>(len >> 8);
    buf[len_offset+3] = static_cast<uint8_t>(len);
}

// ---------- byte readers ----------

inline int16_t  get_i16(const uint8_t* p) {
    return static_cast<int16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}
inline int32_t  get_i32(const uint8_t* p) {
    return static_cast<int32_t>(
        (static_cast<uint32_t>(p[0]) << 24) |
        (static_cast<uint32_t>(p[1]) << 16) |
        (static_cast<uint32_t>(p[2]) <<  8) |
         static_cast<uint32_t>(p[3]));
}
inline int64_t  get_i64(const uint8_t* p) {
    uint64_t u = 0;
    for (int i = 0; i < 8; ++i) u = (u << 8) | p[i];
    return static_cast<int64_t>(u);
}

} // namespace spido_pg::wire
