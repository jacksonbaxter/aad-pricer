#include "data/calibration_set.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace aadp {
namespace {

using taifex::OptionRight;
using taifex::OptionRow;
using taifex::Session;

// An out-of-the-money put at 900 against a forward of 1000, passing every filter.
OptionRow passing_row() {
  OptionRow row;
  row.trade_date = 20240103;
  row.contract = "TXO";
  row.expiry = "202401";
  row.strike = 900.0;
  row.right = OptionRight::kPut;
  row.last_trade_price = 5.0;
  row.settlement_price = 5.0;
  row.volume = 3;
  row.session = Session::kRegular;
  return row;
}

const std::vector<ExpiryForward> kForwards = {{"202401", 1000.0}};

std::size_t selected(const OptionRow& row) {
  const std::vector<OptionRow> rows = {row};
  const auto slices = select_calibration_set(rows, kForwards);
  EXPECT_TRUE(slices.has_value());
  return slices->at(0).rows.size();
}

TEST(CalibrationSet, KeepsARowThatPassesEveryFilter) { EXPECT_EQ(selected(passing_row()), 1u); }

TEST(CalibrationSet, DropsOtherContractsAndExpiries) {
  OptionRow row = passing_row();
  row.contract = "TEO";
  EXPECT_EQ(selected(row), 0u);
  row = passing_row();
  row.expiry = "202402";
  EXPECT_EQ(selected(row), 0u);
}

TEST(CalibrationSet, DropsAfterHoursEvenWhenAllElsePasses) {
  OptionRow row = passing_row();
  row.session = Session::kAfterHours;
  EXPECT_EQ(selected(row), 0u);
}

TEST(CalibrationSet, KeepsOnlyTheOutOfTheMoneySide) {
  OptionRow row = passing_row();
  row.right = OptionRight::kCall;  // A call below the forward.
  EXPECT_EQ(selected(row), 0u);
  row.strike = 1100.0;  // A call above it.
  EXPECT_EQ(selected(row), 1u);
  row.right = OptionRight::kPut;  // A put above it.
  EXPECT_EQ(selected(row), 0u);
}

TEST(CalibrationSet, AStrikeOnTheForwardIsOnNeitherSide) {
  for (const OptionRight right : {OptionRight::kCall, OptionRight::kPut}) {
    OptionRow row = passing_row();
    row.strike = 1000.0;
    row.right = right;
    EXPECT_EQ(selected(row), 0u);
  }
}

TEST(CalibrationSet, NeedsPositiveVolume) {
  OptionRow row = passing_row();
  row.volume = 0;
  EXPECT_EQ(selected(row), 0u);
  row.volume = std::nullopt;
  EXPECT_EQ(selected(row), 0u);
}

TEST(CalibrationSet, NeedsSettlementEqualToLastTrade) {
  OptionRow row = passing_row();
  row.last_trade_price = 5.1;
  EXPECT_EQ(selected(row), 0u);
  row.last_trade_price = std::nullopt;
  EXPECT_EQ(selected(row), 0u);
  row = passing_row();
  row.settlement_price = std::nullopt;
  EXPECT_EQ(selected(row), 0u);
}

TEST(CalibrationSet, ThresholdIsInclusiveAtOnePoint) {
  OptionRow row = passing_row();
  row.settlement_price = 1.0;
  row.last_trade_price = 1.0;
  EXPECT_EQ(selected(row), 1u);
  row.settlement_price = 0.9;
  row.last_trade_price = 0.9;
  EXPECT_EQ(selected(row), 0u);
}

TEST(CalibrationSet, BidAndAskDoNotMatter) {
  OptionRow row = passing_row();
  row.last_best_bid = std::nullopt;
  row.last_best_ask = std::nullopt;
  EXPECT_EQ(selected(row), 1u);
  row.last_best_bid = 1000.0;
  row.last_best_ask = 1.0;
  EXPECT_EQ(selected(row), 1u);
}

TEST(CalibrationSet, RowsThatCannotCompareAreDropped) {
  OptionRow row = passing_row();
  row.strike = std::numeric_limits<double>::quiet_NaN();
  EXPECT_EQ(selected(row), 0u);
}

TEST(CalibrationSet, EachSliceUsesItsOwnForward) {
  OptionRow near = passing_row();
  OptionRow far = passing_row();
  far.expiry = "202402";
  const std::vector<OptionRow> rows = {near, far};
  // 900 is below the first forward, so the put survives there, and above the second.
  const std::vector<ExpiryForward> forwards = {{"202401", 1000.0}, {"202402", 800.0}};
  const auto slices = select_calibration_set(rows, forwards);
  ASSERT_TRUE(slices.has_value());
  ASSERT_EQ(slices->size(), 2u);
  EXPECT_EQ((*slices)[0].expiry, "202401");
  EXPECT_EQ((*slices)[0].forward, 1000.0);
  EXPECT_EQ((*slices)[0].rows.size(), 1u);
  EXPECT_EQ((*slices)[1].expiry, "202402");
  EXPECT_TRUE((*slices)[1].rows.empty());
}

TEST(CalibrationSet, ExpiryWithoutAForwardIsNotSelected) {
  const std::vector<OptionRow> rows = {passing_row()};
  const auto slices = select_calibration_set(rows, {});
  ASSERT_TRUE(slices.has_value());
  EXPECT_TRUE(slices->empty());
}

TEST(CalibrationSet, RejectsBadForwards) {
  for (const double forward : {0.0, -1.0, std::numeric_limits<double>::quiet_NaN(),
                               std::numeric_limits<double>::infinity()}) {
    const std::vector<ExpiryForward> forwards = {{"202401", forward}};
    const auto slices = select_calibration_set({}, forwards);
    ASSERT_FALSE(slices.has_value());
    EXPECT_EQ(slices.error(), CalibrationError::kBadForward);
  }
}

TEST(CalibrationSet, RejectsADuplicateExpiry) {
  const std::vector<ExpiryForward> forwards = {{"202401", 1000.0}, {"202401", 1001.0}};
  const auto slices = select_calibration_set({}, forwards);
  ASSERT_FALSE(slices.has_value());
  EXPECT_EQ(slices.error(), CalibrationError::kDuplicateExpiry);
}

}  // namespace
}  // namespace aadp
