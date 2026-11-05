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

Stages 1a to 3b, a1d9325 to e495a37, then the CI fixes up to 54933ed. Stage 4, c17c90a to 4072072, and stage 5a, to 065e5b6, then the second CI round up to 48d30a7. Stage 5b, 9459507 to e98a570, stage 5c to a398d1f with its CI fix in 1029047, then Intel SDE for the 8-lane CI job, 36d701d and a6be05a. Measured numbers are under each target in `BENCHMARKS.md`, from the files in `results/`.

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
- Owed: `scripts/verify_commits.sh` needs rsync, which the Linux host lacks; the GCC pre-commit check there copies files without it.
- Stage 5b's runs 41918421254 and 41924618330 passed 22 of 22, but on both the 8-lane runner lacked AVX-512, so that job was build-only with a warning. Run 41898528776's runner had it. Whether a green run had tested 8 lanes depended on the runner GitHub assigned.
- Run 41942650525 at a398d1f passed 16 of 22. Both tidy jobs failed on `tests/bench/platform_test.cpp`, whose x86-only CPUID test (8e8a464) the Mac's tidy cannot see: unchecked optional access, and `find` where tidy wants `contains`. The four lane jobs that ran tests counted `aadp_simd_fallback_cost_smoke_not_a_result` as skipped, because their build target list lacked that binary (added in 7b97155), and a skip fails a job. 1029047 fixes both. So 8e8a464 to a398d1f fail CI's tidy, and 7b97155 to a398d1f its lane jobs. Run 41953763165 at 1029047 passed 22 of 22, with the 8-lane job build-only again.

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

SVI-2, iteration only, not a result: 400 out-of-the-money quotes (|z| ≤ 3, T from 0.0137 to 2, σ from 0.1 to 0.8) through `measure.sh` on macOS, 5 repetitions: 140 ns per inversion (56.1 µs per set, CV 0.09 %), against 20.9 ns per `Black76Price`. No measurement plan, stamped file or BENCHMARKS entry exists for it.

The checks, none of which repairs anything; each violation carries its kind, its k range, worst k, value and bound:
- butterfly grid: k from −3 to 3 in steps of 0.001, joined with m + σ y for y from −40 to 40 in steps of 0.01, so about 14,000 points, 100 per σ around the minimum of w. Reason: [−3, 3] is e^±3 of the forward, far beyond quoted strikes, and w'' has width σ. ε_g(k) = 32 ε c_w S, with S the sum of the magnitudes of g's terms and c_w the condition of w's sum: about 1e-14 on these slices, against Vogt's −0.033;
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

Objective and weights. A slice is the calibration set of one expiry with its F, D and T. Quote i has strike K_i, log-moneyness k_i = ln(K_i / F), settlement V_i and the noise scale σ_i, with σ_i² = σ_leg² + tick_i² / 12, tick_i the tick at V_i: a uniform rounding to the tick has variance tick² / 12. No volume or spread weights. The model price is `Black76Price(F, D, K_i, T, √(w(k_i) / T))`, w raw SVI. The objective is Φ = Σ r_i², r_i = (V_model,i − V_i) / σ_i, reported with Φ / (n − 5).

