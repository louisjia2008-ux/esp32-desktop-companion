#pragma once

#include <Arduino.h>

#include <functional>
#include <map>
#include <string>
#include <utility>

enum HTTPMethod : uint8_t {
  HTTP_GET,
  HTTP_POST,
};

class WebServer;

namespace fake_web_server {

struct Response {
  int status = 0;
  std::string contentType;
  std::string body;
  std::map<std::string, std::string> headers;
};

inline WebServer* latestServer = nullptr;

inline WebServer* latest() { return latestServer; }

}  // namespace fake_web_server

class WebServer {
 public:
  using Handler = std::function<void()>;
  using Arguments = std::map<std::string, std::string>;

  explicit WebServer(uint16_t port) : port_(port) {
    fake_web_server::latestServer = this;
  }

  ~WebServer() {
    if (fake_web_server::latestServer == this) {
      fake_web_server::latestServer = nullptr;
    }
  }

  WebServer(const WebServer&) = delete;
  WebServer& operator=(const WebServer&) = delete;

  void on(const char* path, HTTPMethod method, Handler handler) {
    handlers_[{method, path == nullptr ? "" : path}] = std::move(handler);
  }

  void onNotFound(Handler handler) { notFound_ = std::move(handler); }

  void begin() {
    active_ = true;
    ++beginCalls_;
  }

  void stop() {
    active_ = false;
    ++stopCalls_;
  }

  void handleClient() { ++handleClientCalls_; }

  String arg(const char* name) const {
    const auto found = requestArguments_.find(name == nullptr ? "" : name);
    return found == requestArguments_.end() ? String()
                                             : String(found->second);
  }

  void sendHeader(const char* name, const char* value,
                  bool first = false) {
    (void)first;
    response_.headers[name == nullptr ? "" : name] =
        value == nullptr ? "" : value;
  }

  void send(int status, const char* contentType, const String& body) {
    response_.status = status;
    response_.contentType = contentType == nullptr ? "" : contentType;
    response_.body = body.stdString();
  }

  void send(int status, const char* contentType, const char* body) {
    send(status, contentType, String(body));
  }

  bool dispatch(HTTPMethod method, const std::string& path,
                Arguments arguments = {}) {
    requestArguments_ = std::move(arguments);
    response_ = {};
    const auto found = handlers_.find({method, path});
    if (found != handlers_.end()) {
      found->second();
      return true;
    }
    if (notFound_) {
      notFound_();
      return true;
    }
    return false;
  }

  bool active() const { return active_; }
  uint16_t port() const { return port_; }
  size_t beginCalls() const { return beginCalls_; }
  size_t stopCalls() const { return stopCalls_; }
  size_t handleClientCalls() const { return handleClientCalls_; }
  const fake_web_server::Response& response() const { return response_; }

 private:
  uint16_t port_ = 0;
  std::map<std::pair<HTTPMethod, std::string>, Handler> handlers_;
  Handler notFound_;
  Arguments requestArguments_;
  fake_web_server::Response response_;
  bool active_ = false;
  size_t beginCalls_ = 0;
  size_t stopCalls_ = 0;
  size_t handleClientCalls_ = 0;
};
