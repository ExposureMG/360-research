# SDK reference ↔ OpenXenos hardware data: cross-check

Compares [xenon-llvm-reference.md](xenon-llvm-reference.md) (compiled from the SDK docs) with `OpenXenos/isa/` as of commit `279d34c` (ISA table, `xenon_execution_model.md`, `SHORTCOMINGS.md`, `hwtest/findings/*`). OpenXenos was **read only**; nothing was written there. This file, the scripts in [crosscheck/](crosscheck/) and my edits to the reference doc all live in this folder.

## 0. How information flows (read before using Part B)

OpenXenos is clean-room: its numbers come from hardware, binaries and public sources, not from the SDK.

| Part | Direction | Safe for OpenXenos? |
|---|---|---|
| **A** | hardware numbers → corrections to the SDK reference | Yes. Nothing from the SDK goes back. |
| **B** | SDK claims → scrutiny of OpenXenos | **Only as a private oracle.** It states SDK values where that's the only way to show a difference. Don't paste them into OpenXenos or cite the SDK there. Your own evidence ladder counts a "document" as L4 corroboration; citing this one would make the SDK a provenance source. |
| **B2** | open OpenXenos questions the SDK happens to answer | **Pointers only.** I did not restate the answers. Derive them independently from hardware or the corpus first, then check against the reference doc section I name. |

Tags: **[agree]** both sources match, **[differs]** they disagree, **[untested]** the SDK makes a claim your data doesn't cover, **[note]** observation.

---

## 1. Scoreboard

| What was compared | Result |
|---|---|
| Vector latency class, 195 SDK per-instruction pages vs your per-mnemonic latencies | **191 equal**, 4 differ (`vexptefp`, `vlogefp` and their 128 forms: SDK 14, hardware 12) |
| SDK vector instruction pages vs hardware execute/trap | 235 of 235 mapped pages **execute** on the console |
| SDK's dropped legacy VMX list vs your `unsupported` list | **identical** (22 of 22) |
| Your vector entries with no SDK page (27) | exactly: the 22 dropped ops, `lvsl128`/`lvsr128` (illegal on hardware), `dst`/`dstst`/`dss` (SDK silent, execute on hardware) |
| SDK scalar instruction list (42) vs your table | all 42 present; all execute (alignment/trap behaviour aside); `mftb` has timing but no semantic record |
| VMX128 operand-tie formulas (5 FMA-family ops) vs SDK tie rules | all agree, and your result settles a self-contradiction in the SDK page (A2.5) |
| Numeric claims compared (Part A1) | 31 agree, none contradicted by clean data |
| Items where hardware corrects my reference doc (Part A2) | 11 |
| Part B | 7 differences or weak spots, 8 SDK claims your data doesn't cover |

---

## Part A: hardware → SDK reference

### A1. Where the two sources agree

