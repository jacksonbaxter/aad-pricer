#include "quantlib/quantlib_side.hpp"

#include <array>
#include <boost/version.hpp>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <ql/exercise.hpp>
#include <ql/experimental/volatility/svismilesection.hpp>
#include <ql/handle.hpp>
#include <ql/instrument.hpp>
#include <ql/instruments/asianoption.hpp>
#include <ql/instruments/averagetype.hpp>
#include <ql/instruments/barrieroption.hpp>
#include <ql/instruments/barriertype.hpp>
#include <ql/instruments/payoffs.hpp>
#include <ql/instruments/vanillaoption.hpp>
#include <ql/math/matrix.hpp>
#include <ql/math/randomnumbers/rngtraits.hpp>
#include <ql/math/statistics/incrementalstatistics.hpp>
#include <ql/math/statistics/statistics.hpp>
#include <ql/option.hpp>
#include <ql/pricingengine.hpp>
#include <ql/pricingengines/asian/mc_discr_arith_av_price.hpp>
#include <ql/pricingengines/asian/mc_discr_geom_av_price.hpp>
#include <ql/pricingengines/barrier/mcbarrierengine.hpp>
#include <ql/pricingengines/vanilla/mceuropeanengine.hpp>
#include <ql/processes/blackscholesprocess.hpp>
#include <ql/qldefines.hpp>  // IWYU pragma: keep (ql/config.hpp, read by VersionRecord)
#include <ql/quote.hpp>
#include <ql/quotes/simplequote.hpp>
#include <ql/settings.hpp>
#include <ql/shared_ptr.hpp>
#include <ql/termstructures/volatility/equityfx/blackconstantvol.hpp>
#include <ql/termstructures/volatility/equityfx/blackvoltermstructure.hpp>
#include <ql/termstructures/volatility/equityfx/fixedlocalvolsurface.hpp>
#include <ql/termstructures/volatility/equityfx/localvoltermstructure.hpp>
#include <ql/termstructures/volatility/equityfx/noexceptlocalvolsurface.hpp>
#include <ql/termstructures/volatility/equityfx/piecewiseblackvariancesurface.hpp>
#include <ql/termstructures/volatility/smilesection.hpp>
#include <ql/termstructures/yield/flatforward.hpp>
#include <ql/termstructures/yieldtermstructure.hpp>
#include <ql/time/calendars/nullcalendar.hpp>
#include <ql/time/date.hpp>
#include <ql/time/daycounter.hpp>
#include <ql/time/daycounters/actual360.hpp>
#include <ql/time/daycounters/actual365fixed.hpp>
#include <ql/types.hpp>
#include <ql/version.hpp>
#include <string>
#include <utility>
#include <vector>

#include "pricer/contract.hpp"
#include "quantlib/compare.hpp"
#include "quantlib/local_vol_rows.hpp"
#include "svi/raw_svi.hpp"

namespace aadp::qlcompare::ql {

namespace qlib = QuantLib;

namespace {

enum class EngineKind : std::uint8_t { kArithmetic, kGeometric, kBarrier, kEuropean };

// Everything a new engine needs but the seed. tuned picks IncrementalStatistics over QuantLib's
// default Statistics.
struct EngineSpec {
  EngineKind kind = EngineKind::kArithmetic;
  qlib::ext::shared_ptr<qlib::GeneralizedBlackScholesProcess> process;
  qlib::Size samples = 0;
  bool tuned = false;
  bool control_variate = false;  // kArithmetic only
  bool biased = false;           // kBarrier only
};

}  // namespace

struct Pricer::Impl {
  qlib::ext::shared_ptr<qlib::SimpleQuote> spot;
  qlib::ext::shared_ptr<qlib::Instrument> instrument;
  EngineSpec engine;
};

namespace {

// The fixed evaluation date. Any date would do: every time below is a day count on it.
qlib::Date Today() { return {5, qlib::January, 2026}; }

// Year fraction = days / base, so 60-day spacing on base 3000 gives t_k = k / 50 exactly
// (EVIDENCE E17). A bench-only subclass through QuantLib's protected constructor.
class FixedBaseDayCounter : public qlib::DayCounter {
 public:
  explicit FixedBaseDayCounter(double base)
      : qlib::DayCounter(qlib::ext::make_shared<Days>(base)) {}

