// The recs-model router and handlers (contracts/model.yaml).
//
// Every request goes through Service::handle():
//   1. pick or generate the request id, take ONE snapshot of the serving bundle (so the response
//      and its X-Model-Version always come from the same artifact, even during a hot swap);
//   2. match the route and method; /v1/* routes answer 503 model_unavailable without a bundle;
//   3. validate the input again (recs-api validates too, but this service is the last line):
//      body size, JSON nesting depth, exact object shapes (unknown fields are rejected, like the
//      schema's additionalProperties: false), integer ranges, the 0.5 rating grid, list lengths,
//      the genre enum;
//   4. run the handler, converting ApiError to the contract's Error shape and any other exception
//      to a 500 `internal` — nothing escapes;
//   5. record metrics and write one log line. Logs carry the route template, never the raw path,
//      query string or body, so no rating payload (or search text) is ever logged.
//
// Numbers: model scores are float32. They are printed via the shortest decimal that round-trips the
// float (4.23f prints as 4.23, not 4.230000019073486), see tidy().
#include "service.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <iostream>
#include <limits>
#include <mutex>
#include <random>
#include <set>
#include <unordered_set>

#include <nlohmann/json.hpp>

#include "genres.hpp"
#include "onboarding.hpp"
#include "search.hpp"

namespace recs::service {
namespace {

using nlohmann::json;

// ---- errors -------------------------------------------------------------------------------------

struct ApiError {
  int status;
  std::string code;
  std::string message;
};

[[noreturn]] void bad_request(std::string message) { throw ApiError{400, "bad_request", std::move(message)}; }

std::string dump(const json& j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); }

std::string error_body(std::string_view code, std::string_view message) {
  return dump(json{{"error", {{"code", code}, {"message", message}}}});
}

constexpr std::string_view kUnavailable = "no verified artifact is loaded";

// ---- routes -------------------------------------------------------------------------------------

enum class Route { recommend, compare, similar, onboarding, search, batch, version, models, healthz, metrics, other };

struct RouteInfo {
  Route route;
  std::string_view path;      // exact path (similar: prefix)
  std::string_view method;
  std::string_view template_; // used in logs and metric labels
};

constexpr std::array<RouteInfo, 10> kRoutes = {{
    {Route::recommend, "/v1/recommend", "POST", "/v1/recommend"},
    {Route::compare, "/v1/compare", "POST", "/v1/compare"},
    {Route::similar, "/v1/similar/", "GET", "/v1/similar/{movie_id}"},
    {Route::onboarding, "/v1/onboarding", "POST", "/v1/onboarding"},
    {Route::search, "/v1/movies/search", "GET", "/v1/movies/search"},
    {Route::batch, "/v1/movies/batch", "POST", "/v1/movies/batch"},
    {Route::version, "/v1/version", "GET", "/v1/version"},
    {Route::models, "/v1/models", "GET", "/v1/models"},
    {Route::healthz, "/healthz", "GET", "/healthz"},
    {Route::metrics, "/metrics", "GET", "/metrics"},
}};

const RouteInfo* match_route(std::string_view path) {
  for (const auto& r : kRoutes) {
    if (r.route == Route::similar) {
      if (path.starts_with(r.path) && path.find('/', r.path.size()) == std::string_view::npos) return &r;
    } else if (path == r.path) {
      return &r;
    }
  }
  return nullptr;
}

// ---- small helpers ------------------------------------------------------------------------------

// The shortest decimal that round-trips the float, as a double (so JSON shows 4.23, not 4.2300000190734863).
double tidy(float f) {
  char buf[32];
  const auto r = std::to_chars(buf, buf + sizeof buf, f);
  double d = static_cast<double>(f);
  std::from_chars(buf, r.ptr, d);
  return d;
}

bool printable_ascii(std::string_view s) {
  return std::all_of(s.begin(), s.end(), [](char c) { return c >= 0x20 && c < 0x7f; });
}

// Quotes a caller-supplied token for an error message only if it is short and printable.
std::string echo_token(std::string_view s) {
  if (s.size() > 40 || !printable_ascii(s)) return "";
  return " \"" + std::string(s) + "\"";
}

bool valid_request_id(std::string_view id) {
  if (id.empty() || id.size() > 128) return false;
  return std::all_of(id.begin(), id.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
           c == '.' || c == ':';
  });
}

