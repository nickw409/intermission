// make_dataset: builds a Dataset from dense triplets (spine-owned, see recs/dataset.hpp).
#include <string>

#include "recs/dataset.hpp"
#include "recs/sparse.hpp"

namespace recs {

Dataset make_dataset(std::string name, std::uint32_t n_users, std::uint32_t n_items,
                     std::vector<Triplet> ratings, std::vector<MovieMeta> movies) {
  Dataset ds;
  ds.name = std::move(name);
  for (std::uint32_t u = 0; u < n_users; ++u) ds.users.insert(u);
  if (movies.empty()) {
    movies.reserve(n_items);
    for (std::uint32_t i = 0; i < n_items; ++i) {
      MovieMeta m;
      m.movie_id = i;
      m.title = "Movie " + std::to_string(i);
      movies.push_back(std::move(m));
    }
  }
  if (movies.size() != n_items) throw DatasetError("make_dataset: movies.size() != n_items");
  for (const auto& m : movies) {
    const auto before = ds.items.size();
    if (ds.items.insert(m.movie_id) != before) throw DatasetError("make_dataset: duplicate movie_id");
  }
  ds.movies = std::move(movies);
  try {
    ds.by_user = csr_from_triplets(n_users, n_items, std::move(ratings));
  } catch (const SparseError& e) {
    throw DatasetError(std::string("make_dataset: ") + e.what());
  }
  ds.by_item = transpose(ds.by_user);
  return ds;
}

}  // namespace recs
