// Everything the platform-independent core needs from the outside world.
//
// The firmware implements this with ESP-IDF (esp_timer, mbedtls, FAT storage, esp_http_client, a worker
// task); the host tests implement it with fakes. Keeping all of it behind one interface is what lets the
// whole API, the auth and the SwitchBot logic run in unit tests on a PC.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace dispatcher {

using HeaderList = std::vector<std::pair<std::string, std::string>>;

// The outcome of one HTTP request at the transport level.
struct HttpResponse {
  enum class Failure { kNone, kTimeout, kConnect };
  Failure failure = Failure::kNone;  // anything but kNone means no response was received
  int status = 0;
  std::string reason;                // "OK", "Not Found", ... when known
  std::string body;                  // at most max_response_bytes of it
};

class Platform {
 public:
  virtual ~Platform() = default;

  // ---- time ----
  // Seconds from a monotonic clock; never jumps.
  virtual double monotonic_s() const = 0;
  // Whether the wall clock has been set (for example by SNTP). SwitchBot request signing and HTTPS
  // certificate checks need it.
  virtual bool wall_clock_valid() const = 0;
  // Milliseconds since the Unix epoch; only meaningful when wall_clock_valid().
  virtual int64_t wall_time_ms() const = 0;

  // ---- randomness and crypto (raw bytes in and out) ----
  virtual void random_bytes(uint8_t* out, size_t length) = 0;
  virtual std::string hmac_sha256(const std::string& key, const std::string& message) = 0;
  virtual std::string pbkdf2_hmac_sha256(const std::string& password, const std::string& salt, int iterations,
                                         size_t output_length) = 0;

  // ---- persistence: whole documents, replaced as a unit ----
  // Reads the stored document `name` ("config.json", "auth.json"); false if there is none.
  virtual bool load_document(const std::string& name, std::string* text) = 0;
  // Stores the document; false if it could not be stored.
  virtual bool save_document(const std::string& name, const std::string& text) = 0;

  // ---- network ----
  // Sends an HTTP or HTTPS request (redirects followed, certificates verified) and waits for the answer.
  // Blocks, so callers must not hold locks while calling it. Reads at most max_response_bytes of the body.
  virtual HttpResponse http_request(const std::string& method, const std::string& url, const HeaderList& headers,
                                    const std::string& body, double timeout_s, size_t max_response_bytes) = 0;

  // ---- background work ----
  // Runs `task` soon on a worker thread, one task at a time, in the order submitted. Used for the actions
  // that fire when a button is pressed, so a slow HTTPS request never delays Bluetooth processing.
  virtual void run_in_background(std::function<void()> task) = 0;
};

}  // namespace dispatcher