The fit, in the quasi-explicit form of Zeliade (De Marco and Martini, 2009): for fixed (m, σ), with y = (k − m) / σ, w = a + d y + c √(y² + 1), c = b σ, d = ρ b σ, so w is linear in (a, d, c).
- Inner solve, for fixed (m, σ): Gauss-Newton on Φ in (a, d, c). At the current w_i, J_i = (∂V_i / ∂w) / σ_i, with ∂V / ∂w = D F φ(d1) / (2 √w) for calls and puts alike. One step minimises Σ J_i² (t_i − w_i(a, d, c))², t_i = w_i − r_i / J_i, a weighted linear least squares in (a, d, c). So the price-space weight 1 / σ_i² maps to the variance-space weight J_i², the price's sensitivity to w over its noise, squared. The first step is taken at the market total variances ŵ_i = T σ̂_i² (σ̂_i from Let's Be Rational), where r_i = 0: that is the classical quasi-explicit solve with vega weights. Its fixed point has Σ J_i r_i ∂w_i / ∂(a, d, c) = 0, the stationarity of Φ itself, so iterating it gives the price-space optimum for that (m, σ), not a variance-space proxy. Each step is a convex quadratic programme in three variables with the linear constraints below, solved exactly by trying every set of at most three active constraints and keeping the best feasible point. The step is accepted with backtracking, α = 1, 1/2, ... to 2^-40, on Φ itself and on positivity. Stop when Φ falls by at most 1e-13 of itself, when no α gives a decrease, or after 50 steps (then the evaluation is marked not converged). A J_i below 1e-300 drops that quote from the step, never from Φ.
- Domain of the inner solve. Zeliade's is 0 ≤ c ≤ 4σ, |d| ≤ c, |d| ≤ 4σ − c, 0 ≤ a ≤ max ŵ_i. Kept: |d| ≤ c (|ρ| ≤ 1, which gives c ≥ 0, b ≥ 0), c + |d| ≤ 4σ (b (1 + |ρ|) ≤ 4) and a ≤ max ŵ_i. Changed, decided now with this reason: a ≥ 0 is replaced by positivity of the minimum total variance, a + √(c² − d²) > 0, that is a + b σ √(1 − ρ²) > 0. a ≥ 0 is sufficient for w > 0 but not necessary, and it excludes slices whose hyperbola lifts the minimum above a: Vogt's slice has a = −0.041 and minimum variance 0.0116, and on 5-day slices b σ √(1 − ρ²) can exceed the at-the-money variance, about 4e-4, so a ≥ 0 could bind for no reason of arbitrage. Positivity is not linear, so it is held by the backtracking (the set is convex and every iterate is inside it); the starting point is the first quasi-explicit solve if it is positive, else the flat slice (min ŵ_i, 0, 0). The bound 4 is Zeliade's and is not Lee's 2: a slope above 2 is left for the wing check to find, never prevented.
- Outer search over (m, σ): m ∈ [k_min − Δ, k_max + Δ], Δ = k_max − k_min over the quotes; σ ∈ [1e-4, 4]. 1e-4 is twenty times below the smallest strike spacing in k (50 points at 22,800, 0.0022), where the data cannot tell a narrower hyperbola from a kink; 4 is far beyond the data's width in k, where the hyperbola is a parabola on the data. A grid of 9 values of m evenly over [k_min, k_max] by 9 values of σ log-spaced over [1e-3, 1], each evaluated by the inner solve (the profile objective). Nelder-Mead in (m, ln σ) from the 5 grid points with the lowest profile objective (ties in index order), with the initial simplex one grid step in each coordinate, coefficients 1, 2, 1/2, 1/2, trial points clamped to the box. A start converges when every vertex lies within 1e-10 of the best in both m and ln σ, or when the three vertex objectives are equal doubles; at 600 profile evaluations it stops unconverged. The result is the start with the lowest Φ; the report says how many starts reached within 1e-8 of it, relatively. No randomness: the grid is fixed, so there is no seed.
- Fit errors are values, never exceptions: too few quotes (fewer than 5 distinct strikes), a non-finite or non-positive F, D or T, a bad quote (strike, price or σ_i not finite and positive), a quote with no implied volatility, and no feasible start.

