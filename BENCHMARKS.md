# BENCHMARKS

Targets for aad-pricer, and the evidence behind each one. Nothing here has been measured on this project yet. Every number below is a published figure, or arithmetic on one, and is a target until a results file says otherwise.

Research date: 2026-10-30. Raw evidence, with verbatim quotes and local copies, is outside the repo under `.orchestrator/research-cache/aad-pricer/` (see `NOTES.md`, "Evidence index").

## How to read this file

Each target states the number, one source with a URL that was opened, a verbatim quote, what the source actually measured, and a class:

| Class | Meaning |
|---|---|
| `DIRECT` | Closest published match to this workload. Still different hardware, so it is a target, not proof that it is reachable here. |
| `CONTEXT` | Different hardware or workload. It sets the bar a reader would compare us with, and nothing more. |
| `STRUCTURAL` | A hardware-independent layout or behaviour fact, checkable in code. |
| `DERIVED` | Arithmetic on sourced numbers, with the arithmetic shown. Not the source's own figure. |
| `UNCONFIRMED` | No opened source confirms it. Where we looked is listed in section 12. |

`Lead check` records whether the session lead opened the source personally, not only the subagent. `done` means the URL was re-downloaded and the quoted text was found (`verify/verify_sources.log`). `pending` means the subagent's evidence has not been re-opened yet.

Hosts are never mixed. `mac-m4max` is Apple M4 Max, macOS, unpinned, 24 MHz counter. `linux-zen1` is the `debian` host, AMD Ryzen Embedded V1605B (Zen 1), pinned with `taskset -c 4`. Pinned headline numbers come from `linux-zen1`. Wall-time ratios are valid on `mac-m4max` if the results header says so. Metal numbers come only from `mac-m4max`. Published server-class figures will beat this embedded chip, and any such gap is reported with the hardware difference as the reason. No target is shrunk to hide it.

## 1. AAD cost

**The multiple.** AAD multiple = (wall time for value plus all first-order Greeks) / (wall time for one pricing), same binary, same flags, same path count, same random numbers, single thread unless stated. It is reported twice, against the scalar primal and against the fastest primal (SIMD), because a slower primal flatters the ratio: the same code in `AAD-6`'s source moves from 1.27 to 1.90 between compilers. Record and reverse times are reported separately.

**What one reverse sweep produces.** Delta, vega (flat-vol case), rho, dividend rho, theta, and the sensitivity to every SVI parameter of every slice. Gamma, vanna and volga do not come from it. They take a second pass, are reported as a separate multiple (`AAD-8`), and never enter the headline.

### AAD-1: barrier, general tape, single thread: multiple below 3.0

- Source: Savine, *Modern Computational Finance*, Wiley 2018, printed p.424, PDF p.452. https://antoinesavine.com/wp-content/uploads/2021/05/modern-computational-finance.pdf
- Quote: "Our AAD implementation produces the entire risk in around 8 seconds (8,250 ms), less than three times a single valuation."
- Source measured: iMac Pro, Xeon W-2140B, 3.20 GHz, Windows 10, VS2017 release | 3y European call K=120 with 150 barrier, Dupire local vol on 60 times and 30 spots, 500,000 paths, 156 weekly steps, Sobol, 1,801 sensitivities (1,081 active), barrier smoothing of 5 | single thread | wall time, general operator-overloading tape wiped per path. "a pricing took around 3 seconds", so the implied multiple is about 2.75 (derived, 8,250 / 3,000, approximate).
- Class: `DIRECT`. Closest match found in every dimension, on different hardware, and on a smoothed barrier whose bias the source does not state. Lead check: done.

### AAD-2: barrier, stretch with expression templates: multiple 1.5

- Source: same page as `AAD-1`.
- Quote: "With the improvements of Chapter 15, when we further accelerate AAD by factor around 2, we produce this report in less than 5 seconds (4,500 ms)."
- Source measured: as `AAD-1`. Multiple derived: 4,500 / 3,000 = 1.5. The source does not state 1.5.
- Class: `DERIVED` from `DIRECT`. Stretch, applies only if our tape adopts expression templates. Lead check: done.

### AAD-3: barrier, multi-threaded: multiple 2.86, stretch 1.64

- Source: Savine, section 12.6, printed p.433-434, PDF p.461-462 (same URL).
- Quote: "With the parallel implementation, we get the exact same results in exactly one second, with perfect parallel efficiency." and "a parallel price with Sobol takes 350 ms in our example"
- Source measured: as `AAD-1` | 8 cores, thread-local tapes. Multiple derived: 1,000 / 350 = 2.86; with Chapter 15, 575 / 350 = 1.64.
- Class: `DERIVED` from `DIRECT`. `linux-zen1` has only two pinned threads (`cpu4`, `cpu6`), so eight threads can only be run on `mac-m4max`, unpinned. Lead check: done.

