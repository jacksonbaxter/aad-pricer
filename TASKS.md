# TASKS

Plan through to the Done criteria. Tick items when done, and add anything new. Targets are in `BENCHMARKS.md`; findings and decisions are in `NOTES.md`.

Each phase ends with a phase report and waits for a go-ahead. Every commit builds and passes tests. Work is on `jb-build`, with no remote.

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
- [ ] SIMD layer on Google Highway with static dispatch per build (`HWY_COMPILE_ONLY_STATIC`). Targets: NEON (2), AVX2 with FMA (4), AVX-512 (8, build-only: no host here can run it), each with a one-lane `FixedTag<double, 1>` instantiation, so scalar and SIMD share one FMA behaviour. `HWY_SCALAR` as a separate no-SIMD build
- [x] CI compiles x86-64 AVX2 and AVX-512 and arm64 NEON: `linux-avx2`, `linux-avx512-build` and the `macos-15` jobs. AVX-512 is build-only. See `NOTES.md`: the workflow has never executed on GitHub
- [ ] Lane-width test matrix in CI, one job for each lane width that can run. It needs the SIMD kernels, so it waits on Phase 3
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

- [ ] Runner and `faults/manifest.json`: mutation on a copy under `build/`, baseline must pass, mutated run must fail for the named reason
- [ ] One fault per oracle: Black-Scholes, barrier closed form, geometric Asian, quadrature references, finite differences, naive barrier, tolerance rule, SE ceiling, K count, reference drift, Philox, inverse normal, N-thread
- [ ] One fault per check: ASan, UBSan, TSan, clang-tidy, clang-format, `-Wall`, `-Wextra`, `-Werror`, each fuzz target, data checksum, missing capture, actionlint, `scripts/check.sh` itself, results-header refusal, host-mixing refusal, attribution on output
- [ ] Results file with baseline and mutated outcome for every fault, on `mac-m4max`, and on `linux-zen1` for what runs there

### Fuzzing

- [x] libFuzzer target for the TAIFEX CSV parser, including cp950 decoding
- [x] libFuzzer targets for every other parser and decoder: chain filters, config, any results reader

### Checks and CI

- [x] ASan and UBSan with `-fno-sanitize-recover=all`. TSan for all concurrent code that exists so far, on the Mac. Concurrent tapes are checked in Phase 3, under the multi-threaded tape item
- [x] `.clang-tidy` checked in, clang-format pinned. `scripts/run_clang_tidy.sh release fuzz` reports 78 units clean on the Mac and on Linux
- [x] `.github/workflows/ci.yml`: two compilers, Linux and macOS, Debug and Release, QuantLib optional
- [ ] CI compile options for tape layout, and Metal off on non-Apple. Neither is in `ci.yml`. Both depend on that work, so they belong in Phase 3, with the tape and Metal items
- [x] Validate CI locally with actionlint from repo tooling, and by running its commands on the Mac and Linux. The Linux sanitizer and fuzz-smoke jobs have not run anywhere. Recorded plainly in `NOTES.md` that it has never executed on GitHub
- [x] `scripts/compare_measured.py` compares the measured plan with `references/measured_results.csv`. All 90 rows match on the Mac, Linux clang, g++ 14.2 and AVX2
- [x] The two tests that depended on the caller's nice value are fixed (9b746bb)
- [x] Linux run at 89dbee8: clang release and debug, g++ 14.2 release and debug, AVX2 release, 364 of 364 each, 0 skipped. AVX-512 built, not run. Linux clang-tidy clean
- [x] libstdc++ path for the data area compiled and run: g++ 14.2 release and debug at 89dbee8, with the real-file tests run, not skipped
- [x] `scripts/scan_repo.py` scans tracked files for private content and raw data rows. Clean on the Mac and Linux
- [x] `scripts/verify_commits.sh` builds and tests each commit of a range from an export, on the `release` preset. Proved on three commits, 2490bd0 to 556a6c8
- [ ] Full-range `verify_commits` run
- [x] README for this phase: status, build, checks, data, CI. The results table waits for Phase 4
- [ ] `scripts/check.sh` runs every check

### Data

- [x] `scripts/fetch_data.*` downloads the TAIFEX option and futures captures and verifies sha256. Descriptive User-Agent, as Internet Archive's developer guide requires. Data goes to gitignored `data/`, on both hosts
- [x] `NOTICE` with the exact attribution text for datasets 11320 and 11319. Failing attribution voids the licence ab initio, and this covers derivative outputs
- [x] Every generated output that uses the chain carries the attribution: one helper, tested, used by every writer
- [x] If a capture disappears, the script says so and stops. There is no fallback source
- [x] Fields are named for what they are: settlements, never quotes or mids

## Phase 3: Implementation

### Tape

