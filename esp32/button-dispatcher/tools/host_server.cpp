// host_server.cpp: runs the firmware's platform-independent core on this PC behind a real HTTP server, so the
// web page can be tried (and browser-tested) against the C++ implementation without an ESP32.
//
// It serves the page files from the Pi project's static directory and hands /api requests to dispatcher::Api,
// exactly as the firmware's web server does. There is no Bluetooth: a test route injects advertisements,
//   GET /test/packet?mac=AA:BB:CC:DD:EE:01&hex=44002101643a003a013a003a00&rssi=-40
// standing in for a button press.
//
// This is a development tool, not part of the firmware: the crypto is the slow test implementation, the HTTP
// client speaks plain http:// only, and nothing here is hardened.
//
// Build and run (from the project directory):
//   tools/run_host_server.sh --port 8097
//
// Options:
//   --port N               port to listen on (default 8097)
//   --data-dir DIR         where config.json and auth.json are kept (default /tmp/button-dispatcher-host)
//   --static-dir DIR       the page files (default ../../pi/button-dispatcher/button_dispatcher/static)
//   --switchbot-base URL   SwitchBot API base to use, for example http://127.0.0.1:8096/v1.1
//   --iterations N         PBKDF2 iterations (default 200: this build's SHA-256 is slow)

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "dispatcher/api.hpp"
#include "dispatcher/app.hpp"
#include "dispatcher/platform.hpp"
#include "sha256.hpp"

using namespace dispatcher;

namespace {

std::string read_file(const std::string& path, bool* ok) {
  std::ifstream in(path, std::ios::binary);
  *ok = static_cast<bool>(in);
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

// ---- Platform for the PC ----

class HostPlatform : public Platform {
 public:
  explicit HostPlatform(std::string data_dir) : data_dir_(std::move(data_dir)) {
    mkdir(data_dir_.c_str(), 0700);
    worker_ = std::thread([this] { work(); });
    worker_.detach();
  }

  double monotonic_s() const override {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
  }
  bool wall_clock_valid() const override { return true; }
  int64_t wall_time_ms() const override {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
  }
  void random_bytes(uint8_t* out, size_t length) override {
    std::ifstream urandom("/dev/urandom", std::ios::binary);
    urandom.read(reinterpret_cast<char*>(out), static_cast<std::streamsize>(length));
  }
  std::string hmac_sha256(const std::string& key, const std::string& message) override {
    return test_crypto::hmac_sha256(key, message);
  }
  std::string pbkdf2_hmac_sha256(const std::string& password, const std::string& salt, int iterations,
                                 size_t output_length) override {
    return test_crypto::pbkdf2_hmac_sha256(password, salt, iterations, output_length);
  }

  bool load_document(const std::string& name, std::string* text) override {
    bool ok = false;
    *text = read_file(data_dir_ + "/" + name, &ok);
    return ok;
  }
  bool save_document(const std::string& name, const std::string& text) override {
    const std::string path = data_dir_ + "/" + name;
    const std::string temp = path + ".tmp";
    {
      std::ofstream out(temp, std::ios::binary | std::ios::trunc);
      out << text;
      if (!out) return false;
    }
    return std::rename(temp.c_str(), path.c_str()) == 0;
  }

  HttpResponse http_request(const std::string& method, const std::string& url, const HeaderList& headers,
                            const std::string& body, double timeout_s, size_t max_response_bytes) override {
    HttpResponse response;
    // Plain http://host[:port]/path only.
    if (url.rfind("http://", 0) != 0) {
      response.failure = HttpResponse::Failure::kConnect;
      return response;
    }
    const size_t host_start = 7;
    const size_t path_start = url.find('/', host_start);
    std::string authority = url.substr(host_start, path_start == std::string::npos ? std::string::npos : path_start - host_start);
    const std::string path = path_start == std::string::npos ? "/" : url.substr(path_start);
    std::string host = authority;
    std::string port = "80";
    const size_t colon = authority.rfind(':');
    if (colon != std::string::npos) {
      host = authority.substr(0, colon);
      port = authority.substr(colon + 1);
    }

    addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* found = nullptr;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &found) != 0 || found == nullptr) {
      response.failure = HttpResponse::Failure::kConnect;
      return response;
    }
    const int fd = socket(found->ai_family, found->ai_socktype, found->ai_protocol);
    timeval timeout;
    timeout.tv_sec = static_cast<long>(timeout_s);
    timeout.tv_usec = static_cast<long>((timeout_s - static_cast<long>(timeout_s)) * 1e6);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    if (fd < 0 || connect(fd, found->ai_addr, found->ai_addrlen) != 0) {
      if (fd >= 0) close(fd);
      freeaddrinfo(found);
      response.failure = HttpResponse::Failure::kConnect;
      return response;
    }
    freeaddrinfo(found);

    std::string request = method + " " + path + " HTTP/1.1\r\nHost: " + authority + "\r\nConnection: close\r\n";
    for (const auto& header : headers) request += header.first + ": " + header.second + "\r\n";
    if (!body.empty()) request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    request += "\r\n" + body;
    if (send(fd, request.data(), request.size(), 0) < 0) {
      close(fd);
      response.failure = HttpResponse::Failure::kConnect;
      return response;
    }

    std::string raw;
    char chunk[2048];
    while (true) {
      const ssize_t count = recv(fd, chunk, sizeof(chunk), 0);
      if (count > 0) {
        raw.append(chunk, static_cast<size_t>(count));
        continue;
      }
      if (count < 0) {
        close(fd);
        response.failure = HttpResponse::Failure::kTimeout;
        return response;
      }
      break;
    }
    close(fd);

    const size_t line_end = raw.find("\r\n");
    const size_t header_end = raw.find("\r\n\r\n");
    if (line_end == std::string::npos || header_end == std::string::npos) {
      response.failure = HttpResponse::Failure::kConnect;
      return response;
    }
    const std::string status_line = raw.substr(0, line_end);  // "HTTP/1.1 200 OK"
    const size_t first_space = status_line.find(' ');
    response.status = std::atoi(status_line.c_str() + first_space + 1);
    const size_t second_space = status_line.find(' ', first_space + 1);
    if (second_space != std::string::npos) response.reason = status_line.substr(second_space + 1);
    response.body = raw.substr(header_end + 4);
    if (response.body.size() > max_response_bytes) response.body.resize(max_response_bytes);
    return response;
  }

