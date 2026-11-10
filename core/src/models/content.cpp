// Content-based recommender ("content").
//
// THE IDEA
//   Describe every movie by a feature vector built from what the movie *is* (its tags and genres),
//   not from who rated it. A user's taste is then a direction in that feature space, and a movie is
//   recommended when its vector points the same way. Because item vectors never depend on ratings,
//   a brand-new movie with tags/genres can be recommended immediately (no item cold start), but the
//   model can only ever suggest "more of the same" (over-specialization).
//
// ITEM VECTORS v_i (one row per movie, stored sparse)
//   Genome path (Dataset::genome present): v_i = the movie's tag-genome relevance row
//     (Vig, Sen & Riedl 2012), L2-normalized. Movies without genome scores have an all-zero row.
//   TF-IDF path (otherwise; ml-latest-small and ml-32m have no genome):
//     * Terms are tag strings from train.tags, ASCII case-folded and whitespace-trimmed, so
//       "Sci-Fi", "sci-fi " and "SCI-FI" are one term.
//     * Corpus: the N movies with at least one tag application. df(t) = number of distinct movies
//       tagged with t. Terms with df(t) < min_tag_count are dropped (typos and one-off tags carry
//       no similarity information and only bloat the vocabulary).
//     * Weight of term t on movie i, with c = number of applications of t to i:
//           w(i, t) = (1 + ln c) * ln(N / df(t))
//       The sublinear tf (1 + ln c) stops one tag applied by hundreds of users from drowning out
//       the rest (Manning, Raghavan & Schuetze 2008, sec. 6.4.1). A term on every tagged movie gets
//       idf 0 and is left out.
//     * The tag block of each movie is L2-normalized, so a heavily tagged blockbuster and a movie
//       with two tags carry tag information of the same total weight.
//     * Genres are appended as one-hot features with value genre_weight. With genre_weight = 1 a
//       single genre weighs as much as the whole (unit-length) tag block; larger values make
//       genres dominate, smaller values make tags dominate.
//     * Finally the whole vector is L2-normalized.
//   Feature order: kept terms sorted by byte value, then genres sorted by name. A movie with no
//   features at all has a zero vector and is never recommended (score -infinity).
//
// USER PROFILE (fold-in)
//       mu_u = mean of the user's ratings,     p_u = sum_j (r_uj - mu_u) * v_j
//   Mean-centering turns ratings into likes (+) and dislikes (-): a movie rated below the user's
//   own average pushes the profile away from its features. The sum runs over the ratings in item
//   order and is accumulated in double, then stored as float in UserState::vec.
//   Note: if every rating equals the mean (one rating, or all ratings equal), p_u = 0 and nothing
//   can be scored.
//
// SCORE
//       score(u, i) = cos(p_u, v_i) = (p_u . v_i) / (|p_u| |v_i|)
//   -infinity when |v_i| = 0 or |p_u| = 0. The dot products are a sparse matrix (items x features)
//   times dense vector product, one item per loop iteration, each summed sequentially in double:
//   the result does not depend on the number of threads.
//
// EXPLANATION
//   The score splits exactly into per-rated-movie contributions:
//       score(u, i) = sum_j c_j,   c_j = (r_uj - mu_u) (v_j . v_i) / (|p_u| |v_i|)
//   explain() cites the (at most 3) rated movies with the largest positive c_j.
//
// SIMILAR ITEMS
//   similar(i, k): the k movies with the largest positive cosine cos(v_i, v_j), j != i.
//
// BATCH PATH
//   score_trained_user(u) builds p_u from the stored training row of user u with the very same
//   function fold_in() uses (fold_in sorts its input by item first, matching CSR row order), so
//   the two paths agree bit for bit.
//
// PERSISTENCE (save/load; directory contents)
//   features.bin: little-endian; header {magic "RECSCNT1", u32 format version, u32 n_items,
//   u32 n_features, u32 n_terms, u32 n_genres, u32 flags (bit 0 = genome), u64 nnz,
//   u32 min_tag_count, f64 genre_weight} (52 bytes; the config copy must match config.json), then
//   indptr (u64 x (n_items + 1)), feature indices (u32 x nnz), values (f32 x nnz), then the SHA-256
//   of everything before it. load() checks sizes against the file length before allocating and
//   validates every invariant (also for a file with a correct checksum).
//   config.json: the config fit() used. The training matrix is not saved, so a loaded model has no
//   batch path (supports_trained_scoring() == false).
//
// REFERENCES
//   G. Salton, C. Buckley. Term-weighting approaches in automatic text retrieval. Information
//     Processing & Management 24(5), 1988.
//   C. D. Manning, P. Raghavan, H. Schuetze. Introduction to Information Retrieval, ch. 6
//     (tf-idf weighting, sublinear tf scaling, the vector space model and cosine similarity), 2008.
//   M. J. Pazzani, D. Billsus. Content-based recommendation systems. In: The Adaptive Web, LNCS
//     4321, 2007.
//   P. Lops, M. de Gemmis, G. Semeraro. Content-based recommender systems: state of the art and
//     trends. In: Recommender Systems Handbook, 2011.
//   J. Vig, S. Sen, J. Riedl. The Tag Genome: encoding community knowledge to support novel
//     interaction. ACM TiiS 2(3), 2012.
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "recs/model.hpp"
#include "recs/sha256.hpp"