 private:
  class Days final : public qlib::DayCounter::Impl {
   public:
    explicit Days(double base) : base_(base) {}
    [[nodiscard]] std::string name() const override { return "days / 3000 (bench)"; }
    [[nodiscard]] qlib::Time yearFraction(const qlib::Date& d1, const qlib::Date& d2,
                                          const qlib::Date& /*start*/,
                                          const qlib::Date& /*end*/) const override {
      return static_cast<double>(d2 - d1) / base_;
    }

   private:
    double base_;
  };
};

void SetEvaluationDate() {
  if (qlib::Settings::instance().evaluationDate() != Today()) {
    qlib::Settings::instance().evaluationDate() = Today();
  }
}

struct Curves {
  qlib::ext::shared_ptr<qlib::SimpleQuote> spot;
  qlib::ext::shared_ptr<qlib::YieldTermStructure> risk_free;
  qlib::ext::shared_ptr<qlib::YieldTermStructure> dividend;
  qlib::ext::shared_ptr<qlib::BlackVolTermStructure> vol;
};

Curves FlatCurves(double spot, double r, double q, double sigma, const qlib::DayCounter& dc,
                  bool tuned) {
  SetEvaluationDate();
  Curves c{.spot = qlib::ext::make_shared<qlib::SimpleQuote>(spot),
           .risk_free = qlib::ext::make_shared<qlib::FlatForward>(Today(), r, dc),
           .dividend = qlib::ext::make_shared<qlib::FlatForward>(Today(), q, dc),
           .vol = qlib::ext::make_shared<qlib::BlackConstantVol>(Today(), qlib::NullCalendar(),
                                                                 sigma, dc)};
  if (tuned) {
    c.risk_free->enableExtrapolation();
    c.dividend->enableExtrapolation();
    c.vol->enableExtrapolation();
  }
  return c;
}

qlib::ext::shared_ptr<qlib::GeneralizedBlackScholesProcess> ExactProcess(const Curves& c) {
  return qlib::ext::make_shared<qlib::BlackScholesMertonProcess>(
      qlib::Handle<qlib::Quote>(c.spot), qlib::Handle<qlib::YieldTermStructure>(c.dividend),
      qlib::Handle<qlib::YieldTermStructure>(c.risk_free),
      qlib::Handle<qlib::BlackVolTermStructure>(c.vol));
}

qlib::ext::shared_ptr<qlib::GeneralizedBlackScholesProcess> LocalVolProcess(
    const Curves& c, const qlib::ext::shared_ptr<qlib::LocalVolTermStructure>& local) {
  return qlib::ext::make_shared<qlib::GeneralizedBlackScholesProcess>(
      qlib::Handle<qlib::Quote>(c.spot), qlib::Handle<qlib::YieldTermStructure>(c.dividend),
      qlib::Handle<qlib::YieldTermStructure>(c.risk_free),
      qlib::Handle<qlib::BlackVolTermStructure>(c.vol),
      qlib::Handle<qlib::LocalVolTermStructure>(local));
}

qlib::ext::shared_ptr<qlib::StrikedTypePayoff> Call(double strike) {
  return qlib::ext::make_shared<qlib::PlainVanillaPayoff>(qlib::Option::Call, strike);
}

qlib::ext::shared_ptr<qlib::Exercise> AtDay(int days) {
  return qlib::ext::make_shared<qlib::EuropeanExercise>(Today() + days);
}

std::vector<qlib::Date> Fixings(int first, int count, int spacing) {
  std::vector<qlib::Date> dates;
  for (int k = first; k < first + count; ++k) dates.push_back(Today() + k * spacing);
  return dates;
}

template <class S>
qlib::ext::shared_ptr<qlib::PricingEngine> MakeEngine(const EngineSpec& e, qlib::BigNatural seed) {
  switch (e.kind) {
    case EngineKind::kArithmetic:
      return qlib::MakeMCDiscreteArithmeticAPEngine<qlib::PseudoRandom, S>(e.process)
          .withBrownianBridge(false)
          .withAntitheticVariate(false)
          .withControlVariate(e.control_variate)
          .withSamples(e.samples)
          .withSeed(seed);
    case EngineKind::kGeometric:
      return qlib::MakeMCDiscreteGeometricAPEngine<qlib::PseudoRandom, S>(e.process)
          .withBrownianBridge(false)
          .withAntitheticVariate(false)
          .withSamples(e.samples)
          .withSeed(seed);
    case EngineKind::kBarrier:
      return qlib::MakeMCBarrierEngine<qlib::PseudoRandom, S>(e.process)
          .withSteps(50)
          .withBrownianBridge(false)
          .withAntitheticVariate(false)
          .withBias(e.biased)
          .withSamples(e.samples)
          .withSeed(seed);
    case EngineKind::kEuropean:
      return qlib::MakeMCEuropeanEngine<qlib::PseudoRandom, S>(e.process)
          .withSteps(kR12Steps)
          .withBrownianBridge(false)
          .withAntitheticVariate(false)
          .withSamples(e.samples)
          .withSeed(seed);
  }
  return nullptr;
}

qlib::ext::shared_ptr<qlib::PricingEngine> MakeEngine(const EngineSpec& e, qlib::BigNatural seed) {
  return e.tuned ? MakeEngine<qlib::IncrementalStatistics>(e, seed)
                 : MakeEngine<qlib::Statistics>(e, seed);
}

EngineSpec Arithmetic(const Settings& s,
                      qlib::ext::shared_ptr<qlib::GeneralizedBlackScholesProcess> p, bool cv,
                      qlib::Size n) {
  return {.kind = EngineKind::kArithmetic,
          .process = std::move(p),
          .samples = n,
          .tuned = s.tuned,
          .control_variate = cv,
          .biased = false};
}

EngineSpec Geometric(const Settings& s,
                     qlib::ext::shared_ptr<qlib::GeneralizedBlackScholesProcess> p, qlib::Size n) {
  return {.kind = EngineKind::kGeometric,
          .process = std::move(p),
          .samples = n,
          .tuned = s.tuned,
          .control_variate = false,
          .biased = false};
}

EngineSpec Barrier(const Settings& s, qlib::ext::shared_ptr<qlib::GeneralizedBlackScholesProcess> p,
                   bool biased, qlib::Size n) {
  return {.kind = EngineKind::kBarrier,
          .process = std::move(p),
          .samples = n,
          .tuned = s.tuned,
          .control_variate = false,
          .biased = biased};
}

EngineSpec European(const Settings& s,
                    qlib::ext::shared_ptr<qlib::GeneralizedBlackScholesProcess> p, qlib::Size n) {
  return {.kind = EngineKind::kEuropean,
          .process = std::move(p),
          .samples = n,
          .tuned = s.tuned,
          .control_variate = false,
          .biased = false};
}

qlib::ext::shared_ptr<qlib::Instrument> ArithmeticAsian(std::vector<qlib::Date> fixings,
                                                        double strike, int expiry_days) {
  return qlib::ext::make_shared<qlib::DiscreteAveragingAsianOption>(
      qlib::Average::Arithmetic, 0.0, 0, std::move(fixings), Call(strike), AtDay(expiry_days));
}

qlib::ext::shared_ptr<qlib::Instrument> DownAndOut(double barrier, double strike, int days) {
  return qlib::ext::make_shared<qlib::BarrierOption>(qlib::Barrier::DownOut, barrier, 0.0,
                                                     Call(strike), AtDay(days));
}

Pricer Build(qlib::ext::shared_ptr<qlib::SimpleQuote> spot,
             qlib::ext::shared_ptr<qlib::Instrument> instrument, EngineSpec engine) {
  auto impl = std::make_unique<Pricer::Impl>();
  impl->spot = std::move(spot);
  impl->instrument = std::move(instrument);
  impl->engine = std::move(engine);
  return Pricer(std::move(impl));
}

}  // namespace

Pricer::Pricer(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Pricer::Pricer(Pricer&&) noexcept = default;
Pricer& Pricer::operator=(Pricer&&) noexcept = default;
Pricer::~Pricer() = default;

void Pricer::SetSeed(std::uint64_t seed) {
  impl_->instrument->setPricingEngine(
      MakeEngine(impl_->engine, static_cast<qlib::BigNatural>(seed)));
}

Npv Pricer::Price() {
  impl_->instrument->recalculate();
  return {.value = impl_->instrument->NPV(), .error = impl_->instrument->errorEstimate()};
}

double Pricer::CentralBump(double h) {
  const double s0 = impl_->spot->value();
  impl_->spot->setValue(s0 + h);
  const double up = impl_->instrument->NPV();
  impl_->spot->setValue(s0 - h);
  const double down = impl_->instrument->NPV();
  impl_->spot->setValue(s0);
  return (up - down) / (2.0 * h);
}

Pricer MakeCasePricer(const Case& c, const Settings& settings, std::uint64_t samples) {
  const pricer::Market<double> m = OurMarket(c);
  const auto n = static_cast<qlib::Size>(samples);
  switch (c.row) {
    case Row::kR1: {
      const Curves curves =
          FlatCurves(m.spot, m.r, m.q, m.sigma, qlib::Actual360(), settings.tuned);
      const auto option = qlib::ext::make_shared<qlib::DiscreteAveragingAsianOption>(
          qlib::Average::Geometric, 1.0, 0, Fixings(1, 10, 36), Call(c.strike), AtDay(360));
      return Build(curves.spot, option, Geometric(settings, ExactProcess(curves), n));
    }
    case Row::kR2:
    case Row::kR3:
    case Row::kR9a: {
      const Curves curves =
          FlatCurves(m.spot, m.r, m.q, m.sigma, qlib::Actual360(), settings.tuned);
      return Build(curves.spot, ArithmeticAsian(Fixings(0, 13, 30), c.strike, 360),
                   Arithmetic(settings, ExactProcess(curves), c.row == Row::kR3, n));
    }
    case Row::kR4: {
      const Curves curves =
          FlatCurves(m.spot, m.r, m.q, m.sigma, FixedBaseDayCounter(3000.0), settings.tuned);
      return Build(curves.spot, ArithmeticAsian(Fixings(0, 51, 60), c.strike, 3000),
                   Arithmetic(settings, ExactProcess(curves), false, n));
    }
    case Row::kR5:
    case Row::kR6:
    case Row::kR7:
    case Row::kR9b: {
      const Curves curves =
          FlatCurves(m.spot, m.r, m.q, m.sigma, qlib::Actual365Fixed(), settings.tuned);
      const bool biased = c.row == Row::kR7 || c.row == Row::kR9b;
      return Build(curves.spot, DownAndOut(c.barrier, c.strike, 73),
                   Barrier(settings, ExactProcess(curves), biased, n));
    }
  }
  return Pricer(nullptr);
}

Pricer MakeLocalVolPricer(LocalVolRow row, const LocalVolGrid& grid, const Settings& settings,
                          std::uint64_t samples) {
  const pricer::Market<double> m = LocalVolRowMarket(row);
  const qlib::DayCounter dc = row == LocalVolRow::kR11
                                  ? qlib::DayCounter(FixedBaseDayCounter(3000.0))
                                  : qlib::DayCounter(qlib::Actual365Fixed());
  const Curves curves = FlatCurves(m.spot, m.r, m.q, m.sigma, dc, settings.tuned);
  const auto matrix = qlib::ext::make_shared<qlib::Matrix>(grid.spots.size(), grid.times.size());
  for (std::size_t i = 0; i < grid.spots.size(); ++i) {
    for (std::size_t j = 0; j < grid.times.size(); ++j) (*matrix)[i][j] = grid.At(j, i);
  }
  const auto surface = qlib::ext::make_shared<qlib::FixedLocalVolSurface>(Today(), grid.times,
                                                                          grid.spots, matrix, dc);
  if (settings.tuned) surface->enableExtrapolation();
  const auto process = LocalVolProcess(curves, surface);
  const auto n = static_cast<qlib::Size>(samples);
  if (row == LocalVolRow::kR11) {
    return Build(curves.spot, ArithmeticAsian(Fixings(0, 51, 60), 100.0, 3000),
                 Arithmetic(settings, process, false, n));
  }
  return Build(curves.spot, DownAndOut(95.0, 100.0, 73), Barrier(settings, process, true, n));
}

Pricer MakeR12Pricer(const SyntheticSurface& surface, const Settings& settings,
                     std::uint64_t samples) {
  const qlib::DayCounter dc = qlib::Actual360();
  const Curves curves = FlatCurves(surface.spot, surface.r, surface.q, 0.2, dc, settings.tuned);
  std::vector<qlib::Date> dates;
  std::vector<qlib::ext::shared_ptr<qlib::SmileSection>> sections;
  for (std::size_t j = 0; j < surface.terms.size(); ++j) {
    const qlib::Date date = Today() + static_cast<int>(surface.days[j]);
    const svi::RawSvi<double>& p = surface.params[j];
    dates.push_back(date);
    sections.emplace_back(qlib::ext::make_shared<qlib::SviSmileSection>(
        date, surface.terms[j].forward, std::vector<qlib::Real>{p.a, p.b, p.sigma, p.rho, p.m},
        dc));
  }
  const auto black =
      qlib::ext::make_shared<qlib::PiecewiseBlackVarianceSurface>(Today(), dates, sections, dc);
  // 1% where QuantLib's Dupire finds a negative local variance: this project's floor.
  const auto local = qlib::ext::make_shared<qlib::NoExceptLocalVolSurface>(
      qlib::Handle<qlib::BlackVolTermStructure>(black),
      qlib::Handle<qlib::YieldTermStructure>(curves.risk_free),
      qlib::Handle<qlib::YieldTermStructure>(curves.dividend),
      qlib::Handle<qlib::Quote>(curves.spot), 0.01);
  if (settings.tuned) {
    black->enableExtrapolation();
    local->enableExtrapolation();
  }
  Curves with_black = curves;
  with_black.vol = black;
  const auto option = qlib::ext::make_shared<qlib::VanillaOption>(Call(kR12Strike), AtDay(360));
  return Build(
      curves.spot, option,
      European(settings, LocalVolProcess(with_black, local), static_cast<qlib::Size>(samples)));
}

struct FixedSurface::Impl {
  qlib::ext::shared_ptr<qlib::FixedLocalVolSurface> surface;
};

FixedSurface::FixedSurface(const LocalVolGrid& grid) : impl_(std::make_unique<Impl>()) {
  SetEvaluationDate();
  const auto matrix = qlib::ext::make_shared<qlib::Matrix>(grid.spots.size(), grid.times.size());
  for (std::size_t i = 0; i < grid.spots.size(); ++i) {
    for (std::size_t j = 0; j < grid.times.size(); ++j) (*matrix)[i][j] = grid.At(j, i);
  }
  impl_->surface = qlib::ext::make_shared<qlib::FixedLocalVolSurface>(
      Today(), grid.times, grid.spots, matrix, qlib::Actual365Fixed());
}
FixedSurface::FixedSurface(FixedSurface&&) noexcept = default;
FixedSurface& FixedSurface::operator=(FixedSurface&&) noexcept = default;
FixedSurface::~FixedSurface() = default;

double FixedSurface::Vol(double t, double s) const { return impl_->surface->localVol(t, s, true); }

std::vector<std::pair<std::string, std::string>> VersionRecord() {
  std::array<char, 32> hex{};
  std::snprintf(hex.data(), hex.size(), "0x%08x", static_cast<unsigned>(QL_HEX_VERSION));
  std::vector<std::pair<std::string, std::string>> out = {
      {"QL_VERSION", QL_VERSION},
      {"QL_HEX_VERSION", hex.data()},
      {"BOOST_LIB_VERSION", BOOST_LIB_VERSION},
      {"BOOST_VERSION", std::to_string(BOOST_VERSION)},
  };
  const auto define = [&out](const char* name, bool on) {
    out.emplace_back(std::string("config.hpp ") + name, on ? "defined" : "undefined");
  };
#ifdef QL_FASTER_LAZY_OBJECTS
  define("QL_FASTER_LAZY_OBJECTS", true);
#else
  define("QL_FASTER_LAZY_OBJECTS", false);
#endif
#ifdef QL_ENABLE_SESSIONS
  define("QL_ENABLE_SESSIONS", true);
#else
  define("QL_ENABLE_SESSIONS", false);
#endif
#ifdef QL_ENABLE_THREAD_SAFE_OBSERVER_PATTERN
  define("QL_ENABLE_THREAD_SAFE_OBSERVER_PATTERN", true);
#else
  define("QL_ENABLE_THREAD_SAFE_OBSERVER_PATTERN", false);
#endif
#ifdef QL_EXTRA_SAFETY_CHECKS
  define("QL_EXTRA_SAFETY_CHECKS", true);
#else
  define("QL_EXTRA_SAFETY_CHECKS", false);
#endif
#ifdef QL_HIGH_RESOLUTION_DATE
  define("QL_HIGH_RESOLUTION_DATE", true);
#else
  define("QL_HIGH_RESOLUTION_DATE", false);
#endif
#ifdef QL_NULL_AS_FUNCTIONS
  define("QL_NULL_AS_FUNCTIONS", true);
#else
  define("QL_NULL_AS_FUNCTIONS", false);
#endif
#ifdef QL_USE_STD_SHARED_PTR
  define("QL_USE_STD_SHARED_PTR", true);
#else
  define("QL_USE_STD_SHARED_PTR", false);
#endif
#ifdef QL_ENABLE_TRACING
  define("QL_ENABLE_TRACING", true);
#else
  define("QL_ENABLE_TRACING", false);
#endif
  return out;
}

}  // namespace aadp::qlcompare::ql
