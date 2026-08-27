#pragma once
#include <string>
#include <openssl/hmac.h>
#include <sstream>
#include <iomanip>

namespace trader {

// HMAC-SHA256/SHA512 signing helpers used to sign private Poloniex trading
// API requests. Uses OpenSSL rather than any hand-rolled crypto (unlike the
// legacy codebase, which additionally hid the *key material itself* behind
// a bespoke "simplecrypt" obfuscation scheme - an anti-pattern this project
// deliberately does not replicate).
// Keys are expected via environment variables, never hardcoded/obfuscated
// in source or committed files.
inline std::string hmacHex(const std::string& key, const std::string& data, const EVP_MD* md) {
    unsigned char result[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    HMAC(md, key.data(), static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char*>(data.data()), data.size(), result, &len);
    std::ostringstream oss;
    for (unsigned int i = 0; i < len; ++i) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(result[i]);
    }
    return oss.str();
}

inline std::string hmacSha256Hex(const std::string& key, const std::string& data) {
    return hmacHex(key, data, EVP_sha256());
}

inline std::string hmacSha512Hex(const std::string& key, const std::string& data) {
    return hmacHex(key, data, EVP_sha512());
}

} // namespace trader
