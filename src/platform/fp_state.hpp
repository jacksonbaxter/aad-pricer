// Floating-point control state. Flush-to-zero silently changes results, and a linked object
// built with -ffast-math can set it behind our back, so it is read and checked on every run.
#ifndef AADP_PLATFORM_FP_STATE_HPP_
#define AADP_PLATFORM_FP_STATE_HPP_

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace aadp::platform {

struct FpState {
  std::string_view register_name;
  std::uint64_t raw = 0;
  bool ftz = false;
  bool daz = false;
};

// Pure decoders, so both layouts can be tested on either architecture.
FpState decode_fpcr(std::uint64_t raw);
FpState decode_mxcsr(std::uint64_t raw);

FpState read_fp_state();

// Fails with a message if FTZ or DAZ is set.
std::expected<FpState, std::string> check_fp_state();

// For tests that prove the check can fail.
void write_fp_register(std::uint64_t raw);

}  // namespace aadp::platform

#endif  // AADP_PLATFORM_FP_STATE_HPP_
