// cpp-httplib adapter for Service (see server.hpp).
//
// Hardening choices:
//  - Routes are registered as literal paths (plus one path-parameter route for /v1/similar/:id),
//    so httplib never runs a backtracking regex over a caller-controlled path.
//  - Every method is registered on every route, so the Service decides 405 vs. 200.
//  - Bodies over 1 MiB are refused by httplib before they are read (413), and the error handler
//    turns that, and every other httplib-generated error, into the contract's Error shape with
//    X-Model-Version.
//  - Read/write timeouts bound slow clients; the worker pool bounds concurrency.
//  - The Unix socket is created under a restrictive umask and then chmod'ed to the configured
//    mode (0660: owner + the recs group), so it is never world-accessible, even briefly.
#include "server.hpp"

#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include <httplib.h>

namespace recs::service {
namespace {

void write_response(const Response& r, httplib::Response& out) {
  out.status = r.status;
  out.set_content(r.body, r.content_type);
  out.set_header("X-Model-Version", r.model_version);
  out.set_header("X-Request-ID", r.request_id);
  if (!r.allow.empty()) out.set_header("Allow", r.allow);
}

bool is_loopback(const std::string& host) { return host == "127.0.0.1" || host == "::1" || host == "localhost"; }

constexpr const char* kPaths[] = {"/v1/recommend", "/v1/compare",     "/v1/similar/:movie_id",
                                  "/v1/onboarding", "/v1/movies/search", "/v1/movies/batch",
                                  "/v1/version",   "/v1/models",      "/healthz",
                                  "/metrics"};

}  // namespace

HttpServer::HttpServer(Service& service) : service_(service), svr_(std::make_unique<httplib::Server>()) {
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
  svr_->set_read_timeout(5, 0);
  svr_->set_write_timeout(5, 0);
  svr_->set_keep_alive_timeout(5);
  svr_->set_keep_alive_max_count(1000);
}

HttpServer::~HttpServer() {
  stop();
  if (!socket_path_.empty()) ::unlink(socket_path_.c_str());
}

bool HttpServer::bind(const ListenOptions& opts, std::string* error) {
  const auto fail = [&](std::string msg) {
    if (error) *error = std::move(msg);
    return false;
  };
  const int threads = opts.threads > 0 ? opts.threads : 1;
  svr_->new_task_queue = [threads] { return new httplib::ThreadPool(static_cast<std::size_t>(threads)); };

  if (!opts.unix_socket.empty()) {
    const std::string& path = opts.unix_socket;
    struct stat st {};
    if (::lstat(path.c_str(), &st) == 0) {
      if (!S_ISSOCK(st.st_mode)) return fail(path + " exists and is not a socket");
      ::unlink(path.c_str());  // stale socket from a previous run
    }
    svr_->set_address_family(AF_UNIX);
    const mode_t old = ::umask(0177);
    const bool ok = svr_->bind_to_port(path, 80);
    ::umask(old);
    if (!ok) return fail("cannot bind Unix socket " + path + ": " + std::strerror(errno));
    socket_path_ = path;
    if (::chmod(path.c_str(), static_cast<mode_t>(opts.socket_mode)) != 0) {
      return fail("cannot chmod " + path + ": " + std::strerror(errno));
    }
    return true;
  }
  if (!is_loopback(opts.tcp_host)) return fail("TCP fallback must bind a loopback address, not '" + opts.tcp_host + "'");
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
