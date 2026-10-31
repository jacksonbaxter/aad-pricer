// Usage: aadp_quadrature --write <file> | --check <file>
//
// Exits non-zero if the methods disagree, or under --check if the file differs in any byte.
#include <cstdio>
#include <exception>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "quadrature.hpp"

// NOLINTNEXTLINE(misc-const-correctness): the standard fixes the signature of main.
int main(int argc, char** argv) try {
  if (argc != 3) {
    std::fprintf(stderr, "usage: %s --write <file> | --check <file>\n", argv[0]);
    return 2;
  }
  const std::string_view mode = argv[1];
  std::vector<std::string> failures;
  const std::string csv =
      aadp::quadrature::RenderCsv(aadp::quadrature::GenerateReferences(failures));
  for (const std::string& failure : failures) {
    std::fprintf(stderr, "DISAGREEMENT %s\n", failure.c_str());
  }
  if (!failures.empty()) return 1;

  if (mode == "--write") {
    std::ofstream file(argv[2], std::ios::binary);
    file << csv;
    return file.good() ? 0 : 1;
  }
  if (mode == "--check") {
    const std::ifstream file(argv[2], std::ios::binary);
    if (!file) {
      std::fprintf(stderr, "cannot open %s\n", argv[2]);
      return 1;
    }
    std::ostringstream committed;
    committed << file.rdbuf();
    if (committed.str() != csv) {
      std::fprintf(stderr, "%s differs from the regenerated values\n", argv[2]);
      return 1;
    }
    return 0;
  }
  std::fprintf(stderr, "unknown mode %s\n", argv[1]);
  return 2;
} catch (const std::exception& error) {
  std::fprintf(stderr, "aadp_quadrature: %s\n", error.what());
  return 2;
}
