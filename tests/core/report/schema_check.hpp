// A small JSON Schema (2020-12) checker for the keywords the frozen contracts use, so C++ tests can
// validate output against contracts/*.schema.json directly. CI additionally validates with ajv.
// Any keyword it does not implement is reported as an error, so it never silently passes.
#pragma once

#include <cmath>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "fixtures.hpp"

namespace recs::testing {

class SchemaChecker {
 public:
  explicit SchemaChecker(nlohmann::json root) : root_(std::move(root)) {}

  static SchemaChecker from_contract(const std::string& file) {
    std::ifstream in(contracts_dir() / file);
    return SchemaChecker(nlohmann::json::parse(in));
  }

  /// Returns every violation as "<json pointer>: <message>"; empty means valid.
  std::vector<std::string> errors(const nlohmann::json& doc) const {
    std::vector<std::string> out;
    check(root_, doc, "", out);
    return out;
  }

 private:
  static bool is_type(const nlohmann::json& v, const std::string& t) {
    if (t == "object") return v.is_object();
    if (t == "array") return v.is_array();
    if (t == "string") return v.is_string();
    if (t == "boolean") return v.is_boolean();
    if (t == "null") return v.is_null();
    if (t == "number") return v.is_number();
    if (t == "integer") {
      if (v.is_number_integer()) return true;
      if (v.is_number_float()) {
        const double d = v.get<double>();
        return std::isfinite(d) && std::floor(d) == d;
      }
      return false;
    }
    return false;
  }

  const nlohmann::json& resolve(const std::string& ref) const {
    if (ref.rfind("#/", 0) != 0) throw std::runtime_error("unsupported $ref " + ref);
    return root_.at(nlohmann::json::json_pointer(ref.substr(1)));
  }

  void check(const nlohmann::json& s, const nlohmann::json& v, const std::string& at,
             std::vector<std::string>& out) const {
    static const std::set<std::string> ignored = {"$schema", "$id", "title", "description", "$defs"};
    for (const auto& [kw, arg] : s.items()) {
      if (ignored.contains(kw)) continue;
      if (kw == "$ref") {
        check(resolve(arg.get<std::string>()), v, at, out);
      } else if (kw == "type") {
        bool ok = false;
        if (arg.is_string()) ok = is_type(v, arg.get<std::string>());
        else for (const auto& t : arg) ok = ok || is_type(v, t.get<std::string>());
        if (!ok) out.push_back(at + ": expected type " + arg.dump() + ", got " + v.dump());
      } else if (kw == "const") {
        if (v != arg) out.push_back(at + ": expected const " + arg.dump());
      } else if (kw == "enum") {
        bool ok = false;
        for (const auto& e : arg) ok = ok || e == v;
        if (!ok) out.push_back(at + ": not in enum " + arg.dump());
      } else if (kw == "required") {
        if (v.is_object()) {
          for (const auto& k : arg) {
            if (!v.contains(k.get<std::string>())) out.push_back(at + ": missing required key " + k.dump());
          }
        }
      } else if (kw == "properties") {
        if (v.is_object()) {
          for (const auto& [k, sub] : arg.items()) {
            if (v.contains(k)) check(sub, v.at(k), at + "/" + k, out);
          }
        }
      } else if (kw == "additionalProperties") {
        if (v.is_object()) {
          const auto props = s.value("properties", nlohmann::json::object());
          for (const auto& [k, val] : v.items()) {
            if (props.contains(k)) continue;
            if (arg.is_boolean()) {
              if (!arg.get<bool>()) out.push_back(at + ": unexpected key " + k);
            } else {
              check(arg, val, at + "/" + k, out);
            }
          }
        }
      } else if (kw == "items") {
        if (v.is_array()) {
          const std::size_t start = s.contains("prefixItems") ? s["prefixItems"].size() : 0;
          for (std::size_t i = start; i < v.size(); ++i) check(arg, v[i], at + "/" + std::to_string(i), out);
        }
      } else if (kw == "prefixItems") {
        if (v.is_array()) {
          for (std::size_t i = 0; i < arg.size() && i < v.size(); ++i) check(arg[i], v[i], at + "/" + std::to_string(i), out);
        }
      } else if (kw == "minItems") {
        if (v.is_array() && v.size() < arg.get<std::size_t>()) out.push_back(at + ": too few items");
      } else if (kw == "maxItems") {
        if (v.is_array() && v.size() > arg.get<std::size_t>()) out.push_back(at + ": too many items");
      } else if (kw == "minimum") {
        if (v.is_number() && v.get<double>() < arg.get<double>()) out.push_back(at + ": below minimum");
      } else if (kw == "maximum") {
        if (v.is_number() && v.get<double>() > arg.get<double>()) out.push_back(at + ": above maximum");
      } else if (kw == "pattern") {
        if (v.is_string() && !std::regex_search(v.get<std::string>(), std::regex(arg.get<std::string>()))) {
          out.push_back(at + ": does not match " + arg.get<std::string>());
        }
      } else if (kw == "format") {
        if (arg == "date-time" && v.is_string() &&
            !std::regex_match(v.get<std::string>(), std::regex(R"(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(\.\d+)?(Z|[+-]\d{2}:\d{2}))"))) {
          out.push_back(at + ": not a date-time");
        }
      } else if (kw == "oneOf") {
        int matches = 0;
        for (const auto& sub : arg) {
          std::vector<std::string> tmp;
          check(sub, v, at, tmp);
          if (tmp.empty()) ++matches;
        }
        if (matches != 1) out.push_back(at + ": matches " + std::to_string(matches) + " oneOf branches");
      } else {
        out.push_back(at + ": schema keyword '" + kw + "' is not supported by the checker");
      }
    }
  }

  nlohmann::json root_;
};

}  // namespace recs::testing
