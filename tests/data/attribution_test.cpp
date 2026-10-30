#include "data/attribution.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <sstream>
#include <string>
#include <string_view>

#include "data/taifex_csv.hpp"

namespace aadp {
namespace {

// Tests run with the repo root as working directory.
std::string read_notice() {
  const auto bytes = taifex::read_binary_file("NOTICE");
  EXPECT_TRUE(bytes.has_value()) << bytes.error();
  return bytes.value_or("");
}

std::size_t count_lines(std::string_view text) {
  return static_cast<std::size_t>(std::count(text.begin(), text.end(), '\n'));
}

// An empty helper would pass the byte comparison, so pin what the text has to say.
TEST(Attribution, NamesBothDatasetsTheLicenceAndThePinnedCopy) {
  const std::string_view text = attribution_text();
  EXPECT_NE(text.find("data.gov.tw dataset 11320, trade date 2024-11-15"), text.npos);
  EXPECT_NE(text.find("data.gov.tw dataset 11319, trade date 2024-11-15"), text.npos);
  EXPECT_NE(text.find("資料集 11320"), text.npos);
  EXPECT_NE(text.find("資料集 11319"), text.npos);
  EXPECT_NE(text.find("https://data.gov.tw/license"), text.npos);
  EXPECT_NE(text.find("Taiwan Futures Exchange (TAIFEX)"), text.npos);
  EXPECT_NE(text.find("Internet Archive capture 20241117234312"), text.npos);
  EXPECT_NE(text.find("sha256 fd9ddc6cafcbdd1b08cd0f80912dcbd9c19de515cdb248163183378715d53f1e"),
            text.npos);
  EXPECT_NE(text.find("not endorsed by"), text.npos);
  // Two seven-line exhibits and the six-line note, with a blank line between each.
  EXPECT_EQ(count_lines(text), 7u + 1u + 6u + 1u + 7u);
}

TEST(Attribution, NoticeStartsWithTheExactBytes) {
  const std::string notice = read_notice();
  const std::string_view text = attribution_text();
  ASSERT_GE(notice.size(), text.size());
  EXPECT_EQ(std::string_view(notice).substr(0, text.size()), text);
}

TEST(Attribution, NoticeHasTheTextOnlyOnce) {
  const std::string notice = read_notice();
  const auto first = notice.find(attribution_text());
  ASSERT_NE(first, notice.npos);
  EXPECT_EQ(notice.find(attribution_text(), first + 1), notice.npos);
}

TEST(Attribution, OneChangedByteIsNoticed) {
  const std::string notice = read_notice();
  std::string changed(attribution_text());
  changed[changed.find("11319")] = '2';
  EXPECT_NE(std::string_view(notice).substr(0, changed.size()), changed);
}

TEST(Attribution, WriterEmitsTheTextUnchanged) {
  std::ostringstream out;
  write_attribution(out);
  EXPECT_EQ(out.str(), attribution_text());
}

}  // namespace
}  // namespace aadp
