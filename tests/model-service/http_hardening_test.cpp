// HTTP-layer hardening, tested with raw bytes on a real Unix socket: request framing (no
// Transfer-Encoding, Content-Length required and bounded), oversized header lines and header
// counts, slow clients, the bounded worker queue, and the socket ownership probe. Each hostile
// request must be refused quickly, without memory growth, and leave the server answering /healthz.
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include "server.hpp"
#include "service.hpp"
#include "test_bundle.hpp"

namespace fs = std::filesystem;
using nlohmann::json;
using Clock = std::chrono::steady_clock;

namespace {

struct TempDir {
  fs::path path;
  TempDir() {
    std::random_device rd;
    path = fs::temp_directory_path() / ("rh-" + std::to_string(::getpid()) + "-" + std::to_string(rd() % 100000));
    fs::create_directories(path);
    fs::permissions(path, fs::perms::owner_all);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

// Resident set size of this process, in MiB.
double rss_mib() {
  std::ifstream in("/proc/self/status");
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind("VmRSS:", 0) == 0) return std::stod(line.substr(6)) / 1024.0;
  }
  return 0.0;
}

// A raw client connection with 5 s send/receive timeouts.
class RawConn {
 public:
  explicit RawConn(const fs::path& sock) {
    fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    REQUIRE(fd_ >= 0);
    timeval tv{5, 0};
    ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    ::setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, sock.c_str(), sizeof(addr.sun_path) - 1);
    connected_ = ::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0;
  }
  ~RawConn() { ::close(fd_); }
  bool connected() const { return connected_; }
  // Sends everything; false as soon as the peer stops accepting (closed, reset or timed out).
  bool send_all(std::string_view s) {
    while (!s.empty()) {
      const ssize_t n = ::send(fd_, s.data(), s.size(), MSG_NOSIGNAL);
      if (n <= 0) return false;
      s.remove_prefix(static_cast<std::size_t>(n));
    }
    return true;
  }
  // Reads until EOF, error or timeout (at most 1 MiB).
  std::string read_all() {
    std::string out;
    char buf[8192];
    while (out.size() < (1u << 20)) {
      const ssize_t n = ::recv(fd_, buf, sizeof buf, 0);
      if (n <= 0) break;
      out.append(buf, static_cast<std::size_t>(n));
    }
    return out;
  }

 private:
  int fd_ = -1;
  bool connected_ = false;
};

struct Server {
  recs::service::BundleHolder holder;
  recs::service::Metrics metrics;
  recs::service::Service svc{holder, metrics, [](std::string_view) {}};
  recs::service::HttpServer server{svc};
  std::thread thread;
  fs::path sock;

  Server(const fs::path& path, int threads) : sock(path) {
    holder.store(recs::testing::fixture_bundle());
    std::string err;
    REQUIRE(server.bind({path.string(), "", 0, threads, 0660}, &err));
    thread = std::thread([this] { server.run(); });
    server.wait_until_ready();
  }
  ~Server() {
    server.stop();
    thread.join();
  }
  // GET /healthz over a fresh connection; returns the status (or -1) and the elapsed seconds.
  std::pair<int, double> healthz() const {
    const auto t0 = Clock::now();
    httplib::Client c(sock.string(), 80);
    c.set_address_family(AF_UNIX);
    c.set_connection_timeout(5, 0);
    c.set_read_timeout(5, 0);
    auto r = c.Get("/healthz");
    return {r ? r->status : -1, std::chrono::duration<double>(Clock::now() - t0).count()};
  }
};

std::string body_of(const std::string& response) {
  const auto p = response.find("\r\n\r\n");
  return p == std::string::npos ? "" : response.substr(p + 4);
}

}  // namespace

