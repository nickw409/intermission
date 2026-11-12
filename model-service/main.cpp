// recs-model: serves trained models and the movie catalog to recs-api over an internal HTTP API
// on a Unix socket (contracts/model.yaml). Stateless and read-only: it never opens app.db and never
// sees user identities.
//
// Startup: bind the socket first, so recs-api gets fast 503 model_unavailable answers instead of
// connection errors, then start the CURRENT watcher (watcher.hpp), which loads the verified
// artifact named by data/artifacts/CURRENT and hot-swaps in each new one as it appears.
#include <signal.h>

#include <charconv>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

#include <CLI/CLI.hpp>

#include "bundle.hpp"
#include "check.hpp"
#include "server.hpp"
#include "service.hpp"
#include "version.hpp"
#include "watcher.hpp"

int main(int argc, char** argv) {
  CLI::App app{"recs-model: internal model service"};
  std::string artifacts = "data/artifacts";
  std::string socket_path = "/run/recs/model.sock";
  std::string listen;
  int threads = 16;
  int poll_seconds = 2;
  app.add_option("--artifacts", artifacts, "Artifact root holding CURRENT")->capture_default_str();
  app.add_option("--socket", socket_path, "Unix socket path")->capture_default_str();
  app.add_option("--addr", listen, "Dev fallback instead of the socket: 127.0.0.1:PORT or ::1:PORT, e.g. 127.0.0.1:8090");
  app.add_option("--threads", threads, "Worker threads")->check(CLI::Range(1, 64))->capture_default_str();
  app.add_option("--poll-seconds", poll_seconds, "Seconds between checks of CURRENT")
      ->check(CLI::Range(1, 3600))
      ->capture_default_str();
  bool check = false;
  app.add_flag("--check", check,
               "Load and verify the artifact CURRENT names as the server would, print its version, git_sha "
               "and models, and exit 0 if it would be served; 3 if it would be refused, 4 if there is no CURRENT "
               "(deploy preflight)");
  CLI11_PARSE(app, argc, argv);

  if (check) return recs::service::check_artifacts(artifacts, std::cout, recs::service::stderr_log_sink());

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
      std::cerr << recs::service::service_name() << ": --addr must be HOST:PORT\n";
      return 2;
    }
    opts.tcp_host = listen.substr(0, colon);
    const std::string port = listen.substr(colon + 1);
    int p = 0;
    const auto r = std::from_chars(port.data(), port.data() + port.size(), p);
    if (port.empty() || port.size() > 5 || r.ec != std::errc() || r.ptr != port.data() + port.size() || p < 1 ||
        p > 65535) {
      std::cerr << recs::service::service_name() << ": invalid port in --addr\n";
      return 2;
    }
    opts.tcp_port = p;
  }

  recs::service::HttpServer server(service);
  std::string error;
  if (!server.bind(opts, &error)) {
    service.log_event("error", "bind failed: " + error);
    return 1;
  }
  service.log_event("info", listen.empty() ? "listening on unix socket " + socket_path : "listening on " + listen);

  std::thread signal_thread([&] {
    int sig = 0;
    sigwait(&signals, &sig);
    service.log_event("info", "shutdown requested");
    server.stop();
  });

  recs::service::ArtifactWatcher watcher(artifacts, holder, metrics, service);
  watcher.start(std::chrono::seconds(poll_seconds));

  const bool ok = server.run();
  watcher.stop();
  // Wake the signal thread if the server stopped for another reason (it has exited otherwise).
  pthread_kill(signal_thread.native_handle(), SIGTERM);
  signal_thread.join();
  service.log_event("info", "stopped");
  return ok ? 0 : 1;
}