namespace recs {
namespace {

static_assert(std::endian::native == std::endian::little, "features.bin is written in host byte order");

constexpr float kNegInf = -std::numeric_limits<float>::infinity();
constexpr char kMagic[8] = {'R', 'E', 'C', 'S', 'C', 'N', 'T', '1'};
constexpr std::uint32_t kFormatVersion = 1;
constexpr std::uint32_t kFlagGenome = 1;
constexpr const char* kFeaturesFile = "features.bin";
constexpr const char* kConfigFile = "config.json";

// ---------------------------------------------------------------------------------------------
// Config

struct ContentConfig {
  std::int64_t min_tag_count = 2;
  double genre_weight = 1.0;

  Config to_json() const { return Config{{"min_tag_count", min_tag_count}, {"genre_weight", genre_weight}}; }
};

ContentConfig parse_config(const Config& cfg) {
  ContentConfig c;
  if (cfg.is_null()) return c;
  if (!cfg.is_object()) throw std::invalid_argument("content config must be a JSON object");
  for (const auto& [key, value] : cfg.items()) {
    if (key == "min_tag_count") {
      if (!value.is_number_integer()) throw std::invalid_argument("content: min_tag_count must be an integer");
      c.min_tag_count = value.get<std::int64_t>();
      if (c.min_tag_count < 1 || c.min_tag_count > std::numeric_limits<std::uint32_t>::max())
        throw std::invalid_argument("content: min_tag_count must be >= 1");
    } else if (key == "genre_weight") {
      if (!value.is_number()) throw std::invalid_argument("content: genre_weight must be a number");
      c.genre_weight = value.get<double>();
      if (!std::isfinite(c.genre_weight) || c.genre_weight < 0.0)
        throw std::invalid_argument("content: genre_weight must be finite and >= 0");
    } else {
      throw std::invalid_argument("content: unknown config key '" + key + "'");
    }
  }
  return c;
}

// ---------------------------------------------------------------------------------------------
// Helpers

/// Trims ASCII whitespace and lower-cases ASCII letters. Other bytes (UTF-8) are kept as they are.
std::string fold_tag(std::string_view s) {
  const auto is_space = [](char ch) { return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v'; };
  while (!s.empty() && is_space(s.front())) s.remove_prefix(1);
  while (!s.empty() && is_space(s.back())) s.remove_suffix(1);
  std::string out(s);
  for (char& ch : out)
    if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
  return out;
}

/// One sparse row under construction: (feature, weight) pairs in increasing feature order.
using SparseRow = std::vector<std::pair<std::uint32_t, double>>;

double l2_norm(const SparseRow& row) {
  double ss = 0.0;
  for (const auto& [f, w] : row) ss += w * w;
  return std::sqrt(ss);
}

/// Item x feature matrix in CSR form (no timestamps, unlike recs::Csr).
struct FeatureMatrix {
  std::vector<std::uint64_t> indptr{0};
  std::vector<std::uint32_t> indices;
  std::vector<float> values;

  std::uint32_t n_rows() const { return static_cast<std::uint32_t>(indptr.size() - 1); }
  std::span<const std::uint32_t> row_indices(std::uint32_t r) const {
    return {indices.data() + indptr[r], static_cast<std::size_t>(indptr[r + 1] - indptr[r])};
  }
  std::span<const float> row_values(std::uint32_t r) const {
    return {values.data() + indptr[r], static_cast<std::size_t>(indptr[r + 1] - indptr[r])};
  }
  /// Appends `row` scaled to unit length (an empty or all-zero row stays empty).
  void push_normalized(const SparseRow& row) {
    const double norm = l2_norm(row);
    if (norm > 0.0) {
      for (const auto& [f, w] : row) {
        indices.push_back(f);
        values.push_back(static_cast<float>(w / norm));
      }
    }
    indptr.push_back(indices.size());
  }
};

/// Sparse dot product of two rows with increasing feature indices, in double.
double sparse_dot(std::span<const std::uint32_t> ia, std::span<const float> va,
                  std::span<const std::uint32_t> ib, std::span<const float> vb) {
  double dot = 0.0;
  std::size_t a = 0, b = 0;
  while (a < ia.size() && b < ib.size()) {
    if (ia[a] < ib[b]) {
      ++a;
    } else if (ib[b] < ia[a]) {
      ++b;
    } else {
      dot += static_cast<double>(va[a]) * static_cast<double>(vb[b]);
      ++a;
      ++b;
    }
  }
  return dot;
}

// Binary I/O. Everything is little-endian (static_assert above), so values are copied as bytes.
template <class T>
void append_bytes(std::string& out, const T& v) {
  out.append(reinterpret_cast<const char*>(&v), sizeof(T));
}
template <class T>
void append_array(std::string& out, const std::vector<T>& v) {
  out.append(reinterpret_cast<const char*>(v.data()), v.size() * sizeof(T));
}

class Reader {
 public:
  Reader(const std::string& bytes, std::size_t end) : bytes_(bytes), end_(end) {}
  template <class T>
  T get() {
    need(sizeof(T));
    T v;
    std::memcpy(&v, bytes_.data() + pos_, sizeof(T));
    pos_ += sizeof(T);
    return v;
  }
  template <class T>
  std::vector<T> get_array(std::uint64_t n) {
    if (n > (end_ - pos_) / sizeof(T)) fail("truncated");
    std::vector<T> v(n);
    if (n > 0) std::memcpy(v.data(), bytes_.data() + pos_, n * sizeof(T));
    pos_ += n * sizeof(T);
    return v;
  }
  std::size_t pos() const { return pos_; }
  [[noreturn]] static void fail(const std::string& why) { throw std::runtime_error("content model file: " + why); }

 private:
  void need(std::size_t n) const {
    if (n > end_ - pos_) fail("truncated");
  }
  const std::string& bytes_;
  std::size_t end_;
  std::size_t pos_ = 0;
};

// ---------------------------------------------------------------------------------------------
// The model

class ContentModel final : public Model {
 public:
  std::string_view name() const override { return "content"; }

  void fit(const Dataset& train, const Config& cfg, const TrainContext& ctx) override {
    (void)ctx;  // no randomness; the result is the same for every seed and thread count
    const ContentConfig c = parse_config(cfg);
    const std::uint32_t n_items = train.n_items();
    if (train.movies.size() != n_items) throw std::invalid_argument("content: movies.size() != n_items");

    FeatureMatrix features;
    std::uint32_t n_terms = 0, n_genres = 0;
    bool genome = false;
    if (train.genome) {
      genome = true;
      n_terms = build_genome_features(*train.genome, n_items, features);
    } else {
      build_tfidf_features(train, c, features, n_terms, n_genres);
    }

    // Commit (fit() either succeeds completely or leaves the model as it was).
    cfg_ = c;
    n_items_ = n_items;
    n_terms_ = n_terms;
    n_genres_ = n_genres;
    n_features_ = n_terms + n_genres;
    genome_ = genome;
    features_ = std::move(features);
    compute_norms();
    n_train_users_ = train.n_users();
    train_indptr_ = train.by_user.indptr;
    train_items_ = train.by_user.indices;
    train_values_ = train.by_user.values;
  }

  UserState fold_in(std::span<const Rating> ratings) const override {
    UserState s;
    s.ratings.assign(ratings.begin(), ratings.end());
    for (const auto& r : s.ratings) {
      if (r.item >= n_items_) throw std::invalid_argument("content: rating for unknown item " + std::to_string(r.item));
      if (!std::isfinite(r.value)) throw std::invalid_argument("content: non-finite rating value");
    }
    // Item order, so the arithmetic matches the batch path (CSR rows are item-ascending).
    std::sort(s.ratings.begin(), s.ratings.end(), [](const Rating& a, const Rating& b) { return a.item < b.item; });
    std::vector<std::uint32_t> items(s.ratings.size());
    std::vector<float> values(s.ratings.size());
    for (std::size_t k = 0; k < s.ratings.size(); ++k) {
      if (k > 0 && s.ratings[k].item == s.ratings[k - 1].item)
        throw std::invalid_argument("content: duplicate rating for item " + std::to_string(s.ratings[k].item));
      items[k] = s.ratings[k].item;
      values[k] = s.ratings[k].value;
    }
    s.vec = build_profile(items, values, s.mean);
    return s;
  }

  void score(const UserState& u, std::span<float> out) const override {
    if (u.vec.size() != n_features_) throw std::invalid_argument("content: user state has the wrong profile size");
    score_profile(u.vec, out);
  }

  Explanation explain(const UserState& u, ItemIdx item) const override {
    if (item >= n_items_) throw std::out_of_range("content: explain() for unknown item");
    if (u.vec.size() != n_features_) throw std::invalid_argument("content: user state has the wrong profile size");
    Explanation e;
    const double p_norm = profile_norm(u.vec);
    if (p_norm == 0.0 || norms_[item] == 0.0) return e;
    const auto ii = features_.row_indices(item);
    const auto iv = features_.row_values(item);
    for (const auto& r : u.ratings) {
      if (r.item >= n_items_) throw std::invalid_argument("content: user state cites an unknown item");
      const double w = static_cast<double>(r.value) - u.mean;
      if (w == 0.0) continue;
      const double c = w * sparse_dot(features_.row_indices(r.item), features_.row_values(r.item), ii, iv) /
                       (p_norm * norms_[item]);
      const auto weight = static_cast<float>(c);
      if (weight > 0.0f) e.because.push_back({r.item, weight});
    }
    std::sort(e.because.begin(), e.because.end(), [](const Because& a, const Because& b) {
      if (a.weight != b.weight) return a.weight > b.weight;
      return a.item < b.item;
    });
    if (e.because.size() > 3) e.because.resize(3);
    return e;
  }

  std::vector<Scored> similar(ItemIdx item, std::size_t k) const override {
    if (item >= n_items_) throw std::out_of_range("content: similar() for unknown item");
    std::vector<Scored> out;
    if (k == 0 || norms_[item] == 0.0) return out;
    // Scatter the query row into a dense vector, then one sparse dot per candidate.
    std::vector<double> q(n_features_, 0.0);
    const auto qi = features_.row_indices(item);
    const auto qv = features_.row_values(item);
    for (std::size_t t = 0; t < qi.size(); ++t) q[qi[t]] = static_cast<double>(qv[t]);
    for (ItemIdx j = 0; j < n_items_; ++j) {
      if (j == item || norms_[j] == 0.0) continue;
      const auto ji = features_.row_indices(j);
      const auto jv = features_.row_values(j);
      double dot = 0.0;
      for (std::size_t t = 0; t < ji.size(); ++t) dot += q[ji[t]] * static_cast<double>(jv[t]);
      const auto cos = static_cast<float>(dot / (norms_[item] * norms_[j]));
      if (cos > 0.0f) out.push_back({j, cos});
    }
    const auto better = [](const Scored& a, const Scored& b) {
      if (a.score != b.score) return a.score > b.score;
      return a.item < b.item;
    };
    if (out.size() > k) {
      std::partial_sort(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(k), out.end(), better);
      out.resize(k);
    } else {
      std::sort(out.begin(), out.end(), better);
    }
    return out;
  }
  bool supports_similar() const override { return true; }

  bool supports_trained_scoring() const override { return !train_indptr_.empty(); }
  void score_trained_user(UserIdx u, std::span<float> out) const override {
    if (!supports_trained_scoring()) throw NotImplemented("content: no training matrix (model was loaded, not fit)");
    if (u >= n_train_users_) throw std::out_of_range("content: score_trained_user() for unknown user");
    const auto begin = train_indptr_[u], end = train_indptr_[u + 1];
    const std::span<const std::uint32_t> items(train_items_.data() + begin, end - begin);
    const std::span<const float> values(train_values_.data() + begin, end - begin);
    double mean = 0.0;
    score_profile(build_profile(items, values, mean), out);
  }

  std::uint32_t n_items() const override { return n_items_; }
  Config config() const override { return cfg_.to_json(); }

  void save(const fs::path& dir) const override {
    std::string bytes;
    bytes.append(kMagic, sizeof kMagic);
    append_bytes(bytes, kFormatVersion);
    append_bytes(bytes, n_items_);
    append_bytes(bytes, n_features_);
    append_bytes(bytes, n_terms_);
    append_bytes(bytes, n_genres_);
    append_bytes(bytes, genome_ ? kFlagGenome : std::uint32_t{0});
    append_bytes(bytes, static_cast<std::uint64_t>(features_.indices.size()));
    append_bytes(bytes, static_cast<std::uint32_t>(cfg_.min_tag_count));
    append_bytes(bytes, cfg_.genre_weight);
    append_array(bytes, features_.indptr);
    append_array(bytes, features_.indices);
    append_array(bytes, features_.values);
    Sha256 sha;
    sha.update(std::string_view(bytes));
    const auto digest = sha.digest();
    bytes.append(reinterpret_cast<const char*>(digest.data()), digest.size());
    write_file(dir / kFeaturesFile, bytes);
    write_file(dir / kConfigFile, cfg_.to_json().dump());
  }

  void load(const fs::path& dir) override {
    // Config first: it is small and its parser rejects anything unexpected.
    Config json;
    try {
      json = Config::parse(read_file(dir / kConfigFile));
    } catch (const nlohmann::json::exception& e) {
      Reader::fail(std::string("config.json: ") + e.what());
    }
    ContentConfig c;
    try {
      c = parse_config(json);
    } catch (const std::invalid_argument& e) {
      Reader::fail(std::string("config.json: ") + e.what());
    }

    const std::string bytes = read_file(dir / kFeaturesFile);
    constexpr std::size_t kDigest = 32;
    if (bytes.size() < sizeof kMagic + kDigest) Reader::fail("truncated");
    const std::size_t body = bytes.size() - kDigest;
    Sha256 sha;
    sha.update(std::string_view(bytes.data(), body));
    const auto digest = sha.digest();
    if (std::memcmp(digest.data(), bytes.data() + body, kDigest) != 0) Reader::fail("checksum mismatch");

    Reader in(bytes, body);
    const auto magic = in.get<std::array<char, 8>>();
    if (std::memcmp(magic.data(), kMagic, sizeof kMagic) != 0) Reader::fail("bad magic");
    if (in.get<std::uint32_t>() != kFormatVersion) Reader::fail("unsupported format version");
    const auto n_items = in.get<std::uint32_t>();
    const auto n_features = in.get<std::uint32_t>();
    const auto n_terms = in.get<std::uint32_t>();
    const auto n_genres = in.get<std::uint32_t>();
    const auto flags = in.get<std::uint32_t>();
    const auto nnz = in.get<std::uint64_t>();
    const auto min_tag_count = in.get<std::uint32_t>();
    const auto genre_weight = in.get<double>();
    if (min_tag_count != c.min_tag_count || std::bit_cast<std::uint64_t>(genre_weight) != std::bit_cast<std::uint64_t>(c.genre_weight))
      Reader::fail("config.json does not match the feature file");
    if ((flags & ~kFlagGenome) != 0) Reader::fail("unknown flags");
    const bool genome = (flags & kFlagGenome) != 0;
    if (static_cast<std::uint64_t>(n_terms) + n_genres != n_features) Reader::fail("n_terms + n_genres != n_features");
    if (genome && n_genres != 0) Reader::fail("a genome model has no genre features");
    // Exact size check before any allocation (each term is bounded by the file size, so no overflow).
    const std::uint64_t remaining = body - in.pos();
    if (n_items >= remaining / 8 || nnz > remaining / 8 ||
        (static_cast<std::uint64_t>(n_items) + 1) * 8 + nnz * 8 != remaining)
      Reader::fail("size does not match the header");

    FeatureMatrix f;
    f.indptr = in.get_array<std::uint64_t>(static_cast<std::uint64_t>(n_items) + 1);
    f.indices = in.get_array<std::uint32_t>(nnz);
    f.values = in.get_array<float>(nnz);
    if (f.indptr.front() != 0 || f.indptr.back() != nnz) Reader::fail("bad row pointers");
    for (std::uint32_t r = 0; r < n_items; ++r) {
      if (f.indptr[r] > f.indptr[r + 1]) Reader::fail("bad row pointers");
      const auto idx = f.row_indices(r);
      for (std::size_t t = 0; t < idx.size(); ++t) {
        if (idx[t] >= n_features) Reader::fail("feature index out of range");
        if (t > 0 && idx[t] <= idx[t - 1]) Reader::fail("feature indices not increasing");
      }
    }
    // Stored rows are unit vectors of positive weights, so every value is in (0, 1]. This also
    // keeps every dot product finite.
    for (float v : f.values)
      if (!(v > 0.0f && v <= 1.0f)) Reader::fail("feature value out of range");

    cfg_ = c;
    n_items_ = n_items;
    n_features_ = n_features;
    n_terms_ = n_terms;
    n_genres_ = n_genres;
    genome_ = genome;
    features_ = std::move(f);
    compute_norms();
    n_train_users_ = 0;
    train_indptr_.clear();
    train_items_.clear();
    train_values_.clear();
  }

 private:
  /// Genome rows (zeros skipped), L2-normalized. Returns the number of features (genome tags).
  static std::uint32_t build_genome_features(const GenomeMatrix& g, std::uint32_t n_items, FeatureMatrix& out) {
    if (g.n_items != n_items || g.relevance.size() != static_cast<std::size_t>(g.n_items) * g.n_tags)
      throw std::invalid_argument("content: genome shape does not match the catalog");
    SparseRow row;
    for (std::uint32_t i = 0; i < n_items; ++i) {
      row.clear();
      for (std::uint32_t t = 0; t < g.n_tags; ++t) {
        const float r = g.relevance[static_cast<std::size_t>(i) * g.n_tags + t];
        if (!std::isfinite(r) || r < 0.0f) throw std::invalid_argument("content: genome relevance must be finite and >= 0");
        if (r > 0.0f) row.emplace_back(t, static_cast<double>(r));
      }
      out.push_normalized(row);
    }
    return g.n_tags;
  }

  /// TF-IDF over folded tags plus one-hot genres (see the comment at the top of the file).
  static void build_tfidf_features(const Dataset& train, const ContentConfig& c, FeatureMatrix& out,
                                   std::uint32_t& n_terms, std::uint32_t& n_genres) {
    const std::uint32_t n_items = train.n_items();

    // 1. Fold every tag; the vocabulary is the sorted set of distinct folded strings.
    std::vector<std::string> folded;
    folded.reserve(train.tags.size());
    for (const auto& t : train.tags) {
      if (t.item >= n_items) throw std::invalid_argument("content: tag for unknown item");
      folded.push_back(fold_tag(t.tag));
    }
    std::vector<std::string> vocab = folded;
    std::sort(vocab.begin(), vocab.end());
    vocab.erase(std::unique(vocab.begin(), vocab.end()), vocab.end());

    // 2. (item, term) pairs, sorted, so runs give the count c of each term on each item.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> pairs;
    pairs.reserve(folded.size());
    for (std::size_t k = 0; k < folded.size(); ++k) {
      if (folded[k].empty()) continue;
      const auto term = static_cast<std::uint32_t>(std::lower_bound(vocab.begin(), vocab.end(), folded[k]) - vocab.begin());
      pairs.emplace_back(train.tags[k].item, term);
    }
    folded.clear();
    folded.shrink_to_fit();
    std::sort(pairs.begin(), pairs.end());

    // 3. Document frequencies and N (items with at least one non-empty tag).
    std::vector<std::uint32_t> df(vocab.size(), 0);
    std::uint64_t n_docs = 0;
    for (std::size_t k = 0; k < pairs.size(); ++k) {
      if (k == 0 || pairs[k] != pairs[k - 1]) ++df[pairs[k].second];
      if (k == 0 || pairs[k].first != pairs[k - 1].first) ++n_docs;
    }

    // 4. Kept terms get compact ids in vocabulary (sorted) order.
    constexpr std::uint32_t kDropped = std::numeric_limits<std::uint32_t>::max();
    std::vector<std::uint32_t> term_id(vocab.size(), kDropped);
    n_terms = 0;
    for (std::size_t t = 0; t < vocab.size(); ++t)
      if (df[t] >= static_cast<std::uint64_t>(c.min_tag_count)) term_id[t] = n_terms++;

    // 5. Genre vocabulary: sorted distinct names.
    std::vector<std::string> genres;
    for (const auto& m : train.movies) genres.insert(genres.end(), m.genres.begin(), m.genres.end());
    std::sort(genres.begin(), genres.end());
    genres.erase(std::unique(genres.begin(), genres.end()), genres.end());
    n_genres = static_cast<std::uint32_t>(genres.size());

    // 6. One row per item: unit-length tag block, then genre one-hots, then normalize the whole.
    SparseRow row, genre_part;
    std::size_t k = 0;
    for (std::uint32_t i = 0; i < n_items; ++i) {
      row.clear();
      while (k < pairs.size() && pairs[k].first == i) {
        const auto term = pairs[k].second;
        std::uint64_t count = 0;
        while (k < pairs.size() && pairs[k].first == i && pairs[k].second == term) {
          ++count;
          ++k;
        }
        if (term_id[term] == kDropped) continue;
        const double tf = 1.0 + std::log(static_cast<double>(count));
        const double idf = std::log(static_cast<double>(n_docs) / static_cast<double>(df[term]));
        if (idf > 0.0) row.emplace_back(term_id[term], tf * idf);
      }
      // Kept-term ids are increasing in vocabulary order, and pairs are sorted by vocabulary
      // index within an item, so `row` is already sorted by feature.
      const double tag_norm = l2_norm(row);
      for (auto& [f, w] : row) w /= tag_norm;

      genre_part.clear();
      if (c.genre_weight > 0.0) {
        for (const auto& g : train.movies[i].genres) {
          const auto gid = static_cast<std::uint32_t>(std::lower_bound(genres.begin(), genres.end(), g) - genres.begin());
          genre_part.emplace_back(n_terms + gid, c.genre_weight);
        }
        std::sort(genre_part.begin(), genre_part.end());
        genre_part.erase(std::unique(genre_part.begin(), genre_part.end()), genre_part.end());
      }
      row.insert(row.end(), genre_part.begin(), genre_part.end());
      out.push_normalized(row);
    }
  }

  /// |v_i| of the stored float rows, in double. Fit and load both call this, so a loaded model
  /// scores exactly like the one that was saved.
  void compute_norms() {
    norms_.assign(n_items_, 0.0);
    for (std::uint32_t i = 0; i < n_items_; ++i) {
      double ss = 0.0;
      for (float v : features_.row_values(i)) ss += static_cast<double>(v) * static_cast<double>(v);
      norms_[i] = std::sqrt(ss);
    }
  }

  /// p = sum_k (values[k] - mean) v_{items[k]}, in the order given, accumulated in double.
  std::vector<float> build_profile(std::span<const std::uint32_t> items, std::span<const float> values,
                                   double& mean) const {
    mean = 0.0;
    std::vector<double> p(n_features_, 0.0);
    if (!items.empty()) {
      double sum = 0.0;
      for (float v : values) sum += static_cast<double>(v);
      mean = sum / static_cast<double>(values.size());
      for (std::size_t k = 0; k < items.size(); ++k) {
        const double w = static_cast<double>(values[k]) - mean;
        if (w == 0.0) continue;
        const auto fi = features_.row_indices(items[k]);
        const auto fv = features_.row_values(items[k]);
        for (std::size_t t = 0; t < fi.size(); ++t) p[fi[t]] += w * static_cast<double>(fv[t]);
      }
    }
    return std::vector<float>(p.begin(), p.end());
  }

  static double profile_norm(std::span<const float> p) {
    double ss = 0.0;
    for (float v : p) ss += static_cast<double>(v) * static_cast<double>(v);
    return std::sqrt(ss);
  }

  /// out[i] = cos(p, v_i), or -infinity. One item per iteration, each dot summed sequentially:
  /// identical results for any thread count.
  void score_profile(std::span<const float> profile, std::span<float> out) const {
    if (out.size() != n_items_) throw std::invalid_argument("content: score() output size != n_items");
    const double p_norm = profile_norm(profile);
    if (p_norm == 0.0) {
      std::fill(out.begin(), out.end(), kNegInf);
      return;
    }
    const std::vector<double> p(profile.begin(), profile.end());
    const auto n = static_cast<std::int64_t>(n_items_);
    // Small catalogs are not worth a thread team; and when score() is already called from inside
    // a parallel region (the evaluator), OpenMP runs this loop on the calling thread.
#pragma omp parallel for schedule(static) if (n >= 4096)
    for (std::int64_t i = 0; i < n; ++i) {
      const auto item = static_cast<std::uint32_t>(i);
      if (norms_[item] == 0.0) {
        out[item] = kNegInf;
        continue;
      }
      const auto fi = features_.row_indices(item);
      const auto fv = features_.row_values(item);
      double dot = 0.0;
      for (std::size_t t = 0; t < fi.size(); ++t) dot += p[fi[t]] * static_cast<double>(fv[t]);
      out[item] = static_cast<float>(dot / (p_norm * norms_[item]));
    }
  }

  static void write_file(const fs::path& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("content: cannot write " + path.string());
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!out) throw std::runtime_error("content: error writing " + path.string());
  }

  static std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("content: cannot read " + path.string());
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad()) throw std::runtime_error("content: error reading " + path.string());
    return bytes;
  }

  ContentConfig cfg_;
  std::uint32_t n_items_ = 0;
  std::uint32_t n_features_ = 0;
  std::uint32_t n_terms_ = 0;
  std::uint32_t n_genres_ = 0;
  bool genome_ = false;
  FeatureMatrix features_;
  std::vector<double> norms_;

  // The training matrix (users x items), kept for the batch path only; not saved.
  std::uint32_t n_train_users_ = 0;
  std::vector<std::uint64_t> train_indptr_;
  std::vector<std::uint32_t> train_items_;
  std::vector<float> train_values_;
};

}  // namespace

std::unique_ptr<Model> make_content() { return std::make_unique<ContentModel>(); }

}  // namespace recs