TEST_CASE("recs-model: framing is refused before any body is read", "[model-service][hardening]") {
  TempDir tmp;
  Server s(tmp.path / "m.sock", 4);

  SECTION("Transfer-Encoding: chunked, 2 MiB body") {
    RawConn c(s.sock);
    REQUIRE(c.connected());
    REQUIRE(c.send_all("POST /v1/recommend HTTP/1.1\r\nHost: x\r\nX-Request-ID: te-1\r\nTransfer-Encoding: chunked\r\n\r\n"));
    const auto t0 = Clock::now();
    const std::string resp = c.read_all();  // answered from the headers alone, then closed
    // Promptly: well before httplib's 1 s read timeout would have ended the wait for a body.
    CHECK(std::chrono::duration<double>(Clock::now() - t0).count() < 0.5);
    CHECK(resp.rfind("HTTP/1.1 400", 0) == 0);
    CHECK(resp.find("X-Model-Version: " + std::string(recs::testing::kFixtureVersion)) != std::string::npos);
    CHECK(resp.find("X-Request-ID: te-1") != std::string::npos);
    CHECK(resp.find("Connection: close") != std::string::npos);
    CHECK(json::parse(body_of(resp)) ==
          json{{"error", {{"code", "bad_request"}, {"message", "Transfer-Encoding is not accepted; send Content-Length"}}}});
    // The 2 MiB of chunks that follow go nowhere: the connection is closed.
    const std::string chunk = "10000\r\n" + std::string(0x10000, 'a') + "\r\n";
    bool all_sent = true;
    for (int i = 0; i < 32 && all_sent; ++i) all_sent = c.send_all(chunk);
    CHECK_FALSE(all_sent);
  }

  SECTION("chunked flood: bounded memory and time") {
    const double before = rss_mib();
    const auto t0 = Clock::now();
    RawConn c(s.sock);
    REQUIRE(c.connected());
    std::string big = "POST /v1/compare HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n";
    const std::string chunk = "100000\r\n" + std::string(0x100000, 'a') + "\r\n";  // 1 MiB chunks
    c.send_all(big);
    for (int i = 0; i < 64; ++i) {
      if (!c.send_all(chunk)) break;  // stops as soon as the server has closed
    }
    const double secs = std::chrono::duration<double>(Clock::now() - t0).count();
    CHECK(secs < 3.0);
    CHECK(rss_mib() - before < 32.0);
  }

  SECTION("POST without Content-Length") {
    RawConn c(s.sock);
    REQUIRE(c.send_all("POST /v1/recommend HTTP/1.1\r\nHost: x\r\n\r\n{\"model\":\"pop\",\"ratings\":[],\"k\":3}"));
    const std::string resp = c.read_all();
    CHECK(resp.rfind("HTTP/1.1 400", 0) == 0);
    CHECK(json::parse(body_of(resp)) ==
          json{{"error", {{"code", "bad_request"}, {"message", "Content-Length is required"}}}});
  }

  SECTION("Content-Length over 1 MiB, invalid or repeated") {
    {
      RawConn c(s.sock);
      REQUIRE(c.send_all("POST /v1/movies/batch HTTP/1.1\r\nHost: x\r\nContent-Length: 1048577\r\n\r\n"));
      const std::string resp = c.read_all();
      CHECK(resp.rfind("HTTP/1.1 400", 0) == 0);
      CHECK(json::parse(body_of(resp))["error"]["code"] == "body_too_large");
    }
    for (const char* bad : {"Content-Length: -1\r\n", "Content-Length: 12abc\r\n", "Content-Length: 5\r\nContent-Length: 5\r\n",
                            "Content-Length: 99999999999999999999999\r\n"}) {
      INFO(bad);
      RawConn c(s.sock);
      REQUIRE(c.send_all(std::string("POST /v1/movies/batch HTTP/1.1\r\nHost: x\r\n") + bad + "\r\n{\"ids\":[1]}"));
      const std::string resp = c.read_all();
      CHECK(resp.rfind("HTTP/1.1 400", 0) == 0);
      const auto j = json::parse(body_of(resp), nullptr, false);
      CHECK((j.is_object() && j.contains("error")));
    }
    // A correct request on the same server still works.
    RawConn ok(s.sock);
    const std::string body = R"({"ids":[260]})";
    REQUIRE(ok.send_all("POST /v1/movies/batch HTTP/1.1\r\nHost: x\r\nConnection: close\r\nContent-Length: " +
                        std::to_string(body.size()) + "\r\n\r\n" + body));
    CHECK(ok.read_all().rfind("HTTP/1.1 200", 0) == 0);
  }

  SECTION("a 1 MiB header line and 10,000 header lines") {
    for (int variant = 0; variant < 2; ++variant) {
      INFO("variant " << variant);
      const double before = rss_mib();
      const auto t0 = Clock::now();
      {
        RawConn c(s.sock);
        REQUIRE(c.connected());
        std::string req = "GET /v1/version HTTP/1.1\r\nHost: x\r\n";
        if (variant == 0) {
          req += "X-Big: " + std::string(1u << 20, 'a') + "\r\n";
        } else {
          for (int i = 0; i < 10000; ++i) req += "X-H" + std::to_string(i) + ": v\r\n";
        }
        req += "\r\n";
        c.send_all(req);  // may fail part-way once the server gives up on the request
        const std::string resp = c.read_all();
        // Either a 4xx in the contract's shape or a closed connection; never a 200 or a hang.
        if (!resp.empty()) {
          CHECK(resp.rfind("HTTP/1.1 4", 0) == 0);
          CHECK(resp.find("X-Model-Version") != std::string::npos);
        }
      }
      CHECK(std::chrono::duration<double>(Clock::now() - t0).count() < 3.0);
      CHECK(rss_mib() - before < 32.0);
      const auto [status, secs] = s.healthz();
      CHECK(status == 200);
      CHECK(secs < 1.0);
    }
  }

  const auto [status, secs] = s.healthz();
  CHECK(status == 200);
  CHECK(secs < 1.0);
}

