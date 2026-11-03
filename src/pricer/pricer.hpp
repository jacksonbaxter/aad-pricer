// Entry point of the production scalar Monte Carlo pricer, namespace aadp::pricer.
//
//   ContractError Validate(contract, market)          contract.hpp: kNone or why not
//   Price(contract, market, {seed, paths, batches, threads})
//       -> Estimate {mean, standard_error, batch_means}, or the reason it refused
//
// The estimate is the mean of B batch means and its standard error, the statistics the oracle
// runner and src/oracle/tolerance.hpp read. The kernels (kernels.hpp) and the runner (runner.hpp)
// are templates, so a later stage instantiates them on its own Real and per-worker state.
#ifndef AADP_PRICER_PRICER_HPP_
#define AADP_PRICER_PRICER_HPP_

#include <expected>
#include <vector>

#include "pricer/contract.hpp"
#include "pricer/runner.hpp"

namespace aadp::pricer {

struct Estimate {
  double mean = 0.0;
  double standard_error = 0.0;
  std::vector<double> batch_means;
};

struct PriceError {
  ContractError contract = ContractError::kNone;
  RunError run = RunError::kNone;
};

std::expected<Estimate, PriceError> Price(const Contract& contract, const Market<double>& market,
                                          const RunConfig& config);

}  // namespace aadp::pricer

#endif  // AADP_PRICER_PRICER_HPP_
