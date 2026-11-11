# TASKS

Plan through to the Done criteria. Tick items when done, and add anything new. Targets are in `BENCHMARKS.md`; findings and decisions are in `NOTES.md`.

Each phase ends with a phase report and waits for a go-ahead. Every commit builds and passes tests. Work is on `jb-build`, which is published to the public GitHub repository.

## Phase 1: Research

- [x] AAD cost, tape libraries, code-quality bar
- [x] Monte Carlo throughput, RNG, normal transform, SIMD layer, platform matrix
- [x] Barrier treatments, oracles, bump size, tolerance rule
- [x] SVI, arbitrage conditions, implied vol, Dupire, adjoint through calibration
- [x] Option chain search, licence reading, usability attack
- [x] Metal GPU feasibility, float32 error mechanisms, probe
- [x] QuantLib engine mapping, comparison protocol, build plan
- [x] `BENCHMARKS.md`, `TASKS.md`, `NOTES.md` first versions
- [x] Session lead re-opens every source behind a target, and re-runs the derived reference values
- [x] Narrow search on the gaps left by the search budget: Metal throughput, QuantLib speed, raw SVI fit error
- [x] Clear remaining `UNCONFIRMED` entries, or say why they stay
- [ ] Phase report

## Phase 2: Harness

Build the harness before the implementation. Nothing in this phase may claim a performance result.

### Repo and build

- [x] CMake project, C++23, presets for release, debug, asan-ubsan, tsan, fuzz
- [x] Toolchain taken from the environment (`CC`, `CXX`, `SDKROOT`). No hardcoded paths
- [x] `-Wall -Wextra -Werror` on project code, not on third-party code
- [x] `-ffp-contract=off`, no `-ffast-math`, on every target
- [x] Pinned FetchContent with `URL_HASH`: Google Benchmark, HdrHistogram_c, Highway, GoogleTest, Boost 1.92.0, QuantLib 1.44
- [x] Confirm that the toolchain's system libc++ link mode works with all dependencies. Fallback: static `libc++.a`
- [x] Do not depend on `<generator>` or `<stacktrace>`

### Platform layer, arm64 and x86-64

All of this is written for both architectures from the start, so a Linux run needs no code change.

- [x] `platform/cpu_info`: macOS `sysctlbyname`; Linux `/proc/cpuinfo` and `uname`; arm64 Linux `midr_el1` fallback
- [x] `platform/timer`: `clock_gettime(CLOCK_MONOTONIC_RAW)` on both. Raw ticks: `mach_absolute_time` on macOS, `rdtscp` on x86-64, `cntvct_el0` on arm64 Linux. Calibrate on every run, never assume a frequency
- [x] `platform/affinity`: `sched_setaffinity` on Linux. On macOS, record `not pinned` in the header and never claim otherwise. QoS class is recorded as an influence, not a pin
- [x] `platform/perf_counters`: `perf_event_open` on Linux, `:u` events only. On macOS, process-wide `proc_pid_rusage` cycles and instructions, labelled as such. Missing counters are recorded, not substituted
- [x] `platform/cacheline`: project constant, 128 on Apple arm64, 64 on x86-64. Do not use `std::hardware_destructive_interference_size` in layouts: Apple clang reports 256 here
- [x] `platform/fp_state`: record FPCR or MXCSR in the results header. Assert no FTZ or DAZ
- [x] Guard float-to-int conversions: out-of-range behaviour differs between arm64 and x86-64
- [x] SIMD layer on Google Highway with static dispatch per build (`HWY_COMPILE_ONLY_STATIC`). Targets: NEON (2), AVX2 with FMA (4), AVX-512 (8, build-only on both hosts here: neither can run it), each with a one-lane `FixedTag<double, 1>` instantiation, so scalar and SIMD share one FMA behaviour. `HWY_SCALAR` as a separate no-SIMD build (a486c9a, 065e5b6). The lanes use no fused multiply-add at all, so every width gives the scalar pricer's bits. Neither host runs AVX-512, but GitHub's runner did, in run 41898528776, and CI runs it under Intel SDE where the runner lacks it (36d701d)
- [x] CI compiles x86-64 AVX2 and AVX-512 and arm64 NEON: `linux-avx2`, `simd-lanes-8-avx512` (named `linux-avx512-build` until 065e5b6) and the macOS jobs, now on `xcode-27` (54933ed). The AVX-512 job runs the SIMD tests natively where the runner has every x86-64-v4 feature, and otherwise, since 36d701d, under Intel SDE `-skx`; before that it said it was build-only. The workflow first ran on GitHub in Phase 3: see the Phase 3 CI items and `NOTES.md`
- [x] Lane-width test matrix in CI, one job for each lane width that can run: `simd-lanes-1-scalar`, `-2-sse2`, `-2-neon`, `-4-avx2` and `-8-avx512` (065e5b6). Each passed its 14 SIMD tests in run 41898528776 at 48d30a7. Since 36d701d the 8-lane job runs its tests on every run, natively on x86-64-v4 and otherwise under Intel SDE 10.13.1 `-skx`, and a failed SDE download fails it. In run 41957624701 at a6be05a, on an AMD EPYC 7763 without AVX-512, it ran all 24 of the simd label under SDE, with Highway at AVX3: 24 passed, 0 skipped, 705 s
- [x] Any code that depends on lane count takes it from the type, never a constant
- [x] Hosts: `results/mac-m4max/` and `results/linux-zen1/`. No file holds numbers from both
- [x] Every reported benchmark runs through `.orchestrator/measure.sh` on the Mac, or `~/quant-projects/bin/measure.sh` on `debian`. Its load line goes in the results header. A results writer refuses to mark a run as reportable without it
- [x] `debian` is ready. Work only inside `~/quant-projects`. Measurement and Linux-only code only: development, sanitizers and fuzzing stay on the Mac. Sync the committed `HEAD` with `debian_sync.sh`, build in `~/quant-projects/aad-pricer/build`, fetch data with the repo's own script
- [x] `debian` runs: `~/quant-projects/bin/measure.sh taskset -c 4 <bench>`, `cpu5` idle, second thread on `cpu6`. Benchmark directly under `measure.sh`, with no wrapper that leaves background processes. Write output to a file on `debian`, never through SSH. A dropped connection kills the run, so a run with no complete output file is lost and is rerun, and `.logs/measure.log` is checked
- [x] Results header records core, governor, frequency, turbo. Every Linux number cites its commit

