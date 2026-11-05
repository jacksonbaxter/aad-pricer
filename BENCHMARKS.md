# BENCHMARKS

Targets for aad-pricer, and the evidence behind each one. Every target below is a published figure, or arithmetic on one. A line starting "Measured" under a target gives this project's number from the files in `results/`, added in Phase 3. It never changes the target.

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

`Lead check` records that the session lead opened the source personally, not only the subagent. `done` means the URL was re-downloaded and the quoted text was found (`verify/verify_sources.log`, `verify/rerun/`), or, for scans and figures, that the value was read from the page image. Derived values marked `done` were recomputed from their scripts, with output in `verify/rerun/`.

Hosts are never mixed. `mac-m4max` is Apple M4 Max, macOS, unpinned, 24 MHz counter. `linux-zen1` is the `debian` host, AMD Ryzen Embedded V1605B (Zen 1), pinned with `taskset -c 4`. Pinned headline numbers come from `linux-zen1`. Wall-time ratios are valid on `mac-m4max` if the results header says so. Metal numbers come only from `mac-m4max`. Published server-class figures will beat this embedded chip, and any such gap is reported with the hardware difference as the reason. No target is shrunk to hide it.

## 1. AAD cost

**The multiple.** AAD multiple = (wall time for value plus all first-order Greeks) / (wall time for one pricing), same binary, same flags, same path count, same random numbers, single thread unless stated. It is reported twice, against the scalar primal and against the fastest primal (SIMD), because a slower primal flatters the ratio: the same code in `AAD-6`'s source moves from 1.27 to 1.90 between compilers. Record and reverse times are reported separately.

**What one reverse sweep produces.** Delta, vega (flat-vol case), rho, dividend rho, theta, and the sensitivity to every SVI parameter of every slice. Gamma, vanna and volga do not come from it. They take a second pass, are reported as a separate multiple (`AAD-8`), and never enter the headline.

### AAD-1: barrier, general tape, single thread: multiple below 3.0

- Source: Savine, *Modern Computational Finance*, Wiley 2018, printed p.424, PDF p.452. https://antoinesavine.com/wp-content/uploads/2021/05/modern-computational-finance.pdf
- Quote: "Our AAD implementation produces the entire risk in around 8 seconds (8,250 ms), less than three times a single valuation."
- Source measured: iMac Pro, Xeon W-2140B, 3.20 GHz, Windows 10, VS2017 release | 3y European call K=120 with 150 barrier, Dupire local vol on 60 times and 30 spots, 500,000 paths, 156 weekly steps, Sobol, 1,801 sensitivities (1,081 active), barrier smoothing of 5 | single thread | wall time, general operator-overloading tape wiped per path. "a pricing took around 3 seconds", so the implied multiple is about 2.75 (derived, 8,250 / 3,000, approximate).
- Class: `DIRECT`. Closest match found in every dimension, on different hardware, and on a smoothed barrier whose bias the source does not state. Lead check: done.
- Measured at d1596e0, against the scalar primal: `linux-zen1` 1.89, `mac-m4max` 2.74. Met, by 1.11 and 0.26. Not like-for-like: other hardware, and GBM at a flat vol with Philox draws and 5 inputs, not Dupire local vol with Sobol and 1,801 sensitivities. The multiple against the fastest primal waits for stage 5. Files: `results/linux-zen1/aad1-barrier-20261104T071749Z.txt`, `results/mac-m4max/aad1-barrier-20261104T071059Z.txt`.

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
- Measured at d1596e0: `linux-zen1` 1.89 on 2 threads pinned to CPUs 4 and 6, `mac-m4max` 2.68 on 8 threads, unpinned. Both meet 2.86, by 0.97 and 0.18. Both miss the stretch 1.64, by 0.25 and 1.04. Not like-for-like: the source ran 8 cores on other hardware, and the workload differs as in `AAD-1`. Files: `results/linux-zen1/aad3-barrier-mt-20261104T080704Z.txt`, `results/mac-m4max/aad3-barrier-mt-20261104T073414Z.txt`.

### AAD-4: arithmetic Asian: multiple 2.8

- Source: Capriotti, "Fast Greeks by Algorithmic Differentiation", J. Computational Finance 14(3), 2011, section 5.1.2, printed p.33, with Figure 9(a) on printed p.32, annotated ~2.8. http://luca-capriotti.net/pdfs/Finance/jcf_capriotti_press_web.pdf
- Quote: "even for this more complicated example the extra overhead associated with the calculation of the risk is limited to approximately 180% of the cost of calculating the option value."
- Source measured: hardware not stated | "best of" Asian, 12 observation dates, up to 20 assets, local vol, Euler; paths not stated | threads not stated | CPU ratio, delta and vega. This is a pathwise method with an adjoint payout, source-transformed by Tapenade. It is not a full adjoint of the path.
- Class: `UNCONFIRMED` as a target for a full tape. It is the only published multiple found for an Asian on a local-vol path, and it is a partial adjoint, so holding a full tape to it is deliberately hard. The one published full-tape run on our workload gives 5.3 (`MC-2`, derived). Lead check: done, quote and figure read from the page images.
- Measured at d1596e0, 12 fixings: `linux-zen1` 1.91, `mac-m4max` 2.67. Met, by 0.89 and 0.13. At 252 fixings, `mc-asian252`: 1.83 and 2.74. Not like-for-like: our full tape of a one-asset GBM path, against Capriotti's partial adjoint of a best-of in local vol on unstated hardware. Files: `results/linux-zen1/aad4-asian12-20261104T085349Z.txt`, `results/mac-m4max/aad4-asian12-20261104T074006Z.txt`, `results/linux-zen1/mc-asian252-20261104T090220Z.txt`, `results/mac-m4max/mc-asian252-20261104T074520Z.txt`.

### AAD-5: within 1.25 times a hand-written adjoint

- Source: Hogan, ACM TOMS 40(4), 2014, Table I discussion, p.26:12. https://www.met.reading.ac.uk/~swrhgnrj/publications/adept.pdf
- Quote: "Adept is 3.7–3.9 times the computational expense of the original algorithm, and 10–25% more computationally expensive than the hand-written adjoint codes."
- Source measured: 2.5 GHz Intel E5200 Pentium, g++ 4.3.2 -O3 -DNDEBUG | advection and radiative-transfer kernels, repeated with memory reuse | single thread by implication | adjoint time including the forward pass, over the primal.
- Class: `CONTEXT`. Not Monte Carlo, but the same-code comparison of tape and hand-written adjoint is exactly the question. We write a hand-coded pathwise adjoint for the GBM Asian as the baseline. Lead check: done.
- Measured at d1596e0, tape time over hand-adjoint time: `linux-zen1` 1.87 at 12 fixings and 1.82 at 252, `mac-m4max` 2.57 and 2.68. Missed, by 0.57 to 0.62 and 1.32 to 1.43. The hand adjoint skips the reverse pass on the paths out of the money, about 48%, and the tape does not; `tape-skip` over hand is still 1.77 to 1.80 and 2.30 to 2.38. Not like-for-like: Hogan's kernels are not Monte Carlo, on other hardware. Files: `results/linux-zen1/aad4-asian12-20261104T085349Z.txt`, `results/mac-m4max/aad4-asian12-20261104T074006Z.txt`, `results/linux-zen1/mc-asian252-20261104T090220Z.txt`, `results/mac-m4max/mc-asian252-20261104T074520Z.txt`.

### AAD-6: hand-written baseline: multiple below 2.0

