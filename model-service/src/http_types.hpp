// Transport-independent request and response types.
//
// The router (service.hpp) works on these plain structs rather than on cpp-httplib's types, so the
// same handler code runs behind the Unix-socket server, inside unit tests and inside the libFuzzer
// harnesses without any network I/O.
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace recs::service {

struct Request {
  std::string method;  // "GET", "POST", ...
  std::string path;    // URL-decoded path without the query string, e.g. "/v1/similar/260"
  std::vector<std::pair<std::string, std::string>> query;  // decoded query parameters, in order
  std::string request_id;  // the caller's X-Request-ID header (unvalidated; may be empty)
  std::string body;
};

struct Response {
  int status = 200;
  std::string content_type = "application/json";
  std::string body;
  std::string model_version;  // X-Model-Version: the artifact version, or "none"
  std::string request_id;     // X-Request-ID: the caller's (if valid) or a generated one
  std::string allow;          // Allow header, set only on 405
};

}  // namespace recs::service