### Benchmarks

- [x] Google Benchmark plus HdrHistogram, with warm-up
- [x] Results header: CPU model, kernel, compiler version, flags, pinning state, core, governor, frequency, turbo, FP state, load line, commit
- [x] Linux: batch or average anything below about 100 ns. Mac: anything below a few 42 ns ticks. Do not publish a percentile the timer cannot resolve
- [x] Peak RSS recorded by the harness
- [x] README results table generated by a script from raw results files, never typed

### Tests and oracles

- [x] GoogleTest unit test suite
- [x] Tolerance rule implemented once, in one header: batch means, Student t, Bonferroni, SE ceiling. Fix K, α_F, B, seeds and path counts in a committed file before the first measured run
- [x] Black-Scholes closed-form Greeks oracle: O1 to O3, with stated conventions (per unit, theta per year, theta = −dp/dT)
- [x] Barrier closed form and analytic Greeks: down-in via the Merton identity, all eight cases
- [x] Geometric Asian closed form and Greeks: O4
- [x] Discrete barrier and arithmetic Asian references: O5 to O11, from a committed quadrature tool, with the published value next to the derived one
- [x] Central finite-difference oracle with common random numbers and the same discontinuity treatment. Two-h truncation check
- [x] Generic operation harness: every tape operation against finite differences, with NaN, inf, zero and exception parity (B19)
- [x] Naive-AAD failure test: the naive barrier delta converges to the wrong value, about 0.391 against 0.798 at H=95, and the test asserts that failure
- [x] Philox4x32-10 known-answer test, scalar. Open the vectors first: they were not opened in research
- [x] AS241 inverse normal against an independent high-precision reference
- [x] N-thread equals one-thread test (B11)
- [x] Comparison plan committed, K recorded, every row accounted for, pilot seed separate from measured seed

### Fault injection

- [x] Runner and `faults/manifest.json`: mutation on a copy under `build/`, baseline must pass, mutated run must fail for the named reason. `scripts/fault_injection.py`, with 36 tests of its own in `faults/test_fault_injection.py`
- [x] One fault per oracle: Black-Scholes, barrier closed form, geometric Asian, quadrature references, finite differences, naive barrier, tolerance rule, SE ceiling, K count, reference drift, Philox, inverse normal, N-thread. 13 faults. Two found holes, fixed by new tests (493ad27, 3682442)
- [x] One fault per check: ASan, UBSan, TSan, clang-tidy, clang-format, `-Wall`, `-Wextra`, `-Werror`, each fuzz target, data checksum, missing capture, actionlint, `scripts/check.sh` itself, results-header refusal, host-mixing refusal, attribution on output. 19 check faults and 9 fuzz faults
- [x] Results file with baseline and mutated outcome for every fault, on `mac-m4max`, and on `linux-zen1` for what runs there. Mac: 41 of 41 caught at 4928e8f. Linux: 29 of 29 caught at dba8dc5, leaving out the 9 fuzz and 3 sanitizer faults, which run on the Mac only

### Fuzzing

- [x] libFuzzer target for the TAIFEX CSV parser, including cp950 decoding
- [x] libFuzzer targets for every other parser and decoder: chain filters, config, any results reader

### Checks and CI

