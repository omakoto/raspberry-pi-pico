// A fake Platform for the host tests: manual clocks, deterministic randomness, in-memory documents,
// scripted HTTP answers, and a background queue the test can run or inspect.

#pragma once

#include <algorithm>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "dispatcher/platform.hpp"
#include "sha256.hpp"

namespace test_support {

struct RecordedRequest {
  std::string method;
  std::string url;
  dispatcher::HeaderList headers;
  std::string body;
  double timeout_s = 0;
};

class FakePlatform : public dispatcher::Platform {
 public:
  // ---- knobs for tests ----
  double mono = 1000.0;
  bool clock_valid = true;
  int64_t wall_ms = 1700000000000LL;
  std::map<std::string, std::string> documents;
  bool fail_saves = false;
  // Answers each HTTP request; by default every request fails to connect.
  std::function<dispatcher::HttpResponse(const RecordedRequest&)> http_handler = [](const RecordedRequest&) {
    dispatcher::HttpResponse response;
    response.failure = dispatcher::HttpResponse::Failure::kConnect;
    return response;
  };
  std::vector<RecordedRequest> requests;
  // When true, background tasks run immediately inside run_in_background; otherwise they queue up.
  bool run_background_immediately = true;
  std::vector<std::function<void()>> background;
  int pbkdf2_calls = 0;

  void run_queued_background() {
    std::vector<std::function<void()>> tasks;
    tasks.swap(background);
    for (auto& task : tasks) task();
  }

  // ---- Platform ----
  double monotonic_s() const override { return mono; }
  bool wall_clock_valid() const override { return clock_valid; }
  int64_t wall_time_ms() const override { return wall_ms; }

  void random_bytes(uint8_t* out, size_t length) override {
    std::lock_guard<std::mutex> lock(rng_mutex_);
    for (size_t i = 0; i < length; ++i) {
      rng_ ^= rng_ << 13;
      rng_ ^= rng_ >> 7;
      rng_ ^= rng_ << 17;
      out[i] = static_cast<uint8_t>(rng_ >> 24);
    }
  }
  std::string hmac_sha256(const std::string& key, const std::string& message) override {
    return test_crypto::hmac_sha256(key, message);
  }
  std::string pbkdf2_hmac_sha256(const std::string& password, const std::string& salt, int iterations,
                                 size_t output_length) override {
    ++pbkdf2_calls;
    return test_crypto::pbkdf2_hmac_sha256(password, salt, iterations, output_length);
  }

  bool load_document(const std::string& name, std::string* text) override {
    auto it = documents.find(name);
    if (it == documents.end()) return false;
    *text = it->second;
    return true;
  }
  bool save_document(const std::string& name, const std::string& text) override {
    if (fail_saves) return false;
    documents[name] = text;
    return true;
  }

  dispatcher::HttpResponse http_request(const std::string& method, const std::string& url,
                                        const dispatcher::HeaderList& headers, const std::string& body,
                                        double timeout_s, size_t max_response_bytes) override {
    RecordedRequest request{method, url, headers, body, timeout_s};
    {
      // Tests may press buttons from several threads; the handler itself runs unlocked.
      std::lock_guard<std::mutex> lock(requests_mutex_);
      requests.push_back(request);
    }
    dispatcher::HttpResponse response = http_handler(request);
    if (response.body.size() > max_response_bytes) response.body.resize(max_response_bytes);
    return response;
  }

  void run_in_background(std::function<void()> task) override {
    if (run_background_immediately) {
      task();
    } else {
      background.push_back(std::move(task));
    }
  }

 private:
  std::mutex requests_mutex_;
  std::mutex rng_mutex_;
  uint64_t rng_ = 0x9E3779B97F4A7C15ULL;
};

// Header lookup helper (case-insensitive name).
inline const std::string* find_header(const dispatcher::HeaderList& headers, const std::string& name) {
  for (const auto& header : headers) {
    if (header.first.size() != name.size()) continue;
    bool same = true;
    for (size_t i = 0; i < name.size(); ++i) {
      if (std::tolower(static_cast<unsigned char>(header.first[i])) != std::tolower(static_cast<unsigned char>(name[i]))) {
        same = false;
        break;
      }
    }
    if (same) return &header.second;
  }
  return nullptr;
}

}  // namespace test_support