Bounds, and what an active bound records. The fitted parameters are raw (a, b, ρ, m, σ), b = c / σ and ρ = d / c (ρ = 0 when c = 0). Each constraint gets its slack in natural units and is recorded active when the slack is at most 1e-9 of its scale: ρ = −1 and ρ = +1 (1 ∓ ρ), b = 0 (c against max ŵ_i), b (1 + |ρ|) = 4 (against 4σ), a = max ŵ_i, m and ln σ at either end of the box (against the box's width), and the minimum variance at 0 (against max ŵ_i). The QP sets an active constraint exactly (d = −c gives ρ = −1 as a double, not −0.9999999). Stage 7's implicit-function adjoint needs this: at an active bound the fitted parameter does not move with the quotes, so the derivative is taken on the free parameters only, and a plain inverse Hessian would be wrong there. Every active bound and every slack is printed; none is hidden.

Metrics, with no weights:
- `SVI-6`: (1/n) Σ |V_model,i − V_i| / F, in basis points of the forward, against the 4 bp context, which is eSSVI on SPX mids (BENCHMARKS.md).
- `SVI-11`: the Zhang-Lin-Lu formula on one slice, √((1/n) Σ (σ_model,i − σ̂_i)²), σ_model,i = √(w(k_i) / T) and σ̂_i the market implied vol from the settlement, in vol points (100 times the decimal), against 0.73.
- Violations are those of stage 6a's checks, run on the fitted parameters exactly as fitted: `CheckButterfly` with the default grid and the slope bound 2 (invalid parameters, non-positive variance, g below −ε_g on the grid, the d+ limit, either Lee wing, a Martini-Mingone verdict other than no arbitrage), and `CheckCalendar` between each pair of adjacent fitted slices (a crossing on the grid or in an asymptote). Each record counts as one violation, with its kind, k range and worst value. They sit beside the input butterfly counts, which count triples, a different unit; for a like-for-like line, the same triple count is also run on the model prices at the quoted strikes. Nothing is repaired: slices are fitted independently, with no calendar constraint, and a fitted slice with a violation is reported as fitted.

Synthetic recovery, on hand-made parameters only. Three slices shaped like the calibration set: F = 22,800, strikes on a 100-point grid, out of the money, only prices of at least 1 point; σ_i from σ_leg = 2 and the tick rule.
- A: T = 5/365, D = 0.9999, (a, b, ρ, m, σ) = (1e-4, 0.012, −0.45, 0.002, 0.025), strikes 19,700 to 24,100.
- B: T = 12/365, D = 0.9995, (2e-4, 0.02, −0.35, 0.005, 0.04), strikes 19,500 to 24,500.
- C: T = 33/365, D = 0.998, (5e-4, 0.04, −0.5, 0.01, 0.06), strikes 18,000 to 25,000.
- Noiseless: the prices are `Black76Price` at the true parameters. Pass if the fit converges, Φ̂ ≤ 1e-10, and every parameter j satisfies |θ̂_j − θ_j| ≤ 2 (√(Φ̂ C_jj) + √(Φ_true C_jj)), with C = (JᵀJ)⁻¹ from the analytic Jacobian of r in the raw parameters at θ̂, and Φ_true the objective at the true parameters (the prices' own rounding). The reason: near the minimum Φ(θ̂) − Φ(θ*) ≈ δᵀJᵀJδ, so any δ the stopping rule leaves has |δ_j| ≤ √((Φ(θ̂) − Φ(θ*)) C_jj) (Cauchy-Schwarz), and the rounding moves the minimiser θ* from the truth by at most √(Φ_true C_jj) in the same way; 2 allows for the quadratic approximation. So the tolerance is the optimiser's convergence, read through the conditioning, parameter by parameter. Φ̂ ≤ 1e-10 is a weighted residual RMS near 1e-6 of a noise unit, many orders above what the stopping rules leave.
- Noisy: V_i + σ_i z_i, z_i = Φ⁻¹(u_i), u_i = `PathUniform(20261105, i, slice)` (Philox, a development seed, not 1729 or 271828), one draw per slice. Pass if every parameter is within 4 standard errors of the truth, SE_j = √C_jj at θ̂. Fifteen comparisons at 4 SE fail together with probability about 1e-3 in the linear Gaussian approximation. A failure is reported; the seed is not changed.
- ρ at its bound: A with ρ = −1 exactly must fit ρ within the noiseless tolerance of −1. A with ρ = −1.05, a slice outside the domain whose variance is still positive on its strikes, must fit ρ = −1 exactly with the bound recorded active, and its mirror (ρ = +1.05, m = −0.002, strikes reflected in k) ρ = +1 likewise.
- No repair: slices at T = 1, F = 100, D = 1, strikes K = 100 e^k for k from −1.5 to 1.5 in steps of 0.05, out of the money, σ_i = 0.01, built from Vogt's parameters and from the slope-2.25 slice of `SVI-4`, must be recovered within the noiseless tolerance, and the checks must then report the density violation (Vogt) and the wing and d+ violations (slope 2.25) on the fitted parameters. A crossing pair, B and a copy of B at T = 33/365 with a lowered by 1e-4 (so below B at every k), fitted slice by slice, must be reported by the calendar check.

Timing (`SVI-7` per slice, `SVI-8` for the surface) is iteration only, through `measure.sh`, kept in the research cache and not in the report.

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
