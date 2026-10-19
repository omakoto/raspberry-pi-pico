// The web API: the JSON routes the page talks to, as a pure function from a request to a response.
//
// Mirrors the routes of pi/button-dispatcher/button_dispatcher/app.py, with the same paths, the same JSON
// shapes and the same error messages, so the one web page works with both implementations. The HTTP layer
// (esp_http_server on the device) only converts to and from Request/Response, serves the static page files,
// adds the security headers, and runs the slow requests off its own thread.
//
// Run test/run.sh (which builds test_api.cpp) every time this file or api.cpp is touched.

#pragma once

#include <map>
#include <string>

#include "dispatcher/app.hpp"
#include "dispatcher/platform.hpp"

namespace dispatcher {

constexpr const char* kSessionCookieName = "bd_session";
constexpr const char* kCsrfHeaderName = "X-Button-Dispatcher";

struct Request {
  std::string method;  // "GET", "POST", "PUT" or "DELETE"
  std::string path;    // the URL path without the query, for example "/api/state"
  std::map<std::string, std::string> query;  // decoded query parameters
  std::string body;    // JSON text, possibly empty
  std::string cookie;  // the raw value of the Cookie header, possibly empty
  std::string csrf;    // the value of the X-Button-Dispatcher header, possibly empty
  std::string client;  // the client's address, used to throttle failed logins
};

struct Response {
  int status = 200;
  std::string content_type = "application/json";
  std::string body;
  HeaderList headers;  // extra headers: Set-Cookie, Content-Disposition
};

class Api {
 public:
  explicit Api(AppCore& app) : app_(app) {}

  // Handles one request. Every /api route except the sign-in ones needs a valid session cookie, and every
  // request that is not a read must carry the X-Button-Dispatcher header (a browser does not add it to a
  // cross-site request). Paths outside /api give 404; the HTTP layer serves the page files itself.
  Response handle(const Request& request);

  // True for requests that call out to the network and may take seconds ("Run now" and the SwitchBot device,
  // scene and command lists), so the HTTP layer should not run them on its only request thread.
  bool is_slow(const Request& request) const;

 private:
  AppCore& app_;
};

}  // namespace dispatcher