- [x] ASan and UBSan with `-fno-sanitize-recover=all`. TSan for all concurrent code that exists so far, on the Mac. Concurrent tapes are checked in Phase 3, under the multi-threaded tape item
- [x] `.clang-tidy` checked in, clang-format pinned. `scripts/run_clang_tidy.sh release fuzz` reports 78 units clean on the Mac and on Linux
- [x] `.github/workflows/ci.yml`: two compilers, Linux and macOS, Debug and Release, QuantLib optional
- [ ] CI compile options for tape layout, and Metal off on non-Apple. Neither is in `ci.yml`. Both depend on that work, so they belong in Phase 3, with the tape and Metal items
- [x] Validate CI locally with actionlint from repo tooling, and by running its commands on the Mac and Linux. In Phase 2 the Linux sanitizer and fuzz-smoke jobs had not run anywhere, and the workflow had never executed on GitHub. It first ran in Phase 3, at bc3e6d3, and failed 9 of 17 jobs (`NOTES.md`, "Phase 3 findings so far")
- [x] `scripts/compare_measured.py` compares the measured plan with `references/measured_results.csv`. All 90 rows match on the Mac, Linux clang, g++ 14.2 and AVX2
- [x] The two tests that depended on the caller's nice value are fixed (9b746bb)
- [x] Linux run at 89dbee8: clang release and debug, g++ 14.2 release and debug, AVX2 release, 364 of 364 each, 0 skipped. AVX-512 built, not run. Linux clang-tidy clean
- [x] libstdc++ path for the data area compiled and run: g++ 14.2 release and debug at 89dbee8, with the real-file tests run, not skipped
- [x] `scripts/scan_repo.py` scans tracked files for private content and raw data rows. Clean on the Mac and Linux
- [x] `scripts/verify_commits.sh` builds and tests each commit of a range from an export, on the `release` preset. Proved on three commits, 2490bd0 to 556a6c8
- [x] Full-range `verify_commits` run: `.orchestrator/heavy.sh scripts/verify_commits.sh --jobs 6 5ca2bd7..HEAD` at 73bb647 on the Mac, exit 0. 89 commits: 78 pass, 11 no-build (df1bb18 through 0899a0e, before CMake existed), 0 fail, 0 skipped tests. The root commit 5ca2bd7 is outside the range and has no CMake. Mac `release` only, so it does not cover Linux, g++ or the sanitizer presets per commit. `NOTES.md` says which commits failed on Linux
- [x] README for this phase: status, build, checks, data, CI. The results table waits for Phase 4
- [x] `scripts/check.sh` runs every check: 13 steps, fault injection last. Final run at 73bb647: 13 of 13 pass, 0 skipped

### Data

- [x] `scripts/fetch_data.*` downloads the TAIFEX option and futures captures and verifies sha256. Descriptive User-Agent, as Internet Archive's developer guide requires. Data goes to gitignored `data/`, on both hosts
- [x] `NOTICE` with the exact attribution text for datasets 11320 and 11319. Failing attribution voids the licence ab initio, and this covers derivative outputs
- [x] Every generated output that uses the chain carries the attribution: one helper, tested, used by every writer
- [x] If a capture disappears, the script says so and stops. There is no fallback source
- [x] Fields are named for what they are: settlements, never quotes or mids

## Phase 3: Implementation

Stages 1a to 3b are done, then stage 4, second-order Greeks, stage 5a, the SIMD primal on Highway lanes, stage 5b, the vector tape, and stage 5c, the lanes in the AAD bench with their assembly, with three rounds of CI fixes and Intel SDE for the 8-lane CI job. Stage 6a, implied volatility and the arbitrage checks, and stage 6b, the parity forwards, the calibration set and the raw SVI fit to one day's settlements, are done, with one CI fix (2b6f343). Stage 7a, local volatility off the raw SVI surface with its 15 parameter sensitivities on the tape (c1449fd to a4bb6ff), and stage 7b, the sensitivities to the calibration settlements through the fit (bcdbc28 to d3a0648), are done, each green in CI on its first run. Stage 7c, an arbitrage-free eSSVI surface beside the raw SVI fit (31267b1 to 8cbaaf3), is done, green in CI on its first run. Stage 8 is done: the references, pilot and ceilings for the 57 awaiting rows (6bf17a9 to 4398aee), then measured run 2 of all 147 rows (d70a1d4, 22e9d3d), which passed 145 and failed 2, `P3.DB.d12.rho` and `P3.CB.d12.rho`, on the SE ceiling. Findings are in `NOTES.md`, "Phase 3 findings so far", and measured numbers under each target in `BENCHMARKS.md`. Every stage 5b, 5c, 6 and 7 timing is iteration only.

### Tape

