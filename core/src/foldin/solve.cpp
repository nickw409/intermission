// Dense SPD solve shared by MF fold-in and ALS. Spine-owned.
#include "recs/foldin.hpp"

#include <stdexcept>

#include <Eigen/Cholesky>
#include <Eigen/Core>

namespace recs {

std::vector<double> solve_spd(std::span<const double> A, std::span<const double> b, std::size_t k,
                              double lambda) {
  if (A.size() != k * k || b.size() != k) throw std::invalid_argument("solve_spd: size mismatch");
  const auto n = static_cast<Eigen::Index>(k);
  Eigen::MatrixXd M = Eigen::Map<const Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>>(A.data(), n, n);
  M.diagonal().array() += lambda;
  Eigen::LLT<Eigen::MatrixXd> llt(M);
  if (llt.info() != Eigen::Success) throw std::runtime_error("solve_spd: matrix is not positive definite");
  const Eigen::VectorXd x = llt.solve(Eigen::Map<const Eigen::VectorXd>(b.data(), n));
  return {x.data(), x.data() + n};
}

}  // namespace recs
