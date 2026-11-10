// Dataset names become path components (data/raw/<name>, data/cache/<name>.bin and
// configs/<model>.<name>.tuned.json), so every recs-train command checks them first.
#pragma once
#include <stdexcept>
#include <string>

namespace recs::cli {

inline void check_dataset_name(const std::string& name) {
  const bool ok = !name.empty() && name != "." && name != ".." &&
                  name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") ==
                      std::string::npos;
  if (!ok) throw std::runtime_error("invalid dataset name '" + name + "' (use letters, digits, '.', '_', '-')");
}

}  // namespace recs::cli