- [x] Tape bytes per path recorded by the harness, beside peak RSS (e4a9a98). Linux RSS probe fixed in c1c1a62 and d1596e0
- [x] Each benchmark verifies its gradient inside the run (B33) (e4a9a98). It caught a real bug in the barrier bump, fixed in 1172aaa
- [x] Tangent-adjoint dot-product test (B20) (96644e1)
- [x] Pricer entry-point fuzzer under sanitizers (B23): `fuzz_pricer` (5036729) and `fuzz_pricer_gradient` (96644e1)
- [x] Tape operation-sequence fuzzer against finite differences (B23, exceeds): `fuzz_tape_operations` (96644e1, round-off at every stencil point since 6932c4e)
- [x] Struct-of-arrays tape: statement and operand arrays, chunked, 16-byte active type, sizes asserted (19d7789)
- [x] Thread-local tape with RAII scope, mark and rewind per path, no allocation after warm-up (19d7789; allocation test a7a2591)
- [x] Surface and other set-up stay outside the per-path tape, swept once from a mark to the start. Per-path adjoint of shared set-up breaks the constant multiple (19d7789). Swept once per block of 1024 paths, counted in 653ea07
- [x] Hand-coded pathwise adjoint for the GBM Asian, as the `AAD-5` baseline (16a5211)
- [ ] One-node-per-operation tape first. Then evaluate expression templates against `AAD-2`. The one-node tape is done (19d7789). Expression templates remain, at stage 11
- [x] Vector values: one tape structure shared across lanes, with per-lane partials and a vector adjoint. Requires branch-free kernels, which the smoothed and bridge treatments give. Fall back to the scalar tape where lanes diverge. Measure both, and show assembly for any claim (B15). Lane kernels branch-free on every product and treatment (a486c9a). Design and oracle first (6c72d84), vector tape and B16 (139f398), gradients with the counted fallback (a60129d), lane fuzzer and CI (a859c7c), 11 faults (77dd0c8). Bit-identical to the scalar tape at 1 and 2 lanes on NEON, 1, 2 and 4 on AVX2, and 8 in CI under Intel SDE (36d701d, run 41957624701). No pricer kernel falls back. Both tapes and a fallback's cost timed, iteration only (8e8a464, 7b97155); measured runs are under "Iterate". B15: the operand loop is packed at 2, 4 and 8 lanes, with no gathers (`docs/asm/`, 8022793)
- [x] Multi-threaded: one tape per worker, gradient accumulators merged in fixed block order. TSan clean (653ea07)
- [ ] B4: justify the chunk size, 2^13 entries, by a sweep. Not swept yet
- [ ] B9: keep the zero-adjoint skip only if the measurement supports it. Measured as `tape-skip` (c90e7ca): faster than the plain sweep on every pricer workload on both hosts, slower on Lax-Wendroff. Whether the default sweep uses it is not decided
- [ ] B17: benchmarks record whether the primal auto-vectorised. Checked once, at 1078a20: it did not. By `-Rpass` remarks on NEON and AVX2, no loop in `src/pricer` or `src/math` vectorised, and the scalar step loop has no packed double arithmetic in the NEON, AVX2 or AVX-512 build (`docs/asm/`, 8022793; `NOTES.md`, "Stage 5c"). The results files do not record it yet
- [ ] B13: time the thread-local slot with and without `thread_local`. Every recorded operation reads it once, and on macOS that read is a thread-local lookup. Needs a CMake option to build the other way
- [x] B14: merge documented and order-independent. Block sums go to per-block slots, merged in block order after the workers join (653ea07; `src/ad/gradient.hpp`)
- [ ] Draw the one-step survival step in the complement form 1 − q = Φ(−a) + p u. At a = 8, u = 2^-53 the tape's delta is 0.0351 against the true 0.0266: the primal's conditioning, not the adjoint (`tests/ad/one_step_corner_test.cpp`)

### Pricer

- [x] Own `exp`, `log` and inverse-normal kernels, so libm differences do not reach results (f23dcd2, a43f40c)
- [x] Arithmetic Asian, geometric Asian (5fa6df6)
- [x] Continuous barrier: log-space Brownian-bridge survival weight (5fa6df6)
- [x] Discrete barrier: one-step survival. Never apply the bridge to a discretely monitored contract: at H=97, 3.060 continuous against 3.834 discrete (5fa6df6; the pricer refuses the bridge on a discrete barrier)
- [x] Hard-indicator mode, for naive failure and for QuantLib rows R5 and R7 (5fa6df6)
- [x] Knock-ins through parity. Digitals through last-step conditional expectation (5fa6df6)
- [x] Documented smoothing mode, for comparison with `AAD-1`, with its bias measured (5fa6df6; bias table 5036729)
- [ ] State and measure the bias of each treatment. Bridge under Euler is first order, with payoff conditions. Done for exact GBM steps (5036729, table in `NOTES.md`). The Euler part on the local-vol surface is measured, iteration only, on coupled draws at N = 2^20 (tool 5347509, results a4bb6ff, `NOTES.md`, "Stage 7a results"): left at n = 4, the Asian +0.36 ± 0.03, the continuous barrier by the bridge +0.33 ± 0.11 and by sampled crossing +0.68 ± 0.17, with the Asian's halving ratios near first order. What remains: the discrete barrier's Euler bias, by one-step survival, hard indicator and smoothing, is not resolved at that N
- [x] Barrier gamma through one-step survival, because bridge first derivatives are not Lipschitz (0d682fc, 09ad30a). Discrete barriers by one-step survival, continuous ones by one-step survival with the bridge. The plain bridge weight is never differentiated twice
- [x] SIMD paths, kernels instantiated from 1 to N lanes. Fixed block order for reduction. Scalar pricer 5fa6df6, lanes a486c9a: every product's price is the scalar pricer's bits at every width, on 1, 2, 3 and 8 threads
- [ ] Philox4x32-10 stream keyed on (path, step). Evaluate xoshiro256++ as CPU-only option with fixed block-to-stream assignment. Philox keyed on (seed, path, step) is done (5fa6df6), and on lanes, lane i drawing path first + i (a486c9a). xoshiro256++ remains, at stage 11
- [ ] Inverse normal: AS241 with masked tails, Giles as fallback if vector divergence dominates. The scalar masked AS241 is done (a43f40c), and its lane form, bit-identical (a486c9a). The lanes never diverge, but every lane computes the tail's `log` and `sqrt`. Whether Giles is faster is not measured
- [x] Scalar and SIMD agreement test, on every lane width that can run (a486c9a). Bit-identical, so no tolerance. Widths 1 and 2 on NEON, 1, 2 and 4 on AVX2 on the Linux host, and every CI lane job (065e5b6)
- [ ] Philox4x32-10 known-answer test on SIMD lanes and Metal. Moved from Phase 2 for the same reason. Lanes done: Random123's vectors, 10 and 7 rounds, at every width (a486c9a). Metal remains, at stage 9
- [ ] Sobol with Joe-Kuo directions and Brownian-bridge construction, as an evaluated option. SE is meaningless on Sobol, so oracles stay on pseudo-random

