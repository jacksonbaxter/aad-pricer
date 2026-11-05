# aad-pricer

A C++23 Monte Carlo pricer for path-dependent options, with its own tape-based adjoint algorithmic differentiation (AAD) that computes all Greeks in one backward pass. The plan is to benchmark AAD against bump-and-revalue, calibrate an SVI volatility surface to a real option chain and price off it, and compare speed and accuracy with QuantLib on the same instruments.

## Status

Phase 3, the implementation, is under way. Stages 1a to 3b of twelve are done.

The test and measurement harness was built first, in Phase 2, so that the oracles were proved before the code they judge existed. What is here today:

- A platform layer for arm64 and x86-64: CPU information, timers, pinning, performance counters, floating-point state.
- Google Benchmark and HdrHistogram harness code, and a results-file writer that stamps each run with its host, compiler and flags.
- Own `exp`, `log` and AS241 inverse normal in `src/math/`, so libm differences do not reach results.
- The production scalar pricer in `src/pricer/`: arithmetic and geometric Asian, European, digitals, and barriers with a Brownian-bridge weight, one-step survival, a hard indicator or smoothing, on Philox draws keyed on path and step, multi-threaded with a fixed block order.
- The tape in `src/ad/`: struct of arrays, one statement per operation, one tape per worker thread, with no allocation after warm-up. A hand-written adjoint of the GBM Asian is the baseline it is measured against.
- Bump and revalue on common random numbers, with step sizes from a committed h study, and an AAD benchmark that checks every gradient inside the run before timing it.
- Closed-form and finite-difference oracles, with the tolerance rule implemented once and every comparison fixed in `references/comparison_plan.csv`. K is 147: 90 rows ran in Phase 2, and all 147 run on the production pricer and tape at stage 8.
- cp950 and TAIFEX file readers, and libFuzzer targets on every parser, decoder and pricer entry point, plus a tape fuzzer against finite differences.
- A fault-injection runner that plants one bug at a time in a copy of the code, to show each oracle and check can fail.

The first AAD and bump-and-revalue results are in `results/mac-m4max/` and `results/linux-zen1/`. They are scalar: SIMD comes at stage 5. Second-order Greeks, SIMD, the SVI fit, the GPU pricer and the QuantLib comparison are still to come. `TASKS.md` has the plan, `BENCHMARKS.md` the published targets with what has been measured against them, and `NOTES.md` the findings and decisions.

## Build and run

You need CMake 3.25 or later, Ninja, Python 3 and a C++23 compiler. The toolchain comes from the environment (`CC`, `CXX`, and `SDKROOT` on macOS). `clang-format` and `clang-tidy` must be LLVM 23.1.4, and the scripts refuse any other version. Third-party dependencies are pinned archives, fetched into the build tree with their hashes checked. QuantLib is off unless `AAD_PRICER_BUILD_QUANTLIB_COMPARE` is set.

```sh
cmake --preset release
cmake --build --preset release
ctest --preset release
```

| Preset | Purpose |
|---|---|
| `release` | Optimised, baseline SIMD |
| `debug` | No optimisation |
| `asan-ubsan` | ASan and UBSan |
| `tsan` | TSan |
| `fuzz` | libFuzzer targets with ASan and UBSan |
| `release-gcc` | Optimised, with g++, Linux only |
| `release-avx2` | Optimised, AVX2 with FMA, x86-64 only |
| `release-avx512` | Optimised, AVX-512, x86-64 only. Build only: it has no test preset |

Fetch the data before running the real-file tests. Without it they skip, and `scripts/check.sh` counts a skipped test as a failure.

```sh
scripts/fetch_data.sh            # download the pinned captures and verify their sha256
scripts/fetch_data.sh --verify   # verify what is on disk, with no network
```

Run every check, one after another:

```sh
scripts/check.sh
```

Each step prints one PASS, FAIL or SKIP line and writes a log under `build/logs/check/`. There are 13 steps: the nice level, format, clang-tidy, the repo scan, build and ctest for `release`, `debug`, `asan-ubsan` and `tsan`, a timed run of every fuzz target, the measured oracle plan compared with `references/measured_results.csv`, data verification, actionlint, and fault injection last. The fault-injection step runs against the committed `HEAD`, so uncommitted edits do not reach it. Every step runs even after a failure. `AADP_JOBS` sets the jobs, at most 6 and by default 6, and `AADP_FUZZ_SECONDS` the seconds per fuzz target, by default 60. A skipped test or a step that could not run is a failure unless `--allow-skipped-tests` or `--allow-skip-step` names it. `--only <step,...>` runs part of the list and always exits non-zero, so a partial run can never read as a pass.

Other scripts:

- `scripts/fetch_tools.sh` installs the pinned actionlint into `build/tools/`, checked by sha256. With `--llvm` it also installs LLVM 23.1.4, which is how CI gets its compiler.
- `python3 -I scripts/scan_repo.py` fails if a tracked file holds a home path, an IP address, the current user or host name, an agent notes file, or a raw line of the market data. The exceptions are listed at the top of the script.
- `scripts/verify_commits.sh <range>` builds and tests each commit of a range in an export under `build/commit-check/`. It never checks out a commit.