- Source: Giles, Oxford report NA-07-12, Table 2, p.13. https://people.maths.ox.ac.uk/gilesm/files/NA-07-12.pdf
- Quote: "the most efficient code is the hand-coded reverse mode; the timings are similar to those reported in [12] with the valuation plus 80 sensitivities being obtained at a cost which is less than twice the cost of calculating the valuation on its own."
- Source measured: single Pentium 4, g++ and icc at highest optimisation | LMM, 40 time steps, 80 forward rates | one CPU | ms per path. Hand 0.47/0.37 = 1.27 on g++, 0.19/0.10 = 1.90 on icc (derived). FADBAD++ tape 16.8 and 48.6 (derived).
- Class: `CONTEXT`. Applies to our hand-coded baseline, not to the tape. Lead check: done.
- Measured at d1596e0, hand adjoint over the scalar primal: `linux-zen1` 1.02 at 12 fixings and 1.00 at 252, `mac-m4max` 1.04 and 1.02. Met, by 0.96 to 1.00. Not like-for-like: Giles's LMM with 80 sensitivities on a Pentium 4; ours has 5 inputs and skips the reverse pass out of the money. Files: `results/linux-zen1/aad4-asian12-20261104T085349Z.txt`, `results/mac-m4max/aad4-asian12-20261104T074006Z.txt`, `results/linux-zen1/mc-asian252-20261104T090220Z.txt`, `results/mac-m4max/mc-asian252-20261104T074520Z.txt`.

### AAD-7: bump and revalue cost

- Source: Capriotti, Jiang, Macrina, 2017, section 4.2, p.16. https://discovery.ucl.ac.uk/id/eprint/1523595/1/Macrina_AADMC_final.pdf
- Quote: "the cost of bumping, for one-sided finite-difference estimators, is in general (1 + N) times the cost of as single valuation, where N is the number of model parameters"
- Target: the measured bump time matches (1 + N), or 2N + 1 for central differences (derived), within measurement noise, and AAD's speedup is reported as bump time over AAD time at equal path count, common random numbers and the same optimisation level. Bump size is stated and chosen by experiment.
- Class: `STRUCTURAL` formula. Lead check: done.
- Measured at d1596e0 on `aad1-barrier`, N = 5: central bumps cost 10.96 pricings and forward 5.99 on `linux-zen1`, 11.01 and 6.02 on `mac-m4max`, against 11 and 6. Met. Across the five pricer workloads the range is 10.75 to 11.06 and 5.84 to 6.17. AAD's speedup, bump time over tape time at equal paths on common random numbers, is 5.81 over central and 3.18 over forward on `linux-zen1`, 4.01 and 2.20 on `mac-m4max`. Each file states the step and its bias per Greek. Files: `results/linux-zen1/aad1-barrier-20261104T071749Z.txt`, `results/mac-m4max/aad1-barrier-20261104T071059Z.txt`.

### AAD-8: second-order Greeks: cost reported separately

- Target: gamma, vanna and volga by forward-over-reverse, with their own multiple. No numeric target.
- Class: `UNCONFIRMED`. No published forward-over-reverse timing on a Monte Carlo barrier or Asian was found. See section 12.
- Proposed rows, not targets (fb97650): `aad8-asian12`, on `AAD-4`'s contract, and `aad8-barrier`, on `AAD-1`'s contract under one-step survival. Savine's smoothing has no unbiased second derivative, so it is not used there. Second order runs on the conditional kernels, so each row's multiples are against the first-order tape on those kernels and against their primal. Neither enters `AAD-1` or `AAD-4`. No measured run yet. Iteration figures, macOS only, are in `NOTES.md`, "Second-order Greeks".

### Theory, not targets