### Bump and revalue

- [x] Common random numbers, same path count, same optimisation level (6bfdc62, e4a9a98)
- [x] Bump size chosen by experiment, stated in results. With treatments, h is limited by round-off, so check by halving. The n^(−1/5) rule applies only to naive differences (e4a9a98, rerun in 1172aaa: `bench/bump_size_study.csv`, `bench/aad_steps.hpp`)
- [x] Report the AAD multiple beside tape bytes per path and peak RSS (c3831fa; results 2fc28be on `mac-m4max`, e495a37 on `linux-zen1`)
- [x] Report accuracy beside wall time: AAD without QMC is not guaranteed faster than differences at fixed accuracy for few Greeks (c3831fa: SE, bias and RMSE² × time per Greek)

### Second-order Greeks

- [x] Forward-over-reverse gamma, vanna, volga. Separate cost, never in the headline multiple (0d682fc, 09ad30a; faults 3e4975f; bench fb97650, faults 4072072). Timed apart in `aad8-asian12` and `aad8-barrier`, which have had iteration runs only. Their measured runs are under "Iterate"

### SVI

- [x] Vendor nothing. Let's Be Rational through pinned FetchContent, with its notice preserved; note the host only serves plain http, so the hash is what protects it (83d93b1). The live archive's SHA-256 is Phase 1's pin, its notice is quoted in `NOTICE`, and it is fetched, cached in CI and never committed. clang needs one identifier renamed in the build tree
- [x] TAIFEX parser, filters and parity regression from `NOTES.md`, "Recommended calibration set". Parser (622d569) and filters (d98cb32) from Phase 2. The parity regression over `settle_all` (9328a13) reproduces Phase 1's F, D and residual RMS within a relative 1e-9, F within 0.92 points of TX where a TX month exists, and the 65, 51 and 64 rows of the calibration set
- [x] Let's Be Rational inversion, accuracy and speed (83d93b1; bench 74e5b62). `SVI-1` met on 18,560 quotes: worst 0.25 of the vol bound and 0.27 of the price bound. Speed is iteration only so far (`SVI-2`, 140 ns on macOS); its measured run is under "Iterate"
- [x] Raw SVI per slice: quasi-explicit inner solve, multi-start, parity-noise weights (8eef2cb; chain and `aadp_svi_fit` 75fa774). Gauss-Newton on the price objective, σ_i² = σ_leg² + tick²/12 with `settle_all`'s σ_leg, a 9 × 9 grid in (m, σ) and Nelder-Mead from the best 5. Synthetic slices are recovered within the pre-registered tolerances. The fit is to settlements of one day, not tradable quotes, and maturity reaches 33 days
- [x] Handle rho sticking at its bound: an active bound breaks a plain implicit-function adjoint (8eef2cb, c94296d). Every bound's slack is recorded, and an active bound is set exactly, so stage 7 can take the derivative on the free parameters only. ρ = −1.05 and +1.05 fit −1 and +1 exactly, with the bound recorded active. On the real slices ρ stays inside its bounds; the active bound there is b(1 + |ρ|) = 4
- [x] Butterfly: g(k) on a grid, wing conditions at 2, Lee moment formula, Martini-Mingone classifier (fd60de0)
- [x] Calendar: numerical crossing check between slices (fd60de0). A union grid and the wing asymptotes. No necessary and sufficient condition is known, and the API says so
- [x] Tests on known violators: Vogt, the slope 2.25 counterexample, a constructed crossing pair (fd60de0; faults 4dafb00). Vogt is flagged on k ∈ [0.643, 1.256] and classified type 3; slope 2.25 fails at 2 and passes at 4; four crossing pairs are caught
- [x] Report input butterfly counts beside fit violations. Do not repair silently (9328a13, 75fa774). Input triples, non-convex and beyond half a tick: 168, 51 and 23, as Phase 1. The fit report lists each fitted slice's violations beside them, and the same triple count on the model prices at the quoted strikes: 0 of 168 non-convex. Nothing is repaired
- [x] Report per-slice implied-vol RMSE by the `SVI-11` formula, unweighted, beside the weighted objective (8eef2cb, 75fa774): 0.96, 0.41 and 1.40 vol points at 5, 12 and 33 days, beside Φ / (n − 5) of 0.207, 2.32 and 3.24
- [ ] `aadp_results_stamp` cannot stamp a non-timing report: it refuses the fit report ("first line is not the results magic"). So the fit report is not yet in `results/` and cannot reach the README table. It needs a non-timing report kind, with a fault. Until then the report is kept in the research cache
- [x] eSSVI as an optional arbitrage-free comparison, reporting what it costs in fit error (31267b1..8cbaaf3: pre-registration 31267b1, surface, fit, checker, report tool and fuzzer ae72e4c, fuzz fixes 3f0e5a9 and b8a67a0, 33 faults and results 8cbaaf3). Mingone's global parametrisation, with butterfly B1 and B2 and calendar N and S held by construction, all three slices fitted at once from 16 starts. Iteration only, on settlements of one day, reaching 33 days: Φ 449.02 against raw SVI's 310.35, 1.45 times; `SVI-6` 0.450, 1.174 and 1.603 bp of F against 0.287, 1.132 and 1.231; `SVI-11` 1.89, 1.02 and 2.50 vol points against 0.96, 0.41 and 1.40. Stage 6a's checks find no violation on the converted slices, and no constraint is active. Priced off it, no path-step is floored or capped (`NOTES.md`, "Stage 7c results"; `SVI-9`)
  - Why it matters: the raw SVI fit pins b(1 + |ρ|) = 4, Zeliade's bound and twice Lee's 2, on all three slices, with ρ from 0.964 to 0.992 and the vertex m to the right of the data. So every fitted slice has butterfly arbitrage beyond its quoted strikes (density, right wing, d+ limit, Martini-Mingone type 1), and adjacent slices cross in total variance, once inside the 12-day slice's quotes (`NOTES.md`, "Stage 6b results"). The pricing surface stays raw SVI, as pre-registered; eSSVI at stage 7 is the arbitrage-free comparison, with its cost in fit error (`SVI-9`)