std::string generate_request_id() {
  thread_local std::mt19937_64 rng{[] {
    std::random_device rd;
    return (static_cast<std::uint64_t>(rd()) << 32) ^ rd();
  }()};
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out(32, '0');
  for (int half = 0; half < 2; ++half) {
    std::uint64_t v = rng();
    for (int i = 0; i < 16; ++i) {
      out[static_cast<std::size_t>(half * 16 + i)] = kHex[v & 0xF];
      v >>= 4;
    }
  }
  return out;
}

std::string utc_now_ms() {
  const auto now = std::chrono::system_clock::now();
  const auto t = std::chrono::system_clock::to_time_t(now);
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
  std::tm tm{};
  gmtime_r(&t, &tm);
  char buf[40];
  const std::size_t n = std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S", &tm);
  std::snprintf(buf + n, sizeof buf - n, ".%03dZ", static_cast<int>(ms));
  return buf;
}

std::string_view safe_method(std::string_view m) {
  for (std::string_view k : {"GET", "POST", "PUT", "DELETE", "PATCH", "HEAD", "OPTIONS"}) {
    if (m == k) return k;
  }
  return "OTHER";
}

// ---- JSON input validation ----------------------------------------------------------------------

// Rejects nesting deeper than Limits::kMaxJsonDepth before the parser sees the text, so no input
// can drive the parser or the DOM destructor into deep recursion.
bool nesting_ok(std::string_view s) {
  int depth = 0;
  bool in_string = false;
  bool escaped = false;
  for (char c : s) {
    if (in_string) {
      if (escaped) {
        escaped = false;
      } else if (c == '\\') {
        escaped = true;
      } else if (c == '"') {
        in_string = false;
      }
      continue;
    }
    if (c == '"') {
      in_string = true;
    } else if (c == '[' || c == '{') {
      if (++depth > Limits::kMaxJsonDepth) return false;
    } else if (c == ']' || c == '}') {
      if (--depth < 0) return false;  // more closers than openers: malformed
    }
  }
  return true;
}

json parse_object(std::string_view body) {
  if (body.size() > Limits::kMaxBodyBytes) throw ApiError{400, "body_too_large", "request body exceeds 1 MiB"};
  if (!nesting_ok(body)) throw ApiError{400, "invalid_json", "request body is not valid JSON"};
  json j = json::parse(body.begin(), body.end(), nullptr, /*allow_exceptions=*/false, /*ignore_comments=*/false);
  if (j.is_discarded()) throw ApiError{400, "invalid_json", "request body is not valid JSON"};
  if (!j.is_object()) bad_request("request body must be a JSON object");
  return j;
}

// Unknown fields are rejected (additionalProperties: false); required fields must be present.
void check_fields(const json& obj, std::initializer_list<std::string_view> allowed,
                  std::initializer_list<std::string_view> required) {
  for (const auto& [key, value] : obj.items()) {
    if (std::find(allowed.begin(), allowed.end(), key) == allowed.end()) bad_request("unexpected field" + echo_token(key));
  }
  for (std::string_view r : required) {
    if (!obj.contains(r)) bad_request("missing field \"" + std::string(r) + "\"");
  }
}

// A JSON Schema "integer" (an integral number; 3.0 counts) within [lo, hi].
std::optional<std::int64_t> as_int(const json& v, std::int64_t lo, std::int64_t hi) {
  std::int64_t x = 0;
  if (v.is_number_unsigned()) {
    const auto u = v.get<std::uint64_t>();
    if (u > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) return std::nullopt;
    x = static_cast<std::int64_t>(u);
  } else if (v.is_number_integer()) {
    x = v.get<std::int64_t>();
  } else if (v.is_number_float()) {
    const double d = v.get<double>();
    if (!std::isfinite(d) || std::floor(d) != d || d < static_cast<double>(lo) || d > static_cast<double>(hi)) {
      return std::nullopt;
    }
    x = static_cast<std::int64_t>(d);
  } else {
    return std::nullopt;
  }
  if (x < lo || x > hi) return std::nullopt;
  return x;
}

std::int64_t movie_id(const json& v) {
  const auto id = as_int(v, 1, Limits::kMaxMovieId);
  if (!id) bad_request("movie_id must be an integer between 1 and 2147483647");
  return *id;
}

