// STUB (Wave 0). Owner: workstream C. Replace with the real implementation.
#include "recs/eval.hpp"
#include "util/todo.hpp"
namespace recs {
EvalResult evaluate(const Model&, const Dataset&, std::span<const std::vector<Rating>>, std::span<const UserIdx>, const EvalOptions&) { RECS_TODO("evaluate"); }
nlohmann::json make_report(const ReportHeader&, const EvalResult&, const EvalOptions&) { RECS_TODO("make_report"); }
fs::path write_report(const nlohmann::json&, const fs::path&) { RECS_TODO("write_report"); }
std::string make_run_id(std::string_view, std::string_view) { RECS_TODO("make_run_id"); }
std::optional<std::uint64_t> peak_rss_bytes() { RECS_TODO("peak_rss_bytes"); }
std::vector<RatingBaselineResult> evaluate_rating_baselines(const Dataset&, std::span<const std::vector<Rating>>, std::span<const UserIdx>) { RECS_TODO("evaluate_rating_baselines"); }
fs::path write_per_user(const ReportHeader&, const EvalResult&, const EvalOptions&, const fs::path&) { RECS_TODO("write_per_user"); }
nlohmann::json compare_runs(const nlohmann::json&, const nlohmann::json&, std::size_t, std::uint64_t) { RECS_TODO("compare_runs"); }
}  // namespace recs
