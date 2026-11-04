# NOTES

Research findings and decisions. Later phases add what was tried, including what did not help.

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
| `src/data/`, `tools/chain_profile/`, `scripts/fetch_data.sh`, `NOTICE` | cp950 decoder, TAIFEX readers, data script | `data` |
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
- Private note: the one `.gitignore` line that keeps the local settings file out, and any mention of the research folder.
- Private note, by digest: the notes file at the repo root, only while its sha256 matches a pin. Any edit fires its findings again until someone reviews it and pins the new digest. Every other kind still applies to it. Whether that file should be tracked at all is the user's open decision.
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

Stages 1a to 3b, a1d9325 to e495a37, then the CI fixes up to 54933ed. Measured numbers are under each target in `BENCHMARKS.md`, from the files in `results/`.

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
- The tape costs 166 times the primal on Lax-Wendroff on `mac-m4max` and 106 on `linux-zen1`, against Hogan's 32 to 33 for Adept. The hand adjoint costs 3.37 and 3.24 times, against Hogan's 2.1. Not yet explained: an iterate item in `TASKS.md`.
- Two Linux bugs in the per-variant RSS probe. On Linux, `wait4` reported the parent's peak for every `posix_spawn` child, so each child now reports its own (c1c1a62). And the kernel writes a tab after `VmHWM:`, which the parser did not accept, so every probe failed (d1596e0). The fault `bench-rss-parent-peak` is caught.
- Threaded rows on `linux-zen1` run on two pinned CPUs, 4 and 6, through `AADP_PIN_CPUS=4,6` in `scripts/run_bench.sh` (bc3e6d3). cpu5, the SMT sibling of cpu4, is recorded as meant to be idle, which nothing enforces. The stamp checks each pinned CPU against `measure.sh`'s list, and refuses a two-CPU run from a Debug build (0bde08f).
- 3a's iteration runs are not results and are not quoted here.

### CI

- CI first ran on GitHub as run 41770201004 at bc3e6d3 and failed 9 of 17 jobs: actionlint, clang-tidy, `linux-gcc-release`, `linux-gcc-debug`, `linux-tsan` and the four macOS jobs. The Linux clang, AVX2, AVX-512, ASan and UBSan, fuzz-smoke, format and repo-scan jobs passed.
- actionlint: SC2086 on unquoted GCC configure arguments. Local actionlint had no shellcheck, so it never linted `run:` steps. d8e39d8 quotes them and pins shellcheck 0.11.0, and the fault `check-actionlint-shellcheck` shows the local lint now sees it.
- GCC: g++ 14 `-Werror` failed on a sign conversion and a dangling `else`. The local GCC check then found a possibly null `%s` at -O3. All three fixed in 74f0aac.
- `linux-tsan`: TSan's static runtime on Linux already defines `operator new` and `delete`, so the counting allocator in `tests/ad/allocation_test.cpp` cannot link there. 8d00d85 leaves that one test out of Linux TSan builds only. macOS TSan still runs it, 502 of 502.
- macOS: the four jobs failed to link `std::__1::__hash_memory`. The cause and the move to `xcode-27` are above, under "CI on macOS". What still differs from the measurement Mac: the runner's OS is 27.0.1, not 27.2, the SDK comes with Xcode 27.2, the image is a preview, and the hardware is a VM.
- clang-tidy: CI's Linux clang uses g++ 14's libstdc++, while the Linux host and the Mac use libc++. CI named findings in 8 files. On the Linux host those 8 files gave 10 findings with libstdc++ and 2 with libc++, and the Mac none. 74f0aac's own fix added 2 more, in `tests/math/inverse_normal_test.cpp`. bb7b26f clears all 12, in 9 files.
- bb7b26f also prints every finding in the tidy log, since CI's log named only the files, and uploads the full logs when the job fails (`actions/upload-artifact` pinned to cf430e0, v7.0.2).
- `scripts/check.sh` passes 13 of 13 at 54933ed, with 100 of 100 faults caught. The fixes are committed and not pushed, so no CI run has seen them yet.
- Run 41884006533 at 065e5b6 passed 16 of 21 jobs. The four macOS jobs failed `PerfCounters.MacOsCountersAreProcessWideAndLabelled`: GitHub's macOS runners are virtual machines (the runner-images repo builds them as VM images; the jobs now print `kern.hv_vmm_present`), and there `proc_pid_rusage` leaves `ri_cycles` and `ri_instructions` still. Cycle and instruction counts cannot come from a GitHub macOS runner. The harness already wrote such a counter as `missing`; its note now says `unavailable`, which counter, the value it stayed at and whether the host is a VM. The test reads `kern.hv_vmm_present` itself: where it is 0, as on the measurement Mac, both counters must count; on a VM, each counter must count or be labelled unavailable with that reason. `HarnessSelftest.MacOsHeaderNeverWritesAStuckCounterAsACount` checks the same in the header, and the fault `bench-darwin-stuck-counter`, a counter that stayed still reported as a count of 0, is caught on every host. No test is skipped for this.
- Run 41884006533 also failed `linux-tsan`: `HarnessSelftest.RefusesAHistogramOfBatchesTooShortToResolve` and run_bench.sh's failed-run check relied on one unit of work (16 multiply-adds) being below the gate. Under Linux TSan it takes about 200 ns, at the gate of 100 to 200 ns: on debian the test failed too and the script check passed only because its median, 190 ns, fell under that run's 200 ns gate. Both now use the selftest's `--no-work`, a batch of two timer reads, 60 ns there.
- The macOS runners start a job at nice -10, which the stamp tool refuses, so `aadp_run_bench_script` failed there. The test steps raise niceness by the measured offset to 0, and print both levels.
- CI's Linux clang jobs (debug, release, asan-ubsan, tsan, avx2, the lane jobs, fuzz-smoke and `clang-tidy`) now use libc++ from the pinned LLVM 23.1.4 release, as the Linux host does: the setup action writes the host's `clang++.cfg` (`-stdlib=libc++` and a link-time rpath to `lib/x86_64-unknown-linux-gnu`), checks that the release ships libc++ there, and probes which library a program gets. The GCC jobs stay on libstdc++, and `clang-tidy-libstdcxx` keeps tidy on it, since its findings differ by library.
- Owed: `scripts/verify_commits.sh` needs rsync, which the Linux host lacks; the GCC pre-commit check there copies files without it.

### Rule slips

None affected a number.
- Stage 1a: one empty zsh heredoc, written to `/tmp`.
- Stage 1b: two `<<` heredocs. It also ended a turn while a fuzz fault was still running, then resumed and finished it.
- Stage 2a: two `python3 - <<'EOF'` heredocs.
- Stage 3a edited files outside its brief where its work needed them: `src/ad/tape.*`, `src/results/table.*` and `aad_table.*`, the table tool, the results fuzzer and `cmake/area_bench.cmake`.
- Stage 3b: one here-string. A rebuild on the Mac during a measurement, which was later discarded. One build on the Linux host run over an attached SSH session, which timed out.

## Option chain

### Result

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
- Weights: price residual over σ_i, where σ_i² = σ_leg² + tick_i²/12, and σ_leg = 1.87, 2.43, 1.55 points. No volume or spread weights.

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
- Rounding mode, denormals and real `exp` and `log` error on this GPU are unknown, and need short kernels.
- Whether Apple's command-buffer limit bites at useful batch sizes is unknown.
- `actionlint` is not installed. Phase 2 needs it from repo tooling. Resolved in Phase 2: `scripts/fetch_tools.sh` installs actionlint 1.7.12 into `build/tools/`, pinned by sha256 (fbb1135).
