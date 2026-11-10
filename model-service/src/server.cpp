// cpp-httplib adapter for Service (see server.hpp).
//
// Hardening choices:
//  - Routes are registered as literal paths (plus one path-parameter route for /v1/similar/:id),
//    so httplib never runs a backtracking regex over a caller-controlled path.
//  - Every method is registered on every route, so the Service decides 405 vs. 200.
//  - Framing is checked before any body byte is read (pre-routing handler): Transfer-Encoding is
//    refused outright (recs-api always sends Content-Length), a POST/PUT/PATCH must carry exactly
//    one well-formed Content-Length, and a Content-Length over 1 MiB is refused. Those responses
//    close the connection, so an unread body is never parsed as a next request. httplib's own
//    payload limit stays as a second line of defence.
//  - httplib bounds the request line (8 KiB), each header line (8 KiB) and the header count (100);
//    every error httplib produces itself is rewritten into the contract's Error shape with
//    X-Model-Version by the error handler.
//  - Slow clients: a 1 s read timeout (per read), a 2 s keep-alive timeout, a 5 s write timeout,
//    and a worker pool of `threads` that can grow to 4x under load with a queue of at most 4x;
//    a connection that finds the queue full is closed at once instead of waiting. Residual risk:
//    httplib has no whole-request deadline, so a client trickling one byte per second can hold one
//    worker for as long as its headers stay under the size limits. The socket is reachable only by
//    the recs group (recs-api), which bounds who can do that.
//  - The Unix socket is created under a umask that yields exactly the configured mode (0660), so
//    it is never wider even briefly, and nothing is chmod'ed afterwards (chmod follows symlinks).
//    Its parent directory must not be world-writable. An existing socket file is removed only if
//    a connect() to it is refused, i.e. no live server owns it.
#include "server.hpp"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <filesystem>

#include <httplib.h>

#ifndef CPPHTTPLIB_VERSION_NUM
#error "cpp-httplib does not define CPPHTTPLIB_VERSION_NUM; cannot check the 0.63.0 minimum (audit SA4-6)"
#endif

namespace recs::service {
namespace {

// Compile-time floor for cpp-httplib (audit SA4-6). Releases before 0.63.0 have known
// request-parsing vulnerabilities; this stops a downgrade of the pin in cmake/deps.cmake from
// building. The header defines CPPHTTPLIB_VERSION_NUM as a string literal such as "0x003f00"
// (0xMMmmpp), so it is parsed here; the integer overload covers a header that switches to a number.
// Anything unparsable (no 0x prefix, a non-hex digit, no digits, more than 16 digits) gives 0,
// which fails the assertion.
[[maybe_unused]] consteval unsigned long long httplib_version_num(unsigned long long v) { return v; }
consteval unsigned long long httplib_version_num(const char* s) {
  if (s[0] != '0' || (s[1] != 'x' && s[1] != 'X') || s[2] == '\0') return 0;
  unsigned long long v = 0;
  int digits = 0;
  for (const char* p = s + 2; *p != '\0'; ++p) {
    if (++digits > 16) return 0;  // would overflow 64 bits
    const char c = *p;
    int d = -1;
    if (c >= '0' && c <= '9') d = c - '0';
    if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
    if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
    if (d < 0) return 0;
    v = v * 16 + static_cast<unsigned long long>(d);
  }
  return v;
}
static_assert(httplib_version_num(CPPHTTPLIB_VERSION_NUM) >= 0x003f00,
              "cpp-httplib 0.63.0 or newer is required (security fixes; audit SA4-6)");

void write_response(const Response& r, httplib::Response& out) {
  out.status = r.status;
  out.set_content(r.body, r.content_type);
  out.set_header("X-Model-Version", r.model_version);
  out.set_header("X-Request-ID", r.request_id);
  if (!r.allow.empty()) out.set_header("Allow", r.allow);
}

bool is_loopback(const std::string& host) { return host == "127.0.0.1" || host == "::1"; }

constexpr const char* kPaths[] = {"/v1/recommend", "/v1/compare",     "/v1/similar/:movie_id",
                                  "/v1/onboarding", "/v1/movies/search", "/v1/movies/batch",
                                  "/v1/version",   "/v1/models",      "/healthz",
                                  "/metrics"};

// An in-memory connection: reads come from a fixed buffer, writes are collected.
class MemoryStream final : public httplib::Stream {
 public:
  explicit MemoryStream(std::string_view in) : in_(in) {}
  bool is_readable() const override { return true; }
  bool wait_readable() const override { return true; }
  bool wait_writable() const override { return true; }
  ssize_t read(char* ptr, std::size_t size) override {
    const std::size_t n = std::min(size, in_.size() - pos_);
    std::memcpy(ptr, in_.data() + pos_, n);
    pos_ += n;
    return static_cast<ssize_t>(n);
  }
  ssize_t write(const char* ptr, std::size_t size) override {
    out_.append(ptr, size);
    return static_cast<ssize_t>(size);
  }
  void get_remote_ip_and_port(std::string& ip, int& port) const override {
    ip = "";
    port = 0;
  }
  void get_local_ip_and_port(std::string& ip, int& port) const override {
    ip = "";
    port = 0;
  }
  socket_t socket() const override { return INVALID_SOCKET; }
  time_t duration() const override { return 0; }
  bool exhausted() const { return pos_ >= in_.size(); }
  const std::string& output() const { return out_; }

