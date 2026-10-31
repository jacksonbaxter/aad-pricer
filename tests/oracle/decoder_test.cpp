// Decoders for the plan and the reference file: what they accept, and that bad input comes back
// as an error value.
#include <gtest/gtest.h>

#include <fstream>
#include <ios>
#include <sstream>
#include <string>
#include <vector>

#include "mc/gbm_kernels.hpp"
#include "oracle/black_scholes.hpp"
#include "oracle/decode_util.hpp"
#include "oracle/plan.hpp"
#include "oracle/references.hpp"

namespace aadp {
namespace {

const std::string kInstrument =
    "european:S0=100;K=100;H=0;sigma=0.2;r=0.05;q=0;T=1;m=1;type=call;spot=0";

std::string Directives(int k = 2) {
  return "#! K=" + std::to_string(k) +
         "\n#! alpha_F=0.01\n#! B=4\n#! critical=4.1\n#! ceiling_factor=1.65\n"
         "#! fd_ceiling_factor=3\n#! fd_b_factor=2\n#! pilot_seed=1\n#! measured_seed=2\n";
}

const std::string kColumns = "id,instrument,greek,reference,N,B,seed,se_ceiling,b,status\n";

std::string RunRow(const std::string& id = "a") {
  return id + "," + kInstrument + ",value,REF,400,4,2,0.1,0,run\n";
}

const std::string kAwaitingRow =
    "z,svi-surface:asian,d_a_slice1,phase3,-,4,-,-,-,awaiting-phase3\n";

std::string GoodPlan() { return Directives() + kColumns + RunRow() + kAwaitingRow; }

TEST(DecodePlan, AcceptsRunAndAwaitingRows) {
  const auto plan = DecodePlan(GoodPlan());
  ASSERT_TRUE(plan.has_value()) << plan.error().message;
  EXPECT_EQ(plan->header.declared_k, 2U);
  EXPECT_EQ(plan->header.batches, 4U);
  ASSERT_EQ(plan->rows.size(), 2U);
  EXPECT_EQ(plan->rows[0].status, RowStatus::kRun);
  EXPECT_EQ(plan->rows[0].paths, 400U);
  EXPECT_EQ(plan->rows[1].status, RowStatus::kAwaitingPhase3);
}

TEST(DecodePlan, RoundTrips) {
  const auto plan = DecodePlan("# a comment\n" + GoodPlan());
  ASSERT_TRUE(plan.has_value());
  const auto again = DecodePlan(EncodePlan(*plan));
  ASSERT_TRUE(again.has_value());
  EXPECT_EQ(*plan, *again);
  EXPECT_EQ(EncodePlan(*plan), EncodePlan(*again));
}

struct BadCase {
  std::string name;
  std::string input;
};

TEST(DecodePlan, RejectsEachBadInputWithAnErrorValue) {
  const std::string d = Directives(1);
  const std::string instrument = kInstrument;
  const std::vector<BadCase> cases = {
      {.name = "empty", .input = ""},
      {.name = "no columns line", .input = Directives()},
      {.name = "bad columns line", .input = d + "id,instrument\n"},
      {.name = "missing directive", .input = "#! K=1\n" + kColumns},
      {.name = "unknown directive", .input = "#! Q=1\n" + d + kColumns},
      {.name = "repeated directive", .input = "#! K=1\n" + d + kColumns},
      {.name = "nine columns",
       .input = d + kColumns + "a," + instrument + ",value,REF,400,4,2,0.1,run\n"},
      {.name = "eleven columns",
       .input = d + kColumns + "a," + instrument + ",value,REF,400,4,2,0.1,0,run,x\n"},
      {.name = "bad id",
       .input = d + kColumns + "a b," + instrument + ",value,REF,400,4,2,0.1,0,run\n"},
      {.name = "bad status",
       .input = d + kColumns + "a," + instrument + ",value,REF,400,4,2,0.1,0,done\n"},
      {.name = "bad greek",
       .input = d + kColumns + "a," + instrument + ",gamma,REF,400,4,2,0.1,0,run\n"},
      {.name = "bad instrument",
       .input = d + kColumns + "a,european:S0=100,value,REF,400,4,2,0.1,0,run\n"},
      {.name = "zero N",
       .input = d + kColumns + "a," + instrument + ",value,REF,0,4,2,0.1,0,run\n"},
      {.name = "N not multiple of B",
       .input = d + kColumns + "a," + instrument + ",value,REF,401,4,2,0.1,0,run\n"},
      {.name = "B differs",
       .input = d + kColumns + "a," + instrument + ",value,REF,400,8,2,0.1,0,run\n"},
      {.name = "B of one",
       .input = d + kColumns + "a," + instrument + ",value,REF,400,1,2,0.1,0,run\n"},
      {.name = "negative N",
       .input = d + kColumns + "a," + instrument + ",value,REF,-400,4,2,0.1,0,run\n"},
      {.name = "zero ceiling",
       .input = d + kColumns + "a," + instrument + ",value,REF,400,4,2,0,0,run\n"},
      {.name = "negative b",
       .input = d + kColumns + "a," + instrument + ",value,REF,400,4,2,0.1,-1,run\n"},
      {.name = "nan ceiling",
       .input = d + kColumns + "a," + instrument + ",value,REF,400,4,2,nan,0,run\n"},
      {.name = "inf b",
       .input = d + kColumns + "a," + instrument + ",value,REF,400,4,2,0.1,inf,run\n"},
      {.name = "hex ceiling",
       .input = d + kColumns + "a," + instrument + ",value,REF,400,4,2,0x1p3,0,run\n"},
      {.name = "FD on value",
       .input = d + kColumns + "a," + instrument + ",value,FD:0.1,400,4,2,0.1,0,run\n"},
      {.name = "FD zero step",
       .input = d + kColumns + "a," + instrument + ",delta,FD:0,400,4,2,0.1,0,run\n"},
      {.name = "FD no step",
       .input = d + kColumns + "a," + instrument + ",delta,FD:,400,4,2,0.1,0,run\n"},
      {.name = "awaiting fixes N", .input = d + kColumns + "z,x,y,z,400,4,-,-,-,awaiting-phase3\n"},
      {.name = "duplicate id", .input = Directives() + kColumns + RunRow() + RunRow()},
      {.name = "carriage return",
       .input = Directives() + kColumns + RunRow().substr(0, RunRow().size() - 1) + "\r\n" +
                kAwaitingRow},
      {.name = "embedded NUL",
       .input = d + kColumns + std::string("a\0b,x,y,z,-,4,-,-,-,awaiting-phase3\n", 36)},
      {.name = "long line", .input = d + kColumns + std::string(5000, 'a') + "\n"},
      // Found by fuzz_plan: this used to pass as a comment, so a K stated here changed nothing.
      {.name = "directive after columns", .input = d + kColumns + "#! K=9\n"},
      {.name = "directive without space", .input = "#!K=1\n" + d + kColumns},
      {.name = "control byte in comment", .input = "# a\tb\n" + d + kColumns},
  };
  for (const BadCase& c : cases) {
    EXPECT_FALSE(DecodePlan(c.input).has_value()) << c.name;
  }
}

// DecodePlan reads K. Comparing it with the row count is the runner's job, because a K that
// differs from the row count has to reach a report and a non-zero exit, not a parse error.
TEST(DecodePlan, KeepsADeclaredKThatDiffersFromTheRows) {
  const auto plan = DecodePlan(Directives(5) + kColumns + RunRow());
  ASSERT_TRUE(plan.has_value());
  EXPECT_EQ(plan->header.declared_k, 5U);
  EXPECT_EQ(plan->rows.size(), 1U);
}

TEST(DecodeInstrument, ReadsEveryField) {
  const auto in = DecodeInstrument(
      "one_step:S0=100;K=90;H=95;sigma=0.3;r=0.1;q=0.02;T=0.2;m=50;type=put;spot=1");
  if (!in) FAIL() << "did not decode";
  EXPECT_EQ(in->contract.product, Product::kDownOutOneStep);
  EXPECT_EQ(in->contract.type, OptionType::kPut);
  EXPECT_EQ(in->contract.strike, 90.0);
  EXPECT_EQ(in->contract.barrier, 95.0);
  EXPECT_EQ(in->contract.steps, 50U);
  EXPECT_TRUE(in->contract.include_spot);
  EXPECT_EQ(in->market.spot, 100.0);
  EXPECT_EQ(in->market.sigma, 0.3);
  EXPECT_EQ(in->market.r, 0.1);
  EXPECT_EQ(in->market.q, 0.02);
  EXPECT_EQ(in->market.maturity, 0.2);
}

TEST(DecodeInstrument, RefusesBadContracts) {
  const std::string tail = ";sigma=0.3;r=0.1;q=0;T=0.2;m=50;type=call;spot=0";
  for (const std::string& bad : std::vector<std::string>{
           "",
           "european",
           "swaption:S0=100;K=100;H=0" + tail,
           "bridge:S0=100;K=100;H=0" + tail,     // Barrier kernel without a barrier.
           "bridge:S0=100;K=100;H=100" + tail,   // Already knocked out.
           "bridge:S0=100;K=100;H=120" + tail,   // Above the spot.
           "european:S0=100;K=100;H=95" + tail,  // A barrier on a vanilla.
           "european:S0=-1;K=100;H=0" + tail,
           "european:S0=100;K=0;H=0" + tail,
           "european:S0=100;K=100;H=0;H=0" + tail,
           "european:S0=100;K=100;H=0;sigma=0;r=0.1;q=0;T=0.2;m=50;type=call;spot=0",
           "european:S0=100;K=100;H=0;sigma=0.3;r=0.1;q=0;T=0;m=50;type=call;spot=0",
           "european:S0=100;K=100;H=0;sigma=0.3;r=0.1;q=0;T=0.2;m=0;type=call;spot=0",
           "european:S0=100;K=100;H=0;sigma=0.3;r=0.1;q=0;T=0.2;m=99999999;type=call;spot=0",
           "european:S0=100;K=100;H=0;sigma=0.3;r=0.1;q=0;T=0.2;m=50;type=call;spot=2",
           "european:S0=100;K=100;H=0;sigma=0.3;r=0.1;q=0;T=0.2;m=50;type=digital;spot=0",
           "european:S0=100;K=100;H=0;sigma=0.3;r=0.1;q=0;T=0.2;m=50;type=call",
           "european:S0=100;K=100;H=0;sigma=0.3;r=0.1;q=0;T=0.2;m=50;type=call;spot=0;z=1",
       }) {
    EXPECT_FALSE(DecodeInstrument(bad).has_value()) << bad;
  }
}

TEST(FiniteDifferenceStep, ParsesOnlyAPositiveStep) {
  EXPECT_EQ(FiniteDifferenceStep("FD:0.001"), 0.001);
  EXPECT_FALSE(FiniteDifferenceStep("O5").has_value());
  EXPECT_FALSE(FiniteDifferenceStep("FD:-1").has_value());
  EXPECT_FALSE(FiniteDifferenceStep("FD:x").has_value());
}

const std::string kRefHeader = "id,parameters,value,b,method,source\n";

TEST(DecodeReferences, AcceptsAndRoundTrips) {
  const auto table = DecodeReferences("# c\n" + kRefHeader + "O1,S0=1;K=2,0.5,1e-9,m,s\n");
  ASSERT_TRUE(table.has_value());
  ASSERT_NE(table->Find("O1"), nullptr);
  EXPECT_EQ(table->Find("O1")->value, 0.5);
  EXPECT_EQ(table->Find("nope"), nullptr);
  const auto again = DecodeReferences(EncodeReferences(*table));
  ASSERT_TRUE(again.has_value());
  EXPECT_EQ(*table, *again);
}

TEST(DecodeReferences, RejectsBadInput) {
  for (const std::string& bad : std::vector<std::string>{
           "",
           kRefHeader,
           "O1,p,1,0,m,s\n",
           kRefHeader + "O1,p,1,0,m\n",
           kRefHeader + "O1,p,1,0,m,s,t\n",
           kRefHeader + ",p,1,0,m,s\n",
           kRefHeader + "O1,,1,0,m,s\n",
           kRefHeader + "O1,p,one,0,m,s\n",
           kRefHeader + "O1,p,nan,0,m,s\n",
           kRefHeader + "O1,p,1,-1,m,s\n",
           kRefHeader + "O1,p,1,inf,m,s\n",
           kRefHeader + "O1,p,1,0,m,s\nO1,p,2,0,m,s\n",
           kRefHeader + "O1,p,1,0,m,s\r\n",
           kRefHeader + std::string(5000, 'x') + "\n",
           "# a\tb,c\n" + kRefHeader + "O1,p,1,0,m,s\n",
       }) {
    EXPECT_FALSE(DecodeReferences(bad).has_value()) << bad;
  }
}

TEST(DecodeReferences, ReadsTheCommittedFile) {
  const std::ifstream file("references/oracle_values.csv", std::ios::binary);
  ASSERT_TRUE(file.good());
  std::ostringstream text;
  text << file.rdbuf();
  const auto table = DecodeReferences(text.str());
  ASSERT_TRUE(table.has_value()) << table.error().line << ": " << table.error().message;
  EXPECT_NE(table->Find("O5"), nullptr);
}

TEST(ParseDouble, Limits) {
  EXPECT_EQ(decode::ParseDouble("1e-9"), 1e-9);
  EXPECT_FALSE(decode::ParseDouble("").has_value());
  EXPECT_FALSE(decode::ParseDouble("1e999").has_value());
  EXPECT_FALSE(decode::ParseDouble("1.5x").has_value());
  EXPECT_FALSE(decode::ParseDouble(std::string(100, '1')).has_value());
  EXPECT_FALSE(decode::ParseUnsigned("99999999999999999999999").has_value());
  EXPECT_FALSE(decode::ParseUnsigned("-1").has_value());
  EXPECT_FALSE(decode::ParseUnsigned("").has_value());
}

}  // namespace
}  // namespace aadp