- [x] Time interpolation: Gatheral-Jacquier Lemma 5.1, or eSSVI scheme. Neither, as pre-registered (5f0e5c3): total variance linear in T at fixed k between slices, the first slice scaled to t = 0, and nothing past the last slice, 33 days (96f4959). Lemma 5.1's guarantee needs slices that are free of arbitrage and do not cross, and these cross, so here each calendar crossing becomes a negative ∂T w and is counted. eSSVI's scheme belongs to the eSSVI item
- [x] Dupire local variance as ∂T w / g, on the tape. Count any floored points and report them. Templated on `Real`, with the registered floor rule (96f4959); the 15 slice parameters on the tape (5347509). Every Euler step is classified and counted by reason, and floored and capped points are located in k and t per segment. On the real surface 0.385% of the Asian's and the bridge's path-steps are floored, at k from 0.0713 to 0.1344, where stage 6b's calendar crossings are, and 52 path-steps are capped (a4bb6ff; `NOTES.md`, "Stage 7a results"; `SVI-9`)
- [x] Price Asian and barrier off the surface. Maturity is limited to 33 days by the usable chain, and this is stated (5347509; results a4bb6ff). The arithmetic Asian call and the down-and-out calls, discrete by one-step survival and continuous by the bridge, K = 22,800, H = 21,500, maturing at the 202412 expiry, 33 days; a maturity past it is refused. Iteration only, on settlements of one day: 336.794 ± 0.440, 570.356 ± 0.829 and 552.838 ± 0.820, with their 15 sensitivities, the tape checked against `Dual` per path
- [ ] Optional second run on 202501 and 202503, labelled as exchange-set marks in every output
- [x] Market-quote sensitivities through the implicit function theorem. Disclose any Gauss-Newton approximation (bcdbc28..d3a0648: pre-registration bcdbc28, adjoint 352cf29, report tool, real-data test and 23 faults e1da8dc, results d3a0648). The KKT system with b(1 + |ρ|) = 4 active on every slice, and the exact Hessian by forward over reverse, for all 180 settlements of the calibration set, with F and D fixed and through the parity regression. Gauss-Newton is disclosed and measured: it moves dθ/dV by up to 62% and the price sensitivities by up to 14%, and fails 882 to 890 of each 900 parameter bump comparisons. The multipliers are 0.99 and 0.18 standard errors from zero on the two short slices, so the derivative there holds only for small moves. The registered bump-and-recalibrate check FAILS on 20 of 1,080 price comparisons, all on the barriers, at settlements 46 to 50 of 202411W4 and settlement 52 of 202411 through parity, where 7a's floors are; every parameter and parity comparison passes (`NOTES.md`, "Stage 7b results"; `SVI-10`)
- [ ] F_j and D_j on the tape. Stage 7b takes dP/d(F_j, D_j) by 12 central differences on common random numbers, which make the sensitivities through parity cost 1.94 to 2.46 times the price with its 15 sensitivities (iteration only, macOS)
- [ ] A finer-h price check for the barriers. In stage 7b's bump check, six halvings from min(σ_i, V_i / 2) on 2^16 paths do not reach the scale where the barrier price differences settle near the floored region, and 20 price comparisons failed. That registered result stays a failure