 private:
  std::string_view in_;
  std::size_t pos_ = 0;
  std::string out_;
};

}  // namespace

// httplib::Server with access to its protected per-request entry point, for process_raw().
class ServiceServer final : public httplib::Server {
 public:
  std::string process_memory(std::string_view bytes) {
    MemoryStream s(bytes);
    for (int i = 0; i < 8 && !s.exhausted(); ++i) {  // a few keep-alive requests per connection
      bool closed = false;
      if (!process_request(s, "", 0, "", 0, false, closed, nullptr) || closed) break;
    }
    return s.output();
  }
};

HttpServer::HttpServer(Service& service) : service_(service), svr_(std::make_unique<ServiceServer>()) {
  const httplib::Server::Handler handler = [this](const httplib::Request& req, httplib::Response& res) {
    Request r;
    r.method = req.method;
    r.path = req.path;
    for (const auto& [k, v] : req.params) r.query.emplace_back(k, v);
    r.request_id = req.get_header_value("X-Request-ID");
    r.body = req.body;
    write_response(service_.handle(r), res);
  };
  for (const char* p : kPaths) {
    svr_->Get(p, handler);
    svr_->Post(p, handler);
    svr_->Put(p, handler);
    svr_->Patch(p, handler);
    svr_->Delete(p, handler);
    svr_->Options(p, handler);
  }
  // Framing checks before any body byte is read.
  svr_->set_pre_routing_handler([this](const httplib::Request& req, httplib::Response& res) {
    const auto refuse = [&](std::string_view code, std::string_view message) {
      write_response(service_.error_response(400, code, message, req.get_header_value("X-Request-ID")), res);
      res.set_header("Connection", "close");
      return httplib::Server::HandlerResponse::Handled;
    };
    if (req.has_header("Transfer-Encoding")) {
      return refuse("bad_request", "Transfer-Encoding is not accepted; send Content-Length");
    }
    const std::size_t n_lengths = req.get_header_value_count("Content-Length");
    if (n_lengths > 1) return refuse("bad_request", "more than one Content-Length header");
    if (n_lengths == 1) {
      const std::string v = req.get_header_value("Content-Length");
      std::uint64_t len = 0;
      const auto r = std::from_chars(v.data(), v.data() + v.size(), len);
      if (v.empty() || v.size() > 19 || r.ec != std::errc() || r.ptr != v.data() + v.size()) {
        return refuse("bad_request", "invalid Content-Length");
      }
      if (len > Limits::kMaxBodyBytes) return refuse("body_too_large", "request body exceeds 1 MiB");
    } else if (req.method == "POST" || req.method == "PUT" || req.method == "PATCH") {
      return refuse("bad_request", "Content-Length is required");
    }
    return httplib::Server::HandlerResponse::Unhandled;
  });
  // Errors httplib produces itself (404 for unknown paths, 413, 400 for a malformed request, ...).
  // Responses that already came from the Service carry X-Model-Version and pass through untouched.
  svr_->set_error_handler([this](const httplib::Request& req, httplib::Response& res) {
    if (res.has_header("X-Model-Version")) return httplib::Server::HandlerResponse::Unhandled;
    write_response(service_.transport_error(res.status, req.get_header_value("X-Request-ID")), res);
    return httplib::Server::HandlerResponse::Handled;
  });
  svr_->set_exception_handler([this](const httplib::Request& req, httplib::Response& res, std::exception_ptr) {
    write_response(service_.transport_error(500, req.get_header_value("X-Request-ID")), res);
  });
  svr_->set_payload_max_length(Limits::kMaxBodyBytes);
  svr_->set_read_timeout(1, 0);
  svr_->set_write_timeout(5, 0);
  svr_->set_keep_alive_timeout(2);
  svr_->set_keep_alive_max_count(1000);
}

HttpServer::~HttpServer() {
  stop();
  if (!socket_path_.empty()) ::unlink(socket_path_.c_str());
}

std::string HttpServer::process_raw(std::string_view bytes) { return svr_->process_memory(bytes); }

bool HttpServer::bind(const ListenOptions& opts, std::string* error) {
  const auto fail = [&](std::string msg) {
    if (error) *error = std::move(msg);
    return false;
  };
  const auto threads = static_cast<std::size_t>(opts.threads > 0 ? opts.threads : 1);
  svr_->new_task_queue = [threads] { return new httplib::ThreadPool(threads, 4 * threads, 4 * threads); };

  if (!opts.unix_socket.empty()) {
    const std::string& path = opts.unix_socket;
    if (path.size() >= sizeof(sockaddr_un::sun_path)) {
      return fail("Unix socket path is longer than " + std::to_string(sizeof(sockaddr_un::sun_path) - 1) + " bytes: " + path);
    }
    std::filesystem::path parent = std::filesystem::path(path).parent_path();
    if (parent.empty()) parent = ".";
    struct stat dir {};
    if (::stat(parent.c_str(), &dir) != 0 || !S_ISDIR(dir.st_mode)) return fail("socket directory " + parent.string() + " does not exist");
    if ((dir.st_mode & S_IWOTH) != 0) return fail("socket directory " + parent.string() + " is world-writable");

    struct stat st {};
    if (::lstat(path.c_str(), &st) == 0) {
      if (!S_ISSOCK(st.st_mode)) return fail(path + " exists and is not a socket");
      // Probe: a live server accepts the connection; a stale socket file refuses it.
      const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
      if (fd < 0) return fail(std::string("socket() failed: ") + std::strerror(errno));
      sockaddr_un addr{};
      addr.sun_family = AF_UNIX;
      std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
      const int rc = ::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
      const int err = errno;
      ::close(fd);
      if (rc == 0) return fail("another server is listening on " + path);
      if (err != ECONNREFUSED) return fail("cannot probe existing socket " + path + ": " + std::strerror(err));
      ::unlink(path.c_str());
    }
    svr_->set_address_family(AF_UNIX);
    const mode_t old = ::umask(static_cast<mode_t>(~opts.socket_mode & 0777));
    const bool ok = svr_->bind_to_port(path, 80);
    const int err = errno;
    ::umask(old);
    if (!ok) return fail("cannot bind Unix socket " + path + ": " + std::strerror(err));
    socket_path_ = path;
    return true;
  }
  if (!is_loopback(opts.tcp_host)) {
    return fail("TCP fallback must bind a numeric loopback address (127.0.0.1 or ::1), not '" + opts.tcp_host + "'");
  }
  if (opts.tcp_port == 0) {
    port_ = svr_->bind_to_any_port(opts.tcp_host);
    if (port_ <= 0) return fail("cannot bind " + opts.tcp_host);
    return true;
  }
  if (!svr_->bind_to_port(opts.tcp_host, opts.tcp_port)) {
    return fail("cannot bind " + opts.tcp_host + ":" + std::to_string(opts.tcp_port));
  }
  port_ = opts.tcp_port;
  return true;
}

bool HttpServer::run() { return svr_->listen_after_bind(); }

void HttpServer::stop() {
  if (svr_) svr_->stop();
}

void HttpServer::wait_until_ready() const { svr_->wait_until_ready(); }

}  // namespace recs::service
