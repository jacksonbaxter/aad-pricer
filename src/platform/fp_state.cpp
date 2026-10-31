#include "platform/fp_state.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <string>

namespace aadp::platform {

FpState decode_fpcr(std::uint64_t raw) {
  // Arm ARM FPCR: FZ is bit 24 and flushes both inputs and outputs. FIZ, bit 0, comes with
  // FEAT_AFP and flushes inputs only.
  const bool fz = ((raw >> 24) & 1U) != 0;
  const bool fiz = (raw & 1U) != 0;
  return {.register_name = "FPCR", .raw = raw, .ftz = fz, .daz = fz || fiz};
}

FpState decode_mxcsr(std::uint64_t raw) {
  // Intel SDM vol. 1, MXCSR: DAZ is bit 6 and FZ is bit 15.
  return {.register_name = "MXCSR",
          .raw = raw,
          .ftz = ((raw >> 15) & 1U) != 0,
          .daz = ((raw >> 6) & 1U) != 0};
}

FpState read_fp_state() {
#if defined(__aarch64__) || defined(__arm64__)
  std::uint64_t raw = 0;
  __asm__ volatile("mrs %0, fpcr" : "=r"(raw));
  return decode_fpcr(raw);
#elifdef __x86_64__
  std::uint32_t raw = 0;
  __asm__ volatile("stmxcsr %0" : "=m"(raw));
  return decode_mxcsr(raw);
#else
#error "Only arm64 and x86-64 are supported"
#endif
}

std::expected<FpState, std::string> check_fp_state() {
  const FpState state = read_fp_state();
  if (state.ftz || state.daz) {
    std::array<char, 128> text{};
    const char* flags = state.ftz ? "FTZ" : "DAZ";
    if (state.ftz && state.daz) flags = "FTZ and DAZ";
    std::snprintf(text.data(), text.size(), "%.*s=0x%llx has %s set",
                  static_cast<int>(state.register_name.size()), state.register_name.data(),
                  static_cast<unsigned long long>(state.raw), flags);
    return std::unexpected(std::string(text.data()));
  }
  return state;
}

void write_fp_register(std::uint64_t raw) {
#if defined(__aarch64__) || defined(__arm64__)
  __asm__ volatile("msr fpcr, %0" : : "r"(raw));
#elifdef __x86_64__
  auto value = static_cast<std::uint32_t>(raw);
  __asm__ volatile("ldmxcsr %0" : : "m"(value));
#endif
}

}  // namespace aadp::platform
