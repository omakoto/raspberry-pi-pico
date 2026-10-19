// Small helpers shared by the host tests.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace test_util {

// "44001f" -> {0x44, 0x00, 0x1f}. Stops at the first character pair that is not hex.
inline std::vector<uint8_t> from_hex(const std::string& hex) {
  auto value = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  std::vector<uint8_t> bytes;
  for (size_t i = 0; i + 1 < hex.size(); i += 2) {
    const int hi = value(hex[i]);
    const int lo = value(hex[i + 1]);
    if (hi < 0 || lo < 0) break;
    bytes.push_back(static_cast<uint8_t>(hi * 16 + lo));
  }
  return bytes;
}

}  // namespace test_util