## Fault injection

A check that cannot fail proves nothing. `scripts/fault_injection.py` plants one bug at a time, from `faults/manifest.json`, and shows that the oracle or check aimed at it fails for the stated reason. There are 100 faults: 23 against the oracles, 64 against the checks and 13 against the fuzz targets. Each is applied to a copy under `build/fault`, never to the working tree. The unmutated copy must pass first, and the mutated run must fail with output matching the fault's reason. Failing for some other reason, such as a compile error where a test failure was expected, counts as not caught.

```sh
python3 -I scripts/fault_injection.py --list                 # print the faults
python3 -I scripts/fault_injection.py --only oracle-philox   # run one fault
python3 -I scripts/fault_injection.py --jobs 6 --output <file>   # run all of them
```

It needs the toolchain in the environment, as the build does. `--kind` runs one kind, and `--source <dir>` mutates an export with no `.git`. The schema and the verdicts are described at the top of the script, and the runner's own tests are in `faults/test_fault_injection.py`. `scripts/check.sh` runs every fault as its last step.

Reports are in `results/<host>/fault-injection-<time>.txt`, with the commit, a scope line and one line per fault. Those committed reports are from Phase 2, when there were 41 faults. On the Mac all 41 were caught. On the Linux host 29 ran and all 29 were caught. The 9 fuzz faults and 3 sanitizer faults were left out there, because sanitizer and fuzz runs stay on the Mac. The latest full `scripts/check.sh` run, at 54933ed on the Mac, caught all 100. What the faults found is in `NOTES.md`, under "Fault injection results".

## Results

Results tables come only from `aadp_results_table`, which reads the raw results files, and are never typed by hand:

```sh
build/release/bin/aadp_results_table results/
```

No file holds numbers from two hosts, and the tool refuses a file filed under another host's directory. It leaves out a fault-injection report in a host directory, judged by the report's first line. `results/` now holds the stage 3b AAD and bump-and-revalue runs at d1596e0, beside harness self-tests and fault-injection reports. This README has no results table yet. It will be generated by `aadp_results_table` once the benchmarks are complete, never typed.

## Data

The option chain is TAIFEX TXO, European options on the TAIEX, with the matching futures file. Read this before using anything derived from it:

- **These are exchange settlement prices, not tradable quotes.** TAIFEX's rule is that settlement is the last trade, and if there was no trade in the final 15 minutes, TAIFEX decides it. At least 73.7% of all regular-session rows and 52.0% of out-of-the-money rows are exchange-set, and for the 202506 expiry it is 100%. The file has no trade times, so "market-observed" is only an upper bound.
- **It covers one trade date, 2024-11-15.** No stability test is possible.
- **The usable surface reaches only 33 days.** The market-anchored expiries are 5, 12 and 33 days out, so pricing off it is limited to short maturities. Anything longer-dated would be fitted to exchange-set marks and labelled that way.

What remains doubtful, none of it resolved in the data's favour:

1. The licence chain is reasonable, not express: Securities and Futures Bureau, then TAIFEX's authority, then the licence. The listed provider is the Bureau, not TAIFEX.
2. TAIFEX's own terms are ambiguous. Clause 3 exempts data it authorised data.gov.tw to offer, but its next sentence asks for prior written consent to quote or reprint, and the English terms have no exemption at all. The raw file is kept unmodified and the source is cited. This is a reading, not legal advice.
3. Internet Archive's Terms of Use could not be read, because the page is a JavaScript shell. Its developer guide requires a descriptive User-Agent on automated requests, which the fetch script sends. The `id_` URL modifier the pinned links use is observed behaviour, not documented.
4. There is a single point of failure. There is one capture, and if it goes, calibration cannot run until a mirror or a new snapshot is chosen. The fetch script has no fallback source on purpose.
5. The licence covers the open file only. Venue POST quotes were a diagnostic and must not feed calibration.
6. One date only, as above.

The raw files are never committed. `scripts/fetch_data.sh` downloads the Internet Archive captures named in `scripts/data_manifest.txt` and verifies their sha256. `NOTES.md` has the full analysis under "Option chain". Attribution for the data and the third-party build dependencies is in [`NOTICE`](NOTICE).

## CI

`.github/workflows/ci.yml` runs on GitHub. Its first run, at bc3e6d3, failed 9 of 17 jobs: actionlint, clang-tidy, the two GCC jobs, Linux TSan and the four macOS jobs. The fixes, and a second round after run 41884006533, are pushed, and CI has confirmed them: the last green run is 41902764987, at 9459507, with all 22 jobs passing. `NOTES.md` has the causes. The workflow covers Linux and macOS, clang and, on Linux, gcc, Debug and Release, ASan with UBSan, TSan, clang-tidy on libc++ and on libstdc++, format, a fuzz smoke, the repo scan, AVX2 and AVX-512 builds, and a SIMD lane matrix at 1, 2, 4 and 8 lanes. The 8-lane job always runs its tests: natively on a runner with every x86-64-v4 feature, and otherwise under Intel SDE as a Skylake-X (`-skx`), so a green run has tested 8 lanes either way. AVX-512 is build-only on the two measurement hosts, which cannot run it.