### Measured run 2

- [x] References for the 57 awaiting rows, registered before any code or pilot (6bf17a9; `NOTES.md`, "Stage 8: references for the 57 awaiting rows"). The 12 knock-in rows against references in `references/oracle_values.csv`: Black-Scholes minus the discrete down-and-out recursion, and the Reiner-Rubinstein down-and-in for the continuous barrier (0e6d88d). The 45 surface rows against central differences of the local-vol price on the same paths, with h* and b from a committed ladder (01dbe41)
- [x] Pilot on seed 1729 and ceilings committed before run 2 (pilot 92f297f, plan a977c5f): `ceiling_factor` 1.65 on the knock-in rows, the plan's `fd_ceiling_factor` 3 on the surface rows, K still 147 and the 90 run-1 rows byte-identical. Largest pilot distance 2.82 SE; every surface row resolved at ℓ* = 0. Fingerprint per host (bf484f3). The fault 8a missed, `svi-floor-carries-derivative`, is caught by `LocalVolFloors` (fa766db, c53a117; 316 faults)
- [x] Measured run 2: all 147 rows, seed 271828, run once (`references/measured_results_run2.csv` d70a1d4, NOTES 22e9d3d). Plan a977c5f, the code of bf484f3 at 4398aee, `mac-m4max`, 623 s. 145 pass and 2 FAIL, 0 unresolved, 0 skipped. Both failures are on the SE ceiling only: `P3.DB.d12.rho`, SE 11.30 against its ceiling 8.16 (z +0.85), and `P3.CB.d12.rho`, SE 11.32 against 7.23 (z +0.94). They stay failures. The 90 rows shared with run 1 all pass and print the same estimates, references and SEs. Run 1's file is unchanged, and run 2 is reported beside it (`NOTES.md`, "Stage 8b: measured run 2"; `BENCHMARKS.md`, section 10)
- [ ] The `fd_ceiling_factor` 3 ceiling, set from one pilot, came out too tight for the `d12` surface rows. On development seeds after run 2, their paired SE spreads by about 3.4 times across seeds at 2^22, against the about 2.9 the factor allows, while on one seed it falls as N^-1/2 (`NOTES.md`, "Stage 8b: measured run 2"). A ceiling rule for such heavy-tailed rows is owed before any later measured run. Nothing about run 2 changes: its two failures stand
- [ ] The g = 0 boundary term is not exercised by any surface row. In the pilot and in run 2 no path-step crosses g = 0 inside any bump: the butterfly-floored count changes on no row at h*. It needs a check of its own

### Metal

- [ ] Backend behind `AAD_PRICER_ENABLE_METAL`, on when `APPLE` and Metal is found, off otherwise. Non-Apple builds and CI unaffected
- [ ] Objective-C++ `.mm` with ARC behind a plain C++ interface. Run-time kernel compile from an embedded string, because this host has no offline `metal` compiler
- [ ] GBM Asian and barrier kernels. Log-moneyness state, no `log` in the kernel path, exact 23-bit uniforms, symmetric inverse normal
- [ ] Reduction on the host in double. A float32 running sum stalls: up to about 83% error at N = 1e8 (derived)
- [ ] Short kernels to measure rounding mode, denormals, `uint`-to-`float` rounding and actual `exp` and `log` error on this GPU. None of these is known
- [ ] Find a safe command-buffer batch size. Apple's time limit is unpublished in what we opened
- [ ] Table: double (32-bit uniforms), double (23-bit uniforms), CPU float32, GPU float32, with paired difference and its SE
- [ ] GPU against SIMD CPU, same N and RNG, including buffer setup and readback. CPU float32 line included
- [ ] State plainly in README and WRITEUP that AAD does not run on the GPU

### QuantLib comparison

Build only behind an option, because 985 source files make it a long build.