std::size_t get_k(const json& obj) {
  const auto k = as_int(obj.at("k"), 1, static_cast<std::int64_t>(Limits::kMaxK));
  if (!k) bad_request("k must be an integer between 1 and 100");
  return static_cast<std::size_t>(*k);
}

// The rating grid: 0.5, 1.0, ..., 5.0.
std::optional<float> as_rating(const json& v) {
  if (!v.is_number()) return std::nullopt;
  const double d = v.get<double>();
  if (!std::isfinite(d)) return std::nullopt;
  const double twice = d * 2.0;
  if (std::floor(twice) != twice || twice < 1.0 || twice > 10.0) return std::nullopt;
  return static_cast<float>(d);
}

struct ParsedRatings {
  std::vector<Rating> known;  // dense ids, request order
  std::int64_t ignored = 0;   // well-formed ratings of movies the catalog does not know
};

ParsedRatings parse_ratings(const Bundle& b, const json& v) {
  if (!v.is_array() || v.size() > Limits::kMaxRatings) {
    bad_request("ratings must be an array of at most 5000 {movie_id, rating} objects");
  }
  ParsedRatings out;
  out.known.reserve(v.size());
  std::unordered_set<std::int64_t> seen;
  seen.reserve(v.size() * 2);
  for (const auto& r : v) {
    if (!r.is_object()) bad_request("ratings must be an array of at most 5000 {movie_id, rating} objects");
    check_fields(r, {"movie_id", "rating"}, {"movie_id", "rating"});
    const std::int64_t id = movie_id(r.at("movie_id"));
    const auto value = as_rating(r.at("rating"));
    if (!value) bad_request("rating must be in [0.5, 5] in 0.5 steps");
    if (!seen.insert(id).second) bad_request("duplicate movie_id " + std::to_string(id) + " in ratings");
    if (const auto idx = b.catalog.items.find(id)) {
      out.known.push_back({*idx, *value, 0});
    } else {
      ++out.ignored;
    }
  }
  return out;
}

// Known excluded items; unknown ids are skipped silently.
std::vector<ItemIdx> parse_exclude(const Bundle& b, const json& obj) {
  std::vector<ItemIdx> out;
  if (!obj.contains("exclude")) return out;
  const json& v = obj.at("exclude");
  if (!v.is_array() || v.size() > Limits::kMaxExclude) bad_request("exclude must be an array of at most 10000 movie ids");
  out.reserve(v.size());
  for (const auto& e : v) {
    if (const auto idx = b.catalog.items.find(movie_id(e))) out.push_back(*idx);
  }
  return out;
}

// The optional `genres` filter as a GenreMask; 0 (absent or empty) means no filter. Must be an array
// of at most 19 distinct names from the contract's Genre enum.
GenreMask parse_genres(const json& obj) {
  if (!obj.contains("genres")) return 0;
  const json& v = obj.at("genres");
  if (!v.is_array() || v.size() > kGenres.size()) bad_request("genres must be an array of at most 19 distinct genre names");
  GenreMask mask = 0;
  for (const auto& g : v) {
    if (!g.is_string()) bad_request("genres must be an array of at most 19 distinct genre names");
    const auto& name = g.get_ref<const std::string&>();
    const auto bit = genre_bit(name);
    if (!bit) bad_request("unknown genre" + echo_token(name));
    if (mask & *bit) bad_request("duplicate genre" + echo_token(name));
    mask |= *bit;
  }
  return mask;
}

// What recommend() must mask for this request: the caller's exclude list plus, under a genre
// filter, every item with none of the wanted genres. Masking before recommend()'s top-k cut (not
// filtering its output) means the result is the k best MATCHING items, in the order the unfiltered
// ranking would put them. Shared by recommend and compare, so every model is filtered the same way
// and no model needs to know about genres. O(n_items) with one bitwise AND per item.
// Any future re-ranker (mmr, hybrid) must receive this pre-masked list too, so the genre filter is
// applied before re-ranking: re-ranking first and filtering after would return fewer than k items
// and a different order than re-ranking the matching set.
std::vector<ItemIdx> masked_items(const Bundle& b, std::vector<ItemIdx> exclude, GenreMask wanted) {
  if (wanted == 0) return exclude;
  const std::uint32_t n = b.n_items();
  std::size_t non_matching = 0;
  for (ItemIdx i = 0; i < n; ++i) non_matching += (b.genre_mask[i] & wanted) == 0;
  exclude.reserve(exclude.size() + non_matching);
  for (ItemIdx i = 0; i < n; ++i) {
    if ((b.genre_mask[i] & wanted) == 0) exclude.push_back(i);
  }
  return exclude;
}

