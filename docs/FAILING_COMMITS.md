# Failing commits

For anyone bisecting this history: the commits that fail the per-commit check, by full hash, with what fails. `git log -1 <hash>` gives each one's message.

The per-commit check (`scripts/verify_commits.sh`) configures, builds and tests each commit on its own, from an export, with nothing checked out. On the Linux host, `linux-zen1` (an AMD Zen 1 machine running Debian), with the `release` preset, it ran every commit of a1d9325..2444dc8 (the range's base itself not included): 225 commits, through 2444dc8. 144 pass and 81 fail; every one of the 81 configured and built, and failed in its tests. After a commit's run, each test that failed was run again by itself on that commit's build. The commits after 2444dc8 are not in this record. The record names each failing test and its result alone; it does not record the compiler or how busy the host was.

The 81 failures are of three kinds, and 3 commits have two:
- The timing test `PerfCounters.EachRegionReportsItsOwnTimes`, in 46 commits, all before ee75be5 (position 175). Run again alone it passes in 33 of the 46 and fails again in 13. What was measured (`docs/NOTES.md`, "Stage 11"): the old test, which spun 30 ms of wall time, passed 9 of 20 runs on the Linux host beside the per-commit job and 0 of 20 beside a pinned busy loop, and the fixed test, which spins 30 ms of the thread's CPU time and came with ee75be5, passed 20 of 20 in both. Whether load on the host caused each of these 46 failures is not established: the record does not show the load, and 13 fail again alone.
- The run 2 fingerprint, `aadp_run2_fingerprint` (`Run2Fingerprint.CommittedBitsOnThisHost`), in 34 commits, every one from bf484f3 (position 138) to 92cc148 (position 171); f131c23 (position 172) is the first that passes. It fails again alone in all 34. `docs/NOTES.md`, "Stage 11: one statement order on every compiler", gives the cause found at bf484f3 and 92cc148: the order in which the tape recorded statements depended on the compiler, and the clang `release` build on the Linux host failed the committed file there.
- The RSS probes, in 4 commits from c1c1a62 (position 42) on. Only at c1c1a62 do they fail again alone, with `aadp_bench_aad_smoke_reduced_size_not_a_result`; in the other 3 the probe passes alone.

So 48 of the 81 have a test that fails again when run alone, and in 33 every failing test passes alone.

History is left as it is. Every results file, report and page cites the commit it came from by hash, and rewriting history would change every hash after the first rewritten commit and break each of those citations.

| Position | Commit | Failing tests, each run again alone | Kind |
|---:|---|---|---|
| 1 | `6073f09b615babaac20b1b292d39f22fa719ea81` | `PerfCounters` passes alone | timing test |
| 6 | `5fa6df6312a84e86964dcd6ed1a102eef3be9602` | `PerfCounters` passes alone | timing test |
| 9 | `f863647d7e7c37a905775d8d06058cf763edd7bd` | `PerfCounters` passes alone | timing test |
| 12 | `19d77897491592853a9efe3ae73da4d9cc22345f` | `PerfCounters` passes alone | timing test |
| 14 | `a7a2591ceb4053dba3fbfa93968849bd7ee0a361` | `PerfCounters` passes alone | timing test |
| 16 | `6f83702eb21520821567fa1b3c5b3749e13de7c9` | `PerfCounters` fails alone | timing test |
| 17 | `797ad17bc17b1789ee3031626689d4123331c59a` | `PerfCounters` passes alone | timing test |
| 19 | `040a32f07a5e031f2271b7ef99adf7b62380c367` | `PerfCounters` passes alone | timing test |
| 23 | `6932c4e041b5844a39637a7bb3e605f4f1c8fd7d` | `PerfCounters` passes alone | timing test |
| 24 | `653ea0745c228a4b2337bb526abab86f2c2e3ae0` | `PerfCounters` passes alone | timing test |
| 25 | `16a5211f90606653257111cf932e2b49c9a769c9` | `PerfCounters` passes alone | timing test |
| 26 | `6bfdc62140a7faec6fc5a08a45345f8baec08240` | `PerfCounters` passes alone | timing test |
| 27 | `c90e7cabdb0ea1620e38dfa05682d887a5b70d04` | `PerfCounters` fails alone | timing test |
| 28 | `9042fe36be8ab81d37a1e270d7fcb89ddadd8b27` | `PerfCounters` passes alone | timing test |
| 30 | `1172aaa4527b0a8b3de8d7ea4f933a75f484a5ee` | `PerfCounters` fails alone | timing test |
| 31 | `c3831fae3b01e09b0eec4b4f7ff0ff259ec8aab3` | `PerfCounters` passes alone | timing test |
| 32 | `f0971dc5a2a275dd43845b64091cd870bfdf3679` | `PerfCounters` passes alone | timing test |
| 36 | `93f232168da37ce6261f38a4298b3ca6a3e1ff91` | `PerfCounters` fails alone | timing test |
| 37 | `efca5cf1fdeb938ee11bbb6309cfeb2f46d7f83e` | `PerfCounters` passes alone | timing test |
| 39 | `619186056f43f89be5ef98214f38590ae3ea626a` | `PerfCounters` fails alone | timing test |
| 40 | `bc3e6d3abf0c91dcacc9b1db211d50a5fa8d775e` | `PerfCounters` passes alone | timing test |
| 42 | `c1c1a62c67f143e29f98271cbd2416f9964f838d` | `RssProbe (idle child)` fails alone, `RssProbe (tape variant)` fails alone, `aadp_bench_aad_smoke` fails alone | RSS probe |
| 43 | `d1596e0e06689b84324e2135fb10686b9ac7739b` | `RssProbe (tape variant)` passes alone | RSS probe |
| 46 | `74f0aac77b8e9270e6c4983fc1c85348ddb46705` | `PerfCounters` passes alone | timing test |
| 48 | `d8e39d88bfd361afa6b61096feb563bab3838387` | `PerfCounters` fails alone | timing test |
| 49 | `8d00d85b0dc963fab2c4c8dd89dc54577bbf76ff` | `PerfCounters` fails alone | timing test |
| 50 | `bb7b26fa959683d44e4e4c31e62bd868c5ca8cb7` | `PerfCounters` fails alone | timing test |
| 51 | `54933edce701238aaadfb3cef742f3df39649963` | `PerfCounters` fails alone, `RssProbe (tape variant)` passes alone | timing test; RSS probe |
| 52 | `7a72f73d9d5d65ba799d0ff4655aa4a9d997cbd0` | `PerfCounters` passes alone | timing test |
| 54 | `c17c90a8fa9de674a6c0eeca54bc5863ea5a4091` | `PerfCounters` passes alone | timing test |
| 55 | `0d682fca48c345971665ac42c0f865ede55237a5` | `PerfCounters` fails alone | timing test |
| 56 | `09ad30ac0095a8eb6160bfcd66539ccf98a15c5e` | `PerfCounters` passes alone | timing test |
| 62 | `22e98845f2750fc1a562dfa1f932e11caae0f650` | `PerfCounters` passes alone | timing test |
| 63 | `d0cbc09b920b8a7440fe8f4ee938b1907f21e92e` | `RssProbe (tape variant)` passes alone | RSS probe |
| 64 | `065e5b62c11c8edff2f225431cce27483dd47127` | `PerfCounters` passes alone | timing test |
| 68 | `f8aa71f2a198ece1167a84558f9cb06c5e1776c5` | `PerfCounters` passes alone | timing test |
| 77 | `a60129dc19580b8126af698e7b8e06915b9c3b29` | `PerfCounters` passes alone | timing test |
| 82 | `7b971554caef6b53392a2d598a8b40d04face5d7` | `PerfCounters` passes alone | timing test |
| 90 | `6fec621b3fde168342b0acb00158a4bef50356ec` | `PerfCounters` passes alone | timing test |
| 95 | `83d93b15c5855b685bac1631f80491061e5db4c3` | `PerfCounters` fails alone | timing test |
| 96 | `fd60de0a0bc405ddc57e10a0d1be2025e04700f7` | `PerfCounters` passes alone | timing test |
| 102 | `8eef2cb8b1267ebf76e1b629f5b21b1f375c7ae4` | `PerfCounters` passes alone | timing test |
| 107 | `8ae3239a67fb527b2b0a4d3508f29487db38e864` | `PerfCounters` fails alone | timing test |
| 108 | `23936a9e1f61e6c89a712ef555f95c4e3f1678e7` | `PerfCounters` passes alone | timing test |
| 116 | `534750995cd893ff07811c60df4866ce5b1c11a8` | `PerfCounters` passes alone | timing test |
| 125 | `00028e341b12b341ca98a66d77f7700eb501db64` | `PerfCounters` fails alone | timing test |
| 126 | `255cb494540e3c385f3d48a787e210784558388d` | `PerfCounters` passes alone | timing test |
| 138 | `bf484f3af19276f684dadf922e21cfe207280a14` | `Run2Fingerprint` fails alone | run 2 fingerprint |
| 139 | `a418764eb42bd814be685d30cb425bddf027a035` | `Run2Fingerprint` fails alone | run 2 fingerprint |
| 140 | `fa766db13ff9648b53b98f6a2649f1c950ac35f1` | `Run2Fingerprint` fails alone | run 2 fingerprint |
| 141 | `c53a117e83342859f9a2d45b8956ec10ceed645a` | `Run2Fingerprint` fails alone | run 2 fingerprint |
| 142 | `4398aeec1e4fea99e0e39656cf668a6874a41f2a` | `Run2Fingerprint` fails alone | run 2 fingerprint |
| 143 | `d70a1d4902b213c0b04a80f6291ceabd5f69e533` | `Run2Fingerprint` fails alone | run 2 fingerprint |
| 144 | `22e9d3d712c0b332e664841541d42f1474769a65` | `Run2Fingerprint` fails alone | run 2 fingerprint |
| 145 | `e76492398d6225978b2d17a5c9285feae8f60842` | `Run2Fingerprint` fails alone | run 2 fingerprint |
| 146 | `1ad0e8e731c5a4f2aa087e3535be2929c8410e83` | `Run2Fingerprint` fails alone | run 2 fingerprint |
| 147 | `830eed65f2f301ad715718b021d25558429dc2ab` | `PerfCounters` passes alone, `Run2Fingerprint` fails alone | timing test; run 2 fingerprint |
| 148 | `4069c8f88cd45e1ecd35c96f7921bbaf600b76c4` | `Run2Fingerprint` fails alone | run 2 fingerprint |
| 149 | `4aa79251f5e88dc684117f4ff68a1b424acde6bb` | `Run2Fingerprint` fails alone | run 2 fingerprint |
| 150 | `25db19ff22390cd404480d445237d1caca0f0355` | `Run2Fingerprint` fails alone | run 2 fingerprint |
| 151 | `d65b8e52441ca3301098009867bf38c5c1c4c01f` | `Run2Fingerprint` fails alone | run 2 fingerprint |
| 152 | `d605c783ac94206159424e4c14f081df759235c9` | `Run2Fingerprint` fails alone | run 2 fingerprint |
| 153 | `959315b4823b383e9beddcf31c290d2fd4140d67` | `Run2Fingerprint` fails alone | run 2 fingerprint |
| 154 | `9180dc34cfca9a1d92a9a5ae5160adc3c22678ed` | `Run2Fingerprint` fails alone | run 2 fingerprint |
| 155 | `e62141c2bcfc4465c1b500040b7bdf34363d62f3` | `Run2Fingerprint` fails alone | run 2 fingerprint |
| 156 | `01f467d8c1bf9c9f3009e9efa8c81359aad3da31` | `Run2Fingerprint` fails alone | run 2 fingerprint |
| 157 | `244a4535f6a76031a1bfd5bd208a8112130bdb32` | `Run2Fingerprint` fails alone | run 2 fingerprint |
| 158 | `e67aa02d3b3d167a74f3bef2f825eab41542d8b9` | `Run2Fingerprint` fails alone | run 2 fingerprint |
| 159 | `22223a58374b1afb28f2e483d49776f9c0e7a083` | `Run2Fingerprint` fails alone | run 2 fingerprint |
| 160 | `822acaa5ec063ccac4c662983b91f230411cd82e` | `Run2Fingerprint` fails alone | run 2 fingerprint |
| 161 | `b0ee6248f0596c76b1fd34811d9e26840db68d66` | `PerfCounters` passes alone, `Run2Fingerprint` fails alone | timing test; run 2 fingerprint |
| 162 | `eddbaa4b6cdfd9e875d41f86076b4b2e080d96c1` | `Run2Fingerprint` fails alone | run 2 fingerprint |
| 163 | `009605caa10d2a2ee1da75ce30ba93a8f30aae44` | `Run2Fingerprint` fails alone | run 2 fingerprint |
| 164 | `ac6b2d6afee2f7fb0179391ef35716cc8dc9876a` | `Run2Fingerprint` fails alone | run 2 fingerprint |
| 165 | `67e340efa5df5faf9387baa1619dc8ac8ad20e16` | `Run2Fingerprint` fails alone | run 2 fingerprint |
| 166 | `ae08e8745e1b238f953ccc1c17e96dea1511e8d6` | `Run2Fingerprint` fails alone | run 2 fingerprint |
| 167 | `a33a0229d3e1b7bf1da6eb95657d89ad49ca5f82` | `Run2Fingerprint` fails alone | run 2 fingerprint |
| 168 | `55217b3b21a1b9b5f8d05eca5a83a301e0f13730` | `Run2Fingerprint` fails alone | run 2 fingerprint |
| 169 | `e6313148bbf6852f55b5171642cc780a4e20fabe` | `Run2Fingerprint` fails alone | run 2 fingerprint |
| 170 | `ee2c98f8e60ec541d1456bd77577d751b479dd9a` | `Run2Fingerprint` fails alone | run 2 fingerprint |
| 171 | `92cc148810c0886c6bba8ccd0813c7c81b7cdd70` | `Run2Fingerprint` fails alone | run 2 fingerprint |
