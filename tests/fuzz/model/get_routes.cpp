// libFuzzer: arbitrary method, path, query parameters and X-Request-ID.
// Input layout: fields separated by '\n': method, path, request id, then key=value query pairs.
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "fuzz_common.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  std::string_view in(reinterpret_cast<const char*>(data), size);
  std::vector<std::string_view> fields;
  while (!in.empty() && fields.size() < 16) {
    const auto nl = in.find('\n');
    fields.push_back(in.substr(0, nl));
    if (nl == std::string_view::npos) break;
    in.remove_prefix(nl + 1);
  }
  recs::service::Request req;
  req.method = fields.size() > 0 ? std::string(fields[0]) : "GET";
  req.path = fields.size() > 1 ? std::string(fields[1]) : "/";
  req.request_id = fields.size() > 2 ? std::string(fields[2]) : "";
  for (std::size_t i = 3; i < fields.size(); ++i) {
    const auto eq = fields[i].find('=');
    req.query.emplace_back(std::string(fields[i].substr(0, eq)),
                           eq == std::string_view::npos ? std::string() : std::string(fields[i].substr(eq + 1)));
  }
  recs::fuzz::check_response(recs::fuzz::harness().svc.handle(req));
  return 0;
}