constexpr std::array<std::string_view, 8> kModelNames = {"pop", "pop_count", "knn", "content", "mf", "als", "hybrid", "mmr"};

bool is_contract_model(std::string_view name) {
  return std::find(kModelNames.begin(), kModelNames.end(), name) != kModelNames.end();
}

// Resolves a model name from the request: must be a string, in the contract enum, and loaded.
std::pair<std::string, const Model*> resolve_model(const Bundle& b, const json& v) {
  if (!v.is_string()) bad_request("model must be a string");
  const auto& name = v.get_ref<const std::string&>();
  if (!is_contract_model(name)) throw ApiError{400, "unknown_model", "unknown model" + echo_token(name)};
  const Model* m = b.find_model(name);
  if (m == nullptr) throw ApiError{400, "unknown_model", "model" + echo_token(name) + " is not loaded"};
  return {name, m};
}

// ---- query parameters ---------------------------------------------------------------------------

// The value of query parameter `key`, or nullopt if absent. A repeated parameter is an error.
std::optional<std::string_view> query_param(const Request& req, std::string_view key) {
  std::optional<std::string_view> out;
  for (const auto& [k, v] : req.query) {
    if (k != key) continue;
    if (out) bad_request("duplicate query parameter \"" + std::string(key) + "\"");
    out = v;
  }
  return out;
}

// Plain decimal digits only (no sign, spaces or exponent), within [lo, hi].
std::optional<std::int64_t> parse_decimal(std::string_view s, std::int64_t lo, std::int64_t hi) {
  if (s.empty() || s.size() > 12) return std::nullopt;
  if (!std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; })) return std::nullopt;
  std::int64_t v = 0;
  const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
  if (r.ec != std::errc() || r.ptr != s.data() + s.size() || v < lo || v > hi) return std::nullopt;
  return v;
}

// ---- response builders --------------------------------------------------------------------------

json movie_meta(const Bundle& b, ItemIdx i) {
  const MovieMeta& m = b.catalog.movies[i];
  const std::uint32_t count = b.catalog.rating_count[i];
  return json{
      {"movie_id", b.catalog.items.to_external[i]},
      {"title", m.title},
      {"year", m.year ? json(*m.year) : json(nullptr)},
      {"genres", m.genres},
      {"tmdb_id", m.tmdb_id ? json(*m.tmdb_id) : json(nullptr)},
      {"rating_count", count},
      {"mean_rating", count > 0 && std::isfinite(b.catalog.mean_rating[i]) ? json(tidy(b.catalog.mean_rating[i])) : json(nullptr)},
  };
}

}  // namespace

// ---- logging ------------------------------------------------------------------------------------

LogSink stderr_log_sink() {
  auto mu = std::make_shared<std::mutex>();
  return [mu](std::string_view line) {
    std::lock_guard lock(*mu);
    std::cerr.write(line.data(), static_cast<std::streamsize>(line.size()));
    std::cerr.put('\n');
    std::cerr.flush();
  };
}

void Service::log_event(std::string_view level, std::string_view msg) noexcept {
  try {
    if (log_) log_(dump(json{{"ts", utc_now_ms()}, {"level", level}, {"msg", msg}}));
  } catch (...) {
    // Logging must never take a request down.
  }
}

// ---- the service --------------------------------------------------------------------------------

namespace {

class Handler {
 public:
  Handler(const Bundle* bundle, Metrics& metrics) : b_(bundle), metrics_(metrics) {}

