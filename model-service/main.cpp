// recs-model: serves trained models and the movie catalog to recs-api over an internal HTTP API
// on a Unix socket (contracts/model.yaml). Stateless and read-only: it never opens app.db and never
// sees user identities.
//
// Startup: bind the socket first, so recs-api gets fast 503 model_unavailable answers instead of
// connection errors, then load data/artifacts/CURRENT in the background, retrying every few
// seconds until a verified artifact is serving. Watching CURRENT for later changes and hot-swapping
// (Phase 5) is workstream P's model-service/swap/.
#include <signal.h>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#include <CLI/CLI.hpp>

#include "bundle.hpp"
#include "server.hpp"
#include "service.hpp"
#include "version.hpp"

namespace {

struct Shutdown {
  std::mutex mu;
  std::condition_variable cv;
  bool requested = false;

  void request() {
    {
      std::lock_guard lock(mu);
      requested = true;
    }
    cv.notify_all();
  }
  // Waits up to `d`; returns true if shutdown was requested.
  bool wait_for(std::chrono::milliseconds d) {
    std::unique_lock lock(mu);
    return cv.wait_for(lock, d, [this] { return requested; });
  }
};

}  // namespace

int main(int argc, char** argv) {
  CLI::App app{"recs-model: internal model service"};
  std::string artifacts = "data/artifacts";
  std::string socket_path = "/run/recs/model.sock";
  std::string listen;
  int threads = 8;
  int retry_seconds = 5;
  app.add_option("--artifacts", artifacts, "Artifact root holding CURRENT")->capture_default_str();
  app.add_option("--socket", socket_path, "Unix socket path")->capture_default_str();
  app.add_option("--listen", listen, "Dev fallback instead of the socket: loopback HOST:PORT, e.g. 127.0.0.1:8090");
  app.add_option("--threads", threads, "Worker threads")->check(CLI::Range(1, 64))->capture_default_str();
  app.add_option("--retry-seconds", retry_seconds, "Seconds between initial artifact load attempts")
      ->check(CLI::Range(1, 3600))
      ->capture_default_str();
  CLI11_PARSE(app, argc, argv);

  // Block SIGINT/SIGTERM in every thread; a dedicated thread waits for them.
  sigset_t signals;
  sigemptyset(&signals);
  sigaddset(&signals, SIGINT);
  sigaddset(&signals, SIGTERM);
  pthread_sigmask(SIG_BLOCK, &signals, nullptr);
  signal(SIGPIPE, SIG_IGN);

  recs::service::BundleHolder holder;
  recs::service::Metrics metrics;
  recs::service::Service service(holder, metrics, recs::service::stderr_log_sink());

  recs::service::ListenOptions opts;
  opts.threads = threads;
  if (listen.empty()) {
    opts.unix_socket = socket_path;
  } else {
    const auto colon = listen.rfind(':');
    if (colon == std::string::npos) {
      std::cerr << recs::service::service_name() << ": --listen must be HOST:PORT\n";
      return 2;
    }
    opts.tcp_host = listen.substr(0, colon);
    try {
      opts.tcp_port = std::stoi(listen.substr(colon + 1));
    } catch (const std::exception&) {
      opts.tcp_port = -1;
    }
    if (opts.tcp_port < 1 || opts.tcp_port > 65535) {
      std::cerr << recs::service::service_name() << ": invalid port in --listen\n";
      return 2;
    }
  }

  recs::service::HttpServer server(service);
  std::string error;
  if (!server.bind(opts, &error)) {
    service.log_event("error", "bind failed: " + error);
    return 1;
  }
  service.log_event("info", listen.empty() ? "listening on unix socket " + socket_path : "listening on " + listen);

  Shutdown shutdown;
  std::thread signal_thread([&] {
    int sig = 0;
    sigwait(&signals, &sig);
    service.log_event("info", "shutdown requested");
    shutdown.request();
    server.stop();
  });

  std::thread loader([&] {
    for (;;) {
      const auto t0 = std::chrono::steady_clock::now();
      try {
        auto bundle = recs::service::load_current_bundle(artifacts);
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        metrics.set_artifact_load_seconds(secs);
        service.log_event("info", "serving artifact " + bundle->version);
        holder.store(std::move(bundle));
        return;
      } catch (const std::exception& e) {
        service.log_event("warn", std::string("no verified artifact yet: ") + e.what());
      }
      if (shutdown.wait_for(std::chrono::seconds(retry_seconds))) return;
    }
  });

  const bool ok = server.run();
  shutdown.request();
  loader.join();
  if (signal_thread.joinable()) {
    // Wake the signal thread if the server stopped for another reason.
    pthread_kill(signal_thread.native_handle(), SIGTERM);
    signal_thread.join();
  }
  service.log_event("info", "stopped");
  return ok ? 0 : 1;
}