  void run_in_background(std::function<void()> task) override {
    {
      std::lock_guard<std::mutex> lock(queue_mutex_);
      queue_.push_back(std::move(task));
    }
    queue_condition_.notify_one();
  }

 private:
  void work() {
    while (true) {
      std::function<void()> task;
      {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        queue_condition_.wait(lock, [this] { return !queue_.empty(); });
        task = std::move(queue_.front());
        queue_.pop_front();
      }
      task();
    }
  }

  std::string data_dir_;
  std::thread worker_;
  std::mutex queue_mutex_;
  std::condition_variable queue_condition_;
  std::deque<std::function<void()>> queue_;
};

// ---- a very small HTTP server ----

struct Options {
  int port = 8097;
  std::string data_dir = "/tmp/button-dispatcher-host";
  std::string static_dir = "../../pi/button-dispatcher/button_dispatcher/static";
  std::string switchbot_base = kSwitchBotApiBase;
  int iterations = 200;
};

std::string percent_decode(const std::string& text) {
  std::string out;
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '+') {
      out += ' ';
    } else if (text[i] == '%' && i + 2 < text.size() + 0 && std::isxdigit(static_cast<unsigned char>(text[i + 1])) &&
               std::isxdigit(static_cast<unsigned char>(text[i + 2]))) {
      out += static_cast<char>(std::stoi(text.substr(i + 1, 2), nullptr, 16));
      i += 2;
    } else {
      out += text[i];
    }
  }
  return out;
}

std::string reason_for(int status) {
  switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    case 429: return "Too Many Requests";
    case 502: return "Bad Gateway";
    default: return "OK";
  }
}

void send_all(int fd, const std::string& data) {
  size_t sent = 0;
  while (sent < data.size()) {
    const ssize_t count = send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
    if (count <= 0) return;
    sent += static_cast<size_t>(count);
  }
}

void respond(int fd, int status, const std::string& content_type, const std::string& body, const HeaderList& extra,
             bool is_api) {
  std::string head = "HTTP/1.1 " + std::to_string(status) + " " + reason_for(status) + "\r\n";
  head += "Content-Type: " + content_type + "\r\n";
  head += "Content-Length: " + std::to_string(body.size()) + "\r\n";
  head += "Connection: close\r\n";
  head += "X-Content-Type-Options: nosniff\r\nX-Frame-Options: DENY\r\nReferrer-Policy: no-referrer\r\n";
  head += "Content-Security-Policy: default-src 'self'; frame-ancestors 'none'\r\n";
  head += std::string("Cache-Control: ") + (is_api ? "no-store" : "no-cache") + "\r\n";
  for (const auto& header : extra) head += header.first + ": " + header.second + "\r\n";
  send_all(fd, head + "\r\n" + body);
}

