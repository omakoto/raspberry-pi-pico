// A small SHA-256, HMAC-SHA256 and PBKDF2-HMAC-SHA256 for the host tests, so the fake platform can offer
// real crypto (the firmware uses mbedtls). Checked against published test vectors in test_support.cpp.
// Not for production use: it is not hardened and not fast.

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>

namespace test_crypto {

class Sha256 {
 public:
  Sha256() { reset(); }

  void reset() {
    static const uint32_t init[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::memcpy(h_, init, sizeof(h_));
    length_ = 0;
    fill_ = 0;
  }

  void update(const void* data, size_t size) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    length_ += size;
    while (size > 0) {
      const size_t take = std::min(size, sizeof(block_) - fill_);
      std::memcpy(block_ + fill_, p, take);
      fill_ += take;
      p += take;
      size -= take;
      if (fill_ == sizeof(block_)) {
        compress(block_);
        fill_ = 0;
      }
    }
  }

  std::string finish() {
    const uint64_t bit_length = length_ * 8;
    const uint8_t one = 0x80;
    update(&one, 1);
    const uint8_t zero = 0;
    while (fill_ != 56) update(&zero, 1);
    uint8_t tail[8];
    for (int i = 0; i < 8; ++i) tail[i] = static_cast<uint8_t>(bit_length >> (56 - 8 * i));
    update(tail, 8);
    std::string out(32, '\0');
    for (int i = 0; i < 8; ++i) {
      for (int j = 0; j < 4; ++j) out[i * 4 + j] = static_cast<char>(h_[i] >> (24 - 8 * j));
    }
    return out;
  }

 private:
  static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

  void compress(const uint8_t* block) {
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
      w[i] = (static_cast<uint32_t>(block[i * 4]) << 24) | (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
             (static_cast<uint32_t>(block[i * 4 + 2]) << 8) | block[i * 4 + 3];
    }
    for (int i = 16; i < 64; ++i) {
      const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], hh = h_[7];
    for (int i = 0; i < 64; ++i) {
      const uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
      const uint32_t ch = (e & f) ^ (~e & g);
      const uint32_t t1 = hh + s1 + ch + k[i] + w[i];
      const uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
      const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
      const uint32_t t2 = s0 + maj;
      hh = g;
      g = f;
      f = e;
      e = d + t1;
      d = c;
      c = b;
      b = a;
      a = t1 + t2;
    }
    h_[0] += a;
    h_[1] += b;
    h_[2] += c;
    h_[3] += d;
    h_[4] += e;
    h_[5] += f;
    h_[6] += g;
    h_[7] += hh;
  }

  uint32_t h_[8];
  uint8_t block_[64];
  size_t fill_;
  uint64_t length_;
};

inline std::string sha256(const std::string& data) {
  Sha256 hash;
  hash.update(data.data(), data.size());
  return hash.finish();
}

// Raw 32-byte HMAC-SHA256.
inline std::string hmac_sha256(const std::string& key_in, const std::string& message) {
  std::string key = key_in.size() > 64 ? sha256(key_in) : key_in;
  key.resize(64, '\0');
  std::string inner(64, '\0'), outer(64, '\0');
  for (size_t i = 0; i < 64; ++i) {
    inner[i] = static_cast<char>(key[i] ^ 0x36);
    outer[i] = static_cast<char>(key[i] ^ 0x5c);
  }
  return sha256(outer + sha256(inner + message));
}

// Raw PBKDF2-HMAC-SHA256 output of `output_length` bytes.
inline std::string pbkdf2_hmac_sha256(const std::string& password, const std::string& salt, int iterations,
                                      size_t output_length) {
  std::string out;
  for (uint32_t block = 1; out.size() < output_length; ++block) {
    std::string index;
    for (int shift = 24; shift >= 0; shift -= 8) index += static_cast<char>(block >> shift);
    std::string u = hmac_sha256(password, salt + index);
    std::string t = u;
    for (int i = 1; i < iterations; ++i) {
      u = hmac_sha256(password, u);
      for (size_t j = 0; j < t.size(); ++j) t[j] = static_cast<char>(t[j] ^ u[j]);
    }
    out += t;
  }
  out.resize(output_length);
  return out;
}

}  // namespace test_crypto
