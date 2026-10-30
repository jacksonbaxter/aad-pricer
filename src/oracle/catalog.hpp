// Contracts the oracles price, as the text both reference and plan rows carry. A run row must
// quote the same parameters as its reference, or the runner refuses it, so one table feeds both.
#ifndef AADP_ORACLE_CATALOG_HPP_
#define AADP_ORACLE_CATALOG_HPP_

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

}  // namespace aadp::catalog

#endif  // AADP_ORACLE_CATALOG_HPP_