void handle_connection(int fd, const std::string& client, AppCore& app, Api& api, const Options& options) {
  std::string raw;
  char chunk[4096];
  size_t header_end = std::string::npos;
  while (header_end == std::string::npos) {
    const ssize_t count = recv(fd, chunk, sizeof(chunk), 0);
    if (count <= 0) {
      close(fd);
      return;
    }
    raw.append(chunk, static_cast<size_t>(count));
    header_end = raw.find("\r\n\r\n");
    if (raw.size() > 64 * 1024) {
      close(fd);
      return;
    }
  }
  std::istringstream head(raw.substr(0, header_end));
  std::string method, target, version;
  head >> method >> target >> version;
  std::map<std::string, std::string> headers;
  std::string line;
  std::getline(head, line);
  while (std::getline(head, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const size_t colon = line.find(':');
    if (colon == std::string::npos) continue;
    std::string name = line.substr(0, colon);
    for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    size_t value_start = colon + 1;
    while (value_start < line.size() && line[value_start] == ' ') ++value_start;
    headers[name] = line.substr(value_start);
  }
  size_t content_length = headers.count("content-length") ? std::strtoul(headers["content-length"].c_str(), nullptr, 10) : 0;
  std::string body = raw.substr(header_end + 4);
  if (content_length > 32 * 1024) {
    respond(fd, 413, "application/json", "{\"error\": \"request too large\"}", {}, true);
    close(fd);
    return;
  }
  while (body.size() < content_length) {
    const ssize_t count = recv(fd, chunk, sizeof(chunk), 0);
    if (count <= 0) break;
    body.append(chunk, static_cast<size_t>(count));
  }

  Request request;
  request.method = method;
  const size_t question = target.find('?');
  request.path = target.substr(0, question);
  if (question != std::string::npos) {
    std::string query = target.substr(question + 1);
    size_t start = 0;
    while (start <= query.size()) {
      size_t end = query.find('&', start);
      if (end == std::string::npos) end = query.size();
      const std::string pair = query.substr(start, end - start);
      if (!pair.empty()) {
        const size_t equals = pair.find('=');
        request.query[percent_decode(pair.substr(0, equals))] =
            equals == std::string::npos ? "" : percent_decode(pair.substr(equals + 1));
      }
      start = end + 1;
    }
  }
  request.body = body;
  request.cookie = headers.count("cookie") ? headers["cookie"] : "";
  request.csrf = headers.count("x-button-dispatcher") ? headers["x-button-dispatcher"] : "";
  request.client = client;

  if (request.path == "/test/packet") {
    // A stand-in for the Bluetooth scanner: feeds one advertisement to the application core.
    std::string mac = request.query["mac"];
    const std::string hex = request.query["hex"];
    std::string bytes;
    for (size_t i = 0; i + 1 < hex.size(); i += 2) bytes += static_cast<char>(std::stoi(hex.substr(i, 2), nullptr, 16));
    const int rssi = request.query.count("rssi") ? std::atoi(request.query["rssi"].c_str()) : -40;
    if (request.query.count("name")) {
      std::lock_guard<std::mutex> lock(app.mutex);
      app.names[mac] = request.query["name"];
    }
    app.handle_packet(mac, rssi, reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
    respond(fd, 200, "text/plain", "ok", {}, true);
  } else if (request.path.rfind("/api/", 0) == 0) {
    const Response response = api.handle(request);
    respond(fd, response.status, response.content_type, response.body, response.headers, true);
  } else if (method == "GET") {
    std::string file = request.path == "/" ? "index.html" : request.path.substr(request.path.rfind('/') + 1);
    if (request.path != "/" && request.path.rfind("/static/", 0) != 0) file.clear();
    bool ok = false;
    const std::string content = file.empty() ? "" : read_file(options.static_dir + "/" + file, &ok);
    std::string type = "text/plain";
    if (file.size() > 5 && file.substr(file.size() - 5) == ".html") type = "text/html; charset=utf-8";
    if (file.size() > 3 && file.substr(file.size() - 3) == ".js") type = "text/javascript; charset=utf-8";
    if (file.size() > 4 && file.substr(file.size() - 4) == ".css") type = "text/css; charset=utf-8";
    if (file.size() > 4 && file.substr(file.size() - 4) == ".svg") type = "image/svg+xml";
    if (ok) respond(fd, 200, type, content, {}, false);
    else respond(fd, 404, "application/json", "{\"error\": \"not found\"}", {}, true);
  } else {
    respond(fd, 404, "application/json", "{\"error\": \"not found\"}", {}, true);
  }
  close(fd);
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  for (int i = 1; i + 1 < argc; i += 2) {
    const std::string flag = argv[i];
    const std::string value = argv[i + 1];
    if (flag == "--port") options.port = std::atoi(value.c_str());
    else if (flag == "--data-dir") options.data_dir = value;
    else if (flag == "--static-dir") options.static_dir = value;
    else if (flag == "--switchbot-base") options.switchbot_base = value;
    else if (flag == "--iterations") options.iterations = std::atoi(value.c_str());
    else {
      std::cerr << "unknown option " << flag << "\n";
      return 2;
    }
  }

  HostPlatform platform(options.data_dir);
  AppCore app(platform, options.iterations, options.switchbot_base);
  std::string error;
  app.init(&error);
  if (!error.empty()) std::cerr << error << "\n";
  Api api(app);

  const int listener = socket(AF_INET, SOCK_STREAM, 0);
  int yes = 1;
  setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  sockaddr_in address = {};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(static_cast<uint16_t>(options.port));
  if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || listen(listener, 16) != 0) {
    std::perror("bind/listen");
    return 1;
  }
  std::cout << "host server ready on http://127.0.0.1:" << options.port << "/" << std::endl;
  while (true) {
    sockaddr_in peer = {};
    socklen_t peer_length = sizeof(peer);
    const int fd = accept(listener, reinterpret_cast<sockaddr*>(&peer), &peer_length);
    if (fd < 0) continue;
    char text[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &peer.sin_addr, text, sizeof(text));
    std::thread([fd, client = std::string(text), &app, &api, &options] {
      handle_connection(fd, client, app, api, options);
    }).detach();
  }
}