| Quantity | SDK reference | OpenXenos hardware | |
|---|---|---|---|
| Simple integer ALU latency | 2 | 2.04 (35 instructions) | agree |
| Load → ALU use | 2 | use stall 1.92 | agree |
| Load → load/address use | 5 | 5.01 (pointer chase), 5.09 for `lbz` | agree |
| ALU → address | 5 | penalty 4.82 | agree |
| FP load → FP op | 2 | penalty 1.91 | agree |
| Scalar FP (add, mul, madd, `fres`, `frsqrte`, `fsel`, convert) | 10 | 10.00 | agree |
| Vector simple / permute | 4 | 4 (159 of 159 compared) | agree |
| Vector float (add, mul, madd, converts, rounds) | 12 | 12 (26 compared) | agree |
| `vrefp`, `vrsqrtefp`, `vmsum3/4fp128` | 14 | 14 | agree |
| Variable shifts/rotates microcoded | ≈16, 9 named instructions | 16, **same 9 instructions** | agree |
| Algebraic loads microcoded | yes (`lha`, `lwax`) | 21–24 cycles for all 7 forms vs 5 for `lhz` | agree |
| `andis.` microcoded; `add.`/FP record forms not | listed | `andi.`, `andis.` 14; `add.`→`bc` penalty 0.07 (not microcoded) | agree |
| `mullw` throughput | ≈ one per 10 cycles | 11 | agree |
| L1 D-cache | 32 KB, 4-way, 128 B lines | knee 32 KB, 4-way, 8 KB set stride | agree |
| L2 hit minus L1 hit | ≥ 41 | 40.17 | agree |
| L2 | 1 MB, 8 ways of 128 KB | knee 1 MB; 128 KB stride: 8 lines fit, 9 don't | agree |
| DRAM, sequential chase | ≈ 610 | 611.0; `ld_stream` 610.4 per line | agree |
| ERAT reach (64 entries × 4 KB = 256 KB) | step expected at 256 KB | "minor step ×1.35 between 256K and 512K" (45.2 → 57.6) | agree (explains your unexplained step) |
| Prefetch parallelism | ≤ 8 in flight, ≈ 6.6× speedup measured by SDK authors | `dcbt` stream 97 cycles/line = 610/6.3 | agree |
| Store → load (load-hit-store) | 40–80 cycles | 49–54 integer, 63 vector, 80 FP | agree |
| `lwsync` | measured 33–48 | 32.6 | agree (just under) |
| Mispredict | 23–24 | 24.7 (your own note: a lower bound at 50% mispredict) | agree |
| Taken-branch redirect | up to 8 cycles | empty loop 8.21 | agree |
| `fres` / `frsqrte` accuracy | 1/256, 1/32 | 2⁻⁸·⁰⁰, 2⁻⁵·⁰⁰ | agree |
| `vrefp` / `vrsqrtefp` / `vexptefp` accuracy | ~12 bits (6–12 quoted) | 2⁻¹³·⁰, 2⁻¹²·⁴, 2⁻⁸·⁵ | agree |
| FP alignment | float/double need 4-byte alignment; integer and VMX never fault | FP traps iff EA % 4 ≠ 0; 77 integer/vector mnemonics never trap | agree (also settles my "DWORD" reading) |
| Dot product: overflow gives NaN, sums are 1 ulp low, `vmsum3` ignores W | described | overflow → QNaN (3 cases), 1-ulp-low cases, W ignored | agree in kind |
| `vmaddfp128` dst = addend; `vmaddcfp128` dst = multiplicand; `vnmsubfp128` dst = addend | documented | `D=A*B+D`, `D=A*D+B`, `D=-(A*B-D)`, 256/256 lanes each | agree |
| `xdcbt` exists but was withdrawn from the SDK libraries | yes | executes; absent from XDK ≥ 2571 binaries | agree |
| Scalar FP slow on denormal and NaN operands | qualitative (≈60-cycle penalty; "NaN stalls") | ≈47 (denormal), ≈36 (NaN/inf) cycles per op | agree in kind (different quantity: SDK gives result-denormal, you measured operands) |
| ABI red zone | 1,344 bytes below `r1` | "XDK ABI has a red zone" (from `__savegprlr_14`) | agree |

### A2. Corrections to my reference doc (hardware wins)

