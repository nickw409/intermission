// The HTTP/1.1 layer: cpp-httplib on a Unix domain socket (or loopback TCP in development),
// adapting httplib requests to Service::handle().
#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "service.hpp"

namespace recs::service {

class ServiceServer;  // httplib::Server subclass, defined in server.cpp

struct ListenOptions {
  std::string unix_socket;  // e.g. /run/recs/model.sock; used when non-empty
  std::string tcp_host;     // dev fallback; must be 127.0.0.1 or ::1
  int tcp_port = 0;         // 0 = pick a free port (tests)
  int threads = 16;         // worker threads (each keep-alive connection holds one while open)
  unsigned socket_mode = 0660;  // permissions of the Unix socket file
};

class HttpServer {
 public:
  explicit HttpServer(Service& service);
  ~HttpServer();
  HttpServer(const HttpServer&) = delete;
  HttpServer& operator=(const HttpServer&) = delete;

  /// Binds the listening socket. Returns false and sets *error on failure. A stale Unix socket
  /// file is replaced, but any other kind of file at that path is left alone (an error).
  bool bind(const ListenOptions& opts, std::string* error);
  /// The bound TCP port (TCP mode only).
  int port() const { return port_; }
  /// Serves until stop() is called. Returns false if the server stopped with an error.
  bool run();
  void stop();
  /// Blocks until run() is accepting connections.
  void wait_until_ready() const;

  /// Runs raw bytes received on one connection through the full HTTP stack (httplib parsing,
  /// framing checks, routing, Service) in memory and returns the bytes written back. No socket or
  /// thread is involved; used by the raw-HTTP fuzzer and tests.
  std::string process_raw(std::string_view bytes);

 private:
  Service& service_;
  std::unique_ptr<ServiceServer> svr_;
  std::string socket_path_;
  int port_ = 0;
};

}  // namespace recs::service
