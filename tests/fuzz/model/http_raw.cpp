// libFuzzer: raw bytes of one client connection through the whole HTTP stack in memory
// (httplib's request-line, header, Content-Length and chunked parsing, the framing checks,
// routing and the Service), via HttpServer::process_raw. No socket or thread is involved.
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "fuzz_common.hpp"
#include "server.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  static recs::service::HttpServer server(recs::fuzz::harness().svc);
  const std::string out = server.process_raw(std::string_view(reinterpret_cast<const char*>(data), size));
  // Every response is HTTP/1.1, and none is a 5xx: hostile input must never reach `internal`.
  std::size_t pos = 0;
  while ((pos = out.find("HTTP/1.1 ", pos)) != std::string::npos) {
    if (pos + 9 < out.size() && out[pos + 9] == '5') __builtin_trap();
    pos += 9;
  }
  if (!out.empty() && out.rfind("HTTP/1.1 ", 0) != 0) __builtin_trap();
  return 0;
}