| # | My statement | Hardware says | Action |
|---|---|---|---|
| 1 | `vexptefp`, `vlogefp` (+128) are estimate-class, 14 cycles | 12 (per-mnemonic, individually measured) | corrected in reference §4.3.3, Appendix A and CSV notes. The SDK page and white paper look wrong for these two. |
| 2 | VMX "flushes denormals to zero" as a hardware property | `VSCR[NJ]=0`: gradual, ≈58–67 cycles per op. `NJ=1`: flushed, full speed (1.14). The SDK's flush is a **mode the system sets**. | corrected §6. A baremetal target must set `NJ` itself. |
| 3 | Integer compare→branch ≈ 2 cycles [I] | **0** extra (`cmp>bc` penalty 0.00; all 8 compare forms) | corrected §3.6 |
| 4 | ABI: VR14–VR63 volatile, millicode text says VR14–VR31 | non-volatile v14–v31 and v64–v127, volatile v32–v63 (from the helper tables in real XDK binaries) | §7.1 [!] resolved toward the millicode text; my table row was the typo |
| 5 | `vnmsubfp128`: page contradicts itself ("tied to multiplicand" vs "tied to addend") | `D = -(A*B - D)`: the **addend** is tied | §4.3.2 [!] resolved |
| 6 | `vdot3fp`/`vdot4fp` are 32-register forms of `vmsum3/4fp128` [I] | OpenXenos has no such encoding; same intrinsic as the 128 forms | probably assembler aliases; CSV note updated |
| 7 | `mfspr`/`mfcr` are non-pipelined | LR/CTR moves ≈3 cycles; XER 25; CR 26–27; TB 44; FPSCR 30; VSCR 40 | refined §3.5: only some moves are slow |
| 8 | Estimate instructions issue 1 per cycle (pipelined) | `vrefp`/`vrsqrtefp` issue every **2.07** cycles | added to §3.5 |
| 9 | Integer multiply/divide latency [G] | `mulli` 8; `mullw/mulhw` 11; `mulld/mulhd` 14–15; `divw/divd` 18 minimum, **32–48** (`divw`) and **46–78** (`divd`) with wide operands; `fdiv` 74, `fsqrt` 84 | filled in new §14 |
| 10 | ALU → store-data is 2 cycles with forwarding | your adjacent `add; stw` shows ≈5 | left as-is; see B1.6 (conditions differ) |
| 11 | `dcbz` clears a 32-byte block and is deprecated in favour of `dcbz128` | `dcbz` clears **128 bytes** (24 of 24 cases), same as `dcbz128` | corrected §8. The size is probably a mode set by firmware (XeLL/libxenon vs the Microsoft kernel), so don't assume either on baremetal. |

Also now measured and not in the SDK (all added to §14 of the reference doc): `sync` 165, `ptesync` 164 (= `sync`), `lwsync` 33; `eieio` 92 alone; `isync` 28; link stack depth 4; GPR load-use 5; `lwarx`/`stwcx.` ≈ 65–80 cycles alone; `dcbf` 92 resident, 832 after a store.

---

## Part B: SDK claims vs OpenXenos (private oracle; see section 0)

### B1. Differences and weak spots

**B1.1 Throughputs below one per cycle (≈ certainly an artifact).**
The SDK states each integer, load/store and branch pipe takes at most one instruction per cycle (structural hazards). Your tables carry values below that: `lwz` 0.76, `cmp*` 0.76, `crand`… 0.70–0.76, `mtlr` 0.70, `mtctr` 0.73, `mtxer` 0.76, `mcrf` 0.70, `mtfsf` 0.56. These are the *same* number as your loop staircase: from the `branch` plan, per-iteration cost with 0/4/8/12/16 independent `addi` is 8.2 / 11.4 / 14.0 / 17.2 / 20.1, so the 8→16 slope is (20.1 − 14.0) / 8 = **0.76**. Every "0.7–0.76" is the slope of that staircase, not a property of the instruction, and it is not specific to loads (`SHORTCOMINGS` §8.2 mentions only loads).
Why the staircase is plausible *as structure*: steps of about 3.2 cycles per 4 instructions line up with the 4-instruction, 16-byte fetch group, and the 8.2-cycle floor with the 8-cycle taken-branch redirect. So the effect is probably front-end quantisation of a `bdnz` loop, not noise. Where your `issue` uses a different method (16 overlapped chains) you get 0.98 for `add`, which respects the bound.
Suggestion: add an impossibility rule to `verify_isa.py` (≥ 1.0 cycle/instruction for any single-pipe class), and re-time throughput with a method that doesn't depend on a short `bdnz` loop (straight-line code between two `mftb`, ≥ 64 instructions, 16-byte aligned).

**B1.2 Branch density.**
The SDK says correctly predicted not-taken branches are free. Your stream of `bc` not taken costs 3.9–5.0 cycles each (tput 3.9, branch plan 4.5; taken-to-next 5.0) and shows **zero overlap with any other class** (`int+br` 0.00, `lwz+br` 0.00, vector+`br` ≈ 0). So "free" holds only for sparse branches; a dense stream behaves like a ~4-cycle dead time per branch. A scheduling model needs a minimum branch spacing.

