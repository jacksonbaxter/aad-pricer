# NOTES

Research findings and decisions. Later phases add what was tried, including what did not help.

Counts, forwards, fitted parameters, prices and sensitivities here that come from the option chain are derived from it: settlements of one trade date, not tradable quotes. Data: TAIFEX via data.gov.tw, Open Government Data License (NOTICE).

## Evidence index

Raw evidence is outside the repo, because third-party material is not committed: it sits in the project's private research cache, `.orchestrator/research-cache/aad-pricer/`, in the folder above this repo. Evidence files record local paths as `.research/<track>/`, the location before they were moved. Read those paths relative to the cache.

| Track | File | Contents |
|---|---|---|
| AAD cost | `aad-cost/EVIDENCE.md` | 67 entries: theory, measured multiples, memory, bump |
| AD libraries | `ad-tools/EVIDENCE.md` | 51 entries, bar items B1 to B39 |
| MC, RNG, SIMD | `mc-perf/EVIDENCE.md` | 55 entries, platform matrix |
| Oracles | `oracles/EVIDENCE.md` | 60 entries: treatments, reference values, tolerances |
| SVI | `svi/EVIDENCE.md` | 116 entries |
| Chains, archives | `chain-archives/EVIDENCE.md` | Zenodo, Figshare, Mendeley, CRAN candidates |
| Chains, hubs | `chain-hubs/EVIDENCE.md` | Hugging Face, Kaggle, venues, exchanges, portals |
| Chain usability | `chain-verify/EVIDENCE.md` | Attack on the TAIFEX candidate, profile, NOTICE text |
| Metal | `metal-gpu/EVIDENCE.md` | 80 entries, probe, float32 mechanisms |
| QuantLib | `quantlib-compare/EVIDENCE.md` | 59 entries, comparison protocol, build plan |
| Gap searches | `gaps/{metal-throughput,quantlib-speed,svi-fit-error}/EVIDENCE.md` | Narrow pass on the three gaps left by the search budget |
| Lead's check | `verify/verify_sources.log`, `verify/rerun/` | Fresh download of every target source, hashes, quote search, re-run reference values and chain profile |

## Process notes

- Ten research tracks ran, then three narrow gap searches. `metal-gpu` and `quantlib-compare` were stopped by the account usage limit after their evidence files were written, so neither returned a final message, and both evidence files are complete to their search logs.
- The shared WebSearch budget of 200 calls per turn ran out during the first round. After that, research used direct URLs only. Gaps from that are listed in `BENCHMARKS.md` section 12.
- Third-party clones sat inside the working tree during the first round. Six third-party agent-instruction files were among them (QuantLib twice, XAD, Sacado). They were moved out, unopened by the lead, and nothing from them was committed.
- Rule slips by subagents, none affecting a number. `ad-tools` and `chain-hubs` wrote scratch files to `/tmp`, later moved or deleted. `metal-gpu` extended its probe beyond the listed outputs, with a capability kernel and counter queries but no timing. `chain-verify` fetched from Internet Archive with a generic User-Agent, which the fetch script must not repeat.
- Apple's documentation pages are JavaScript shells, so `metal-gpu` read Apple's own per-page JSON, one named page at a time. Accepted: it is the publisher's site and not a search route. It flattens tables, and that lost one table (its E61). The Metal gap search did the same with PyPI's public JSON for one package, for the same reason. No target rests on it.
- Pages behind a CAPTCHA or bot wall were left unread and not bypassed: the EPJ SNA+MC 2024 paper and the UPF thesis on raw SVI.
- Lead check method: `verify/scripts/verify_sources.py` re-downloads each source, compares sha256 and searches for the quote. Its HTML stripping eats C++ `<...>` spans, and PDF line-break hyphens defeat matching, so every miss was resolved by a raw grep. All misses were false alarms. Two pages changed bytes with the quote still present (NablaTensor post, NIST Bonferroni page).
- Corrections the lead check made to the first version of `BENCHMARKS.md`: `SIMD-2` used half an ULP for one ULP, which made the bound too tight; `MC-3` was mislabelled as price-only; `MEM-3` called a run-level peak a per-path figure; build flags in `MC-1` come from the harness and not from the post; derived oracles inherited the publication's 0.001 as b, which would have blunted them.
- Broadie-Glasserman 1996 p.282 is a scan the lead did not read. No target rests on it. β = −ζ(1/2)/√(2π) = 0.582597 was recomputed, and BGK's values were reproduced by quadrature instead.

## Decisions from the Phase 1 review

1. The TAIFEX chain qualifies and is used. Residual doubt goes to the user. Conditions: the raw file stays unmodified and is never committed, the required attribution is in `NOTICE` and on every generated output that uses it, and the fetch script uses a descriptive User-Agent. README and WRITEUP.md state that the data is settlement-based, covers one date and reaches only 33 days, and they name the doubts under "What remains doubtful". If the capture ever disappears, report it and do not swap in another source.
2. The fit is against settlements. Nothing may imply these are tradable quotes.
3. GPU prices only, in float32.
4. Scratch files stay inside the folders, including for subagents. A `/tmp` write is a rule slip even when harmless. The session lead made that slip twice in Phase 1 (`/tmp/lin.txt`, `/tmp/x_nag-*.txt`, deleted afterwards).

## Phase 2 harness design

The harness comes first, and a check that cannot fail proves nothing. So every oracle and check has an injected fault that makes it fail, recorded in a results file.

### Layout

| Path | Contents | CMake area |
|---|---|---|
| `src/platform/` | CPU info, timers, pinning, perf counters, cache-line constant, FP state, SIMD target | `bench` |
| `src/results/`, `tools/results_stamp/`, `bench/` | Benchmarks, measure.sh line parser, results-file reader and writer | `bench` |
| `src/rng/`, `src/mc/` | Philox, inverse normal, minimal path kernels | `oracle` |
| `src/oracle/`, `tools/quadrature/`, `references/` | Closed forms, tolerance rule, comparison plan, quadrature tool, reference values | `oracle` |
| `src/data/`, `tools/chain_profile/`, `scripts/fetch_data.sh`, `NOTICE` | cp950 decoder, TAIFEX readers, parity forwards, calibration set, data script | `data` |
| `src/svi/`, `tools/svi_fit/`, `tools/local_vol_report/` | Implied vol, raw SVI, arbitrage checks, the raw SVI fit, the chain into it, fit report, the surface in time, Dupire local variance, the chain as a surface, local-vol report | `svi` |
| `tests/<area>/`, `fuzz/` | Unit tests, libFuzzer targets, hand-written seed corpora | per area |
| `scripts/check.sh`, `.github/workflows/`, `faults/` | Local check script, CI, fault manifest | `checks` |
| `results/mac-m4max/`, `results/linux-zen1/` | Raw results. No file holds numbers from both | |

Namespace `aadp`. Each area has a CMake option, `AADP_AREA_<NAME>`, default ON.

### Minimal stubs

The oracles need something to test, and AAD is Phase 3. So `src/mc/` holds path kernels templated on `Real`, for GBM exact steps only: European, arithmetic Asian, geometric Asian, discrete down-and-out with hard indicator, discrete down-and-out with one-step survival, continuous down-and-out with bridge weight. Greeks in this phase come from a forward-mode `Dual` in `tests/support/`, labelled there as a stand-in and not AAD. Phase 3 swaps in the tape under the same oracles, so the oracles are proved before the code they judge exists.

### Oracles and the tolerance rule

The rule is `BENCHMARKS.md` section 10, implemented once, with the Student t quantile computed in code rather than typed. `references/comparison_plan.csv` lists every comparison: K is its row count, each row fixes reference, path count, B, seed and SE ceiling, and rows waiting for Phase 3 stay in the count, marked as such. SE ceilings are set from a pilot on a different seed and committed before the measured run, so git history shows them fixed first.

Reference values live in `references/oracle_values.csv`, generated by `tools/quadrature/`. A test regenerates them and compares, so the file cannot drift.

### Fault injection

`faults/manifest.json` lists each fault: what it attacks, the mutation (file, exact text, replacement), the command to run, and the failing test or check that must report it. A runner applies each mutation to a copy under `build/`, never to the working tree, confirms the unmutated baseline passes, confirms the mutated run fails for the named reason, and writes one results file. Failure for another reason, such as a compile error where a test failure was expected, counts as the fault not being caught.

### What Phase 2 found

Oracles:
- K is 147, not the provisional 100: 90 rows run now, 57 wait for Phase 3. Critical value 4.2662.
- Finite-difference rows are paired differences that are mostly zero, with spikes at the payoff kink. Between pilot seeds, the log of the SE ratio had sd 0.25, against about 0.09 that normal theory gives for 64 batches. So batch means on those rows are not close to normal, and the t bound is an approximation there. `DB099.vega` was the worst case, with an SE 2.54 times its pilot-scaled value on one seed. That is why those rows have ceiling factor 3 rather than 1.65.
- AS241 on IEEE double reached a maximum relative error of 8.47e-16 against the `erfc`-polished reference, above Wichura's 6.0e-16 on base-16 arithmetic. `RNG-3` keeps its target. The gap is examined in Phase 3, when the reference's own error has to be separated out. It was: see "Phase 3 findings so far", Math kernels.
- Richardson extrapolation of the discrete arithmetic Asian reached Linetsky case 5 to within 3.97e-11, an independent check on the Asian recursion.
- The oracles caught a mistyped AS241 coefficient (`a7`) during development.
- Fuzzing found a crash in the plan decoder on a misplaced `#!` directive. Fixed, with a regression seed.

Data, facts of the real file that differ from what research assumed:
- 124 after-hours futures settlements are the literal `NULL`. Treated as missing, in that column only.
- 150 calendar-spread rows have negative prices. Allowed on spreads only.
- The futures file has a `-0.00%` field. Accepted.
- Numbers go through a hand-written strict decimal parser, tested against `strtod`. `from_chars` for `double` needs a dylib symbol older macOS may lack, and it accepts `inf` and `nan`.

Benchmarks and hosts:
- zsh's `BG_NICE` silently niced the first `debian` proof run to +5. That run was discarded, and the stamp tool now refuses any nice value other than 0.
- `measure.sh` reports `cpus=` from its `taskset` argument, not the real mask, so the stamp tool cross-checks it against the mask read back in the process.
- `cpuinfo_max_freq` on `debian` says 2.0 GHz while cpu4 ran at 3.59 GHz with boost on. Record the observed frequency, not the nominal one.
- On `debian` the raw TSC moves in steps of 20 ticks and the smallest observed delta was 40, which is about 20 ns.
- Google Benchmark's JSON context carries the host name and the absolute executable path, and `measure.sh` lines carry `host=`. The stamp tool drops these and refuses to write if a home path, IP address or the hostname survives. Fuzzing found two leaks in that code before it was finished.

### Phase 2 checks, CI and interruptions

Interruptions:
- The session was killed at about 18:25 by the low-memory reaper during the clang-tidy step. The cause of the shortage is not established: builds were running in three repos on one Mac. 104 modified files survived, with 5 new ones, and were finished rather than redone: a1be512, 2490bd0.
- Rules since then: at most 6 jobs across a session, and one tidy or sanitizer build at a time.
- Later a usage limit stopped a subagent mid-run. A run it had started on the Linux host with `setsid nohup` finished on its own.
- A usage limit stopped work on 30 and 31 October. The Mac rebooted on 1 November, and the Linux host on 3 November. Each time, uncommitted edits were finished, not redone.

Rule slips:
- Several agents' zsh heredocs or here-strings wrote short-lived files to `/tmp`, the session lead once among them. zsh writes every heredoc and here-string to `/tmp`, whatever `TMPDIR` says.
- Two subagents set `HEAVY_MAX_WAIT`. On 30 October one set it to 7000 s, longer than the default wait, for three fuzz runs started in parallel. On 31 October another set it to 500 s and 30 s, so a ctest of about 8 s at `-j6` overlapped another orchestrator's measurement, which started at 03:17:22 local time.
- Both are now banned in the subagent rules: no heredocs or here-strings in a shell call, and never set `HEAVY_MAX_WAIT`.

Nice value:
- `HarnessSelftest.HeaderRecordsTheNiceValue` and `aadp_run_bench_script` failed when ctest ran as a zsh background job, because `BG_NICE` sets nice 5. `process_nice` read 12 under `nice -n 7`, where the test expected 7.
- The stamp's refusal was right and one test was wrong. Fixed in 9b746bb. The first test now adds the caller's nice to the increment, up to the ceiling, which is 20 on macOS and 19 on Linux. The bench script test now fails early in one clear line when niced.

What a Mac run cannot see:
- A Mac clang-tidy run cannot see `__linux__` branches, and tidy on the Mac removed includes those branches needed.
- a1be512 also fails to compile under the `fuzz` preset: `fuzz_results_json.cpp` was missing `<algorithm>`. 2490bd0 fixed it. `verify_commits.sh` builds only `release`, so it would not catch this.
- 9208e3a, 729ad02, db978e5 and 248c63c fix what the Linux and g++ builds found. 4fd86ac and 89dbee8 fix the two test failures below.
- So the per-commit proof on the Mac does not cover Linux from a1be512 up to 4fd86ac. g++ first built and passed at 248c63c, and every Linux build first passed at 89dbee8.

Which commits were built where, as far as the logs show:

| Commit | Mac | Linux |
|---|---|---|
| a1be512 | `release`, 362 of 362, just before commit. `fuzz` does not compile | Not built |
| 2490bd0, 556a6c8 | `verify_commits`, `release`, 362 of 362 | Not built |
| 9b746bb | `verify_commits`, plus `debug`, `asan-ubsan` and `tsan`, 362 of 362. `fuzz` built | Not built |
| 6480ebf | `check.sh` passed | All four release builds failed |
| 729ad02 | | Clang `release` and AVX2 363 of 364. g++ failed to build |
| 4fd86ac | | g++ failed to build |
| 248c63c | `check.sh` passed | g++ 364 of 364, AVX2 364 of 364, clang `release` 363 of 364 |
| 89dbee8 | | Every build passed, below |

Blank means no complete log. Commits not listed, such as 9208e3a and db978e5, have none on either host. The Mac runs 362 tests where Linux runs 364.

`misc-include-cleaner.IgnoreHeaders`, for the reasons stated in `.clang-tidy`:
- `benchmark/.*`: `benchmark.h` is that library's public header, whatever it splits into behind it.
- `cmath` and `__math/.*`: libc++ keeps `<cmath>` in private `__math` headers the check does not map back.
- `sys/_types/.*`, `_time.h`, `_stdio.h`, `sys/fcntl.h`: macOS internals behind POSIX headers, which do not exist on Linux.
- `linux-gnu/bits/.*` and glibc's `stdio.h`, `stdlib.h`, `time.h`: the same on glibc, its private `bits/` headers and the POSIX parts of the C headers behind `<cstdio>`, `<cstdlib>` and `<ctime>`.

Deliberate behaviour changes in a1be512:
- `run_git` starts git with `posix_spawnp` rather than `popen` through a shell.
- Tool mains catch exceptions and exit with a status, rather than `std::exit` deep inside.
- A plan row that the decoder proved valid but cannot be read again throws, where it was undefined behaviour to dereference.
- Harness batch times round with `llround`, which has no error just below a half.
- `fuzz_results_file` now asserts that a refusal states its reason.

Test faults the Linux run found:
- Two chain profile tests read real data by accident. They named only `--options`, so the tool also opened the default futures path. Fixed in 4fd86ac.
- Parallel ctest processes shared a scratch file, so `SetUp` in one truncated the file another was reading. Seen once on the Linux host at `-j2`. Fixed by a scratch directory per test (89dbee8).

Linux run at 89dbee8:
- Clang release, g++ 14.2 release, AVX2 release, clang debug and g++ debug: 364 of 364 each, 0 skipped.
- AVX-512 built, not run, because the CPU lacks it.
- Linux clang-tidy: 78 units clean.
- The plan's 90 rows match on the Mac, Linux clang, g++ 14.2 and AVX2, at the printed precision: 10 decimals, and 5 significant figures for SE. That is not a bit-identical claim. Those are release builds. Debug builds run a reduced smoke plan, so they have nothing to compare.
- The AVX2 match says nothing about SIMD lanes: Highway is used only for the target report and a smoke test, and no kernel uses it yet.
- Sanitizers and fuzzing ran on the Mac only, by rule. In Phase 2 the Linux sanitizer and fuzz-smoke CI jobs had not been run anywhere. They first ran on GitHub in Phase 3 (below).

CI:
- In Phase 2, `.github/workflows/ci.yml` never executed on GitHub. It first ran as run 41770201004 at bc3e6d3, in Phase 3, and failed 9 of 17 jobs. See "Phase 3 findings so far", CI.
- In Phase 2, local actionlint ran without shellcheck and pyflakes, so it linted no shell in `run:` steps. CI's runner has shellcheck, and the first run failed on it. shellcheck 0.11.0 is now pinned in `scripts/fetch_tools.sh` (d8e39d8).
- No gcc on macOS, because `release-gcc` is Linux only.
- CI uses `g++-14`, newer than the image's default of 13. The Linux host ran 14.2.
- One named allowed skip, `PerfCounters.EachRegionReportsItsOwnTimes`. Counters depend on the runner's kernel settings, and the test skips itself without them. It passed on the Linux host.
- `scripts/run_tests.py` is the shared test runner for `check.sh` and CI, so both judge a run the same way. ctest alone does not fail on a skip. The runner counts a test that did not run, a disabled test and a passing test that prints a `SKIP ` line as skipped, and a skip fails the run unless a regex names it. For fuzzing it finds targets through ctest, and it stops if a file in `fuzz/` has no target.

CI on macOS, after the first GitHub run (at bc3e6d3, where all four macOS jobs failed to link):
- The cause: LLVM 23.1.4's libc++ headers are built with vendor availability markup off, so they assume every symbol of their own dylib exists. `std::hash<std::string>` then calls the exported `std::__1::__hash_memory`, which Apple's libc++ has only from macOS 26.4. `macos-15` linked the macOS 15.5 SDK of Xcode 16.4, which lacks it.
- The measurement Mac compiles against the LLVM 23.1.4 headers and links the system `/usr/lib/libc++.1.dylib`, on macOS 27.2 with the macOS 27.2 SDK. The images GitHub offers on 2026-11-04: `macos-15` (15.8.2), `macos-26` and `macos-latest` (26.6.2, SDKs to 26.5), and the `xcode-27` preview (macOS 27.0.1, Xcode 27.1 with SDK 27.0 by default, Xcode 27.2 with SDK 27.2).
- CI now runs the macOS jobs on `xcode-27` with Xcode 27.2's SDK: the same headers, the same SDK version and the system libc++, as here. Linking LLVM's libc++ statically would also link on `macos-15`, but then CI would test a C++ runtime the measurements never use, so it stays the fallback for when the image goes.
- What still differs: the runner's OS is 27.0.1, not 27.2, so its system libc++ is 27.0.1's build; the SDK comes with Xcode 27.2, not the Command Line Tools; the image is a preview with no SLA, and its label may change when macOS 27 reaches GA; and the hardware is a GitHub VM, not an M4 Max, which matters for no test.
- On Linux, CI's clang uses g++ 14's libstdc++, because the LLVM release defaults to it on Ubuntu. The Linux host's toolchain defaults to libc++. clang-tidy finds different things on each, so the fixes for CI's tidy job were checked on both.

Repo scan exceptions, from `scripts/scan_repo.py`. Each is counted in the output, so none is silent:
- Home path: a path component has to follow, a path inside a URL is not a hit, and `SYNTHETIC_HOME_PATHS` lists exact file and text pairs.
- IPv4, by value: the three RFC 5737 documentation ranges, 127.0.0.0/8 and 0.0.0.0. A dotted run of five or more parts, or an octet above 255, is not an address.
- IPv4, by context: text right after "section", "version" or "v" reads as a numbering. Never for private, shared or link-local ranges, which always fail.
- IPv4, by file: `SYNTHETIC_ADDRESSES` lists exact file and address pairs.
- User and host: a generic name, or one under three characters, is not searched for, and the output says so.
- Private note: the two `.gitignore` lines that keep the local settings file and the root notes file out, exact to the file, and any mention of the research folder.
- Private note, tracked file: none. An agent notes file that git tracks is a finding at any path, with no pin and no exception (since 2026-11-12, below).
- TAIFEX: the first line of each file is not a data row, and a row with a changed field is not caught. With no `data/` this check cannot run, and the output says so.

Peak memory:
- The plan: about 4.3 MB footprint.
- Fuzzers: 470 to 695 MB, on the three that were measured.
- The largest tidy process: 1.13 GB.
- No job in this repo holds a whole trading day: the chain file is 465 KB.

### Fault injection results

The runner, from the header of `scripts/fault_injection.py`:
- `faults/manifest.json` holds 41 faults: 13 oracle, 19 check and 9 fuzz. The runner's own tests are `faults/test_fault_injection.py`, 36 of them.
- Each fault names what it covers, the bug in one sentence, its edits, a preset and build targets, a command, a reason regex and a time limit. An edit is an exact find and replace, where the text must match once, or an append. Optional fields give a fault a git tree, its own copy of `data/`, expected exit codes and files to search.
- Two modes. `detect: test`: the build must succeed and the command must fail with the reason in its output. `detect: build`: the build must fail with the reason in its output.
- It mutates a copy under `build/fault`, taken from `git archive HEAD`, or from `--source` for an export with no `.git`, as on the Linux host. The working tree is never touched.
- The unmutated baseline runs first. It must pass, and the reason regex must not already match its output. Then the mutated run must show the reason.
- `not-applied`, `baseline-failed` and `not-caught` all fail the run. A compile error where a test failure was expected is `not-caught`.

Results:
- Mac: all 41 caught at 4928e8f, `results/mac-m4max/fault-injection-20261103T162544Z.txt`.
- Linux: 29 of the 41 ran at dba8dc5, and all 29 were caught, `results/linux-zen1/fault-injection-20261103T165305Z.txt`. The 9 fuzz faults and the 3 sanitizer faults are left out there, because sanitizer and fuzz runs stay on the Mac. The report's scope line names all 12.
- `aadp_results_table` leaves a fault report out by its first line, and only under a host directory (f40bccf). A renamed results file cannot leave the table that way.
- `scripts/check.sh` now has 13 steps, with `fault-injection` last (4928e8f).
- Final full `scripts/check.sh` at 73bb647: 13 of 13 steps pass, 0 skipped. `release`, `debug`, `asan-ubsan` and `tsan` pass 368 of 368 each, and the fault step catches 41 of 41. It took 2294 s, and the largest process reached about 0.91 GB RSS.

Holes the faults found:
- The quadrature recursion had no discrete-barrier test with `q != 0`, so dropping the dividend yield from the drift went unseen (493ad27).
- Nothing pinned the SE ceiling boundary, so multiplying the ceiling by the critical value went unseen (3682442).
- Before those fixes, both faults passed all 108 oracle tests, including the measured plan. Both were fixed by new unit tests only. No reference, plan, ceiling or tolerance changed.
- 21 refusal tests checked only an exit code. They now assert the reason too (ed6cc94, 64b9ddb, 5896622). Under the header-refusal fault the stamp refuses with the same exit code, so the old forbidden-string stamp test would have passed for the wrong reason.

Fuzz faults:
- Every target runs with `-seed=1`. Execution counts differed between runs, so the seed does not make libFuzzer fully deterministic.
- Eight of the nine faults need a mutated input: every seed passes through them. Only the smoke target's fault fires on a seed.

Warning flags:
- The `-Wall`, `-Wextra` and `-Werror` faults each have a control: dropping just that flag removes the failure.

## Phase 3 oracle policy

Fixed before any Phase 3 code exists, so git history shows it came first.

- `references/measured_results.csv` is measured run 1: 90 rows, the Phase 2 reference kernels in `src/mc/`, Greeks from the forward-mode `Dual`. It is history and never changes.
- The reference kernels in `src/mc/` stay frozen, so `check.sh` keeps reproducing run 1. The production pricer is checked against them path by path, within a bound stated before the check runs.
- The production pricer and tape are judged by measured run 2: one run of all 147 rows of the same pre-registered plan, on seed 271828, at stage 8.
- Run 2 is a correlated look at the 90 run-1 rows, on the same paths with the same estimator. So it is reported beside run 1, never instead of it. A run-2 row that fails is reported as a failure, even if run 1 passed it.
- The 57 awaiting rows get SE ceilings from a pilot on seed 1729, committed before run 2, as the plan header says.
- No ceiling or b changes after a measured result.
- Development and unit tests never use seeds 1729 or 271828.

## Phase 3 findings so far

Stages 1a to 3b, a1d9325 to e495a37, then the CI fixes up to 54933ed. Stage 4, c17c90a to 4072072, and stage 5a, to 065e5b6, then the second CI round up to 48d30a7. Stage 5b, 9459507 to e98a570, stage 5c to a398d1f with its CI fix in 1029047, then Intel SDE for the 8-lane CI job, 36d701d and a6be05a. Stage 6a, e5d4cc5 to 6216fad, and stage 6b to 23936a9, with its CI fix in 2b6f343. Stage 7a, c1449fd to a4bb6ff, and stage 7b, bcdbc28 to d3a0648, with no CI fix. Stage 7c, 31267b1 to 8cbaaf3. Stage 8, 6bf17a9 to 22e9d3d, with measured run 2 in d70a1d4. Stage 9, 4aa7925 to d605c78, and stage 10, 959315b to 244a453, each green in CI (runs 42983993850 and 43107078217). Then the generated README headline and `RESULTS.md` (e67aa02, 22223a5), the QuantLib flag check's path fix (822acaa), the public layout and licence (eddbaa4, 009605c), and `verify_commits` without rsync (67e340e). Stage 11, one statement order on every compiler, is f131c23. What remains of the phase is in `docs/TASKS.md`, "Iterate". Measured numbers are under each target in `BENCHMARKS.md`, from the files in `results/`.

### Math kernels

- `src/math/` has its own `exp` and `log` (Cody and Waite, minimax fits from `tools/math_reference/generate.py`), `sqrt` on the IEEE instruction, and AS241 with masked tails on them (f23dcd2, a43f40c). Worst measured: `exp` 0.613 ULP, `log` 0.840 ULP, `sqrt` 0.5 ULP, AS241 8.39e-16 relative. The target for `exp` and `log`, stated before measuring, was below 1 ULP.
- `RNG-3` is missed: 8.39e-16 against Wichura's 6.0e-16, at p = 0x1.4f72c68e4f73ep-619. That is AS241 itself on IEEE double, against an exact table. The frozen kernel in `src/rng/` gives the same maximum.
- Phase 2's 8.47e-16 at p = 0x1.5fe4df4c60464p-33, about 1.6e-10, splits into 7.16e-16 from AS241 and about 1.3e-16 from the `erfc`-polished reference (`tests/math/inverse_normal_test.cpp`).
- At -O3, LLVM folded `isnan(y) ? NaN : y` to `y` in one inlining context, so `sqrt(-NaN)` kept its sign bit. LLVM treats NaN sign and payload as unspecified. The host-independence hash test caught it while it was being written. So `sqrt` now sanitises its argument before the root and selects after.
- That fix is in f23dcd2, which was committed before the test. b6ec6bd's message says "sqrt now sanitises its argument", but b6ec6bd adds only the test.
- The hashes reproduce on the Linux host: `release` at d1596e0 passed 505 of 505 there, the hash tests among them.
- Rules for the SIMD form at stage 5, so lanes keep the scalar bits: the same operation order, no `MulAdd`, round to nearest, FTZ and DAZ off, and NaN by sanitise then select.

### Pricer

- `src/pricer/`: `Price(contract, market, RunConfig)`, `EvaluatePath<Real>`, `RunBatches`, and `GbmModel::Law` as the seam for local vol at stage 7 (5fa6df6). The normal CDF is Cody's `erfc` on our own `exp`.
- Path by path against the frozen kernels in `src/mc/`, the worst difference is 0.047 of its stated bound. Threads 1, 2, 3 and 8 give the same bits.
- The normal CDF is within 4 ULP wherever Φ is normal: worst 3.63 ULP. It is not monotone ULP by ULP where Cody's regions join. The largest step down is 2.0 ULP, at |y| = 0.46875, within twice the target.
- Against Φ at the unrounded x, the roundings of √2 and of y add a relative error up to 4y²u, u = 2^-53, about 3e-13 at x = −37. The reference in `src/rng/` has the same, so path-by-path checks do not see it.
- Smoothing follows Savine, *Modern Computational Finance*, section 6.3, pp. 236–238: a linear ramp in spot of half-width s = width × S0. s is a plain double, so the width carries no derivative (section 12.4, p. 426).
- A single one-step knock-in path can be negative, because it is vanilla minus a knock-out on its own conditioned path. Its mean is not. Where p(1 − u) underflows to zero, the reference kernel gives an infinity; the pricer weights the path zero.

Bias of each treatment, for exact GBM steps. Down-and-out call, S0 = K = 100, H = 95, σ 0.3, r 0.1, T 0.2, 50 dates, seed 20261109, N = 4,194,304 (5036729). Bias in SE, value then delta:

| Treatment | Against | Value | Delta |
|---|---|---|---|
| Bridge | continuous O7 | −0.38 | +0.54 |
| One-step survival | discrete O5, O6 | −0.06 | −1.08 |
| Hard indicator | discrete O5, O6 | −0.37 | −1464 |
| Hard indicator, naive delta limit | derived 0.391 | | −0.44 (0.3909) |
| Bridge on the discrete contract | discrete O5, O6 | −106.8 | +57.1 |
| Smoothed, width 0.005 S0 | discrete O5, O6 | −4.5 | +1.8 |
| Smoothed, width 0.01 S0 | discrete O5, O6 | −16.7 | +8.8 |
| Smoothed, width 0.02 S0 | discrete O5, O6 | −61.9 | +37.5 |

The smoothing bias in value is about quadratic in the width. The Euler and local-vol part of the bias waits for stage 7.

For later stages:
- `Select` sends its adjoint to the chosen operand only. `Real(double)` constants are passive. Each worker builds its own path function, and its tape, through `make_path_fn`.
- Not yet branch-free, which matters for SIMD: switches on the contract, `down ?` on a bool, and a knock-in runs two kernels.

### Tape

- `src/ad/` (19d7789): struct of arrays, one statement per operation. `Real` is 16 bytes, a statement 1 byte, an operand 12 bytes (an 8-byte partial and a 4-byte index), an adjoint 8 bytes per statement. Chunks of 2^13 entries, never resized. Thread-local active tape with an RAII scope, mark and rewind per path.
- Bytes per statement are exactly 1 + 12k for k operands per statement, on every product and on every benchmark workload. 0 allocations after warm-up.
- Checks: against the forward-mode `Dual`, worst 0.024 of the bound; dot product 0.0698 of its bound on random programs, 0.024 on priced paths; the 90 run rows, tape against `Dual`, worst 7.19e-4 of the bound, with values bit-identical to `Price()`.
- The round-off bound in `src/ad/bounds.hpp` first had an n·2^-1070 term for underflow. The gradient fuzzer found it failing below the normal range: 45.8 times on a digital path worth 6e-319, and 10.8 times in `OneStepCorner` at p near 1e-316.
- The first fix narrowed the claim to the normal range (040a32f). The lead rejected it: it would have left the check blind exactly where it had found an error.
- It was replaced by the error model of gradual underflow (Higham, section 2.2): a product adds an absolute error of at most 2^-1075, and addition is exact when its result is subnormal. The bound is now 2γ_n·A·(1 + 2γ_n) + 2^-1074·W·(1 + 2γ_n) + 2^-1072, on the whole domain (a645eb6). With it, the corner check near q = 0 (E6 in `tests/ad/one_step_corner_test.cpp`) is within 0.369 of the bound, including 160 checks with p subnormal. A new fault, flushing subnormals, is caught.
- The one-step guard (797ad17, bb13b8e): below q = p·v = 2^-1000 the draw carries no derivative, because dΦ⁻¹(q)/dq overflows there and turns a finite Greek into NaN. For a double the value is unchanged. Prices are bit-identical with and without it on 1,792 paths, 520 of which fire it.
- The guard's reach: v ≥ 2^-53 on the Philox grid, so the guard needs p < 2^-947, a < −36.1. On the live side that takes a drift per step towards the barrier above 36.1 standard deviations. Below that, no path reaches it.
- The guard's bias: every dropped term carries the path's final weight, at most p < 2^-947. So it moves a Greek by less than 2^-947 times the derivative scale of the paths that reach it, and not at all when none can.
- The finite-difference round-off in the operation fuzzer is now the largest over the decoding point and the four stencil points (6932c4e), because it depends on the stencil's values too. The fuzzers run about 6 times slower, and ran clean for 120 s.
- The corner a = 8, u = 2^-53 (`tests/ad/one_step_corner_test.cpp`): the tape's delta is 0.0351 against the true 0.0266. The tape matches the `Dual` there. The loss is in the primal, which rounds q = p·v near 1. The complement form 1 − q = Φ(−a) + p·u would avoid it, and is owed.
- libFuzzer's crash handler hung twice on a fault's abort. The faults that run the tape fuzzers now pass `-handle_abrt=0`, so the run dies on the abort itself (d6f3edd).
- Hand-written adjoint for the GBM arithmetic Asian and European, `src/ad/hand_adjoint.hpp` (16a5211). Its tests live in `tests/ad/dual_agreement_test.cpp`. On 12,288 paths it is bit-identical to the tape for delta, rho and dividend rho on every path, for vega on 10,451 and theta on 10,151. The worst difference is 0.013 of the association bound. Its value is `Price()`'s, bit for bit.
- The hand adjoint skips the reverse pass on a path out of the money, because every adjoint there is zero. The tape does not, except in `tape-skip`. So the benchmark files record the nonzero-payoff fraction: 0.1358 for the barrier, 0.5237 and 0.5240 for the Asians at 12 and 252 fixings.

### Threads

- `RunBatches`: the calling thread and threads − 1 `std::jthread` workers take blocks of 1024 paths in turn. Each worker builds its own path function, so it has its own tape, which records its own 18-statement set-up. No tape is read by another thread.
- Each block's sums, Greeks included, go to that block's own slot. The slots are merged in block order after every worker joins. So nothing depends on which worker ran which block.
- The set-up is swept once per block (`EndBlock`), never per path. Not once per worker: a worker's blocks belong to several batches, chosen by scheduling, so one sweep per worker could not give per-batch Greeks, or their SE, that do not depend on the thread count.
- B11 met with no tolerance: value, every Greek and every batch mean are the same bits for 1, 2, 3 and 8 threads, on all 28 cases, and equal a plain loop (653ea07). A stress run of 49,264 short paths in 64 blocks on 8 threads is within 0.00446 of the bound of one path at a time. TSan is clean on the tape's thread tests.
- B14 met, as above. B13 is not met: every recorded operation reads the thread-local slot once, and on macOS that read is a thread-local lookup. Its cost is unmeasured, because timing it needs a CMake option to build without `thread_local`.

### Benchmarks

- How the numbers are produced is in `BENCHMARKS.md`, "How Phase 3 numbers are produced". The runs at d1596e0 are in `results/mac-m4max/` (2fc28be) and `results/linux-zen1/` (e495a37).
- The h-study rule (`bench/bump_study.hpp`): start from h_0 and halve. D_j is the difference quotient at h_0·2^-j, and Δ_j = D_j − D_{j+1}. Step j is stable when |Δ_j| ≤ R_j + R_{j+1}, R being the round-off scale of D_j. The chosen step is the first j where j and j + 1 are both stable. One stable pair alone can be the truncation passing through zero, two in a row cannot.
- The round-off scale is R = γ_n (mean|V+| + mean|V−|) / |x+ − x−|, with n = 8T + P + B for T statements per path, P paths per batch and B batches. It is first order, so the study keeps halving past the chosen step and fails if any later pair is unstable.
- The bias left at the step is (4/3)Δ_j for central and 2Δ_j for forward differences, reported with its SE. The chosen steps are h_0·2^-9 to h_0·2^-18. On the three pricer workloads the largest bias is 9.67e-5, 0.19% of its Greek's SE.
- The in-run check of a bump against the tape allows b = truncation + t·bias_se + R. The truncation is the study's bound at its own size, which does not shrink with the run's size; R is the run's own round-off scale.
- The in-run check (B33) caught a real bug. A spot bump moved the smoothing half-width with spot, while the tape holds it fixed in currency. That put 0.002 into a delta of 0.004 on `AAD-1`. Bumps now rescale the width so the half-width stays fixed in currency, and the study was rerun (1172aaa).
- Lax-Wendroff: the association bound grows by a step's absolute row sum at each of 2000 steps and overflows to infinity. So the hand-against-tape check there uses a first-order stability scale, 2 × 2000 × γ_8 × 4 × ‖w‖₂ (`bench/lax_wendroff.hpp`).
- The tape costs 166 times the primal on Lax-Wendroff on `mac-m4max` and 106 on `linux-zen1`, against Hogan's 32 to 33 for Adept. The hand adjoint costs 3.37 and 3.24 times, against Hogan's 2.1. Not yet explained: an iterate item in `docs/TASKS.md`.
- Two Linux bugs in the per-variant RSS probe. On Linux, `wait4` reported the parent's peak for every `posix_spawn` child, so each child now reports its own (c1c1a62). And the kernel writes a tab after `VmHWM:`, which the parser did not accept, so every probe failed (d1596e0). The fault `bench-rss-parent-peak` is caught.
- Threaded rows on `linux-zen1` run on two pinned CPUs, 4 and 6, through `AADP_PIN_CPUS=4,6` in `scripts/run_bench.sh` (bc3e6d3). cpu5, the SMT sibling of cpu4, is recorded as meant to be idle, which nothing enforces. The stamp checks each pinned CPU against `measure.sh`'s list, and refuses a two-CPU run from a Debug build (0bde08f).
- 3a's iteration runs are not results and are not quoted here.

### Second-order Greeks

- `src/ad/second_order.hpp` (0d682fc): forward over reverse. A path is recorded over `Dual<double, N>` values, N tangent lanes seeded on N inputs, and swept back once. So one record and one reverse sweep per path give the value, the five first-order Greeks and N rows of the Hessian. There is no second pass.
- N = 1 on spot gives gamma and vanna. N = 2 on spot and vol adds volga, and vanna a second time, which the tests use. The cost model is Naumann et al., NAG TR 5/10, p. 2: a Hessian-vector product costs roughly twice the adjoint, per direction.
- It is a separate tape type. `tape.hpp`, `tape.cpp` and `real.hpp` are unchanged, so the headline multiples cannot have moved. The value lanes form the first-order tape's products in its order, so the value and the five Greeks are bit-identical to the first-order tape on the same kernel. Lanes never mix, so the spot row of N = 2 is N = 1's, bit for bit.
- A pathwise second derivative of max(S − K, 0) is zero on every path and misses the kink's mass. So the pass runs conditional kernels (`src/pricer/conditional_kernels.hpp`), which take the last step's expectation in closed form (Glasserman 2004, section 7.2.3). `Price()` and every first-order Greek still run `kernels.hpp`.
- Barrier gamma: discrete barriers by one-step survival (arXiv 1804.03975v3), continuous ones by one-step survival with the bridge (arXiv 1906.11002v2, Algorithm 2 with σ' = 0, exact for GBM in log space). Both have Lipschitz first derivatives, and Theorem 2.6 there makes the second unbiased against the bridge. The plain bridge weight is never differentiated twice. Hard and smoothed barriers are refused: an indicator's pathwise derivatives are zero, and the smoothed barrier is another, biased contract.
- Bias: none beyond Monte Carlo, for exact GBM steps. Two exceptions, both stated: the one-step guard, copied with its bound, and the reflection exponent, capped at 700 so it cannot overflow. Drift per step is held within 4 standard deviations, so the cap changes a path by less than 1e-300 of the forward.
- Per-path checks (`tests/ad/second_order_test.cpp`): each Hessian row against central differences of the tape's own gradient, worst 0.978 of the bound; vanna both ways, worst 0.0066 of its round-off bound.
- Against references (`tests/ad/second_order_oracle_test.cpp`): Black-Scholes, the geometric Asian and Reiner-Rubinstein by nested Duals over the closed forms, and quadrature for one-step survival and the arithmetic Asian. K = 19, N = 2^20, 64 batches, all pass. 15 faults, all caught (3e4975f, 4072072).
- The bench workloads `aad8-asian12` and `aad8-barrier` (fb97650) time this apart from AAD-1 and AAD-4. See `BENCHMARKS.md`, `AAD-8`.
- Iteration only, not results: `mac-m4max`, macOS, scalar code, 1/50 of the path counts, at 4072072. On `aad8-asian12` the first-order tape on the conditional kernels costs 2.46 times their primal, and forward over reverse 1.60 times that tape on one lane, 1.69 on two. On `aad8-barrier`: 1.60, 1.39 and 1.45.

Disclosed, from the stage's own record:
- The first run of checks 3 and 4 failed, before 0d682fc. Check 3 failed on bridge paths, 3.97e-14 against a bound of 1.25e-14. Check 4 required every partial above 2^-511 and met 1.5e-197 on an Asian path. Now the steps are spot 2^-12 S0 and vol 2^-14, products below the normal range are covered by an absolute floor of 2^-1000, and check 4 counts the paths with a lane below 2^-511.
- The H85 row was added after H95's result was seen: H95's gamma is small against its SE, so it tests little, and H85's is not. K went from 18 to 19, which widens every bound slightly, 5.33e-5 to 5.36e-5 on the first row. The added row is the stricter test, not the family.
- The SE ceilings are loose for the Asians. A ceiling assumes the last step's smoothing sets the whole scale, and an average smooths by 1/count of a step. So for the Asians the ceilings are 3,700 to 32,000 times the SE, against about 50 for the European.

### SIMD

- `src/simd/` (a486c9a): `exp`, `log`, `sqrt`, AS241 and the normal CDF lane by lane, each with its scalar twin's operations in the same order. No `MulAdd`, round to nearest, FTZ and DAZ off, NaN by sanitise then select, as stage 1a set out. Philox keyed on (path, step) per lane. Every product and treatment is branch-free from one lane to the target's.
- So every width gives the scalar bits, and no tolerance is needed. The kernels: about 3.1 million inputs each, 2.1 million for `sqrt`, at widths 1 and 2 on NEON, and 1, 2 and 4 on AVX2 on the Linux host. Prices: mean, SE and every batch mean are `Price()`'s at every width, on 1, 2, 3 and 8 threads. Philox lanes match Random123's known answers, 10 and 7 rounds.
- The Linux host builds AVX-512 and cannot run it: Zen 1 has no avx512f. GitHub's runner in run 41898528776 had every x86-64-v4 feature, so `simd-lanes-8-avx512` ran the 14 SIMD tests at 8 lanes there, and passed.
- `aadp_fuzz_simd_kernels` (22e9884) ran the AVX2 lanes under ASan and UBSan on the Linux host, 900 s, 1,735,574 runs, clean. ExpArray's assembly at each width (NEON, ymm, zmm: no FMA, no calls) is in the research cache, `phase3/evidence/stage5a/`.
- At one lane on NEON, LLVM 23 at -O3 folded the scalar's shape, `IfThenElse(m, Sqrt(IfThenElse(m, x, 1)), NaN)`, to `Sqrt(x)`. So `sqrt(-NaN)` kept its sign, as in stage 1a. The lane form takes `CopySign(Sqrt(Abs(x)), x)`: a real number for every negative x, so no fold can treat it as the NaN it replaces. On x ≥ 0 it gives the same bits.
- A fault restoring the scalar shape changed no bits in the fault runner's build, so the fold depends on context, and that fault was dropped. `simd-sanitise-select`, a bare root, is kept and caught.
- Highway's `Ne` on x86 is the ordered not-equal, false for NaN, while C++ `!=` is true. So x != x is `hn::IsNaN`, and `LaneReal`'s `!=` is `Not(Eq)`. On NEON the two agree, so only an x86 build would show it.
- Two drafted faults changed no bits, so they could not fail and were replaced. Multiplying the gaps first, −2 (d0 d1) for (−2 d0) d1: scaling by −2 is exact. And dropping TwoSum's second correction term from the lane `log`. The final 18 SIMD faults are all caught.
- For stage 5b: `LaneReal` carries values only. The lane kernels copy `kernels.hpp`, and a test holds the copy to it; templating the uniform type would remove the copy. The lane inverse normal computes the tail's `log` and `sqrt` on every lane.
- Speedups, all iteration only, are in `BENCHMARKS.md`, `SIMD-1`.

#### Vector tape: design and oracle (stage 5b)

Written before the code. Pathwise gradients on lanes through one tape per group of N lanes (`src/simd/lane_tape.hpp`): statements and operand indices are recorded once per group, and each lane has its own values, partials and adjoints. The reverse sweep runs across lanes, one vector multiply and add per operand. Stage 5b makes no speed claim; timing, the AAD multiple on lanes and assembly are stage 5c's.

Bytes per entry, against the scalar tape's 16 B Real, 1 B statement, 12 B operand and 8 B adjoint:

| N | Active type | Statement | Operand | Operand per lane | Adjoint |
|---|---|---|---|---|---|
| 1 | 16 (32 on x86) | 1 | 12 | 12 | 8 |
| 2 | 32 | 1 | 21 | 10.5 | 16 |
| 4 | 64 | 1 | 37 | 9.25 | 32 |
| 8 | 128 | 1 | 69 | 8.63 | 64 |

- Active type: the lanes' values as one Highway vector, a 4-byte index and a 1-byte lane mask, padded to the vector's alignment, so 16 B per lane. At one lane on x86 Highway keeps the lane in a 16-byte register, so 32 B there. Asserted at compile time.
- Statement: its operand count, shared by the lanes, so 1/N B per path.
- Operand: a 4-byte index and, for N > 1, a 1-byte lane mask, both shared, and N 8-byte partials: 5 + 8N bytes, 12 at one lane, which needs no mask.
- Adjoint: N doubles per statement, for the sweep only, reported apart as now. The boundary buffer below is sweep scratch too: per operand that names a set-up statement, an index, a mask and N contributions.
- A group records the union of its lanes' statements and operands, so it has at least its largest lane's. Tape bytes per path are the group's S + O(5 + 8N), S statements and O operands, over N.

Reused from the scalar tape:
- Chunking: the same 2^13-entry statement and operand chunks, allocated once and never resized, with no statement's operands straddling two chunks. The adjoint vector covers every allocated statement slot.
- Thread-local slot: one per width, a `thread_local LaneTape<D>*` set by an RAII `LaneTapeScope`; a second activation on a thread is an error. It is separate from the scalar tape's slot, since each worker holds both.
- Mark and rewind: the same `Position`. A group is recorded after the mark, swept back to it and rewound, so the tape holds one group.
- The set-up, swept once per block: it stays on the worker's scalar tape (`ad::PathGradient`), recorded once, as now. The lane tape's indices start at the scalar mark, so a lane operand names a set-up statement by its scalar index. The lane sweep never adds into a set-up adjoint: it appends each contribution to a set-up operand to the boundary buffer, in sweep order. After the sweep the buffer is added into the scalar tape's set-up adjoints lane by lane, in path order, skipping lanes past the block's end. At the block's end the scalar tape's own `SweepSetup` runs, once per block, as now. That replay is where the lanes are reduced in path order.

Lane activity, and which kernels are branch-free on lanes:
- The scalar tape records a statement only for its active operands, and `Select` returns the chosen operand itself, so a path's statements depend on which side each Select took. On lanes each Real carries a mask of the lanes in which it is active, and each operand stores that mask. In the sweep a lane whose bit is clear skips the operand by a select, not a multiply by zero, so there is no 0 × ∞ and no extra add. A statement is recorded when any lane has an active operand. So lane ℓ's sweep does exactly the scalar tape's operations on path ℓ, in its order, and nothing else.
- A Select between an active Real and a constant only sets lane values and masks: the payoff floor, `Max(gap, 0)` in the bridge and the sampled crossing, the smoothed ramp's two clamps, and one-step survival's `Select(alive, p, 0)` and `Select(alive, drawn, v)`.
- The hard indicators, discrete and sampled crossing, are Selects of the constants 1 and 0: lane values and no statement. The weight's product then records one operand, as the scalar tape does.
- The one-step guard below 2^-1000 makes q passive on that path in the scalar tape. On lanes it clears that lane's bit of q, so that lane's draw carries no derivative and the others keep theirs. The guard's reach and bias are the scalar's, above under "Tape".
- So European, digital, both Asians, the bridge, the sampled crossing, one-step survival, the hard indicator, smoothing and knock-ins by parity all run on the vector tape.

Where lanes diverge. A Select in which one lane takes one active statement and another lane a different active statement has no single index for its result. No kernel has one: every Select in `kernels.hpp` chooses between an active Real and a constant, or between constants. `Select` detects it and marks the group divergent. The driver then rewinds the group and runs its lanes inside the block on the scalar tape, one path at a time in path order, into the same set-up adjoints, so the bits are the same either way. Every fallback is counted, in groups and in paths, and returned with the estimate. The tests require zero on every product. A test-only kernel with such a Select forces fallbacks and checks the count against the groups whose lanes split.

Oracle, stated before any test runs. No tolerance anywhere:
1. Per path: on every product and treatment of `tests/ad/cases.hpp` at 1, 12 and 50 steps, at every width the build can run, each lane's value and five-input gradient, after its own set-up sweep, are `PathGradient::Evaluate`'s for that path, bit for bit.
2. Block sums: `PriceWithGradientLanes` gives `PriceWithGradient`'s estimate bit for bit (value and five Greeks: mean, SE and every batch mean) at every width, on 1, 2, 3 and 8 threads, with batches that end inside a block so a group runs past the block's end. Fallbacks zero.
3. B16: on random straight-line programs (`tests/ad/random_program.hpp`) with each lane at its own inputs, each lane's input adjoints are those of a scalar sweep of the same program at that lane's inputs: N scalar sweeps per program, bit for bit.
4. Dot product on lanes: each lane's ⟨y', ȳ⟩ from a lane tangent sweep and ⟨x', x̄⟩ from the lane reverse sweep are the scalar tape's bits for that program or path, and so within `dot_product_test.cpp`'s bound, which is checked too.
5. Forced fallback: counted as above, and the bits still those of the scalar loop.

The basis: each lane's values are the scalar's bits (stage 5a); each partial is the scalar's formula on them; no `MulAdd`, and `-ffp-contract=off`; round to nearest; the sweep's multiply and add in the scalar's order with masked lanes doing nothing; and set-up contributions added in path order, then sweep order, which is the order in which the scalar tape adds them. Nothing is expected to differ. One reservation, stated now: the payload of a NaN, should a gradient ever be NaN, since a compiler may commute a scalar multiply. The tests and fuzzers here require finite Greeks, so it is outside their reach. If one turns up, it is reported, not masked.

Results, at 77dd0c8. Nothing differed from the scalar tape in any bit, at any width:
- Widths run: 1 and 2 (NEON) on the Mac; 1, 2 and 4 (AVX2) on the Linux host, on cores 6 and 7; 1, 2 and 4 in CI's lane jobs. AVX-512 built on the Linux host. CI run 41918421254's 8-lane runner lacked AVX-512, so that job was build-only and the 8-lane tests had not yet run. They have since: in run 41957624701 at a6be05a, under Intel SDE, every vector tape and lane gradient test passed at 8 lanes too (below, "CI").
- The Linux host's full AVX2 ctest at a859c7c was not clean: 526 of 539 passed. `PerfCounters.EachRegionReportsItsOwnTimes` failed while the stage's own jobs loaded the host, and failed again in a pinned rerun; the log of the unpinned rerun keeps only its last lines, so it does not show whether it passed. CI already allows that one test as a named skip on Linux. It needs a look. The 12 skips were the data tests: the synced tree had no data link. At 1078a20, with the data linked, the same ctest passed 547 of 547 with 0 skipped.
- Item 1: 7168 paths per width on each of 1, 12 and 50 steps and the wild and dying markets, all bit-identical.
- Item 2: 28 cases at every width on 1, 2, 3 and 8 threads, 4 batches of 3 · 1024 + 17 paths, all bit-identical; 224 runs at widths up to 2 and 336 at widths up to 4. No pricer kernel fell back.
- Item 3: 8000 programs. 245 diverged at 2 lanes and 587 at 4, as the spread is meant to make them; every other lane matched.
- Item 4: 672, 1344 and 2688 priced paths at 1, 2 and 4 lanes, at worst 0.0195 of the bound. The random programs reached at worst 0.0872.
- Item 5: the diverging kernel fell back on 3064 groups and 6127 paths of 12 356 at 2 lanes, and on 2710 and 10 828 at 4. Each count was exact, and the bits were still the scalar's.
- Tape bytes per path at 252 steps, against the scalar tape's (equal to the 1-lane figures): European call 15 927, 13 651.5, 11 881.8; arithmetic Asian with spot 25 503, 21 841.5, 19 000.8; down-in bridge 73 472, 62 867.5, 54 661.8, at 1, 2 and 4 lanes. The largest group, 4 lanes, held 218 647 B.
- No heap allocation after warm-up at any width. TSan on the Mac: 537 of 537, with no reports.
- Fuzzer, seed 20261126, 900 s each: 481 569 runs on the Mac (NEON) and 79 090 on the Linux host under `fuzz-avx2`, both clean.
- Faults: all 11 new faults caught. The allocation fault first missed: `v = {}` keeps a vector's capacity, so it freed nothing. It now swaps with an empty vector.

#### Stage 5c: lanes in the AAD bench, assembly, and the Zen 1 speedups

Everything below is iteration only, run through `measure.sh`, and was never stamped into `results/`. The raw files are in the research cache, under `phase3/evidence/stage5c/`.

The bench (8e8a464):
- `price-lanes` is `simd::PriceLanes`, and `tape-lanes` is the vector tape, both at the build's widest lanes. They run on the five first-order pricer workloads.
- "x fastest primal" is the faster of `price` and `price-lanes`.
- Every lane speedup is printed beside the lane ceiling. On Zen 1 the ceiling also carries "Zen 1: 128-bit FP datapath" (family 17h below model 30h, read through CPUID).
- Two checks run in every run:
  - `lanes-gradient-bits`: the vector tape's value and five Greeks must be the tape's bits (mean, SE and batch means).
  - `lanes-fallback-zero`: any fallback is reported with its group and path counts, and makes the run INVALID.

Iteration runs at 1/50 scale, 3 repetitions, at 1078a20. Every check passed, and no group fell back on either host. Each range below is over the five workloads:

| host, ISA, ceiling | price / price-lanes | tape / tape-lanes | tape-lanes x scalar primal | tape-lanes x fastest primal |
|---|---|---|---|---|
| `mac-m4max`, NEON, 2 lanes, unpinned | 1.31–1.37 | 1.92–2.01 | 1.32–1.42 | 1.78–1.86 |
| `linux-zen1`, AVX2, 4 lanes (Zen 1: 128-bit FP datapath), cpu4 | 1.96–2.08 | 1.82–2.03 | 1.01–1.11 | 2.00–2.23 |

- On the Mac, the threaded workloads ran 8 threads. On Linux they ran 2 threads, on cpus 4 and 6.
- Lane tape bytes per path:

| workload | scalar tape | 2 lanes | 4 lanes |
|---|---|---|---|
| `aad1` | 17 794 | 15 165.5 | 13 506.2 |
| `aad4` | 1 239 | 1 071.0 | 931.5 |
| `mc-asian252` | 25 479 | 21 820.5 | 18 991.5 |

- Peak RSS of tape-lanes over the primal's: 1.33–1.35 on the Mac and 1.16–1.34 on Linux, on one thread.

B17, whether the scalar primal auto-vectorised:
- It did not. Recompiling `pricer.cpp` at the bench flags with `-Rpass` remarks for the loop and SLP vectorisers shows that no loop in `src/pricer` or `src/math` was vectorised, on NEON or AVX2.
- SLP packed only a few straight-line pairs: Horner pairs (`common.hpp:40`), the scale pair in `exp.hpp:99` and `Magnitude`.
- `OutSmoothed<double>`'s step loop has no packed double arithmetic at any width (`docs/asm/`).

B15, the assembly (`scripts/extract_asm.py`, run on the bench binary; the excerpts are in `docs/asm/neon-2.txt`, `avx2-4.txt` and `avx512-8.txt`):
- The vector tape's operand loop is truly vector at all three widths. Each operand does a packed multiply (adjoint by the stored partials), a packed add into the target's adjoints, and a masked store:
  - NEON: `fmul.2d`, `fadd.2d`, `bif`.
  - AVX2: `vmulpd`, `vaddpd` and `vmaskmovpd` on ymm.
  - AVX-512: `vmulpd`, `vaddpd`, and `vmovupd {k1}` on zmm, with the mask byte loaded by `kmovb`.
- In that loop, one scalar index load per operand is shared by the lanes. There are no gathers, no lane inserts and no scalar double arithmetic.
- The scalar tape's loop is `vmulsd`/`vaddsd`, unrolled 4 times on x86.
- The lane primal has no scalar double arithmetic and no calls: 121 packed operations (NEON), 124 on ymm (AVX2) and 123 on zmm (AVX-512).
- The scalar primal has 47–51 scalar double operations, no packed ones, and one call to `inverse_normal`. That function's conditional branches differ by width:
  - NEON: none. Clang uses `fcsel`.
  - AVX2: 17.
  - AVX-512: 1. It uses mask registers.

Why AVX2 on Zen 1 gives more than 2 (SIMD-1's 3.20, 2.76, 2.83, and 2.73 and 2.82 for the kernels):
- Measured with `aadp_bench_simd` at 1/10 scale, under `measure.sh`, `taskset -c 4`, at 1078a20.
- `perf stat` counts are user space, `(6 repetitions − 1) / 5`, so they cover one repetition of one variant.
- Cycles and instructions are the harness's own counters over the timed region. That pass ran 3 `perf stat` events, so nothing was multiplexed.
- A first pass with 4 events had the harness's counters running 80% of the time, and the harness flagged them "MULTIPLEXED, raw counts not scaled". Its timed counts are not used.

| scalar / 4 lanes | cycles | instructions | IPC scalar → lanes | µops per cycle | FP-pipe µops per cycle |
|---|---|---|---|---|---|
| European | 3.18 | 3.35 | 1.18 → 1.12 | 1.23 → 2.17 | 0.53 → 1.93 |
| Asian, 252 | 2.76 | 2.59 | 0.97 → 1.03 | 1.14 → 2.01 | 0.57 → 1.91 |
| barrier, one-step | 2.83 | 2.26 | 0.78 → 0.98 | 0.85 → 1.92 | 0.85 → 1.65 |
| inverse normal | 2.79 | 2.85 | 1.05 → 1.02 | 1.04 → 2.01 | 0.94 → 1.56 |
| `log` | 2.51 | 4.33 | 1.68 → 0.97 | 1.76 → 2.33 | 0.79 → 2.57 |

- The cycle ratio is the instruction ratio times the IPC ratio, which is what the table shows.
- Zen 1 retires each 256-bit instruction as two 128-bit µops, 1.8 to 2.0 µops per instruction in the lanes against 1.1 to 1.2 in the scalar code.
- The two halves run on two of the four 128-bit FP pipes at once. So the split halves peak throughput, which is what "about 2 lanes' throughput" assumes. It does not double the latency.
- The scalar pricers and the inverse normal are latency-bound, not throughput-bound. They run at IPC 0.8 to 1.2 and use 0.5 to 0.95 FP-pipe µops per cycle out of 4: each path waits on its own chain of polynomials and divisions.
- With four independent paths in each instruction, the lane code keeps the same IPC while issuing twice the µops per cycle. So its speedup is the instruction ratio, 2.3 to 3.4. That is below 4 because the lanes compute both sides of every select.
- The barrier also gains IPC, 1.25 times. Its scalar code has 5.1 million branch misses per repetition, against 0.13 million in the lanes. The rest of that gain is not attributed. The Asian shows the same split, 5.1 million against 0.10 million.
- `log` is the throughput-bound case. Its IPC falls from 1.68 to 0.97 as the FP pipes fill. It still exceeds 2 only because the scalar `log` is different code: 98.6 instructions per value, against 22.8 per value at 4 lanes, with branches the lane form lacks (3.7% of them missed).
- The scalar baseline differs from the lanes' one-lane build too. The scalar pricers take 0.60 to 0.90 of `lanes-1`'s cycles, so `lanes-1` / `lanes-4` reaches 3.3 to 4.5, with the Asian above 4. That figure comes from the first, multiplexed `perf stat` pass, scaled by `perf`.
- No speedup above 2 here is left unexplained. Each is the instruction count of code that was latency-bound, or, for `log`, of different scalar code.

Fallback cost (the test-only diverging kernel, European call, 50 steps, 2^17 paths, one thread, medians of 3):
- Every check passed: the bits are the scalar tape's, the control did not fall back, and the diverging run did.
- Fallen-back groups: 32 848 of 65 536 on NEON, and 28 641 of 32 768 on AVX2.
- The cost of a fallback is the diverging time minus the uniform time, divided by the fallen-back groups:

| host | scalar tape | vector tape, diverging | vector tape, uniform | extra per fallen-back group | a fallen-back group against its paths on the scalar tape |
|---|---|---|---|---|---|
| NEON, 2 lanes | 281.3 ms | 308.7 ms | 187.0 ms | 3.71 µs | 1.53 times |
| AVX2, 4 lanes, cpu4 | 569.6 ms | 682.6 ms | 259.2 ms | 14.78 µs | 1.31 times |

Faults: all 9 new faults were caught (1078a20). One needed a second draft: `bench-lanes-fallback` first edited the `Select` branch that the payoff floor never takes, so it changed nothing. It now edits the branch the floor takes.

Rule slips: two zsh redirections on the Mac, both empty and both writing to `/tmp`: a here-string (`<<< ""`) and a heredoc (`python3 - <<'EOF'`).

### CI

- CI first ran on GitHub as run 41770201004 at bc3e6d3 and failed 9 of 17 jobs: actionlint, clang-tidy, `linux-gcc-release`, `linux-gcc-debug`, `linux-tsan` and the four macOS jobs. The Linux clang, AVX2, AVX-512, ASan and UBSan, fuzz-smoke, format and repo-scan jobs passed.
- actionlint: SC2086 on unquoted GCC configure arguments. Local actionlint had no shellcheck, so it never linted `run:` steps. d8e39d8 quotes them and pins shellcheck 0.11.0, and the fault `check-actionlint-shellcheck` shows the local lint now sees it.
- GCC: g++ 14 `-Werror` failed on a sign conversion and a dangling `else`. The local GCC check then found a possibly null `%s` at -O3. All three fixed in 74f0aac.
- `linux-tsan`: TSan's static runtime on Linux already defines `operator new` and `delete`, so the counting allocator in `tests/ad/allocation_test.cpp` cannot link there. 8d00d85 leaves that one test out of Linux TSan builds only. macOS TSan still runs it, 502 of 502.
- macOS: the four jobs failed to link `std::__1::__hash_memory`. The cause and the move to `xcode-27` are above, under "CI on macOS". What still differs from the measurement Mac: the runner's OS is 27.0.1, not 27.2, the SDK comes with Xcode 27.2, the image is a preview, and the hardware is a VM.
- clang-tidy: CI's Linux clang uses g++ 14's libstdc++, while the Linux host and the Mac use libc++. CI named findings in 8 files. On the Linux host those 8 files gave 10 findings with libstdc++ and 2 with libc++, and the Mac none. 74f0aac's own fix added 2 more, in `tests/math/inverse_normal_test.cpp`. bb7b26f clears all 12, in 9 files.
- bb7b26f also prints every finding in the tidy log, since CI's log named only the files, and uploads the full logs when the job fails (`actions/upload-artifact` pinned to cf430e0, v7.0.2).
- `scripts/check.sh` passes 13 of 13 at 54933ed, with 100 of 100 faults caught. These fixes reached GitHub with stages 4 and 5a, in the push up to 065e5b6.
- Run 41884006533 at 065e5b6 passed 16 of 21 jobs. The four macOS jobs failed `PerfCounters.MacOsCountersAreProcessWideAndLabelled`: GitHub's macOS runners are virtual machines (the runner-images repo builds them as VM images; the jobs now print `kern.hv_vmm_present`), and there `proc_pid_rusage` leaves `ri_cycles` and `ri_instructions` still. Cycle and instruction counts cannot come from a GitHub macOS runner. The harness already wrote such a counter as `missing`; its note now says `unavailable`, which counter, the value it stayed at and whether the host is a VM. The test reads `kern.hv_vmm_present` itself: where it is 0, as on the measurement Mac, both counters must count; on a VM, each counter must count or be labelled unavailable with that reason. `HarnessSelftest.MacOsHeaderNeverWritesAStuckCounterAsACount` checks the same in the header, and the fault `bench-darwin-stuck-counter`, a counter that stayed still reported as a count of 0, is caught on every host. No test is skipped for this.
- Run 41884006533 also failed `linux-tsan`: `HarnessSelftest.RefusesAHistogramOfBatchesTooShortToResolve` and run_bench.sh's failed-run check relied on one unit of work (16 multiply-adds) being below the gate. Under Linux TSan it takes about 200 ns, at the gate of 100 to 200 ns: on debian the test failed too and the script check passed only because its median, 190 ns, fell under that run's 200 ns gate. Both now use the selftest's `--no-work`, a batch of two timer reads, 60 ns there.
- The macOS runners start a job at nice -10, which the stamp tool refuses, so `aadp_run_bench_script` failed there. The test steps raise niceness by the measured offset to 0, and print both levels.
- CI's Linux clang jobs (debug, release, asan-ubsan, tsan, avx2, the lane jobs, fuzz-smoke and `clang-tidy`) now use libc++ from the pinned LLVM 23.1.4 release, as the Linux host does: the setup action writes the host's `clang++.cfg` (`-stdlib=libc++` and a link-time rpath to `lib/x86_64-unknown-linux-gnu`), checks that the release ships libc++ there, and probes which library a program gets. The GCC jobs stay on libstdc++, and `clang-tidy-libstdcxx` keeps tidy on it, since its findings differ by library.
- The `check-actionlint` fault matched twice once 065e5b6 added the lane matrix's second `runs-on` line, so it silently stopped applying. dca8324 makes its find unique again.
- Run 41898528776 at 48d30a7 passed 22 of 22 jobs. Each Linux test job had the one named allowed skip, above; the macOS jobs had none. Both tidy jobs report 126 units clean, on libc++ and on libstdc++.
- `scripts/verify_commits.sh` needed rsync, which the Linux host lacks (and installing it is off limits there). 67e340e replaces `rsync -rlc --delete --exclude=/data` with `scripts/sync_tree.py --keep data`, standard-library Python: a file is rewritten only when its bytes differ, so unchanged files keep their mtimes and each commit's build stays incremental; what the commit dropped is deleted; links stay links; the top-level `data` link is neither replaced nor deleted, nor written through. One difference, on purpose: permissions follow the source, where rsync without `-p` left an existing file's mode alone; a mode change keeps the mtime. The sync script comes from the verifier's own tree, not from the commit under check, so the fixed version checks older commits too. `tests/data/verify_commits_test.py` (ctest `data_verify_commits_script`) runs the sync alone and then the verifier on a scratch repo with a PATH that has no rsync. The faults `check-verify-sync-rewrites-all` (every file rewritten) and `check-verify-commits-replaces-data-link` (no `--keep data`) are caught: `python3 -I scripts/fault_injection.py --only check-verify-sync-rewrites-all,check-verify-commits-replaces-data-link`, caught=2. Manifest: 357 faults (62 oracle, 267 check, 28 fuzz).
- Stage 5b's runs 41918421254 and 41924618330 passed 22 of 22, but on both the 8-lane runner lacked AVX-512, so that job was build-only with a warning. Run 41898528776's runner had it. Whether a green run had tested 8 lanes depended on the runner GitHub assigned.
- Run 41942650525 at a398d1f passed 16 of 22. Both tidy jobs failed on `tests/bench/platform_test.cpp`, whose x86-only CPUID test (8e8a464) the Mac's tidy cannot see: unchecked optional access, and `find` where tidy wants `contains`. The four lane jobs that ran tests counted `aadp_simd_fallback_cost_smoke_not_a_result` as skipped, because their build target list lacked that binary (added in 7b97155), and a skip fails a job. 1029047 fixes both. So 8e8a464 to a398d1f fail CI's tidy, and 7b97155 to a398d1f its lane jobs. Run 41953763165 at 1029047 passed 22 of 22, with the 8-lane job build-only again.
- Stage 6a's run 41996756050 at 6216fad passed 22 of 22 on its first run.
- Run 42018873540 at 23936a9, stage 6b's push, passed 21 of 22: `clang-tidy-libstdcxx` failed on two findings that only libstdc++ shows. In `src/data/forwards.cpp`, assigning `*row.settlement_price` to a `std::optional<double>` dereferences an optional only to rewrap it (`bugprone-optional-value-conversion`), and `tests/data/forwards_test.cpp` included `<cstddef>` without using it directly (`misc-include-cleaner`). The Mac's tidy, on libc++, reported 151 units clean at 8ae3239, and CI's `clang-tidy` job on libc++ passed in the same run. Both lines came in with 9328a13, so 9328a13 to 23936a9 fail that CI job. 2b6f343 assigns the optional as it is and drops the include. Run 42023625872 at 2b6f343 passed 22 of 22. Since then, the files a stage touches also go through tidy against libstdc++ on the Linux host before a push.
- Stage 7a's run 42137605150 at a4bb6ff passed 22 of 22 on its first run. Stage 7b's pushes passed 22 of 22 each: 42721885949 at bcdbc28, 42728823678 at e1da8dc (352cf29 went up with it and had no run of its own) and 42732808933 at d3a0648.

#### Intel SDE for the 8-lane job

- Since 36d701d, `simd-lanes-8-avx512` tests 8 lanes on every run. Where `/proc/cpuinfo` lists every x86-64-v4 feature it runs the simd label natively, as before. Otherwise it runs it under Intel's Software Development Emulator as a Skylake-X, `sde64 -skx --`, passed to CMake as `CMAKE_TEST_LAUNCHER`. Skylake-X has every feature of Highway's AVX3, the build's static target, which `Smoke.HighwayMatchesTheConfiguredTarget` checks under the same launcher.
- The job prints the CPU's model and the mode, and checks that every listed simd test will run in that mode. If SDE cannot be downloaded the job fails as BUILD ONLY, so a green run always means the 8-lane tests ran.
- Pin: SDE 10.13.1, released 2026-07-28, from Intel's direct mirror, `https://downloadmirror.intel.com/924984/sde-external-10.13.1-2026-07-28-lin.tar.xz`, sha256 `94e97d62…2b50` (in full in `ci.yml`). That is the SHA256 Intel's download page lists for the file. `actions/cache` keeps the archive under a key of version and hash, and the hash is checked before it is unpacked, so a wrong archive fails at once. It is fetched in CI only and never committed.
- Licence: the Intel Simplified Software License. Its October 2022 text: "You may use and redistribute the software, which is provided in binary form only, (the "Software"), without modification". SDE's own documentation: "free of charge for any type of use". This repo only downloads it. The mirror URL needs no login, cookie or form. Intel's download page puts a licence-acceptance dialog in front of its button, and the CI job prints the archive's licence files to its log. The version named differs by source: August 2021 in the archive's documentation, October 2022 on Intel's licence page, and August 23, 2023 in the SDE article. The texts read are in the research cache, `phase3/sde/`.
- Run 41957624701 at a6be05a passed 22 of 22. The 8-lane runner was an AMD EPYC 7763 without avx512f, avx512bw, avx512cd, avx512dq or avx512vl, so the tests ran under SDE 10.13.1 `-skx`, with Highway at AVX3. 24 of 24 simd tests passed, 0 skipped, in 705 s of ctest real time.
- Those 24 tests include every lane kernel and price test, and every vector tape and lane gradient test: per-path bits, block sums on 1, 2, 3 and 8 threads, B16's random programs, the dot product, the counted fallback, and bytes per path. So stage 5a's lane primal and stage 5b's vector tape are now verified bit for bit at 8 lanes too. A passing test's printed counts are not in the CI log, so the 8-lane counts are not recorded. Times under SDE measure the emulator, and none is used.
- a6be05a adds the fault `check-actionlint-sde-launcher`: the configure step reads the SDE launcher from a step id that does not exist. The 8-lane code itself cannot be faulted locally, since no host here runs AVX-512 and SDE runs only on GitHub's runners.

### Stage 6a: implied volatility, raw SVI and the arbitrage checks

No market data is used in this stage.

#### SVI-1 tolerance, stated before any measurement

A quote at volatility σ is priced by `Black76Price` (Let's Be Rational's `Black` times the discount factor D) and inverted by `ImpliedVolatility`, giving σ̂. It passes only if both of these hold:

- vol: |σ̂/σ − 1| ≤ 8 η (1 + C);
- price: |V(σ̂)/V − 1| ≤ 8 η (1 + 1/C).

C = V / (σ ∂V/∂σ) is the conditioning of the full price as quoted, intrinsic value included: a relative error e in V moves σ by about C e. It comes from Jäckel's `BlackAccuracyFactor`, which evaluates it in scaled form, so it does not underflow in the wings. η = ε + 2^-1074 / V, with ε = 2^-52, one ulp at 1. The second term is the fixed spacing of subnormal doubles, which only matters for a price below about 1e-292.

The reason for each part:
- The price is computed with a relative error of order (1 + 1/C) ε, the attainable accuracy Jäckel gives for the Black function (the comment on `BlackAccuracyFactor` in `lets_be_rational.h`). The inversion maps that to (1 + C) ε in vol, since C times 1/C is 1.
- The inversion itself attains (1 + C) ε: Jäckel's "maximum attainable precision" (SVI-1), whose first-order form is his `ImpliedVolatilityAttainableAccuracy`.
- Discounting and undiscounting add two roundings, which is ε in price and C ε in vol.
- So there are three sources of about (1 + C) ε each, and the factor 8 gives each about 2ε, with margin. For the price, the same three sources apply with 1/C in place of C: a σ̂ off by one ulp moves the price by 1/C ulps.

"Machine precision" means this attainable level, not ε. Where C is large, no digit of σ is attainable in double, and the bound says so. Where 8 η (1 + C) ≥ 1, the vol bound cannot fail; those points are counted and reported, not hidden. In the deep in-the-money wings this is most of the points, because the time value is lost below the intrinsic value's last digit.

Statuses: every grid point must return `ok`. The exception is a point whose computed price equals the discounted intrinsic value exactly, with no time value left in double, which must return `at-intrinsic` with vol 0. Those are counted.

The grid:
- T ∈ {0.0137, 0.0274, 0.0833, 0.25, 0.5, 1, 2, 5} years, so down to 5 days;
- σ ∈ {0.01, 0.03, 0.1, 0.2, 0.4, 0.8, 1.6, 3.2};
- z = ln(K/F) / (σ √T) from −36 to 36 in steps of 0.5;
- calls and puts, F = 100, D = exp(−0.03 T).

That is 8 × 8 × 145 × 2 = 18,560 quotes. Out of the money, with s = σ √T, Let's Be Rational's normalised price b = V / (D √(FK)) is about φ(z) e^(−s²/8) s / (z² − s²/4), at least about 1e-288 on the grid, so always normal. The price itself is b D √(FK), and √(FK) = F e^(z s / 2): above F for calls, but as small as F e^(−129) for the deepest puts. So at the far put wing with large s the price is subnormal or zero. η's second term makes the vol bound vacuous at a subnormal price, and a zero price equals its intrinsic value and must return `at-intrinsic`; both are counted. (A first draft of this paragraph said every grid price was above DBL_MIN; that was corrected before the test first ran.) The test prints the worst point by ratio to the bound, and the worst raw error in ulps among points with C ≤ 1.

An independent price check guards against a wrong archive, a wrong patch or a wrong call/put flag, which a round trip through the same code cannot see. On the grid points with |z| ≤ 6 and s = σ √T ≤ 3, `Black76Price` must agree with the oracle's Black-Scholes closed form (`src/oracle/black_scholes.hpp`, with S = F and r = q = −ln D / T) within 2^-36 D (F N(d1) + K N(d2)). The reason: the oracle's d1 and d2 carry an absolute error of about 2ε (|ln(F/K)|/s + s/2), at most 18ε there. N's elasticity, |d| φ(d) / N(d), is below d² + 1 ≤ 58 for |d| ≤ 7.5, so each term is good to about 1000ε, or 2.3e-13 relative, plus erfc's own few ulps. 2^-36 is 1.5e-11, a margin of about 60. A wrong flag or discount is an O(1) error.

#### SVI-1, first run, and what changed

The first run (macOS, and the same numbers with g++ 14 on the Linux host) failed: of 18,560 quotes, 11,288 were `ok`, 6,397 `at-intrinsic`, 790 `above-maximum` and 85 `below-intrinsic`, and 7 vol errors were over the bound. No price error was over its bound, and the oracle check passed (3,000 prices, worst 4.05e-15 of the terms). Two causes, one in the wrapper and one in the test:

- The wrapper compared V / D with the undiscounted bounds, so the division's rounding put prices that sit at a bound in double on either side of it: 85 deep in-the-money quotes came out below intrinsic, and 790 deep in-the-money calls, where F − K and F are the same double, came out at or above the maximum D F. It now compares V with D times each bound, as the statuses were documented, tests the intrinsic value before the maximum, and treats a V / D that rounds onto the intrinsic value as `at-intrinsic`. With that, every grid point is `ok` (11,216) or `at-intrinsic` (7,344, each with no time value left in double).
- The test applied the vol bound at `at-intrinsic` points too, where vol 0 is what the status rule above demands, so the error there is 1 by construction. At 7,337 of them the bound is vacuous anyway; at the other 7 (C ≈ 1.5e14 to 3e14, bound 0.26 to 0.53) it is not. The bound is a linearisation; at those points the price is flat in σ in double, every σ up to some value (the quote's own among them) giving the same double, so no vol is attainable to any digit whatever C says. The test now judges `at-intrinsic` points by the status rule and the price bound only, and lists those 7. The tolerance itself is unchanged, and it is applied in full to every `ok` point. This reading of the stated rule was chosen after the run, so it is recorded here as such.

Fuzzing also found two cases outside the grid, fixed before this run: a vol below DBL_MIN is now an error (`vol-underflow`), and the fuzzer's own round-trip property counts the subnormal spacing at the normalised price V / (D √(FK)), not only at V.

#### Stage 6a results

Let's Be Rational: the live archive's SHA-256 on 2026-11-04 was the Phase 1 pin, da2f6870…2470 (8,191,851 bytes, Last-Modified 2024-02-18). Its notice permits use, copying, modification and distribution if the notice is preserved; NOTICE quotes it. It is fetched, never committed, and built as its own target without our warning flags and with SYSTEM headers; no per-target flag was needed. clang rejects the identifier `inverse_norm_cdfm½_for_midrange_probabilities` in every language mode, so the build renames it (3 uses) in the build tree; g++ 14 builds the archive unchanged and without a warning. CI restores the archive from `actions/cache`, checks its hash, and fetches it over http only on a miss.

SVI-1 (macOS, clang 23.1.4, and the same on g++ 14 on the Linux host), after the fixes above: 18,560 quotes, 11,216 `ok` and 7,344 `at-intrinsic`, none other. Over the `ok` points the worst vol error is 0.25 of its bound (5.6e-16, 2.5 ulps, C = 0.23, z = −1.5, σ = 0.01, T = 0.0833, put) and the worst price error 0.27 of its bound (1.3e-15, 6 ulps, C = 0.57, z = +0.5, σ = 1.6, T = 0.0833, call). Where the bound is near ε (C ≤ 1 and a normal price), the worst vol error is 3 ulps; where 1/C ≤ 1, the worst price error 2.5 ulps. The smallest out-of-the-money price on the grid is 1.6e-319, subnormal, as the corrected paragraph above expects. The Black-Scholes check: 3,000 prices, worst 4.05e-15 of the terms against 2^-36 (1.46e-11).

SVI-2, iteration only, not a result: 400 out-of-the-money quotes (|z| ≤ 3, T from 0.0137 to 2, σ from 0.1 to 0.8) through `measure.sh` on macOS, 5 repetitions: 140 ns per inversion (56.1 µs per set, CV 0.09 %), against 20.9 ns per `Black76Price`. `BENCHMARKS.md`, `SVI-2`, now carries this line as iteration only. No stamped file exists: the bench has no harness main, so the stamp refuses its output, and its reported run is in `docs/TASKS.md`, "Iterate".

The checks, none of which repairs anything; each violation carries its kind, its k range, worst k, value and bound:
- butterfly grid: k from −3 to 3 in steps of 0.001, joined with m + σ y for y from −40 to 40 in steps of 0.01, so about 14,000 points, 100 per σ around the minimum of w. Reason: [−3, 3] is e^±3 of the forward, far beyond the settlements' strikes, and w'' has width σ. ε_g(k) = 32 ε c_w S, with S the sum of the magnitudes of g's terms and c_w the condition of w's sum: about 1e-14 on these slices, against Vogt's −0.033;
- d+ limit: b (1 + ρ) < 2 (MM Lemma 2.1, so that GJ Lemma 2.2's limit holds); Lee: b (1 ± ρ) < 2, a parameter only so the test can show what 4 passes;
- Martini-Mingone: Theorems 5.10, 6.2 and 6.3 with the waterfall of sections 5.5 and 6.3, ρ = +1 by reflection, σ* on a 4,096-point grid in h = 1/l refined by golden section (MM section 6.2.1);
- calendar: the same union grid for both slices, with ε_w = 16 ε (|w1| + |w2| term sums); and the asymptotes w ~ β|k| + c + q/|k| compared in that order, with tolerances 4 ε, 8 ε and 4 ε of the magnitudes. No necessary and sufficient condition is known (MM, conclusion); the API says so.

Known violators: Vogt (SVI-3) is flagged on k ∈ [0.643, 1.256], 762 points, worst g = −0.0329 at k = 0.879, and the classifier gives type 3 with α = −0.09872, μ = 0.86347, the interval ]−0.72407, 0.82939[ and F = −0.12663, each within 5e-6 of MM section 5.5.1. Slope 2.25 (SVI-4; a = 0.02, b = 1.5, ρ = 0.5, m = 0, σ = 0.2) fails the wing check at 2, the d+ limit and the classifier (type 1), and passes the wing check at bound 4. The crossing pairs (SVI-5): one near the money (gap −0.005 on k ∈ [−0.128, 0.128]) is caught by the grid only, one in both wings by the grid and by the slopes, one with slopes lower by 1e-3, crossing near k = 71 and −38, by the asymptotes only, and one at the 1/|k| order by the asymptotes. Known good: the six arbitrage-free rows of MM Table 1 (min g from 0.0099 to 0.26), GJ Example 5.1's repaired slice (JW to raw by GJ Lemma 3.2; min g 0.258) and SSVI slices of GJ eq. (4.5) with γ = 1/2 and η (1 + |ρ|) ≤ 2 (three surfaces, seven θ from 4e-4 to 1) pass every butterfly check, and adjacent SSVI slices the calendar check. At σ*/2 and 2σ* of each Table 1 row, the classifier and the grid agree (type 4 with negative g, and free).

Faults: 22 added, all caught at 4dafb00 on macOS. Two (`svi-inversion-put-flag`, `svi-calendar-wing-q`) first did not compile, an unused parameter and an unused variable under -Werror, and were rewritten before the commit.

Fuzz (`aadp_fuzz_svi`, ASan and UBSan, macOS): trials of 60, 120 and 180 s found the cases above, then 300 s ran clean; the stated run, 600 s through heavy.sh at the committed code, made 15,056,557 executions with no failure (coverage 652, corpus 461 from 17 seeds).

Process notes: the first macOS gate of the inversion commit tested a stale build, because restoring files with their old timestamps hid the CMake change from ninja; it was rerun before any commit. g++ 14 rejected an unbraced `if` around `EXPECT_STRNE` (-Wdangling-else), fixed before the commit. One Linux pre-commit run failed `PerfCounters.EachRegionReportsItsOwnTimes` (29.11 ms enabled against a 29.7 ms floor) while another project's measurement held the host; it passed on the rerun, and the code under test was not changed by this stage.

### Stage 6b: parity forwards, calibration set and the raw SVI fit

The fit is to TAIFEX settlements of one day, 2024-11-15, which are not tradable quotes, and the surface reaches 33 days only.

#### Pre-registration, stated before any fit

This was committed before the fit code existed and before any fit, on real or synthetic data. Anything decided after a fit is recorded below as such.

Inputs, from Phase 1's definitions (`chain-verify/scripts/profile_chain.py` and `profile_recommended.py` in the research cache), stated again in code:
- Parity: for each expiry, every strike with both a call and a put row in the regular session, both settled (Phase 1's `settle_all`). Ordinary least squares of the settlement C − P on K, so D = −slope and F = intercept / D. The residual RMS divides by n, as Phase 1's does, and σ_leg = RMS / √2: the C − P residual carries two independent legs.
- Already known from Phase 1's `parity.csv`, before any code: `settle_all` gives the published F and D, but its RMS / √2 is 1.80, 2.42 and 1.98 points, not the 1.87, 2.43 and 1.55 in "Recommended calibration set". Those come from `settle_near` (strikes within 5% of the TX settlement), whose RMS `profile_recommended.py` hard-codes, rounded to two places (2.65, 3.44, 2.19), before dividing by √2. Unrounded, the near expiry's is 1.8752, which rounds to 1.88. The method here is the brief's, `settle_all`, and the weights use its σ_leg. The test reproduces Phase 1's numbers by Phase 1's route as well, so that the difference is shown and not hidden.
- T: calendar time from 13:45 on 2024-11-15 (regular-session close, Taipei) to 13:30 on the expiry date (2024-11-20, 2024-11-27, 2024-12-18), in seconds over 365 × 86,400. The year basis is ACT/365 in seconds. Taipei has no daylight saving, so local times subtract directly.
- Calibration set: `select_calibration_set` with the regressed forwards, unchanged. The input butterfly counts use Phase 1's definition: in each slice and each right, strikes sorted, every three adjacent rows; with w = (K3 − K2) / (K3 − K1), the excess is P2 − (w P1 + (1 − w) P3); a triple is non-convex if the excess is above 1e-9 points and beyond half a tick if it is above w h1 + h2 + (1 − w) h3 + 1e-9, h the half-tick of each price. Ticks follow TAIFEX Rules Art. 7: 0.1 below 10 points, 0.5 below 50, 1 below 500, 5 below 1,000, 10 above. The 1e-9 is Phase 1's guard against a decimal difference that rounds; the smallest half-tick is 0.05.
- Oracles: F to two places, D to five and σ_leg (by Phase 1's route) to two, as printed; and the full-precision values in `parity.csv` within a relative 1e-9 (two implementations of the same centred regression in double differ by rounding, about 1e-13 here; one strike more or less moves F by far more than 1e-9). F within 0.92 points of the TX settlement where a TX month exists (202411 and 202412), to the printed rounding. Counts 65, 51, 64 and the butterfly totals 168, 51 and 23, exactly.

Objective and weights. A slice is the calibration set of one expiry with its F, D and T. Point i has strike K_i, log-moneyness k_i = ln(K_i / F), settlement V_i and the noise scale σ_i, with σ_i² = σ_leg² + tick_i² / 12, tick_i the tick at V_i: a uniform rounding to the tick has variance tick² / 12. No volume or spread weights. The model price is `Black76Price(F, D, K_i, T, √(w(k_i) / T))`, w raw SVI. The objective is Φ = Σ r_i², r_i = (V_model,i − V_i) / σ_i, reported with Φ / (n − 5).

The fit, in the quasi-explicit form of Zeliade (De Marco and Martini, 2009): for fixed (m, σ), with y = (k − m) / σ, w = a + d y + c √(y² + 1), c = b σ, d = ρ b σ, so w is linear in (a, d, c).
- Inner solve, for fixed (m, σ): Gauss-Newton on Φ in (a, d, c). At the current w_i, J_i = (∂V_i / ∂w) / σ_i, with ∂V / ∂w = D F φ(d1) / (2 √w) for calls and puts alike. One step minimises Σ J_i² (t_i − w_i(a, d, c))², t_i = w_i − r_i / J_i, a weighted linear least squares in (a, d, c). So the price-space weight 1 / σ_i² maps to the variance-space weight J_i², the price's sensitivity to w over its noise, squared. The first step is taken at the market total variances ŵ_i = T σ̂_i² (σ̂_i from Let's Be Rational), where r_i = 0: that is the classical quasi-explicit solve with vega weights. Its fixed point has Σ J_i r_i ∂w_i / ∂(a, d, c) = 0, the stationarity of Φ itself, so iterating it gives the price-space optimum for that (m, σ), not a variance-space proxy. Each step is a convex quadratic programme in three variables with the linear constraints below, solved exactly by trying every set of at most three active constraints and keeping the best feasible point. The step is accepted with backtracking, α = 1, 1/2, ... to 2^-40, on Φ itself and on positivity. Stop when Φ falls by at most 1e-13 of itself, when no α gives a decrease, or after 50 steps (then the evaluation is marked not converged). A J_i below 1e-300 drops that settlement from the step, never from Φ.
- Domain of the inner solve. Zeliade's is 0 ≤ c ≤ 4σ, |d| ≤ c, |d| ≤ 4σ − c, 0 ≤ a ≤ max ŵ_i. Kept: |d| ≤ c (|ρ| ≤ 1, which gives c ≥ 0, b ≥ 0), c + |d| ≤ 4σ (b (1 + |ρ|) ≤ 4) and a ≤ max ŵ_i. Changed, decided now with this reason: a ≥ 0 is replaced by positivity of the minimum total variance, a + √(c² − d²) > 0, that is a + b σ √(1 − ρ²) > 0. a ≥ 0 is sufficient for w > 0 but not necessary, and it excludes slices whose hyperbola lifts the minimum above a: Vogt's slice has a = −0.041 and minimum variance 0.0116, and on 5-day slices b σ √(1 − ρ²) can exceed the at-the-money variance, about 4e-4, so a ≥ 0 could bind for no reason of arbitrage. Positivity is not linear, so it is held by the backtracking (the set is convex and every iterate is inside it); the starting point is the first quasi-explicit solve if it is positive, else the flat slice (min ŵ_i, 0, 0). The bound 4 is Zeliade's and is not Lee's 2: a slope above 2 is left for the wing check to find, never prevented.
- Outer search over (m, σ): m ∈ [k_min − Δ, k_max + Δ], Δ = k_max − k_min over the settlements; σ ∈ [1e-4, 4]. 1e-4 is twenty times below the smallest strike spacing in k (50 points at 22,800, 0.0022), where the data cannot tell a narrower hyperbola from a kink; 4 is far beyond the data's width in k, where the hyperbola is a parabola on the data. A grid of 9 values of m evenly over [k_min, k_max] by 9 values of σ log-spaced over [1e-3, 1], each evaluated by the inner solve (the profile objective). Nelder-Mead in (m, ln σ) from the 5 grid points with the lowest profile objective (ties in index order), with the initial simplex one grid step in each coordinate, coefficients 1, 2, 1/2, 1/2, trial points clamped to the box. A start converges when every vertex lies within 1e-10 of the best in both m and ln σ, or when the three vertex objectives are equal doubles; at 600 profile evaluations it stops unconverged. The result is the start with the lowest Φ; the report says how many starts reached within 1e-8 of it, relatively. No randomness: the grid is fixed, so there is no seed.
- Fit errors are values, never exceptions: too few settlements (fewer than 5 distinct strikes), a non-finite or non-positive F, D or T, a bad settlement (strike, price or σ_i not finite and positive), a settlement with no implied volatility, and no feasible start.

Bounds, and what an active bound records. The fitted parameters are raw (a, b, ρ, m, σ), b = c / σ and ρ = d / c (ρ = 0 when c = 0). Each constraint gets its slack in natural units and is recorded active when the slack is at most 1e-9 of its scale: ρ = −1 and ρ = +1 (1 ∓ ρ), b = 0 (c against max ŵ_i), b (1 + |ρ|) = 4 (against 4σ), a = max ŵ_i, m and ln σ at either end of the box (against the box's width), and the minimum variance at 0 (against max ŵ_i). The QP sets an active constraint exactly (d = −c gives ρ = −1 as a double, not −0.9999999). Stage 7's implicit-function adjoint needs this: at an active bound the fitted parameter does not move with the settlements, so the derivative is taken on the free parameters only, and a plain inverse Hessian would be wrong there. Every active bound and every slack is printed; none is hidden.

Metrics, with no weights:
- `SVI-6`: (1/n) Σ |V_model,i − V_i| / F, in basis points of the forward, against the 4 bp context, which is eSSVI on SPX mids (BENCHMARKS.md).
- `SVI-11`: the Zhang-Lin-Lu formula on one slice, √((1/n) Σ (σ_model,i − σ̂_i)²), σ_model,i = √(w(k_i) / T) and σ̂_i the market implied vol from the settlement, in vol points (100 times the decimal), against 0.73.
- Violations are those of stage 6a's checks, run on the fitted parameters exactly as fitted: `CheckButterfly` with the default grid and the slope bound 2 (invalid parameters, non-positive variance, g below −ε_g on the grid, the d+ limit, either Lee wing, a Martini-Mingone verdict other than no arbitrage), and `CheckCalendar` between each pair of adjacent fitted slices (a crossing on the grid or in an asymptote). Each record counts as one violation, with its kind, k range and worst value. They sit beside the input butterfly counts, which count triples, a different unit; for a like-for-like line, the same triple count is also run on the model prices at the settlements' strikes. Nothing is repaired: slices are fitted independently, with no calendar constraint, and a fitted slice with a violation is reported as fitted.

Synthetic recovery, on hand-made parameters only. Three slices shaped like the calibration set: F = 22,800, strikes on a 100-point grid, out of the money, only prices of at least 1 point; σ_i from σ_leg = 2 and the tick rule.
- A: T = 5/365, D = 0.9999, (a, b, ρ, m, σ) = (1e-4, 0.012, −0.45, 0.002, 0.025), strikes 19,700 to 24,100.
- B: T = 12/365, D = 0.9995, (2e-4, 0.02, −0.35, 0.005, 0.04), strikes 19,500 to 24,500.
- C: T = 33/365, D = 0.998, (5e-4, 0.04, −0.5, 0.01, 0.06), strikes 18,000 to 25,000.
- Noiseless: the prices are `Black76Price` at the true parameters. Pass if the fit converges, Φ̂ ≤ 1e-10, and every parameter j satisfies |θ̂_j − θ_j| ≤ 2 (√(Φ̂ C_jj) + √(Φ_true C_jj)), with C = (JᵀJ)⁻¹ from the analytic Jacobian of r in the raw parameters at θ̂, and Φ_true the objective at the true parameters (the prices' own rounding). The reason: near the minimum Φ(θ̂) − Φ(θ*) ≈ δᵀJᵀJδ, so any δ the stopping rule leaves has |δ_j| ≤ √((Φ(θ̂) − Φ(θ*)) C_jj) (Cauchy-Schwarz), and the rounding moves the minimiser θ* from the truth by at most √(Φ_true C_jj) in the same way; 2 allows for the quadratic approximation. So the tolerance is the optimiser's convergence, read through the conditioning, parameter by parameter. Φ̂ ≤ 1e-10 is a weighted residual RMS near 1e-6 of a noise unit, many orders above what the stopping rules leave.
- Noisy: V_i + σ_i z_i, z_i = Φ⁻¹(u_i), u_i = `PathUniform(20261105, i, slice)` (Philox, a development seed, not 1729 or 271828), one draw per slice. Pass if every parameter is within 4 standard errors of the truth, SE_j = √C_jj at θ̂. Fifteen comparisons at 4 SE fail together with probability about 1e-3 in the linear Gaussian approximation. A failure is reported; the seed is not changed.
- ρ at its bound: A with ρ = −1 exactly must fit ρ within the noiseless tolerance of −1. A with ρ = −1.05, a slice outside the domain whose variance is still positive on its strikes, must fit ρ = −1 exactly with the bound recorded active, and its mirror (ρ = +1.05, m = −0.002, strikes reflected in k) ρ = +1 likewise.
- No repair: slices at T = 1, F = 100, D = 1, strikes K = 100 e^k for k from −1.5 to 1.5 in steps of 0.05, out of the money, σ_i = 0.01, built from Vogt's parameters and from the slope-2.25 slice of `SVI-4`, must be recovered within the noiseless tolerance, and the checks must then report the density violation (Vogt) and the wing and d+ violations (slope 2.25) on the fitted parameters. A crossing pair, B and a copy of B at T = 33/365 with a lowered by 1e-4 (so below B at every k), fitted slice by slice, must be reported by the calendar check.

Timing (`SVI-7` per slice, `SVI-8` for the surface) is iteration only, through `measure.sh`, kept in the research cache and not in the report.

#### Stage 6b results

Settlements of one day, not tradable quotes; the surface reaches 33 days only. Data: TAIFEX via data.gov.tw, Open Government Data License (NOTICE).

Parity, `settle_all`, as pre-registered: F, D, their standard errors and the residual RMS reproduce Phase 1's `parity.csv` within a relative 1e-9. F = 22,761.47, 22,786.07, 22,792.92 and D = 0.99987, 0.99944, 0.99829, as printed. σ_leg is 1.80, 2.42 and 1.98 points; Phase 1's 1.87, 2.43 and 1.55 come back exactly by its `settle_near` route (above). F − TX is −0.53 for 202411 and +0.9207 for 202412, which is 0.92 to the printed places; 202411W4 has no TX month. T on ACT/365 in seconds: 431,100, 1,035,900 and 2,850,300 s, so 0.0136701, 0.0328482 and 0.0903824 years. The calibration set has 65, 51 and 64 rows. Input butterflies per slice are (triples, non-convex, beyond half a tick) = (61, 15, 3), (47, 19, 13) and (60, 17, 7), which total 168, 51 and 23. Every number above matched on its first run.

The fit (`aadp_svi_fit`, macOS, through `measure.sh` at 75fa774; identical to the last digit at c05b538). The stamp tool refused the report ("first line is not the results magic"): it stamps only timing runs, with their run header. So the report stays in the research cache and nothing goes under `results/`.

| | 202411 | 202411W4 | 202412 |
|---|---|---|---|
| n | 65 | 51 | 64 |
| a | −0.0026677 | −0.0019272 | −0.070714 |
| b | 2.009347 | 2.008524 | 2.036356 |
| ρ | 0.990696 | 0.991513 | 0.964294 |
| m | 0.112363 | 0.150447 | 0.587762 |
| σ | 0.0111257 | 0.0114447 | 0.137556 |
| active | b (1 + \|ρ\|) = 4 | b (1 + \|ρ\|) = 4 | b (1 + \|ρ\|) = 4 |
| Φ, Φ / (n − 5) | 12.42, 0.207 | 106.89, 2.324 | 191.03, 3.238 |
| SVI-6 (bp of F) | 0.287 | 1.132 | 1.231 |
| SVI-11 (vol points) | 0.958 | 0.410 | 1.397 |
| model triples non-convex | 0 of 61 | 0 of 47 | 0 of 60 |
| Martini-Mingone | type 1 | type 1 | type 1 |

All three converged, and all 5 starts reached the best Φ. Every slice has the same shape: ρ near +1, its vertex m to the right of the data (k_max = 0.057, 0.105 and 0.199), and the right wing at the slope bound 4. The settlements lie on the left branch of the hyperbola, so the fit spends the right wing, which no settlement constrains, on curvature. As fitted, each slice fails stage 6a's checks beyond its data. Density: g < 0 from k = 0.120, 0.161 and 0.683, with worst g −28.9, −24.9 and −3.26. The right wing and the d+ limit fail (slope 4 against 2), and the Martini-Mingone verdict is type 1. At the settlements' strikes the model prices are convex (0 non-convex triples), against 15, 19 and 17 among the settlements. Calendar: 202411 → 202411W4 crosses on k ∈ [0.071, 3], inside the W4 settlements' strikes from 0.071 to 0.105, and on k ∈ [−3, −0.733], and both wing asymptotes cross. 202411W4 → 202412 crosses on k ∈ [0.127, 6.09], with the right asymptote crossing. Nothing was repaired. SVI-6 is below the 4 bp context, which comes from eSSVI on SPX mids and so is not like for like. SVI-11 is above 0.73 for 202411 and 202412 and below it for 202411W4.

Synthetic recovery, at c94296d. Noiseless: errors are 0.30 to 0.50 of the tolerance on A, B and C, 0.21 to 0.50 on Vogt and 0.10 to 0.40 on slope 2.25, and no bound is active on A, B or C. Noisy (n = 40, 50, 70; Φ = 28.9, 43.0, 58.8): the largest |z| is 1.265, for ρ of A. A's fit has a = −8.1e-5 < 0, the case for which a ≥ 0 was dropped. ρ = −1 fits −1 exactly, with the bound active. ρ = −1.05 and +1.05 fit −1 and +1 exactly with that bound active (Φ 0.0112 and 0.0114), and the reported parameters give the reported Φ. The fit leaves Vogt's density violation, the slope-2.25 wing and d+ violations and the crossing pair in place for the checks to report.

Linux host, the same commit (75fa774), clang 23.1.4, run under `heavy.sh` with `taskset -c 6,7`. These differences are reported, not judged. T, F, D, σ_leg, n and every count are identical. The active bounds are identical, and every fit converged. The largest absolute differences are a 1.4e-8, b 3.4e-9, ρ 3.3e-9, m 4.4e-8 and σ 2.0e-8 (all 202412), Φ 4.7e-11 (relative 2.5e-13), SVI-6 1.6e-8 bp and SVI-11 3.5e-7 vol points. The parameters move along the flat valley of a fit at its slope bound, while Φ agrees to 13 digits.

Timing, iteration only and not a result (`measure.sh`, macOS, 50 repetitions): `SVI-7` median 82.0, 42.7 and 60.4 ms per slice; `SVI-8` median 186.7 ms for the three fits and every check.

Faults: 29 added. In the first run 25 were caught. Four did not compile (an unused parameter, variable or constant under -Werror) and were rewritten. All 29 were then caught at 8ae3239 on macOS.

Fuzz (`aadp_fuzz_svi_fit`, ASan and UBSan, macOS): trials of 60 s and 180 s and a 300 s run at 45eb4dd. The 300 s run found a fit with b = −1.9e-17: the QP holds |d| ≤ c only to its feasibility tolerance, so c came out at −1.9e-20. b is now reported at its bound (c05b538), and the input is a regression seed. The stated run, 300 s through heavy.sh at c05b538, made 126,226 executions with no failure (coverage 771, corpus 502 from 11 seeds).

Decided after the pre-registration, each before the first real fit unless stated:
- The noisy slices apply the 1-point floor after the noise is added, so a quote can fall out (A has 40 quotes noisy and 42 noiseless).
- Nelder-Mead sorts its vertices with a stable sort, so ties order the same with every standard library. The synthetic outputs were unchanged.
- The real-file chain test asserts only what was pre-registered (sizes, counts, weights, that every fit returns and every check runs), not fitted values.
- After the fuzz finding: the clamp of c at 0 in the conversion to raw parameters. The real and synthetic fits are unchanged to the last digit.
- After writing the faults: the ρ-beyond-bound test also checks Φ at the reported parameters, to FitResultFields' tolerance, because the conversion's clamp of ρ would otherwise hide a solve that ignored ρ ≥ −1.

Process notes: the first run of the real-file chain test expected no TX month for 202411, which was wrong (the realdata parity test already knew it has one). The test was fixed before its commit. The code was unchanged.

### Stage 7a: local volatility off the raw SVI surface

The surface is the stage 6b fit to TAIFEX settlements of one day, 2024-11-15, which are not tradable quotes. It reaches 33 days only, so every contract here matures within 33 days.

#### Pre-registration, stated before any pricing on the real surface

This was committed before the code that implements it, and before any price on the real or a synthetic surface: drafts of that code existed and had been compiled, but none of it had been run. The only real-surface numbers used to write it are the fitted parameters, ranges and violations already printed in "Stage 6b results". Anything decided later is recorded below as such.

The surface stays raw SVI per slice, as fitted, violations and all. The 45 `svi-surface` rows of the plan (`tools/run_oracles/runner.cpp`) are derivatives of the price with respect to the raw parameters (a, b, ρ, m, σ) of each slice, so the pricing surface must be a function of exactly those 15 numbers. Repairing it, or replacing it with eSSVI, would change what the rows mean. Stage 6b found calendar crossings between both adjacent pairs and butterfly violations beyond the data on all three slices, so floored local variances are expected. They are counted and located, never hidden (`SVI-9`). The arbitrage-free comparison is eSSVI, at 7c.

Slices. j = 1, 2, 3 are 202411, 202411W4 and 202412, at T_j = 0.0136701, 0.0328482 and 0.0903824 years (ACT/365 in seconds), with the parity F_j, D_j and the raw SVI w_j(k) of stage 6b, k = ln(K / F_j). The slice parameters are double, and on the tape they are the 15 inputs.

Time interpolation. Total variance is linear in T at constant k between adjacent slices: for T_{j−1} ≤ t < T_j, w(k, t) = (1 − α) w_{j−1}(k) + α w_j(k), α = (t − T_{j−1}) / (T_j − T_{j−1}), so ∂T w = (w_j(k) − w_{j−1}(k)) / (T_j − T_{j−1}). w′ and w″ in k are interpolated the same way, since they are linear in the slices. Before the first slice, w(k, t) = (t / T₁) w₁(k), so ∂T w = w₁(k) / T₁, the implied volatility at fixed k is constant, and w(k, 0) = 0. Beyond the last slice, 33 days, nothing is extrapolated: a contract maturing after T₃ is refused. The reason for this scheme: by Gatheral and Jacquier's Lemma 2.1 (arXiv 1204.0646; proportional dividends) the surface is free of calendar arbitrage exactly when ∂T w ≥ 0, and here the sign of ∂T w at (k, t) is the sign of w_j(k) − w_{j−1}(k), so every calendar crossing stage 6b found becomes a negative ∂T w at those k, nowhere else, and is counted. Gatheral and Jacquier's Lemma 5.1 interpolates call prices instead, and preserves absence of static arbitrage only between slices that are themselves free of it and do not cross; ours are not, so its guarantee does not apply, and its interpolated smile is not SVI, so local variance would need price-space derivatives. eSSVI's scheme is for 7c.

Forward and discount curves. Spot is the nearest parity forward discounted to today, S₀ = D₁ F₁ = 22,758.59, which assumes no dividend over the first 5 days. ln F(t) is linear in t between the nodes (0, ln S₀), (T_j, ln F_j), and ln D(t) between (0, 0) and (T_j, ln D_j). So carry μ(t) = d ln F / dt and rate r(t) = −d ln D / dt are piecewise constant. From the printed F and D: carry 0.00926, 0.0563 and 0.00522, rate 0.00926, 0.0229 and 0.0200 on the three segments. Carry is implied by F, not by D: in the second segment it is above the rate, which the settlements imply and which is left as it is. The curves are passive doubles; F, D and S₀ carry no derivative.

Dupire local variance, at time t and spot S, with k = ln(S / F(t)): σ²_loc = ∂T w / g, where g is Gatheral and Jacquier's eq. (2.1) on the interpolated w, w′, w″ at t, written as
  g = (1 − k q / 2)² − w′ q / 4 − w′² / 16 + w″ / 2,  q = w′ / w.
That is the working denominator of QuantLib's `LocalVolSurface` and of the R code in Gatheral's VW4 notes, expanded (NOTES, "SVI"). VW4's displayed eq. (12) has the opposite sign on the (1/4)(1/4 + 1/w) w′² term; its own R code, QuantLib and g all agree on the minus sign, and Phase 1 checked it numerically (`svi/EVIDENCE.md`, C1 and E74). Rates and carry enter only through k, because the surface is in forward log-moneyness (Gatheral, *The Volatility Surface*, ch. 1). Before the first slice q = w₁′ / w₁, because the factor t / T₁ cancels, and w′ = (t / T₁) w₁′, w″ = (t / T₁) w₁″. So g is defined at t = 0, where it is (1 − k w₁′ / (2 w₁))².

Floor rule. With ω = 2^-200 (about 6e-61), floor = 1e-4 and cap = 4 per year (local volatility 1% and 200%), each (k, t) is classified in this order, on the double values, which are the same bits for double, the tape and the Dual:
1. any of w, ∂T w or g not finite: floored, reason non-finite;
2. w ≤ ω (before the first slice, w₁ ≤ ω): floored, non-positive variance;
3. ∂T w ≤ ω: floored, calendar;
4. g ≤ ω: floored, butterfly;
5. ∂T w ≤ floor · g: floored, below the floor;
6. ∂T w ≥ cap · g: capped;
7. otherwise σ²_loc = ∂T w / g.
A floored point takes σ²_loc = floor and a capped one cap, as constants. Reasons:
- The floor must be positive. One-step survival divides by the step's standard deviation and the bridge by its variance, so a zero local variance would make both 0 / 0. At 1% the step has almost no diffusion: where ∂T w ≤ 0 the surface says the forward variance is negative, and the floor adds no variance it does not have.
- A cap is needed, because as g falls to 0 from above, ∂T w / g grows without bound, and one step at an unbounded variance would carry a path past the finite range. 200% is about four times the highest implied volatility any fitted slice reaches inside its own settlements' strikes, 45% (202411 at k = −0.155 and 202412 at k = −0.354, from the 6b parameters). A local volatility can exceed every implied one, which is why the cap is not lower.
- ω makes "≤ 0" exact in floating point with every recorded partial finite. The tape's reverse sweep multiplies every partial by its statement's adjoint even when that adjoint is zero, so an infinite partial on a floored branch would still turn a Greek into NaN. Above ω, on the validated domain (|a|, |b|, |m| ≤ 1e4, σ ≥ 1e-8, |k| ≤ 400, expiries and their gaps at least 1e-6), every partial of q, g and ∂T w / g stays below about 1e125. A total variance or a g below 6e-61 is zero for any price.
- Steps 5 and 6 test the ratio by multiplication, so the division happens only when its result lies between floor and cap.
Consequences, stated here and not measured at 7a: a floored or capped point carries no derivative. Where g crosses zero, σ²_loc jumps from cap to floor, so the payoff is discontinuous in the parameters there, and the pathwise derivative misses that jump. Stage 8's independent reference, such as finite differences on common random numbers, would see it.

Counting. Every Euler step of every path is classified once, in the production runs (not in the per-path checks, which may record a path more than once). Reported per run: path-steps, floored by reason, capped, floored and capped each as a share of path-steps, the paths with at least one floor, and, for floored and capped points separately, the smallest and largest k and t, overall and per time segment. Expected from stage 6b's checks: before the first slice, density violations on 202411 from k = 0.120; between 202411 and 202411W4, crossings for k ≥ 0.071 and k ≤ −0.733; between 202411W4 and 202412, crossings for k ≥ 0.127. The counts are reported beside those ranges, whatever they are.

Model law: log-Euler with the local volatility frozen at the start of each step. With dates t_i = i T / m, i = 1..m, as in the GBM pricer, and n Euler sub-steps per date, step s runs from t_s = s Δt to t_{s+1}, Δt = T / (m n), and
  x_{s+1} = x_s + (ln F(t_{s+1}) − ln F(t_s)) − σ²_s Δt / 2 + σ_s √Δt z_s,  σ²_s = σ²_loc(t_s, x_s − ln F(t_s)).
So the step's law is normal, with that mean and standard deviation √(σ²_s Δt), and goes through the `Law` seam of `src/pricer/model.hpp`. With a constant σ_loc this is exact GBM with the curves' carry, which is what the flat-surface oracle tests. The registered grid is n = 4: 33 dates and 132 steps of about 6 hours.
- Discrete barrier, one-step survival: monitored at the dates only. The sub-step that ends at a monitoring date survives with p = Φ(gap(mean) / sd), its own mean and sd, and is drawn from its law conditioned on the live side, as in the GBM pricer. The other sub-steps are plain. That step law is the scheme's exact conditional law, so the estimator is unbiased for the Euler scheme's discretely monitored price, and the remaining bias is Euler's.
- Continuous barrier, bridge: every sub-step carries 1 − exp(−2 d₀ d₁ / (σ²_s Δt)), the step's frozen variance. That is the exact survival of the Brownian bridge with the frozen coefficient, so the estimator is unbiased for that interpolation of the scheme, and against the local-volatility diffusion its bias is first order in Δt under Gobet's (2000) payoff conditions ("Barrier treatment" below; abstract only). It is measured, not assumed.
- The sub-stepped kernels are a separate header, so the GBM kernels, the lanes and the second-order code are unchanged. At n = 1 they must give `EvaluatePath`'s bits on the GBM model.
- Refused: a maturity beyond the last slice; products other than European, arithmetic Asian and knock-out barrier; `include_spot`; a surface that fails validation; and, as in the GBM pricer, any contract whose paths could leave the finite range, with the cap's √4 as the largest volatility.

The three `svi-surface` contracts, which stage 8's 45 rows will use:
- Common terms: S₀ = D₁ F₁ = 22,758.59; maturity T = T₃ = 0.0903824 years, the 202412 expiry (2024-12-18 13:30, 33 days); m = 33 dates t_i = i T / 33, about one calendar day apart; payoff discounted by D(T) = D₃; strike K = 22,800.
- Arithmetic Asian: call on the average of S at the 33 dates, S₀ not included.
- Discrete barrier: down-and-out call, K = 22,800, barrier H = 21,500, monitored at the 33 dates, by one-step survival.
- Continuous barrier: down-and-out call, K = 22,800, H = 21,500, monitored continuously, by the bridge on every sub-step.
- Why these terms: K and H lie inside every slice's settlement range of k, whose intersection is [−0.1106, 0.0571]: K is at k = 0.0017, 0.0006 and 0.0003, and H at −0.0570, −0.0581 and −0.0584. Between and around those strikes the surface is constrained by settlements in every slice. Outside, its right wing is extrapolation at the slope bound 4, with density violations from k = 0.12 and crossings from k = 0.071, so a contract struck there would price the fit's extrapolation rather than the data. Paths still travel outside the range, and their floors are counted. A maturity of 33 days spans all three slices, so every one of the 15 parameters reaches the price.

The 15 sensitivities, by tape. The inputs, registered in slice order (202411, 202411W4, 202412) and in each as (a, b, ρ, m, σ), are the only active numbers. The set-up records them and, per slice, σ² and b σ². Everything else per step (the time segment and weight, Δt, ln F(t_s) and its increment) is a passive double fixed by the contract, so per path the tape holds only the path's own operations, which read the set-up's statements. As in the GBM pricer, each worker records its own set-up, each path is swept back to the mark and rewound, and the set-up is swept once per block (`EndBlock`), so the batch means and SE of all 15 do not depend on the thread count. The derivative is the partial derivative of the Monte Carlo price with F, D, T and S₀ held fixed, on the surface as fitted: no refit, which is 7b's implicit-function step.

Tolerances, each stated before its check exists:
- Flat-surface oracle. Three slices with b = 0 and a_j = σ² T_j (σ = 0.2; ρ, m and the SVI σ arbitrary), F_j = S₀ e^{r T_j}, D_j = e^{−r T_j}, r = 0.02 and no dividend, so that S₀ = D₁ F₁ holds. Then w = σ² t exactly, w′ = w″ = 0, g = 1 and σ²_loc = σ², with no floor and no cap, which the test asserts. On the same uniforms, the local-volatility model must equal the GBM model (spot S₀, σ, r, q = 0, T) in the same sub-stepped kernel, path by path, for European, arithmetic Asian, every barrier treatment, n = 1 and 4, 4,096 paths each, development seed. Log-Euler with a constant volatility and deterministic carry is exact GBM, so the two agree in exact arithmetic; in double they differ by rounding, since the local-volatility model forms its drift, variance and discount by other operations. Bound: |V_LV − V_GBM| ≤ 2^-30 D(T) max(S₀, K) per path. Reason: each step's mean and sd differ by a few ulps, which over 132 steps moves log S by at most about 5e-13 absolutely; the bridge factor's sensitivity to a gap is at most about 6 / sd, near 1,200 here; so a path moves by at most about 1e-11 of D max(S₀, K), and 2^-30 (9.3e-10) leaves a margin near 100. A modelling error (a lost −σ²/2, a wrong Δt, the carry of the wrong segment) moves a path by 1e-5 or more of that scale. Separately, the sub-stepped kernels at n = 1 on the GBM model must be bit-identical to `EvaluatePath`, with no tolerance, and at n = 4 a test-side loop written out by hand must give their bits for the Asian and the discrete hard barrier, so that the fixing and monitoring stride is checked by something other than the kernel itself.
- Euler bias, measured, no pass mark, iteration only. Per product and treatment: the arithmetic Asian; the discrete down-and-out call by one-step survival, hard indicator and smoothing (half-width 0.005 S₀); the continuous one by the bridge and by sampled crossing; all on the real surface with the contract terms above. Levels n = 1, 2, 4, 8, 16. For each n up to 8, Δ(n) = V(n) − V(2n) on coupled draws: the fine level uses the production draws at 2n, bit for bit the production price, and the coarse step j uses z = (z_{2j} + z_{2j+1}) / √2, mapped back to a uniform by Φ and clamped to the open grid, with the crossing uniform of fine step 2j. Each level is an unbiased estimator of its own scheme, because the coarse uniforms are independent U(0, 1); the coupling only shrinks the variance of Δ. N = 2^20, 64 batches, seed 20261115. The SE of Δ comes from the 64 batch differences. Reported: V at each level, Δ(n) ± SE, the ratios Δ(n) / Δ(2n), and the bias left at the registered n = 4, estimated as 2 Δ(4) under first-order weak convergence, which the ratios test. A Δ(4) within 2 SE is reported as not resolved at this N.
- Per-path tape against the `Dual`. The tape's gradient in the 15 parameters must agree with the forward-mode `Dual` of `tests/support/`, path by path and parameter by parameter, within `AgreementBound` of `src/ad/bounds.hpp`, the existing tape bound: 2 γ_n A (1 + 2 γ_n) + 2^-1074 W (1 + 2 γ_n) + 2^-1072, n = 2 T + O from the path's own tape. The `Dual` has five tangents, so each path runs three times, one slice seeded each time. Its value and the tape's must be the double kernel's bits. Cases: two synthetic surfaces, one free of static arbitrage (checked by stage 6a's checks first) and one shaped like the fitted surface, with ρ near +1, the vertex to the right of the data and crossing slices, on which floors must occur on the checked paths; every product and treatment; n = 1 and 4; 256 paths each. Then the real surface in the real-data test, the three contracts, 64 paths each.
- Dupire check, two parts.
  - Finite differences of ∂T w and g, on synthetic surfaces only: the two above. Grid: k from −0.3 to 0.3 in steps of 0.01; t at 0.1, 0.5 and 0.9 of each of the three segments, and t = 0 for g. ∂T w by a central difference in t, h_t = 2^-10 times the segment's length, both points inside the segment. g from w, w′ and w″ by central differences in k of the interpolated w, h_k = 2^-10 min σ over the slices involved (for t = 0, of w₁). Tolerances: |Δ_t − ∂T w| ≤ 2^-30 S_N, S_N = (|w_{j−1}| + |w_j|) / (T_j − T_{j−1}), or |w₁| / T₁ before the first slice, because w is linear in t inside a segment, so the difference has no truncation and its rounding is about 2^-42 S_N; |g_FD − g| ≤ 1e-6 S_g, S_g the sum of the four terms' absolute values, because at h_k = 2^-10 σ the truncation of w′ and w″ is about h² w‴ / 6 and h² w⁗ / 12, below 1e-7 of the terms' scale for raw SVI, and the rounding is smaller still. Only points with w > ω.
  - Dupire in call prices, which the first part cannot replace: it shares g's formula, so a wrong denominator, such as VW4's displayed sign, passes it. On the arbitrage-free synthetic surface with F = 100 at every slice and D = 1, so that k = ln(K / F) for every t and σ²_loc(K, T) = 2 ∂T C / (K² ∂KK C): C by `Black76Price` (Let's Be Rational), the out-of-the-money option at each K, at t the midpoints of the three segments and k from −0.3 to 0.3 in steps of 0.01, where the point is not floored or capped, g ≥ 0.05 and |k| / √w ≤ 2.5. h_K = 2^-9 K √w, h_T = 2^-10 times the segment. Tolerance |v_FD / σ²_loc − 1| ≤ 1e-4. Reason: the second difference's truncation is about (h_K / (K √w))² / 12 times C⁗ / C″, a few times 1e-6 relative; its rounding is about 4 ε C / h_K², below 1e-5 of C″ in that range of k even when g = 0.05 shrinks C″ twenty times; the T difference is far smaller.
- Floor rule, exact: synthetic points built to fall in each class must be classified as such, counts must add up to the path-steps, and the tallies must be the same for 1, 2, 3 and 8 threads, as must every price, Greek and batch mean.

Runs on the real surface, iteration only (`aadp_local_vol_report`):
- Prices and Greeks: the three contracts, n = 4, N = 2^20, 64 batches, seed 20261115 (a development seed), 8 threads: price ± SE, the 15 sensitivities ± SE, and the floor report.
- Euler bias: the plan above.
- Diagnostic, no tolerance: a European call at K = 22,800 maturing at each slice's expiry, beside that slice's Black-76 price at w_j(k), to show what the floors, the cap and Euler do to the vanillas the surface came from. Its grid is the production one as near as it goes: round(33 T_j / T₃) dates (5, 12 and 33) and n = 4.
- Timing: through `measure.sh` on `mac-m4max`, single thread, N = 2^16, 5 repetitions, median wall time per product for the price alone and for the price with the 15 sensitivities. Iteration only, macOS.
- The real-data test asserts only what is stated here: the surface builds from the fit, S₀ = D₁ F₁, the three contracts validate, a maturity one second past T₃ is refused, prices and tallies are finite and consistent, and tape against `Dual` holds on 64 paths per contract. Its price run is 4,096 paths, 8 batches, 4 threads. The synthetic tests and the real-data test use the development seeds 20261116, 20261117 and 20261118; the bias tool clamps a coarse uniform to [2^-53, 1 − 2^-53], the ends of the Philox grid.

Fuzz: the surface and local-variance entry point, and a short local-volatility path on the tape, from raw bytes with synthetic parameters, 300 s through `heavy.sh` under ASan and UBSan. Invariants: validation errors are values; every local variance is finite and within [floor, cap], with a classification that matches its values; the tape's values are the double's bits; the gradient is finite and within the agreement bound of the `Dual` on the seeded slice.

Every check above gets a fault in `faults/manifest.json`, VW4's displayed sign among them.

Out of scope here: sensitivities to the settlements through the implicit function theorem (7b), eSSVI (7c), stage 8's references and pilots, the vector tape on the surface.

#### Stage 7a results

Decided after the pre-registration, each before the run it affects:
- Dupire part 1, g. The registered bound, 1e-6 S_g, failed on synthetic surface B at 31 of 610 points, all on its left wing (k from −0.29 to −0.10); A passed at 0.088 of it. The failure is in the bound, not in g: at the failing points the same differences in 60-digit Decimal err by 2e-11 to 9e-11, and in double by 4e-7 to 1.1e-6 (`evidence/stage7a/g_fd_rounding.py`). The registration said the rounding was "smaller still", which holds for w of the size of its terms but not here, where ρ x + √(x² + σ²) cancels (ρ = 0.985, x < 0): w is 0.006 while its terms are near 1. The bound now adds the differences' own rounding, R = E (|∂g/∂w| + |∂g/∂w′| / h_k + 2 / h_k²), with E = 2^-51 S_w, S_w = Σ |c_j| (|a_j| + b_j (|ρ_j x_j| + r_j)) weighted as w is, and E taken as about four roundings of S_w per evaluation of w. h_k, the grid and 1e-6 S_g are unchanged. With R, the worst point is at 0.74 of the bound on B and 0.088 on A. The test prints both ratios and the count beyond the registered bound.
- Dupire part 2, prices. The first run failed at k = 0 in the first segment, with Dupire −2.3e-5 against ∂T w / g = 0.027: the test chose put or call at each strike of the second difference, so at K = F it mixed a put at K − h_K with calls at K and K + h_K, which adds (F − K + h_K) / h_K² to it. The type is now that of the centre strike, at every point of the difference. Then 76 points pass, the worst at 2.1e-6 of the 1e-4 bound.
- Batch against path, a check the registration did not list, with its bound stated before it first ran: the batch gradient sums each path's adjoints into the set-up and sweeps it once per block, so it must be the mean of the per-path gradients within 1e-10 times the mean of their absolute values. Only the order of the same additions differs, about 2^-52 times a few thousand statements at most, while a set-up sweep skipped, repeated or misread is off by whole multiples.
- Tape against `Dual`, a second check, added after the first fault run: a fault that shuffled the real-data test's gradient passed it. `AgreementBound` sums absolute partials, which grow geometrically along a path where the local variance is steep in k, so it is far above the derivatives themselves: on the real surface the smallest bound is 1.2e46 times the path's largest |derivative|, so it detects nothing there, and on surface B at n = 4 it is loose by the same mechanism. It stays as registered, and each difference must also be within 2^-26 of the path's largest |derivative| over the 15. Not a proof: 2^-26 is about 2,000 times the largest difference seen before it was written (7.3e-12 of the parameter's own derivative), and a wrong or misplaced derivative is off by the order of the scale. Measured with the check in place: worst 5.1e-14 of the scale on the synthetic surfaces and 5.2e-14 on the real one.
- The surface test now also locates a slice's own time, t = T₁, as the start of the next segment with weight 0, after a fault that moved that node passed the first fault run.
- Seven faults did not compile under `-Werror` in the first run (an unused variable or function left behind by the mutation), and were rewritten to keep their intent and compile: the curve midpoint, ρ's bound, the q guard (twice: test and fuzz), the carry taken at the step's start, the range check and the floored-path count.
- The report tool has `--mode report|bias|timing` and `--paths`, so that one tool gives the three registered runs; the plan named only the tool.

Settlements of one day, not tradable quotes; the surface reaches 33 days only. Data: TAIFEX via data.gov.tw, Open Government Data License (NOTICE). Every number below is iteration only: none is stamped, and the outputs are in the research cache (`evidence/stage7a/`), not under `results/`.

Synthetic checks (macOS, at 5347509), as registered:
- Flat surface against GBM, path by path, 4,096 paths, n = 1 and 4, nine contracts: the worst path is at 1.0e-4 of the 2^-30 D max(S₀, K) bound (the bridge at n = 4); n = 1 is `EvaluatePath`'s bits, and the hand loop at n = 4 gives the Asian's and the hard barrier's bits.
- Tape against `Dual`, 256 paths per case, surfaces A and B, seven contracts, n = 1 and 4: the worst difference is 6.6e-4 of `AgreementBound` and 5.1e-14 of the path scale, and values are the double kernel's bits. On B, 2,086 of 98,560 checked path-steps floored, as the registration requires; none on A. The batch gradient agrees with the mean of the per-path gradients, as the new check below asks.
- Dupire part 1: ∂T w within 1.4e-4 of its bound on A and B; g as in the decisions below. Part 2: 76 points, worst |Dupire in prices / (∂T w / g) − 1| = 2.1e-6, bound 1e-4.
- Prices, Greeks and tallies are the same bits for 1, 2, 3 and 8 threads, and the tallies add up to the path-steps.

The real surface (`aadp_local_vol_report`, macOS, N = 2^20, 64 batches, seed 20261115, 8 threads, n = 4: 132 steps). S₀ = D₁ F₁ = 22,758.588763. Carry 0.009263, 0.056322 and 0.005225 and rate 0.009263, 0.022856 and 0.019997 on the three segments, as registered. Price ± SE, then d price / d parameter ± SE:

| | Asian call | discrete down-and-out call, one-step | continuous down-and-out call, bridge |
|---|---|---|---|
| price | 336.794 ± 0.440 | 570.356 ± 0.829 | 552.838 ± 0.820 |
| 202411 a | 35,315 ± 590 | −3,807 ± 911 | −2,342 ± 1,025 |
| 202411 b | 56.84 ± 0.93 | −5.88 ± 1.46 | −4.56 ± 1.66 |
| 202411 ρ | −8,194 ± 96 | 486 ± 152 | 440 ± 197 |
| 202411 m | 339.8 ± 7.0 | 54.8 ± 12.4 | 61.3 ± 13.3 |
| 202411 σ | 6,813 ± 181 | −1,299 ± 285 | −898 ± 296 |
| 202411W4 a | 51,787 ± 231 | 2,368 ± 412 | 661 ± 500 |
| 202411W4 b | 88.34 ± 0.36 | 2.62 ± 0.68 | −2.16 ± 0.84 |
| 202411W4 ρ | −15,569 ± 55 | −358 ± 100 | 771 ± 138 |
| 202411W4 m | 584.6 ± 2.1 | 41.4 ± 8.0 | 3.6 ± 8.9 |
| 202411W4 σ | 7,904 ± 49 | 391 ± 101 | 389 ± 107 |
| 202412 a | 17,247 ± 59 | 56,778 ± 164 | 50,696 ± 152 |
| 202412 b | 635.8 ± 2.2 | 2,088.9 ± 6.0 | 1,863.3 ± 5.6 |
| 202412 ρ | −20,640 ± 65 | −67,092 ± 176 | −59,514 ± 165 |
| 202412 m | 330.6 ± 0.7 | 1,015.6 ± 1.8 | 874.1 ± 2.2 |
| 202412 σ | 8,002 ± 30 | 26,654 ± 85 | 23,940 ± 79 |

Floors, per contract over 138,412,032 path-steps. The Asian and the bridge share their paths, so their tallies are equal; one-step survival conditions the dated sub-steps, so its paths differ.
- Asian and bridge: 532,933 floored (0.385%): 521,608 calendar and 11,325 below the floor; no non-finite, non-positive or butterfly point. 34,156 paths (3.26%) have at least one floor. Floored k from 0.0713 to 0.1344, t from 0.0137 to 0.0897; by segment, none before T₁, 181,408 in [T₁, T₂) at k 0.0713 to 0.1134, 351,525 in [T₂, T₃] at k 0.1264 to 0.1344. Capped: 52 path-steps on 8 paths, in [T₂, T₃], at k from −1.201 to −1.001 and t from 0.0760 to 0.0897.
- One-step: 534,901 floored (523,535 calendar, 11,366 below the floor) on 34,297 paths, the same k and t ranges, and no cap.
- Against the expectation registered from stage 6b: the crossings for k ≥ 0.071 between 202411 and 202411W4 and for k ≥ 0.127 between 202411W4 and 202412 are where the floors are (from 0.0713 and 0.1264). No path reached the density violations of 202411 beyond k = 0.120 before T₁, nor the crossings for k ≤ −0.733 in [T₁, T₂). The cap was not expected; it falls where 202412's left wing has g near zero far outside the settlements' strikes.

Diagnostic, European call K = 22,800 at each slice's expiry (n = 4; 5, 12 and 33 dates), against Black-76 at w_j(k): 202411 190.312 ± 0.241 against 188.943 (+1.369, +5.7 SE), no floor; 202411W4 346.573 ± 0.489 against 345.463 (+1.110, +2.3 SE), 181,163 floored path-steps; 202412 595.756 ± 0.820 against 595.674 (+0.083, +0.1 SE). The first slice's gap is beyond its SE with no floor or cap on its paths, so it is the scheme's bias over 20 steps of a 5-day option; this run cannot split it further. The bias table below is for the 33-day contracts only.

Euler bias (coupled draws, N = 2^20, 64 batches; V(n) and Δ(n) = V(n) − V(2n) ± SE, ratio Δ(n) / Δ(2n)):

| | Δ(1) | Δ(2) | Δ(4) | Δ(8) | bias left at n = 4, 2 Δ(4) |
|---|---|---|---|---|---|
| Asian | +0.545 ± 0.031 (1.67) | +0.325 ± 0.022 (1.83) | +0.178 ± 0.015 (1.71) | +0.104 ± 0.012 | +0.355 ± 0.030 |
| discrete, one-step | +0.264 ± 0.073 | +0.107 ± 0.061 | +0.066 ± 0.057 | −0.047 ± 0.042 | +0.131 ± 0.114, not resolved |
| discrete, hard | +0.097 ± 0.098 | +0.133 ± 0.075 | −0.059 ± 0.056 | +0.012 ± 0.048 | −0.118 ± 0.112, not resolved |
| discrete, smoothed 0.005 | +0.108 ± 0.068 | +0.082 ± 0.052 | +0.031 ± 0.036 | +0.011 ± 0.028 | +0.061 ± 0.072, not resolved |
| continuous, bridge | +0.823 ± 0.084 (1.78) | +0.463 ± 0.070 (2.83) | +0.163 ± 0.054 (2.29) | +0.071 ± 0.045 | +0.327 ± 0.108 |
| continuous, sampled crossing | +0.793 ± 0.109 (1.45) | +0.546 ± 0.115 (1.60) | +0.342 ± 0.085 (6.79) | +0.050 ± 0.061 | +0.683 ± 0.169 |

V(16) is 336.517, 570.484, 570.510, 569.663, 552.581 and 552.607. The Asian's ratios, 1.7 to 1.8, are near the 2 of first-order weak convergence, so 2 Δ(4) is a fair estimate there: +0.36 ± 0.03, about 0.8 of the price's SE at N = 2^20. The bridge's ratios are noisier but consistent with first order: +0.33 ± 0.11. The discrete barriers are not resolved at this N at any level past n = 1. Ratios whose Δ(2n) is within 2 SE of zero say nothing and are not quoted.

Timing, iteration only and not a result (`measure.sh`, macOS, single thread, N = 2^16, 5 repetitions, load1 3.86 at the start): price alone median 344.9, 566.1 and 429.6 ms for the Asian, one-step and bridge; with the 15 sensitivities 4,417.6, 4,630.4 and 5,084.9 ms, 12.8, 8.2 and 11.8 times the price alone.

Real-data test: its four checks pass on macOS with the captures present (0 skipped). Report tool test: six checks pass on the synthetic chain.

Fuzz (`aadp_fuzz_local_vol`, ASan and UBSan, macOS): a 60 s trial and the stated 300 s run through heavy.sh, seed 20261120, made 367,147 executions with no failure, from 16 hand-written seeds (`evidence/stage7a/gen_fuzz_local_vol_seeds.py`).

Faults: 29 added to `faults/manifest.json`, VW4's displayed sign among them (`evidence/stage7a/add_faults.py`). The first run, at 5347509, caught 20: seven did not compile and two passed, which the decisions above answer. The second, on the tree of the next commit with the LLVM toolchain, caught all 29: 26 on the source tree and the three real-data faults on a copy of the captures under `build/` (`evidence/stage7a/faults-run1-5347509.txt`, `faults-run2.txt`, `faults-run2-realdata.txt`).

### Stage 7b: market-quote sensitivities through the fit

The inputs are the stage 6b calibration settlements of one day, 2024-11-15, which are not tradable quotes, and the surface reaches 33 days only.

#### Pre-registration, stated before any sensitivity on the real surface

This was committed before the code that implements it, and before any sensitivity, multiplier, Hessian or bump on the real chain. Drafts of the code existed, and were run on synthetic slices and synthetic chains only: slices A, B and C of stage 6b, and slices shaped like the 6b fit (ρ near +1, the slope bound active), made from hand-written parameters with noise from development seeds 20261121 and 20261124. Those runs shaped three choices, recorded here as such: the halving rule (below) was changed twice, because a rule judging a level by its neighbours alone picked levels on the continuous-bridge price where the differences settle only at small h; the multiplier is taken from a Newton step, because the gradient-only estimate carried a first-order error; and Gauss-Newton keeps the constraint's curvature and may then refuse (below). The synthetic tests listed below ran with their stated tolerances while the code was written; no tolerance was changed after a failure, but the reduced-problem and small bump tests first required Gauss-Newton to solve, and now accept its refusal as a disagreement, after it refused as not a minimum on the constrained slice. The only real-chain numbers used to write this are those already printed in "Stage 6b results" and "Stage 7a results": every slice fits with b (1 + |ρ|) = 4 active, ρ near +1, and nothing else active. The drafting session was stopped by a usage limit before this was committed. Before committing it, its build trees, the evidence directory and its command history were searched: the report tool had run only on synthetic chains (`--options` and `--futures` under the build tree, written by a synthetic generator), the real-data test had been built but never run, and the gate that ran then had the drafts moved out of the tree, so no sensitivity, multiplier, Hessian or bump on the real chain existed when this was committed.

Quantity. dP_c / dV_i for the three 7a contracts c (the arithmetic Asian call, the discrete down-and-out call with one-step survival and the continuous one with the bridge, each at n = 4 on the raw SVI surface of 7a) and every calibration settlement V_i, i over the 180 settlements (65, 51 and 64 for 202411, 202411W4 and 202412), in price points per point of settlement.

The chain V → (F, D, σ_leg) → θ* → P.
1. Parity. F_j, D_j and σ_leg,j come from the regression of y = C − P on K over the `settle_all` points of slice j (stage 6b). A calibration settlement whose strike has a parity point is one leg of it: y moves by +V_i for a call and −V_i for a put; a strike without a point has no leg. Ordinary least squares is linear in y, so with slope β and intercept α, ∂β/∂y_j = (K_j − K̄) / S_KK, ∂α/∂y_j = 1/n − K̄ ∂β/∂y_j, ∂D/∂y_j = −∂β/∂y_j, ∂F/∂y_j = (∂α/∂y_j − F ∂D/∂y_j) / D, and ∂σ_leg/∂y_j = e_j / (n RMS √2), since the residuals are e = (I − hat) y. Both readings are reported. **Market fixed**: F, D and σ_leg held at their regression values, the fit's own Jacobian, which is what SVI-10's implicit-function step means and what a separate forward curve would give. **Through parity**: the total derivative, adding Σ_p ∂θ*/∂p · ∂p/∂V_i for p in (F, D, σ_leg), and on the price side ∂P/∂F_j and ∂P/∂D_j. Through parity is the full chain and the headline; market fixed is its decomposition.
2. The weights. σ_i = √(σ_leg² + τ_i²), τ_i² = tick(V_i)² / 12. The tick is piecewise constant in V_i, so its derivative is zero almost everywhere; a settlement exactly on a tick boundary (10, 50, 500 or 1000) has one-sided derivatives only, and the report counts any. The brief's statement that σ_i moves only through the tick is incomplete: through parity, every σ_i also moves with σ_leg, and that is included. Membership of the calibration set is piecewise constant in the settlements too, so it is held, with derivative zero almost everywhere. The bump holds every τ_i and the set at their unbumped values, which is the same derivative.
3. The fit. θ* = (a, b, ρ, m, σ) minimises Φ = Σ r_i², r_i = (B_i(θ; F, D) − V_i) / σ_i, B_i Black-76 at the raw SVI total variance, under the 6b bounds. Its derivative comes from the KKT conditions (below).
4. The price. ∂P/∂θ is 7a's tape gradient in the 15 slice parameters, on 7a's paths. 7a holds F, D and S₀ = D₁ F₁ passive, so through parity ∂P/∂F_j and ∂P/∂D_j come from central differences on common random numbers, with h_ℓ = 2^-ℓ 2^-12 F_j (or D_j), ℓ = 0..5, the level chosen by the halving rule below with refit error 2^-44 |P|, and per batch at that level, so that every sensitivity to a settlement has a batch-means SE. S₀, the carry and the rates move with F₁ and D₁ as 7a defines them.

KKT treatment. The active constraints at the fit are written c_k(θ) ≤ 0: the slope b (1 + |ρ|) − 4 (in the fit's inner variables c + |d| = 4σ), and ρ − 1 and −1 − ρ. At the optimum ∇Φ + Σ μ_k ∇c_k = 0 and c_k = 0, and differentiating in an input p gives
  [H_L ∇c; ∇cᵀ 0] [dθ/dp; dμ/dp] = [−∂²Φ/∂θ∂p; 0],  H_L = H + Σ μ_k ∇²c_k,
so the derivative lies on the constraint manifold (∇c · dθ = 0). The slope's gradient is (0, 1 + |ρ|, b sgn ρ, 0, 0) and its Hessian has ∂²c/∂b∂ρ = sgn ρ, which H_L carries; the ρ bounds are linear. Any other active bound (the boxes on m and σ, a at its maximum, b = 0, the minimum variance) is refused as an error value, as is the slope at ρ = 0, where |ρ| has no derivative; none is active in the 6b fit. Each parameter is scaled by 1 / √(GN_jj), and the scaled KKT matrix is factored by LU with partial pivoting. Error values, never a throw: the reduced Hessian Zᵀ H_L Z on a null-space basis of the active gradients not positive definite (not a minimum), a zero pivot (singular), or a 1-norm condition number above 2^40 (ill-conditioned: beyond it about 2^-12 of relative accuracy is left).

Multipliers and strict complementarity. The fitted point is stationary only to the fit's tolerance, so μ is taken from one Newton step on the KKT system from it, [H_L ∇c; ∇cᵀ 0] [δ; μ] = [−∇Φ; 0], which also estimates the distance δ to the exact KKT point. A second step from θ + δ gives μ₂, and margin = μ / |μ − μ₂| is how many times μ exceeds its numerical uncertainty. Strict complementarity is required as μ > 0 with margin ≥ 10; otherwise the slice is refused (degenerate multiplier), because a multiplier indistinguishable from zero means a small move of the settlements can release the bound and the derivative is one-sided. Reported per constraint: μ (Newton), the gradient-only least-squares μ, μ₂, the margin, significance = μ / √(Σ (∂μ/∂V_i σ_i)²), μ against its standard error under the fit's own noise model, and quote distance = min_i μ / (|∂μ/∂V_i| σ_i), the fewest noise units one settlement alone must move to release the bound, to first order. A small significance (below 2) is not refused: the derivative holds locally, the report says how far, and beyond that distance the slice's sensitivities are one-sided.

Exact Hessian against Gauss-Newton. H, ∂²Φ/∂θ∂V_i and ∂²Φ/∂θ∂(F, D, σ_leg) come from one record of Φ on the second-order tape over `Dual<double, 8>`, with tangents seeded on (θ, F, D, σ_leg) and the settlements registered as inputs with zero tangent, and one reverse sweep: forward over reverse. H includes the residual term 2 Σ r_i ∇²r_i. Gauss-Newton replaces every second derivative of Φ by 2 JᵀJ, J the Jacobian of r read from the same record's tangents; the settlement cross term is the same for both, since r_i is linear in V_i. It keeps μ ∇²c, which is exact, so it can be indefinite on the manifold and refuse as not a minimum; that is reported as its result. Reported per slice: both Hessians, max |H − GN| / √(GN_rr GN_cc), the condition numbers, the relative difference of dθ*/dV per parameter, and every product's sensitivities and bucket sums under Gauss-Newton beside the exact ones.

The bump-and-recalibrate check, for every settlement, both chains:
- Levels h_ℓ = 2^-ℓ min(σ_i, V_i / 2), ℓ = 0..5: V_i ± h_ℓ, and through parity also the leg, the regression refitted and every σ_i rebuilt with τ_i held. Each bumped slice is refitted by the production fit with its default options, from the same deterministic start grid; no tighter tolerance is used, and the refit's error is measured instead. A level is usable when both refits succeed, converge and keep the unbumped active set.
- Compared: dθ*/dV_i (5 per settlement and chain), through parity also d(F, D, σ_leg)/dV_i, and dP_c/dV_i for the three contracts, as the central difference of P on the refitted surface (with the refitted F and D through parity), on common random numbers at N = 2^16, 64 batches, seed 20261115, 8 threads, against Σ ∂P/∂θ dθ*/dV_i (+ ∂P/∂F dF/dV_i + ∂P/∂D dD/dV_i through parity), with ∂P/∂θ from the tape on the same paths.
- Refit error per parameter: ε_p = max |θ_p(alternative) − θ̂_p| over two alternative start grids, 8 points in m and 8 in σ, so each refit is off by up to ε_p and the difference by ε_p / h. For a price, Σ |∂P/∂θ_p| ε_p + 2^-44 |P|; for the parity outputs, 2^-44 |value|.
- Halving rule. At each usable level ℓ with a finer usable level, truncation(ℓ) = max |D_ℓ − D_m| over every finer usable m (at least 3/4 of an h² error, 1/2 of an h error, and it sees a sequence that settles late); the level with the smallest truncation + refit / h_ℓ is judged, and passes if |D_ℓ − reference| ≤ 4 (truncation + refit / h_ℓ) + floor. With no such level the comparison is unresolved, which is a failure.
- Floors: for θ, κ 2^-52 times the slice's largest |dθ_p/dV| over its settlements and both chains, κ the scaled KKT condition number; for the parity outputs, 2^-40 times the slice's largest; for a price, the θ floors weighted by |∂P/∂θ_p|, plus, through parity, the ∂P/∂F_j and ∂P/∂D_j selections' own 4 (truncation + refit) times |dF/dV_i| and |dD/dV_i|.
- Pass: every exact-Hessian comparison passes, none unresolved: 1,800 on θ, 540 on the parity outputs and 1,080 on prices. Gauss-Newton is judged by the same rule as a disclosure, not a requirement. The tolerances are not loosened after a result; a failure is reported as one.

Synthetic tests (`tests/svi/calibration_adjoint_test.cpp`), with these tolerances:
- Exact fit, Henrard's known answer: 5 quotes of slice B, 5 parameters, so dθ*/dV = G^-1 with G_ij = ∂B_i/∂θ_j in closed form (vega in w times ∂w/∂θ, with `std::erfc`), and dθ*/d(F, D) = −G^-1 ∂B/∂(F, D). The IFT, exact and Gauss-Newton, must equal it within 2^-42 κ of the largest entry in scaled units.
- The tape's Φ, gradient, H, GN and every cross term against nested forward-mode `Dual`s of Φ written out in the test, at fits to noisy A, B and C: within 2^-36 √(GN_jj GN_kk).
- The slope constraint against the reduced problem, b = L / (1 + |ρ|) eliminated, at a point polished by Newton on the reduced problem (noisy C fitted with its bound at 0.8 of its true slope): every derivative within 2^-42 κ, μ = −∂Φ/∂b / (1 + |ρ|) within 2^-42 κ μ, (1 + |ρ|) db + b sgn ρ dρ = 0 within 2^-40 of its terms, and Gauss-Newton must refuse or differ by more than 1,000 times that tolerance.
- The refusals, each as its own error value; the parity derivative against central differences at h = 0.01 and 0.005 within 4 times their difference plus 2^-44 |value| / 0.005; the legs' signs; the halving rule on hand-made sequences; and the bump check in small, every 7th quote of noisy A (unconstrained) and noisy C (slope bound at 0.8), where every exact comparison must pass and Gauss-Newton must fail at least one.

Real-data test (skips without the captures): every slice's adjoint is computed, with one positive multiplier per active bound and margin ≥ 10, κ ≤ 2^40, the manifold residual ≤ 2^-40, every derivative finite, and the middle settlement of each slice passes the check on θ with the market held.

Report (`aadp_quote_sensitivity_report`), iteration only, in `evidence/stage7b/`, not under `results/`: N = 2^20, 64 batches, seed 20261115 (7a's report seed, so 7a's paths), 8 threads, n = 4. Per contract the price and its 15 sensitivities, ∂P/∂(F_j, D_j), and the 180 dP/dV_i ± SE in full in three versions (market fixed exact, market fixed Gauss-Newton, through parity exact), summarised per expiry and k = ln(K / F_j) bucket with edges −0.10, −0.05, −0.02, 0.02 and 0.05 (n, sum ± SE, max |.|), and the sum over all 180, which is the response to every settlement up one point. Per slice the multipliers, both Hessians and the per-settlement table. The check at N = 2^16 as above.

Cost, through `measure.sh`, single thread, N = 2^16, 5 repetitions, medians: the price with its 15 sensitivities, then the implicit-function step for all three slices and the composition for 180 settlements on top, then through parity the 12 CRN prices for ∂P/∂(F, D). Iteration only, not stamped. SVI-10's 1.18 to 1.83 is the rate for an exact fit, context and not like for like.

Fuzz: `aadp_fuzz_calibration_adjoint`, the KKT entry point on synthetic slices from raw bytes (and through a small fit), 300 s through `heavy.sh` under ASan and UBSan, seed 20261125. Invariants: errors are values and say what the input shows; an adjoint is finite, has one positive multiplier per bound with margin ≥ 10 and κ in [1, 2^40], and each solution solves its KKT system, rebuilt in the fuzzer from the constraints' formulas, within 2^-36 of its terms, so on the manifold.

Every check above gets a fault in `faults/manifest.json`. Out of scope: eSSVI (7c) and stage 8's references.

#### Stage 7b results

Settlements of one day, not tradable quotes; the surface reaches 33 days only. Data: TAIFEX via data.gov.tw, Open Government Data License (NOTICE). Every number below is iteration only: none is stamped, and the outputs are in the research cache (`evidence/stage7b/`), not under `results/`. Commits: bcdbc28 (the pre-registration, alone), 352cf29 (the adjoint, the parity derivative, the halving rule, their tests and the fuzzer), e1da8dc (the report tool, its tests, the real-data test and the faults).

Before the pre-registration was committed, the build trees, the evidence directory and the drafting session's command history were searched for any sensitivity on the real chain: there was none (the pre-registration says what was found).

Decided after the pre-registration:
- The check's refits are polished. On the Linux host's GCC build (libstdc++ and its libm), the tool test's short synthetic check failed one of 55 parameter comparisons through parity: ρ of the first 202412 settlement, error 9.65e-8 against a tolerance of 4.29e-8. The production refits' differences settle at the two finest levels (2.76099396e-3, 2.76099386e-3), 9.6e-8 from the IFT's 2.76089750e-3, while the coarser levels fall off as h², towards 2.7608883e-3. A diagnostic (`evidence/stage7b/probe2.cpp`) shows the cause on the Mac, where the same settlement errs by 7.9e-8 and passes only because its truncation estimate is larger: the fit stops on its simplex tolerance, and at small h the two refits' stopping errors do not cancel. Moved each by one Newton step on its own KKT system, the refits converge to the IFT (2.76110631e-3, 2.76094970e-3, 2.76091053e-3 at the last three levels, against 2.76089747e-3 at the polished base point). The registration's refit error, the spread over two alternative start grids, is about 2e-10 for ρ there, so it does not see this stopping error. The tool's check now polishes by default (`--refit polished`), and `--refit production` gives the registered check. The tolerances, the floors, the halving rule and the refit error are as registered. This was decided after the registered check had run on the real chain and failed (below); it does not change that result, and both checks are given below.
- `qs-check-reference-gn` did not compile under `-Werror` in its first run (an unused parameter left by the mutation) and now keeps the parameter used. Two faults were added after the interruption, for checks that had none: the unit bump-and-refit test (`qs-bump-exact-is-gauss-newton`) and the real-data middle-settlement check (`qs-realdata-bump-cross-sign`).

The fits (n = 65, 51, 64; every slice converged, slope bound b (1 + |ρ|) = 4 the only active bound):

| slice | Φ | μ (Newton) | μ (least squares) | margin | μ / SE | quote distance | κ exact | κ GN | max \|H − GN\| / √(GN GN) | GN against exact, d θ / d V |
|---|---|---|---|---|---|---|---|---|---|---|
| 202411 | 12.42 | 3.8416e-3 | 3.8368e-3 | 1.9e9 | 0.99 | 2.94 | 1.6e6 | 1.4e6 | 1.6e-3 | 13% to 16% |
| 202411W4 | 106.89 | 6.6420e-4 | 6.7866e-4 | 6.2e7 | 0.18 | 0.63 | 4.1e5 | 4.0e5 | 6.7e-4 | 1.2% |
| 202412 | 191.03 | 9.9664e-2 | 1.0066e-1 | 1.1e9 | 4.88 | 9.63 | 1.2e8 | 4.9e7 | 2.7e-3 | 60% to 62% |

- Multipliers and complementarity. Every multiplier is positive and numerically clear of zero (μ against the second Newton step's μ₂: margins 6e7 to 2e9, against the required 10), so strict complementarity holds at the fitted points and the derivative is two-sided there. Statistically it is weak on two slices: under the fit's own noise model μ is 0.99 and 0.18 standard errors from zero on 202411 and 202411W4, and a single settlement moving 2.9 and 0.63 noise units, to first order, would release the bound. Beyond that distance the slope bound can go inactive, the derivative becomes one-sided, and these sensitivities hold only for moves that keep it active. Had any multiplier been indistinguishable from zero, the slice would have been refused with `degenerate-multiplier`; none was. The least-squares multiplier from the gradient alone differs from the Newton one by 0.1% to 2.2%, the first-order error the registration expected. The KKT solutions lie on the constraint manifold to 3e-19 to 2e-18 (scaled), and the Newton steps to the exact KKT point are 1e-9 to 7e-8 per parameter.
- Exact Hessian against Gauss-Newton. The residual curvature 2 Σ r_i ∇²r_i moves the scaled Hessian by at most 1.6e-3, 6.7e-4 and 2.7e-3, but the problem is ill-conditioned along the constraint, so d θ* / d V under Gauss-Newton differs from the exact one by up to 13% to 16% of its largest entry on 202411, 1.2% on 202411W4 and 60% to 62% on 202412. Gauss-Newton did not refuse on any real slice. On the prices the effect is smaller, since the price gradients weight the parameters: the largest single settlement moves 8% (Asian), 12% (discrete) and 14% (bridge) of the largest sensitivity, and the sums over all 180 settlements move by +0.012, +0.008 and +0.006 points per point. On the bump check Gauss-Newton fails 882 to 890 of each 900 parameter comparisons, with worst error over tolerance from 1,392 to 7,544: that is SVI-10's disclosure, measured.
- d P / d(F_j, D_j) by CRN differences (N = 2^20): F derivatives from 0.024 to 0.53 per point; d P / d D for 202411W4 is exactly 0 at every level for all three contracts, since in 7a's curves D₂ enters neither the drift nor the payoff's discount (D₁ sets S₀ and D₃ the discount).

Per contract, at N = 2^20, 64 batches, seed 20261115, 8 threads, n = 4 (`evidence/stage7b/report-n2p20.txt`, all 180 rows in three versions with SEs, and the bucket tables):

| contract | price ± SE | Σ_i dP/dV_i, market fixed | under Gauss-Newton | through parity | by expiry (market fixed): 202411, W4, 202412 | largest dP/dV_i |
|---|---|---|---|---|---|---|
| Asian call | 336.79 ± 0.44 | +0.694 ± 0.019 | +0.706 ± 0.020 | +0.628 ± 0.019 | +0.175, +0.357, +0.162 | +0.0346, W4 K = 22,800 C |
| discrete down-and-out | 570.36 ± 0.83 | +0.293 ± 0.044 | +0.302 ± 0.044 | +0.242 ± 0.045 | −0.100, −0.148, +0.542 | −0.0826, 202411 K = 24,100 C |
| bridge down-and-out | 552.84 ± 0.82 | +0.032 ± 0.044 | +0.038 ± 0.044 | −0.033 ± 0.045 | −0.224, −0.225, +0.482 | −0.0755, 202411 K = 24,100 C |

The Asian's response sits on the at-the-money settlements (the ATM bucket of 202411W4 carries +0.35 of the +0.69). The barriers load positively on 202412's settlements from k = −0.10 to 0.05 and negatively on its outer wings and on the upper wings (k ≥ 0.02) of the two short slices. Through parity changes single sensitivities by up to 7% (barriers) and 15% (Asian) of the largest, and flips the sign of the Asian's response to the far out-of-the-money puts of 202411 (+0.0024 to −0.0016 at K = 19,500), whose settlement also moves the forward. One settlement of 202411W4 and one of 202412 sit on a tick boundary, where the derivative is one-sided.

The bump-and-recalibrate check, every settlement, both chains, N = 2^16, 64 batches, seed 20261115, 8 threads; 19 of 2,160 bumped levels unusable in each (a refit failed, did not converge or changed its active set):

| comparisons | production refits (registered) | polished refits |
|---|---|---|
| parameters, market fixed, 900 | 900 pass, worst 0.46 | 900 pass, worst 0.46 |
| parameters, through parity, 900 | 900 pass, worst 0.65 | 900 pass, worst 0.83 |
| F, D, σ_leg through parity, 540 | 540 pass, worst 0.34 | 540 pass, worst 0.34 |
| prices, market fixed, 540 | 531 pass, worst 7.07 | 531 pass, worst 6.89 |
| prices, through parity, 540 | 529 pass, worst 20.0 | 529 pass, worst 19.8 |
| verdict | FAIL, 20 of 1,080 prices | FAIL, the same 20 |

(worst = error over tolerance; `evidence/stage7b/check-n2p16-production-refits.txt` and `-polished-refits.txt`.) Every parameter and parity comparison passes, unresolved none. The check fails as registered: 20 price comparisons, all on the two barrier contracts (11 discrete, 9 bridge), on settlements 46 to 50 of 202411W4 in both chains and settlement 52 of 202411 through parity, the upper wing in the region where 7a located the calendar floors (k ≥ 0.071 between 202411 and 202411W4). There the six levels of the price difference do not settle: for settlement 48 of 202411W4 on the discrete barrier they run −0.0385, −0.0237, −0.0176, −0.0143, −0.0193, −0.0172 against the composed −0.0021. The halving rule took the sequence as settled where it was not. A diagnostic (`evidence/stage7b/probe.cpp`, `probe-w4.txt`) differences the barrier prices in each of 202411W4's five parameters alone on the same paths, from 10^-3 of the parameter down to 10^-6 of it: at the coarse steps the differences are off by up to 30% for ρ and change sign for m on the bridge; at the finest step a, b and σ agree with the tape's gradient to six digits, and ρ and m, still converging, to 0.6% to 4%. So the differences are consistent with the 7a gradient and the composition, and what fails is the check's resolution: on 2^16 common paths, the barrier prices have kinks (floored path-steps) within the parameter moves that the settlement bumps make, and six halvings from min(σ_i, V_i / 2) do not reach the scale where the difference settles. The tolerances stay as registered, so this stands as a failure of the check on 20 of 1,080 price comparisons.

Exact fit (5 quotes of slice B, closed-form G^-1): the IFT, exact and Gauss-Newton, matches within 2^-42 κ of the largest entry, as do the reduced problem, the forward-mode Hessians (2^-36) and the parity derivative; the halving rule passes its hand-made sequences; the small bump check passes every exact comparison and Gauss-Newton fails at least one (`aadp_svi_calibration_adjoint_test`, macOS and the Linux host's GCC build). Real-data test: both checks pass on both, with the captures present (0 skipped).

Cost, through `measure.sh` (macOS, load 3.9 at start and 5.9 at the end), single thread, N = 2^16, 5 repetitions, medians, iteration only, at e1da8dc (`evidence/stage7b/timing-measure.txt`): the price with its 15 sensitivities takes 4,450, 4,659 and 5,016 ms (Asian, discrete, bridge); the implicit-function step for all three slices and the composition for 180 settlements add 0.46 to 0.47 ms, a ratio of 1.0001; through parity, d P / d(F, D) by 12 CRN prices adds 4,161, 6,815 and 5,123 ms, ratios 1.94, 2.46 and 2.02. The sensitivities to the settlements themselves are nearly free beside the price gradient; the cost through parity is the CRN differences, which a tape that also carried F_j and D_j would remove. SVI-10's 1.18 to 1.83 is for exact-fit rates calibrations, so it is context, not a like-for-like comparison.

Faults: 23 in `faults/manifest.json` (21 drafted before the interruption, 2 added after), all caught at e1da8dc by `python3 -I scripts/fault_injection.py --only <the 23 ids>` with the toolchain sourced, through `heavy.sh` (`evidence/stage7b/faults-e1da8dc.txt`). The first run, on the staged tree before its commit, caught 20: one did not compile (above), and the two real-data faults had no data beside the export; the second run caught those three.

Fuzz (`aadp_fuzz_calibration_adjoint`, ASan and UBSan, macOS, at e1da8dc): the drafting session's first two runs stopped on the fuzzer's own residual invariant, too strict row by row when b and ρ are both fixed by two active bounds, and absolute for a subnormal σ_leg; both inputs are synthetic and are kept as the regression seeds `regression_rho_and_slope_active` and `regression_subnormal_sigma_leg`. The final run, 300 s through `heavy.sh` at seed 20261125, executed 152,483 inputs with no failure (`evidence/stage7b/fuzz-300s-mac-e1da8dc.txt`).

### Stage 7c: an arbitrage-free eSSVI surface beside the raw SVI fit

The inputs are the stage 6b calibration settlements of one day, 2024-11-15, which are not tradable quotes, and the surface reaches 33 days only. This is the comparison `SVI-9` asks for: "If an arbitrage-free constrained fit is added, its cost in fit error is reported." The raw SVI surface stays the pricing surface of stages 7a, 7b and 8 (the 45 `svi-surface` rows are in raw parameters); eSSVI is a second, labelled line, and sensitivities to the settlements through it are out of scope.

#### Pre-registration, stated before any eSSVI fit on the real chain

This was committed before the code that implements it, and before any eSSVI fit, check or price on the real chain. Drafts of `src/svi/essvi.*` and `src/svi/essvi_fit.*` existed, outside the build, and were run on synthetic inputs only: the hand-made surface below with exact Black-76 prices and with noise from development seed 20261122, and 3,000 random points of the box below from seed 20261124 through the map, the constraint checker and stage 6a's checks. Those runs shaped two things, recorded as such: the stop rules (with exact prices the fit ends at Φ near 1e-26, where the scaled gradient is rounding and says nothing, so "no λ lowers Φ" is a converged stop), and the domain clause of the construction test below (3 of the 3,000 points converted to raw slices outside stage 6a's evaluated domain, all from large θ; none had a violation). No real-chain number was used beyond those already printed in "Stage 6b results" and "Stage 7a results". Hendriks and Martini's paper could not be opened (SSRN answered 403, as in Phase 1), so its Proposition 3.5 is read through Mingone (arXiv 2204.00312, section 2.1) and Pasquazzi (arXiv 2304.02106, section 1).

Parametrisation. Each slice j has three numbers (θ_j, ρ_j, ψ_j), ψ = θ φ, and
  w_j(k) = (θ_j + ρ_j ψ_j k + √((ψ_j k + θ_j ρ_j)² + θ_j² (1 − ρ_j²))) / 2,  k = ln(K / F_j),
the SSVI slice of Gatheral and Jacquier (arXiv 1204.0646, eq. (4.1)) with its own ρ, as Corbetta et al. (arXiv 1804.04924, eq. (1) and section 2.1, "We also substitute a new parameter ψ to the product θϕ") and Mingone (section 2, the eSSVI formula in (θ, ρ, ψ)) write it. θ_j = w_j(0) is the at-the-money total variance. F_j, D_j and T_j are stage 6b's parity forwards, discounts and ACT/365 times.

Constraints, for slices 1, 2, 3 = 202411, 202411W4, 202412:
- Butterfly, each slice: B1 ψ (1 + |ρ|) < 4 and B2 ψ² (1 + |ρ|) ≤ 4θ. These are Gatheral and Jacquier's Theorem 4.2 (sufficient) in ψ, Corbetta's eqs. (2) and (3), and Mingone's f_GJ (section 2.2.1). B1 is the Lee and d+ condition b (1 + |ρ|) < 2 of the converted slice. Mingone's necessary and sufficient f_MM (section 2.2.2) is less strict but needs a minimisation inside every evaluation; it is not used.
- Calendar, each adjacent pair (1 the shorter): N θ₂ ≥ θ₁ and ψ₂ ≥ ψ₁ max((1 + ρ₁) / (1 + ρ₂), (1 − ρ₁) / (1 − ρ₂)), which is Corbetta's "θ_i and ψ_i non-decreasing" with |(ρ₂ψ₂ − ρ₁ψ₁) / (ψ₂ − ψ₁)| ≤ 1 (section 2.2, and conditions 1, 2 and 4 of section 5.1), written without the division; and S ψ₂ / ψ₁ ≤ θ₂ / θ₁, Mingone's sufficient condition (section 2.1, "ψ2 ≤ (ψ1/θ1) θ2") and the first branch of Pasquazzi's condition S. Corbetta calls N necessary and sufficient. Pasquazzi shows N alone is not ("condition N' and condition S are not jointly sufficient"; N and S jointly are, for θ₂ ≠ θ₁, Proposition 4.14), and the random search in "SVI" found crossings in 27,061 of 119,956 pairs under Corbetta's conditions alone and none in 110,199 with S's first branch added. So S is imposed. Its second, quadratic branch would allow more surfaces; it is not used, so the constrained set is smaller than the arbitrage-free one, and its cost in fit error is an upper bound in that sense.

The global parametrisation, so that every fitted surface satisfies the constraints by construction: Mingone's section 3.2, eqs. (4) to (6), with f = f_GJ. Coordinates x = (ρ₁, ρ₂, ρ₃, θ₁, a₂, a₃, c₁, c₂, c₃); p_i = max((1 + ρ_{i−1}) / (1 + ρ_i), (1 − ρ_{i−1}) / (1 − ρ_i)), θ_i = θ_{i−1} p_i + a_i; f_i = min(4 (1 − 10^-9) / (1 + |ρ_i|), √(4 θ_i / (1 + |ρ_i|))); A₁ = 0, A_i = ψ_{i−1} p_i; C_i = min(ψ_{i−1} θ_i / θ_{i−1} for i > 1, f_i, f_{i+1} / p_{i+1}, ..., f_N / (p_{i+1} ⋯ p_N)); ψ_i = A_i + c_i (C_i − A_i). Mingone's Proposition 3.1 proves the slices satisfy B1, B2, N and S for every point with ρ ∈ ]−1, 1[, θ₁, a > 0 and c ∈ ]0, 1[; conversely any surface satisfying them maps into that box (S and N together give θ_i ≥ θ_{i−1} p_i, and N with B2 of later slices gives ψ_i ≤ C_i), so Mingone's open box is exactly the constrained set; the fit's box below trims it only within 10^-6 of ρ = ±1, below 10^-3 of the range of c₁, within 10^-9 of B1, and at variances ten times beyond the settlements'. The factor 1 − 10^-9 on B1 keeps it strict in floating point, where stage 6a's d+ check is strict; for these slices it never binds, because θ ≤ 1 makes B2 the smaller term. Which of C_i's terms binds is reported per slice.

Calibration.
- Settlements, weights and objective exactly as stage 6b: the 65, 51 and 64 calibration settlements, σ_i² = σ_leg² + tick_i² / 12, r_i = (V_model,i − V_i) / σ_i with V_model the Black-76 price (Let's Be Rational) at w_j(k_i), and Φ = Σ_j Φ_j, Φ_j = Σ_i r_ji² over slice j. Stage 6b's Φ per slice, summed, because the calendar constraints couple the slices.
- Scheme: global, all nine coordinates at once, not Corbetta's slice-by-slice algorithm. Reasons: Corbetta's anchor θ = θ* − ρψk* is a first-order solution of w(k*) = θ* (Corbetta section 2.1, "At first order"; Mingone's introduction and Pasquazzi section 2.1 say the same), its first slice is fitted with no regard to the later ones, and its objective is the L1 price error; a sequential fit would not minimise Φ over the constrained set, and the cost in fit error must be measured at that minimum to be the cost of the constraints.
- The box: ρ_i ∈ [−(1 − 10^-6), 1 − 10^-6]; θ₁ ∈ [10^-3 ŵ_min,1, 10 ŵ_max,1]; a_i ∈ [0, 10 ŵ_max,i]; c₁ ∈ [10^-3, 1]; c₂, c₃ ∈ [0, 1]; ŵ_min,j and ŵ_max,j the smallest and largest market total variance of slice j (stage 6b's ŵ). The ends of the paper's open intervals are included, so a fit can sit on a constraint with equality, which is the closure of the arbitrage-free set and is reported as active. The ρ bound keeps the converted σ = θ √(1 − ρ²) / ψ at least 7e-7 for θ ≥ 10^-6 (B2 gives ψ ≤ 2√θ), inside stage 6a's and 7a's validation; c₁ ≥ 10^-3 keeps ψ₁ off 0, where m = −θρ/ψ and σ grow without bound; the θ₁ and a ranges are wide beyond any settlement's variance. A coordinate within 10^-9 of the width of an end is reported at that end.
- Optimiser: Levenberg-Marquardt on the box, with Marquardt's scaling (damping λ diag(JᵀJ)), λ starting at 10^-3, divided by 3 after a step that lowers Φ and multiplied by 4 after one that does not. The Jacobian is exact: ∂r_i/∂x = (D F φ(d₁) / (2 √w)) (∂w_i/∂x) / σ_i, with ∂w_i/∂x carried by the forward-mode `ad::Dual` through the map and the slice (the map's min and max choose their branch on values, so the Jacobian is that of the branch taken). A coordinate at an end of the box whose gradient points outward is held there for the step; every trial point is clamped into the box. No randomness inside a start.
- Starts: 16. Start 0 is Mingone's (section 4.2): ρ_i = 0, c_i = 1/2, θ₁ = θ*₁, a_i = max(θ*_i − θ*_{i−1}, 0), with θ*_j the market total variance at k = 0, linear in k between the two settlements around it. Starts 1 to 15 draw each coordinate from Philox `PathUniform(20261121, start, coordinate)`, a development seed (not 1729 or 271828): ρ_i uniform on [−0.95, 0.95], θ₁ = θ*₁ (1/2 + u), a_i = 2u max(θ*_i − θ*_{i−1}, θ*_i / 10), c_i uniform on [0.05, 0.95]. The result is the start with the lowest Φ (the first on a tie); the report gives every start's Φ, stop and iterations, and how many reached within 10^-8 of the best, relatively.
- Convergence. A start stops converged when (1) the scaled projected gradient, max over the free coordinates of |g_j| / √(A_jj Φ) with g = Jᵀr and A = JᵀJ (at most 1 by Cauchy-Schwarz, and scale-free), is at most 10^-9; (2) no λ up to 10^16 lowers Φ (a minimum to rounding along every damped direction; how an exact fit ends); or (3) ten accepted steps in a row each lower Φ by at most 10^-14 of itself. It stops unconverged after 2,000 iterations. The stop and the scaled gradient are reported. A kink of the map (a change of the binding term of C_i or of p_i's branch) can hold a minimum with a non-zero one-sided gradient; that shows as stop (2) or (3) with a gradient above 10^-9, and is reported, not hidden.
- Errors are values: no slices or more than 8, expiries not strictly increasing, a slice that FitRawSvi would refuse (the same reasons, with the slice's index), and no start with a finite Φ.

Conversion and checks, as for the raw fit. Each fitted slice is converted to raw SVI by Mingone's section 2.2.2: a = θ (1 − ρ²) / 2, b = ψ / 2, ρ, m = −θρ/ψ, σ = θ √(1 − ρ²) / ψ. The converted parameters go through stage 6a's `CheckButterfly` (default grid, slope bound 2: parameters, positive variance, g on the grid, the d+ limit, both Lee wings, Martini-Mingone) and `CheckCalendar` between each adjacent pair, exactly as stage 6b ran them on the raw fit, and Phase 1's triple count runs on the model prices at the settlements' strikes. Expected: no violation anywhere. A violation is reported with its kind, location and value, and nothing is repaired. Separately, `CheckEssviConstraints` evaluates B1, B2, N and S on the fitted (θ, ρ, ψ) and reports each slack.

What is reported (`aadp_essvi_report`), per slice beside raw SVI's from the same run (stage 6b's code, refitted):
- the parameters (θ, ρ, ψ) and their raw conversion, each coordinate's value and box, the ends reached, the binding term of C_i, each constraint's slack and whether it is active;
- SVI-6 (mean |V_model − V| / F in bp), SVI-11 (unweighted implied-vol RMSE in vol points), Φ_j and Φ_j / (n_j − 3), beside raw SVI's Φ_j / (n_j − 5); and the whole surface, Φ / (180 − 9) beside Σ Φ_raw / (180 − 15). The per-slice degrees of freedom are nominal for eSSVI, whose slices share constraints. The cost in fit error is the difference in each.
- the checks above on both surfaces, and the triple counts;
- the starts, their stops and the convergence.

Pricing, a second, labelled line beside 7a's. The converted slices go into 7a's surface unchanged: total variance linear in t at fixed k between slices and (t / T₁) w₁ before the first, Dupire with 7a's floor rule, the log-Euler law, and 7a's three contracts (the arithmetic Asian call, the discrete down-and-out call by one-step survival and the continuous one by the bridge, K = 22,800, H = 21,500, 33 dates, n = 4, T = T₃). The run is 7a's: N = 2^20, 64 batches, seed 20261115, 8 threads. Reported per contract: the raw SVI price, which must reproduce 7a's printed price and SE (336.794 ± 0.440, 570.356 ± 0.829, 552.838 ± 0.820), the eSSVI price ± SE, and the paired difference eSSVI − raw on the same uniforms, path by path, with its SE from the 64 batch differences; and the eSSVI floor report as 7a's (counts by class, paths with a floor, k and t ranges overall and per segment, and the cap). Expected: no floor and no cap. Before T₁ the scheme is Corbetta's short extrapolation exactly (θ_t = λθ₁, ψ_t = λψ₁, ρ₁, section 5.2), proved free of arbitrage. Between slices it is not Corbetta's (section 5.1 interpolates θ, ψ and ρψ linearly, not w): ∂T w ≥ 0 does hold wherever w_j ≥ w_{j−1}, which the calendar constraints give, but g ≥ 0 of the interpolated slice is not guaranteed by the scheme, so a butterfly or below-the-floor point is possible and would be counted, located and reported.

Timing (`SVI-8`), iteration only: `aadp_essvi_report --mode timing` through `measure.sh` on macOS, 50 repetitions, median and minimum wall time of the eSSVI surface fit alone and of the fit with every check (the constraint checker, `CheckButterfly` on each converted slice and `CheckCalendar` on each pair), the latter the same scope as stage 6b's 186.7 ms. Context only: Corbetta's 1.2 s is Python on 12 maturities.

Tolerances, each with its reason:
- Conversion: |w_raw(k) − w_eSSVI(k)| ≤ 64 ε S, ε = 2^-52, S = θ + ψ |k| + √((ψk + θρ)² + θ² (1 − ρ²)), on k from −3 to 3 in steps of 0.01. Both forms are about ten correctly rounded operations on terms no larger than S (the raw form's b r is exactly the eSSVI root over 2, and an error of ε|m| in m moves w by at most ψ ε |m| ≤ ε θ), so the difference is below about 20 ε S; 64 leaves a factor 3. Slices: the hand-made surface below, Corbetta's Table 1 rows at 0.03 and 2.95 years, and a slice at each end of the ρ box with ψ at B2.
- Constraint checker: a constraint is violated when its slack is below −64 ε (for the strict B1, below +64 ε), the slacks being dimensionless (1 − ψ (1 + |ρ|) / 4, 1 − ψ² (1 + |ρ|) / (4θ), θ₂ / θ₁ − 1, the smaller of ψ₂ (1 ± ρ₂) / (ψ₁ (1 ± ρ₁)) − 1, and 1 − ψ₂θ₁ / (ψ₁θ₂)). Each side is at most about eight correctly rounded operations from the parameters, so a computed slack is off by about 16 ε at most; 64 leaves a factor 4, and the violations the tests plant are 10^-6. Active: |slack| ≤ 10^-9, stage 6b's rule.
- Same objective as stage 6b: each fitted Φ_j must equal stage 6b's `Objective` at the converted raw parameters within 10^-9 relatively. The two compute w in different forms, which differ by at most 64 ε S (above), moving each r_i by a relative amount of order 10^-12 or less; 10^-9 leaves a factor of about 1000, and a wrong weight, forward or conversion is off by far more.
- On the tape: the map, the slice and the conversion recorded on `ad::Real` give the double values bit for bit, and the tape's gradient of w(k) in the nine coordinates agrees with the `ad::Dual`'s within 2^-40 of the largest of the nine |derivatives|. The two apply the same local partials; only the order of summation differs, over at most a few hundred statements, about 2^-45 of the scale.

Synthetic tests, on hand-made parameters only, with stage 6b's slice shapes (F = 22,800, strikes on a 100-point grid, out of the money, prices of at least 1 point, σ_i from σ_leg = 2 and the tick rule; T = 5, 12 and 33 days, D = 0.9999, 0.9995, 0.998, strikes 19,700 to 24,100, 19,500 to 24,500 and 18,000 to 25,000). The hand-made surface H: (θ, ρ, ψ) = (4.4e-4, −0.5, 0.02), (1.0e-3, −0.45, 0.03), (2.8e-3, −0.5, 0.05), inside every constraint with slack at least 0.34.
- Recovery, noiseless: prices are Black-76 at H. Pass if the fit converges, Φ̂ ≤ 10^-10 and every parameter within 2 (√(Φ̂ C_jj) + √(Φ_true C_jj)), C = (JᵀJ)⁻¹ in (θ, ρ, ψ) at the fit (`EssviParameterCovariance`), Φ_true the objective at H: stage 6b's rule, for its reason (the stopping rule's residual and the prices' rounding, read through the conditioning).
- Recovery, noisy: V_i + σ_i z_i, z_i = Φ⁻¹(`PathUniform(20261122, i, slice)`), the 1-point floor applied after the noise, one draw. Pass if each of the nine parameters is within 4 standard errors √C_jj of H; nine comparisons at 4 SE fail together with probability about 6e-4 in the linear Gaussian approximation. A failure is reported; the seed is not changed.
- On a constraint: the box point of H with ψ₂ = 0.04 (H_B), with c₁ then set to 1, so that ψ₁ is at its cap C₁ with B2 binding (H's own ψ₂ would then break N): the noiseless fit must put c₁ at its upper end and recover every parameter within the noiseless tolerance. Outside one: H with ψ₂ = 1.05 ψ₁ θ₂ / θ₁ and ψ₃ = 0.06 (N still holds on both pairs, S fails on the first): the fit must put c₂ at its upper end with S binding, report every constraint satisfied, and its Φ must equal `EssviObjective` at the returned point within 10^-12 relatively (the same computation; only the summation order of the slices differs).
- By construction: 300 points of the box from `PathUniform(20261124, point, coordinate)`, with ρ_i uniform on [−(1 − 10^-6), 1 − 10^-6], θ₁ = 10^(−4 + 3u), a_i uniform on [0, 2θ₁], and each c_i at its upper end with probability 1/4, at its lower end with probability 1/4 and uniform otherwise, so that the caps hold with equality on many slices. Every point must pass the constraint checker. Every point whose three converted slices pass stage 6a's `CheckParameters` must pass `CheckButterfly` and both `CheckCalendar` pairs with no violation, and at least 290 of the 300 must be inside that domain. These are the same first 300 points the drafts ran.
- Just outside a constraint, rejected and clamped: from H, with one perturbation of 10^-6 relative each: ψ₁ above its B2 cap (on H_B); ψ₂ above S's cap; ψ₂ below N's lower bound ψ₁ p₂; and θ₂ below θ₁ p₂ (a₂ < 0) with ψ₂ = ψ₁ p₂. The checker must flag the planted constraint (S in the last case, which N and a₂ < 0 together force) and no other. `ToGlobal` must clamp exactly one coordinate, the one standing for it (c₁ to 1; c₂ to 1; c₂ to 0; a₂ to 0), record the value it had and the end it was set to, and `FromGlobal` of the clamped point must pass the checker with that constraint active. A ρ beyond the box (1 − 10^-7) is clamped to it and recorded, though no constraint is violated. Just inside (the same perturbations reversed, and in the last case ψ₂ = ψ₁ p₂ (1 + 5·10^-7), inside both N and S): no violation, no clamp, and `FromGlobal(ToGlobal(s))` gives s back within 10^-12 relatively per parameter (the map back repeats the map's own operations on the same coordinates, so a dozen roundings separate the two).
- Errors as values: each of the error codes above on a hand-made input.

The real-data test asserts only what is stated here, with the captures present (it skips visibly without them, as the other real-data tests do): the fit returns for all three slices and converges; the constraint checker finds no violation; each converted slice passes `CheckParameters` and the converted surface passes 7a's `ValidateSurface`; each Φ_j equals stage 6b's `Objective` at the converted parameters within 10^-9; the checks run; and the three contracts validate on the eSSVI surface and price to finite values at 4,096 paths. The fitted values, violation counts and prices are reported by the tool, not asserted.

Fuzz: `aadp_fuzz_essvi_fit`, from raw bytes, every field free (NaN and infinities included): one to three slices with F, D, T and up to 12 quotes each (strike, price, noise, put mask), fitted with 2 random starts and 60 iterations through the same code; and a box point read from the bytes through `FromGlobal`. Properties: nothing throws or trips a sanitizer; an error has a name and matches the input (a bad slice, quote or expiry order where the input has one, at an index inside it); a fit is finite, every coordinate lies in its box, the checker finds no violation, Φ is the sum of the slices' Φ_j and of their squared residuals, each Φ_j equals stage 6b's `Objective` at the converted slice within 10^-9 relatively, and the starts are counted consistently; a box point maps to slices that the checker passes. 300 s through `heavy.sh` under ASan and UBSan, seed 20261123, from hand-written synthetic seeds; a finding is fixed and its input kept as a seed.

Faults: every check above gets one in `faults/manifest.json` that makes it fail for its named reason (conversion, each constraint in the map and in the checker, the checker's strictness, the clamps and their records, the recovery and boundary tests, the objective's weights, the paired difference and the floor report in the tool, the real-data assertions and the fuzzer's properties).

Out of scope: sensitivities to the settlements through eSSVI (the IFT of 7b on this fit), and stage 8's oracles, which stay on the raw SVI parameters as registered in Phase 2.

#### Stage 7c results

Settlements of one day, not tradable quotes; the surface reaches 33 days only. Data: TAIFEX via data.gov.tw, Open Government Data License (NOTICE). Every number below is iteration only: none is stamped, and the outputs are in the research cache (`evidence/stage7c/`, the reports at b8a67a0), not under `results/`. Commits: 31267b1 (the pre-registration, alone), ae72e4c (the map, the checker, the fit, the chain layer, the report tool, their tests, the real-data test and the fuzzer), 3f0e5a9 and b8a67a0 (the fixes for what the fuzzer found, below; 3f0e5a9's message also names the hypot root, which was already in ae72e4c), and the commit of the faults and this section.

Decided after the pre-registration:
- The fuzzer found three inputs that broke a registered property, all synthetic and kept as regression seeds (`regression_theta_underflow`, `regression_subnormal_theta`, `regression_subnormal_expiry`). Each was fixed, and the run repeated:
- `EssviTotalVariance` takes its root as hypot does, scaled by the larger of |ψk + θρ| and θ. The 60 s fuzz trial found a slice (D = 1.2e103, so a quote's total variance near 1e-207) where θ near 4e-210 made (θρ)² underflow to 0, so that w(0) came out as θ/2, and Φ_j and stage 6b's `Objective` at the converted slice differed by 1.6%. The real-chain fit moved by about 1e-10 relatively in its coordinates and not at all in its printed Φ, metrics or prices (`evidence/stage7c/pre-hypot/`).
- The 300 s run at ae72e4c found a fit that broke B2 by 8e-13 relatively: a quote's total variance of 1.5e-312, a subnormal, put θ₁'s box among the subnormals, where 64ε cannot hold. Total variances below 2^-1000 (about 9e-302) are now refused with the non-positive ones (`kNoImpliedVol`, `kNotAttained`), and the checker's S and the map's S cap take θ's ratio first, so that no product of two small numbers underflows. The real-chain coordinates moved by about 1e-13 relatively; Φ, the metrics and the prices did not move in their printed digits.
- The third, at 3f0e5a9: T = 1.1e-311, where the converted slice's w / T overflowed in stage 6b's `Objective` (NaN) though not in the fit's own evaluation, one ulp of w away. Quotes with a market total variance above 2^100 or an implied volatility above 2^500 are now refused the same way (b8a67a0). The real chain's report is unchanged.
- The agreement of Φ_j with stage 6b's `Objective` is within 10^-9 relatively plus a rounding scale, Σ_i d_i (2 |r_i| + d_i) with d_i = 1024 ε V_i / σ_i, in the fit test, the real-data test and the fuzzer. The relative tolerance alone failed on the exact-price recoveries, where Φ_j is near 1e-27 and the two paths to the price differ by rounding (Φ_j 2.6e-28 against 3.2e-28 on GCC). The scale adds about 1e-9 to the noisy test's tolerance of 1.3e-7, and nothing measurable on the real chain.
- Three test surfaces did not say what the registration says, and were corrected before any test passed: H's slack on S is 0.34 exactly, so the check allows rounding below it; on H, ψ₂ at its S cap puts ψ₃ = 0.05 exactly on N's lower bound, so the just-outside S case also raises ψ₃ to 0.06, as the registered fit test outside S does; and the ρ-beyond-the-box case is a single slice, since after H's slices N needs ψ₃ ≥ ψ₂ (1 − ρ₂) / (1 − ρ₃), beyond B1. A check of B1 on the map (θ = 10, c₁ = 1, slack equal to the margin) and the tool test's parsed checks (the paired difference against the difference of the two means, and every floor report over 135,168 path-steps) were added for faults that had no check to catch them. The fuzzer also checks that a fit's slices are in order of expiry.
- `kNoFeasibleStart` is not reached by any test; the fuzzer accepts it, named, without an input condition.

The surface fit (all 16 starts reached Φ = 449.0201209; the best stopped on no decrease at a scaled gradient of 1.27e-9, which the registration allows and the report shows; no coordinate at a box end):

| slice | n | θ | ρ | ψ | ψ capped by | Φ eSSVI, Φ/(n−3) | Φ raw, Φ/(n−5) | SVI-6 bp, eSSVI / raw | SVI-11 vol points, eSSVI / raw |
|---|---|---|---|---|---|---|---|---|---|
| 202411 | 65 | 5.358e-4 | −0.5399 | 0.01719 | B2 | 26.84, 0.433 | 12.42, 0.207 | 0.450 / 0.287 | 1.893 / 0.958 |
| 202411W4 | 51 | 1.510e-3 | −0.5808 | 0.01943 | S | 109.31, 2.277 | 106.89, 2.324 | 1.174 / 1.132 | 1.020 / 0.410 |
| 202412 | 64 | 4.361e-3 | −0.4423 | 0.04683 | S | 312.88, 5.129 | 191.03, 3.238 | 1.603 / 1.231 | 2.498 / 1.397 |
| surface | 180 | | | | | 449.02, Φ/171 = 2.626 | 310.35, Φ/165 = 1.881 | | |

The cost in fit error (`SVI-9`): Φ rises by 138.7 (a factor 1.45) over the surface, 14.4, 2.4 and 121.8 by slice; SVI-6 by 0.16, 0.04 and 0.37 bp and SVI-11 by 0.94, 0.61 and 1.10 vol points. Unlike every raw slice (ρ near +1, the right wing at slope 4), every eSSVI slice has ρ near −0.5 and wing slopes b (1 ± ρ) of at most 0.034. None of the constraints is active: the closest are N on ψ between 202411 and 202411W4 (slack 0.030) and S between 202411W4 and 202412 (0.166). The ψ intervals' upper ends are B2's (202411) and S's (the others), and each ψ lies inside its interval.

Checks on the converted slices (`CheckButterfly` and `CheckCalendar`, as 6b ran them): no violation of any kind. Minimum g on the grid 0.254, 0.262 and 0.262, Martini-Mingone no-arbitrage on all three; calendar minimum gaps 2.86e-4 (202411 → 202411W4) and 2.50e-3 (202411W4 → 202412). The model prices have 0 non-convex triples, as raw SVI's do, against 15, 19 and 17 among the settlements. The constraint checker finds no violation. The raw fit's failures are 6b's: four butterfly violations per slice (density, the d+ limit, the right wing, Martini-Mingone type 1) and calendar crossings on both pairs.

Prices off the surface (7a's run: N = 2^20, 64 batches, seed 20261115, 8 threads, n = 4; the raw line reproduces 7a's prices and SEs exactly; the difference is paired, path by path, its SE from the 64 batch differences):

| contract | raw SVI (7a) | eSSVI | eSSVI − raw | eSSVI floors and caps | raw floored path-steps |
|---|---|---|---|---|---|
| arithmetic Asian call | 336.794 ± 0.440 | 336.945 ± 0.436 | +0.151 ± 0.044 (+3.4 SE) | 0 and 0 of 138,412,032 | 532,933 (0.385%) |
| down-and-out, discrete, one-step | 570.356 ± 0.829 | 570.330 ± 0.849 | −0.026 ± 0.128 (−0.2 SE) | 0 and 0 | 534,901 (0.387%) |
| down-and-out, continuous, bridge | 552.838 ± 0.820 | 552.756 ± 0.838 | −0.082 ± 0.129 (−0.6 SE) | 0 and 0 | 532,933 (0.385%) |

On the eSSVI surface no path-step is floored or capped, in any segment, as expected. The Asian's difference is 3.4 paired SEs, so the two surfaces give different Asian prices on the same paths, by 0.045% of the price; the barriers' differences are within their paired SEs. Nothing here says which surface is right: both fit the same settlements, and the raw one prices with 0.39% of its path-steps on the floor.

Timing (`SVI-8`), iteration only, `measure.sh`, macOS, at b8a67a0, 50 repetitions: the eSSVI surface fit alone, median 4.69 ms (minimum 4.35); the fit with every check, 6.65 ms (6.48), against stage 6b's 186.7 ms for the three raw fits and their checks, a different optimiser (`evidence/stage7c/timing-measure-b8a67a0.txt`; at ae72e4c, 4.59 and 6.54 ms).

Synthetic recovery, macOS and the Linux host's GCC build: from exact prices, H is recovered to the bit (Φ = 0, so the registered tolerance 2 (√(Φ c_jj) + √(Φ_true c_jj)) is 0 and is met); with noise, every parameter within 1.95 SE (z from −1.95 to 1.57, Φ = 131.39 over 37, 50 and 70 quotes); H_B is recovered to the bit with c₁ at its upper end and B2 binding; data outside S ends with c₂ at its upper end, S binding and every constraint held (Φ = 1.62); 300 box points pass the checker, all 300 inside stage 6a's domain, with no butterfly or calendar violation; the just-outside, ρ, just-inside, B1-strict and tape-against-Dual tests pass. Real-data test: both checks pass with the captures present (0 skipped), on macOS and on the Linux host.

Faults: 33 in `faults/manifest.json` (`evidence/stage7c/add_faults.py`), covering the conversion, each constraint in the map and in the checker, B1's strictness, the clamps and their records, the fit's weights, Jacobian, box ends and errors, the recovery tests, the tool's paired difference, floor report and attribution, the four real-data assertions and four fuzzer properties. Run by `python3 -I scripts/fault_injection.py --only <the 33 ids>` with the toolchain sourced, through `heavy.sh`, at b8a67a0: 31 caught at once; `essvi-check-strict` and `essvi-fit-jacobian` did not compile under `-Werror` (an unused variable and an unused parameter left by the mutation), now keep them used, and were caught on the second run (`evidence/stage7c/faults-b8a67a0.txt`, `faults-b8a67a0-run2.txt`). The four real-data faults ran on a copy of the captures.

Fuzz (`aadp_fuzz_essvi_fit`, ASan and UBSan, macOS, seed 20261123, from 17 hand-written synthetic seeds): the 60 s trial and the 300 s runs at ae72e4c and 3f0e5a9 found the three inputs above (after 128,369, 810,574 and 790,309 executions). The final 300 s run, on b8a67a0's tree, executed 775,484 inputs with no failure (`evidence/stage7c/fuzz-300s-mac-b8a67a0.txt`), from the 17 hand-written seeds (`evidence/stage7c/gen_fuzz_essvi_fit_seeds.py`) and the three regression seeds.

### Stage 8: references for the 57 awaiting rows

The 45 `svi-surface` rows use the stage 6b fit to TAIFEX settlements of one day, 2024-11-15, which are not tradable quotes, and the surface reaches 33 days only. Data: TAIFEX via data.gov.tw, Open Government Data License (NOTICE).

#### Pre-registration, stated before any code or pilot

This was committed alone, at the commit after 8cbaaf3, before any stage 8 code had been written or drafted, before any pilot, and before any run on seed 1729 or 271828. The only numbers used to write it are those already printed in this file ("Stage 6b results", "Stage 7a results", "Stage 7b results") and in `references/`. Anything decided later is recorded in the results as such.

What does not change. The plan keeps K = 147, B = 64, α_F = 0.01 and the critical value 4.2662177494486286, and every `#!` directive. The 57 rows keep their `id`, `instrument` and `greek` fields byte for byte, and their order. The 90 run-1 rows and their N, seed, ceiling and b do not change in any byte. What stage 8a fills in on the 57 rows, through `aadp_run_oracles emit-plan` and nothing else: `reference` (now the placeholder `phase3`), N, seed, `se_ceiling`, b and `status`. `references/measured_results.csv` (run 1) and `references/pilot_results.csv` (the Phase 2 pilot) never change.

##### The estimator of run 2, for all 147 rows

Run 2 judges the production pricer and tape, as the oracle policy says. For every row the estimate is the batch mean of the production code: `ad::PriceWithGradient` (`src/pricer/` and `src/ad/`) for the 102 GBM rows, with the value and the five tape Greeks, theta = −dV/dT as `src/ad/gradient.cpp` gives it; and `ad::PriceLocalVolWithGradient` for the 45 surface rows, component 5 j + i for slice j and parameter i. A finite-difference reference is a central difference of the production price, `pricer::Price` or `pricer::PriceLocalVol`, on the same uniforms. The frozen kernels of `src/mc/` and the forward-mode `Dual` play no part in run 2; they keep reproducing run 1 (below).

##### Knock-in rows, 12

Contracts. A knock-in pays (S_T − K)^+ at T if the barrier was hit, with no rebate. Dates t_i = i T / m, i = 1..m, as every plan contract; they scale with T.

| rows | instrument (unchanged) | product | terms | monitoring, treatment of the knock-out leg | the same terms as |
|---|---|---|---|---|---|
| `P3.KID.*` | `knock-in-parity:discrete` | down-and-in call | S0 = K = 100, H = 95, σ = 0.30, r = 0.10, q = 0, T = 0.2, m = 50 | at the 50 dates, one-step survival | `DB095` (O5, O6) |
| `P3.KIC.*` | `knock-in-parity:continuous` | down-and-in call | the same | continuous, the bridge on the 50 steps | `CB095` (O7) |

Why these terms:
- Parity makes the knock-in exactly vanilla minus the knock-out with the same monitoring. On these terms the knock-out leg is already judged, in run 1 and again in run 2: its value and discrete delta against BGK's published O5, O6 and O7, and its other Greeks by DB095's finite-difference rows and CB095's Reiner-Rubinstein rows. So if a knock-in row fails while its knock-out row passes, the fault is in what the knock-in adds: the vanilla leg on an m-step path, the subtraction, and the tape through two kernels.
- Both rows have one contract under two monitorings. Their gap is BGK's discrete-continuous gap (O5 against O7) with its sign reversed, and the pricer must never use the bridge on the discrete one.
- The discrete reference needs no new quadrature contract. The O5 and O6 recursions, already cross-checked by two methods, carry over unchanged.
- The published knock-ins found in Phase 1 are El Babsiri-Noel's (oracles E31): S = K = 100, H = 90, r = 0.05, q = 0.02, T = 1. They are on other terms and have no Greeks. They are used below as a guard on the closed-form code, not as a row.

Estimator. The production pricer's knock-in, `Knock::kIn` (`src/pricer/kernels.hpp`): on each path, the European payoff on the unconditioned normals minus the knock-out leg, which under one-step survival runs on its own conditioned path. Greeks come from the tape, one record per path through both kernels. N = 2^22, the plan's barrier count, so at seed 271828 a knock-in row reads the same uniforms as its knock-out row. That correlation is allowed: Bonferroni needs no independence. The ceiling factor is 1.65, the existing rule for reference rows.

Greeks, as `src/oracle/black_scholes.hpp` and every existing row define them. Value is in units of S0, discounted at e^{−rT}. Delta is ∂V/∂S0. Vega is ∂V/∂σ per 1.00 of annual volatility. Rho is ∂V/∂r per 1.00 of the continuously compounded annual rate, which enters both the drift and the discount. Dividend rho is ∂V/∂q per 1.00, and q enters only the drift (r − q). Theta is −∂V/∂T per year, with m fixed and the dates moving with T. Nothing is scaled to 1% or to a day.

References, one id per row in `references/oracle_values.csv`, generated by `tools/quadrature`. The new rows are appended after `O11`, and every existing row stays byte-identical, which the regenerate-and-compare ctest shows. Each row's `parameters` field is DB095's (or CB095's) text, `S0=100;K=100;H=95;sigma=0.3;r=0.1;q=0;T=0.2;m=50;type=call;spot=0`. Each plan row's `reference` is its own id, as `CB085.value` is.

| rows | reference | b |
|---|---|---|
| `P3.KID.value`, `.delta` | `BlackScholes` over `test::Dual` (closed form) minus the backward GL recursion over `test::Dual`, at the fine resolution of `AddDiscreteBarrier` | 1e-9 |
| `P3.KID.vega`, `.rho`, `.divrho`, `.theta` | the same, other tangents | 1e-8 |
| `P3.KIC.*`, all six | `BarrierOptionValue(kDownIn, kCall, K = 100, H = 95, rebate 0)` over `test::Dual`: Haug's C(1, 1) term, since K ≥ H | 0 |

The discrete quadrature over `Dual`:
- `DiscreteDownOutGaussLegendre` and `DiscreteDownOutSimpson` become templates on `Real`. The double functions are their double instantiations, so O5, O6 and the naive rows keep their bits.
- The grid is fixed at the base point's double values: the nodes, weights, panel and interval counts, the top, and the kernel band. Only each step's drift, sd and discount carry tangents, and so does the last step's closed form, through `test::ActiveMarket` with spot, σ, r, q and T seeded. The reason: the node counts change in steps with σ and T (`ceil`), so a grid that moved with the tangent would add a term that is not differentiable. With the grid fixed, the tangent is the fixed rule applied to the derivative of the integrand. That derivative is a Gaussian times polynomials of degree at most 2 in each step's z, so it converges as the value does, and its tail beyond 12 sd is below e^{−65}.
- Simpson's two grids are extrapolated, (16 f − c) / 15, tangent by tangent. Its delta for this purpose is the spot tangent at the root. The existing five-point stencil stays for O6.

Checks in the tool. Each failure is a `DISAGREEMENT` and fails the tool and its ctest:
1. Coarse against fine GL, tangent by tangent, within that row's b.
2. Fine GL against Simpson, the same.
3. The GL `Dual` delta against the GL score delta (O6) within 1e-9.
4. For KIC, (DI + DO)(Dual), with DO the closed form of the CB095 rows, against `BlackScholes`(Dual), value and five Greeks, within 1e-11 absolute.
5. For KIC, the DI value against BS minus `ContinuousDownOutCallBgk` (BGK Proposition 3.1, the tool's own formula) within 1e-11. Those two formulas share no code.
6. The E31 guard: `BarrierOptionValue(kDownIn)` at the five El Babsiri-Noel rows (σ = 0.10 to 0.30) within 1e-5 of 0.07187, 0.60638, 1.64005, 2.98495 and 4.50952. 1e-5 is QuantLib's own analytic tolerance for that table. Half a unit in the fifth decimal would claim more than the source checks.

Why each b:
- KIC is closed form, as CB095 is, so b is 0. Double rounding is near 1e-14, far below any SE.
- KID value and delta inherit O5's and O6's 1e-9, since the Black-Scholes leg is exact.
- The other four KID Greeks get 1e-8. Their integrands carry the polynomial factors above, and 1e-9 has been shown only for the value and delta. The checks above enforce the b that is used. At an SE of order 1e-3 to 1e-2, either value is invisible in the bound.

What the knock-in rows prove:
- The vanilla leg is right on an m-step path.
- The parity subtraction has the right signs.
- The knock-out leg's adjoint enters the knock-in's Greeks with a minus sign.
- Under one-step survival, the vanilla leg reads the unconditioned draws while the knock-out leg conditions its own.

What they do not prove: anything about the knock-out leg beyond its own rows, since its error enters both.

##### SVI-surface rows, 45

A row is d P_c / d θ_{j,i}, for the three 7a contracts c (arithmetic Asian call; discrete down-and-out call, one-step survival; continuous down-and-out call, bridge; K = 22,800, H = 21,500, T = T₃, m = 33, n = 4). The 15 parameters are (a, b, ρ, m, σ) of slices j = d05, d12, d33, that is 202411, 202411W4 and 202412 at 5, 12 and 33 days, in the order of `runner.cpp`. The model is stage 7a's: raw SVI as fitted by stage 6b's code from the captured chain, violations and all, linear total variance in t, Dupire with 7a's floor rule, and log-Euler with the coefficient frozen per step. The derivative is the partial derivative of the Monte Carlo price with F, D, T and S0 held fixed, with no refit, as 7a registered. The unit is index points per unit of the parameter: a and b in total variance, ρ dimensionless, m and σ in log-moneyness.

Options weighed. The reference must not depend on the tape.

(a) A central difference, on common random numbers, of the same production price estimator.
- What it is: `PriceLocalVol` at θ ± h, one parameter moved, on the run's own uniforms, paired batch by batch with the tape.
- What it proves: the tape's 15 numbers are the derivatives of the very function the pricer computes. That covers:
  - the whole surface pipeline on the tape, from θ to w, w′ and w″ per slice, through the interpolation weights, ∂T w and g, to σ²_loc;
  - the floor rule's branches, where a floored or capped point carries no derivative;
  - the frozen-coefficient law, and one-step survival and the bridge on the sub-steps;
  - the order in which the 15 inputs are registered, and the set-up records σ² and b σ²;
  - the per-block set-up sweep and its allocation to batches.
- Beyond what 7a's per-path tape-against-`Dual` check shows, it also catches:
  - a partial or a branch that the tape and the `Dual` share through the template code, which only a difference of the primal can expose;
  - in expectation, the boundary term that the pathwise derivative misses where g crosses 0 and σ²_loc jumps from the cap to the floor (7a, floor rule, "Consequences"). A difference straddling the jump on a path-step adds J / 2h with probability of order h, so its mean keeps the term while the pathwise derivative drops it.
- The Euler bias cancels, since both sides differentiate the same n = 4 scheme. So the rows check the pathwise estimator, the surface pipeline and the set-up sweep.
- What it does not prove:
  - That the price is right. An error common to the primal and the tape passes: a wrong Dupire denominator such as VW4's displayed sign, a wrong interpolation, curve or floor rule. Those are 7a's flat-surface oracle and Dupire checks.
  - Anything about the continuous local-volatility model's sensitivities, or the Euler bias of the gradient. That is 7a's bias study, which covers prices only.
  - Anything about the fit or the sensitivities to the settlements. That is 7b.
  - A boundary term at g = 0 where no path-step lies within a bump of it. The tallies below say whether any did.
  - Independence from `EvaluateSubstepped`, which both sides run.

(b) A PDE solution of the continuous local-volatility model for the two barriers.
- What it is: one dimension in log S, absorbing at H for the continuous contract, zeroed below H at the 33 dates for the discrete one, the 15 derivatives by bumps or a tangent PDE, and some other method for the Asian, whose average needs a second state dimension.
- What it proves: it is an independent implementation of the model, so it would catch an error common to the Monte Carlo primal and its tape.
- What it does not prove, at a b we could honestly state:
  - Its b must hold the derivative of the n = 4 Euler bias in each parameter, which nothing has measured. 7a measured the price bias only: +0.33 ± 0.11 for the bridge, and unresolved for the discrete barrier at 2^20. It could be estimated only by coupled Monte Carlo levels, which reuse the code under test, and only with a noise we cannot bound before running it.
  - Near the floors σ_loc has kinks in x, so the PDE's grid convergence there is first order too: the same slow convergence 7b saw.
  - It judges a different quantity from the one the rows define, the continuous model's derivative rather than the Monte Carlo price's.
  - It covers 30 rows, not 45, and needs a new solver with its own oracles.

(c) The forward-mode `Dual` through the same kernels.
- What it is: independent of the tape, and 7a already checks it path by path.
- Why it is not the row's reference: it shares every partial and branch with the tape through the template, so it misses what both AD modes would miss.

Choice: (a), for all 45 rows. It is the only option that judges the quantity the rows define, at a b that the pilot estimates rather than assumes. The model-level check (b) would add stays an open question for the lead (below). It is not part of stage 8.

The step rule.
- For row (j, i): h0 = 2^-8 w_j(0) / M_{j,i}.
  - w_j(0) is the fitted at-the-money total variance, a + b (−ρ m + √(m² + σ²)), about 5.3e-4, 1.5e-3 and 4.4e-3.
  - M_{j,i} is the largest |∂w_j / ∂θ_i (k)| on the grid k = −0.5, −0.499, ..., 0.5, from the closed forms ∂w/∂a = 1, ∂w/∂b = ρ (k − m) + r, ∂w/∂ρ = b (k − m), ∂w/∂m = −b (ρ + (k − m) / r) and ∂w/∂σ = b σ / r, with r = √((k − m)² + σ²).
- So the first move changes the slice's total variance by at most 2^-8 of its at-the-money value anywhere a path goes.
- The grid is not the settlements' range of k, which is where 7b's trouble came from. ∂w/∂m and ∂w/∂b are largest to the right of the vertex m, beyond every settlement, where the floored path-steps are (k from 0.071 to 0.134). ±0.5 is about ±8 standard deviations of the 33-day log return and holds every floored point 7a found. The 52 capped path-steps at k near −1.1 lie outside it.
- Levels: h_ℓ = 2^-ℓ h0, ℓ = 0..8.
- The step must stay inside the validated domain on both sides: |ρ ± h0| ≤ 1, σ − h0 ≥ 1e-8, b − h0 ≥ 0. Otherwise the row is refused as unresolved, never clipped. From the 6b parameters:
  - the ρ steps are at most about 8e-6, against margins 1 − |ρ| of at least 0.0085;
  - the σ steps are at most about 1e-5, against σ ≥ 0.011;
  - the largest step of all, about 2e-4 for 202412's b, is far below b ≈ 2.
- Example: for 202411W4's m, h0 ≈ 1.5e-6 and h_8 ≈ 6e-9. 7b's finest parameter move was 1e-6 of m, about 1.5e-7.

The ladder and the level, on the pilot only, from finite differences alone, never the tape.
- Seed 1729, N_p = 2^20, 64 batches, the run's contract and threads.
- Per batch q: D_{ℓ,q} = (V_q(θ + h_ℓ) − V_q(θ − h_ℓ)) / (2 h_ℓ), with V_q the batch mean of `PriceLocalVol`, every level and both sides on the same uniforms.
- Δ_ℓ = D_ℓ − D_{ℓ+1}, ℓ = 0..7, and E_ℓ = Δ_{ℓ+1} − Δ_ℓ / 4, ℓ = 0..6. Each is formed per batch, with mean and SE from the 64 values (`BatchMeansStatistics`). c is the plan's critical value.
- Pair ℓ is consistent with h² truncation when |mean E_ℓ| ≤ c SE(E_ℓ) + |mean Δ_ℓ| / 16. Under D(h) = D + A h² + B h⁴, E_ℓ = −(45/256) B h_ℓ⁴. So the slack admits the level only while the h⁴ term is at most about a quarter of the h² term there.
- The chosen level ℓ* is the smallest ℓ in 0..5 with pairs ℓ and ℓ + 1 both consistent, so h² is seen over two further halvings. One consistent pair alone can be a truncation that passes through zero, and two in a row cannot.
- The reference step is h* = h_{ℓ*+1}, and the plan's reference field is `FD:<h*>`, printed with 17 significant digits.
- b = RoundUp3(2 (|mean Δ_ℓ*| + c SE(Δ_ℓ*)) / 3). That is BuildPlan's existing FD formula, with FD(2h) − FD(h) = Δ_ℓ*, so `two_h_mean` and `two_h_se` are those of Δ_ℓ*.
- Why the coarsest such level: the paired SE against the tape comes from the paths whose floor or cap kink lies within ±h. A per-path difference there is bounded by the slope jump, so its variance grows like h, while the truncation is A h² in expectation. A larger h keeps more such paths in each batch, so the SE that sets the ceiling rests on more of them. The h² behaviour is shown, not assumed. Round-off differs path by path, so it sits in the paired SE, not in b.
- No ℓ ≤ 5 qualifies: the row's reference is unresolved (below).

Stage 7b's slow convergence, accounted for. 7b's check differenced settlement bumps through refits. Each bump moved all five parameters of a slice by up to min(σ_i, V_i / 2), and it judged six absolute halvings on 2^16 paths without an SE, so a sequence still moving at h_5 was taken as settled. Its probe of 202411W4's parameters found ρ and m still 0.6% to 4% from the tape at 1e-6 of the parameter. Here:
- each row moves one parameter, scaled to where the paths go rather than the settlements;
- the ladder reaches about 25 times below 7b's finest move;
- every level is a paired batch statistic with its SE;
- h² must be seen over two halvings;
- a sequence that never settles is reported, not passed.

The rows most at risk, from 7b's probe, are `P3.DB.d12.{rho,m}` and `P3.CB.d12.{rho,m}`, and plausibly their d05 counterparts. Their ladders are printed in full.

Floor tallies, a disclosure with no pass mark:
- For every bumped price the pilot records the FloorTally's butterfly-floored and capped counts against the base run's. A change in the butterfly count means some path-step crossed g = 0 inside the bump, and so the cap-to-floor jump, inside it. The changes are written to the ladder file per row and level, and reported.
- Run 2 reports the same at h*.
- 7a found no butterfly-floored point and 52 capped path-steps at 2^20.

The estimate and the comparison. The tape's batch means at N = 2^22, seed 271828, against D_{h*} on the same uniforms, by the existing `CheckPaired`: |mean(tape − D)| ≤ c SE_paired + b, and SE_paired ≤ the ceiling. The ceiling is 3 × SE_pilot × √(2^20 / 2^22), the existing factor for FD rows: their paired differences are mostly zero, with spikes at the kinks, which is the case that factor was set for. The brief named 1.65, and the reason for 3 is above; see the open questions. N = 2^22 is the plan's barrier count. The number of spikes in each batch grows with N, and 2^22 runs in minutes per contract.

Rows that cannot be judged:
- None is known to be unjudgeable in advance.
- A row with no resolved level gets plan status `unresolved`. It counts in K. It is run in run 2 at the finest level h_8, with its estimate, D_8 and paired SE printed. Its verdict is `UNRESOLVED`, it counts as not passed, and the run exits non-zero. The critical value is unchanged.
- The g = 0 boundary term, for every row, is judged only to the extent that path-steps lie within the bumps. The tallies say how far that is.
- No row is dropped, and K stays 147.

##### The plan, the runner and the files

- Instruments.
  - `knock-in-parity:discrete` and `knock-in-parity:continuous` resolve, through a fixed table in `src/oracle/catalog.hpp`, to the two contracts above, with DB095's and CB095's parameter text. The runner's check that a reference quotes its instrument's parameters uses the resolved text.
  - `svi-surface:arithmetic`, `svi-surface:discrete-barrier` and `svi-surface:continuous-barrier` resolve to 7a's three contracts on the fit of the captured chain under `--data` (default `data/`).
  - Greek `d_<p>_<dNN>` decodes to the parameter index p in (a, b, rho, m, sigma) and the slice index d05, d12, d33 → 0, 1, 2.
- `aadp_run_oracles run --estimator reference` is the default and what the ctest `aadp_oracle_plan_measured` and `check.sh`'s measured-plan step call.
  - It judges only the rows the frozen kernels can price, the 90 of run 1.
  - It prints each of the 57 as `<id> not-in-run-1`, a line `scripts/compare_measured.py` does not read, and counts them as `run2_only` in the summary.
  - So run 1 keeps coming back byte for byte, and those tests keep their scope and time limit.
- `--estimator production` judges all 147 rows as above. It refuses a plan with any `awaiting-phase3` row. Without `data/` the 45 surface rows are `SKIPPED: no data`, which fails the run as any skip does.
- Every other consumer of the plan's run rows (`tests/ad/oracle_equivalence_test.cpp`, `tests/pricer/path_agreement_test.cpp`, `tests/oracle/runner_test.cpp`) keeps its scope to the GBM rows by instrument kind, and says so. The runner test's count changes from 90 run and 57 awaiting to 102 GBM rows and 45 surface rows, with `unresolved` counted apart.
- The plan decoder accepts the status `unresolved`. Such a row has N, seed and `FD:<h_8>`, and writes `-` for `se_ceiling` and b.
- `aadp_run_oracles pilot --rows stage8 --estimator production --plan references/comparison_plan.csv --seed 1729 --divisor 4 --threads 8 --data data --references references/oracle_values.csv --out references/pilot_results_stage8.csv --ladder references/svi_fd_ladder.csv`.
  - It refuses a seed equal to the plan's `measured_seed`.
  - `pilot_results_stage8.csv` has the 57 rows in `pilot_results.csv`'s seven columns, under the same directives (seed 1729, 64 batches, divisor 4) and a comment naming the estimator. For a knock-in row, `estimate` is the tape mean, `reference` the reference value, `se` the estimator's SE, and the two-h columns 0. For a surface row, `estimate` is the tape mean, `reference` the mean of D_{h*}, `se` the paired SE, and the two-h columns those of Δ_ℓ*.
  - `svi_fd_ladder.csv` has, per row and level: h_ℓ, the mean and SE of D_ℓ, Δ_ℓ and E_ℓ, the consistency verdict, and the tally changes. Per row it gives ℓ*, h* and b, or `unresolved` with its reason.
- `emit-plan` gains `--pilot-stage8` and `--ladder`. It writes the 57 rows by the rules above:
  - 1.65 on the 12 knock-in rows, with b the reference's;
  - 3 on the 45 surface rows, with b and `FD:<h*>` from the ladder.
  - The 90 run-1 rows, every directive and every existing comment line come out byte-identical to 5993b4a's. One comment line is appended for each new row kind, giving its estimator, reference and factor.

##### The pilot

- Seed 1729, divisor 4: N_p = 2^20 for all 57 rows, 64 batches, 8 threads. The run goes through `heavy.sh` with `/usr/bin/time -l`, with the captures checked first by `scripts/fetch_data.sh --verify`.
  - Knock-ins: one tape run per contract.
  - Surfaces, per contract: one tape run and the full ladder, 15 × 9 × 2 = 270 prices.
- Ceilings by the existing rule, `factor × SE_pilot × √(N_p / N)`, rounded up to 3 figures: factor 1.65 on the 12 reference rows, 3 on the 45 finite-difference rows.
- Disclosed with the pilot and used for nothing: each row's distance / SE, as Phase 2's pilot printed it. For each surface row: b / (c SE_paired), the bound's resolution relative to |estimate|, and the tally changes.
  - Also the SE at a second development seed, 20261129, at N_p and at h*. Its ratio to the 1729 SE tests the basis of factors 1.65 and 3 on these rows. A ratio above 2.9 (or above 1.62 on a knock-in row) is reported before run 2 as a risk to that row's ceiling. Nothing changes because of it.
- A pilot distance beyond c is a pilot finding, investigated before run 2. If it shows a bug, the fix gets its own commit and the pilot is repeated on 1729 at the fixed commit. Both pilots are reported. No ceiling or b is ever set from a measured result.

Commit order:
1. this registration, alone;
2. the code, in small commits, each through the full gate: the quadrature templates, references and checks; the plan and runner modes; the ladder, the rule and the tests below; the faults;
3. the pilot's two files, alone, with the raw logs in the research cache;
4. the plan from `emit-plan`, alone;
5. the fingerprint file and its test (below);
6. NOTES "Stage 8a results".

Then push, confirm that CI is green at 6, and only then run on 271828. No seed 1729 or 271828 run happens before its step.

##### Measured run 2, stage 8b

- Once, at commit 6 (docs-only after 5, so the code under test is 5's), all 147 rows, seed 271828.
- Preconditions: a clean tree; CI green on it; `fetch_data.sh --verify` passes; the fingerprint test passes on the Mac.
- `aadp_run_oracles run --estimator production --plan references/comparison_plan.csv --references references/oracle_values.csv --data data --threads 8`, through `heavy.sh`. It is a correctness run, not a timing, so it goes through `heavy.sh`, not `measure.sh`. Host: `mac-m4max`, macOS. The thread count changes no bit.
- If the run dies before it prints a judged row, it may be restarted at the same commit, and the partial log is kept and reported. Once a row is judged, the run is done, and nothing is rerun or re-seeded.
- The raw output goes to `phase3/evidence/stage8b/` in the research cache. `references/measured_results_run2.csv` holds it in run 1's columns, and `compare_measured.py` must rebuild it byte for byte from the raw output. Its header names run 2, the plan commit, the code commit, the seed, B, K, the critical value, the estimator, the host, and the summary (run, passed, failed, unresolved, skipped). It also carries the TAIFEX attribution and the statement that the 45 surface rows rest on settlements of one day, not tradable quotes, reaching 33 days.
- Which run-1 rows it repeats: all 90, at the same N, seed, ceilings and b, with the production estimator in place of the frozen kernels and the `Dual`.
  - The production pricer matches the frozen kernels path by path within 0.047 of its bound, and the tape matches the `Dual` within round-off (check 7). So run 2 re-reads the same paths with other code; it is not a second independent look at these 90 rows.
  - A verdict that changes points to the code, not to chance. A run-2 failure is a failure even where run 1 passed.
- NOTES "Stage 8b results" reports run 2 beside run 1, never instead of it:
  - for the 90 rows, the run-1 estimate, the run-2 estimate, the difference, and both verdicts;
  - the 57 alone, with every failure and every unresolved row named;
  - the surface rows' tally changes.
- `references/measured_results.csv` stays as it is. README and `BENCHMARKS.md` show both runs.

Later stages, the fingerprint:
- `references/run2_fingerprint.txt` holds the production-mode output of all 147 rows at the development seed 20261128, N / 1024 paths per row, 64 batches, surface rows at their plan h*. Every estimate, reference and SE is a hex float.
- The ctest `aadp_run2_fingerprint` (release presets) compares it bit for bit. The 102 GBM rows run on every host. The 45 surface rows run only with the captures and otherwise skip visibly, with the named skip of the other real-data tests. The GCC pre-commit and CI run it.
- If any host's bits differ from the Mac's, stage 8a stops and reports to the lead before run 2. It does not keep one file per host.
- A later stage that changes the pricer or the tape either keeps this test passing, or says in its report and its commit that run 2 applies to commit 6 only, and re-pins the file in a commit that says so. Run 2 is never repeated on 271828.

##### Tests at development seeds

Seeds 20261128 (tests) and 20261129 (the pilot's second seed); neither is 1729 or 271828.
- `aadp_knock_in_rows_test`: the 12 comparisons at N = 2^18, 64 batches, seed 20261128, through `CheckAgainstReference` with c and b and no ceiling. It is deterministic. Builds other than release use 2^12 and check finiteness only.
- `aadp_svi_rows_test`: 7a's synthetic surfaces A and B, the three products with the terms 7a's synthetic tape-against-`Dual` tests use, N = 2^14, 64 batches, seed 20261128, the full ladder, rule and paired comparison.
  - On A, which has no floors, all 45 must resolve and pass, with the paired SE below 0.1 of the tape's own SE.
  - On B, where floors occur, every resolved row must pass, and the unresolved count is printed.
  - Builds other than release use 2^10 and check that the ladder runs.
- `aadp_fd_ladder_test` runs the rule on hand-made batch tables with known answers:
  - pure h² plus noise: ℓ* = 0, and b by the formula;
  - first-order truncation: unresolved;
  - late settling: ℓ* is the first level with two consistent pairs;
  - one consistent pair that crosses zero: not chosen there;
  - noise only: ℓ* = 0, with b from the SE.
  - It also checks h0 on a hand-made slice whose maximum is known in closed form, the domain refusal for ρ + h0 > 1, and a constructed bump that moves g through 0 on a path-step and must change the tally.
- A real-data test, with the captures: the 15 h0 against the closed form, all inside the domain.
- Plan tests:
  - the emitted plan's 90 rows and directives are 5993b4a's bytes;
  - the 12 knock-in ceilings use 1.65, and the 45 surface ceilings use 3 with `FD:<h*>` equal to the ladder's;
  - an `unresolved` row round-trips, is run, prints `UNRESOLVED`, and makes the run exit non-zero;
  - reference mode leaves run 1's bytes unchanged.

##### Faults

One per new reference and comparison in `faults/manifest.json`, each with the test that must catch it:
- `ki-parity-sign`: the knock-in returns vanilla + knock-out. Caught by `aadp_knock_in_rows_test`, value rows.
- `ki-leg-weighted`: the vanilla leg is multiplied by the path's survival weight. Caught there, KIC value.
- `ki-knockout-leg-passive`: the knock-out leg enters the subtraction as a passive copy of its value. The value is unchanged and the Greeks are the vanilla's. Caught there, the five Greek rows.
- `ki-closed-form-branch`: `kDownIn` with K ≥ H uses C(1, −1). Caught by the tool's parity, BGK and E31 checks.
- `kid-gl-passive-discount`: the GL recursion over `Dual` builds its discount from the passive T. Caught by GL against Simpson, theta and rho.
- `kid-simpson-drift-tangent`: Simpson over `Dual` drops r from the drift's tangent. Caught by GL against Simpson, rho.
- `kid-root-passive-spot`: the root step takes log spot as passive. Caught by the `Dual` delta against the score delta.
- `ki-row-resolves-to-knock-out`: the catalog maps `knock-in-parity:discrete` to the knock-out contract. Caught by `aadp_knock_in_rows_test`.
- `svi-fd-neighbour-parameter`: the ladder bumps parameter i + 1. Caught by `aadp_svi_rows_test` on A.
- `svi-endblock-twice`: the set-up sweep is added twice per block. Caught there.
- `svi-floor-carries-derivative`: a floored point takes floor + (x − Value(x)), so the primal is unchanged and the tape and the `Dual` both get a derivative. Caught by `aadp_svi_rows_test` on B. 7a's tape-against-`Dual` check passes under it, which is what this comparison adds.
- `svi-fd-uncoupled`: the + side uses another seed. Caught by the paired-SE requirement on A.
- `fd-ladder-ratio`: the consistency test uses Δ / 2. Caught by `aadp_fd_ladder_test`.
- `fd-ladder-b-noise`: b drops c SE. Caught there.
- `fd-ladder-single-pair`: one consistent pair is accepted. Caught there, the zero-crossing case.
- `svi-h0-quoted-range`: M is taken over the settlements' range of k. Caught by the h0 test.
- `svi-tally-change-ignored`: the tally comparison is not recorded. Caught by the constructed g = 0 bump.
- `plan-unresolved-passes`: an unresolved row is counted as passed. Caught by the plan test.
- `emit-plan-run1-factor`: run-1 rows are recomputed with factor 3. Caught by the 5993b4a byte test.
- `emit-plan-svi-factor`: surface rows get 1.65. Caught by the plan test.
- `emit-plan-h0-not-hstar`: the plan writes `FD:<h0>`. Caught by the plan test.
- `pilot-measured-seed`: the pilot accepts 271828. Caught by the refusal test.
- `reference-mode-runs-57`: reference mode tries the 57 rows. Caught by `aadp_oracle_plan_measured`.
- `fingerprint-one-ulp`: the GBM discount is multiplied by 1 + 2^-52. Caught by `aadp_run2_fingerprint`.
- `run2-curation`: the curated file drops the verdict column. Caught by a test of the curation on a synthetic runner output.

Open for the lead, none of which changes the rules above unless the lead decides so before the pilot:
- The 45 surface rows take the finite-difference factor 3, which is the existing rule for paired finite-difference rows. The brief named 1.65.
- Filling `reference` (now the placeholder `phase3`) is read here as filling in the rows, not changing them.
- Whether a model-level check of the barrier sensitivities, option (b), belongs in a later stage.

Out of scope: a PDE or other model-level reference for the surface sensitivities; sensitivities to the settlements through eSSVI; any change to run 1.

#### Stage 8a results

Iteration only: none of these numbers is stamped, and the raw logs are in the research cache (`evidence/stage8a/`), not under `results/`. The 45 surface rows rest on the stage 6b fit to TAIFEX settlements of 2024-11-15, which are not tradable quotes, and the surface reaches 33 days only. Data: TAIFEX via data.gov.tw, Open Government Data License (NOTICE). Commits: 0e6d88d (the knock-in references), 01dbe41 (the plan and runner modes, the production estimator, the ladder and the tests), e814d82 (the faults), 92f297f (the pilot's two files, alone), a977c5f (the plan from `emit-plan`, alone: the ceilings of the 57 rows), bf484f3 (the fingerprint files, their test and fault), and the commit of this section.

Decided after the pre-registration:
- The lead's answers, before the pilot: the 45 surface rows take the finite-difference factor 3; filling `reference` through `emit-plan` is filling in the rows, accepted so long as ids, instruments, Greeks, batches and K = 147 are unchanged and the 90 run-1 rows, `pilot_results.csv` and the header outside the registered change keep their bytes; no PDE check.
- The fingerprint is kept per host (the lead's answer), replacing the registered single `references/run2_fingerprint.txt` and the registered stop on a cross-host difference: `references/run2_fingerprint_mac-m4max.txt` and `run2_fingerprint_linux-zen1.txt`, each compared bit for bit on its own host. On any other host (CI) `aadp_run2_fingerprint` checks that the bits do not depend on the thread count and prints how many lines differ from the Mac's, comparing nothing bit for bit. A cross-host difference is reported, not a stop. Each file opens with `#` lines carrying the TAIFEX attribution, which the test skips.
- `reference-mode-runs-57` is caught by `aadp_stage8_plan_test` (`ReferenceModeKeepsRunOnesLines`, the registered plan test that reference mode leaves run 1's bytes unchanged), not by `aadp_oracle_plan_measured` as the fault list says.
- The fd-ladder test's constructed bump: surface A with the second slice's a lowered by 0.001 puts +52 butterfly-floored path-steps on 4096 paths against A's none. The first construction tried, A's σ₀ lowered by 0.0225, gave none.
- `svi-floor-carries-derivative` is a miss, and is not in the manifest. Surface B has about 5 floored path-steps in 4096 paths, and every resolved row on B passes under the fault. A on its own with a lowered by 0.001 also passes under it. Lowering a by 0.003 or 0.01 leaves 15 rows unresolved, and two rows fail there with the correct code, on the g = 0 boundary term the registration says these rows judge only partly. So no development surface found here separates the fault from the code; 7a's tape-against-`Dual` check does not see it either.

The pilot: seed 1729, N_p = 2^20, 64 batches, 8 threads, at e814d82, through `heavy.sh` with `/usr/bin/time -l`, after `fetch_data.sh --verify` passed; 875 s, peak RSS 11 MB (`evidence/stage8a/pilot_1729.log`, `.stderr`, `.meta`). Second seed 20261129, disclosure only.

| group | rows | largest distance / SE | ceilings | b | second-seed SE ratio |
|---|---|---|---|---|---|
| P3.KID | 6 | 1.77 (divrho) | 0.000558 to 0.0169 | the reference's, 1e-9 and 1e-8 | 0.863 to 1.180 |
| P3.KIC | 6 | 1.76 (divrho) | 0.00189 to 0.0413 | 0 | 1.029 to 1.163 |
| P3.AS | 15 | 2.42 (d33.sigma) | 0.00291 to 738 | 0.00424 to 515 | 0.086 to 1.325 |
| P3.DB | 15 | 2.82 (d33.sigma) | 0.00568 to 1060 | 0.0244 to 775 | 0.159 to 1.138 |
| P3.CB | 15 | 2.77 (d33.sigma) | 0.00544 to 1060 | 0.0252 to 774 | 0.149 to 1.321 |

- No pilot distance exceeds c = 4.266. Every surface row resolves at ℓ* = 0, so h* = h0 / 2; no row is unresolved.
- No second-seed ratio passes 1.62 (knock-ins) or 2.9 (surface rows), so no row's ceiling is reported at risk. Ratios far below 1 (0.086 on P3.AS.d12.b, 0.149 on P3.CB.d33.m, 0.159 on P3.DB.d33.m) mean the pilot's paired SE on those rows was raised by a few spikes, so their ceilings are loose rather than tight.
- b / (c SE_paired) runs from 0.238 to 2.561 (P3.DB.d05.m and P3.CB.d05.m).
- Resolution, (c SE + b) / |estimate|: above 1 on seven rows, where the bound is wider than the sensitivity and only a gross error would fail: P3.CB.d05.rho (54.1), CB.d05.a (25.6), CB.d05.sigma (5.97), DB.d05.a (2.34), CB.d12.a (2.31), DB.d05.rho (2.04), DB.d05.sigma (1.66). Above 0.1 on 16 rows, all barrier rows; none on the Asian.
- Floor tallies of the base runs, of 138,412,032 path-steps: the Asian 0 butterfly-floored and 86 capped, the discrete barrier 0 and 0, the continuous barrier 0 and 86. At h* the butterfly count changes on no row, and the capped count by one path-step on one side on six rows (P3.AS.d05.a, AS.d12.a, AS.d33.rho, CB.d05.a, CB.d12.a, CB.d33.rho). No path-step crosses g = 0 inside any bump, so the g = 0 boundary term is not exercised by these rows.

The plan (`emit-plan --pilot-stage8 --ladder`), checked against the plan before it: every directive the same and K = 147; the seven existing comment lines kept and two appended; the 90 run-1 rows byte-identical; the 57 rows' id, instrument and Greek unchanged and in order, all with status `run`. `measured_results.csv`, `pilot_results.csv` and `oracle_values.csv` did not change.

The fingerprint: every row of the committed plan (a977c5f) through the production estimator at seed 20261128, N / 1024 = 4096 paths per row, 64 batches, with the captures, as hex floats. The Mac's file is from `aadp_run_oracles fingerprint` at 8 threads through `heavy.sh`; the Linux host's is from its GCC pre-commit build of a977c5f's tree, through its `heavy.sh`, under the same `#` lines. On each host the test also runs 1 thread and finds the same bits.
- The 102 GBM rows, knock-ins included, are the same to the bit on both hosts.
- All 45 surface rows differ between the hosts: by up to 2.7e-4 relatively in the tape's mean (P3.CB.d05.m), 0.47% in the central difference (P3.DB.d05.a) and 23% in the paired SE (P3.DB.d05.b). Each host fits the surface from the captures itself, through its own libm, which is the likely source; it was not traced further. The difference of the two hosts' tape means reaches 241 paired SEs (P3.CB.d12.b), since the paired SE measures the tape against the central difference on one surface, not across surfaces. Each host's own comparison is in line: the largest |tape − D| / SE_paired is 2.43 on the Mac and 2.80 on the Linux host, against c = 4.27.
- Under the lead's answer this is reported, not a stop. Run 2 runs on the Mac and is pinned by the Mac's file.

Faults: 24 new in `faults/manifest.json`, 315 in all. The 23 of e814d82 ran by `python3 -I scripts/fault_injection.py --only <ids> --manifest faults/manifest.json` through `heavy.sh` with the toolchain sourced, on 01dbe41's code (e814d82 changed the manifest only), with `svi-floor-carries-derivative` beside them: 23 caught, that one not (`evidence/stage8a/faults-01dbe41.log`). `fingerprint-one-ulp` ran at bf484f3 and was caught by `aadp_run2_fingerprint` (`faults-bf484f3.log`).

The miss, closed after stage 8a: `svi-floor-carries-derivative` is now caught, with no change to `src/`. Its edit does change the tape. A floored step keeps its value's bits and gains the unfloored d(dT w / g), on the tape and the `Dual` alike. It was missed for two reasons. First, no check had the power to see it. B floors about 5 path-steps in 4096 paths and the real surface 0.385%, too few to move a price-level row beyond c SE + b. Second, nothing looked at a floored step's derivative directly. The floor-rule unit tests check classes, values, bits and finiteness, and 7a's tape-against-`Dual` check compares two recordings of the same template. `LocalVolFloors` in `tests/ad/local_vol_gradient_test.cpp` (fa766db) now makes that check. Per path, on common random numbers, it compares the tape's 15 derivatives with central differences of the double path at h = 2^-20 max(|θ|, 2^-7) and at h / 2. It runs five synthetic surfaces, each surface A with one class over a wide region: non-positive variance, calendar, butterfly, below the floor, and the cap. Each surface has 256 paths, European and Asian calls at strike 25, 4 sub-steps, seed 20261117. The tolerance, stated before the test first ran, is 2 |D_h − D_{h/2}| + 2^-20 of the path's largest |D_{h/2}|, on paths whose step classes no bump changes; at most 2 of 256 are left out. The code passes at 0.64 of the tolerance at worst. Under the fault every compared path that crosses a floor fails, and the worst ratio on each floored surface is above 1.4 × 10^8. That includes the 256 paths crossing only non-positive variance, the 256 calendar-only, the 107 butterfly-only and the 255 below-floor-only paths, while the cap test, which the fault leaves alone, passes. The fault is now in the manifest (316 in all) with these tests as its detector, and its reason requires exactly the four floored-class tests to fail. `python3 -I scripts/fault_injection.py --only svi-floor-carries-derivative --manifest faults/manifest.json`, through `heavy.sh` on fa766db's code: caught. Nine faults on `svi/local_vol.*` or with detectors in the edited test file are still caught (`lv-vw4-displayed-sign`, `lv-q-unguarded`, `lv-fuzz-q-unguarded`, `lv-floor-calendar-dropped`, `lv-cap-dropped`, `lv-fuzz-cap-dropped`, `lv-gradient-shuffled`, `lv-block-sweep-skips-a`, `lv-paths-with-floor-uncounted`). `aadp_run2_fingerprint` passes on the Mac, so run 2's code and pin are those of stage 8a. Raw logs: `evidence/stage8a-floor/`.

Gates: every commit through the release and fuzz ctest runs (0 failed, 0 skipped), the format check, the repository scan, clang-tidy on the touched files and the GCC pre-commit on the Linux host.

#### Stage 8b: measured run 2

Run 2 is the one measured run of all 147 rows of the plan on the production pricer and tape, seed 271828, run once. It is reported beside run 1, never instead of it. The 45 surface rows rest on the stage 6b fit to TAIFEX settlements of 2024-11-15, which are not tradable quotes, and the surface reaches 33 days only. Data: TAIFEX via data.gov.tw, Open Government Data License (NOTICE). The curated output is `references/measured_results_run2.csv` (d70a1d4, alone). `scripts/compare_measured.py` rebuilds its 147 rows byte for byte from the raw output. `references/measured_results.csv` (run 1) is unchanged, as `git diff` shows.

The run:
- Commit 4398aee, pushed, with CI green (run 42897836941). Registered for commit 6 (a418764), but the commits between differ from it only in `tests/ad/local_vol_gradient_test.cpp`, `faults/manifest.json` and this file. `src/`, `tools/`, `cmake/` and `references/` are those of bf484f3, the code commit the registration names.
- Checks before the run, all passed: a clean tree; `git diff a977c5f -- references/comparison_plan.csv` empty; `aadp_run2_fingerprint` (`Run2Fingerprint.CommittedBitsOnThisHost`) passed on the Mac, comparing its file bit for bit; `scripts/fetch_data.sh --verify` passed on both captures. The release build had nothing left to build at that commit.
- Host `mac-m4max`, macOS 27.2 (26B123), Apple M4 Max, clang 23.1.4, CMake 4.4.5, the `release` preset. As the registration fixes, this is a correctness run, so it ran through `heavy.sh`, not `measure.sh`, at nice 0, with `/usr/bin/time -l`. Run 1 (32dd5e1) was recorded on the Mac too: no Linux build passed until 89dbee8, which came after it.
- Command: `aadp_run_oracles run --estimator production --plan references/comparison_plan.csv --references references/oracle_values.csv --data data --threads 8`.
- Wall time 623 s (2026-11-10, 21:28:24 to 21:38:47 UTC), 4,976 s of user time, peak RSS 11.7 MiB. Exit 1, because two rows failed.
- One attempt, complete. Nothing was lost or rerun.
- Raw output in the research cache, `phase3/evidence/stage8b/` (`run2_271828.log`, `.stderr`, `.meta`).

Result: 145 of 147 pass, 2 fail, 0 unresolved, 0 skipped. The critical value is c = 4.266218.

| group | rows | passed | largest abs(z), run 2 | largest SE / ceiling |
|---|---|---|---|---|
| European (VA, VB, VC) | 18 | 18 | 1.60 (VC.value) | 0.643 |
| geometric Asian (GA) | 6 | 6 | 0.92 (GA.delta, rho, divrho) | 0.599 |
| arithmetic Asian (AA) | 12 | 12 | 2.50 (AA50K100.delta) | 0.710 |
| discrete barrier, one-step survival (DB, DBS) | 36 | 36 | 2.59 (DB090.vega) | 0.842 (DB099.vega) |
| continuous barrier, bridge (CB, CBK90) | 18 | 18 | 0.82 (CBK90.value) | 0.843 (CB085.theta) |
| P3.KID | 6 | 6 | 2.00 (rho) | 0.644 |
| P3.KIC | 6 | 6 | 0.68 (value) | 0.643 |
| P3.AS | 15 | 15 | 1.63 (d05.b) | 0.783 (d12.rho) |
| P3.DB | 15 | 14 | 1.76 (d33.sigma) | 1.385 (d12.rho) |
| P3.CB | 15 | 14 | 1.82 (d33.sigma) | 1.565 (d12.rho) |

z is (estimate − reference) / SE. On FD rows, SE is the paired SE.

The failures. Both fail the SE ceiling only. Their distances are small: 0.13 and 0.15 of the bound.

| row | estimate (tape) | reference D_{h*} | z | bound, c SE + b | SE | ceiling | SE / ceiling |
|---|---|---|---|---|---|---|---|
| P3.DB.d12.rho | −50.303 | −59.953 | +0.854 | 71.92 (b 23.7) | 11.30 | 8.16 | 1.385 |
| P3.CB.d12.rho | 940.520 | 929.857 | +0.942 | 72.28 (b 24.0) | 11.32 | 7.23 | 1.565 |

- These are two of the rows the registration named as most at risk: `P3.DB.d12.{rho,m}` and `P3.CB.d12.{rho,m}`.
- Their pilot paired SEs at 2^20 were 5.44 and 4.82. The ceiling is 3 times that, scaled by √(2^20 / 2^22). Run 2's SE is 4.16 and 4.70 times the scaled pilot SE.
- At stage 8a the second seed's SE ratios on these rows were 1.138 and 1.321, under the 2.9 that would have flagged them.
- The two `d12.sigma` rows came next: 0.837 and 0.951 of their ceilings.
- By the rule both rows fail, and they stay failures. No ceiling, b, step or seed changes, and nothing is rerun on 271828.

Run 1 beside run 2, the 90 shared rows:
- All 90 pass in both runs. No verdict changes.
- Every row prints the same estimate (10 decimals), reference and SE (5 significant figures) in both files. So the run-2 minus run-1 difference is 0 at printed precision on every row, and the largest abs(z) per group is the same in both runs (the first five rows of the table above).
- 89 rows are byte-identical, bound and verdict included. On DBS.rho the bound differs in its last printed figure (run 1 6.8029e-06, run 2 6.8028e-06), so its SE differs beyond the printed figures.
- As the registration says, this is a re-reading of the same paths by other code, not a second independent look. It shows that the production pricer and tape reproduce the frozen kernels and the `Dual` on all 90 rows, at the precision printed.
- The full per-row comparison is the two committed files, row for row.

The 57 new rows: 55 pass. The two failures are above. No row is unresolved.

Floor tallies, a disclosure with no pass mark. Path-steps per contract: 2^22 × 33 × 4 = 553,648,128.
- Base runs: the Asian 0 butterfly-floored and 292 capped; the discrete barrier 0 and 0; the continuous barrier 0 and 292.
- At h*, the butterfly count changes on no row. So, as in the pilot, no path-step crosses g = 0 inside any bump, and the g = 0 boundary term is not exercised by these rows.
- The capped count changes on 10 rows, all of them Asian or continuous-barrier rows:
  - by one path-step on one side on AS.d05.a, AS.d12.rho, AS.d33.a, AS.d33.b, CB.d05.a, CB.d12.rho, CB.d33.a and CB.d33.b;
  - on AS.d33.rho and CB.d33.rho, by −1 at +h and +2 at −h.
- P3.CB.d12.rho, one of the failures, is among them: −1 at −h. P3.DB.d12.rho has no capped step at all.

The weak barrier rows 8a flagged, where the bound is wider than the estimate. The resolution is (c SE + b) / abs(estimate).

| row | resolution, pilot | resolution, run 2 | z, run 2 |
|---|---|---|---|
| P3.CB.d05.rho | 54.1 | 1.47 | −0.58 |
| P3.CB.d05.a | 25.6 | 10.4 | +1.04 |
| P3.CB.d05.sigma | 5.97 | 2.60 | −0.00 |
| P3.DB.d05.a | 2.34 | 1.16 | +1.01 |
| P3.CB.d12.a | 2.31 | 1.39 | −1.26 |
| P3.DB.d05.rho | 2.04 | 1.61 | −0.56 |
| P3.DB.d05.sigma | 1.66 | 0.755 | −0.02 |

- All seven pass. Six still have a bound wider than the estimate, so only a gross error would fail them.
- P3.DB.d12.rho joins them in run 2 (1.43) because its SE grew.

Investigation, not part of run 2 and not a result. It ran on development seeds, after run 2, through `heavy.sh` at the same commit. The plans were copies of the five `d12` rows (`P3.AS.d12.rho`, `P3.DB.d12.{rho,sigma}`, `P3.CB.d12.{rho,sigma}`) with h*, ceiling and b unchanged, and only N and seed changed. Only the SEs are read; the verdicts in those logs use a K of 5 and unscaled ceilings and mean nothing. Raw logs and plans: `evidence/stage8b/investigation/`.

The paired SE on the d12 rho rows, DB / CB, against N and seed. The 1729 and 20261129 SEs are the stage 8a pilot's.

| N | 1729 | 20261129 | 20261130 | 20261131 | 271828 (run 2) |
|---|---|---|---|---|---|
| 2^20 | 5.44 / 4.82 | 6.19 / 6.37 | 7.39 / 7.37 | 3.77 / 3.88 | |
| 2^22 | | | 3.30 / 3.35 | 7.17 / 7.12 | 11.30 / 11.32 |
| 2^24 | | | 1.66 / 1.66 | | |

What the table shows:
- On one seed, 20261130, the SE falls as N^-1/2 (7.39, 3.30, 1.66). So the SE does scale; it is not stuck.
- At a fixed N it spreads by a factor of about 2 across four seeds at 2^20, and about 3.4 across three seeds at 2^22. The 8a rule, factor 3 on one pilot, allows about 2.9 in the ratio of two SEs.
- 20261131 at 2^22 comes to 0.88 and 0.98 of the two ceilings, and run 2's seed to 1.39 and 1.57.
- This fits paired differences that are mostly zero with rare large spikes: the SE then rests on a few paths. It is the case the factor 3 was set for, but with a wider tail than the Phase 2 FD rows that set it.
- The DB and CB SEs agree within 3% on every seed but the pilot's (5.44 against 4.82 on 1729), though the discrete barrier has no capped path-step and the continuous one has 292. So the spikes are not the caps, and not the barrier treatment. Their source was not traced.
- The tape agrees with the difference on every seed and size: abs(z) is at most 1.28 on the two rho rows, and at most 1.81 on all five rows.
- No check here changes run 2's verdicts.

### Stage 9: Metal GPU backend, prices only

AAD does not run on the GPU. Greeks come from the double-precision CPU tape, and the GPU backend prices only, in float32 ("Decisions", "GPU: price only"). Every GPU number in this stage is iteration only: nothing is stamped into `results/`, and the reported `GPU-1` run comes at stage 11, through `measure.sh`.

#### Pre-registration, stated before any GPU number

Committed alone, before any kernel of this stage ran on the GPU. The only GPU facts known at this commit are Phase 1's probe (`metal-gpu/EVIDENCE.md`, E22 to E32: Apple M4 Max, Apple9 family, no `double`, a 32-bit `mulhi` and a 64-bit `ulong` product correct on one input) and the specification's guarantees.

Contracts: the existing GBM workloads of `bench/aad_workloads.cpp`, all with S0 100, σ 0.2, r 0.03 and q 0.
- `asian12`, the contract of `aad4-asian12`: arithmetic Asian call, K 100, 12 fixings, T 1, S0 not in the average.
- `asian252`, the contract of `mc-asian252`: the same at 252 fixings.
- `barrier156`, the contract of `aad8-barrier`: up-and-out call, K 120, H 150, discrete at 156 dates, T 3, by one-step survival (Gerstner, Harrach, Roth, arXiv 1804.03975v3).

Uniforms. One Philox4x32-10 block per (path, step), with the counter (path low word, path high word, step, 0) and the key (seed low word, seed high word) of `src/pricer/draws.hpp`, and only the block's first word w. That word is the top 32 bits of the production pricer's 52-bit normal uniform, so every column below draws from the production stream, truncated.
- 32-bit: u = (w + 1/2) 2^-32, in double.
- 23-bit: u = (⌊w / 2^9⌋ + 1/2) 2^-23. In float32 it is computed as float(2⌊w / 2^9⌋ + 1) × 2^-24: an odd integer below 2^24 times a power of two, exact under either rounding mode the specification allows (E9, E10). So u runs from 2^-24 to 1 − 2^-24, never reaches 0 or 1, and 1 − u is on the same grid. No `float(uint)` conversion touches a uniform, so the u = 1.0 hazard of E43 cannot arise.

The four columns (TASKS "Metal"; du Toit's and NAG's table, E57):
- A, double with 32-bit uniforms, and B, double with 23-bit uniforms: the production double kernels, `pricer::EvaluatePath<double>` on `GbmModel<double>` (this project's exp, log, AS241 and Cody normal CDF), fed these uniforms in place of the 52-bit ones. Nothing in `src/pricer/` changes, so run 2's code is untouched.
- C, CPU float32, and D, GPU float32: one float32 kernel source, compiled as C++ for the CPU, and embedded as a string and compiled at run time by Metal for the GPU.
  - Host: the step drift μ = (r − q − σ²/2) T/m, the step sd s = σ √(T/m), k = K/S0 and b = log(H/S0) are computed in double and rounded once to float. The kernel works in units of S0, and the host multiplies the reduced double mean by S0 e^(−rT) in double.
  - State: log-moneyness x = log(S/S0), from x = 0, with x ← (x + μ) + s z in the double kernel's order. No `log` is taken of the state anywhere; the barrier enters as the precomputed b. The only `log` in the kernel is the inverse normal's.
  - Asian: A = (Σ e^(x_i)) / m, the sum compensated (Kahan), payoff max(A − k, 0).
  - Barrier, per step: m = x + μ, a = (b − m) / s, p = Φ(a), weight ← weight × p, q = p u, z = Φ^-1(q′), x = m + s z, where q′ = max(q, 2^-24) when p > 0, and q′ = u when p = 0 (the double kernel's dead path, which draws from u). Payoff weight × max(e^x − k, 0) at the last date. The floor at 2^-24 keeps q′ inside the range the float32 inverse normal was fitted on. It binds only where p u < 2^-24, which the double kernel does not floor, so it is part of the float32 kernel's difference from B and is counted in it, not removed.
  - Φ: Cody's erfc, with the regions, coefficients and exp split of `src/pricer/normal_cdf.hpp`, in float32 with the coefficients rounded to float.
  - Φ^-1: Giles' single-precision erfinv ("Approximating the erfinv function", GPU Computing Gems Jade Edition, 2011, Table 5; E47): z = √2 x P(w), x = 2q′ − 1, w = −log(4 q′ (1 − q′)). That w equals Giles' −log((1 − x)(1 + x)) for q′ on the 23-bit grid, and keeps its relative accuracy for a q′ off it. P(w) depends on x only through x², so z(1 − u) = −z(u) exactly on the grid: the inverse normal is symmetric.
  - exp, log and sqrt: `metal::precise::exp`, `precise::log` and `precise::sqrt` on the GPU; `std::exp`, `std::log` and `std::sqrt` on float, the host's libm, on the CPU.
  - GPU compile options: `MTLMathModeSafe` and `MTLMathFloatingPointFunctionsPrecise`, with `#pragma METAL fp contract(off)`. The C++ builds already have `-ffp-contract=off`. So C and D run the same operations in the same order, and can differ only through exp, log and sqrt, the GPU's rounding of + − × ÷, and denormals, each measured by a property kernel below.
  - Reduction: each GPU thread, and the CPU line likewise, prices a span of S = 16 consecutive paths and writes a compensated float32 sum of their payoffs and a float32 sum of their squares. The host sums the spans in double, in span order. S above 64 is refused, so no float32 sum runs over more than 64 terms, and nothing is summed across threads on the GPU.
- Paired differences B − A, C − B, D − B and D − C, each with its SE from the N/S per-span differences of the same paths: SE = sd(d_span) / (S √(N/S)). Each column's own SE is the per-path sample SE.

`GPU-2`, the check, as `BENCHMARKS.md` states it: |D − B| ≤ 0.01 SE_B on each contract, SE_B the Monte Carlo SE of B. The factor is BENCHMARKS', and nothing is loosened after a result. A contract that misses it is reported as a failure at that N, with its measured ratio |D − B| / SE_B. C − B is reported beside it, not judged.

`GPU-3`, reported, not judged: (D − B) / B per contract, with its SE, beside NAG's "around 1e-7" and its published 4.08e-7 (Table 3), which are context, not pass marks. The barrier caveat, stated before measuring (BENCHMARKS `GPU-3`; E55): `barrier156` may land above 1e-7 with no fault, because its weight is a product of 156 float32 survival probabilities, each rounded, and its near-barrier steps carry the float32 rounding of a, p and q. Also reported: (C − B) / B, (D − C) / C, and (B − A) / A, the effect of the uniforms' resolution alone.

N and seeds, all development seeds, never 1729 or 271828:
- The test, under ctest: N = 2^20 per contract, seed 20261201, in Release builds. Every other build runs a smoke size, N = 2^16, with the same bound, as the repo's other sized tests do.
- The table: N = 2^24 per contract, seed 20261202, one run.
- `GPU-1` timing: N = 2^24 per contract, seed 20261203.
- Property-kernel inputs: seed 20261204.

`GPU-1` scope, iteration only in this stage:
- GPU: wall clock from before the buffers are allocated to after the host reduction, across all command buffers. That covers allocation, encoding, the commits, the wait, a status and error check on every command buffer, and the reduction in double. Device creation, the run-time compile of the library and pipeline creation are timed separately and reported apart. Each command buffer's `GPUStartTime` to `GPUEndTime` is a breakdown under the wall clock, never the headline (E64).
- SIMD CPU: `simd::PriceLanes` at 2 lanes on NEON, in double, on the production 52-bit uniforms from the same Philox blocks, 64 batches. Ceiling: 2 double lanes on NEON (`SIMD-1`). The ratio is the SIMD CPU's wall clock over the GPU's, at the same N.
- CPU float32 line: kernel C on the same uniforms and span layout, with the same reduction, so that a GPU ratio does not mix hardware with precision (BENCHMARKS `GPU-1`).
- Both CPU lines on 1 thread and on 16 (the M4 Max's 12 performance and 4 efficiency cores). Five repetitions each after one warm-up; median and minimum. Run through `measure.sh` on macOS, and not stamped.

Command-buffer batch size, by experiment, because Apple's time limit is unpublished in what Phase 1 opened (E66). On `asian252`, the slowest contract, the paths per command buffer double from 2^14, one buffer at a time, each checked for status and error, stopping at the first error or after the first buffer whose GPU time exceeds 2 s. The safe batch is the largest power of two whose buffer takes at most 50 ms of GPU time and is at least 16 times shorter than the longest buffer that completed. Every size tried, its GPU time and any error are reported.

Property kernels on this GPU, compiled with the production options above unless named otherwise. Their results go into this section.
- Arithmetic: +, −, ×, ÷ and sqrt on 2^20 operand pairs against the CPU's IEEE round-to-nearest-even results, bit for bit. Bit patterns from Philox, seed 20261204: random sign and mantissa, exponents from −30 to 30, so every result is normal. Plus constructed ties and near-ties around 1 and 2^24, which say which rounding mode the GPU uses. Mismatches are counted per operation.
- Contraction: a × b + c on 2^20 triples against the unfused CPU result, bit for bit.
- Denormals: the smallest denormal and three others as inputs to x × 1, x + 0 and x > 0, and denormal results from normal inputs (2^-126 × 2^-k): flushed or kept, on input and on output.
- `uint` to `float`: 2^24 + 1, 2^24 + 3, 2^32 − 129, 2^32 − 128, 2^32 − 1 and 2^20 random words against the CPU's round-to-nearest-even conversion, and whether float(2^32 − 1) × 2^-32 is 1.0 on this GPU.
- exp on x = float(−87 + 175 i / 2^22), i = 0 to 2^22, and on the pricer's range, x = float(−3 + 6 i / 2^22): the error in float ULPs of the exact value, against the host's double `exp` of the same float x. `precise` and `fast`, with the host's float `exp` beside them.
- log on x = float(2^t), t = −126 + 254 i / 2^22, and on x = float(0.5 + 1.5 i / 2^22): ULPs of the exact value, and the absolute error on [0.5, 2]. `precise` and `fast`, with the host's float `log` beside them.
- Compensation: the Kahan sum of 1 followed by 2^16 terms of 0.75 × 2^-24, whose plain float32 sum stays at 1, through the production kernel's own compensated add.

Checks, as tests, each with a fault in `faults/manifest.json` that makes it fail for its named reason:
- `GPU-2` on the three contracts, at the test's N and seed.
- Philox4x32-10 known answers on the GPU (TASKS "Pricer"): Random123's vectors at 10 and 7 rounds, those of `tests/oracle/philox_test.cpp`. And the GPU's four words equal `pricer::DrawStep`'s Philox words on 2^16 (seed, path, step) triples, including paths across 2^32 and seeds with a high word.
- The 23-bit uniform on the GPU equals B's double uniform exactly, on the constructed words above and on 2^20 random ones, and lies in [2^-24, 1 − 2^-24].
- Arithmetic and contraction: 0 mismatches under the production options.
- exp and log, `precise`, within the specification's 4 ULP (MSL Table 8.1, E3) on both grids. Stated before measuring: if this GPU exceeds it, the test fails and the gap is reported, with the bound unchanged.
- Denormals and `uint` to `float`: the behaviour measured here is then asserted, so that a change of device or driver shows. These two assert a fact found, not a bound set in advance.
- Compensation: the GPU's compensated sum equals the CPU's bit for bit and the exact 1 + 0.75 × 2^-8, while the plain sum of the same terms stays at 1.
- Reduction: the host reduction of 2^25 span sums of 1 gives exactly 2^25, where a float32 running sum stalls at 2^24.

CI. GitHub's macOS runners are virtual machines (`ci.yml`, "Show whether the runner is a virtual machine"), and whether Metal has a device there is not known. A CI step prints the device, or that there is none. With a device, the GPU tests run and must pass like any other. With none, they skip with the reason, that skip is allowed by name in that job only, and the job summary says so; it is never silent. On this Mac they run, with 0 skips in the gate. Non-Apple builds have no Metal backend (`AAD_PRICER_ENABLE_METAL` off) and run the CPU float32 tests only.

#### Stage 9 results

Iteration only: nothing here is stamped into `results/`. Apple M4 Max, macOS 27.2 (26B123), the release preset, LLVM 23.1.4. The property run and the table first ran on the uncommitted stage 9 tree over 4aa7925, and ran again at d65b8e5, after the clang-tidy fixes, which changed no kernel operation. Every number came out identical, digit for digit. The code is 25db19f. The faults are d65b8e5.

Properties, with the production options and seed 20261204:
- Arithmetic: +, −, ×, ÷ and sqrt give 0 mismatches against the CPU's round-to-nearest-even on 1,048,576 random pairs. Every constructed tie and near-tie agrees too: 1 + 2^-24 gives 1, 1 + 3 × 2^-24 gives 1 + 2^-22, and 2^24 + 3 gives 2^24 + 4. So this GPU rounds to nearest even.
- Contraction: 0 of 1,048,576 a × b + c results differ from two roundings. A fused multiply-add would have changed 395,276 of them. The pragma carries this, as a one-off probe shows (`fault_injection.py` on a scratch manifest; not a manifest fault). With `#pragma METAL fp contract(off)` removed and `MTLMathModeSafe` kept, exactly those 395,276 differ, so safe math alone fuses every a × b + c here.
- Denormals are flushed both ways:
  - as inputs: x × 1 = 0, x + 0 = 0, x > 0 is false and x ≠ 0 is false for 2^-149, 2^-140, 2^-127 and the largest denormal;
  - as results: 2^-126 × 2^-k = 0 for k = 1, 10 and 23, and 1.5 × 2^-126 − 2^-126 = 0.
  The CPU keeps all of them. The test asserts this table.
- `uint` to `float` rounds to nearest even, with 0 of 1,048,581 words different from the CPU. float(2^32 − 128) × 2^-32 and float(2^32 − 1) × 2^-32 are both exactly 1.0. So E43's u = 1.0 hazard is real on this GPU. The kernels never convert a whole word.
- exp and log errors, in float ULPs of the exact value:

  | Function | Grid | `precise` | `fast` | Host float |
  |---|---|---|---|---|
  | exp | x from −87 to 88 | 1.448 | 63.79, at x = −86.64 | 0.514 |
  | exp | x from −3 to 3 | 1.491 | 4.24 | 0.513 |
  | log | x from 2^-126 to 2^128 | 2.446 | 3.457 | 0.545 |
  | log | x from 0.5 to 2 | 2.600, max abs 7.86e-8 | 4.000 | 0.560 |

  `precise` is within MSL's 4 ULP on all four grids. It differs from the host's float result at 36 to 45% of the points, which is one reason C and D differ at all. The wide log grid's last point, float(2^128), is infinite and is left out, so that grid has 4,194,304 points, not 4,194,305.
- Compensation: the GPU's compensated sum of 1 and then 2^16 terms of 0.75 × 2^-24 is 1.002929688, the exact value. It equals the CPU's bit for bit. The plain float sum stays at 1.
- Fast math, a one-off probe like the contraction one (`MTLMathModeFast` with fast functions, the pragma kept):
  - ÷ mismatches 299,095 of the 2^20 pairs;
  - the compensated sum collapses to 1, because the Kahan carry is reassociated away;
  - there is still no contraction;
  - GPU-2 at the test's 2^20 paths still passes. On asian252 the ratio is 0.00058 and (D − B) / B is 8.3e-7, against 0.00015 and 2.2e-7 in safe mode.
  GPU-2's 0.01 SE cannot see fast math. The property tests can, and the faults `metal-fast-math` and `metal-compensation-dropped` show it.
- Setup: device 0.0007 s. In the first process, the library compiled in 0.125 s and the pipelines took 0.071 s. In later processes they took 0.0025 s and 0.0016 s, because Metal caches the compiled source. On CI's paravirtual device, compile took 0.24 to 0.65 s and pipelines 0.14 to 0.41 s.

Batch size, on asian252 with one command buffer at a time:
- The batches had no error up to 2^29 paths, a buffer of 3.540 s of GPU time. The run stopped there, as registered, because 3.54 s is over 2 s. Apple's limit was not reached.
- 2^22 paths took 0.0284 s. 2^23 took 0.0565 s, over the 50 ms rule.
- The safe batch is **4,194,304 paths (2^22)** per command buffer, 125 times shorter than the longest buffer that completed.

Philox on the GPU:
- Random123's known answers at 10 and 7 rounds pass (`Gpu.PhiloxKnownAnswersTenAndSevenRounds`).
- The four words equal `pricer::DrawStep`'s on 2^16 (seed, path, step) triples, including paths across 2^32 and seeds with a high word.

The table: N = 2^24 per contract, seed 20261202, span 16, CPU columns on 16 threads. Rel. is the difference over its base column, with that ratio's SE.

| Contract | A | B | C | D | SE_B |
|---|---|---|---|---|---|
| asian12 | 5.631797137 | 5.631795589 | 5.631795172 | 5.631794979 | 2.0103e-3 |
| asian252 | 5.298645396 | 5.298644896 | 5.298646267 | 5.298646065 | 1.8916e-3 |
| barrier156 | 1.053313784 | 1.053314198 | 1.053314074 | 1.053314084 | 7.7632e-4 |

| Contract | (B − A) / A | (C − B) / B | (D − B) / B, GPU-3 | (D − C) / C | Spans with C = D in bits |
|---|---|---|---|---|---|
| asian12 | −2.75e-7 (1.05e-7) | −7.40e-8 (1.4e-10) | **−1.083e-7** (1.4e-10) | −3.43e-8 (1.2e-10) | 431,551 of 1,048,576 |
| asian252 | −9.43e-8 (4.05e-8) | +2.586e-7 (8.8e-10) | **+2.206e-7** (8.8e-10) | −3.80e-8 (1.1e-10) | 548,097 |
| barrier156 | +3.93e-7 (2.96e-7) | −1.175e-7 (7.6e-9) | **−1.081e-7** (7.5e-9) | +9.4e-9 (4.9e-10) | 211,804 |

GPU-2, |D − B| ≤ 0.01 SE_B, passes on all three contracts:

| Contract | \|D − B\| / SE_B at N = 2^24 | At the test's N = 2^20 (seed 20261201) |
|---|---|---|
| asian12 | 0.00030 | 0.00008 |
| asian252 | 0.00062 | 0.00015 |
| barrier156 | 0.00015 | 0.00004 |

|C − B| / SE_B, reported and not judged, is 0.00021, 0.00072 and 0.00016.

GPU-3 is reported, not judged:
- (D − B) / B is −1.08e-7, +2.21e-7 and −1.08e-7.
- NAG's "around 1e-7", and its 4.08e-7, are context. Two contracts sit at about that, and asian252 sits at about twice it.
- asian252's gap comes from the float32 kernel, not from the GPU. C − B is +2.59e-7, while D − C is only −3.8e-8. Its 252 exps per path compound the rounding in x.
- The registered barrier caveat did not bite. barrier156 lands at 1.08e-7, with D − C at 9.4e-9.
- The uniforms' resolution alone, B − A, is of the same order or larger: −2.7e-7, −9.4e-8 and +3.9e-7. It is significant (2.6 and 2.3 SE) on the two Asians, and within 1.3 SE on the barrier.

`GPU-1`, iteration only, run through `measure.sh` at d65b8e5, with nice 0 and load 2.52 at the start. The settings were N = 2^24, seed 20261203, and 2^22 paths per command buffer (4 buffers). Wall-clock medians are over 5 repetitions after 1 warm-up; the minimums are in the evidence. The GPU is timed from allocation to the host reduction, and its setup is reported apart. SIMD CPU is `simd::PriceLanes` on NEON at 2 double lanes, the ceiling on this host, in double on the 52-bit uniforms. CPU float32 is kernel C.

| Contract | GPU | SIMD, 1 thread | SIMD, 16 threads | float32, 1 thread | float32, 16 threads |
|---|---|---|---|---|---|
| asian12 | 0.00665 s | 4.724 s (711×) | 0.401 s (60.3×) | 2.631 s (396×) | 0.215 s (32.3×) |
| asian252 | 0.1149 s | 94.93 s (826×) | 7.428 s (64.6×) | 52.51 s (457×) | 4.258 s (37.0×) |
| barrier156 | 0.1364 s | 130.7 s (959×) | 11.33 s (83.1×) | 141.3 s (1036×) | 10.89 s (79.9×) |

The ratios are each CPU line's wall clock over the GPU's at the same N. Against the float32 line, which has the same precision, the same kernel and the same uniforms, the GPU is 32 to 80 times faster than 16 threads. About half of the asians' ratio against SIMD is precision and lanes: there the SIMD CPU is 1.7 to 1.9 times slower than the float32 line on 16 threads. On the barrier the two CPU lines are within 4%. The summed GPUStartTime-to-GPUEndTime per run was 0.0055 s, 0.1136 s and 0.1380 s; the barrier's sum exceeds its wall clock because the buffers overlap. The reported `GPU-1` comes at stage 11.

CI: GitHub's macOS runners have a Metal device.
- At d65b8e5 (run 42983993850), the probe printed `metal: device Apple Paravirtual device` on all four macOS jobs (clang release and debug, ASan with UBSan, TSan).
- On each of them all 14 `Gpu` tests ran and passed, with 0 skipped: the rounding, denormal and `uint` tables, the exp and log bounds and GPU-2 hold on that device as on this Mac.
- No skip was allowed, so none happened. The conditional skip in `ci.yml` stays for a runner with no device.
- The Linux jobs build with the Metal backend off and run the CPU float32 tests.

Faults: 22 new, all caught at 25db19f on the Mac (338 in the manifest).
- 14 on the Metal checks: `metal-philox-mulhi`, `metal-philox-key-swap`, `metal-uniform-one`, `metal-fast-math`, `metal-contract-on`, `metal-exp-via-exp2`, `metal-log-via-shift`, `metal-compensation-dropped`, `metal-denormal-probe-normal`, `metal-uint-probe-truncates`, `metal-buffer-offset`, `metal-draw-second-word`, `metal-onestep-weight`, and `metal-no-device-skip`, under which every `Gpu` test skips on this Mac and `run_tests.py` refuses the skip.
- 8 on the CPU float32 line: `f32-reduce-float`, `f32-inverse-normal-coefficient`, `f32-symmetry-broken`, `f32-normal-cdf-coefficient`, `f32-cpu-span-stride`, `f32-paired-se`, `f32-double-column-bits` and `f32-params-strike`.
- The 14 Metal faults need a Metal device, so a Linux fault run leaves them out.

### Stage 10: QuantLib comparison

QuantLib v1.44 and Boost 1.92.0, fetched as pinned archives with their hashes and built behind `AAD_PRICER_BUILD_QUANTLIB_COMPARE`, which stays off by default and in CI. QuantLib is linked, never copied: no QuantLib source enters this repo, and the bench uses its public headers only. Every number of this stage is iteration only: nothing is stamped into `results/`, and the reported runs come at stage 11.

#### Pre-registration, stated before any QuantLib number

Committed alone, before any QuantLib engine has priced anything here. What is known at this commit: Phase 1's reading of the v1.44 source (`quantlib-compare/EVIDENCE.md`, E1 to E59; nothing there was compiled or run), and that `ql_library` compiles with this toolchain, with no warning, in a scratch build of 107 s at 6 jobs that priced nothing. The rows are the protocol's (`quantlib-compare/EVIDENCE.md`, "Comparison protocol"), and that file is data.

Common to every row.
- QuantLib side: a fixed evaluation date; `FlatForward` continuous rates and `BlackConstantVol` on one day counter; `BlackScholesMertonProcess` with default arguments, so the exact log step at constant volatility (E7, E8); `PseudoRandom` (Mersenne Twister with Acklam's inverse, E9); `withBrownianBridge(false)`, antithetic off, `withSamples(n)` with no tolerance and no `maxSamples` (E6); a nonzero seed always (E5). Control variate off except on R3.
- Our side: the production pricer, `pricer::Price` on one thread ("ours, scalar"), and the fastest primal, the faster of that and `simd::PriceLanes` at the build's widest lanes on one thread ("ours, fastest"; NEON, 2 double lanes on `mac-m4max`), as in the AAD bench. `PriceLanes` returns `Price`'s bits (stage 5a), so the two have one accuracy verdict; the bench checks that the bits agree and marks the run INVALID if they do not. Rows with no lane form say so and their fastest column is the scalar one. Neither side exits early on a knock-out: QuantLib's barrier pricers run every step (E40), and our kernels carry a killed path to the end with weight zero (`src/pricer/kernels.hpp`).
- The RNG streams cannot match (E9), so every row is a statistical comparison of each engine with the row's reference, never of one engine with the other.

The rows. References are those of `BENCHMARKS.md` section 9 and `references/oracle_values.csv`, with their b.

| Row | Contract | QuantLib | Ours | Reference, b |
|---|---|---|---|---|
| R1 | Geometric Asian call, S 100, K 100, q 0.03, r 0.06, σ 0.20, T 360 days Actual360, 10 fixings at t_i = 0.1 i, no t = 0 fixing | `MCDiscreteGeometricAPEngine` | `kAsianGeometric`, 10 steps | O4, 5.3425606635, b 0 |
| R2 | Arithmetic Asian call, S 100, r 0.0367, σ 0.17801, q 0, T 360 days Actual360, fixings every 30 days from the evaluation date, k = 0..12 (13 points, S0 among them), K 90, 100, 110 | `MCDiscreteArithmeticAPEngine`, control variate off | `kAsianArithmetic`, 12 steps, S0 in the average | AA12K090, AA12K100, AA12K110: 11.904915748797, 4.881961616712, 1.363037952856, b 1e-9 |
| R3 | R2 at K 100 | as R2, `withControlVariate(true)`: QuantLib's geometric control, coefficient 1 (E4) | the same control with coefficient 1: per path, arithmetic payoff − geometric payoff on the same path + the geometric closed form (`src/oracle/geometric_asian.hpp`, 13 points); no lane form | AA12K100, b 1e-9 |
| R4 | R2 at K 100 with n = 50: fixings every 60 days, k = 0..50, on a bench-only `DayCounter` subclass with year fraction days / 3000 (E17); QuantLib unmodified | as R2 | `kAsianArithmetic`, 50 steps, S0 in the average | O9, 4.937202814482, b 1e-9 |
| R5 | Down-and-out call, S0 = K = 100, H 85, 90, 95, 97, 99, σ 0.30, r 0.10, q 0, T 73 days Actual365Fixed, rebate 0, continuous monitoring | `MCBarrierEngine`, `withBias(false)`, `withSteps(50)`: the extremum sampled per step (E11) | `kHard` on the continuous contract, 50 steps: the same crossing law sampled against each step's second uniform, hard indicator | the closed form (`src/oracle/barrier.hpp`), b 0: O7 and CB085 from the file, and H 90, 97, 99 computed in double (Phase 1's E32: 5.9772419956, 3.0595633046, 1.1707930349) |
| R6 | R5 | R5's QuantLib run: QuantLib has no survival-weight mode, so it adds no QuantLib comparison | `kBridge`, 50 steps: the product of bridge survival weights | as R5 |
| R7 | R5 with discrete monitoring at t_i = i T / 50, i = 1..50, knock-out on `<=` | `MCBarrierEngine`, `withBias(true)`, `withSteps(50)` (E13) | `kHard` on the discrete contract, 50 steps | DB085.value, DB090.value, O5, DB097.value, DB099.value: 6.3222401405, 6.0981961431, 4.9067890354, 3.8339777052, 2.3363868958, b 1e-9 |
| R9a | Delta of R2 at K 100 | central bump of the spot quote, h = 0.01, two `NPV()`s on the same seed (common random numbers, E28) | the tape's pathwise delta (`ad::PriceWithGradient`), and on lanes `simd::PriceWithGradientLanes`, which gives the tape's bits | below |
| R9b | Delta of R7 at H 95 | central bump, h = 1.0 (1% of S0), two `NPV()`s on the same seed, `withBias(true)` | two estimators, each its own comparison: the protocol's, the tape on Savine's smoothed barrier with half-width 0.005 S0, the narrowest of stage 2's bias study (`docs/NOTES.md`, "Pricer"); and the production one for this contract, the tape on one-step survival, whose delta is run 2's DB095.delta | O6, 0.7978141916, b 1e-9 |

R9's references and b, by the section 10 rule ("b is the reference or finite-difference truncation allowance"), computed in the bench from `tools/quadrature` before any Monte Carlo runs, and printed:
- R9a has no published reference (E29: "central bump of E29 is derivable"). Δ_ref = (V(S0 + h) − V(S0 − h)) / 2h with h = 0.01, V the arithmetic Asian quadrature at its fine resolution (the one behind AA12K100). b = 2 × 1e-9 / 2h + |CD(2h) − CD(h)|: the quadrature's b carried through the difference, plus three times the leading truncation term as estimated by the two steps. QuantLib's bump has the same h, so its expectation differs from Δ_ref only by the quadrature's error in V, which that b covers. The same b for both engines.
- R9b, QuantLib: its bump estimates CD_h(V) of the discrete contract, not the delta. So b = 1e-9 + |CD_h(V) − O6| + 2 × 1e-9 / 2h, with V the Gauss-Legendre recursion at `kFineGaussLegendre` at S0 ± h: the reference's own b, the bump's truncation computed exactly from the quadrature, and the quadrature's error carried through the difference. Ours: O6's b, 1e-9; the smoothed estimator's bias is not an allowance, it is what the comparison measures.

R8, a recorded non-comparison: a discretely monitored barrier at m = 50 has no exact deterministic engine in QuantLib. `AnalyticBarrierEngine` is continuous (E14), the BGK shift is not in the library (E16) and is an approximation in any case, and `BinomialBarrierEngine` ties monitoring to tree steps (E15). So R8 is reported as impossible, with that reason, and nothing stands in for it.

R10 to R12, labelled, never judged for accuracy:
- R10, speed only. R7's contract at H 95 on one local-vol grid. The grid: σ_loc from this project's Dupire (`svi/local_vol.hpp`) on a synthetic raw SVI surface written in the bench (no market data), at the 51 times t = 0, 0.02, ..., 1 and 111 spots 30, 32, ..., 250, exported once. QuantLib reads it through `FixedLocalVolSurface` (linear in spot per time, linear in local vol in time, flat outside, E21) in the 5-argument `BlackScholesMertonProcess`, `MCBarrierEngine` `withBias(true)`, `withSteps(50)`. Ours reads the same matrix with the same rule in a bench-only model behind the pricer's model interface (`Law(i, x)`: left-point σ, log-Euler), through `pricer::EvaluatePath` and the runner; it has no lane form. Strict for speed. There is no reference for the discrete contract under local vol, so no accuracy.
- R11, speed only. R4's arithmetic Asian, 51 points, on R10's grid and process, control variate off (E24). QuantLib cannot sub-step between fixings (E23), so both run one log-Euler step per fixing, with a shared Euler bias nobody measures. No reference exists, so no accuracy.
- R12, sanity only. A European call on a synthetic raw SVI surface: QuantLib's `SviSmileSection` per slice, `PiecewiseBlackVarianceSurface` and `NoExceptLocalVolSurface` (its own Dupire by bumps, E19), priced by `MCEuropeanEngine` with 50 steps; ours by `pricer::PriceLocalVol` with 50 sub-steps. Each against the Black price at the SVI implied volatility at (K, T), T a slice expiry, reported as a distance in its own SE with no pass mark: the two local volatilities differ by construction (E19, E22, E25), and both carry Euler bias.
- For R10 and R11, each engine's mean and SE are printed beside each other as a check that both price the same contract. That is a sanity line, not an accuracy measure.

Accuracy, `QL-2`. Distance from the row's reference by the section 10 rule, never the distance between engines.
1. |x̄ − ref| ≤ t(1 − α_F / (2K), B − 1) SE + b, with α_F = 0.01 and B = 64. K = 42 comparisons: R1 2, R2 6, R3 2, R4 2, R5 10, R6 5, R7 10, R9a 2, R9b 3. Critical value 3.8975749765 (`CriticalValue(0.01, 42, 64)`).
2. SE ≤ a ceiling fixed before the run. The ceilings come from a pilot after this commit, on its own seed, and are committed before the accuracy run: ceiling = factor × SE_pilot × √(N_pilot / N), rounded up to 3 figures, factor 1.65 against a reference and 3 on QuantLib's two bumps (R9a and R9b), the factors of the plan header and their reasons (`references/comparison_plan.csv`).

The SE.
- Ours: 64 batch means from the runner, as for every row of run 2.
- QuantLib, every row but R5: 64 batches, each its own `NPV()` of N / 64 samples on seed 100 × base + 1 + b, b = 0..63. A bump row runs the two bumped `NPV()`s of a batch on the batch's seed and takes their difference as the batch's value.
- QuantLib, R5: one `NPV()` of N samples on seed 100 × base, with QuantLib's own `errorEstimate()` as the SE. The extremum uniforms come from a Mersenne Twister rebuilt on seed 5 at every `calculate()` (E12), so 64 separate `NPV()`s would share them and their batch means would not be independent. Within one run every path draws fresh uniforms, so the sample SE is valid there. The same critical value is used, which is conservative for a sample SE with about 4 million degrees of freedom.
- The default and tuned columns run the same paths. The tuned column's estimate is checked against the default's, which it must match to rounding (|difference| ≤ 1e-12 × |value| + 1e-15); it is not a second comparison.

N and seeds, all development seeds, never 1729 or 271828.
- Pilot: N_pilot = 2^20, seed 20261111.
- Accuracy run: N = 2^22 per comparison, seed 20261112, through `heavy.sh`.
- Speed: seed 20261113. N = 2^20 paths per timed call on R1 to R9, and 2^18 on R10 to R12, where QuantLib's local-vol steps cost far more (E25). The R10 and R11 sanity lines and R12's distances run at N = 2^18 on seed 20261112.
- ctest smoke checks of the QuantLib rows: seed 20261114.

Speed, `QL-1`, iteration only at this stage, through `measure.sh` on `mac-m4max`, single thread on both sides (E42). One timed call is one `NPV()` for QuantLib, construction of the instrument and engine outside it, and one `Price` or `PriceLanes` call for ours, accumulation included on both sides. One warm-up call, then 3 repetitions; the median is reported with the minimum and maximum. Metrics:
- paths/s = N / t;
- ns per path-step = t / (N × steps), where steps is the row's date count; a bump row counts its two `NPV()`s in t;
- RMSE² × time, from the accuracy run and the timing: RMSE² = max(SE², d²) at the accuracy run's N, d = x̄ − ref, which is the variance when the distance is within one SE and the squared distance otherwise; time = the per-path time × that N. This is the work-normalised error, the same for any N when the estimator is unbiased, so a cheaper but noisier estimator cannot look better than it is (R6 against R5, R9b's bump against the tape).
- Each QuantLib column is reported against ours, scalar and fastest, as a ratio of paths/s and of RMSE² × time.

The two QuantLib columns, fixed now.
- Default: QuantLib's v1.44 configuration defaults (E47), recorded from the cache, `Statistics` (`GenericRiskStatistics`, which stores every sample, E41), no call to `enableExtrapolation()`, and no LTO. It is built with exactly our flags, so it is default QuantLib, not a careless build.
- Tuned: everything a fair user could do without patching QuantLib. `enableExtrapolation()` on every term structure the engine reads (the volatility, risk-free and dividend curves, and the local-volatility surfaces on R10 to R12), which skips the range checks and the `maxTime()` day counting on every step (E38); `IncrementalStatistics` in place of `Statistics` (E41); and link-time optimisation, ThinLTO over QuantLib and the bench in a separate build (`-flto=thin` in `CMAKE_CXX_FLAGS`, so QuantLib inherits it). If a piece does not compile or link with this toolchain, the tuned column goes without it and says so; nothing else replaces it. A third line, tuned without LTO, from the default build, separates LTO's share.
- QuantLib's `QL_REQUIRE` checks cannot be switched off (E48) and stay on: QuantLib without them would need patching, and would no longer be QuantLib.
- Our two columns come from the default build, the project's own release flags, without LTO.

Build, version record and the flag check.
- One configure preset, `quantlib`, the release preset with the option on, and `quantlib-lto` for the tuned column. Flags stay in `CMAKE_CXX_FLAGS` and `CMAKE_CXX_FLAGS_RELEASE`, which QuantLib inherits (E49): `-ffp-contract=off -O3 -DNDEBUG`, static libraries, position-independent code on for both.
- The bench step fails unless a sampled QuantLib translation unit (`ql/pricingengines/barrier/mcbarrierengine.cpp`) and the bench's own translation unit show the same optimisation and architecture flags, read from `compile_commands.json`: every `-O`, `-march`, `-mcpu`, `-mtune`, `-m` ISA flag, `-ffp-contract`, `-ffast-math` and its parts, `-flto` and `-DNDEBUG`. The check has a fault.
- The results header records the QuantLib version and its hex (`QL_VERSION`, `QL_HEX_VERSION` from `ql/version.hpp`), the generated `ql/config.hpp`'s settings, the Boost version from `boost/version.hpp`, every `QL_*` cache value, the compiler, the flags, the link mode (static) and LTO.

The checks this stage adds, each with a fault in `faults/manifest.json` that makes it fail for its named reason: the flag check; R3's control-variate estimator and R9's references; the grid local-volatility model against QuantLib's interpolation rule; the lanes-equal-scalar and default-equals-tuned checks in the bench; and a ctest smoke run of every QuantLib row at N = 2^16 against its reference, in the `quantlib` preset, which catches a wrong engine setting such as `withBias(false)` on R7.

Changes to the protocol, decided now and not after seeing a number:
- The protocol's R = 32 seeds of N each becomes the section 10 rule's B = 64 batches summing to N = 2^22, so QuantLib is judged by the same rule as every oracle row.
- R9b runs the protocol's smoothed estimator and, beside it, one-step survival, the production estimator for this contract since stage 2.
- R3's regression-coefficient variant, which the protocol allows as a separate labelled row, is not run.
- QuantLib's `withSamples(n)` counts samples. With antithetic off, a sample is a path, the same count as ours (E4).

#### Stage 10 results

Iteration only: nothing here is stamped into `results/`. Apple M4 Max, macOS 27.2 (26B123), LLVM 23.1.4, the `quantlib` preset, and `quantlib-lto` for the tuned column. The code is 9180dc3, the faults e62141c and the ceilings 01f467d. The raw files are in the research cache, under `phase3/evidence/stage10/`.

Build and version record.
- QuantLib v1.44 (`QL_HEX_VERSION` 0x014400f0) and Boost 1.92.0 (`BOOST_VERSION` 109200) come from their release archives, pinned by SHA-256 in `cmake/AadpDependencies.cmake`. Both are static libraries.
- `ql/config.hpp`: `QL_FASTER_LAZY_OBJECTS` is defined. Sessions, the thread-safe observer, extra safety checks, high-resolution dates, null-as-functions, `std::shared_ptr` and tracing are undefined. Every `QL_*` cache value, the compiler, the flags, the link mode and LTO are in each output's header.
- The tuned column links with ThinLTO over QuantLib and the bench. Apple's `ar` cannot index LLVM bitcode, and CMake's platform files set `CMAKE_AR` as a normal variable that hides the cache. So `CMakeLists.txt` sets `CMAKE_AR` and `CMAKE_RANLIB` to LLVM's tools when `CMAKE_CXX_FLAGS` has `-flto`. Nothing was dropped from the tuned column: `IncrementalStatistics`, extrapolation on every term structure and ThinLTO all built and linked.
- The flag check passes on both compile databases. QuantLib's `mcbarrierengine.cpp` has `-ffp-contract=off -O3 -DNDEBUG -arch arm64`, plus `-flto=thin` in the LTO build. So do the bench's four units, `pricer.cpp` and `simd_pricer.cpp`.

The pilot ran at 9180dc3: N_pilot = 2^20, seed 20261111, 111 s. Its 42 ceilings were committed in 01f467d, before the accuracy run.

Accuracy, `QL-2`. N = 2^22 per comparison, seed 20261112, at 01f467d, through `heavy.sh`, 819 s.
- **All 42 comparisons pass both conditions** of the section 10 rule, with critical value 3.8975749765, and every SE is under its ceiling.
- Every check held:
  - tuned equals default, with a gap of exactly 0, on all 18 QuantLib comparisons;
  - the lanes give the scalar bits on all 23 of ours that have a lane form;
  - the grid rule equals `FixedLocalVolSurface`, with a largest relative difference of 0 at 10,000 points.
- z is (x̄ − ref) / SE. The verdict also allows b: for R9b's QuantLib bump, b is 0.000510, the truncation of CD(1.0) of the recursion (0.7983237) against O6. R9a's reference is CD(0.01) of the quadrature, 0.5764557354, with b 1.55e-7.

| Case | Reference | QuantLib estimate (SE) | z | ours estimate (SE) | z | verdicts |
|---|---|---|---|---|---|---|
| R1 | 5.342560663 | 5.353618118 (0.00383) | +2.89 | 5.342995366 (0.00356) | +0.12 | pass, pass |
| R2.K090 | 11.90491575 | 11.91253571 (0.00509) | +1.50 | 11.90506555 (0.00436) | +0.03 | pass, pass |
| R2.K100 | 4.881961617 | 4.887607197 (0.00368) | +1.53 | 4.882090916 (0.00309) | +0.04 | pass, pass |
| R2.K110 | 1.363037953 | 1.36444064 (0.00201) | +0.70 | 1.363176811 (0.0019) | +0.07 | pass, pass |
| R3 | 4.881961617 | 4.882228294 (0.000161) | +1.65 | 4.881972016 (0.000139) | +0.07 | pass, pass |
| R4 | 4.937202814 | 4.942235935 (0.00342) | +1.47 | 4.935214824 (0.00329) | -0.60 | pass, pass |
| R5.H085 | 6.307574932 | 6.306541078 (0.00446) | -0.23 | 6.301505442 (0.00436) | -1.39 | pass, pass |
| R5.H090 | 5.977241996 | 5.976650848 (0.00447) | -0.13 | 5.971668072 (0.0044) | -1.27 | pass, pass |
| R5.H095 | 4.39750256 | 4.396665526 (0.00425) | -0.20 | 4.3964999 (0.00431) | -0.23 | pass, pass |
| R5.H097 | 3.059563305 | 3.056216389 (0.00379) | -0.88 | 3.057226106 (0.00409) | -0.57 | pass, pass |
| R5.H099 | 1.170793035 | 1.169145941 (0.00254) | -0.65 | 1.170788188 (0.00265) | -0.00 | pass, pass |
| R6.H085 | 6.307574932 | none (R5's run) |  | 6.301502548 (0.00435) | -1.39 | pass |
| R6.H090 | 5.977241996 | none (R5's run) |  | 5.972091898 (0.00436) | -1.18 | pass |
| R6.H095 | 4.39750256 | none (R5's run) |  | 4.39562019 (0.00432) | -0.44 | pass |
| R6.H097 | 3.059563305 | none (R5's run) |  | 3.058645862 (0.0038) | -0.24 | pass |
| R6.H099 | 1.170793035 | none (R5's run) |  | 1.172495124 (0.00245) | +0.69 | pass |
| R7.H085 | 6.322240141 | 6.327426711 (0.00499) | +1.04 | 6.316027298 (0.00434) | -1.43 | pass, pass |
| R7.H090 | 6.098196143 | 6.104207967 (0.00483) | +1.24 | 6.093088818 (0.00448) | -1.14 | pass, pass |
| R7.H095 | 4.906789035 | 4.910370047 (0.0044) | +0.81 | 4.904065068 (0.00441) | -0.62 | pass, pass |
| R7.H097 | 3.833977705 | 3.835204673 (0.00366) | +0.34 | 3.831927992 (0.00409) | -0.50 | pass, pass |
| R7.H099 | 2.336386896 | 2.339216907 (0.00309) | +0.92 | 2.33656961 (0.00358) | +0.05 | pass, pass |
| R9a | 0.5764557354 | 0.5767583091 (0.000287) | +1.05 | 0.5765086022 (0.000242) | +0.22 | pass, pass |
| R9b | 0.7978141916 | 0.7990566154 (0.00103) | +1.20 | one-step 0.7971675021 (0.000466); smoothed 0.7984335422 (0.00115) | -1.39; +0.54 | pass, pass, pass |

- **The QuantLib rows lean the same way together.** Here every Asian, R7 and R9 QuantLib estimate is above its reference, R1 the furthest at +2.89. In the 2^16 smoke on seed 20261114 they were all below, down to −3.45 on R7.H085.
- Every QuantLib comparison but R5 draws its batches from the same seeds, 100 × base + 1 + b. A row's error is driven mostly by the sum of its stream's leading normals, whatever the dimension, so the rows' errors are strongly correlated.
- Ours share one seed too, and R5 to R7 lean negative together.
- The Bonferroni critical value does not assume independence, so the family-wise rate stays at most 0.01. The 42 z-values are not 42 independent draws, though, and should not be read as a distribution.
- Post-registration diagnostic, which changed nothing:
  - After the smoke and before the pilot, a scratch probe (`build/s10/probe.cpp`, not committed) ran QuantLib's R2.K100 and R7.H085, batched and single-run, on eight unregistered bases, 31000001 to 31000008. That is seeds 3100000100 to 3100000864, at 2^16 per run, and R7.H085 at 2^20 per run on four of them.
  - R2.K100's mean z was +0.23 batched and +0.18 single. The R7.H085 runs at 2^20 average 6.3233, against 6.3222.
  - It found no bias. No protocol, code or seed changed after it. QuantLib's biased path pricer was read as well: it checks every one of the 50 grid points (i = 1..n − 1, n = 51), as DB0xx does.

Speed, `QL-1`, iteration only. Run through `measure.sh` at 01f467d, nice 0, with load 3.81, 1.28 and 1.12 at the starts of the three runs. Single thread on both sides, 1 warm-up and 3 repetitions, medians with the minimum and maximum in the evidence. The maximum was within 1.4% of the minimum in every cell. N = 2^20 on R1 to R9 and 2^18 on R10 to R12, seed 20261113.

The table gives paths/s in millions. "default / scalar" is QuantLib's default column over ours, scalar; "fastest" is the faster of scalar and lanes. RMSE² × time uses the accuracy run's SE and distance at 2^22 and the median time scaled to 2^22. For the two R9b lines, QuantLib's bump is set against each of our estimators.

| Row | ours scalar | ours lanes | QL default | QL tuned, no LTO | QL tuned (ThinLTO) | default / scalar | default / fastest | tuned / scalar | tuned / fastest | RMSE² × time: ours fastest, QL default, QL tuned |
|---|---|---|---|---|---|---|---|---|---|---|
| R1 | 4.528 | 5.800 | 1.032 | 1.132 | 1.140 | 0.228 | 0.178 | 0.252 | 0.197 | 9.143e-06, 0.0004968, 0.0004499 |
| R2.K100 | 2.674 | 3.657 | 1.049 | 1.190 | 1.183 | 0.392 | 0.287 | 0.443 | 0.324 | 1.097e-05, 0.0001275, 0.000113 |
| R3 | 2.716 | none | 1.036 | 1.167 | 1.160 | 0.381 | 0.381 | 0.427 | 0.427 | 2.992e-08, 2.879e-07, 2.571e-07 |
| R4 | 0.693 | 0.921 | 0.253 | 0.288 | 0.286 | 0.366 | 0.275 | 0.413 | 0.31 | 4.936e-05, 0.0004192, 0.0003716 |
| R5.H095 | 0.601 | 0.825 | 0.212 | 0.237 | 0.234 | 0.353 | 0.257 | 0.39 | 0.284 | 9.427e-05, 0.0003565, 0.0003229 |
| R6.H095 | 0.632 | 0.884 | none | none | none |  |  |  |  | 8.869e-05 |
| R7.H095 | 1.019 | 1.247 | 0.252 | 0.287 | 0.284 | 0.247 | 0.202 | 0.278 | 0.228 | 6.538e-05, 0.0003227, 0.0002861 |
| R9a | 1.066 | 1.987 | 0.520 | 0.597 | 0.590 | 0.487 | 0.261 | 0.553 | 0.297 | 1.241e-07, 7.391e-07, 6.513e-07 |
| R9b (one-step) | 0.120 | 0.192 | 0.125 | 0.144 | 0.142 | 1.04 | 0.652 | 1.18 | 0.738 | 9.128e-06, 5.169e-05, 4.566e-05 |
| R9b (smoothed) | 0.234 | 0.444 | 0.125 | 0.144 | 0.142 | 0.535 | 0.282 | 0.605 | 0.319 | 1.256e-05, 5.169e-05, 4.566e-05 |
| R10 | 0.337 | none | 0.111 | 0.112 | 0.113 | 0.331 | 0.331 | 0.337 | 0.337 | none (no reference) |
| R11 | 0.371 | none | 0.114 | 0.115 | 0.116 | 0.308 | 0.308 | 0.313 | 0.313 | none (no reference) |
| R12 | 0.425 | none | 0.045 | 0.046 | 0.046 | 0.107 | 0.107 | 0.109 | 0.109 | none (no reference) |

- **QuantLib's default column runs at 0.23 to 0.49 of our scalar paths/s on R1 to R9a, and at 0.18 to 0.38 of our fastest.** Per path-step it spends 79 to 97 ns, and 160 ns on R9a's two `NPV()`s. Ours spends 20 to 33 ns scalar on R1 to R7, and 78 ns on R9a's tape.
- The tuned column gains 10 to 14% over the default on R1 to R9, and the tuned line without LTO gains 10 to 15%. ThinLTO adds nothing measurable: tuned with and without it differ by −1.2% to +1.2% on every row. One column holds both `IncrementalStatistics` and the extrapolation, so their shares of the gain are not separated.
- **R9b is the one row where QuantLib's paths/s beats ours, scalar.** Its two-`NPV()` bump runs at 1.04 (default) to 1.18 (tuned) times the one-step tape on one scalar thread, and at 0.65 to 0.74 times its lane form. By RMSE² × time, the one-step tape on lanes is still 5.0 to 5.7 times better than the bump. Against the smoothed tape, the bump runs at 0.54 to 0.61 of scalar.
- **RMSE² × time: QuantLib is 3.4 to 54 times ours, fastest.**
  - R1's 54 and R2.K100's 11.6 come from distances of +2.89 and +1.53 SE. Above one SE, the registered max(SE², d²) takes d², which there carries the correlated luck above. With SE² alone the ratios would be 6.5 and 4.9.
  - On R3 the control variate makes both sides' RMSE² × time about 400 times smaller than R2.K100's, and QuantLib stays 8.5 to 9.6 times ours.
- R10 and R11: QuantLib runs at 0.31 to 0.34 of ours, and its three columns are within 2% of one another. The local-volatility lookup dominates there, and `IncrementalStatistics` and extrapolation barely matter.
- R12: QuantLib runs at 0.11 of ours. `NoExceptLocalVolSurface` takes Dupire by bumping the Black surface at every step (E19), while ours reads the SVI slices in closed form.

Sanity lines, at N = 2^18 on seed 20261112, not judged.
- R10: ours 4.1420 ± 0.0120 against QuantLib's 4.1710 ± 0.0112. R11: 5.6100 ± 0.0153 against 5.6521 ± 0.0138. The differences are 1.8 and 2.0 combined SE, in the same direction as the QuantLib rows above. On the smoke seed they were −0.4 and −0.3.
- R12, against the Black price at the SVI implied volatility, 13.8627: ours 13.9003 ± 0.0305 (+1.23 SE), QuantLib 13.9638 ± 0.0284 (+3.56 SE). There is no pass mark: as registered, the two local volatilities differ by construction and both carry Euler bias.

R8 is not a comparison: QuantLib has no exact deterministic engine for a barrier monitored at m = 50 (E14 to E16). R10 and R11 are speed only and R12 sanity only, as registered.

The LTO build's smoke (2^16, seed 20261114) gives the default build's estimates and SEs on every line. That is QuantLib's default and tuned columns, and ours, the same in every printed digit, so ThinLTO changed no result.

debian, `linux-zen1`. The `quantlib` preset was built at 9180dc3 through `heavy.sh` at `-j2`, pinned to cpus 6 and 7, with LLVM 23.1.4 from the toolchain.
- Configure took 50 s. QuantLib, Boost's headers and the bench took 2,094 s.
- The flag check passes there: QuantLib's unit and the bench's all have `-ffp-contract=off -O3 -DNDEBUG`.
- `aadp_qlcompare_test` passes 12 of 12.
- The smoke (2^16, seed 20261114, SSE2 at 2 double lanes) passes all 42 comparisons and every check.
- Every accuracy line, on both engines, equals the Mac's in all ten printed digits. Among the sanity lines only R12's QuantLib estimate differs, by 1e-7 (13.8618331 against 13.861833), which comes from QuantLib's own SVI and Dupire code.
- Between steps, the run waited about 9.5 hours in `heavy.sh` on another project's measurements. The build itself ran on cpus 6 and 7 while that project measured on cpu 4, as `taskset` keeps them apart there.

Faults: 12 new, all caught at 9180dc3 on the Mac (350 in the manifest).
- 6 on `aadp_qlcompare_test`, in the release preset:
  - `ql-r6-quantlib-comparison`: R6 is given a QuantLib comparison;
  - `ql-r7-continuous`: R7 is continuous on our side;
  - `ql-cv-control-dropped`: R3's control is dropped from the path;
  - `ql-r9b-bump-allowance`: R9b's allowance lacks the bump's truncation;
  - `ql-grid-node-held`: the grid rule holds the spot at a grid time;
  - `ql-ceiling-no-sqrt`: a ceiling is scaled without the square root.
- 1 on the flag check's self-test: `ql-flag-check-ignores-o`.
- 5 in the `quantlib` preset:
  - `ql-bench-flags-differ`: the bench is built at `-O1`, and the build stops at the flag check with `flag mismatch bench/quantlib/main.cpp`;
  - `ql-r7-unbiased`: without `withBias(true)`, the smoke fails R7.H099 at z = −66;
  - `ql-tuned-differs`: tuned-equals-default;
  - `ql-lanes-seed`: lanes-equal-scalar;
  - `ql-grid-time-variance`: the grid check against `FixedLocalVolSurface`.

The 5 `quantlib`-preset faults stopped being judged after their first run, because the flag check looked for QuantLib in the wrong place.
- The check found its sampled QuantLib unit by the suffix `_deps/quantlib-src/ql/pricingengines/barrier/mcbarrierengine.cpp`. That is where FetchContent puts QuantLib by default, in `<build>/_deps/`. The fault runner keeps one source cache for every preset, keyed by a hash of `cmake/AadpDependencies.cmake`, and configures with `FETCHCONTENT_SOURCE_DIR_QUANTLIB` pointing into it, at `build/fault/_deps/<hash>/quantlib-src/`. There, the suffix does not match.
- So the unmutated `aadp_bench_quantlib` stopped at the flag check with `check_quantlib_flags: no QuantLib unit ...` (exit 2). All five faults came out `baseline-failed`: `ql-bench-flags-differ`, `ql-r7-unbiased`, `ql-tuned-differs`, `ql-lanes-seed` and `ql-grid-time-variance`. An `--only` rerun of the five at 22223a5 gave the same result, and the copy's `CMakeCache.txt` and `compile_commands.json` put the unit under the cache.
- They were caught at 9180dc3 because that run configured the `quantlib` copy before the cache held QuantLib's sources, so FetchContent fetched into the build tree. The runner then copied those sources into the cache: its `quantlib-src` is dated 35 s after that run's start. Every later run read them from there.
- The fix: CMake passes `quantlib_SOURCE_DIR`, wherever FetchContent put the sources, to the check as a second argument, both in the bench's build and in the ctest. The check looks up `ql/pricingengines/barrier/mcbarrierengine.cpp` there by its full resolved path, not by a guessed suffix. With no directory, or no unit at that path, it still exits 2. The flags it compares, and how it compares them, are unchanged.
- The self-test's QuantLib now sits outside the build tree, as under the fault runner, for every existing case. It gains three cases: QuantLib in the build tree, where FetchContent puts it by default; a unit named relative to its directory; and a unit only under another directory, which must exit 2.
- In a normal `quantlib` build, with QuantLib in `build/quantlib/_deps/quantlib-src`, the check passes, its ctest passes, and a copy of that compile database with `-O1` added to `bench/quantlib/main.cpp` gives `flag mismatch bench/quantlib/main.cpp: ... -O1`, exit 1.

CI: run 43107078217, at 01f467d, passed all 22 jobs, as the GitHub API lists them. This line said 23 until the commit that fixed the flag check.
- The new library, its 12 tests and the flag check's self-test ran in every build job. The release jobs went from 720 to 733 tests on Linux and from 732 to 745 on macOS.
- The Linux jobs keep their one allowed, named skip from before. macOS had none.
- CI builds no QuantLib: the option stays off there, and the `quantlib` presets are not in its lists, as registered.

### Generated README headline and RESULTS.md (e67aa02)

- `aadp_results_table --docs <root> --write|--check` (`src/results/docs.cpp`) generates the README's headline block, between its markers, and `RESULTS.md`. It reads `results/headline.csv`, the target headings of `BENCHMARKS.md`, every file in `results/<host>/` and the measured oracle runs, and holds no number itself.
- `scripts/check.sh` gained the `results-docs` step, its 11th of 14, before `data-verify`. It builds `aadp_results_table` in the release preset and runs `--docs . --check`, which fails unless both files match a fresh generation byte for byte.
- CI's two release jobs, `linux-clang-release` and `macos-clang-release`, run the same check after their build.
- 4 faults, which bring the manifest to 354 (62 oracle, 264 check, 28 fuzz):
  - `check-results-docs-readme`: a number in the README's headline is edited by hand;
  - `check-results-docs-results-md`: a mean batch time in `RESULTS.md` is edited by hand;
  - `check-results-docs-host-mixing`: a file under `results/mac-m4max/` says it came from `linux-zen1`;
  - `fuzz-results-docs`: the measured-run row decoder reads past a short row, which `fuzz_results_docs` finds under ASan.
- 22223a5 corrected the README's fault count, 316 to 354, and run 43107078217's job count, 23 to 22.

The full check at 22223a5 failed on the flag check's path bug (above, under "Stage 10 results").
- `scripts/check.sh` through `heavy.sh`: 13 of 14 steps passed, and fault-injection failed with `caught=349 not-caught=0 not-applied=0 baseline-failed=5`.
- Its fault report names e67aa02 as the commit under test. 22223a5 differs from it only in two lines of `README.md`.
- The five were the `quantlib`-preset faults. No fault was missed.

The full check at 822acaa, after the fix, passed. It finished unattended: the subagent that started it, `scripts/check.sh` through `heavy.sh`, was stopped by a usage limit first, and this result is taken from the run's complete summary and fault report.
- 14 of 14 steps passed, none skipped. Tests: release 764 of 764, and debug, asan-ubsan and tsan 763 of 763 each, with 0 failed and 0 skipped in every preset.
- Fault injection: `caught=354 not-caught=0 not-applied=0 baseline-failed=0`. Its report names 822acaa as the commit under test, and the five `quantlib`-preset faults are caught again.
- The steps took 8276 s in all, 5612 s of it fault injection.

### Repo layout, 2026-11-12

The repo is public, and these files are what a reader opens first.
- The agent-instruction file at the root is no longer tracked. It stays on disk, and `.gitignore` keeps it out. History is not rewritten, so older commits still hold it.
- `NOTES.md` and `TASKS.md` moved to `docs/NOTES.md` and `docs/TASKS.md`. Every reference to them moved too. In `references/`, `results/headline.csv` and `bench/quantlib/ceilings.csv`, only the path in one comment line per file changed; no data row did.
- `scripts/scan_repo.py` lost its pinned-digest exception for that file. Any agent notes file that git tracks now fails the scan, at any path. Its test is `tests/data/scan_repo_test.py` (ctest `data_repo_scan_script`), and fault `check-repo-scan-notes-file` drops the root file's name from the scan's list. The manifest holds 355 faults.
- Commits: eddbaa4 (the layout), then 009605c (the licence). `LICENSE` is the MIT text. An audit of every committed file against the recorded terms added the chain's attribution where a derived file lacked it, as a comment or header line only: `BENCHMARKS.md` section 11, this file, `docs/TASKS.md`, two real-data tests, `tests/data/decimal_test.cpp` and `tools/chain_profile/main.cpp`. The README's Licence section names the derived files.
- Open: `aadp_run_oracles run --data` prints the surface rows without the attribution. Its raw output is never committed, and the curated run 2 file carries it.

The full check at 009605c passed.
- `scripts/check.sh` through `heavy.sh`: 14 of 14 steps passed, none skipped. Tests: release 765 of 765, and debug, asan-ubsan and tsan 764 of 764 each, with 0 failed and 0 skipped in every preset. Fuzz: 20 targets, 60 s each, 0 failed.
- Fault injection: `caught=355 not-caught=0 not-applied=0 baseline-failed=0`, with 009605c as the commit under test.
- The steps took 8367 s in all, 5613 s of it fault injection.
- The commit that adds these lines changes only this file and had no full run of its own. Format and the repo scan ran on it.

### Stage 11: one statement order on every compiler

Iteration only: nothing here is a stamped result. Raw output is in the research cache, `evidence/stage11-s1/`. Commit: f131c23 (the code, the test, the faults and the Linux file).

How it was found.
- `references/run2_fingerprint_linux-zen1.txt` was written from the Linux host's GCC pre-commit build ("Stage 8a results"), and `aadp_run2_fingerprint` had only ever been run against it under g++. The open item in `docs/TASKS.md`, "Iterate", asked for the clang `release` build there.
- At 92cc148, clang 23.1.4 `release` on the Linux host, with the captures linked, failed it: 45 of the 147 data lines differ, exactly the surface rows (`P3.AS`, `P3.DB`, `P3.CB`). The other 102 match bit for bit, and 1 and 8 threads agree.

The cause.
- C++ leaves unspecified the order in which the two operands of an overloaded operator, and the arguments of a call, are evaluated. clang evaluates them left to right; g++ 14.2.0 right to left.
- The demo: `x * x + s * s` on a type whose `operator*` prints, built with `-std=c++23 -O3 -ffp-contract=off` on the Linux host, prints the product 4 first under clang and 9 first under g++.
- On the tape, each operation with an active operand records a statement when it is evaluated. So an expression with two operands that record, such as the surface's `beta * lo.w + alpha * hi.w`, records them in one order under clang and in the other under g++.
- The values do not change. The reverse sweep adds each statement's contributions to its operands' adjoints in statement order, so the adjoints can round differently.

What was and was not bit-identical between the two builds of 92cc148 on the Linux host (a scratch probe linked against each build's libraries):
- Identical: the parsed chain, the fitted SVI parameters, the plain prices, the 90 bumped prices of the central differences and the tape's primal values.
- Different, in the probe: the tape's gradients, in 43 of the 45 surface rows' means and 2,619 of their 2,880 batch means.
- A statement trace in a scratch copy found the same statements, values and operand counts, in a different order.
- A hash of every recorded statement (value, operand indices and partials, in order) over the whole fingerprint at 1 thread, 127,375,581 statements, differs between the compilers from its first checkpoint, at 65,536 statements. So the GBM rows were recorded in a different order too, and their bits came out the same anyway.

The fix.
- Wherever code instantiated on a tape type had two or more operands or arguments that each record a statement in one full expression, they are now named temporaries, in the order clang evaluated them. clang records what it did before, and g++ now records the same.
- Full expressions rewritten, by file:
  - `src/pricer/conditional_kernels.hpp`, 9: `Mass`, the share-measure bounds and the call in `Corridor`, the arguments in `AsianGeometric`, both legs in `AsianArithmetic`, the return of `OutOneStep`, and in `OutOneStepBridge` the loop's weight, the reflection exponent and the return.
  - `src/svi/surface.hpp`, 3: `w`, `w1` and `w2` between two slices.
  - `src/svi/calibration_adjoint.cpp`, 2: the slice's `w` and the model price, on the second-order tape.
  - `src/pricer/kernels.hpp`, 1: `Finish`, which carries the comment the others point to.
  - `src/simd/lane_kernels.hpp`, 1: the lane `Finish`.
  - `src/pricer/model.hpp`, 1: `GbmModel`'s drift per step, now `StepDrift`.
  - `src/pricer/local_vol_model.hpp`, 1: the drift in `Law`.
  - `src/svi/local_vol.hpp`, 1: the Durrleman `g`.
  - `bench/lax_wendroff.hpp`, 1: the flux.
- Left as they are, with the reason:
  - `src/svi/raw_svi.hpp` (`Evaluate`, `TotalVariance`, the Durrleman `g`, `DPlus`, `DMinus`) and `src/svi/essvi.hpp`: instantiated on `double` and `Dual` only, never on a tape. The surface evaluates its slices through `EvaluateSlice`.
  - Every other expression on a tape type: at most one operand per operator records, or the operands are braced or designated initializers or mem-initializers, which C++ evaluates in order.
  - `src/oracle/` and the tools: no tape.
  - The tests' own kernels (`tests/simd/diverging_kernel.hpp`, `tests/ad/random_program.hpp`): each compares two runs within one compiler.
- The sweep's check: the statement hash, made safe for several threads (a hash per statement, its index on its tape included, summed), over seven test binaries that record on all three tapes (scalar, second-order, lane): `aadp_ad_statement_order_test`, `aadp_ad_dual_agreement_test`, `aadp_ad_second_order_test`, `aadp_ad_local_vol_gradient_test`, `aadp_simd_lane_gradient_test`, `aadp_svi_calibration_adjoint_test` and `aadp_aad_bench_unit`. Each ran twice under each compiler on the Linux host. The two runs agree, and clang and g++ agree in all seven.

The prediction and its outcome.
- Registered before the Linux runs (2026-11-13T00:36Z): with the fix, the clang `release` build and the g++ `release-gcc` build on the Linux host each give exactly clang's lines at 92cc148, that is, the file's 102 lines that were not surface rows and clang's 45 surface lines. The sha256 of those 147 data lines, each ending in a newline, is `139276448413734809ea87a74522abd7420d1d9355d2393f0c045a25ebf2b858`.
- Outcome: met. Both builds of the fixed tree give 147 data lines with that sha256. The statement hash over the fingerprint is the same under both, and the same as clang's at 92cc148.
- On the Mac, `references/run2_fingerprint_mac-m4max.txt` is unchanged and `aadp_run2_fingerprint` passes.

The change in the Linux file: its 45 surface lines take clang's values, and its comment says the lines hold for both compilers.
- 31 rows have a different estimate, by at most 2.8e-14 relatively (`P3.CB.d05.m`).
- All 45 have a different SE, by at most 6.2e-9 relatively (`P3.AS.d33.m`).
- No reference value changes. The 102 other lines do not change.

Two plain statements.
- Run 2 was measured on the Mac under clang, and its fingerprint did not change.
- From bf484f3, the commit that added the Linux file, up to this fix, the clang `release` build on the Linux host fails `aadp_run2_fingerprint`. It was run at bf484f3 and at 92cc148, and failed on the 45 surface rows at both; the sites predate bf484f3.

The check that keeps it fixed.
- `tests/ad/statement_order_test.cpp` (ctest `aadp_ad_statement_order_test`, labels `ad svi fast`) records eight paths on fixed inputs: two GBM paths through the production kernels, a local-volatility path through the sub-stepped kernels and the SVI surface, and five paths through the conditional kernels. It sweeps each tape forward and back absolutely, and hashes every statement's tangent and adjoint in recording order. The hashes, statement counts and operand counts are pinned, one constant for every host and compiler.
- On the Linux host against 92cc148's code, the test passes under clang and fails under g++: all eight hashes differ, and all three tests fail. On the fixed code it passes under both, and on the Mac.
- Three faults reverse two named temporaries, the order g++ had chosen, and are caught under clang on the Mac: `order-surface-w-swapped` (`LocalVolPathIsPinned`), `order-finish-swapped` (`GbmPathsArePinned`) and `order-conditional-swapped` (`ConditionalPathsArePinned`).
- Five faults whose edits named the rewritten expressions now make the same bug on the named form: `ad-second-geometric-sd`, `lv-surface-alpha-swapped`, `lv-vw4-displayed-sign`, `lv-drift-without-half-variance` and `qs-put-as-call`. Run with `--only` and `--manifest` through `heavy.sh` on an export of the uncommitted tree, these eight and the 35 other faults whose edits touch the changed files: 43 caught. The two that read the chain failed their baselines on the first run, because the export had no data where the runner looks for it, and were caught on a rerun with it. That tree differs from f131c23 only in the test, whose one later change was clang-tidy's (designated initializers, an unsigned shift, an include pragma); the three order faults ran again on the final test and were caught.
- The manifest holds 360 faults: 62 oracle, 270 check, 28 fuzz.

Gates, on the tree f131c23 commits: on the Mac, `release` 769 of 769 and `fuzz` 20 of 20, 0 failed and 0 skipped, the format check, the repository scan and clang-tidy; on the Linux host, the GCC pre-commit (757 of 757), clang-tidy on libc++ and on libstdc++ (the latter on f131c23's code beside the next commit's edits to `README.md`, `BENCHMARKS.md` and `docs/TASKS.md`), and the clang `release` build with `Run2Fingerprint` and `StatementOrder` passing, 4 of 4.

CI found an open fault outside this stage's code. Run 43357450022, at 271c28c, passed 21 of its 22 jobs; `fuzz-smoke` failed on `aadp_fuzz_svi_fit` 21 s into its 30 s, with `invariant failed: SVI-11 finite: std::isfinite(ImpliedVolRmse(r.model_vol, r.market_vol))` (`fuzz/fuzz_svi_fit.cpp`, line 160).
- In that run the three `StatementOrder` tests passed under g++ (`linux-gcc-release`) and clang (`linux-clang-release`, `macos-clang-release`).
- That target builds from `fuzz/fuzz_svi_fit.cpp`, `src/svi/fit.*`, `arbitrage.*`, `implied_vol.*` and `raw_svi.hpp`, none of which this stage touched. libFuzzer's mutations are random, and the smoke at 92cc148 passed.
- On the Mac's fuzz build, from a copy of the seed corpus, 90 s per libFuzzer seed: seeds 1 and 2 passed, and seed 3 found the same failure. The input is in the research cache, `evidence/stage11-s1/fuzz-svi-fit/`, and not committed.
- A fit that meets the target's other properties so far reports an implied-vol RMSE that is not finite. It is not fixed here, and is listed under `docs/TASKS.md`, "Iterate".

`PerfCounters.EachRegionReportsItsOwnTimes` under load (`tests/bench/platform_test.cpp`, Linux only). Iteration only; raw output in the research cache, `evidence/stage11-s1/piece1/`.
- The cause: a per-thread counter (pid 0, any CPU) is enabled only while its thread is on a CPU, so its enabled time follows the thread's CPU time, not the wall clock. The test spun 30 ms of `steady_clock` and required at least 29.7 ms enabled, so with another runnable task on its core it saw about half. Stage 6a's pre-commit failure (29.11 ms) and stage 5b's two came while other jobs loaded the host, which fits this cause; their runs were not traced.
- The fix: the spin runs until the thread's CPU time (`CLOCK_THREAD_CPUTIME_ID`) has advanced 30 ms, however often the thread is preempted. That clock is a system call, so it is read only between 50 µs bursts of spinning on `steady_clock`, which keeps the region mostly user cycles. The checks are unchanged: the first region's enabled time at least 29.7 ms, 99% of 30 ms, and the second region's enabled time and cycles each below half the first's. No check was added, so no fault; `bench-darwin-stuck-counter`, the one fault whose command runs `PerfCounters.*`, is still caught on the Mac (`--only`). CI's named skip for this test, which skips itself where the runner's kernel settings give no counters, is unchanged.
- Pass counts on `linux-zen1`, clang 23.1.4 `release`, 20 runs each, through `heavy.sh`. Shared: `taskset -c 6,7` while the per-commit verification job loaded cpus 6 and 7 (79% to 91% busy over each set). Hog: `taskset -c 2` with a busy loop pinned to cpu 2.

| Test | Shared, cpus 6 and 7 | Hog on cpu 2 |
|---|---|---|
| New (CPU-time spin) | 20 of 20 | 20 of 20 |
| Old (wall-time spin) | 9 of 20 | 0 of 20 |

- The old test's failures saw 14.8 to 29.6 ms enabled when shared and 13.9 to 17.0 ms beside the hog, against the 29.7 ms floor.

A central fast path in the scalar inverse normal (`src/math/inverse_normal.hpp`). Raw output in the research cache, `evidence/stage11-s1/piece2/`.
- What was recorded about the select form: it exists so that a SIMD lane runs the scalar's operations in the scalar's order and gets its bits (a43f40c, and "SIMD" above). Nothing recorded asks the scalar function itself to be branch-free, and its AVX2 build already had 17 conditional branches ("Stage 5c", B15). So one branch does not defeat a recorded reason.
- The change: when |p − 1/2| ≤ 0.425, the select form's own test and so 85% of uniform draws, `math::inverse_normal` evaluates the central rational alone, with the select form's operations in its order, and skips the `log`, the `sqrt` and the tail selects. Every other p, NaN included, runs the select form, now `inverse_normal_detail::SelectForm`, which the lane kernels copy and which did not change. The lane kernels did not change.
- The same bits for every input: `MathInverseNormal.FastPathGivesTheSelectFormsBits` compares the two on 2,299,801 inputs, 0 differing in any bit: the central region on a grid and at random, 4096 doubles on each side of 1/2, of both central edges (1/2 ∓ 0.425 as doubles; the decimal 0.925 is the upper one, the decimal 0.075 one double below the lower one), of exp(−25) and its complement, of the smallest subnormal, of 2^−1022 and of the largest double below 1; 64 points in every binade of p and of 1 − p down to the smallest subnormal; and 0, −0, 1, negatives, values above 1, both infinities and five NaNs.
- Faults, all caught with `--only`: `oracle-math-fast-path-edge` (the fast path tests p against the decimals 0.075 and 0.925, which sends p = 0.925 to the central rational, one ULP off the select form's tail value) and `oracle-math-select-form-central` (the select form's central ratio divides first), both on the new test; `oracle-math-central-identity` now makes its bug in the fast path, and the central region's test against the reference still catches it. `oracle-math-inverse-normal`, `oracle-math-inverse-normal-edges`, `simd-inverse-normal-order` and `fuzz-simd-kernels` are still caught. The manifest holds 362 faults: 64 oracle, 270 check, 28 fuzz.
- The tape records what it recorded before: `InverseNormal` on a `Real` records one statement from the value and its partial, and both are the same bits. `aadp_run2_fingerprint`, `aadp_ad_statement_order_test`, the lane bit-identity tests and the inverse normal's host-independence hash pass unchanged on the Mac, and on the Linux host under clang `release` and g++ `release-gcc`. No reference file, seed, tolerance or ceiling changed.

Before and after, iteration only, scalar code, the stage 11 driver of `PERF.md` linked against each build: the inverse normal over 1,032,192 Philox-grid uniforms, and the `mc-asian252` workload at 1/10 scale through `bench/aad_workloads` as `aadp_bench_aad` runs it. Three alternating rounds; ranges over the rounds.

`linux-zen1`, `release-avx2` (x86-64-v3, AVX2, scalar code), clang 23.1.4, `cycles:u` of the timed loop only (`perf stat --control`), through `heavy.sh taskset -c 4`, governor schedutil:

| Kernel | Before | After | Change |
|---|---|---|---|
| Inverse normal, cycles per value | 121.7 to 121.8 | 58.9 | −52% |
| Inverse normal, instructions per value | 163.6 | 64.2 | |
| Inverse normal, branch misses per value | 0.655 | 0.303 | |
| `mc-asian252` price, cycles per path-step | 206.2 to 207.4 | 161.6 to 162.1 | −22% |
| `mc-asian252` hand adjoint, cycles per path-step | 206.3 to 208.9 | 163.7 to 164.6 | −21% |
| `mc-asian252` tape, cycles per path-step | 415.0 to 416.9 | 368.3 to 370.5 | −11% |

`mac-m4max`, macOS, `release` (AArch64, scalar code), clang 23.1.4, wall time per unit, the median of each run's repetitions, each run under `measure.sh`, unpinned:

| Kernel | Before | After | Change |
|---|---|---|---|
| Inverse normal, ns per value | 13.89 to 13.90 | 5.83 | −58% |
| `mc-asian252` price, ns per path-step | 29.54 to 29.61 | 17.12 to 17.15 | −42% |
| `mc-asian252` hand adjoint, ns per path-step | 30.02 to 30.04 | 17.25 to 17.28 | −43% |
| `mc-asian252` tape, ns per path-step | 78.85 to 78.87 | 64.14 to 64.22 | −19% |

- This makes the tape's cost against the hand adjoint worse: both lose the same inverse-normal time per draw, which is a larger share of the hand adjoint. The medians over the rounds give tape over hand 1.99 before and 2.25 after on `linux-zen1`, and 2.63 before and 3.72 after on `mac-m4max`. The same holds for tape over price.
- The scalar baseline of every lane speedup that draws normals now runs the fast path, while the lanes still take every lane's `log` and `sqrt`. So a future run of `SIMD-1` or the lane variants will show smaller speedups for the inverse normal and the pricers than the stage 5c figures; the lanes did not get slower.

CI after the order fix: run 43363186821 on `jb-build` at f6634c7 passed 22 of 22 jobs, and run 43367198277 on `main` at f6634c7 passed 22 of 22.

The `aadp_fuzz_svi_fit` finding, fixed. Raw output is in the research cache: the finding in `evidence/stage11-s1/fuzz-svi-fit/`, the fix's runs in `evidence/stage11-s2/`.
- The finding: CI run 43357450022 at 271c28c, job `fuzz-smoke`, target `aadp_fuzz_svi_fit`, property "SVI-11 finite" (`fuzz/fuzz_svi_fit.cpp`, line 160), reproduced on the Mac at libFuzzer seed 3 (above).
- The input: F = 100, D = 1, and T = 1.3e-309, a subnormal, with 12 quotes. Every quote inverts, and the fit works in total variance w = σ²T, where nothing is extreme. But every vol, σ = sqrt(w / T), lies between 3.0e152 and 1.2e154: finite, as the target requires. The model vols miss the market vols by up to 9.1e153, whose square is 8.3e307, and the 12 squared errors sum past the largest double, 1.8e308. So `ImpliedVolRmse` returned +inf. The RMSE itself is 5.0e155 vol points.
- So the fit was not wrong. Its parameters, Φ, model prices and per-quote vols are finite and meet every other property of the target. Only the reported SVI-11 overflowed, in its intermediate sum of squares. A vol that the fit reports finite is a square root of a double, so below 1.35e154, and 100 times an RMSE that size is finite: the true value always fits.
- The fix, in `ImpliedVolRmse` (`src/svi/fit.cpp`): the sum is taken as before, and returned as before whenever it is finite. Only when it is not, and every error is finite, are the errors scaled by 2^−e, e the binary exponent of the largest, then squared and summed, and the root scaled back. Scaling by a power of two is exact. An error that is not finite still gives +inf or NaN, as before. No sum that fits is computed differently, so no result that was finite can change.
- The tests, in `tests/svi/fit_test.cpp`, with their tolerances set before their first run. `FitMetrics.ImpliedVolRmseWhereTheSquaredErrorsOverflow`: four errors of 2e154 give 2e156 vol points to 4 ULPs; errors 3e154 and −4e154 give 100 sqrt(12.5) × 1e154 to 1e-14 relatively, a few roundings of 2^−53 each; an infinite vol gives +inf and a NaN gives NaN. `FitMetrics.ImpliedVolRmseFiniteAtATinyExpiry`, a hand-made slice: F = 100, D = 1, T = 1e-308, 10 out-of-the-money strikes 100 e^(0.1 (i − 4.5)), total variance alternating 0.01 and 1.5, noise 1, the default fit. The fit stays nearly flat, about 6e153 from every market vol, and the RMSE must be finite and match the same RMSE taken in units of sqrt(w) and divided by sqrt(T), to 1e-12 relatively. Both tests failed on the unfixed code, with +inf, and pass with the fix.
- The same slice in the target's layout is the hand-made seed `fuzz/fuzz_svi_fit_seeds/regression_tiny_expiry_rmse`. On the unfixed code the target's smaller search also gives an infinite RMSE on it, so every fuzz run now starts with the case. CI's input stays in the research cache.
- Faults, both caught with `--only` through `heavy.sh` on an export of the uncommitted tree: `svi-fit-rmse-overflow` (only a NaN sum is rescaled, so an overflowing one reports +inf; `ImpliedVolRmseFiniteAtATinyExpiry`) and `svi-fit-rmse-scale-back` (the rescaled RMSE is not scaled back; `ImpliedVolRmseWhereTheSquaredErrorsOverflow`). The manifest holds 364 faults: 64 oracle, 272 check, 28 fuzz.
- Results on the real chain did not move. `aadp_svi_fit` on the settlements of 2024-11-15, Mac `release`, gives byte-identical output before and after the fix (115 lines, its three SVI-11 lines among them; sha256 `28979bc68a2f7f3a87f10dc221f0169df68afc89b7680e19cc917eec398f8007`). `aadp_run2_fingerprint` passes on the Mac, and on the Linux host under clang `release` and g++ `release-gcc`.
- The commit is 6a21a31. The two faults ran on a tree whose test file differed from 6a21a31's only in where clang-format breaks one statement.
- After the fix, the target ran for 600 s on the Mac's fuzz build, through `heavy.sh`, for each of libFuzzer seeds 1, 2 and 3, each from a fresh copy of the seed corpus (12 files): 248,404, 260,531 and 231,215 runs, and no property failed. CI's input from run 43357450022 passes on the fixed build.

CI on these commits: run 43378416021 on `jb-build` at 5d2e385 passed 22 of 22, and `main` was moved to 5d2e385. Run 43382519971 on `main` at 5d2e385 and run 43383070342 on `jb-build` at 6a21a31 each failed `fuzz-smoke`, on a different target, `aadp_fuzz_plan`; each passed its other 21 jobs, and `aadp_fuzz_svi_fit` passed in both. So `main` was not moved to 6a21a31.

The `aadp_fuzz_plan` finding, fixed. Raw output is in the research cache, `evidence/stage11-s2/`.
- The finding: in both runs the target trapped at `fuzz/fuzz_plan.cpp`, line 47, `if (!instrument) __builtin_trap();`, after libFuzzer had inserted the text `knock-in-parity:continuous` from a comparison it traced. The two runs' inputs were not kept (CI keeps no fuzz artifact), and a hand-made input reproduces the trap at the same line.
- The cause is in the fuzz target, not in the decoder. Stage 8 (01dbe41) let a run row name a knock-in by parity (`knock-in-parity:discrete|continuous`) or a surface contract (`svi-surface:<product>`, whose Greek is `d_<p>_<dNN>`), and the decoder and the runner resolve a row through `ResolveInstrument`, as the committed plan's 57 stage 8 rows need. The target still decoded each run row with `DecodeInstrument`, which knows the GBM kinds only, and trapped on any accepted plan with such a row. Its seeds had none, so only a mutation that produced one could find it.
- The fix: the target resolves each row as the runner does. For every row that is not awaiting, it requires N > 0 and a multiple of B, and that the instrument resolves. A surface row must have a surface Greek and a positive finite-difference step. A row that is not a surface row must be a run row. For a GBM or knock-in run row it keeps every check it had (the ceiling, b, the spot, the steps, one path priced, the Greek and the step), and adds that the contract is a knock-in exactly when the row names one. Unresolved rows were skipped before, and are checked now. Nothing the target checked before is dropped, and no input is skipped.
- The test: the hand-made seed `fuzz/fuzz_plan_seeds/valid_stage8_rows.csv`, a plan with a GBM row, a knock-in row, a surface run row and an unresolved surface row, all synthetic. The unfixed target traps on it at line 47, so the `aadp_fuzz_plan` ctest, which replays the seeds, fails before the fix and passes after. The committed plan, `references/comparison_plan.csv`, also passes the fixed target.
- The fault: `fuzz-plan-knock-in-lost` (the resolver drops the knock-in mark, so the runner would price the knock-out contract). It is caught with `--only`, and `fuzz-plan` is still caught. The manifest holds 365 faults: 64 oracle, 272 check, 29 fuzz.
- After the fix, the target ran for 200 s on the Mac's fuzz build, through `heavy.sh` with its dictionary, for each of libFuzzer seeds 1, 2 and 3, each from a fresh copy of the seed corpus: 12,120,731, 10,056,227 and 10,135,578 runs, and no property failed.

### Rule slips

None affected a number.
- Stage 1a: one empty zsh heredoc, written to `/tmp`.
- Stage 1b: two `<<` heredocs. It also ended a turn while a fuzz fault was still running, then resumed and finished it.
- Stage 2a: two `python3 - <<'EOF'` heredocs.
- Stage 3a edited files outside its brief where its work needed them: `src/ad/tape.*`, `src/results/table.*` and `aad_table.*`, the table tool, the results fuzzer and `cmake/area_bench.cmake`.
- Stage 3b: one here-string. A rebuild on the Mac during a measurement, which was later discarded. One build on the Linux host run over an attached SSH session, which timed out.
- Stage 4: one empty heredoc. 0d682fc fails the format check, in `src/ad/dual.hpp` and `src/pricer/conditional_kernels.hpp`; 09ad30a formats them. It also edited `cmake/area_pricer.cmake` and `src/results/aad_table.cpp`, outside its brief, where its work needed them.
- Second CI round: one `python3 - <<'EOF'` heredoc.
- Stage 5b: one here-string and one empty heredoc, both written to `/tmp`.
- Stage 5c: an empty here-string and an empty heredoc, both written to `/tmp` (above, under "Stage 5c"). A pre-commit wrapper hit the tool's 30-minute background limit while waiting on another project's measurement lock; the Linux host's run itself finished and passed. Its first `perf` pass was multiplexed: the harness's counts from it are not used, and the one figure taken from it, scaled by `perf`, says so (above).
- Intel SDE: one wait call longer than 600 s, which the tool moved to the background.
- Stage 6a: the first pre-commit wrapper (the README commit) hit the tool's 30-minute background limit while waiting on another project's measurement lock; the Linux host's run itself finished and passed, and its logs were fetched by hand. The 600 s fuzz call ran past its 600 s tool timeout and was moved to the background, then waited for by polling.
- Stage 6b: one empty here-string, written to `/tmp`. The Linux host's fit run was first started with `&` from its zsh login shell, so at nice 5. It was stopped within a minute, before it had built anything, and restarted from bash at nice 0; nothing from the first start is used. The `measure.sh` call for the fit report waited on load past its 600 s tool timeout and was moved to the background, then waited for.
- Stage 7a: one empty heredoc. One foreground wait ran past its tool timeout and was moved to the background, then waited for. Two pre-commit wrappers on the Linux host hit the tool's background time limit; the remote runs finished and their logs were fetched by poll. Before the coordinator's heavy.sh rule (2026-11-05), these Mac jobs ran outside `heavy.sh`: the gates of the first three commits (release and fuzz builds and their full ctest runs), three further full release builds, the full fuzz build, the 60 s development fuzz, the per-file clang-tidy runs, the report preview and the first fault run, which also ran without the toolchain environment and so with Apple clang.
- Stage 7b, the drafting session (from its build tree and command history): two `python3 -I - <<< ""` here-strings, written to `/tmp`; six runs of the report tool on synthetic chains outside `heavy.sh`, three of them checks of 2 min 20 s at 8 threads and the rest under 30 s. Its 300 s fuzz runs, gate and tidy went through `heavy.sh`. Stage 7b, the continuing session: three runs of the tool's short synthetic check (about 22 s each, 8 threads) and a few seconds of diagnostics ran outside `heavy.sh`. The registered check on the real chain (production refits) and the N = 2^20 report ran on the uncommitted tree just before `--refit` was added, which changes neither the report nor the production refits; the polished check and the timing ran on e1da8dc's code. The first GCC pre-commit of 352cf29's tree failed on a range-for copy that clang accepts (`-Werror=range-loop-construct`), fixed before the commit.
- Stage 7c: one empty `<<'EOF'` heredoc, which ran nothing. Two background jobs through `heavy.sh` (the pre-registration gate and a build) hit the tool's 30-minute background limit while waiting on another project's measurement lock, before they started, and were restarted with a longer limit; one foreground wait ran past its 600 s tool timeout and was moved to the background, then waited for. Outside `heavy.sh` on the Mac: the scratch probes (`build/s7c/probe*.cpp`, two-file clang compiles of under a minute, and runs of seconds: the synthetic fits and box points before the pre-registration, and the replays of the three fuzz findings), and `clang-format`. The code commits' GCC pre-commit and libstdc++ tidy ran on the Linux host.
- Stage 8a: outside `heavy.sh` on the Mac: the quadrature tool's targeted builds and its `--write` and `--check` runs, a quadrature test run while drafting, two scratch probes (`build/s8a/probe_bits`, `probe_ki`: compiles and runs of seconds), one targeted build of the tool before the pilot, and `emit-plan` runs of under a second. The gates, tidy, faults, pilot and fingerprints went through it. Waits on another project's measurement lock: one Mac gate call hit its 590 s timeout in the fuzz build, and its remaining steps (fuzz 19 of 19, format, scan) were run by hand; two GCC pre-commit wrappers hit the tool's background limit, and their Linux runs finished and passed and were read over SSH; the Linux fingerprint's first call ran past its 600 s foreground timeout and then the background limit before it started, and was rerun detached on the host, a second time after the first rerun called a `/usr/bin/time` the host does not have. One Mac gate was stopped and rerun after a test file changed under it; the rerun found a format violation in `main.cpp`, fixed before the commit.
- Stage 8b: outside `heavy.sh` on the Mac, the curation and analysis scripts (`build/s8b/`, runs of under a second). The curation script's import wrote `scripts/__pycache__/` into the working tree; it was untracked, and was deleted before anything was staged. The run, the gates and the investigation went through `heavy.sh`. The first GCC pre-commit wrapper (the run-2 file) hit the tool's background limit while the Linux host's `heavy.sh` waited on another project's measurement lock, about 7 hours in all; the remote run finished and passed (708 of 708), and its status and logs were read over SSH. One wait on CI ran past 600 s and the tool moved it to the background. One sentence of "Stage 8b: measured run 2" (the host line, on where run 1 was recorded) was edited after the second GCC pre-commit had shipped its tree: the tree the Linux host built and tested (708 of 708) differs from 22e9d3d's in that one line of `docs/NOTES.md` and nowhere else.
- Stage 9: `aadp_metal_info` ran once directly on the Mac, outside `heavy.sh`: one device creation and library compile, under a second. Two waits ran past the tool's 600 s foreground limit and the tool moved them to the background: the libstdc++ clang-tidy pre-commit wrapper (838 s on the Linux host, PASS) and one wait on CI, which was stopped and replaced by waits bounded under 600 s. The tool keeps an output file in the system temporary directory for each background command, including the fault run and the `GPU-1` run, whose output went to `build/s9/`. One `python3 -I -` read its script from stdin, and two `cat` commands redirected only from `/dev/null`; none was a heredoc. The GCC pre-commit and the libstdc++ tidy for d65b8e5 shipped a tree that differs from d65b8e5 only in `README.md`, then uncommitted.
- Stage 10: two Mac jobs were started with a bare zsh `&`, so at nice 5: the first full clang-tidy run and the first gate of 9180dc3's tree. The gate's ctest failed on `aadp_run_bench_script`, which refuses nice 5. Both were rerun from the tool's background mode at nice 0, and nothing from the niced runs is used (the tidy findings were fixed and the clean rerun is the one recorded). Running the flag check's self-test while drafting wrote `scripts/__pycache__/`, untracked; it was deleted before anything was staged, and the test now sets `sys.dont_write_bytecode`. One `python3 -I -` read nothing from `/dev/null`, a no-op; it was no heredoc. The tool keeps an output file in the system temporary directory for each background command; the runs' own output went to `build/s10/` and the research cache. Outside `heavy.sh` on the Mac: the release preset's configure (under a second), single-target runs of `aadp_qlcompare_test` of a few seconds, `clang-format`, the flag check script and the scratch table scripts. The first GCC pre-commit of 9180dc3's tree failed on `-Werror=dangling-reference` in the new test, fixed before the commit. e62141c and 01f467d were gated once, on the tree that held both. The scratch probe of QuantLib on unregistered seeds is recorded above, under "Stage 10 results". The libstdc++ clang-tidy pre-commit wrapper hit the tool's two-hour background limit while the Linux host's `heavy.sh` waited on another project's measurements; the remote run finished and passed (176 units clean), and its status and logs were read over SSH.
- Layout and licence (2026-11-12): the full check was started from a small Python launcher in its own session, at nice 0 and through `heavy.sh`, not from the tool's background mode, whose two-hour limit is shorter than the run. The layout commit was made with `git commit -i` and an explicit path list that left out the untracked root file, so that git would not stage it again.
- Stage 11: `aadp_bench_aad --help` started the full benchmark directly on the Mac, outside `heavy.sh` and `measure.sh`, and it was stopped after about 2 minutes; nothing from it is used. The scratch probes on the Mac (two-file compiles and runs of seconds) ran outside `heavy.sh`. One `cat` waited on stdin and was stopped; one `python3 -I -` and one `cat` read only `/dev/null`, no-ops, and none was a heredoc. The first Mac gate ran without the toolchain environment: its builds used the configured clang and ninja, and `data_verify_commits_script` failed for want of ninja on PATH; it was rerun with the environment and only the rerun is recorded. One tidy script started the Mac tidy with a bare `&` inside a bash script, at nice 0. One foreground wait ran past its 600 s tool timeout and was moved to the background, then stopped. The tool keeps an output file in the system temporary directory for each background command; the runs' own output went to `build/s11a/` and the research cache. The test file got a fix for clang-tidy only after the first fault run, and the three order faults ran again on the final test (above).
- Stage 11, the timing test and the inverse normal's fast path: two smoke runs of a timing driver on the Mac outside `measure.sh`, whose numbers are not used; two pre-commit runs superseded by reruns, of which only the reruns are recorded. The tool keeps an output file in the system temporary directory for each background command.
- Stage 11, the SVI-11 fix: one `python3 -I -` waited on stdin for 2 minutes and was stopped; it ran nothing and was no heredoc. Outside `heavy.sh` on the Mac: the scratch probes (`build/s11b/probe*.cpp`, `makeseed.cpp`: compiles and runs of seconds), and single-target rebuilds and runs of `aadp_svi_fit_test`, `aadp_svi_fit`, `aadp_run2_fingerprint` (5 s) and `aadp_fuzz_plan` (single inputs). One foreground wait ran past its 600 s tool timeout and was moved to the background, then finished. The GCC pre-commit and the libstdc++ clang-tidy of the plan fix shipped trees that differ from the commit only in a few sentences of this file. The tool keeps an output file in the system temporary directory for each background command; the runs' own output went to `build/s11b/` and the research cache.
- Stage 11, the lead: an unattended job's Python import wrote `scripts/__pycache__/` into the working tree. It was removed at once and never staged.

## Option chain

### Result

The counts and values in this section are derived from the chain. Data: TAIFEX via data.gov.tw, Open Government Data License (NOTICE).

TAIFEX TXO, trade date 2024-11-15, qualifies, for three expiries only. It was the one candidate of over 30 to pass all four tests: real, licensed for redistribution, valid provenance, fixed file.

### Licence, quoted

Dataset page https://data.gov.tw/dataset/11320 lists `授權方式 政府資料開放授權條款-第1版`.

Open Government Data License v1.0, https://data.gov.tw/en/license:

> The Data Providing Organization grants User a perpetual, worldwide, non-exclusive, irrevocable, royalty-free copyright license to reproduce, distribute, publicly transmit, publicly broadcast, publicly recite, publicly present, publicly perform, compile, adapt to the Open Data provided for any purpose, including but not limited to making all kinds of Derivative Works either as products or services.

> If User fails to comply with the attribution requirement, the rights granted under this License shall be deemed to have been void ab initio.

TAIFEX terms, https://www.taifex.com.tw/cht/edu/userTerms, clause 3:

> 任何人不得逕自使用、修改、重製、公開播送、改作、散布、發行、公開發表、進行還原工程、解編或反向組譯等侵害智慧財產權之行為，但臺灣期貨交易所已授權「政府資料開放平臺」（https://data.gov.tw）提供公眾使用之本網站資料，不在此限。若您欲引用或轉載前述軟體、程式或網站內容，請依法先取得臺灣期貨交易所或其他權利人之事前書面同意或授權

The carve-out exempts site data that TAIFEX authorised data.gov.tw to offer to the public. The next sentence asks for prior written consent to quote or reprint the "aforementioned" software, programs or site content, and that is ambiguous. The English terms at https://www.taifex.com.tw/enl/eduEng/userTerms have no carve-out at all.

### What remains doubtful

None of this was resolved in the data's favour.

1. Licence chain is reasonable, not express: Securities and Futures Bureau, then TAIFEX's authority, then the licence. The listed provider is the Bureau, not TAIFEX.
2. TAIFEX terms ambiguity, above. Keep the raw file unmodified and cite the source. This is a reading, not legal advice.
3. Internet Archive's Terms of Use could not be read, because the page is a JavaScript shell. Its developer guide requires a descriptive User-Agent on automated requests. The `id_` modifier is observed behaviour, not documented.
4. Single point of failure. There is one capture, and if it goes the calibrate step cannot run until the user chooses a mirror or a new snapshot.
5. The licence covers the open file only. Venue POST quotes were a diagnostic and must not feed calibration.
6. One date only. No stability test is possible.

### What the data is

- TXO is European on the TAIEX. All six expiry dates are confirmed from TAIFEX's final-settlement table.
- TAIFEX's rule (Art. 15): settlement is the last trade, and if there was no trade in the final 15 minutes, TAIFEX decides it. So volume > 0 does not prove a price is market-observed. The file has no trade times, so "market-observed" is an upper bound.
- At least 73.7% of all regular rows and 52.0% of OTM rows are exchange-set. 202506 is 100%.
- `盤後` rows dated 20241115 ran before that day's regular session. A date filter alone mixes two market states. Regular session only.
- Bid and ask are blank on most untraded series, and quotes exist only on traded rows. Mid parity RMSE is 13 to 232 points against 2 to 3.4 for settlements.

| Expiry | Date | T | OTM listed | Traded | Settle = last | Final set | Exchange-set |
|---|---|---|---|---|---|---|---|
| 202411 | 2024-11-20 | 0.0137 | 117 | 105 | 94 | 65 | 19.7% |
| 202411W4 | 2024-11-27 | 0.0328 | 67 | 63 | 51 | 51 | 23.9% |
| 202412 | 2024-12-18 | 0.0904 | 115 | 94 | 64 | 64 | 44.3% |
| 202501 | 2025-01-15 | | 79 | 34 | 16 | excluded | 79.7% |
| 202503 | 2025-03-19 | | 69 | 21 | 17 | excluded | 75.4% |
| 202506 | 2025-06-18 | | 57 | 7 | 0 | excluded | 100% |

### Recommended calibration set

- Expiries 202411, 202411W4, 202412. Session `一般`. Field `結算價`, OTM by forward, Black-76 implied vol.
- Filters: `成交量 > 0`, `結算價 == 最後成交價`, settlement at least 1.0 point. The last threshold is ours.
- Not used: mids, `盤後`, venue quotes, a two-consecutive-zero-bid cut (a `-` in the open file is not a zero bid).
- Forward and discount factor from `settle_all` parity regression: F 22761.47, 22786.07, 22792.92, DF 0.99987, 0.99944, 0.99829. Within 0.92 points of TX futures where a TX month exists.
- T: calendar time from 13:45 on 2024-11-15 to 13:30 on expiry.
- Weights: price residual over σ_i, where σ_i² = σ_leg² + tick_i²/12, and σ_leg = RMS / √2 of the `settle_all` regression: 1.80, 2.42, 1.98 points, which the stage 6b fit uses. No volume or spread weights. (Corrected at stage 6b. This line first gave 1.87, 2.43, 1.55 as if from `settle_all`. Those come from Phase 1's `settle_near` route, strikes with |K / TXref − 1| ≤ 5%, whose RMS `profile_recommended.py` hard-codes rounded to two places, 2.65, 3.44, 2.19, before dividing by √2. Stage 6b reproduces them by that route; see "Stage 6b".)

Input arbitrage before any fit, to report beside fit violations:

| Set | Butterfly triples | Violations | Beyond half-tick |
|---|---|---|---|
| Settlements, all strikes | 984 | 200 | 55 |
| Settlements, OTM | 480 | 102 | 45 |
| Mids, all two-sided | 355 | 150 | 123 |

### Consequence for the project

The market-anchored surface runs 5, 12 and 33 days, so pricing off it is limited to short maturities. Any longer-dated surface is fitted to exchange-set marks and is labelled that way everywhere.

### What else was checked

- Failed on terms or provenance: FirstRateData SPX sample, BTC/ETH Figshare package (Tardis forbids redistribution), ALSI (Bloomberg), three Nifty sets, Deribit (personal use only on the 2021 text), Tardis, several Hugging Face sets with no licence.
- Doubtful: Mendeley Deribit snapshots (CC0 over venue terms), CRAN `NMOF` DAX and `RND` S&P 500 (GPL packaging over exchange data), Binance EOHSummary (CC BY-NC-SA, thin strikes), DoltHub options (anonymous uploader), Aevo snapshots (upstream terms unread), Kaggle kylegraupe (CC0, no source named).
- Exchange terms read, no redistribution: Cboe, Eurex, ICE, Nasdaq, LSEG, Euronext, ASX, JPX, HKEX, Borsa Istanbul.
- Could not read, no conclusion: CME, OCC, JSE, NSE, BSE, B3, MOEX, TASE, SGX, KRX, SIX, OKX, Bybit.
- Not checked: data.go.kr, data.go.jp, data.europa.eu, data.gov.in, Kaggle catalogue search, on-chain venues.

## AAD

- No single constant is "the" cheap-gradient bound. Sources give exactly 3, [3, 4], at most 4, at most 5 and below 6, in different models with different numerators, and the primary text was not opened.
- Sources mean different things by "multiple". Capriotti, Giles and Savine use value plus Greeks over value. dco's R and Hogan use adjoint run time including the forward pass over primal. Savine's own figures disagree: "less than three times", "approx 4-8x" on a slide that computes to 2.5, "2-5 times". Only the first has a stated workload.
- A better primal raises the multiple. Giles's own code runs 1.27 on g++ and 1.90 on icc. So we report against the scalar primal and the fastest primal.
- Per-path tape beats whole-simulation tape on memory by three orders of magnitude (dco: 1.9 MB against 2 GB). Run-time evidence is mixed, so we measure it.
- Per-path adjoint of shared set-up destroys the constant multiple (Capriotti & Giles: linear in N without binning). The surface stays outside the per-path tape.
- Tape cost budget from dco/c++: up to 2 for the active type, 2 to 6 for recording, under 1.5 for interpretation.
- Hogan's LW loop, with no transcendental functions, runs at 33 times primal. It's the worst case for a tape, and we always publish one such workload.

## Barrier treatment

- Continuous contract: log-space Brownian-bridge survival weight, ∏(1 − p_n). Under exact GBM steps the only remaining error is Monte Carlo noise; that is our inference, not a theorem we opened. Under Euler with local vol, bias is first order in 1/N, with payoff support conditions (Gobet 2000, abstract only). A down-and-out with K ≤ H does not meet them, so that case is measured.
- Discrete contract: one-step survival. Unbiased, and its pathwise version is unbiased (Gerstner et al., Theorems 2.5 and 2.7, arXiv 1804.03975v3). The paper calls the integrand Lipschitz, but ∂Φ⁻¹(pu)/∂p is unbounded at the corners (derived), so test AAD at p near 1.
- Never use the bridge on a discretely monitored contract. BGK Table 2.1 at H=97: 3.060 continuous against 3.834 discrete.
- First derivatives from the bridge are accurate, but they are not Lipschitz, so differentiating again is unstable (Gerstner 2019). Barrier gamma goes through one-step survival.
- Naive failure, derived: pathwise delta of the discrete down-and-out converges to 0.391 against 0.798 at H=95, and more paths do not help.
- QuantLib's `withBrownianBridge` is path construction, not a barrier treatment. QuantLib's unbiased barrier pricer samples the extremum from uniforms with a hard-coded seed 5 that `withSeed` does not reach.

## Bump size

With a treatment in place, the estimator is continuous in θ, so variance of the central difference does not grow as h falls. h is then limited by round-off, not by Monte Carlo, and we choose it by halving. Glynn's n^(−1/5) scaling applies only to the naive barrier difference, where a path can jump. A search summary claimed n^(−1/6) for common random numbers, which is wrong: that is Glynn's rate for independent streams.

## SVI

- Use wing limit 2, not 4. Zeliade and QuantLib check `b(1+|ρ|) <= 4`, which is necessary but admits a Lee violation: a slice at 2.25 passes it and fails the butterfly lemma (derived).
- VW4 eq. (12) has a sign error in the Dupire denominator. Its own R code, QuantLib and g(k) all agree on the opposite sign.
- Local variance is ∂T w / g(k) (derived, expanding g). So butterfly and calendar freedom give non-negative local variance, and a floored point is a real signal: count it.
- Corbetta's eSSVI calendar conditions are claimed necessary and sufficient, but Mingone and Pasquazzi disagree, and a random search found crossings in 27,061 of 119,956 pairs under Corbetta's conditions alone, against 0 of 110,199 with ψ₂/ψ₁ ≤ θ₂/θ₁ added (derived).
- On the DAX, the quasi-explicit fit puts rho at −1 on 11 of 12 slices. An active bound breaks a plain implicit-function adjoint, so we plan for it.
- "RMSE" means several different things across papers, so those figures are not comparable. Zhang, Lin and Lu define theirs per timestamp, then average, which is why `SVI-11` reports a slice RMSE by that exact formula.
- On the same table, Hagan SABR, skew-SABR, cubic spline and a quartic polynomial all beat raw SVI on the three-month contracts. So a raw SVI fit may not be the best fit, and any fit-quality claim is reported beside the arbitrage cost of the alternative.

## Decisions

### RNG: Philox4x32-10 as the reference stream

The brief requires scalar and SIMD to share a stream, and a float32 error comparison needs the GPU on the same draws. A keyed 32-bit counter generator runs identically on scalar, NEON, AVX2 and Metal, at any lane and thread count. It keys on (path, step). xoshiro256++ is faster and has a vector port in Highway, so it is evaluated as a CPU-only option with fixed block-to-stream assignment, where whole blocks of paths map to lanes.

### SIMD: Highway, static dispatch

`std::simd` is not usable: libc++ has none and libstdc++ is x86-only. Highway is Apache-2.0 or BSD-3-Clause, and covers NEON, AVX2 and AVX-512. Static dispatch per build, so one binary has one FMA behaviour. SME is skipped: streaming mode managed 16 GOPS against Neon's 56 in double precision on a base M4 (context, not this chip).

### How the tape handles vector values

One tape structure is shared across lanes: statements and operand indices are recorded once, with per-lane partials and a vector adjoint. All lanes must therefore take the same statement sequence, so kernels must be branch-free, which is what the bridge, one-step survival and smoothing treatments give. Where that does not hold, the scalar tape runs instead. Failure to vectorise the adjoint "can easily add an order of magnitude" (NAG authors, vendor article). No published measurement of a hand-written SIMD tape sweep was found, so this is measured, and claimed only with assembly.

### GPU: price only

AAD does not run on the GPU. Greeks come from the double-precision CPU tape, and the GPU backend prices only, in float32. MSL has no `double` for the accumulation NAG needed, no AD tool for MSL was found, and a second hand-written adjoint adds risk to the project's central claim. Kernels compile at run time from an embedded string, because this host has no offline `metal` compiler. The binding is a small Objective-C++ file behind a C++ interface.

float32 defects to prevent, not report: a float32 running sum stalling (up to about 83% at N = 1e8, derived), `u = 1.0` giving INF, and reassociation removing compensated sums. After those, expected error is about 1e-7 to 1e-5 relative, below SE at any N this project will run, and only a paired comparison can see it.

### QuantLib: pinned, optional, compared on strict rows

v1.44 and Boost 1.92.0 through FetchContent, behind an option. QuantLib's `withBias(true)` is the strict discrete contract, so BGK m=50 is representable exactly. Its arithmetic Asian engine cannot sub-step between fixings, so local-vol rows are one step per fixing. RNG streams cannot match, so every row is a statistical comparison. Row definitions: `quantlib-compare/EVIDENCE.md`, "Comparison protocol".

## Open questions for later phases

- Whether upstream LLVM 23 with libc++, C++23 and Boost 1.92 compiles QuantLib on macOS is unverified, because Phase 1 forbids building. `cmake` is available. Boost is not installed, and comes through FetchContent.
- Rounding mode, denormals and real `exp` and `log` error on this GPU are unknown, and need short kernels. Resolved at stage 9: round to nearest even, denormals flushed on input and output, `precise` exp within 1.5 ULP and log within 2.6 ULP ("Stage 9 results").
- Whether Apple's command-buffer limit bites at useful batch sizes is unknown. Resolved at stage 9: no buffer failed up to 3.54 s of GPU time, and the batch is 2^22 paths, 28 ms ("Stage 9 results").
- `actionlint` is not installed. Phase 2 needs it from repo tooling. Resolved in Phase 2: `scripts/fetch_tools.sh` installs actionlint 1.7.12 into `build/tools/`, pinned by sha256 (fbb1135).
