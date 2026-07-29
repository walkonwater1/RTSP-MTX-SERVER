#pragma once
/**
 * Minimal Base64 encoder/decoder — no external dependencies.
 */
#include <string>
#include <cstdint>
#include <vector>

namespace base64 {

static const char kChars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

inline std::string encode(const uint8_t* data, size_t len) {
  std::string out;
  out.reserve((len + 2) / 3 * 4);
  int val = 0, valb = -6;
  for (size_t i = 0; i < len; ++i) {
    val = (val << 8) + data[i];
    valb += 8;
    while (valb >= 0) {
      out.push_back(kChars[(val >> valb) & 0x3F]);
      valb -= 6;
    }
  }
  if (valb > -6) out.push_back(kChars[((val << 8) >> (valb + 8)) & 0x3F]);
  while (out.size() % 4) out.push_back('=');
  return out;
}

inline std::string encode(const std::string& data) {
  return encode(reinterpret_cast<const uint8_t*>(data.data()), data.size());
}

inline std::string decode(const std::string& encoded) {
  std::string out;
  out.reserve(encoded.size() * 3 / 4);
  int val = 0, valb = -8;
  for (unsigned char c : encoded) {
    if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
    int idx = -1;
    if (c >= 'A' && c <= 'Z') idx = c - 'A';
    else if (c >= 'a' && c <= 'z') idx = c - 'a' + 26;
    else if (c >= '0' && c <= '9') idx = c - '0' + 52;
    else if (c == '+') idx = 62;
    else if (c == '/') idx = 63;
    if (idx < 0) continue;
    val = (val << 6) + idx;
    valb += 6;
    if (valb >= 0) {
      out.push_back(static_cast<char>((val >> valb) & 0xFF));
      valb -= 8;
    }
  }
  return out;
}

} // namespace base64
