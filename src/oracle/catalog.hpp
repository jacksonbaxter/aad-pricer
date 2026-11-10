// Contracts the oracles price, as the text both reference and plan rows carry. A run row must
// quote the same parameters as its reference, or the runner refuses it, so one table feeds both.
#ifndef AADP_ORACLE_CATALOG_HPP_
#define AADP_ORACLE_CATALOG_HPP_

#include <array>

namespace aadp::catalog {

// Text form: "<kind>:<key>=<value>;...". Maturities 1/12 and 11/12 are spelled to round-trip.
inline constexpr const char* kVanillaO1 =
    "european:S0=105;K=100;H=0;sigma=0.36;r=0.1;q=0.1;T=0.5;m=1;type=call;spot=0";
inline constexpr const char* kVanillaO2 =
    "european:S0=72;K=75;H=0;sigma=0.19;r=0.09;q=0;T=1;m=1;type=call;spot=0";
inline constexpr const char* kVanillaO3 =
    "european:S0=430;K=405;H=0;sigma=0.2;r=0.07;q=0.05;T=0.083333333333333329;m=1;type=put;spot=0";
inline constexpr const char* kGeometricO4 =
    "geometric:S0=100;K=100;H=0;sigma=0.2;r=0.06;q=0.03;T=1;m=10;type=call;spot=0";
inline constexpr const char* kArithmeticN12K90 =
    "arithmetic:S0=100;K=90;H=0;sigma=0.17801;r=0.0367;q=0;T=1;m=12;type=call;spot=1";
inline constexpr const char* kArithmeticN12K100 =
    "arithmetic:S0=100;K=100;H=0;sigma=0.17801;r=0.0367;q=0;T=1;m=12;type=call;spot=1";
inline constexpr const char* kArithmeticN12K110 =
    "arithmetic:S0=100;K=110;H=0;sigma=0.17801;r=0.0367;q=0;T=1;m=12;type=call;spot=1";
inline constexpr const char* kArithmeticN50K90 =
    "arithmetic:S0=100;K=90;H=0;sigma=0.17801;r=0.0367;q=0;T=1;m=50;type=call;spot=1";
inline constexpr const char* kArithmeticO9 =
    "arithmetic:S0=100;K=100;H=0;sigma=0.17801;r=0.0367;q=0;T=1;m=50;type=call;spot=1";
inline constexpr const char* kArithmeticN50K110 =
    "arithmetic:S0=100;K=110;H=0;sigma=0.17801;r=0.0367;q=0;T=1;m=50;type=call;spot=1";
inline constexpr const char* kArithmeticO11 =
    "arithmetic:S0=90;K=87;H=0;sigma=0.13;r=0.025;q=0.06;T=0.91666666666666663;m=1;type=put;spot=1";
inline constexpr const char* kOneStepH85 =
    "one_step:S0=100;K=100;H=85;sigma=0.3;r=0.1;q=0;T=0.2;m=50;type=call;spot=0";
inline constexpr const char* kOneStepH90 =
    "one_step:S0=100;K=100;H=90;sigma=0.3;r=0.1;q=0;T=0.2;m=50;type=call;spot=0";
inline constexpr const char* kOneStepH95 =
    "one_step:S0=100;K=100;H=95;sigma=0.3;r=0.1;q=0;T=0.2;m=50;type=call;spot=0";
inline constexpr const char* kOneStepH97 =
    "one_step:S0=100;K=100;H=97;sigma=0.3;r=0.1;q=0;T=0.2;m=50;type=call;spot=0";
inline constexpr const char* kOneStepH99 =
    "one_step:S0=100;K=100;H=99;sigma=0.3;r=0.1;q=0;T=0.2;m=50;type=call;spot=0";
inline constexpr const char* kOneStepO8 =
    "one_step:S0=100;K=100;H=95;sigma=0.6;r=0.1;q=0;T=0.2;m=4;type=call;spot=0";
inline constexpr const char* kBridgeO7 =
    "bridge:S0=100;K=100;H=95;sigma=0.3;r=0.1;q=0;T=0.2;m=50;type=call;spot=0";
inline constexpr const char* kBridgeH85 =
    "bridge:S0=100;K=100;H=85;sigma=0.3;r=0.1;q=0;T=0.2;m=50;type=call;spot=0";
// Strike below the barrier, and a dividend yield: the branch the two BGK contracts never reach.
inline constexpr const char* kBridgeK90 =
    "bridge:S0=100;K=90;H=95;sigma=0.25;r=0.08;q=0.04;T=0.5;m=25;type=call;spot=0";

// Stage 8's knock-in rows (NOTES.md, "Stage 8"): a down-and-in call by parity, vanilla minus the
// knock-out with the same monitoring, on DB095's and CB095's terms. A row names the alias; the
// text it resolves to is the knock-out leg's contract, which its reference must quote, and the
// flag is what makes the priced contract the knock-in.
struct KnockInEntry {
  const char* instrument;
  const char* contract;
  bool knock_in;
};
inline constexpr std::array<KnockInEntry, 2> kKnockIns = {{
    {.instrument = "knock-in-parity:discrete", .contract = kOneStepH95, .knock_in = true},
    {.instrument = "knock-in-parity:continuous", .contract = kBridgeO7, .knock_in = true},
}};

// Stage 7a's three contracts on the surface fitted to the captured chain, in the order of
// svi::SurfaceContracts, and the names of the 15 parameters a surface row's Greek d_<p>_<dNN>
// differentiates by: p in parameter order, dNN the slice at 5, 12 and 33 days.
inline constexpr std::array<const char*, 3> kSurfaceInstruments = {
    "svi-surface:arithmetic", "svi-surface:discrete-barrier", "svi-surface:continuous-barrier"};
inline constexpr std::array<const char*, 5> kSurfaceParameters = {"a", "b", "rho", "m", "sigma"};
inline constexpr std::array<const char*, 3> kSurfaceSlices = {"d05", "d12", "d33"};

}  // namespace aadp::catalog

#endif  // AADP_ORACLE_CATALOG_HPP_
