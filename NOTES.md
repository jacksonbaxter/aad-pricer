# NOTES

Research findings and decisions. Later phases add what was tried, including what did not help.

## Evidence index

Raw evidence is outside the repo, because third-party material is not committed: `/Users/jacksonbaxter/Repositories/quant-projects/.orchestrator/research-cache/aad-pricer/`. Evidence files record local paths as `.research/<track>/`, the location before they were moved. Read those paths relative to the cache.

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
| Lead's check | `verify/verify_sources.log` | Fresh download of every target source, hashes, quote search |

## Process notes

- Ten research tracks ran. `metal-gpu` and `quantlib-compare` were stopped by the account usage limit after their evidence files were written, so neither returned a final message, and both evidence files are complete to their search logs.
- The shared WebSearch budget of 200 calls per turn ran out during the first round. After that, research used direct URLs only. Gaps from that are listed in `BENCHMARKS.md` section 12.
- Third-party clones sat inside the working tree during the first round. Six third-party agent-instruction files were among them (QuantLib twice, XAD, Sacado). They were moved out, unopened by the lead, and nothing from them was committed.
- Rule slips by subagents, none affecting a number. `ad-tools` and `chain-hubs` wrote scratch files to `/tmp`, later moved or deleted. `metal-gpu` extended its probe beyond the listed outputs, with a capability kernel and counter queries but no timing. `chain-verify` fetched from Internet Archive with a generic User-Agent, which the fetch script must not repeat.
- Apple's documentation pages are JavaScript shells, so `metal-gpu` read Apple's own per-page JSON, one named page at a time. Accepted: it is the publisher's site and not a search route. It flattens tables, and that lost one table (its E61).
- Lead check method: `verify/scripts/verify_sources.py` re-downloads each source, compares sha256 and searches for the quote. Its HTML stripping eats C++ `<...>` spans, and PDF line-break hyphens defeat matching, so every miss was resolved by a raw grep. All misses were false alarms. Two pages changed bytes with the quote still present (NablaTensor post, NIST Bonferroni page).
- Corrections the lead check made to the first version of `BENCHMARKS.md`: `SIMD-2` used half an ULP for one ULP, which made the bound too tight; `MC-3` was mislabelled as price-only; `MEM-3` called a run-level peak a per-path figure; build flags in `MC-1` come from the harness and not from the post; derived oracles inherited the publication's 0.001 as b, which would have blunted them.
- Broadie-Glasserman 1996 p.282 is a scan the lead did not read. No target rests on it. β = −ζ(1/2)/√(2π) = 0.582597 was recomputed, and BGK's values were reproduced by quadrature instead.

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
- "RMSE" means several different things across papers, so those figures are not comparable.

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
- `actionlint` is not installed. Phase 2 needs it from repo tooling.