  // Fold-in + top-k + explanations for one model. Every cited item is one the user rated.
  // `exclude` already carries the genre filter (masked_items), so top-k is taken over matching items.
  json run(const std::string& name, const Model& m, const ParsedRatings& in, std::size_t k,
           const std::vector<ItemIdx>& exclude) {
    const auto t0 = std::chrono::steady_clock::now();
    const UserState u = m.fold_in(in.known);
    const std::vector<Scored> top = recommend(m, u, k, exclude);
    metrics_.observe_foldin(name, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());

    std::unordered_set<ItemIdx> rated;
    for (const Rating& r : in.known) rated.insert(r.item);
    const std::uint32_t n = b_->n_items();
    json items = json::array();
    for (const Scored& s : top) {
      if (s.item >= n || !std::isfinite(s.score)) continue;  // defensive: never emit inf/NaN or a bad id
      json because = json::array();
      Explanation e;
      try {
        e = m.explain(u, s.item);
      } catch (const NotImplemented&) {
        // A model without explanations still recommends; `because` stays empty.
      }
      for (const Because& c : e.because) {
        if (because.size() == 3) break;
        if (c.item >= n || !rated.contains(c.item) || !std::isfinite(c.weight)) continue;
        because.push_back({{"movie_id", b_->catalog.items.to_external[c.item]}, {"weight", tidy(c.weight)}});
      }
      items.push_back({{"movie_id", b_->catalog.items.to_external[s.item]}, {"score", tidy(s.score)}, {"because", std::move(because)}});
    }
    return items;
  }

  std::string recommend_route(const Request& req) {
    const json j = parse_object(req.body);
    check_fields(j, {"model", "ratings", "k", "exclude", "genres"}, {"model", "ratings", "k"});
    const auto [name, model] = resolve_model(*b_, j.at("model"));
    const std::size_t k = get_k(j);
    const ParsedRatings ratings = parse_ratings(*b_, j.at("ratings"));
    const auto exclude = masked_items(*b_, parse_exclude(*b_, j), parse_genres(j));
    return dump(json{{"model", name}, {"items", run(name, *model, ratings, k, exclude)}, {"ignored", ratings.ignored}});
  }

  std::string compare_route(const Request& req) {
    const json j = parse_object(req.body);
    check_fields(j, {"models", "ratings", "k", "exclude", "genres"}, {"models", "ratings", "k"});
    const json& names = j.at("models");
    if (!names.is_array() || names.empty() || names.size() > Limits::kMaxCompareModels) {
      bad_request("models must list 1-5 distinct model names");
    }
    std::vector<std::pair<std::string, const Model*>> models;
    std::set<std::string> seen;
    for (const auto& n : names) {
      auto resolved = resolve_model(*b_, n);
      if (!seen.insert(resolved.first).second) bad_request("models must list 1-5 distinct model names");
      models.push_back(std::move(resolved));
    }
    const std::size_t k = get_k(j);
    const ParsedRatings ratings = parse_ratings(*b_, j.at("ratings"));
    const auto exclude = masked_items(*b_, parse_exclude(*b_, j), parse_genres(j));  // once, for every model
    json results = json::array();
    for (const auto& [name, model] : models) {
      results.push_back({{"model", name}, {"items", run(name, *model, ratings, k, exclude)}});
    }
    return dump(json{{"results", std::move(results)}, {"ignored", ratings.ignored}});
  }

  std::string similar_route(const Request& req, std::string_view id_text) {
    const auto id = parse_decimal(id_text, 1, Limits::kMaxMovieId);
    if (!id) bad_request("movie_id must be a positive integer");
    const std::string_view name = query_param(req, "model").value_or("knn");
    if (name != "knn" && name != "content") throw ApiError{400, "unknown_model", "similar supports knn and content"};
    std::size_t k = Limits::kDefaultSimilarK;
    if (const auto kt = query_param(req, "k")) {
      const auto kv = parse_decimal(*kt, 1, static_cast<std::int64_t>(Limits::kMaxK));
      if (!kv) bad_request("k must be an integer between 1 and 100");
      k = static_cast<std::size_t>(*kv);
    }
    const Model* m = b_->find_model(name);
    if (m == nullptr || !m->supports_similar()) {
      throw ApiError{400, "unknown_model", "model \"" + std::string(name) + "\" is not loaded"};
    }
    const auto item = b_->catalog.items.find(*id);
    if (!item) throw ApiError{404, "not_found", "movie " + std::to_string(*id) + " not found"};
    json items = json::array();
    const std::uint32_t n = b_->n_items();
    for (const Scored& s : m->similar(*item, k)) {
      if (items.size() == k) break;
      if (s.item >= n || s.item == *item || !std::isfinite(s.score)) continue;
      items.push_back({{"movie_id", b_->catalog.items.to_external[s.item]}, {"similarity", tidy(s.score)}});
    }
    return dump(json{{"model", name}, {"movie_id", *id}, {"items", std::move(items)}});
  }