**B1.3 Loads paired with lower-set VSU ops don't overlap at all.**
Prediction from the SDK's structure: a load (address pipe) and a lower-set vector op (`fp`, `vint`, `vlogic`, `vfp`) use different pipes and should pair. Measured: all 12 `lwz/ld/lbz` + lower-set pairs are confident, cost 2.40–2.41 cycles per pair against ≈2.19 serial (overlap −0.21 to −0.28, i.e. slightly *worse* than serial, identically for every combination). The four `stw` + lower-set pairs go the other way (1.9–2.0 per pair, faster than `stw` alone at 2.96), which is the store-throughput anomaly in `SHORTCOMINGS` §8.2. By contrast the same-set pairs match the SDK's setup-set model: lower–lower serial (9 of 10 confident), upper–upper serial (3 of 3). Integer + lower-set pairs reach only the common 0.65 ceiling (B1.1). Candidate cause to test for the loads: the A/B-slot rule (dual issue only for an even-word instruction with the *following* odd-word instruction). Your kernel generator doesn't control the parity of the first instruction; run each pairing both ways (kernel starting at an even and an odd word).

**B1.4 Multiply/divide blocking.**
The SDK says non-pipelined integer instructions stall issue of everything behind them. Your tput variant has `int+div` overlap 1.03 and `int+shift` 1.00 (the other instruction fully hidden inside the divide/shift), although tagged non-confident. If real, mul/div/variable-shift occupy a unit but do not block younger independent instructions. This changes how to schedule around them, so it's worth a clean re-run (independent filler, not the dependent `nop`).

