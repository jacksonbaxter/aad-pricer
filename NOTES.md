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
- AS241 on IEEE double reached a maximum relative error of 8.47e-16 against the `erfc`-polished reference, above Wichura's 6.0e-16 on base-16 arithmetic. `RNG-3` keeps its target. The gap is examined in Phase 3, when the reference's own error has to be separated out.
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
- Sanitizers and fuzzing ran on the Mac only, by rule. The Linux sanitizer and fuzz-smoke CI jobs have not been run anywhere.

CI:
- `.github/workflows/ci.yml` has never executed on GitHub.
- actionlint ran without shellcheck and pyflakes, so it linted no shell in `run:` steps.
- No gcc on macOS, because `release-gcc` is Linux only.
- CI uses `g++-14`, newer than the image's default of 13. The Linux host ran 14.2.
- One named allowed skip, `PerfCounters.EachRegionReportsItsOwnTimes`. Counters depend on the runner's kernel settings, and the test skips itself without them. It passed on the Linux host.
- `scripts/run_tests.py` is the shared test runner for `check.sh` and CI, so both judge a run the same way. ctest alone does not fail on a skip. The runner counts a test that did not run, a disabled test and a passing test that prints a `SKIP ` line as skipped, and a skip fails the run unless a regex names it. For fuzzing it finds targets through ctest, and it stops if a file in `fuzz/` has no target.

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