  std::string onboarding_route(const Request& req) {
    const json j = parse_object(req.body);
    check_fields(j, {"n", "exclude"}, {"n"});
    const auto n = as_int(j.at("n"), 1, 100);
    if (!n) bad_request("n must be between 1 and 100");
    const auto excl = parse_exclude(*b_, j);
    const std::unordered_set<ItemIdx> exclude(excl.begin(), excl.end());
    json ids = json::array();
    for (ItemIdx i : onboarding_picks(b_->catalog, b_->by_popularity, exclude, static_cast<std::size_t>(*n))) {
      ids.push_back(b_->catalog.items.to_external[i]);
    }
    return dump(json{{"movie_ids", std::move(ids)}});
  }

  std::string search_route(const Request& req) {
    const auto q = query_param(req, "q");
    if (!q) bad_request("q must be 1-100 characters");
    const auto len = utf8_length(*q);
    if (!len) bad_request("q must be valid UTF-8");
    if (*len < 1 || *len > Limits::kMaxQueryChars) bad_request("q must be 1-100 characters");
    std::size_t limit = Limits::kDefaultSearchLimit;
    if (const auto lt = query_param(req, "limit")) {
      const auto lv = parse_decimal(*lt, 1, static_cast<std::int64_t>(Limits::kMaxSearchLimit));
      if (!lv) bad_request("limit must be an integer between 1 and 50");
      limit = static_cast<std::size_t>(*lv);
    }
    json ids = json::array();
    for (ItemIdx i : b_->search.search(*q, limit)) ids.push_back(b_->catalog.items.to_external[i]);
    return dump(json{{"movie_ids", std::move(ids)}});
  }

  std::string batch_route(const Request& req) {
    const json j = parse_object(req.body);
    check_fields(j, {"ids"}, {"ids"});
    const json& ids = j.at("ids");
    if (!ids.is_array() || ids.empty() || ids.size() > Limits::kMaxBatchIds) bad_request("ids must contain 1-200 movie ids");
    json movies = json::array();
    json missing = json::array();
    for (const auto& v : ids) {
      const std::int64_t id = movie_id(v);
      if (const auto i = b_->catalog.items.find(id)) {
        movies.push_back(movie_meta(*b_, *i));
      } else {
        missing.push_back(id);
      }
    }
    return dump(json{{"movies", std::move(movies)}, {"missing", std::move(missing)}});
  }

  std::string models_route() {
    json names = json::array();
    for (const auto& n : b_->manifest.models) {
      if (is_contract_model(n) && b_->find_model(n) != nullptr) names.push_back(n);
    }
    return dump(json{{"artifact_version", b_->version},
                     {"trained_at", b_->manifest.created_at},
                     {"default_model", b_->manifest.default_model},
                     {"models", std::move(names)}});
  }

 private:
  const Bundle* b_;
  Metrics& metrics_;
};

}  // namespace

Service::Service(const BundleHolder& holder, Metrics& metrics, LogSink log)
    : holder_(holder), metrics_(metrics), log_(std::move(log)) {}