**B1.5 `dcbz` size, and the store-stream saving.**
SDK: plain `dcbz` clears a 32-byte block (fetching the 128-byte line if absent), superseded by `dcbz128`. Your semantic data: `dcbz` clears 128 bytes in 24 of 24 cases, identical to `dcbz128`. So on this console under XeLL the size is 128; the SDK describes the retail configuration. (Which HID/HV bit selects it isn't in either source; worth recording with the console state.) Consequence for your store stream: `st_stream_dcbz_stvx` (plans.py:1377) uses `dcbz`, which on this console zeroes the whole line, so it is a fair test of the technique. It measured 148.9 vs 209.6 cycles per line without it, a 29% saving, while the SDK authors report 200–300% for zero-then-write on full lines. The report says "no clear saving", which sits uneasily with a 29% gain. Re-run with `dcbz128`, a prefetched variant and a threshold the 29% would pass, and compare both to the 2–3× the SDK claims.

**B1.6 ALU → store-data latency.**
SDK: 5 cycles, dropping to 2 when a store issues at least two cycles after the add (forwarding into the store pipe). Your `add>stw` (adjacent) gives dep 6.28 vs independent 3.45, penalty 2.83. The same kernel shape gives `addi>lwz_addr` 6.28, where 5 is expected, so the adjacent store behaves as if it takes the 5. Your kernel only tests the adjacent form; the SDK's condition is about distance. Test `add ; filler ; filler ; stw` (dep vs ind): if forwarding works the penalty vanishes, otherwise it is ≈ 2.

**B1.7 `vexptefp`/`vlogefp`.**
Here the SDK is likely wrong (A2.1). No action for OpenXenos.

### B2. Open OpenXenos items the SDK happens to answer (pointers only)

| Your open item | Where it is | Where the SDK material is in my reference |
|---|---|---|
| Summation order and rounding of `vmsum3fp128`/`vmsum4fp128` | `SHORTCOMINGS` §8.6 "Wrong in the table" | §4.3.4 |
| Vector argument/return registers, call-site conventions | `SHORTCOMINGS` §8.7, §8.10 item 8 | §7.1, §7.2 |
| Which XDK ABI items GCC doesn't match (red zone, v14–v19) | `SHORTCOMINGS` §8.7 | §7.1, §7.3, §7.4 |

I'd suggest finishing the corpus-based derivation first and using those sections only to check the result.

### B3. SDK claims not covered by your data (candidate tests)

| # | SDK claim | Your status |
|---|---|---|
| 1 | Record forms of *logical/shift/extend* ops are microcoded (`exts.`, `cntlz.`, `eqv.`, `cntlzd.`, `rldimi.`); `add.`, `subf.` and FP record forms are not | **Untested.** `latency.json` has base forms plus `addic.`, `andi.`, `andis.`. This matters for LLVM: compare-folding emits `and.`, `or.`, `rlwinm.` etc. If they cost ≈14 cycles and block both threads, the subtarget should disable that folding. Highest-value test in this list. |
| 2 | Integer access that crosses a **32-byte** boundary is retried in microcode, +40–60 cycles; a `double` that is 4- but not 8-aligned crossing 32 B likewise | `SHORTCOMINGS` §8.6 plans 128-byte and 4 KB boundaries; the boundary in question is 32 B |
| 3 | False load-hit-store when addresses differ by a multiple of 4 KB (only the low 12 bits are compared) | `stw>lwz_sameline`/`otherline` don't test 4 KB aliasing |
| 4 | `mftb` 64-bit read can return a wrong value (4-cycle lag between halves); `TBL` alone is safe | untested (mftb is only timed: 44 cycles) |
| 5 | Hardware thread priority: equal priority alternates dispatch slots; unequal gives the higher thread 31 of 32. An interrupt on a thread resets it to medium, so a spin loop must re-set low each iteration | Narrows your caveat rather than adding one. The libxenon idle loop has `or r1,r1,r1` **inside** the polling loop (startup_from_xell.S, label `1:`), i.e. the SDK's recommended pattern, so the sibling is at low priority nearly all the time and your thread should own about 31 of 32 dispatch slots: close to single-thread. A priority reset is therefore an unlikely cause of the "slow first job of a pair" regime. That leaves warm-up effects (cold caches/predictor/ERAT on the first kernel of a pair), which your own analysis excluded only for I-cache capacity. A calibration kernel at the start of each job (1.0 cycles per independent `add`) would show which regime a job ran in. |
| 6 | `lvxl`, `lvxl128` and the element loads `lvebx/lvehx/lvewx` behave exactly like `lvx` | your semantic plan ran them (24 cases each) but the repo copy keeps only status, not the per-case values, so I could not check |
| 7 | `lvlx/lvrx/stvlx/stvrx` and `dcbz128` raise alignment exceptions on non-cacheable memory | needs an uncached mapping |
| 8 | `lwarx`/`stwcx.` has implementation bugs (the SDK gives no detail) | your single-thread cost numbers (≈65–80 cycles) can't see ordering bugs; `SHORTCOMINGS` §8.5 already asks for litmus tests |

### B4. Notes that need no action

- **Two pairing variants.** I only compared the `tput` variant to the SDK's structure. The `chain` variant measures latency hiding, so "together" there says nothing about issue structure.
- **Random-chase memory is ≈27 cycles above sequential** (637.7 vs 611.0): this is likely translation cost, not DRAM. Worth recording both.
- **L2 conflict stride.** Your analyser's "8-way, set stride 64K" is the defect you already list in `SHORTCOMINGS` §8.9; the SDK's 8 × 128 KB geometry agrees with your 128 KB-stride reading.
- **`nop` class.** `ori 0,0,0` reads and writes r0, hence the 2.0-cycle "alone" figure; you have already noted it as a dependent chain.
- **PMC counters.** The SDK names a CPU counter set (IS2 stall cycles, load-hit-store count, flush counts, VIQ stalls, microcode instruction count) but gives no SPR encodings, so it can't substitute for your SPR map.

---

## 3. What I changed in my own material
- [xenon-llvm-reference.md](xenon-llvm-reference.md): corrections A2.1–A2.8 in place, new §14 "Hardware cross-check" with the measured numbers that fill gaps.
- [xenon-vmx128-instructions.csv](xenon-vmx128-instructions.csv): notes on the four disputed rows.
- [crosscheck/](crosscheck/): the scripts behind Part A1 and B1.3, runnable against the read-only repo.
