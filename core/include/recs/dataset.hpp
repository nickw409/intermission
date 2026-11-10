// MovieLens ingest, dataset construction and the binary cache. Workstream A owns the implementation.
// FROZEN after Wave 0.
#pragma once

#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include "recs/types.hpp"

namespace recs {

namespace fs = std::filesystem;

class DatasetError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

/// Parses ratings.csv, movies.csv, links.csv (optional), tags.csv (optional) and, if present,
/// genome-scores.csv + genome-tags.csv from `raw_dir`. Fails loudly (CsvError / DatasetError
/// with file and row) on malformed rows, out-of-range ratings, duplicate (user, movie) pairs, or a
/// rating for a movie missing from movies.csv. Tags whose movie is unknown are an error too.
Dataset load_movielens(const fs::path& raw_dir, std::string name);

/// Builds a Dataset from already-dense triplets (row = user, col = item). Used by tests, synthetic
/// data and the retrain merge. External IDs are set equal to the dense indices unless `users` /
/// `items` are supplied; `movies` gets placeholder titles ("Movie <id>") if empty.
Dataset make_dataset(std::string name, std::uint32_t n_users, std::uint32_t n_items,
                     std::vector<Triplet> ratings, std::vector<MovieMeta> movies = {});

/// Phase 5 (workstream P): appends website ratings from a recs-api export (contracts/export.md)
/// as extra users after the MovieLens users. Ratings for movies missing from the catalog are
/// dropped and counted. Returns the merged dataset; `site_users` receives the external site user
/// IDs in their new dense order.
Dataset merge_site_ratings(const Dataset& movielens, const fs::path& export_csv,
                           std::vector<std::int64_t>* site_users = nullptr,
                           std::size_t* dropped = nullptr);

/// Binary cache: header {magic "RECSDS01", format version, counts}, payload, trailing SHA-256 of
/// header+payload. load_cache verifies everything and throws DatasetError on any mismatch,
/// truncation or version change, so a stale or corrupt cache is never used silently.
void save_cache(const Dataset& ds, const fs::path& file);
Dataset load_cache(const fs::path& file);

/// Loads `cache_file` if it exists and verifies; otherwise parses `raw_dir` and writes the cache.
Dataset load_or_build(const fs::path& raw_dir, const fs::path& cache_file, std::string name);

}  // namespace recs