Response Service::handle(const Request& req) noexcept {
  const auto t0 = std::chrono::steady_clock::now();
  Response res;
  std::string_view route_template = "other";
  std::string internal_error;  // what() of an unexpected exception, for the single error log line
  try {
    try {
      res.request_id = valid_request_id(req.request_id) ? req.request_id : generate_request_id();
      const std::shared_ptr<const Bundle> bundle = holder_.get();  // one snapshot per request
      res.model_version = bundle ? bundle->version : "none";

      const RouteInfo* route = match_route(req.path);
      if (route == nullptr) throw ApiError{404, "not_found", "no such route"};
      route_template = route->template_;
      if (req.method != route->method) {
        res.allow = std::string(route->method);
        throw ApiError{405, "bad_request", "method not allowed"};
      }
      if (route->route == Route::healthz) {
        res.status = bundle ? 200 : 503;
        res.body = bundle ? R"({"status":"ok"})" : R"({"status":"unavailable"})";
      } else if (route->route == Route::metrics) {
        res.content_type = "text/plain; version=0.0.4; charset=utf-8";
        res.body = metrics_.render(res.model_version);
      } else {
        if (!bundle) throw ApiError{503, "model_unavailable", std::string(kUnavailable)};
        Handler h(bundle.get(), metrics_);
        switch (route->route) {
          case Route::recommend: res.body = h.recommend_route(req); break;
          case Route::compare: res.body = h.compare_route(req); break;
          case Route::similar: res.body = h.similar_route(req, std::string_view(req.path).substr(route->path.size())); break;
          case Route::onboarding: res.body = h.onboarding_route(req); break;
          case Route::search: res.body = h.search_route(req); break;
          case Route::batch: res.body = h.batch_route(req); break;
          case Route::version: res.body = dump(json{{"artifact_version", bundle->version}}); break;
          case Route::models: res.body = h.models_route(); break;
          default: throw ApiError{404, "not_found", "no such route"};
        }
      }
    } catch (const ApiError& e) {
      res.status = e.status;
      res.content_type = "application/json";
      res.body = error_body(e.code, e.message);
    } catch (const std::exception& e) {
      res.status = 500;
      res.content_type = "application/json";
      res.body = error_body("internal", "internal error");
      // The exception text comes from librecs (model or artifact code), never from request data.
      internal_error = e.what();
    } catch (...) {
      res.status = 500;
      res.content_type = "application/json";
      res.body = error_body("internal", "internal error");
      internal_error = "unknown exception";
    }
    if (res.model_version.empty()) res.model_version = "none";
    if (res.request_id.empty()) res.request_id = generate_request_id();

    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    metrics_.observe_request(route_template, res.status, secs);
    if (log_) {
      json line = {{"ts", utc_now_ms()},
                   {"level", res.status >= 500 ? "error" : "info"},
                   {"msg", "request"},
                   {"request_id", res.request_id},
                   {"method", safe_method(req.method)},
                   {"route", route_template},
                   {"status", res.status},
                   {"latency_ms", std::round(secs * 1e6) / 1e3},
                   {"model_version", res.model_version}};
      if (!internal_error.empty()) line["error"] = internal_error;
      log_(dump(line));
    }
  } catch (...) {
    // Only reachable on allocation failure or a throwing log sink.
    res.status = 500;
    res.content_type = "application/json";
    res.body = R"({"error":{"code":"internal","message":"internal error"}})";
    if (res.model_version.empty()) res.model_version = "none";
  }
  return res;
}

Response Service::transport_error(int http_status, std::string_view request_id) noexcept {
  int status = http_status >= 400 && http_status < 600 ? http_status : 400;
  std::string_view code = "bad_request";
  std::string_view message = "malformed HTTP request";
  if (http_status == 413) {
    status = 400;
    code = "body_too_large";
    message = "request body exceeds 1 MiB";
  } else if (http_status == 414) {
    status = 400;
    message = "request URI too long";
  } else if (http_status == 431) {
    status = 400;
    message = "request headers too large";
  } else if (http_status >= 500) {
    code = "internal";
    message = "internal error";
  } else if (http_status == 404) {
    code = "not_found";
    message = "no such route";
  }
  return error_response(status, code, message, request_id);
}

Response Service::error_response(int status, std::string_view code, std::string_view message,
                                 std::string_view request_id) noexcept {
  Response res;
  try {
    res.request_id = valid_request_id(request_id) ? std::string(request_id) : generate_request_id();
    const auto bundle = holder_.get();
    res.model_version = bundle ? bundle->version : "none";
    res.status = status;
    res.body = error_body(code, message);
    metrics_.observe_request("other", res.status, 0.0);
    if (log_) {
      log_(dump(json{{"ts", utc_now_ms()}, {"level", "info"}, {"msg", "request"}, {"request_id", res.request_id},
                     {"method", "OTHER"}, {"route", "other"}, {"status", res.status}, {"latency_ms", 0.0},
                     {"model_version", res.model_version}}));
    }
  } catch (...) {
    res.status = 500;
    res.body = R"({"error":{"code":"internal","message":"internal error"}})";
    if (res.model_version.empty()) res.model_version = "none";
  }
  return res;
}

}  // namespace recs::service