### AAD-4: arithmetic Asian: multiple 2.8

- Source: Capriotti, "Fast Greeks by Algorithmic Differentiation", J. Computational Finance 14(3), 2011, section 5.1.2, printed p.33, Figure 9(a). http://luca-capriotti.net/pdfs/Finance/jcf_capriotti_press_web.pdf
- Quote: "even for this more complicated example the extra overhead associated with the calculation of the risk is limited to approximately 180% of the cost of calculating the option value."
- Source measured: hardware not stated | "best of" Asian, 12 observation dates, up to 20 assets, local vol, Euler; paths not stated | threads not stated | CPU ratio, delta and vega. This is a pathwise method with an adjoint payout, source-transformed by Tapenade. It is not a full adjoint of the path.
- Class: `UNCONFIRMED` as a target for a full tape. It is the only published multiple found for an Asian on a local-vol path, and it is a partial adjoint, so holding a full tape to it is deliberately hard. The one published full-tape run on our workload gives 5.3 (`MC-2`, derived). Lead check: done for the quote; the Figure 9(a) annotation is pending.

### AAD-5: within 1.25 times a hand-written adjoint

- Source: Hogan, ACM TOMS 40(4), 2014, Table I discussion, p.26:12. https://www.met.reading.ac.uk/~swrhgnrj/publications/adept.pdf
- Quote: "Adept is 3.7–3.9 times the computational expense of the original algorithm, and 10–25% more computationally expensive than the hand-written adjoint codes."
- Source measured: 2.5 GHz Intel E5200 Pentium, g++ 4.3.2 -O3 -DNDEBUG | advection and radiative-transfer kernels, repeated with memory reuse | single thread by implication | adjoint time including the forward pass, over the primal.
- Class: `CONTEXT`. Not Monte Carlo, but the same-code comparison of tape and hand-written adjoint is exactly the question. We write a hand-coded pathwise adjoint for the GBM Asian as the baseline. Lead check: done.

### AAD-6: hand-written baseline: multiple below 2.0

- Source: Giles, Oxford report NA-07-12, Table 2, p.13. https://people.maths.ox.ac.uk/gilesm/files/NA-07-12.pdf
- Quote: "the most efficient code is the hand-coded reverse mode; the timings are similar to those reported in [12] with the valuation plus 80 sensitivities being obtained at a cost which is less than twice the cost of calculating the valuation on its own."
- Source measured: single Pentium 4, g++ and icc at highest optimisation | LMM, 40 time steps, 80 forward rates | one CPU | ms per path. Hand 0.47/0.37 = 1.27 on g++, 0.19/0.10 = 1.90 on icc (derived). FADBAD++ tape 16.8 and 48.6 (derived).
- Class: `CONTEXT`. Applies to our hand-coded baseline, not to the tape. Lead check: done.

### AAD-7: bump and revalue cost

- Source: Capriotti, Jiang, Macrina, 2017, section 4.2, p.16. https://discovery.ucl.ac.uk/id/eprint/1523595/1/Macrina_AADMC_final.pdf
- Quote: "the cost of bumping, for one-sided finite-difference estimators, is in general (1 + N) times the cost of as single valuation, where N is the number of model parameters"
- Target: the measured bump time matches (1 + N), or 2N + 1 for central differences (derived), within measurement noise, and AAD's speedup is reported as bump time over AAD time at equal path count, common random numbers and the same optimisation level. Bump size is stated and chosen by experiment.
- Class: `STRUCTURAL` formula. Lead check: pending.

### AAD-8: second-order Greeks: cost reported separately

- Target: gamma, vanna and volga by forward-over-reverse, with their own multiple. No numeric target.
- Class: `UNCONFIRMED`. No published forward-over-reverse timing on a Monte Carlo barrier or Asian was found. See section 12.

### Theory, not targets

