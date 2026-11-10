// Writes the seed corpora for the librecs loader fuzzers (tests/fuzz/core/corpus/<harness>/).
//
//   fuzz_core_seedgen <tiny fixture dir> <corpus dir> <csv fixture dir>
//
// Seeds are real files produced by the real writers, from the synthetic tiny fixture (never
// MovieLens data) and a 3 x 4 hand-made dataset, laid out the way each harness expects its input.
// Small model configs keep every seed a few KiB at most; the output is deterministic, so a re-run
// with unchanged formats leaves the committed corpus unchanged. Re-run after a format change.
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "../artifact_template.hpp"
#include "../fuzz_util.hpp"
#include "../model_load.hpp"
#include "recs/dataset.hpp"
#include "recs/model.hpp"

namespace fs = std::filesystem;
using recs::fuzz::write_file;

namespace {

std::string read_file(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  if (!in) throw std::runtime_error("cannot read " + p.string());
  return {std::istreambuf_iterator<char>(in), {}};
}

// Drops the trailing SHA-256 (the harnesses re-seal their input).
std::string without_digest(std::string bytes, const fs::path& from) {
  if (bytes.size() < 32) throw std::runtime_error(from.string() + " is shorter than its SHA-256 trailer");
  bytes.resize(bytes.size() - 32);
  return bytes;
}

// The manifest with a fixed created_at, so re-running the generator reproduces the committed seed.
std::string stable_manifest(const fs::path& file) {
  auto j = nlohmann::json::parse(read_file(file));
  j["created_at"] = "2026-01-01T00:00:00Z";
  return j.dump(2) + "\n";
}

void seed(const fs::path& corpus, const std::string& harness, const std::string& name, const std::string& bytes) {
  const fs::path dir = corpus / harness;
  fs::create_directories(dir);
  write_file(dir / name, bytes);
}

// The harness input for a saved model directory (inverse of write_model_input).
std::string model_input(const std::string& model, const fs::path& dir) {
  const auto f = recs::fuzz::model_files(model);
  std::string binary = read_file(dir / f.binary);
  if (f.sealed) binary = without_digest(std::move(binary), dir / f.binary);
  if (!f.has_config) return binary;
  return recs::fuzz::join_config(read_file(dir / "config.json"), binary);
}

recs::Config small_config(const std::string& model) {
  if (model == "pop") return {{"m", 5}};
  if (model == "knn") {
    return {{"neighbors", 5}, {"shrinkage", 10}, {"min_support", 1}, {"normalize", true}, {"positive_only", false}};
  }
  if (model == "content") return {{"min_tag_count", 1}, {"genre_weight", 1.0}};
  if (model == "mf") return {{"k", 3}, {"max_epochs", 3}};
  if (model == "als") return {{"k", 3}, {"iters", 2}};
  return recs::Config::object();
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 4) {
    std::cerr << "usage: fuzz_core_seedgen <tiny fixture dir> <corpus dir> <csv fixture dir>\n";
    return 2;
  }
  const fs::path tiny = argv[1], corpus = argv[2], csv_dir = argv[3];
  const fs::path work = recs::fuzz::fresh_dir("seedgen");

  const recs::Dataset tiny_ds = recs::load_movielens(tiny, "tiny");
  const recs::Dataset small_ds = recs::fuzz::template_dataset();

  // Dataset cache: body without the trailing SHA-256 (the harness seals it).
  for (const auto& [name, ds] : {std::pair{"tiny", &tiny_ds}, std::pair{"small", &small_ds}}) {
    recs::save_cache(*ds, work / "ds.bin");
    seed(corpus, "dataset_cache", name, without_digest(read_file(work / "ds.bin"), work / "ds.bin"));
  }

  // CSV reader: first byte 0 (any field count), then the text.
  for (const auto& e : fs::directory_iterator(csv_dir)) {
    if (e.path().extension() != ".csv") continue;
    seed(corpus, "csv_reader", e.path().stem().string(), std::string(1, '\0') + read_file(e.path()));
  }
  seed(corpus, "csv_reader", "tiny_movies", std::string(1, '\x02') + read_file(tiny / "movies.csv"));
  seed(corpus, "csv_reader", "tiny_tags", std::string(1, '\x03') + read_file(tiny / "tags.csv"));

  // Artifact: manifest and catalog of the template artifact.
  const fs::path art = recs::fuzz::write_template_artifact(work / "artifact");
  seed(corpus, "artifact_manifest", "template", stable_manifest(art / "manifest.json"));
  seed(corpus, "artifact_catalog", "template", read_file(art / "catalog" / "catalog.bin"));
  {
    auto pop = recs::make_popularity();
    pop->fit(tiny_ds, small_config("pop"), {});
    const fs::path art2 = recs::write_artifact(work / "artifact-tiny", "v1", recs::make_catalog(tiny_ds), {pop.get()},
                                               "pop", "abc1234", "tiny");
    seed(corpus, "artifact_catalog", "tiny", read_file(art2 / "catalog" / "catalog.bin"));
    seed(corpus, "artifact_manifest", "tiny", stable_manifest(art2 / "manifest.json"));
  }

  // Model files: each model fitted on both datasets with a small config.
  for (const std::string model : {"pop", "pop_count", "knn", "content", "mf", "als"}) {
    for (const auto& [name, ds] : {std::pair{"tiny", &tiny_ds}, std::pair{"small", &small_ds}}) {
      auto m = recs::make_model(model);
      try {
        m->fit(*ds, small_config(model), {});
      } catch (const std::exception& e) {
        std::cerr << model << " on " << name << ": " << e.what() << " (skipped)\n";
        continue;
      }
      const fs::path dir = recs::fuzz::fresh_dir("seedgen-" + model + "-" + name);
      m->save(dir);
      seed(corpus, "model_load_" + model, name, model_input(model, dir));
    }
  }
  std::cout << "seeds written to " << corpus.string() << "\n";
  return 0;
}