TEST_CASE("recs-model: slow clients do not starve /healthz", "[model-service][hardening]") {
  TempDir tmp;
  Server s(tmp.path / "m.sock", 2);
  std::atomic<bool> stop{false};
  // Each trickler sends a request line and then one short header line every 200 ms, never
  // finishing its headers; each byte arrives well within the 1 s read timeout.
  const auto trickle = [&] {
    RawConn c(s.sock);
    if (!c.send_all("GET /healthz HTTP/1.1\r\n")) return;
    while (!stop.load()) {
      if (!c.send_all("X: y\r\n")) return;
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
  };

  SECTION("two tricklers with --threads 2") {
    std::vector<std::thread> t;
    for (int i = 0; i < 2; ++i) t.emplace_back(trickle);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const auto [status, secs] = s.healthz();
    stop = true;
    for (auto& th : t) th.join();
    CHECK(status == 200);
    CHECK(secs < 2.0);
  }

  SECTION("more tricklers than workers plus queue: new connections fail fast, not hang") {
    std::vector<std::thread> t;
    for (int i = 0; i < 24; ++i) t.emplace_back(trickle);  // pool max 8 workers + 8 queued
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const auto [status, secs] = s.healthz();
    CHECK(secs < 2.0);  // answered or refused, but promptly
    (void)status;
    stop = true;
    for (auto& th : t) th.join();
    // Once the tricklers are gone the server recovers.
    bool recovered = false;
    for (int i = 0; i < 50 && !recovered; ++i) {
      recovered = s.healthz().first == 200;
      if (!recovered) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    CHECK(recovered);
  }
}

TEST_CASE("recs-model: process_raw runs the full HTTP stack in memory", "[model-service][hardening]") {
  recs::service::BundleHolder holder;
  holder.store(recs::testing::fixture_bundle());
  recs::service::Metrics metrics;
  recs::service::Service svc(holder, metrics, [](std::string_view) {});
  recs::service::HttpServer server(svc);
  const std::string ok = server.process_raw("GET /v1/version HTTP/1.1\r\nHost: x\r\n\r\n");
  CHECK(ok.rfind("HTTP/1.1 200", 0) == 0);
  CHECK(ok.find(std::string(recs::testing::kFixtureVersion)) != std::string::npos);
  const std::string te = server.process_raw("POST /v1/recommend HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n");
  CHECK(te.rfind("HTTP/1.1 400", 0) == 0);
  CHECK(server.process_raw("garbage\r\n\r\n").rfind("HTTP/1.1 400", 0) == 0);
}

TEST_CASE("recs-model: socket ownership: live sockets and unsafe directories are refused", "[model-service][hardening]") {
  TempDir tmp;
  const fs::path sock = tmp.path / "m.sock";
  Server live(sock, 2);
  recs::service::BundleHolder holder;
  recs::service::Metrics metrics;
  recs::service::Service svc(holder, metrics, [](std::string_view) {});
  {
    recs::service::HttpServer second(svc);
    std::string err;
    CHECK_FALSE(second.bind({sock.string(), "", 0, 2, 0660}, &err));
    CHECK(err.find("another server is listening") != std::string::npos);
  }
  CHECK(live.healthz().first == 200);  // the live server's socket was not unlinked

  const fs::path open_dir = tmp.path / "open";
  fs::create_directories(open_dir);
  fs::permissions(open_dir, fs::perms::all);
  recs::service::HttpServer third(svc);
  std::string err;
  CHECK_FALSE(third.bind({(open_dir / "m.sock").string(), "", 0, 2, 0660}, &err));
  CHECK(err.find("world-writable") != std::string::npos);

  // Numeric loopback only for the TCP fallback.
  recs::service::HttpServer tcp(svc);
  CHECK_FALSE(tcp.bind({"", "localhost", 0, 2, 0660}, &err));
  CHECK_FALSE(tcp.bind({"", "10.0.0.1", 0, 2, 0660}, &err));
}