- [ ] Tape bytes per path recorded by the harness, beside peak RSS
- [ ] Each benchmark verifies its gradient inside the run (B33)
- [ ] Tangent-adjoint dot-product test (B20)
- [ ] Pricer entry-point fuzzer under sanitizers (B23)
- [ ] Tape operation-sequence fuzzer against finite differences (B23, exceeds)
- [ ] Struct-of-arrays tape: statement and operand arrays, chunked, 16-byte active type, sizes asserted
- [ ] Thread-local tape with RAII scope, mark and rewind per path, no allocation after warm-up
- [ ] Surface and other set-up stay outside the per-path tape, swept once from a mark to the start. Per-path adjoint of shared set-up breaks the constant multiple
- [ ] Hand-coded pathwise adjoint for the GBM Asian, as the `AAD-5` baseline
- [ ] One-node-per-operation tape first. Then evaluate expression templates against `AAD-2`
- [ ] Vector values: one tape structure shared across lanes, with per-lane partials and a vector adjoint. Requires branch-free kernels, which the smoothed and bridge treatments give. Fall back to the scalar tape where lanes diverge. Measure both, and show assembly for any claim (B15)
- [ ] Multi-threaded: one tape per worker, gradient accumulators merged in fixed block order. TSan clean

### Pricer

- [ ] Own `exp`, `log` and inverse-normal kernels, so libm differences do not reach results
- [ ] Arithmetic Asian, geometric Asian
- [ ] Continuous barrier: log-space Brownian-bridge survival weight
- [ ] Discrete barrier: one-step survival. Never apply the bridge to a discretely monitored contract: at H=97, 3.060 continuous against 3.834 discrete
- [ ] Hard-indicator mode, for naive failure and for QuantLib rows R5 and R7
- [ ] Knock-ins through parity. Digitals through last-step conditional expectation
- [ ] Documented smoothing mode, for comparison with `AAD-1`, with its bias measured
- [ ] State and measure the bias of each treatment. Bridge under Euler is first order, with payoff conditions
- [ ] Barrier gamma through one-step survival, because bridge first derivatives are not Lipschitz
- [ ] SIMD paths, kernels instantiated from 1 to N lanes. Fixed block order for reduction
- [ ] Philox4x32-10 stream keyed on (path, step). Evaluate xoshiro256++ as CPU-only option with fixed block-to-stream assignment
- [ ] Inverse normal: AS241 with masked tails, Giles as fallback if vector divergence dominates
- [ ] Scalar and SIMD agreement test, on every lane width that can run. Moved from Phase 2: it needs real SIMD kernels
- [ ] Philox4x32-10 known-answer test on SIMD lanes and Metal. Moved from Phase 2 for the same reason
- [ ] Sobol with Joe-Kuo directions and Brownian-bridge construction, as an evaluated option. SE is meaningless on Sobol, so oracles stay on pseudo-random

### Bump and revalue

- [ ] Common random numbers, same path count, same optimisation level
- [ ] Bump size chosen by experiment, stated in results. With treatments, h is limited by round-off, so check by halving. The n^(−1/5) rule applies only to naive differences
- [ ] Report the AAD multiple beside tape bytes per path and peak RSS
- [ ] Report accuracy beside wall time: AAD without QMC is not guaranteed faster than differences at fixed accuracy for few Greeks

### Second-order Greeks

- [ ] Forward-over-reverse gamma, vanna, volga. Separate cost, never in the headline multiple

### SVI

- [ ] Vendor nothing. Let's Be Rational through pinned FetchContent, with its notice preserved; note the host only serves plain http, so the hash is what protects it
- [ ] TAIFEX parser, filters and parity regression from `NOTES.md`, "Recommended calibration set"
- [ ] Let's Be Rational inversion, accuracy and speed
- [ ] Raw SVI per slice: quasi-explicit inner solve, multi-start, parity-noise weights
- [ ] Handle rho sticking at its bound: an active bound breaks a plain implicit-function adjoint
- [ ] Butterfly: g(k) on a grid, wing conditions at 2, Lee moment formula, Martini-Mingone classifier
- [ ] Calendar: numerical crossing check between slices
- [ ] Tests on known violators: Vogt, the slope 2.25 counterexample, a constructed crossing pair
- [ ] Report input butterfly counts beside fit violations. Do not repair silently
- [ ] Report per-slice implied-vol RMSE by the `SVI-11` formula, unweighted, beside the weighted objective
- [ ] eSSVI as an optional arbitrage-free comparison, reporting what it costs in fit error
- [ ] Time interpolation: Gatheral-Jacquier Lemma 5.1, or eSSVI scheme
- [ ] Dupire local variance as ∂T w / g, on the tape. Count any floored points and report them
- [ ] Price Asian and barrier off the surface. Maturity is limited to 33 days by the usable chain, and this is stated
- [ ] Optional second run on 202501 and 202503, labelled as exchange-set marks in every output
- [ ] Market-quote sensitivities through the implicit function theorem. Disclose any Gauss-Newton approximation

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
- [ ] Reported numbers through `measure.sh` only
- [ ] Run all benchmarks on `linux-zen1` once it is ready

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