The bound is on operation counts, not wall time. Griewank gives "the complexity ratio is exactly ω = 3" for polynomial multiplication counts (https://papers.baulab.info/papers/also/Griewank-2012.pdf, PDF p.8). Capriotti gives Cost[F & J] / Cost[F] ≤ ω_A m with ω_A ∈ [3, 4] (eq. 3.5, printed p.10, PDF p.8, read from the page image, citing Griewank 2000). These are different models and different numerators, and the textbook that would map constants to models (Griewank & Walther) could not be opened, so no single number is "the" bound. Real tapes exceed it: dco/c++ "between 5 and 15" (https://support.nag.com/doc/techrep/pdf/tr3_14.pdf, section 4.1).

### How Phase 3 numbers are produced

One binary, `aadp_bench_aad` (`bench/aad_bench.cpp`), one results file per workload. Each first-order workload times seven variants as Google Benchmarks named `<workload>.<variant>`: `price` (the scalar primal), `tape` (value and all Greeks), `tape-skip` (the same with the zero-adjoint skip, B9), `tape-record` (record and rewind only, so reverse = tape - tape-record), `hand` (hand-written adjoint, where one exists), `bump-central` (2N + 1 pricings) and `bump-forward` (N + 1). One timed batch is one run over all the workload's paths.

The two second-order workloads (stage 4, fb97650) time `price`, `tape` and four more, so the bench has 11 variants: `cond-price` (the primal of the conditional kernels, which take the last step's expectation in closed form), `cond-tape` (value and five Greeks on them, one reverse sweep), `for-spot` (forward over reverse with one tangent lane: gamma and vanna too) and `for-spot-vol` (two lanes: volga too). Each is one record and one sweep per path.

Stage 5c (8e8a464) adds two lane variants on the five first-order pricer workloads, so the bench has 13: `price-lanes` (`simd::PriceLanes`, the scalar primal's bits, at the build's widest lanes) and `tape-lanes` (value and five Greeks on the vector tape, the tape's bits, at the same lanes). The "x fastest primal" column divides by the faster of `price` and `price-lanes`; the second-order workloads and `lw-worst` have no lane form, and their files say so. Every lane speedup is printed beside the lane ceiling, which on Zen 1 also says "Zen 1: 128-bit FP datapath". Two more in-run checks: `lanes-gradient-bits`, the vector tape's value and five Greeks against the tape's, bit for bit, and `lanes-fallback-zero`, which makes the run INVALID if any group fell back to the scalar tape.

Iteration only, not results (1/50 scale, 3 repetitions, at 1078a20, ranges over the five workloads, no group fell back): `tape-lanes` x scalar primal 1.32 to 1.42 and x fastest primal 1.78 to 1.86 on `mac-m4max`, macOS, NEON, ceiling 2 lanes; 1.01 to 1.11 and 2.00 to 2.23 on `linux-zen1`, AVX2, ceiling 4 lanes (Zen 1: 128-bit FP datapath). No measured run yet. Raw files in the research cache, `phase3/evidence/stage5c/`.

| Workload | Targets | Setup |
|---|---|---|
| `aad1-barrier` | AAD-1, AAD-7 | up-and-out call, smoothed discrete barrier, 156 dates, 500,000 paths, 1 thread |
| `aad3-barrier-mt` | AAD-3 | the same on `--aadp-threads` worker threads |
| `aad4-asian12` | AAD-4, AAD-5, AAD-6 | arithmetic Asian call, 12 fixings, 300,000 paths, 1 thread |
| `mc-asian252` | MC-1, MC-2, AAD-5, AAD-6 | the same at 252 fixings |
| `mc3-asian252-mt` | MC-3 | `mc-asian252` on `--aadp-threads` worker threads |
| `lw-worst` | Hogan's Lax-Wendroff (B34) | 100 points, 2000 steps, c = 0.5, 100 inputs |
| `aad8-asian12` | AAD-8, proposed | `aad4-asian12`'s contract and paths, second order on the conditional kernels |
| `aad8-barrier` | AAD-8, proposed | `aad1-barrier`'s contract under one-step survival, not smoothing, 500,000 paths |

Each workload's section and the header state the rest: differences from the target, seed 20261103, 50 batches, threads and their setup, the instruction set (Highway's target and lane count for the lane variants, scalar C++ for the rest), the tape configuration, tape bytes per path, peak RSS per variant (a fresh process running that variant alone), and the nonzero-payoff fraction.

Before anything is timed, every variant runs once at the benchmark's own size and is checked (`bench/aad_checks.hpp` states each bound): value bits against `Price()`, the skip against the tape, the hand adjoint against the tape, and both bumps against the tape. The second-order workloads have four exact checks instead, and no statistical one, so the statistical family is unchanged: each variant's value against its own kernels' primal, both forward-over-reverse variants' five Greeks against `cond-tape`'s, and the two-lane spot row against the one-lane one, all bit for bit, and vanna both ways per batch within its round-off bound. A failed check writes `valid no` and `gradient_check: FAILED` into the file, the table prints the workload as INVALID with no multiples, and `--aadp-require-valid` exits 9.

Bump steps come from the h study, committed as `bench/bump_size_study.csv` (every halving, its change, round-off scale and verdict) and `bench/aad_steps.hpp` (the chosen step, bias, bias SE and bias bound per Greek). The ctest `aadp_bump_study_committed` checks that one chooses the other. To redo it: `build/release/bin/aadp_bump_study --threads=8 --out=bench/bump_size_study.csv`, then `build/release/bin/aadp_bump_study --steps-header=bench/bump_size_study.csv` into `bench/aad_steps.hpp` and clang-format.

On `mac-m4max`, from a clean checkout of the commit, with the toolchain sourced:

```sh
cmake --preset release && cmake --build --preset release -j 6
python3 -I scripts/run_tests.py ctest release
B=build/release/bin/aadp_bench_aad
scripts/run_bench.sh $B aad1-barrier --aadp-workload=aad1-barrier --aadp-require-valid --batches=5 --repetitions=3
scripts/run_bench.sh $B aad3-barrier-mt --aadp-workload=aad3-barrier-mt --aadp-threads=8 --aadp-require-valid --batches=10 --repetitions=3
scripts/run_bench.sh $B aad4-asian12 --aadp-workload=aad4-asian12 --aadp-require-valid --batches=20 --repetitions=3
scripts/run_bench.sh $B mc-asian252 --aadp-workload=mc-asian252 --aadp-require-valid --batches=5 --repetitions=3
scripts/run_bench.sh $B mc3-asian252-mt --aadp-workload=mc3-asian252-mt --aadp-threads=8 --aadp-require-valid --batches=10 --repetitions=3
scripts/run_bench.sh $B lw-worst --aadp-workload=lw-worst --aadp-require-valid --batches=200 --repetitions=3
build/release/bin/aadp_results_table build/release/results/mac-m4max
```

`run_bench.sh` runs each under `measure.sh`, passes `--reportable`, and stamps the file into `build/release/results/<host>/`. It reaches `results/<host>/` only when copied there after reading. At one batch per run, `aad1-barrier` and `mc-asian252` take about 15 minutes each on `mac-m4max`, mostly in `bump-central`.

On `linux-zen1`, in the export of the same commit, with `$HOME/quant-projects/.toolchain/env.sh` sourced, builds through `$HOME/quant-projects/bin/heavy.sh` at `-j 2`, and each run detached with its output in a file on the host:

```sh
$HOME/quant-projects/bin/heavy.sh cmake --preset release
$HOME/quant-projects/bin/heavy.sh cmake --build --preset release -j 2
python3 -I scripts/run_tests.py ctest release
B=build/release/bin/aadp_bench_aad
scripts/run_bench.sh $B aad1-barrier --aadp-workload=aad1-barrier --aadp-require-valid --batches=5 --repetitions=3
scripts/run_bench.sh $B aad4-asian12 --aadp-workload=aad4-asian12 --aadp-require-valid --batches=20 --repetitions=3
scripts/run_bench.sh $B mc-asian252 --aadp-workload=mc-asian252 --aadp-require-valid --batches=5 --repetitions=3
scripts/run_bench.sh $B lw-worst --aadp-workload=lw-worst --aadp-require-valid --batches=200 --repetitions=3
AADP_PIN_CPUS=4,6 scripts/run_bench.sh $B aad3-barrier-mt --aadp-workload=aad3-barrier-mt --aadp-threads=2 --aadp-require-valid --batches=10 --repetitions=3
AADP_PIN_CPUS=4,6 scripts/run_bench.sh $B mc3-asian252-mt --aadp-workload=mc3-asian252-mt --aadp-threads=2 --aadp-require-valid --batches=10 --repetitions=3
build/release/bin/aadp_results_table build/release/results/linux-zen1
```

There `run_bench.sh` adds `taskset -c 4`, or `taskset -c 4,6` with `AADP_PIN_CPUS=4,6` for the two threaded workloads (bc3e6d3). cpu5 is recorded as meant to be idle, which nothing enforces. The stamp checks each pinned CPU against `measure.sh`'s list.

An iteration run, which is never a result: `AADP_NOT_REPORTABLE="iteration" scripts/run_bench.sh $B aad-iteration --aadp-scale=50 --batches=1 --repetitions=3`, rendered with `aadp_results_table --iteration <file>`, which prints it under an ITERATION ONLY banner.

## 2. Tape memory

Tape memory is the main practical limit of AAD, so every AAD multiple is reported beside tape bytes per path and peak RSS.

### MEM-1: active type 16 bytes, statement at most 8 bytes, operand at most 12 bytes

- Source: Adept at commit d0a7751, `Statement.h:28-29`, `Active.h:559-560`; CoDiPack at c25d5c1, `jacobianBaseTape.hpp:105-110`. https://github.com/rjhogan/Adept
- Class: `STRUCTURAL`, read from source, not measured. The 16 and 8 bytes assume Adept's default 8-byte `double`, 4-byte index and alignment padding. Asserted with `static_assert`. Lead check: done, lines read at those commits.
- Measured: met on both hosts. `Real` 16 B, statement 1 B, operand 12 B, by `static_assert` in `src/ad/tape.hpp`. Every results file at d1596e0 records it in `tape_config`. Files: `results/linux-zen1/aad1-barrier-20261104T071749Z.txt`, `results/mac-m4max/aad1-barrier-20261104T071059Z.txt`.

### MEM-2: 12k + 1 bytes per statement with linear indexing

- Source: Sagebaum, Albring, Gauger, arXiv 1709.07229, p.7. https://arxiv.org/pdf/1709.07229
- Quote: "k ∗ 8 + k ∗ 4 + 1 = 12 ∗ k + 1 for each statement."
- Class: `STRUCTURAL`. Preprint; the ACM TOMS 2019 version was not opened. Lead check: done.
- Measured at d1596e0: met exactly, on every workload and both hosts. Tape bytes per path are statements plus 12 times operands: 1,078 + 12 × 1,393 = 17,794 B on `aad1-barrier`, 63 + 12 × 98 = 1,239 B on `aad4-asian12`, 1,263 + 12 × 2,018 = 25,479 B on `mc-asian252`. Files: `results/linux-zen1/aad1-barrier-20261104T071749Z.txt`, `results/mac-m4max/aad1-barrier-20261104T071059Z.txt`, `results/linux-zen1/aad4-asian12-20261104T085349Z.txt`, `results/mac-m4max/aad4-asian12-20261104T074006Z.txt`, `results/linux-zen1/mc-asian252-20261104T090220Z.txt`, `results/mac-m4max/mc-asian252-20261104T074520Z.txt`.
- Lane tape, stages 5b and 5c, not a results file. A group of N lanes records each statement once, 1 B, and each operand once, a 4-byte index, a 1-byte lane mask and N 8-byte partials: 5 + 8N bytes, so 21, 37 and 69 B at 2, 4 and 8 lanes, and 12 B at one lane, which needs no mask. Tape bytes per path are the group's bytes over N. From the bench's iteration runs at 1078a20, against the scalar tape's 17,794, 1,239 and 25,479 B: `aad1-barrier` 15,165.5 and 13,506.2 B at 2 and 4 lanes, `aad4-asian12` 1,071.0 and 931.5 B, `mc-asian252` 21,820.5 and 18,991.5 B. From `LaneTapeMemory.BytesPerPathAtEveryWidthBesideTheScalarTape` at 252 steps, at 1, 2 and 4 lanes: European call 15,927, 13,651.5 and 11,881.8 B; arithmetic Asian with spot 25,503, 21,841.5 and 19,000.8 B; down-in bridge 73,472, 62,867.5 and 54,661.8 B. One lane equals the scalar tape. The test passed at 8 lanes in CI under Intel SDE, but its printed figures are not in that log.

### MEM-3: peak memory at most 1.9 MB with a per-path tape, n = 10, and 7.1 MB at n = 222

- Source: Naumann & du Toit, NAG TR 3/14, Table 1, PDF p.15. https://support.nag.com/doc/techrep/pdf/tr3_14.pdf
- Quote: "The naive approach (mc/a1s) without checkpointing yields infeasible memory requirements for gradient sizes n ≥ 22."
- Source measured: reference computer with 3 GB, CPU not stated | European call, local vol, 10,000 paths, 360 steps | single thread by implication | peak memory, table rows `10 ... 1.8s (2GB) 1.3s (1.9MB)` and `222 ... - (> 3GB) 24.4s (7.1MB)`, whole-simulation tape against per-path ensemble. The peak is for the run, not one tape, and it grows with gradient size n.
- Class: `DIRECT`, vendor-affiliated. Compared at the same steps and a stated n. Lead check: done.
- Measured at d1596e0: missed. Peak RSS of the whole process running the tape on one thread is 3,678,208 to 3,899,392 B on `linux-zen1` and 2,392,064 to 2,408,448 B on `mac-m4max`, over 1.9 MB by 1.78 to 2.00 MB and 0.49 to 0.51 MB. An idle process alone peaks at 3,416,064 to 3,596,288 B and 2,080,768 B. The tape's own chunks are 172,032 B. Not like-for-like: the whole process, 5 inputs not n = 10, 12 to 252 steps not 360, GBM not local vol. Files: `results/linux-zen1/aad1-barrier-20261104T071749Z.txt`, `results/mac-m4max/aad1-barrier-20261104T071059Z.txt`, `results/linux-zen1/aad4-asian12-20261104T085349Z.txt`, `results/mac-m4max/aad4-asian12-20261104T074006Z.txt`, `results/linux-zen1/mc-asian252-20261104T090220Z.txt`, `results/mac-m4max/mc-asian252-20261104T074520Z.txt`.

### MEM-4: per-path tape below 50 MB in the worst case

- Source: Savine, section 11.4, printed p.404, PDF p.432 (URL as `AAD-1`).
- Quote: "The size of the tape is related to the processing time of one single path, hence, always below 50 MB and generally substantially less than that."
- Class: `CONTEXT`, a design rule rather than a measurement. Lead check: done.
- Measured at d1596e0: met on both hosts. The largest pricer path, on `mc-asian252`, records 25,479 B of tape and 10,104 B of adjoints. The Lax-Wendroff run, one sweep over 2000 steps, records 29,801,875 B and 11,058,392 B of adjoints in 46,669,824 B of chunks, 3.3 MB under the limit. Files: `results/linux-zen1/mc-asian252-20261104T090220Z.txt`, `results/mac-m4max/mc-asian252-20261104T074520Z.txt`, `results/linux-zen1/lw-worst-20261104T103623Z.txt`, `results/mac-m4max/lw-worst-20261104T081214Z.txt`.

### MEM-5: peak RSS at most 1.09 times the primal

- Source: Naumann, Paris talk, 24.01.2019, slide 8. https://calcul.math.cnrs.fr/attachments/journee-da-2019/naumann.pdf
- Quote: "pathwise AAD by dco/c++ 1.9 86 1.6", against "Primal 1.2 79 1".
- Source measured: hardware not stated | scalar SDE, Euler, 10^4 paths, 10^3 steps, precomputed random numbers, no payoff | threads not stated | MB. Derived: 86 / 79 = 1.09. Whole-simulation tape: 728 MB.
- Class: `DERIVED`, `CONTEXT`. Toy SDE. Lead check: done.
- Measured at d1596e0, peak RSS of the tape over the primal, each in a fresh process. `linux-zen1`: 1.03, 1.02 and 1.04 on one thread (`aad1-barrier`, `aad4-asian12`, `mc-asian252`), met; 1.05 and 1.09 on two threads, where `mc3-asian252-mt` is 1.092, 0.002 over. `mac-m4max`: 1.10, 1.11 and 1.11 on one thread, missed by 0.01 to 0.02; 1.61 and 1.63 on 8 threads, missed. On `linux-zen1` this is near the noise: the idle process peaks between 3,366,912 and 3,596,288 B across the runs, a spread of 229,376 B, and the tape adds 86,016 to 356,352 B, inside that spread except on `mc3-asian252-mt`. Not like-for-like: Naumann's toy SDE with precomputed draws. Files: `results/linux-zen1/aad1-barrier-20261104T071749Z.txt`, `results/mac-m4max/aad1-barrier-20261104T071059Z.txt`, `results/linux-zen1/aad3-barrier-mt-20261104T080704Z.txt`, `results/mac-m4max/aad3-barrier-mt-20261104T073414Z.txt`, `results/linux-zen1/aad4-asian12-20261104T085349Z.txt`, `results/mac-m4max/aad4-asian12-20261104T074006Z.txt`, `results/linux-zen1/mc-asian252-20261104T090220Z.txt`, `results/mac-m4max/mc-asian252-20261104T074520Z.txt`, `results/linux-zen1/mc3-asian252-mt-20261104T095003Z.txt`, `results/mac-m4max/mc3-asian252-mt-20261104T080643Z.txt`.

### MEM-6: no heap allocation after warm-up

- Target: proved by a counting-allocator test. Hogan and Savine reuse memory; asserting it exceeds them.
- Class: `STRUCTURAL`. Bar item B5.
- Measured: met on both hosts, by a test rather than a results file. `TapeAllocation.NoHeapAllocationAfterWarmUp` (`tests/ad/allocation_test.cpp`, a7a2591) counts 0 allocations after warm-up, on every product and treatment at 50 and 252 steps, 4096 paths each. Linux TSan builds leave it out, because TSan's runtime there defines `operator new` (8d00d85).

Sizing context, not a target: "tapes consume around 5 GB of RAM per second per core in the non-instrumented calculation" (Savine, printed p.403).

## 3. Monte Carlo throughput

No source publishes paths/s for a path-dependent, CPU-only workload on Apple Silicon with named hardware, so these are `CONTEXT` throughout. STAC-A2 reports are login-gated and report Greeks elapsed time and options/s, and cannot be converted to paths/s.

### MC-1: arithmetic Asian, 252 fixings, price only: 1.044 million paths/s, single thread

- Source: NablaTensor blog, "How XAD and MatLogica performed". https://nablatensor.com/blog/xad-matlogica-aadc-monte-carlo-benchmark
- Quote: "On the Asian, those figures are 1.044 and 0.197 million."
- Source measured: CPU and compiler unnamed in the post | arithmetic Asian, 252 fixings, K=100, vol 20%, r 3%, T=1, seven spots, 300,000 paths, double | single thread | million paths/s, median of warm calls. Derived: 1.044e6 × 252 = 263 million path-steps/s.
- Build details are not in the post. They come from the author's harness, https://github.com/nablatensor-dev/nablatensor/tree/bench/the-same-ladder-in-cpp: `xad_CMakeLists.txt:11` has `-O3 -DNDEBUG -mavx2 -mfma` and `xad_xad_ladder.cpp:90` has `std::mt19937_64`. x86-64 is inferred from those flags.
- Class: `CONTEXT`. Single-author blog with an interest in the result. The post's table is labelled "cached draws", but it does not say how the price-only figure got its draws. We run cached-draw and inline generation modes, so either reading can be compared. Lead check: done, post re-downloaded and the harness lines read in the subagent's copy.
- Measured at d1596e0, price only, one thread: `linux-zen1` 6.145e4 paths/s, `mac-m4max` 1.488e5. Missed, by 17.0 and 7.0 times. Not like-for-like: the source's CPU is unnamed, x86-64 with AVX2 by its flags, while our primal is scalar until stage 5, with Philox draws generated inline. Files: `results/linux-zen1/mc-asian252-20261104T090220Z.txt`, `results/mac-m4max/mc-asian252-20261104T074520Z.txt`.

### MC-2: same, with five Greeks by tape: 0.197 million paths/s, single thread

- Source and quote: as `MC-1`. The five Greeks are "delta, strike sensitivity, vega, rho, and maturity sensitivity", one tape per path, cached draws. Derived multiple: 1.044 / 0.197 = 5.3, single thread. The post gives 0.127 million with fresh draws.
- Class: `CONTEXT`. Lead check: done.
- Measured at d1596e0, value and five Greeks by tape, one thread: `linux-zen1` 3.36e4 paths/s, `mac-m4max` 5.43e4. Missed, by 5.9 and 3.6 times. Our draws are generated inline, and the source's figure with fresh draws, 0.127 million, is still 3.8 and 2.3 times ours. Not like-for-like: hardware as `MC-1`, and our five Greeks are delta, vega, rho, dividend rho and theta. Files: `results/linux-zen1/mc-asian252-20261104T090220Z.txt`, `results/mac-m4max/mc-asian252-20261104T074520Z.txt`.

### MC-3: value and five Greeks, eight threads: 1.245 million paths/s, stretch 5.41

- Source: same post, results table, "million paths per second for value and Greeks", cached draws. Quote: "XAD C++ tape, harness pool | 8 | 39.43 | 1.245" and "MatLogica AADC | 8 | 130.76 | 5.41" (HTML table cells, separators ours)
- Class: `CONTEXT`. 5.41 is a recorded-graph JIT, not a tape, runs through Python bindings, and falls to 0.487 without the draw cache. `mac-m4max` only, unpinned. Lead check: done.
- Measured at d1596e0, value and five Greeks: `linux-zen1` 6.345e4 paths/s on 2 threads pinned to CPUs 4 and 6, `mac-m4max` 3.914e5 on 8 threads, unpinned. Missed, by 19.6 and 3.2 times, and the stretch 5.41 by more. Not like-for-like: `linux-zen1` has 2 threads, not 8, and the hardware differs as in `MC-1`. Files: `results/linux-zen1/mc3-asian252-mt-20261104T095003Z.txt`, `results/mac-m4max/mc3-asian252-mt-20261104T080643Z.txt`.

### MC-4: NEON European: 18.2 million paths/s, serial

- Source: arXiv 2606.16059, Section 5.3, Table 1. https://arxiv.org/pdf/2606.16059
- Quote: "Mojo serial (1 option)             54.8 ms"
- Source measured: "a commodity Apple Silicon laptop", chip not named, "NEON SIMD width 2 for double precision" | "Monte Carlo call, 1M paths", steps, RNG and transform not stated | serial | best of five. Derived: 1e6 / 0.0548 s = 18.2 million paths/s.
- Class: `CONTEXT`. The only Apple Silicon figure found. Preprint, European. It is a floor, not a bar: the same table has NumPy at 16.6 ms and "Mojo parallelize (per option)" at 11.9 ms, so the serial row was not the fastest on that machine. Lead check: done.

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

- Source: Wichura, Algorithm AS 241, Table 3, printed p.479, PDF p.4. https://csg.sph.umich.edu/abecasis/gas_power_calculator/algorithm-as-241-the-percentage-points-of-the-normal-distribution.pdf
- Source measured: PPND16 on IBM 3081, base 16, 14 digits | 50,000 values uniform on a logit scale, p constrained to (1e-70, 1 − 1e-15) | relative error, maximum 6.0e-16 for |p − 0.5| ≤ 0.425, 5.8e-16 beyond.
- Class: `DIRECT` for accuracy, on non-IEEE arithmetic. We measure relative error on IEEE double against a high-precision reference, because the same polynomials can land differently there. Scanned, so there is no quotable text. Lead check: done, read from the page image.
- Measured: missed. Maximum relative error 8.39e-16 at p = 0x1.4f72c68e4f73ep-619, 1.4 times the target. That is AS241 itself on IEEE double: the exact table in `tests/math/` removes the reference's own error (`NOTES.md`, "Phase 3 findings so far"). The same bits on both hosts, by the host-independence hash. An accuracy test, `MathInverseNormal.ProductionMatchesTheTable`, not a stamped results file. Not like-for-like: Wichura's 6.0e-16 is on base-16 arithmetic.

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
- No measured run yet. Iteration only, not results: `aadp_bench_simd` (d0cbc09) at 1/10 of its workloads' sizes, one thread, seed 20261119, medians of 5 repetitions. Each figure is the scalar pricer or kernel over the widest lanes, in the same binary, and every timed SIMD result was checked bit for bit in the run.
  - `mac-m4max`, macOS, NEON, ceiling 2 lanes, unpinned, at d0cbc09. European, one step: 0.81. Arithmetic Asian, 252 fixings: 1.32. `AAD-1`'s barrier under one-step survival: 1.98. Kernels: `exp` 2.23, `log` 1.99, inverse normal 1.20, normal CDF 1.48. The scalar `exp` is other code, and against its own one lane the lane `exp` gives 1.78.
  - `linux-zen1`, AVX2, ceiling 4 lanes, but about 2 lanes' throughput on Zen 1, pinned to cpu6, at 065e5b6 with d0cbc09's code. European 3.20, Asian 2.76, barrier 2.83. Kernels: `exp` 1.93, `log` 2.73, inverse normal 2.82, normal CDF 1.63. Against one lane, the European gives 4.21 and the Asian 4.51, above the ceiling of 4, because the one-lane build is 1.32 and 1.63 times slower than the scalar pricer there. Five of the seven exceed 2, the throughput the ceiling line expects of Zen 1.
  - Why AVX2 on Zen 1 exceeds 2, explained at stage 5c (`NOTES.md`, "Stage 5c"). Iteration only: `aadp_bench_simd` at 1/10 scale, `linux-zen1`, AVX2, ceiling 4 lanes (Zen 1: 128-bit FP datapath), `taskset -c 4`, at 1078a20, with `perf stat` user-space counts. The scalar code is latency-bound: IPC 0.8 to 1.2, using 0.5 to 0.95 of the 4 FP-pipe µops a cycle allows. The lanes keep about the same IPC, while each 256-bit instruction runs as two 128-bit halves on two FP pipes at once. So the speedup follows the instruction ratio, scalar over 4 lanes: European 3.35, Asian 2.59, barrier 2.26, inverse normal 2.85, `log` 4.33. The cycle ratios in that run were 3.18, 2.76, 2.83, 2.79 and 2.51. The 128-bit datapath halves peak throughput, which binds only throughput-bound code. The barrier also gains 1.25 times in IPC, partly from branch misses, 5.1 million per repetition in the scalar code against 0.13 million on lanes; the rest of that gain is not attributed. `log` exceeds 2 only because the scalar `log` is different code: 98.6 instructions per value, against 22.8 at 4 lanes.
  - Lanes in the AAD bench, stage 5c (8e8a464). Iteration only, not results: `aadp_bench_aad` at 1/50 scale, 3 repetitions, at 1078a20, ranges over the five first-order pricer workloads, every check passed, no group fell back. `mac-m4max`, macOS, NEON, ceiling 2 lanes, unpinned, threaded workloads on 8 threads: `price` / `price-lanes` 1.31 to 1.37, `tape` / `tape-lanes` 1.92 to 2.01. `linux-zen1`, AVX2, ceiling 4 lanes (Zen 1: 128-bit FP datapath), cpu4, two threads on cpus 4 and 6: 1.96 to 2.08 and 1.82 to 2.03. The AAD multiples on lanes are under "How Phase 3 numbers are produced".
  - AVX-512 has no timing: neither host runs it. Its tests run in CI, under Intel SDE where the runner lacks AVX-512, and emulated time measures nothing here.

### SIMD-2: scalar and SIMD agree

- Target: same stream, same results within a stated bound. Scalar is the one-lane instantiation (`FixedTag<double, 1>`) of the same kernels under the same static target, so per-path results are bit-identical between lane widths within one build.
- A separate `HWY_SCALAR` build or a libm-based scalar check may differ in fused multiply-add and in kernel rounding. For those, relative price difference at most 3e-13 at 252 steps (derived: 252 steps × 5 ULP × 2.22e-16 = 2.8e-13, first order, from 4 ULP for Highway's `Log` plus 1 ULP for libm, then rounded up). This assumes one log or exp call per step. If kernels make more calls, the bound is recomputed before measuring.
- Across architectures, results are not expected to be bit-identical, because fused multiply-add status differs by target.
- Source for the 4 ULP: `hwy/contrib/math/math-inl.h:2358`, "Max Error: ULP = 4", on the `Log` block. https://raw.githubusercontent.com/google/highway/master/hwy/contrib/math/math-inl.h
- Class: `DERIVED`, stated before measuring. Lead check: done, line read in the subagent's copy, arithmetic redone.
- Measured: met within each build, bit-identical, by tests rather than a results file (a486c9a). The lanes run this project's own kernels with no fused multiply-add, not Highway's `Log`, so every width gives the scalar pricer's bits: each kernel on about 3.1 million inputs (2.1 million for `sqrt`), and every product's mean, SE and batch means on 1, 2, 3 and 8 threads. Widths 1 and 2 on `mac-m4max` (NEON), 1, 2 and 4 on `linux-zen1` (AVX2), and 1 (`HWY_SCALAR`), 2 (SSE2, NEON), 4 (AVX2) and 8 (AVX-512) in CI run 41898528776. The 3e-13 bound across builds was not needed, and no SIMD result was compared across builds.
- Measured, the vector tape (stage 5b, by tests): bit-identical to the scalar tape, so again no tolerance. Each lane's value and five Greeks per path, and the block sums' mean, SE and batch means on 1, 2, 3 and 8 threads, at 1 and 2 lanes on `mac-m4max` (NEON), 1, 2 and 4 on `linux-zen1` (AVX2, at a859c7c), 1, 2 and 4 in CI's lane jobs, and 8 in CI run 41957624701 under Intel SDE `-skx`. No pricer kernel fell back.
- B16, vector adjoints against N scalar sweeps (139f398): 8000 random straight-line programs, each lane at its own inputs, each lane's input adjoints against a scalar sweep of the same program. 15,510 lanes compared at 2 lanes and 29,652 at 4, 0 not bit-identical. 245 and 587 programs diverged, as the generator's spread is meant to make some do, and are counted, not compared. The dot product reached at worst 0.0872 of its bound. Same counts on `mac-m4max` (1 and 2 lanes) and `linux-zen1` (1, 2 and 4). The test also passed at 8 lanes in CI run 41957624701 under SDE; its 8-lane counts are not in that log.

## 6. Metal GPU

Metal is an additional backend, float32 only, on `mac-m4max`. AAD does not run on the GPU. Greeks come from the double-precision CPU tape, and the GPU backend prices only.

### GPU-1: GPU against SIMD CPU at the same path count

- Target: measured wall clock from before buffer allocation to after host reduction, across all command buffers, at the same N and RNG, via `measure.sh`. Kernel compile and pipeline creation reported separately. A CPU float32 line is included, so a GPU ratio does not mix hardware with precision.
- Class: `UNCONFIRMED`. No published Monte Carlo pricer on Metal was reachable, so no pass mark exists, and none is invented. Stated as measured.

### GPU-2: float32 error far below Monte Carlo standard error

- Source: du Toit, Lotz, Naumann, "Adjoint Algorithmic Differentiation of a GPU Accelerated Application", section 6.2, PDF p.10. https://support.nag.com/doc/techrep/pdf/adjoint-algorithmic-differentiation-of-gpu-accelerated-app.pdf
- Quote: "the standard deviation of the Monte Carlo estimate was orders of magnitude greater than the threshold of single precision accuracy (roughly 10−6)."
- Source measured: NVIDIA K20X | FX basket call on 10 rates, 10-factor local vol, 10,000 paths, 360 Euler steps | GPU | price comparison.
- Target: |float32 − double| on identical uniforms at most 0.01 × SE. The factor is our reading of "orders of magnitude" and is stated before measuring.
- Class: `CONTEXT`, different model and hardware. Lead check: done.

### GPU-3: paired relative difference about 1e-7

- Source: NAG, section 5.4, PDF p.7, Table 3 p.8. https://support.nag.com/numeric/gpus/local-volatility-fx-basket-option-on-cpu-and-gpu.pdf
- Quote: "one would expect the relative error between the two results to be around 1e-7. If the error is significantly worse than this, it may suggest that the calculation is unstable in single precision, or that there are significant accumulations of round off errors"
- Published reference: Table 3 rows `Dp 0.5892391192` and `Sp 0.5892388787`, overall relative error 4.08e-7, a reference and not a pass mark. Barrier prices may land above 1e-7 with no fault, because survival products and near-barrier steps carry larger rounding error (derived in the Metal evidence).
- Class: `CONTEXT`. Lead check: done.

## 7. QuantLib comparison

QuantLib v1.44 and Boost 1.92.0, built through pinned FetchContent in release mode with the same flags. Protocol and row definitions are in `TASKS.md`.

### QL-1: speed

- Target: faster than QuantLib's tuned configuration in paths/s and in RMSE² × time, on every strictly like-for-like row. The size of the gap is not a target.
- Class: `UNCONFIRMED` as a number. This target is relative and comes from our own run. No published figure states the full setup. The nearest are `QL-3` and `QL-4`, and neither supports a ratio. `quantlib-benchmark` reports tasks/s and has no Asian or barrier Monte Carlo case.

### QL-2: accuracy

- Target: on each row, distance from the stated high-precision reference (section 9) within the tolerance rule (section 10). Distance between the two engines is never the measure.
- Class: `STRUCTURAL`.

### QL-3: QuantLib Asian Monte Carlo: about 115,400 samples/s

- Source: AMD Xilinx Vitis Libraries 2019.2, MCAsianEngines, Table 3. https://xilinx.github.io/Vitis_Libraries/quantitative_finance/2019.2/guide_L2/engines/MCAsianEngines.html
- Quote: "Table 3 shows the performance improvement in comparison with CPU-based Quantlib result (Tolerance = 0.02)", with rows `SampNum 25951 33642 46805` and `CPU Execution time (us) 224911 310856 601068`.
- Source measured: hardware, compiler, QuantLib version, steps and threads not stated | McAsianAPEngine column, 25,951 samples | CPU time in µs. Derived: 25,951 / 0.224911 s = 115,383 samples/s.
- Class: `CONTEXT`. A vendor baseline for its own FPGA. The CPU and FPGA prices in that column differ by 0.093 (1.98441 against 1.89144), above the page's own tolerance, so the two sides may not be pricing the same thing. Lead check: done.

### QL-4: QuantLib European Monte Carlo, one step: about 2.33 million paths/s

- Source: AMD Xilinx Vitis Libraries 2020.1, MCEuropeanEngine benchmark. https://xilinx.github.io/Vitis_Libraries/quantitative_finance/2020.1/guide_L2/benchmarks/MCEuropeanEngine.html
- Quote: "Baseline is Quantlib, a Widely Used C++ Open Source Library, running on platform with 2 Intel(R) Xeon(R) CPU E5-2690 v4 @3.20GHz, 8 cores per processor and 2 threads per core.", with `Baseline 20.155ms` and "workload 1 steps, 47000 paths".
- Source measured: 2 × Xeon E5-2690 v4, compiler, QuantLib version and thread count for the run not stated | put K=40, S=36, r 6%, vol 20%, dividend 0, T=1, 1 step, 47,000 paths, tolerance 0.02 | execution time, what it includes not stated. Derived: 47,000 / 0.020155 s = 2.33 million paths/s.
- Class: `CONTEXT`. Vendor baseline. One step says little about cost per path on an Asian or barrier. The page's prose says 488X and 1954X while its table says 380X and 1521X, and the table's own times give the latter. Lead check: done.

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

- Target: flags any crossing of total variance between adjacent slices, tested on a constructed crossing pair. A necessary-and-sufficient condition for two raw SVI slices was not found. Martini & Mingone list as still pending "the question of the characterization of no Calendar Spread arbitrage for two SVI slices corresponding to different maturities." (https://arxiv.org/pdf/2005.03340, conclusion). The check is numerical, and we say so.
- Class: `STRUCTURAL`. Lead check: done.

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

### SVI-11: raw SVI implied-vol RMSE: at most 0.73 vol points per slice

- Source: Zhang, Lin, Lu, arXiv 2603.27501v1, Table 3 p.24, metric p.23. https://arxiv.org/pdf/2603.27501
- Quote: "Suppose that at a given timestamp there are n available strike prices for implied volatility calculation, then the RMSE is defined as" √((1/n) Σ (σ_model − σ_market)²), then "the RMSE values obtained at each timestamp are aggregated, and the average RMSE is computed for each model across different underlyings and contracts." Table 3 row: "SVI 7.314×10−3 8.898×10−3 5.992×10−3 1.328×10−3" (IO202504 first).
- Source measured: CSI 300 index options, contract IO202504, 2025-01-20 to 2025-04-17, about 88 to 1 days to expiry, 5-minute calibrations | raw SVI per slice in total variance, with a > 0, tighter than usual | in-sample, equal-weighted across strikes, no strike filter stated | mean of per-timestamp slice RMSEs. The table has no unit, so 0.73 vol points assumes vol as a decimal (derived).
- Target: our per-slice RMSE by this formula, unweighted, for each of the three slices, beside the fitted objective, which is weighted. A mean over a contract's life is not a slice at 5 days, and short-dated slices are harder, so a miss at 5 days is reported with that reason. It says nothing about error against bid-ask.
- Class: `CONTEXT`. Preprint, on data from a database provided by one author's institution, and a different market. A quartic polynomial and a cubic spline beat SVI on the three near contracts of the same table, so 0.73 reflects this paper's calibration and is not a floor for raw SVI. Lead check: done, table read from the page image.

## 9. Reference values for oracles

Published value first, then the value we test against. A derived value comes from deterministic quadrature in the research scripts and is used only where it is more precise than, and consistent with, the publication. Phase 2 commits a quadrature tool to the repo, so these values do not stay outside it.

| ID | Contract | Published | Source | Reference used | b (reference uncertainty) | Lead check |
|---|---|---|---|---|---|---|
| O1 | BS delta, call K=100, S=105, q=r=.10, T=.5, σ=.36 | 0.5946 | QuantLib 5d6defcc `test-suite/europeanoption.cpp:405+`, Haug 1998 | closed form | 0 | done |
| O2 | BS rho, call K=75, S=72, q=0, r=.09, T=1, σ=.19 | 38.7325 | as O1 | closed form | 0 | done |
| O3 | BS theta, put K=405, S=430, q=.05, r=.07, T=1/12, σ=.20 | -31.1924 per year | as O1 | closed form | 0 | done |
| O4 | Geometric Asian, 10 fixings, S=K=100, q=.03, r=.06, σ=.20, T=1 | 5.3425606635 | QuantLib `test-suite/asianoptions.cpp:429` | closed form | 0 | done |
| O5 | Discrete down-and-out call, S0=K=100, H=95, σ=.30, r=.10, T=.2, m=50 | 4.907 | Broadie, Glasserman, Kou 1997, Table 2.1. https://www.columbia.edu/~sk75/mfBGK.pdf | 4.9067890354 (derived) | 1e-9 | done |
| O6 | Delta of O5 | 0.798 | same, Table 2.4 | 0.7978141916 (derived) | 1e-9 | done |
| O7 | Continuous down-and-out, same parameters, H=95 | 4.398 | same, Table 2.1 | closed form, 4.3975025600 | 0 | done |
| O8 | Discrete down-and-out, m=4, H=95, σ=.6, T=.2 | 9.49052 | BGK 1999, Table 1. https://business.columbia.edu/sites/default/files-efs/pubfiles/1851/connecting.pdf | 9.4905 | 1e-4 | done |
| O9 | Discrete arithmetic Asian, n=50, K=100, S0=100, r=.0367, σ=.17801, T=1, 51 points including S0 | 4.93736 | Fusai & Meucci, Table 6. https://openaccess.city.ac.uk/id/eprint/6061/1/Pricing%20Discretely%20monitored%20asian%20Fusai_Meucci_JBF.pdf | 4.937202814482 (derived) | 1e-9 | done |
| O10 | Continuous arithmetic Asian call, Linetsky case 5: r=.05, σ=.50, T=1, S0=K=2.0, q=0 | 0.2464156905 | Linetsky 2004, Table 3 p.866, read from the page image. http://users.iems.northwestern.edu/~linetsky/asian.pdf | limit check only | 1e-10 | done |
| O11 | Two-fixing arithmetic Asian put, first fixing at t=0, S=90, K=87, q=.06, r=.025, σ=.13, T=11/12 | 1.3942835683, wrong | QuantLib `test-suite/asianoptions.cpp`. Exact: half a vanilla put with K=84 | 1.3939809367 (derived) | 0 | done |

Note on O5 and O6: two independently written quadratures agree. `refvalues.py` gives 4.9067890354 and 0.7978141916 at 1,408 and at 3,060 nodes, and `crosscheck.py` gives 4.906789 and 0.79782. All five H rows round to BGK's values at 0.001 except H=99, where 2.3363868958 sits 6.1e-4 from the published 2.337, inside BGK's stated accuracy. So b comes from the quadrature, not from the publication's 0.001, which would blunt the test.

Note on O9: the published table does not reproduce at its printed σ. All geometric rows match closed form only at σ ≈ 0.1780163, and the n=12 and n=50 arithmetic rows then come within 2e-5, so the table used an unrounded σ. The derived value is at the stated σ = 0.17801. Recursion and brute-force quadrature agree to 2.5e-14 at n=3.

Note on O10: a continuous average, reachable by a time-stepped simulator only in the limit.

Do not use QuantLib's ten-digit Levy values (`asianoptions.cpp:683-775`). The two-fixing case is off by 3.0e-4, which a tolerance of 2e-2 hides. Printed digits are not correct digits. O11 keeps the identity as a free test.

Published table values and derived values recomputed by the lead: `verify/rerun/barrier.out`, `asian.out`, `refcheck.out`, `crosscheck.out`, `naive.out`, `critical.out`.

## 10. Tolerance rule

Stated before measuring. Each comparison passes only if both of these hold:

1. |x̄ − ref| ≤ t(1 − α_F / (2K), B − 1) · SE + b
2. SE ≤ a ceiling fixed per comparison before the run, so a noisy run cannot pass by being noisy.

SE is from B batch means, b is the reference or finite-difference truncation allowance, K is the number of comparisons in the suite, and α_F is the family-wise error rate. Bonferroni needs no independence assumption, which matters because Greeks from the same paths are correlated.

- Source: NIST/SEMATECH e-Handbook, section 7.4.7.3. https://itl.nist.gov/div898/handbook/prc/section4/prc473.htm
- Constants, pre-registered in `references/comparison_plan.csv` at commit 5993b4a, before the first measured run: α_F = 0.01, B = 64, K = 147, giving a critical value of 4.2662 (computed in code, `src/oracle/student_t.hpp`). 90 rows run now and 57 wait for Phase 3. K counts every Greek and case pair, including rows not yet runnable. It was 100 provisionally, with a critical value of 4.155. If K changes again, the constant is recomputed before any run.
- SE ceilings: `factor × SE_pilot × sqrt(N_pilot / N)`, from a pilot on seed 1729, a different seed from the measured 271828. The factor is 1.65 on reference rows and 3 on finite-difference rows. The factors are stated in the plan header with their reasons. The finite-difference factor and one step size were set from development seeds before the plan was committed, which the plan's order of commits does not by itself show, so it is stated here.

QuantLib's 2.34 × SE pattern is useful for its two-part shape, but the multiplier has no documented basis and is not copied. Its fixed tolerances (for example `2.0e-2` for arithmetic Asians) are not tied to any SE.

## 11. The option chain

Real, free and redistributable. Licence text and full reasoning are in `NOTES.md`.

- Data: TAIFEX TXO daily report, trade date 2024-11-15, data.gov.tw dataset 11320, Open Government Data License v1.0.
- Fixed file: `https://web.archive.org/web/20241117234312id_/https://www.taifex.com.tw/data_gov/taifex_open_data.asp?data_name=DailyMarketReportOpt`, 465,662 bytes, sha256 `fd9ddc6cafcbdd1b08cd0f80912dcbd9c19de515cdb248163183378715d53f1e`. Lead check: done, hash matched on a fresh download.
- Usable surface: three expiries only, 2024-11-20, 2024-11-27, 2024-12-18 (5, 12, 33 days). Longer expiries are exchange-set marks, not market prices. Any figure that includes them is labelled so.
- Input arbitrage, before any fit. Fit violations are always reported beside these. Lead check: done, the profile was re-run from the fresh download, and `arb.csv`, `parity.csv` and `profile_recommended.txt` are byte-identical to the subagent's (`verify/rerun/chain/`).

| Set | Butterfly triples | Non-convex | Beyond half a tick |
|---|---|---|---|
| Calibration set, three expiries | 168 | 51 | 23 |
| OTM settlements, all expiries | 480 | 102 | 45 |
| All settlements, all expiries | 984 | 200 | 55 |

## 12. Unconfirmed targets, and where we looked

The WebSearch budget ran out during research. A later narrow pass gave `GPU-1`, `QL-1` and the raw SVI error their first real search, at 8 searches and 6 opened pages each. Every absence below is the result of a bounded search, not proof that nothing exists.

| Target | Looked in | Search state |
|---|---|---|
| `AAD-4` Asian full-tape multiple | Capriotti 2011, Capriotti & Giles 2024 review, Giles reports, Savine, XAD pages, NAG reports | Searched before the budget ended |
| `AAD-8` second-order timing | Capriotti, Joshi & Yang (abstract), Giles, Savine, Huge & Savine | Searched before the budget ended |
| `SIMD-1` NEON Monte Carlo speedup | Intel, AMD, Arm and Apple pages, Mojo preprint | Partly searched; Intel and AMD pages 404 |
| `GPU-1` Metal Monte Carlo throughput | Apple docs and talks, philipturner/metal-benchmarks, Random123, GPU Gems, plus the narrow pass | Searched. No option-pricing figure with a named chip. See below |
| `QL-1` QuantLib speed | quantlib.org, docs, Implementing QuantLib, `Docs/`, XAD pages, FinanceBench paper, plus the narrow pass | Searched. `QL-3` and `QL-4` are vendor baselines. No barrier figure, and none states a full build |
| `SVI-10` adjoint through calibration | Henrard, Christianson, Giles talks | Searched before the budget ended |
| Raw SVI error at 5 to 35 days, or against bid-ask | Zeliade, Aurell, Corbetta, Martini-Mingone, Ackerer, Pasquazzi, FPI-SVI, plus the narrow pass | Searched. `SVI-11` is the nearest. A UPF thesis on raw SVI for SPY and QQQ was behind a bot wall and is unread |

What the narrow pass found for `GPU-1`, neither of which is a target:

- Liu, arXiv 2401.11455v2, Table VII: "Fresh fuel 4.5E3 3.7E3 1.5E3", particles/s/Watt, on an M2 Max with Metal. https://arxiv.org/pdf/2401.11455. Neutron transport, not finance, and an efficiency: no wattage or wall time is given, so particles/s cannot be derived. Lead check: done.
- montepath, an alpha open-source project, `benchmarks/release-results.json`: `mc_metal_european_call_native`, `"per_iteration_us": 1450.0830000000003`, 3 iterations. https://raw.githubusercontent.com/IndarKarhana/montepath/main/benchmarks/release-results.json. No chip, step count or precision, and the file does not define its work unit. Lead check: done.
- The EPJ SNA+MC 2024 paper is the best unread lead for M3 Max figures. It was behind a CAPTCHA, which was not bypassed.

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