- [ ] Flags in `CMAKE_CXX_FLAGS` so QuantLib inherits them. Fail the bench step unless a sampled QuantLib translation unit and a bench translation unit show the same optimisation and architecture flags
- [ ] Record QuantLib version, Boost version, all `QL_*` values, compiler, flags, link mode, LTO
- [ ] Report "default" and "tuned" QuantLib columns, so a gap cannot be blamed on a careless baseline
- [ ] Rows R1 to R7 and R9, with paths/s, ns per path-step, RMSE² × time, and accuracy against the row's reference
- [ ] Rows R10 to R12, labelled as speed-only or sanity rows. R8 stays a recorded non-comparison
- [ ] If QuantLib cannot be built here, list it under `Blocked:` and substitute nothing

### Iterate

- [ ] `perf record -g --user-callchains -e cycles:u` on `debian`. Perf cycles and TSC ticks are different units. Zen 1 `cache-misses` appears to count L2 misses: confirm in AMD's documentation and cite it before quoting the number
- [ ] Iterate against the harness. Record what was tried in `NOTES.md`, including what did not help
- [ ] Look at the local-vol AAD cost: on stage 7a's surface the price with its 15 sensitivities takes 8.2 to 12.8 times the price alone (iteration only, macOS, single thread, N = 2^16; `NOTES.md`, "Stage 7a results"). Why is not yet known
- [ ] Find out why the tape costs 166 times the primal on Lax-Wendroff on `mac-m4max`, and 106 on `linux-zen1`, against Hogan's 32 to 33 for Adept (B34). The hand adjoint costs 3.37 and 3.24 times, against Hogan's 2.1
- [ ] Reported numbers through `measure.sh` only
- [ ] Run all benchmarks on `linux-zen1` once it is ready. The six AAD workloads ran there at d1596e0 (e495a37), the threaded ones on two pinned CPUs. Later stages' benchmarks remain. Neither `aad8-asian12` and `aad8-barrier` (stage 4), `aadp_bench_simd` (stage 5a), nor the lane variants `price-lanes` and `tape-lanes` (stage 5c) has a measured run on either host yet
- [x] Fill the AAD benchmark's "x fastest primal" column: the faster of `price` and `price-lanes`, on the five first-order pricer workloads (8e8a464). The second-order workloads and `lw-worst` have no lane primal, and the table says so. Iteration figures only (`NOTES.md`, "Stage 5c"); the measured runs are the item above

### CI

- [x] Fix the failures of the first GitHub run, 41770201004 at bc3e6d3, which failed 9 of 17 jobs: d8e39d8, 74f0aac, 0bde08f, 8d00d85, bb7b26f, 54933ed. `scripts/check.sh` passes 13 of 13 at 54933ed
- [x] Fix the failures of run 41884006533 at 065e5b6, which passed 16 of 21 jobs: 22aaf7c, 4cf7fea, dca8324, f8aa71f, 2f1997f. Causes in `NOTES.md` (48d30a7)
- [x] Confirm every CI job is green once a push works. Run 41898528776 at 48d30a7 passed 22 of 22 jobs
- [x] CI's Linux clang jobs use libc++, like the Linux measurement host. The GCC jobs keep libstdc++ (2f1997f). A second tidy job, `clang-tidy-libstdcxx`, keeps tidy on libstdc++, because the findings differ by library
- [x] Fix the failures of run 41942650525 at a398d1f, which passed 16 of 22 jobs: 1029047. Both tidy jobs flagged an x86-only test branch the Mac's tidy cannot see, and four lane jobs lacked the fallback-cost binary, so its smoke test was skipped. Run 41953763165 at 1029047 passed 22 of 22
- [x] Run the 8-lane tests on every CI run: Intel SDE 10.13.1 `-skx` where the runner lacks AVX-512, at a pinned URL and sha256 (36d701d), with the fault `check-actionlint-sde-launcher` (a6be05a). Run 41957624701 at a6be05a passed 22 of 22
- [x] Fix the failure of run 42018873540 at 23936a9, which failed only `clang-tidy-libstdcxx`, on two findings that only libstdc++ shows: 2b6f343. Run 42023625872 at 2b6f343 passed 22 of 22
- [ ] `scripts/verify_commits.sh` needs rsync, which the Linux host lacks. Fix before stage 12

## Phase 4: Final review and Done

- [ ] Every target in `BENCHMARKS.md` met or beaten on the harness, or the write-up states the measured number, the gap and why
- [ ] `scripts/check.sh` passes
- [ ] Each fuzzer runs clean for 30 minutes, through `heavy.sh`
- [ ] README: what the project is, how to build and run it, results table generated by the harness
- [ ] WRITEUP.md: blog-post-length design trade-offs with numbers: data layout, memory, branch prediction, what was tried, what did not work
- [ ] README and WRITEUP.md state that the data is settlement-based, covers one date and reaches only 33 days, name the doubts in `NOTES.md`, and do not imply settlements are tradable quotes
- [ ] WRITEUP reports arbitrage violations on the real fit, and the bias of each barrier treatment
- [ ] Bar items B1 to B39 checked against the repo, each ticked or explained
- [ ] Final review of the whole repo. For each problem: file and line, why it is wrong, how to show it fails. Then fix it
- [ ] Final model and effort check