The bound is on operation counts, not wall time. Griewank gives "the complexity ratio is exactly ω = 3" for polynomial multiplication counts (https://papers.baulab.info/papers/also/Griewank-2012.pdf, PDF p.8). Finance sources quote ω_A ∈ [3, 4] for value plus gradient. These are different models and different numerators, and the textbook that would map constants to models (Griewank & Walther) could not be opened, so no single number is "the" bound. Real tapes exceed it: dco/c++ "between 5 and 15" (https://support.nag.com/doc/techrep/pdf/tr3_14.pdf, section 4.1).

## 2. Tape memory

Tape memory is the main practical limit of AAD, so every AAD multiple is reported beside tape bytes per path and peak RSS.

### MEM-1: active type 16 bytes, statement at most 8 bytes, operand at most 12 bytes

- Source: Adept at commit d0a7751, `Statement.h:28-29`, `Active.h:559-560`; CoDiPack at c25d5c1, `jacobianBaseTape.hpp:105-110`. https://github.com/rjhogan/Adept
- Class: `STRUCTURAL`, read from source, not measured. Asserted with `static_assert`. Lead check: pending.

### MEM-2: 12k + 1 bytes per statement with linear indexing

- Source: Sagebaum, Albring, Gauger, arXiv 1709.07229, p.7. https://arxiv.org/pdf/1709.07229
- Quote: "k ∗ 8 + k ∗ 4 + 1 = 12 ∗ k + 1 for each statement."
- Class: `STRUCTURAL`. Preprint; the ACM TOMS 2019 version was not opened. Lead check: done.

### MEM-3: peak tape at most 1.9 MB per path

- Source: du Toit & Naumann, NAG TR 3/14, Table 1, PDF p.15. https://support.nag.com/doc/techrep/pdf/tr3_14.pdf
- Quote: "The naive approach (mc/a1s) without checkpointing yields infeasible memory requirements for gradient sizes n ≥ 22."
- Source measured: reference computer with 3 GB, CPU not stated | European call, local vol, 10,000 paths, 360 steps, n = 10 | single thread by implication | peak memory, 1.9 MB per-path ensemble against 2 GB whole-simulation.
- Class: `DIRECT`, vendor-affiliated. Lead check: pending for the table row.

### MEM-4: per-path tape below 50 MB in the worst case

- Source: Savine, section 11.4, printed p.404, PDF p.432 (URL as `AAD-1`).
- Quote: "The size of the tape is related to the processing time of one single path, hence, always below 50 MB and generally substantially less than that."
- Class: `CONTEXT`, a design rule rather than a measurement. Lead check: done.

### MEM-5: peak RSS at most 1.09 times the primal

- Source: Naumann, Paris talk, 24.01.2019, slide 8. https://calcul.math.cnrs.fr/attachments/journee-da-2019/naumann.pdf
- Quote: "pathwise AAD by dco/c++ 1.9 86 1.6", against "Primal 1.2 79 1".
- Source measured: hardware not stated | scalar SDE, Euler, 10^4 paths, 10^3 steps, precomputed random numbers, no payoff | threads not stated | MB. Derived: 86 / 79 = 1.09. Whole-simulation tape: 728 MB.
- Class: `DERIVED`, `CONTEXT`. Toy SDE. Lead check: done.

### MEM-6: no heap allocation after warm-up

- Target: proved by a counting-allocator test. Hogan and Savine reuse memory; asserting it exceeds them.
- Class: `STRUCTURAL`. Bar item B5.

Sizing context, not a target: "tapes consume around 5 GB of RAM per second per core in the non-instrumented calculation" (Savine, printed p.403).

## 3. Monte Carlo throughput

No source publishes paths/s for a path-dependent, CPU-only workload on Apple Silicon with named hardware, so these are `CONTEXT` throughout. STAC-A2 reports are login-gated and report Greeks elapsed time and options/s, and cannot be converted to paths/s.

### MC-1: arithmetic Asian, 252 fixings, price only: 1.044 million paths/s, single thread

- Source: NablaTensor blog, "How XAD and MatLogica performed". https://nablatensor.com/blog/xad-matlogica-aadc-monte-carlo-benchmark
- Quote: "On the Asian, those figures are 1.044 and 0.197 million."
- Source measured: x86-64, CPU unnamed, built `-O3 -DNDEBUG -mavx2 -mfma` | arithmetic Asian, 252 fixings, K=100, vol 20%, r 3%, T=1, seven spots, 300,000 paths, `std::mt19937_64`, double, cached draws | single thread | million paths/s, median of warm calls. Derived: 1.044e6 × 252 = 263 million path-steps/s.
- Class: `CONTEXT`. Single-author blog with an interest in the result. Cached draws exclude RNG cost, so we run a cached-draw mode to compare like with like, as well as inline generation. Lead check: done.

### MC-2: same, with five Greeks by tape: 0.197 million paths/s, single thread

- Source and quote: as `MC-1`. Derived multiple: 1.044 / 0.197 = 5.3. The post gives 0.127 million with fresh draws.
- Class: `CONTEXT`. Lead check: done.

### MC-3: same, eight threads: 1.245 million paths/s, stretch 5.41

- Source: same post, results table. Quote: "| XAD C++ tape, harness pool | 8 | 39.43 | 1.245" and "| MatLogica AADC | 8 | 130.76 | 5.41"
- Class: `CONTEXT`. 5.41 is a recorded-graph JIT, not a tape, and runs through Python bindings. `mac-m4max` only, unpinned. Lead check: pending for these rows.

### MC-4: NEON European: 18.2 million paths/s, serial

- Source: arXiv 2606.16059, Section 5.3, Table 1. https://arxiv.org/pdf/2606.16059
- Quote: "Mojo serial (1 option)             54.8 ms"
- Source measured: "a commodity Apple Silicon laptop", chip not named, "NEON SIMD width 2 for double precision" | "Monte Carlo call, 1M paths", steps, RNG and transform not stated | serial | best of five. Derived: 1e6 / 0.0548 s = 18.2 million paths/s.
- Class: `CONTEXT`. The only Apple Silicon figure found. Preprint, European. Lead check: pending.

## 4. Random numbers

Decision recorded in `NOTES.md`: Philox4x32-10 is the reference stream on scalar, SIMD and Metal, because a keyed counter gives identical draws regardless of lanes, threads or backend, and it allows a path-by-path float32 vs double comparison. xoshiro256++ is evaluated as the faster CPU-only option.

### RNG-1: Philox4x32-10: 3.6 cycles per byte

- Source: Salmon et al., SC11, Table 2. https://www.thesalmons.org/john/random123/papers/random123sc11.pdf
- Quote: "Philox4×32-10           (4+2)×4           0       4×4      3.6      3.4"
- Source measured: 3.07 GHz quad-core Xeon X5667 (Westmere), gcc 4.5.3 | Random123 kernels | single core | cycles per byte.
- Class: `CONTEXT`. Cycles measured on `linux-zen1` with `perf`. Lead check: done.

### RNG-2: xoshiro256++: 0.58 ns per 64 bits, 0.33 cycles per byte

- Source: https://prng.di.unimi.it/, "Comparison of PRNGs".
- Quote: "xoshiro256++ 256 2 256 − 1 — 0.58 0.33" (table cells joined)
- Source measured: AMD EPYC 9R45 (Zen 5), SMT off, fixed 4.5 GHz, gcc 16.2.0 and clang 23.1.3 `-O3 -march=native -fno-unroll-loops` | 3 × 10^9 outputs, minimum of five runs | single core | ns per 64 bits, cycles per byte.
- Class: `CONTEXT`. The ns figure will be missed on Zen 1, and that gap is hardware. Cycles per byte is the fairer comparison. Lead check: done.

### RNG-3: inverse normal: accuracy 6.0e-16 maximum relative error

- Source: Wichura, Algorithm AS 241, Table 3, PDF p.4. https://csg.sph.umich.edu/abecasis/gas_power_calculator/algorithm-as-241-the-percentage-points-of-the-normal-distribution.pdf
- Source measured: PPND16 on IBM 3081 | 50,000 values uniform on a logit scale | relative error, maximum 6.0e-16 central, 5.8e-16 tails.
- Class: `DIRECT` for accuracy. Scanned, the value was read from a page image. Lead check: pending.

### RNG-4: inverse normal speed: 47 cycles scalar, stretch 9 cycles vector

- Source: Sheridan et al., arXiv 2012.09715, Table 2. https://arxiv.org/pdf/2012.09715
- Quote: "ASA241 [16, 69]                  —                      Intel           icc             Single            47 ± 1" and "Intel (HA)                   MKL VSL                    Intel           icc             Double           9 ± 0.5"
- Source measured: Intel Skylake Xeon Gold, icc | clock cycles per random number.
- Class: `CONTEXT`. 47 is single precision and ours is double, so the target is harder than the source. 9 is a vendor vector library at a wider width than NEON or Zen 1. Lead check: done.

## 5. SIMD

### SIMD-1: speedup reported against the lane ceiling

- Ceiling: double-precision lanes are 2 on NEON (`mac-m4max`), 4 on AVX2, 8 on AVX-512. Zen 1 runs each 256-bit AVX2 operation as two 128-bit ones, so `linux-zen1` has 4 lanes but about 2 lanes' throughput. Neither host has AVX-512: that code path is built, and not run.
- Target: SIMD speedup over scalar reported beside the ceiling, with the instruction set recorded. No efficiency percentage is claimed.
- Class: `UNCONFIRMED` for any efficiency figure. No published NEON Monte Carlo speedup with a named chip and step count was found.

### SIMD-2: scalar and SIMD agree

- Target: same stream, same results within a stated bound. Scalar is the one-lane instantiation of the same kernels, so per-path results are bit-identical within one build. Against a libm-based scalar check, relative price difference at most 2e-13 at 252 steps (derived: 252 steps × 4 ULP × 1.1e-16 = 1.1e-13, using Highway's stated 4 ULP `Log` bound, then rounded up).
- Across architectures, results are not expected to be bit-identical, because fused multiply-add status differs by target.
- Class: `DERIVED`, stated before measuring. Lead check: pending.

## 6. Metal GPU

Metal is an additional backend, float32 only, on `mac-m4max`. AAD does not run on the GPU. Greeks come from the double-precision CPU tape, and the GPU backend prices only.

### GPU-1: GPU against SIMD CPU at the same path count

- Target: measured wall clock from before buffer allocation to after host reduction, across all command buffers, at the same N and RNG, via `measure.sh`. Kernel compile and pipeline creation reported separately. A CPU float32 line is included, so a GPU ratio does not mix hardware with precision.
- Class: `UNCONFIRMED`. No published Monte Carlo pricer on Metal was reachable, so no pass mark exists, and none is invented. Stated as measured.

### GPU-2: float32 error far below Monte Carlo standard error

- Source: du Toit, Naumann et al., NAG, section 6.2, PDF p.10. https://support.nag.com/doc/techrep/pdf/adjoint-algorithmic-differentiation-of-gpu-accelerated-app.pdf
- Quote: "the standard deviation of the Monte Carlo estimate was orders of magnitude greater than the threshold of single precision accuracy (roughly 10−6)."
- Target: |float32 − double| on identical uniforms at most 0.01 × SE. The factor is our reading of "orders of magnitude" and is stated before measuring.
- Class: `CONTEXT`, different model and hardware. Lead check: pending.

### GPU-3: paired relative difference about 1e-7

- Source: NAG, section 5.4, PDF p.7, Table 3 p.8. https://support.nag.com/numeric/gpus/local-volatility-fx-basket-option-on-cpu-and-gpu.pdf
- Quote: "one would expect the relative error between the two results to be around 1e-7. If the error is significantly worse than this, it may suggest that the calculation is unstable in single precision, or that there are significant accumulations of round off errors"
- Published reference 4.08e-7, a reference and not a pass mark. Barrier prices may land above 1e-7 with no fault, because survival products and near-barrier steps carry larger rounding error (derived in the Metal evidence).
- Class: `CONTEXT`. Lead check: pending.

## 7. QuantLib comparison

QuantLib v1.44 and Boost 1.92.0, built through pinned FetchContent in release mode with the same flags. Protocol and row definitions are in `TASKS.md`.

### QL-1: speed

- Target: faster than QuantLib's tuned configuration in paths/s and in RMSE² × time, on every strictly like-for-like row. The size of the gap is not a target.
- Class: `UNCONFIRMED`. No published speed figure for QuantLib's Monte Carlo on Asian or barrier options, and no published custom-code ratio, was found on quantlib.org, Implementing QuantLib, its docs, or the XAD pages. `quantlib-benchmark` reports tasks/s and has no Asian or barrier Monte Carlo case.

### QL-2: accuracy

- Target: on each row, distance from the stated high-precision reference (section 9) within the tolerance rule (section 10). Distance between the two engines is never the measure.
- Class: `STRUCTURAL`.

## 8. SVI

### SVI-1: implied volatility inversion at machine precision

- Source: Jäckel, "Let's Be Rational", abstract, version of 25 Mar 2016. http://www.jaeckel.org/LetsBeRational.pdf
- Quote: "two iterations to maximum attainable precision on standard (64 bit floating point) hardware for all possible inputs."
- Class: `DIRECT` for accuracy. Lead check: done.

### SVI-2: inversion speed: 180 ns

- Source: Schadner, arXiv 2604.24480, numerical results. https://arxiv.org/pdf/2604.24480
- Quote: "the explicit formula requires 59.49 nanoseconds per evaluation at the median run, compared with 180.03 nanoseconds for LBR"
- Source measured: i5-1145G7 2.60 GHz, GCC under Linux/WSL2, flags not stated | 100,000 random cases × 10 × 10 runs | threads not stated | ns per evaluation.
- Class: `CONTEXT`. Preprint. Lead check: done.

### SVI-3: butterfly check catches a known violation

- Source: Gatheral & Jacquier, arXiv 1204.0646, Lemma 2.2 p.4 and Example 3.1. https://arxiv.org/pdf/1204.0646
- Quote: "A slice is free of butterfly arbitrage if and only if g(k) ≥ 0 for all k ∈ R and lim d+ (k) = −∞."
- Target: the check flags the Vogt vector (a, b, m, ρ, σ) = (−0.0410, 0.1331, 0.3586, 0.3060, 0.4153), t = 1.
- Class: `STRUCTURAL`. Lead check: done.

### SVI-4: wing check at the Lee limit of 2, not 4

- Source: Martini & Mingone, arXiv 2005.03340, Section 2. https://arxiv.org/pdf/2005.03340
- Quote: "In the case of SVI, the 1st condition translates to b(1+ρ) < 2 and the second one to b(1−ρ) < 2."
- Target: the check rejects a = 0.02, b = 1.5, ρ = 0.5, m = 0, σ = 0.2, which has b(1+|ρ|) = 2.25 and passes QuantLib's bound of 4 (derived counterexample).
- Class: `STRUCTURAL`. Lead check: done.

### SVI-5: calendar check by dense numerical crossing test

- Target: flags any crossing of total variance between adjacent slices, tested on a constructed crossing pair. A necessary-and-sufficient condition for two raw SVI slices is not published: Martini & Mingone leave "the question of the characterization of no Calendar Spread arbitrage" open. The check is numerical, and we say so.
- Class: `STRUCTURAL`. Lead check: pending.

### SVI-6: fit error at most 4 bp of forward

- Source: Corbetta et al., arXiv 1804.04924, Section 4. https://arxiv.org/pdf/1804.04924
- Quote: "with a typical average option price error below 4 bips of the underlying value."
- Source measured: SPX end of day, 8 Jan 2018, 12 maturities, OTM, price at least 2 ticks | eSSVI, not raw SVI | error against mid, in bp of forward, read from plots.
- Class: `CONTEXT`. A fit error on one underlying and date does not transfer to another. Our input noise is 2 to 3.4 index points of parity RMSE on a forward near 22,800, about 0.9 to 1.5 bp (derived), so the target is reachable in principle. Lead check: done.

### SVI-7: calibration time: 15 ms per slice, stretch 3 ms

- Source: Le et al., arXiv 2301.07830, Section 4. https://www.arxiv.org/pdf/2301.07830
- Quote: "for the FPI-SVI algorithm, the calculation time of each contract is stable at approximately 0.003 s; for the quasi-explicit SVI method, the calculation time of each contract is stable at approximately 0.015 s."
- Source measured: personal computer, unspecified | 100 calculation steps | per contract. The same paragraph says "50 times", but its own numbers give 5 (derived), so the 50 is not used.
- Class: `CONTEXT`. Lead check: done.

### SVI-8: surface time below 1.2 s

- Source: Corbetta et al., Section 3.2.5.
- Quote: "takes 1.2 seconds on a Intel E5-2673 v3 processor."
- Source measured: Intel E5-2673 v3, Python, no parallelism | 12 maturities, average 98 options | eSSVI | whole-surface wall time. The 0.1 s and 0.01 s in the same section are projections, not measurements, and are not used.
- Class: `CONTEXT`. A low bar for C++ and three slices. `SVI-7` is the real target. Lead check: done.

### SVI-9: every violation reported, none repaired silently

- Target: butterfly and calendar violations on the real fit are listed beside the counts already in the input (section 11). If an arbitrage-free constrained fit is added, its cost in fit error is reported.
- Class: `STRUCTURAL`.

### SVI-10: market-quote sensitivities through calibration

- Target: implicit-function-theorem adjoint. Any Gauss-Newton approximation is disclosed, because SVI residuals are not zero.
- Context: ratios of 1.18 to 1.83 are for exact-fit rates calibrations (Henrard, https://quant.opengamma.io/Adjoint-Algorithmic-Differentiation-OpenGamma.pdf, Table 2).
- Class: `UNCONFIRMED`. No published cost through SVI was found.

## 9. Reference values for oracles

Published value first, then the value we test against. A derived value comes from deterministic quadrature in the research scripts and is used only where it is more precise than, and consistent with, the publication.

`pending` values have not yet been re-run by the session lead.

| ID | Contract | Published | Source | Reference used | b (reference uncertainty) | Lead check |
|---|---|---|---|---|---|---|
| O1 | BS delta, call K=100, S=105, q=r=.10, T=.5, σ=.36 | 0.5946 | QuantLib 5d6defcc `test-suite/europeanoption.cpp:405+`, Haug 1998 | closed form | 0 | done |
| O2 | BS rho, call K=75, S=72, q=0, r=.09, T=1, σ=.19 | 38.7325 | as O1 | closed form | 0 | done |
| O3 | BS theta, put K=405, S=430, q=.05, r=.07, T=1/12, σ=.20 | -31.1924 per year | as O1 | closed form | 0 | done |
| O4 | Geometric Asian, 10 fixings, S=K=100, q=.03, r=.06, σ=.20, T=1 | 5.3425606635 | QuantLib `test-suite/asianoptions.cpp:429` | closed form | 0 | done |
| O5 | Discrete down-and-out call, S0=K=100, H=95, σ=.30, r=.10, T=.2, m=50 | 4.907 | Broadie, Glasserman, Kou 1997, Table 2.1. https://www.columbia.edu/~sk75/mfBGK.pdf | 4.9067890354 (derived) | 0.001 published | pending |
| O6 | Delta of O5 | 0.798 | same, Table 2.4 | 0.7978141916 (derived) | 0.001 | pending |
| O7 | Continuous down-and-out, same parameters, H=95 | 4.398 | same, Table 2.1 | closed form | 0 | done |
| O8 | Discrete down-and-out, m=4, H=95, σ=.6, T=.2 | 9.49052 | BGK 1999, Table 1. https://business.columbia.edu/sites/default/files-efs/pubfiles/1851/connecting.pdf | 9.4905 | 1e-4 | done |
| O9 | Discrete arithmetic Asian, n=50, K=100, S0=100, r=.0367, σ=.17801, T=1 | 4.93736 | Fusai & Meucci, Table 6. https://openaccess.city.ac.uk/id/eprint/6061/1/Pricing%20Discretely%20monitored%20asian%20Fusai_Meucci_JBF.pdf | 4.937202814482 (derived) | see note | pending |
| O10 | Continuous arithmetic Asian, Linetsky case 5 | 0.2464156905 | Linetsky 2004, Table 3. http://users.iems.northwestern.edu/~linetsky/asian.pdf | limit check only | 1e-10 | pending |
| O11 | Two-fixing arithmetic Asian put, identity with a vanilla | 1.393981 (derived) | exact identity | closed form | 0 | pending |

Note on O9: the published table does not reproduce at its printed σ. All geometric rows match closed form only at σ ≈ 0.1780163. The derived value is at the stated σ = 0.17801. Note on O10: a continuous average, reachable by a time-stepped simulator only in the limit.

Do not use QuantLib's ten-digit Levy values (`asianoptions.cpp:683-775`). The two-fixing case listed as 1.3942835683 is exactly 1.393981 (derived), a difference of 3.0e-4 hidden by a tolerance of 2e-2.

## 10. Tolerance rule

Stated before measuring. Each comparison passes only if both of these hold:

1. |x̄ − ref| ≤ t(1 − α_F / (2K), B − 1) · SE + b
2. SE ≤ a ceiling fixed per comparison before the run, so a noisy run cannot pass by being noisy.

SE is from B batch means, b is the reference or finite-difference truncation allowance, K is the number of comparisons in the suite, and α_F is the family-wise error rate. Bonferroni needs no independence assumption, which matters because Greeks from the same paths are correlated.

- Source: NIST/SEMATECH e-Handbook, section 7.4.7.3. https://itl.nist.gov/div898/handbook/prc/section4/prc473.htm
- Provisional constants: α_F = 0.01, B = 64, K = 100, giving a critical value of 4.155 (derived). K counts every Greek and case pair, including any skipped later. If K changes, the constant is recomputed before any run.

QuantLib's 2.34 × SE pattern is useful for its two-part shape, but the multiplier has no documented basis and is not copied. Its fixed tolerances (for example `2.0e-2` for arithmetic Asians) are not tied to any SE.

## 11. The option chain

Real, free and redistributable. Licence text and full reasoning are in `NOTES.md`.

- Data: TAIFEX TXO daily report, trade date 2024-11-15, data.gov.tw dataset 11320, Open Government Data License v1.0.
- Fixed file: `https://web.archive.org/web/20241117234312id_/https://www.taifex.com.tw/data_gov/taifex_open_data.asp?data_name=DailyMarketReportOpt`, 465,662 bytes, sha256 `fd9ddc6cafcbdd1b08cd0f80912dcbd9c19de515cdb248163183378715d53f1e`. Lead check: done, hash matched on a fresh download.
- Usable surface: three expiries only, 2024-11-20, 2024-11-27, 2024-12-18 (5, 12, 33 days). Longer expiries are exchange-set marks, not market prices. Any figure that includes them is labelled so.
- Input arbitrage, before any fit (subagent's count, lead re-run pending): 102 of 480 OTM settlement butterflies are non-convex, 45 beyond half a tick. Fit violations are always reported beside these.

## 12. Unconfirmed targets, and where we looked

The WebSearch budget ran out during research. Some absences below are therefore results of bounded searches, not proof that nothing exists. Each entry says which.

| Target | Looked in | Search state |
|---|---|---|
| `AAD-4` Asian full-tape multiple | Capriotti 2011, Capriotti & Giles 2024 review, Giles reports, Savine, XAD pages, NAG reports | Searched before the budget ended |
| `AAD-8` second-order timing | Capriotti, Joshi & Yang (abstract), Giles, Savine, Huge & Savine | Searched before the budget ended |
| `SIMD-1` NEON Monte Carlo speedup | Intel, AMD, Arm and Apple pages, Mojo preprint | Partly searched; Intel and AMD pages 404 |
| `GPU-1` Metal Monte Carlo throughput | Apple docs and talks, philipturner/metal-benchmarks, Random123, GPU Gems | **Not searched.** Budget was gone; direct URLs only |
| `QL-1` QuantLib speed | quantlib.org, docs, Implementing QuantLib, `Docs/`, XAD pages, FinanceBench paper | **Not searched.** Bounded walk only |
| `SVI-10` adjoint through calibration | Henrard, Christianson, Giles talks | Searched before the budget ended |
| Raw SVI fit error on a real chain, with its error defined | Zeliade, Aurell, Corbetta, Martini-Mingone, Ackerer, Pasquazzi, FPI-SVI | The last search was refused |

## 13. Numbers deliberately not used

| Number | Why not |
|---|---|
| XAD table (Heston 4.30, LIBOR 7.25, derived) | Withdrawn from the live page, known only from an archived snapshot, repo 404 |
| STAC-A2 33.4 ms, 119.2 options/s | Greeks workload not public, cannot convert to paths/s, reports login-gated |
| "AAD costs at most 4 times" as wall time | An operation-count bound. Tapes exceed it |
| Corbetta 0.1 s and 0.01 s | Projections, not measurements |
| FPI-SVI "50 times" | Contradicted by its own 0.003 s and 0.015 s |
| QuantLib Levy values | Printed digits are not correct digits |
| NablaTensor 9.7× adjoint vs bump | JDK engine with one recording, a different design from a per-path tape |
| VW4 eq. (12) sign | The display is wrong; its own R code, QuantLib and g(k) all have the opposite sign |

## 14. Code-quality bar

Drawn from reading XAD, Adept, CoDiPack, FastAD, Stan Math, CppAD, ADOL-C, autodiff, Savine's CompFinance, QuantLib and QuantLib-Risks at recorded commits. "Exceeds" means the surveyed libraries mostly do not do it.

**Tape and memory**
- B1. `static_assert` the active type is 16 bytes (XAD, Adept, CoDiPack).
- B2. Statement at most 8 bytes and operand at most 12 bytes, asserted at compile time, with struct-of-arrays or a recorded reason otherwise.
- B3. Tape bytes per statement and per operand reported by the tape itself (XAD `src/Tape.cpp:903-912`).
- B4. Chunked storage with no relocation while recording, chunk size justified by a sweep (CoDiPack Table V).
- B5. No heap allocation after warm-up, proved by a counting allocator. Exceeds.
- B6. Mark and rewind per path, then one mark-to-start sweep for set-up (design in `CompFinance/mcBase.h`).
- B7. A position or checkpoint API with its own test (XAD `test/Checkpointing_test.cpp`).
- B8. Slot-reuse decision recorded with measurements (CoDiPack found it mixed; XAD ships it off).
- B9. Zero-adjoint skip kept only if our measurement supports it.

**Threads**
- B10. Thread-local active tape, with a second activation on a thread an error, and an RAII scope (XAD `Tape.hpp:140-141`, Stan `nested_rev_autodiff.hpp:27-29`).
- B11. N-thread gradient equals one-thread gradient for fixed seeds, within a stated tolerance. Exceeds XAD, which has no threaded tests.
- B12. Threading tests in CI as a named job under TSan. Exceeds: no AD library surveyed runs TSan.
- B13. Cost of the thread-local pointer reported, with and without it ("a few percent" in Hogan, "roughly a 20% penalty" in Stan).
- B14. Merge documented and order-independent.

**SIMD and vector adjoints**
- B15. Any claim of a vectorised reverse pass is shown with generated assembly (FastAD paper p.7-8).
- B16. Vector adjoint mode tested against N scalar sweeps (`XAD/test/TestHelpers.hpp`).
- B17. Benchmarks record whether the primal auto-vectorised, because it can move the ratio by an order of magnitude.

**Derivative testing**
- B18. Every operation has an analytic derivative test, in reverse and tangent mode.
- B19. One generic harness checks every operation against finite differences, and checks exception, NaN, inf and zero parity with `double` (Stan `test_ad.hpp:326-332`). The most demanding test design surveyed.
- B20. Dot-product identity between tangent and adjoint for whole programs, at about 1e-14 (CoDiPack).
- B21. Whole-pricer AAD against bumped Greeks and closed-form Black-Scholes.
- B22. Compile-failure tests for misuse, and a multi-translation-unit include test.
- B23. Fuzz pricer entry points under sanitizers (QuantLib `fuzz-test-suite`). Fuzzing operation sequences against finite differences exceeds every project surveyed.

**CI**
- B24. Two compilers, two OSes, Debug and Release, every tape layout option on the matrix.
- B25. ASan and UBSan with `-fno-sanitize-recover=all` on every PR. Exceeds.
- B26. Coverage published.
- B27. clang-tidy with a checked-in config in CI.
- B28. Pinned clang-format check.
- B29. Install and consume test.

**Benchmarks**
- B30. Primal, hand-coded adjoint, AAD and bump on the same machine in one run.
- B31. AAD time split into record and reverse.
- B32. Each number carries hardware, compiler and flags, versions, thread count, pinning state, repetitions, median and spread.
- B33. Gradient correctness verified inside the benchmark run.
- B34. One adversarial workload with no transcendental functions, always published (Hogan's LW case runs at 22 to 33 times the primal).
- B35. Benchmarks run in CI, and raw results live in the repo. XAD's published evidence no longer resolves.

**API, docs, licence**
- B36. A short usage example that compiles in CI.
- B37. Tape design document with a byte-level layout (CoDiPack `TapingStrategy.md`).
- B38. Independent implementation. Nothing copied from AGPL, GPL or proprietary sources: XAD, CoDiPack, parts of QuantLib-Risks, and CompFinance, which is not open source.
- B39. `CITATION.cff` and `CHANGELOG.md`.
