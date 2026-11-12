// Run 2's code at the development seed (docs/NOTES.md, "Stage 8", and the lead's answer recorded in
// "Stage 8a results"): every row of the committed plan through the production estimator at seed
// 20261128 and N / 1024, as hex floats, bit for bit against references/run2_fingerprint_<host>.txt
// on the two reportable hosts. Each host keeps its own file; on any other host, CI's included,
// nothing is compared bit for bit and the lines that differ from the Mac's are reported. On every
// host the bits must not depend on the thread count. Without the captures the 45 surface rows skip,
// visibly.
#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iostream>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "oracle/decode_util.hpp"
#include "oracle/plan.hpp"
#include "oracle/references.hpp"
#include "platform/host_label.hpp"
#include "production.hpp"
#include "svi/chain_surface.hpp"

namespace aadp::oracle_runner {
namespace {

namespace fs = std::filesystem;

std::string ReadFile(const fs::path& path) {
  const std::ifstream file(path, std::ios::binary);
  std::ostringstream text;
  text << file.rdbuf();
  return text.str();
}

std::vector<std::string> LinesOf(const std::string& text, const std::set<std::string>& skip) {
  std::vector<std::string> out;
  for (const std::string_view line : decode::Lines(text)) {
    if (line.empty() || line.starts_with('#')) continue;
    if (skip.contains(std::string(line.substr(0, line.find(' '))))) continue;
    out.emplace_back(line);
  }
  return out;
}

TEST(Run2Fingerprint, CommittedBitsOnThisHost) {
  const auto plan = DecodePlan(ReadFile("references/comparison_plan.csv"));
  ASSERT_TRUE(plan.has_value());
  const auto references = DecodeReferences(ReadFile("references/oracle_values.csv"));
  ASSERT_TRUE(references.has_value());

  std::optional<svi::ChainSurface> surface;
  std::set<std::string> surface_ids;
  for (const PlanRow& row : plan->rows) {
    const auto instrument = ResolveInstrument(row.instrument);
    if (instrument && instrument->kind == InstrumentKind::kSurface) surface_ids.insert(row.id);
  }
  if (fs::exists("data/DailyMarketReportOpt.csv") && fs::exists("data/DailyMarketReportFut.csv")) {
    auto loaded = LoadChainSurface("data");
    ASSERT_TRUE(loaded.has_value()) << loaded.error();
    surface = *std::move(loaded);
  } else {
    std::cout << "SKIPPED: data/ has no capture, so the " << surface_ids.size()
              << " surface rows did not run. Fetch it with scripts/fetch_data.sh.\n";
  }
  const svi::ChainSurface* s = surface ? &*surface : nullptr;
  const std::set<std::string> skip = s != nullptr ? std::set<std::string>{} : surface_ids;

  const std::string eight = Fingerprint(*plan, *references, s, 8);
  const std::string one = Fingerprint(*plan, *references, s, 1);
  EXPECT_EQ(one, eight) << "the bits depend on the thread count";
  const std::vector<std::string> produced = LinesOf(eight, {});
  EXPECT_EQ(produced.size(), plan->rows.size() - skip.size());

  const std::string host(platform::detect_host_label());
  const fs::path mac_file =
      "references/run2_fingerprint_" + std::string(platform::kHostMacM4Max) + ".txt";
  if (host == platform::kHostOther) {
    std::size_t differ = 0;
    const std::vector<std::string> mac = LinesOf(ReadFile(mac_file), skip);
    for (std::size_t i = 0; i < produced.size() && i < mac.size(); ++i) {
      if (produced[i] != mac[i]) ++differ;
    }
    std::cout << "host other: no file of its own, nothing compared bit for bit. " << differ
              << " of " << produced.size() << " lines differ from the Mac's.\n";
    return;
  }
  const fs::path file = "references/run2_fingerprint_" + host + ".txt";
  ASSERT_TRUE(fs::exists(file)) << file.string() << " is missing";
  const std::vector<std::string> committed = LinesOf(ReadFile(file), skip);
  ASSERT_EQ(produced.size(), committed.size());
  for (std::size_t i = 0; i < produced.size(); ++i) {
    EXPECT_EQ(produced[i], committed[i]);
  }
}

}  // namespace
}  // namespace aadp::oracle_runner
