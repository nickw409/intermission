// The HTTP/1.1 layer: cpp-httplib on a Unix domain socket (or loopback TCP in development),
// adapting httplib requests to Service::handle().
#pragma once

#include <memory>
#include <string>

#include "service.hpp"

namespace httplib {
class Server;
}

namespace recs::service {

struct ListenOptions {
  std::string unix_socket;  // e.g. /run/recs/model.sock; used when non-empty
  std::string tcp_host;     // dev fallback; must be a loopback address
  int tcp_port = 0;         // 0 = pick a free port (tests)
  int threads = 8;          // worker threads
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

 private:
  Service& service_;
  std::unique_ptr<httplib::Server> svr_;
  std::string socket_path_;
  int port_ = 0;
};

}  // namespace recs::service
