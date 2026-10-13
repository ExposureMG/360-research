# Xbox 360 "Xenon" CPU: Reference for an LLVM Subtarget

Compiled from the Xbox 360 SDK documentation in this folder (Sept 2013 XDK, doc build 21256.3).
Companion files: [xenon-vmx128-instructions.csv](xenon-vmx128-instructions.csv) (all 245 VMX128 instruction pages, one row each) and [xenon-openxenos-crosscheck.md](xenon-openxenos-crosscheck.md) (comparison with hardware measurements from `OpenXenos/isa`).

**Revision note:** after the hardware cross-check, 11 statements here were corrected in place (flagged "hardware" or pointing to §14). Section 14 collects hardware-measured numbers that fill gaps. Those come from OpenXenos (hardware testing, clean-room), **not** from the SDK; cite OpenXenos or your own measurements for them, never this document.

---

## 0. Read this first

**Licensing.** The source docs are covered by the *Xbox 360 Development Kit License* (see `.rights`), which allows internal use only for building and testing Xbox 360 titles. Your target (LLVM) is open source. This document is a private working summary written in my own words, with sources named so you can check each fact yourself. Treat it as a reference for *cross-checking*, not as text to paste into patches, TableGen files, commit messages or comments. Where possible, re-derive numbers from hardware measurement (section 12) or independent sources and cite those. I'm not a lawyer; how you handle provenance is your call.

**Tags used throughout**

| Tag | Meaning |
|---|---|
| **[D]** | Stated directly in the docs |
| **[I]** | Inferred or derived by me from documented facts (derivation given) |
| **[!]** | The docs contradict themselves, are ambiguous, or describe an erratum |
| **[G]** | Gap: the docs don't say |

**Scope.** The docs are a *game-developer* SDK manual. They describe timing, ABI, intrinsics and semantics well. They contain **no instruction binary encodings, no system-level/privileged architecture (MSR, SPRs, hypervisor, interrupts, SMT control) and no full PowerPC instruction list.** Section 11 lists the gaps.

Sources are cited as page names inside `xbox360sdk.chm` (e.g. `xenon_cpu_pipelines.htm`). Section 13 is a source index.

---

## 1. One-page cheat sheet

| Topic | Fact | Tag |
|---|---|---|
| CPU | 3 identical 64-bit PowerPC cores, 3.2 GHz, 2 hardware threads per core (6 total), big-endian | [D] |
| Execution | **In-order**, 2-wide dispatch in aligned instruction pairs; the compiler does all scheduling | [D] |
| Pointers / ABI width | 64-bit registers but **32-bit pointers**; `int` = `long` = `void*` = 4 bytes; addresses are truncated to 32 bits | [D] |
| Registers | 32×64-bit GPR, 32×64-bit FPR, **128×128-bit VMX registers** (per thread), LR, CTR, XER, CR, FPSCR, VSCR | [D] |
| Scalar FP | single and double; all ops latency **10**, 1/cycle; `fdiv`/`fsqrt` non-pipelined; has `fsqrt`, `fsqrts`, `fres`, `frsqrte`, `fsel` | [D] |
| Integer | ALU latency **2**; load→ALU 2; anything→address **5**; `mullw` issues once per ~10 cycles; mul/div non-pipelined and stall both threads | [D] |
| Vector | float 12, estimates 14, dot product 14, simple 4, permute 4, load 2 | [D] |
| Reg-file transfers | GPR↔FPR↔VR **only through memory** (load-hit-store, 40–80 cycles) | [D] |
| Branch mispredict | 23–24 cycles; FP/vector compare → branch usually **flushes** the thread | [D] |
| Caches | 128-byte lines everywhere; L1I 32 KB 2-way; L1D 32 KB 4-way write-through/no-write-allocate; L2 1 MB 8-way shared write-back | [D] |
| Memory latency | L1 hit 5 (2 visible), L2 hit ≥41, DRAM ≈610 cycles | [D] |
| Barriers | `lwsync` = acquire/release fence (not store→load); `sync` = full | [D] |
| Atomics | `lwarx`/`stwcx.` exist (reservation = 1 cache line) but the docs say the CPU implementation has bugs (undescribed) | [!] |
| No TOC | No TOC/function descriptors; globals via `lis`+`lo16`; function pointers are raw addresses | [D] |
| u64↔FP | **No hardware** unsigned-64 conversions; compiler calls `__u64tod` etc. | [D] |
| Denormals | FPU supports them (≈47–60 cycles per denormal op); VMX flushes them to zero **only when `VSCR[NJ]` is set** (the SDK's system sets it; with `NJ=0` VMX is gradual and ≈58–67 cycles per op) | [D], corrected by hardware (§14) |
| Hardware errata | 64-bit `mftb` read race; `lwarx`/`stwcx.` bugs; `xdcbt` removed for bugs | [D] |

---

## 2. Target description

### 2.1 Core and ISA level
- Custom IBM 64-bit PowerPC. The integer and scalar FP units are described as virtually unchanged from standard PowerPC; the scalar FPU includes the *optional* instructions square root, reciprocal-square-root estimate, reciprocal estimate and select. The integer unit has an *enhanced prefetch instruction*. [D] `xenon_cpu_overview.htm`
- Reference ISA text named by the docs: *PowerPC Microprocessor Family: Programming Environments Manual for 64 and 32-Bit Microprocessors, Version 2.0* and the *AltiVec PEM* (referred to as `ppc_pem.pdf`; **not included** in this folder). [D] [G]
- Vector unit: **VMX128**, a custom extension of VMX/AltiVec (section 4.3). Single-precision only: **no double-precision vector math**. [D]
- The CPU runs in 32-bit address mode: addresses are truncated to 32 bits. [D] `callstd_introduction.htm`

### 2.2 Register files
| File | Size | Notes |
|---|---|---|
| GPR | 32 × 64-bit | per hardware thread |
| FPR | 32 × 64-bit | per hardware thread |
| VR | **128 × 128-bit** | per hardware thread; most non-128 instruction forms only reach VR0–VR31 (section 4.3.2) |
| LR, CTR, CR, XER, FPSCR, VSCR | | per thread |
| Time base | 50 MHz; one tick = 64 CPU cycles | read with `mftb` (see erratum, section 9) |

### 2.3 Data layout facts
| Item | Value | Tag |
|---|---|---|
| Endianness | big-endian; vector element X (word 0) is the most significant word and sits at the lowest address | [D] |
| `char`/`short`/`int`/`long`/`long long` | 1 / 2 / 4 / 4 / 8 bytes | [D] |
| pointer | 4 bytes | [D] |
| `float`/`double` | 4 / 8 bytes; natural alignment | [D] |
| vector | 16 bytes, 16-byte aligned | [D] |
| Struct padding | members at natural alignment; struct size rounded up to the largest member alignment | [D] |
| Heap allocations | 16-byte aligned by default | [D] |
| Stack locals | max supported alignment is 16 bytes; larger `align()` on locals is ignored | [D] |
| Bit-field order | **msb-to-lsb** is the default (conventional PPC); an `lsb_to_msb` mode exists via pragma | [D] |
| GPU-visible data | must be at least 4-byte aligned | [D] |

**LLVM notes** [I]: this is a 64-bit-register, **ILP32** configuration (pointers 32-bit, native integer widths 32 and 64). A candidate data-layout shape is big-endian, `p:32:32`, `i64:64`, `n32:64`, 16-byte vector alignment; verify against what the LLVM PPC backend actually accepts before relying on it. Because addresses are truncated to 32 bits, code never needs to zero- or sign-extend a pointer before using it as an address. [D→I]

### 2.4 Instruction inventory the docs confirm
The docs list only the scalar instructions exposed as compiler intrinsics, plus the whole VMX128 set. Everything else is "standard PowerPC" by reference and is **not enumerated** [G].

**Documented scalar instructions** (`atoc_ref_ppc.htm`, `dev_compiler_ppc_intrinsics.htm`):

- Count leading zeros: `cntlzw`, `cntlzd`
- 64-bit multiply high: `mulhd`, `mulhdu`
- FP math: `fabs`, `fsqrt`, `fsqrts`, `fres`, `frsqrte`, `fsel`
- FP/int conversion: `fcfid`, `fctid`, `fctidz`, `fctiw`, `fctiwz`, `stfiwx`
- Byte-reversed memory: `lhbrx`, `lwbrx`, `sthbrx`, `stwbrx`
- Update-form memory: `lbzu`, `lhzu`, `lwzu`, `lfsu`, `lfdu`, `stbu`, `sthu`, `stwu`, `stfsu`, `stfdu`
- Cache control: `dcbt`, `dcbf`, `dcbst`, `dcbz` (SDK: 32-byte, deprecated; **hardware under XeLL: clears 128 bytes**, §14), `dcbz128`
- Ordering: `sync`, `lwsync`, `eieio`
- Time base: `mftb`
- Traps: `tw`, `td`, `twi`, `tdi`
- Hardware thread priority: low/medium/high (exposed as intrinsics; the encodings are not given [G])

**Evidence of what is *not* in hardware:**
- Unsigned 64-bit ↔ float/double conversions are done by library helpers `__u64tod`, `__u64tos`, `__dtou64`, `__stou64`. [D] `callstd_floating_point_emulation.htm`
- No VMX integer multiply/multiply-sum (full dropped list is in `vmx128_overview_unsupported.htm`; includes `vmuleub`…`vmulesh`, `vmuloub`…`vmulosh`, `vsum*`, `vmhaddshs`, `vmhraddshs`, `vmladduhm`, `vmsum*bm/hm/hs`). [D]
- No vector floating-point divide, square root, negate, absolute value or register-move instruction: the original compiler used `vxor`/`vandc` with a sign mask and `vor vD,vS,vS` for moves, and `vspltisw 0` for zero. [D]

Instructions the docs never mention (word-match search): `popcnt*`, `fcfidu`, `fctidu`, `ldbrx`/`stdbrx`, `lfiwax`/`lfiwzx`, `divwu`, `extsw`, `prtyw`, `mtfsf`, `mffs`, `icbi`, `mtmsr`/`mfmsr`. Absence from the docs is **not** proof of absence in hardware [G]. Hardware for any of these must be confirmed elsewhere.

---

## 3. Scheduling model

Primary sources: `xenon_cpu_pipelines.htm` (white paper plus Figure 1, `cpu_pipeline.jpg`), `xbox_360_cpu_best_practices.htm`, `xenon_cpu_caches.htm`, `dev_compiler_qxstalls.htm`, and the Latency table on each `vmx128_*.htm` page.

### 3.1 Machine structure
1. **Fetch/buffer** (6 stages): fetches 4 instructions per 16-byte aligned block (takes 4 cycles), per-thread buffers (5 buffers × 4 instructions per thread), the two threads alternate cycles. Fetch continues during stalls so it can catch up after branches. [D]
2. **Decode / dependency check** (6 stages, last stall point **IS2**): takes one **8-byte-aligned pair** of instructions from *one* thread. Integer, branch and load/store instructions are checked and issued from IS2 **in strict program order**. After IS2 there are no more stall points for these. [D]
3. Vector and scalar-FP instructions go IS2 → **VIQ** (8 stages) → stall point **VQ8** → 4 setup stages → execute. Hazards for VSU instructions are resolved at VQ8, not IS2; VQ8 data-hazard stalls can reach 14 cycles. [D]
4. Both hardware threads share everything after the pair buffers. The two threads cannot pass each other: one stalling stalls the other, **except** where the hardware uses a *flush* instead (section 3.8). [D]
5. A core can have **over 80 instructions in flight**. The minimum fetch-to-result time of an integer add is 19 cycles. [D]

### 3.2 Pipeline table
Stage numbers are the diagram's coordinate system (0–40). "Needs operands at" is the stage where the unit reads its inputs; "result at" is the stage where a dependent instruction's operand stage must be at or after.

| Pipeline | Needs operands at | Length | Result at | Source / tag |
|---|---|---|---|---|
| Branch | 17 | 6 cells in figure | CR after its 1st stage, CTR after its 2nd | [D] text; 6 from figure [I] |
| Integer | 17 | 2 | 19 | [D] |
| Address generation + int load/store | **14** | 5 | 19 (integer load result) | [D] |
| Vector/scalar **load** | 27 | 2 | 29 | length from figure [I]; 2-cycle result [D] |
| Vector/scalar **store** | 27 | 10 (figure) | 37 | [I] (from figure) |
| Vector permute | 27 | 4 | 31 | [D] latency 4 |
| Vector simple | 27 | 4 | 31 | [D] latency 4 |
| Scalar float | 27 | 10 | 37 | [D] latency 10 |
| Vector float | 27 | 12 (14 for estimates) | 39 (41) | [D] |
| Dot product | 27 | 14 | 41 | [D] latency 14 |

**Setup stage sets** (a hidden structural resource): VSU instructions pass through one of two 4-stage setup blocks, each accepting **one instruction per cycle**. [D] `xenon_cpu_pipelines.htm`, `dev_compiler_qxstalls.htm`

| Set | Feeds |
|---|---|
| **Upper (U)** | vector/scalar load, vector/scalar store, vector permute |
| **Lower (L)** | vector simple, scalar float, vector float, dot product |

So two VSU instructions from the *same* set cannot start in the same cycle, even though they use different execution pipes (the paper's Example 12: `vmaddfp` + `fmul` conflict). [D]

### 3.3 Latency = result stage − operand stage
Latency between two dependent instructions is the producer's *result* stage minus the consumer's *needs operands at* stage. [D] (this is the paper's own method). I checked it against all six worked examples in the paper:

| Case | Calculation | Result | Paper says |
|---|---|---|---|
| `add`→`add` | 19 − 17 | 2 | 2 ✔ |
| `vmaddfp`→`vmaddfp` | 39 − 27 | 12 | 12 ✔ |
| `lwzx`→`add` | 19 − 17 | 2 | 2 ✔ |
| `add`→`lwzx` (address) | 19 − 14 | 5 | 5 ✔ |
| `add`→`stwx` (data) | 19 − 14 | 5 (2 with forwarding) | 5 / 2 ✔ |
| `fcmpo`→`bge` | 37 − 17 | 20 → **flush instead** | 20, flush ✔ |

**Derived latency matrix** [I] (cycles from producer issue to earliest consumer issue). Rows are producers, columns are the consumer's operand use.

| Producer ↓ / Consumer → | Integer ALU / branch operand | Load/store address | Integer store data | Vector/FP operand | FP store data |
|---|---|---|---|---|---|
| Integer ALU (add, xor, rlwinm, cntlzw …) | 2 | 5 | 2 (with forwarding; 5 without) | — | — |
| Integer load (lwz …) | 2 | 5 | 5 | — | — |
| FP/vector load (lfs, lfd, lvx …) | — | — | — | 2 | — |
| Scalar FP (fadd, fmul, fmadd …) | — | — | — | 10 | **0** (forwarded) |
| Vector float | — | — | — | 12 (14 estimates) | 12 (no vector forwarding) |
| Vector simple / permute | — | — | — | 4 | 4 |
| Dot product | — | — | — | 14 | 14 |

Cross-register-file moves do not exist; they go through memory (section 3.7).

### 3.4 Issue rules
- Issue happens per aligned **pair**. If the first instruction stalls, both wait; if only the second stalls, the first issues and the second retries next cycle. Until both have issued, nothing else (including the other thread) issues. [D]
- **Slot A** = instruction at an address that's a multiple of 8; **slot B** = the other. Dual issue is only possible for an A-slot instruction with the B-slot instruction that *follows* it; a B-slot instruction never pairs with the next A-slot instruction. [D] (`/QXSTALLS`)
- Structural hazard: only **one integer instruction per cycle** (two adds in a pair serialise). [D] An integer op and a load/store can dual-issue (different pipes). [D] `add`+`lwz` example.
- Vector/scalar loads and stores occupy **both** the address-generation/LS pipe (at IS2) *and* a VSU load/store pipe via VIQ. Model them as two resource uses. [D]
- Dependent instructions are stalled at IS2 just long enough that the operands are ready when needed; the second of a dependent `add` pair issues from IS2 two cycles after the first. [D]
- A VSU instruction reaches VQ8 at least 8 cycles after its IS2 issue (VIQ depth). [D]
- A pair where the second instruction overwrites a register the first one reads (write-after-read, e.g. `add r3,r3,r4` + `lwz r4,0(r5)`) co-issues without trouble. [D]

### 3.5 Non-pipelined and microcoded instructions
**Non-pipelined** (block both threads until done): integer multiply (`mullw`, `mulld`, …), integer divide (`divd`, `divdu`, …), `fdiv`, `fsqrt`/`fsqrts`, and moves *from* the condition register or SPRs. Integer ones stall at IS2; FP ones stall at VQ8. [D] The performance-counter docs separately describe integer multiply, integer divide and `mfspr` as "double issue" instructions (they count twice). [D] **Hardware refinement (§14):** SPR/CR moves are not uniformly slow (`mflr`/`mfctr` ≈3 cycles; `mfxer` 25, `mfcr` 26, `mftb` 44), and the estimate instructions `vrefp`/`vrsqrtefp` issue only every ≈2 cycles rather than every cycle.
- `mullw`: one issue per ~10 cycles. [D] All other latencies and throughputs are **[G]**.

**Microcoded**: 11 cycles to fetch the microcode, then the expanded instructions run. Everything behind it (both threads) is blocked. [D] The documented microcoded set:

| Group | Examples | Notes |
|---|---|---|
| Some record-form (`.`) instructions | `exts.`, `cntlz.`, `andis.`, `eqv.`, `cntlzd.`, `rldimi.` | `add.`/`subf.` and FP record forms are **not** microcoded |
| Algebraic loads | `lha`, `lwax` | |
| Load/store multiple and string | `lmw`, `stmw`, `lswi`, `stswi` | |
| Variable-amount shifts/rotates | `slw`, `srw`, `sraw`, `sld`, `srd`, `srad`, `rldcl`, `rldcr`, `rlwnm` | ≈ **16-cycle latency, 1 per 16 cycles**; immediate forms are 2-cycle |
| Integer loads/stores crossing a 32-byte boundary | | retried in microcode, **+40–60 cycles** |
| Misaligned `double` loads/stores crossing 32 B | | same |

The original compiler avoided microcode when optimizing for speed and used it when optimizing for size. [D] **LLVM note** [I]: the generic PPC prologue/epilogue code uses `stmw`/`lmw` on 32-bit PowerPC; that must be disabled here (except at `-Oz`).

### 3.6 Branches
| Event | Cost | Tag |
|---|---|---|
| Correctly predicted not-taken | 0 | [D] |
| Correctly predicted taken | 0–8 cycles (6–7 typically wasted; hidden if fetch is ahead) | [D] |
| Unconditional branch | always predicted correctly | [D] |
| Mispredict (either direction) | **23–24 cycles** | [D] |
| Branch on FP or vector compare | CR arrives ~20 cycles late → the thread is **flushed and the branch re-issued** (possibly repeatedly) | [D] |
| `bctr` (indirect/virtual calls, jump tables) | usually mispredicted | [D] |

- Predictor scans each fetched 4-instruction block (4 cycles); branch-target alignment on a 16-byte boundary and taken branches at the end of a 16-byte block minimise cost. [D]
- Predictor components named: branch history table, function return stack, static prediction. Sizes, hint-bit behaviour and `bdnz` handling are **[G]**.
- Integer compare→branch latency isn't stated; the stage model suggested ≈2 [I], but **hardware measures no extra cycles** for any `cmp`/`cmpi`/`cmpl`/`cmpli`→`bc` (§14). `mtcrf` runs in the integer pipe; `crand`/`mfcr`-type CR ops run in the branch pipe. [D]
- Compiler guidance: replace FP/vector-compare branching with `fsel`, `vsel`, `vmaxfp`/`vminfp` and masks. [D]

### 3.7 Memory timing
| Event | Cycles | Tag |
|---|---|---|
| L1 D-cache hit (load result) | 5 (≈2 visible to an ALU consumer) | [D] |
| L1 I-cache latency | 4 (normally hidden) | [D] |
| L1 miss, L2 hit | ≥ **41** (core 0; a few more for cores 1–2); the dependent instruction triggers a **flush** and re-issues on arrival | [D] |
| L2 miss → DRAM | ≈ **610**; an RC machine is occupied ≈570 | [D] |
| ERAT / L1-miss penalties | ≈ 40 | [D] |
| **Load-hit-store** | a loaded value is not usable until the store drains (~20+ cycles); penalty **40–80** and a flush. Detection compares only the low 12 address bits, so addresses equal mod 4 KB also trigger it | [D] |
| Non-cacheable read | about as slow as an L2 miss | [D] |
| `dcbz128` | ~140 cycles before writes to that line proceed | [D] |
| `lwsync` | measured **33–48 cycles** | [D] |
| `InterlockedIncrement` | ≈250 cycles | [D] |
| Uncontended critical section enter+leave | ≈700 cycles | [D] |

The compiler schedules assuming L1 hits. [D] Per core: **8 outstanding reads/prefetches**; the L2 handles 8 misses (8 "RC machines"). A full queue also causes a flush. [D]

Flush triggers named: L1/L2 miss use, load-hit-store, store queue full, ERAT miss (including on prefetch), FP/vector compare→branch. [D]

### 3.8 SMT interaction
- Thread switching happens after an aligned pair. Dependencies *between* pairs can be hidden by the other thread; dependencies *within* a pair cannot, so avoid placing dependent instructions in one aligned pair. [D]
- Per core there are 32 dispatch slots. Equal priority: alternate slots. Unequal priority: high thread 31 of 32, low thread 1 of 32. Default is medium. High priority is discouraged because it can starve system work. [D]
- Instruction-cache contention occurs when the threads run different code. [D]

### 3.9 Suggested mapping to an LLVM `SchedMachineModel` [I]
This is my design sketch from the facts above, **untested**, not something the docs provide.

- `IssueWidth = 2`, `MicroOpBufferSize = 0` (in-order), `MispredictPenalty = 23`.
- **Stage model via `ReadAdvance`**: give integer ALU and load results a write latency of **5** (the load/store-pipe distance), then give consumers that enter the integer pipeline a `ReadAdvance<…, 3>`. Address operands get no advance. This reproduces ALU→ALU 2, load→ALU 2, ALU→address 5, load→address 5, and ALU→store-data 2 if the store-data read also gets the advance.
- VSU instructions use plain write latencies 2 / 4 / 10 / 12 / 14.
- Resources: Integer, Branch, LS-agen, VS-Load, VS-Store, VPerm, VSimple, SFloat, VFloat, VDot, plus two issue-limiting resources **SetupUpper** and **SetupLower** (1 per cycle each). A vector load/store consumes LS-agen *and* VS-Load/Store.
- Non-pipelined ops: model as occupying their pipe for N cycles *and* a "blocks everything" hazard; microcoded ops similarly with an 11-cycle front penalty.
- Pair-based issue and A/B slot rules do not fit a plain resource model. Use a custom `ScheduleHazardRecognizer` that tracks the 8-byte pair position, the same way the Power dispatch-group recognizers do. Dependent instructions inside one pair are the thing to avoid.
- Treat FP/vector compare→branch as a very high latency (≈20) hazard or a flush cost rather than a normal dependency.
- Load-hit-store: model as an expensive memory dependency between a store and a load to a possibly aliasing address; cross-register-file copies are always store+load pairs.

---

## 4. Instruction set notes

### 4.1 Cost-sensitive scalar operations (cost-model checklist) [D unless noted]
| Operation | Cost note |
|---|---|
| Variable shifts/rotates | ≈16 cycles, microcoded; immediate forms 2 |
| Integer multiply/divide | non-pipelined, stall both threads |
| FP divide/sqrt | non-pipelined, stall both threads |
| Integer divide by zero | **no trap; result undefined**: the compiler inserted checks unless told otherwise |
| GPR↔FPR↔VR transfer, FP↔int conversion via GPR | load-hit-store, 40–80 cycles |
| Scalar FP `fcmp` + branch | flush |
| `bctr` | mispredict-prone |
| Unaligned integer access crossing 32 B | +40–60 |
| Unaligned float/double | alignment *exception* unless ≥4-byte aligned [I from "DWORD-aligned"] |
| Stack traffic mid-function | likely load-hit-stores; the CPU has 192+ registers per thread, so spilling is a last resort |
| Boolean-heavy `&&` / `or` chains | multiple branches; prefer arithmetic selects (original compiler weak here) |

### 4.2 Conversions and rounding (scalar)
- `fctid`, `fctidz`, `fctiw`, `fctiwz` write the integer result **into an FPR** (move to a GPR is via memory, or use `stfiwx` to store the low word directly). [D]
- `fcfid` converts a signed 64-bit integer held in an FPR; rounding follows FPSCR RN. [D]
- `__frnd` = `fctid` + `fcfid` pair. [D]
- `fres` accurate to 1 part in 256; `frsqrte` accurate to 1 part in 32. [D]
- `fsel`: result = (A ≥ 0) ? B : C; NaN in A selects C; +0 and −0 compare equal. `fabs` is preferred over `fsel` for absolute value. [D]
- `fmadd`/`fmadds` are generated by default for `a*b+c` (contraction on by default; pragma to disable). [D]

### 4.3 VMX128

#### 4.3.1 Overview
- 128 registers × 128 bits, shared integer/float interpretation; no penalty for switching. [D]
- Float math is **single precision only**. Estimates (`vrefp`, `vrsqrtefp`, `vexptefp`, `vlogefp`) give about 12 bits (6–12 quoted). A Newton–Raphson step takes 2 instructions for reciprocal and 4 for reciprocal sqrt, with `vnmsubfp` as the key instruction. [D]
- Removed vs AltiVec: the integer multiply/sum families (list in `vmx128_overview_unsupported.htm`). [D]
- `lvxl`, `lvxl128` etc. behave **identically** to `lvx`/`lvx128` (no "don't keep in cache" hint). The element loads `lvebx/lvehx/lvewx` are described as identical to `lvx`. Stores of elements remain element-specific. [D]
- `lvx`/`stvx` ignore the low 4 address bits (never misalign-fault). [D]
- `mtvscr` flushes the pipelines. [D]
- All 245 instruction pages are tabulated in the CSV; class summary in Appendix A.

#### 4.3.2 Register reach and operand constraints
Most **non-128** forms can only name VR0–VR31 (5-bit fields). The **128** forms reach VR0–VR127 but some squeeze the encoding by tying operands. [D] `vmx128_instruction_summary.htm`, individual pages

| Instruction | Constraint | Tag |
|---|---|---|
| `vmaddfp128` | destination **must equal** vSrcC (addend); destructive | [D] |
| `vmaddcfp128` | destination **must equal** vSrcB (multiplicand); computes A·B + C | [D] |
| `vnmsubfp128` | destination tied to the addend (vSrcC), `D = -(A*B - D)`. The page also says "same as mul2", which contradicts its own pseudocode; **hardware settles it for the addend** (256/256 lanes, §14) | [D], hardware-confirmed |
| `vsel128` | destination is also the control mask (mask is destroyed) | [D] |
| `vperm128` | permute-control register must be in **VR0–VR7** | [D] |
| `vrlimi128`, `vpkd3d128` | destination is also the insert target (mask-insert), so it is read-modify-write | [D] |
| Integer-math, compare-integer, byte/half splat/merge/pack/shift forms | no 128 variant; **VR0–VR31 only** | [D] |
| `vdot3fp`, `vdot4fp` | appear to be alternative assembler spellings of `vmsum3fp128`/`vmsum4fp128` (same intrinsic and semantics); no separate 32-register encoding is known from the hardware-derived ISA table | [I] |

CSV column `register_reach` gives the per-instruction answer (76 base forms have a 128 twin, 85 have none, 8 are 128-only). **LLVM note** [I]: this implies at least a full `VR128` class plus a `VR32` (VR0–VR31) subclass for the operands of non-128 forms, and a tiny `VR0–7` class for `vperm128`'s control operand; tied-operand constraints must be encoded in the instruction definitions.

#### 4.3.3 Pipeline classes
Full per-instruction list in the CSV; counts: Vector Simple 111 (4 cycles), Vector Permute 54 (4), Vector Float 34 (12), Vector/Scalar Load 18 (2), Vector/Scalar Store 16 (n/a), Vector Float estimates 8 (14), Dot product 4 (14). **Hardware correction (§14):** only 4 of the 8 "estimate" instructions (`vrefp`, `vrsqrtefp` and their 128 forms) take 14 cycles; `vexptefp` and `vlogefp` (and 128 forms) measure **12**, like other Vector Float ops.
- **[!]** `vrlimi128`: the instruction page says *Vector Simple*; the new-instructions white paper says *Permute pipeline*; the summary table says lower (= simple) set. I used the instruction page in the CSV. Verify on hardware.
- **[!]** `vcmpgefp128` and `vcmpgtub` have no latency table in the docs; I classified both as Vector Simple/4 by analogy with sibling compares.
- Float-to-int/int-to-float converts and the four round-to-integer ops (`vrfim/in/ip/iz`) are Vector Float (12). Vector float compares, min/max and logicals are Vector Simple (4).

#### 4.3.4 New-in-VMX128 instructions (semantics)
- **`vmulfp128`**: plain multiply (saves loading a zero addend). Same 12-cycle latency as `vmaddfp`.
- **`vpermwi128`**: swizzle the four words of one register by an 8-bit immediate, 2 bits per destination word, X in the top bits. Permute class, 4 cycles.
- **`vrlimi128 vD, vB, mask(4b), rot(2b)`**: rotate vB left by `rot` words, then insert into vD only the words selected by `mask` (X is bit value 8, W is 1). vB unchanged.
- **`lvlx/lvrx`, `stvlx/stvrx`** (plus `l` variants): left/right partial vector loads/stores confined to one aligned 16-byte block. `lvlx` loads from the address up to the next 16-byte boundary, left-aligned, remaining bytes zero. `lvrx` loads from the previous boundary up to the address, right-aligned (nothing if the address is aligned). An unaligned vector load is `vor(lvlx(p,0), lvrx(p,16))`; a store is `stvlx(v,p,0); stvrx(v,p,16)`. `lvlx` is also the cheap way to load a 1/2/4/8-byte naturally aligned scalar into a vector. **All four raise an alignment exception on non-cacheable memory.**
- **`vmsum3fp128` / `vmsum4fp128`** (dot product): result replicated to all four lanes; `3` ignores W. 14 cycles, pipelined. Internals: products truncated to 28 bits, aligned to the largest exponent (so result is order-independent), simplified (non-true) negation → occasional **1-bit errors** (e.g. (1,1,1,1)·(1,−1,1,−1) gives about 2⁻²⁸, not 0). **Overflow gives NaN, not infinity.** Guaranteed accuracy ≈ 1 part in 2²³ of the largest product.
- **`vpkd3d128` / `vupkd3d128`** (D3D pack/unpack, 4 cycles, permute class). Operands: data type DT (0–6), mask-select MS (0–3), shift SHW (0–3), all immediates. Packed types: `D3DCOLOR`, `NORMSHORT2`, `NORMPACKED32`, `FLOAT16_2`, `NORMSHORT4`, `FLOAT16_4`, `NORMPACKED64`. Packing to integer fields expects the float input pre-biased as `3.0 + x·2⁻²²` (both signed and unsigned fields); unpacking yields `3.0 + x·2⁻²²` for signed fields and `1.0 + x·2⁻²³` for unsigned ones, which one `vmaddfp` then rescales. The most-negative signed value unpacks to NaN. Pack clamps to the format range.
  - Xbox 16-bit float: 1 sign, 5 exponent, 10 mantissa; **no Inf/NaN**, the top exponent is used as an ordinary exponent (max value 131008); denormals unpack to zero; packing **rounds toward zero** and saturates at 131008.

#### 4.3.5 Compare/branch helpers
- `vcmp*` produce an all-ones/all-zeros lane mask. The record forms (`.` suffix) also set CR6; the intrinsic result has 0x80 = all lanes true, 0x20 = all lanes false. Branching on them flushes like scalar FP compares. [D]
- `vcmpbfp` (bounds compare) is Vector Simple. [D]

---

## 5. Memory model and atomics

Sources: `lockless_programming.htm`, `xenon_cpu_caches.htm`, `MemoryBarrier.htm`, `AcquireLockBarrier.htm`, `ReleaseLockBarrier.htm`.

### 5.1 Reordering behaviour
The CPU does not reorder *instructions*, but **writes complete out of order** (store queue → eight 64-byte store-gather buffers → L2) and reads can effectively be reordered (cache misses, prefetch, branch prediction). [D]

| Reordering | No barrier | after `lwsync` | after `sync` |
|---|---|---|---|
| Read moves ahead of read | Yes | No | No |
| Write moves ahead of write | Yes | No | No |
| Write moves ahead of read | Yes | No | No |
| **Read moves ahead of write** (store→load) | Yes | **Yes** | No |

- `lwsync` therefore gives acquire/release strength; it does **not** order an earlier store before a later load of a different address. `sync` is needed for full sequential consistency (Dekker-style patterns). [D]
- A load whose *address* depends on an earlier load (pointer chase) is ordered after it without any barrier. [D]
- `isync` and `eieio` are not needed for ordinary synchronization; `eieio` is for ordering accesses to caching-inhibited memory. [D]
- `lwsync` and friends do **not** affect non-cacheable/write-combined memory. [D]
- Cached writes to the same aligned 64-byte block from one core are ordered/simultaneous; to different blocks they may reorder. [D]

**LLVM note** [I]: the docs' guidance matches the usual PowerPC mapping: acquire/release/acq_rel fences → `lwsync`, seq_cst fence → `sync`. The docs add that their `Interlocked*` primitives contain **no** barrier of their own, so atomics RMW lowering must add `lwsync` (or `sync`) before/after as the ordering requires.

### 5.2 `lwarx` / `stwcx.` (and `ldarx`/`stdcx.`)
- Reservation granularity is the whole **128-byte cache line**; any write to the line by another thread clears it, including unrelated data in the same line. [D]
- **[!] The docs state there are bugs in the CPU's implementation, say that Microsoft's library functions contain workarounds, and give no details.** Anyone lowering LL/SC for this target needs the actual erratum text from another source before trusting a plain loop. [G]
- Title code was told never to use these instructions directly (system-reserved). [D]
- A correct atomic RMW also needs `lwsync` for visibility/ordering as above. [D]

### 5.3 Atomicity of plain accesses
Naturally aligned loads/stores of native types are atomic; unaligned ones are not guaranteed to be, and the docs also say natural alignment is required for the Interlocked operations. [D] The docs do not say whether a 16-byte `lvx`/`stvx` is single-copy atomic. [G]

### 5.4 Other
- Interlocked-style operations exist for 32 and 64 bit (and SList variants); acquire/release variants append/prepend `lwsync`. [D]
- Hardware threads are statically assigned by software (no OS migration) and there is no priority-inversion protection. [D]

---

## 6. Floating-point behaviour

| Topic | Behaviour | Tag |
|---|---|---|
| Scalar formats | IEEE single/double in 64-bit FPRs | [D] |
| Fused multiply-add | `fmadd(s)` available; used by default when contraction is on (`fp_contract` ON default, `/fp:fast` default) | [D] |
| Vector `vmaddfp` | product is **not** rounded before the add | [D] |
| Scalar denormals | supported, **≈60-cycle penalty** per denormal result | [D] |
| Vector denormals | flushed to zero (properly signed) **when `VSCR[NJ]=1`**, which the SDK's system sets. Hardware with `NJ=0`: gradual underflow, ≈58–67 cycles per op | [D]; mode dependence from hardware (§14) |
| [!] | The ABI page also says denormals are "not supported" in a blanket sentence, which contradicts the two rows above; the detailed text wins | [!] |
| FP exceptions | enabling them makes the CPU **≥5× slower**, even if none fire | [D] |
| NaN inputs | extra cycles in the scalar FPU ("VIQ NaN stalls" counter) | [D] |
| Vector estimates | `vrefp`/`vrsqrtefp`: ~12 bits; scalar `fres`: 1/256; `frsqrte`: 1/32 | [D] |
| Vector abnormal results | counter exists (e.g. `vrefp` of 0, `vsubfp` with ∞) | [D] |
| Dot product | order-independent but has 1-bit errors; overflow → NaN | [D] |
| u64↔FP | software helpers | [D] |
| 16-bit float (pack/unpack only) | see 4.3.4 | [D] |

**LLVM note** [I]: the vector flush-to-zero behaviour is a candidate for the `denormal-fp-math` function attribute on vector operations. Never constant-fold or assume IEEE-denormal semantics for VMX results.

---

## 7. ABI (Microsoft's Xbox 360 convention; this is *not* SysV)

Sources: `callstd_introduction.htm`, `callstd_stack_frame_layout.htm`, `callstd_no_toc.htm`, `callstd_compressed_pdata.htm`, `callstd_floating_point_emulation.htm`, `dev_compiler_inline_assembly.htm`.

You only need this for interoperating with existing Xbox 360 code (XEX titles, the XDK libraries). A baremetal-only target is free to pick its own convention; if you do, avoid the traps in section 7.6.

### 7.1 Registers
| Registers | Role | Persistence |
|---|---|---|
| r0 | scratch ("zero" in some forms) | volatile |
| r1 | stack pointer | non-volatile |
| r2 | reserved, unused by compiler | non-volatile |
| r3–r10 | integer args and return | volatile |
| r11, r12 | scratch | volatile |
| r13 | reserved, unused by compiler | non-volatile |
| r14–r31 | general | **non-volatile** |
| f0 | scratch | volatile |
| f1–f13 | FP args and return | volatile |
| f14–f31 | general | **non-volatile** |
| vr0 | scratch | volatile |
| vr1–vr13 | vector args and return | volatile |
| vr14–vr31 | general | **non-volatile** (SDK table says volatile; see [!] below and §14) |
| vr32–vr63 | general | **volatile** (per hardware-side evidence) |
| vr64–vr127 | general | **non-volatile** |
| LR, CTR, XER, FPSCR | | volatile |
| CR fields 0–1, 6–7 | | volatile |
| CR fields 2–5 | | non-volatile |

- **[!]** The SDK table says VR14–VR63 volatile, but its millicode text says the vector save/restore helpers cover VR14–VR31. The hardware-side evidence (helper tables read from real XDK modules, §14) gives **v14–v31 and v64–v127 non-volatile, v32–v63 volatile**, which matches the millicode text; treat the table row as a typo. Only `__savegprlr_29/30` and `__restgprlr_30` appear by name in the SDK docs. [G]
- **[!]** The frame text says "no provision to save CR on the stack" while also listing CR2–CR5 as non-volatile; the prologue text says non-volatile CR parts must be saved. Contradictory.
- `/QVMXRESERVE` removes VR64–VR95 from allocation so titles can pin constants in them. [D]

### 7.2 Argument passing and return
- The argument list is laid out like a memory structure; vectors need 16-byte alignment, everything else 8-byte. Arguments are right-justified in 8-byte slots; **padding bits are undefined** (so callees may not assume sign/zero extension of narrow arguments). [D]
- First 8 integer-class doublewords in r3–r10; first 13 FP args in f1–f13 and first 13 vector args in vr1–vr13. When a prototype exists, an FP arg still **consumes its integer slot position** (the "next available integer register is skipped"). Without a prototype or for varargs, FP args are also duplicated into integer registers/stack. [D]
- `float` arguments are passed as 32-bit values padded to 64 bits. [D]
- Structs/unions are passed by value in memory/integer registers (small power-of-two sizes right-justified, otherwise left-justified); even if they contain floats or vectors. `__declspec(passinreg)` lets a single-member struct of `float`/`double`/`__vector4` travel in an FP/vector register. [D]
- C++ `this` is the first parameter. [D]
- Return: float/double in f1, vector in vr1 (static member functions only), other values ≤ 64 bits in r3. Larger structs: the caller passes a hidden result pointer **as the first argument (r3)**; non-static member functions returning vectors follow the struct rule. [D]
- Varargs: the callee spills r3–r10 into the first eight doublewords of the parameter area; `va_list` is a plain pointer; `va_arg` increments by size; `va_end` is a no-op. [D]

### 7.3 Stack frame
- 16-byte aligned stack. Minimum frame for non-leaf functions is **10 doublewords**; outgoing parameter area is at least 8 doublewords and always present for non-leaf functions. [D]
- Layout, from the stack pointer upward (low → high address): linkage area/back chain, outgoing parameters, dynamic allocations, locals (16-byte aligned), VR/FPR/GPR save areas, saved LR. Back chain at `0(r1)`. [D, ordering of the doc's list read as high→low]
- **Red zone: 1,344 bytes below r1.** Leaf functions using ≤1,344 bytes need no frame at all. [D]
- No stack probing: frames larger than 32 KB need no special code. [D]
- `alloca`: space is a multiple of 16; a back-chain word is written at the new top; a frame pointer is used for locals afterwards. [D]

### 7.4 Prologue/epilogue (matters if the Xbox kernel's unwinder must walk your frames)
- Prologue may only use `stw`, `std`, `stfd`, `stvx` with r1 as base to save registers; the frame is acquired with `stwu`/`stwux` only (the only prologue instructions allowed to modify r1). All modified non-volatiles (including LR and the non-volatile CR parts) must be saved. [D]
- **Millicode** helpers for save/restore: `__savegprlr_n`, `__restgprlr_n` (GPR n..31 with LR; n = 13…31), `__savefpr_n`/`__restfpr_n` (n = 14…31), `__savevmx_n`/`__restvmx_n`. Example sequence: `mflr r12; bl __savegprlr_30; stwu r1,-40(r1)` … `addi r1,r1,0x40; b __restgprlr_30`. The save helper stores r12 (LR) at −8(r1), r31 at −16(r1), r30 at −24(r1) *before* the frame is allocated (using the red zone). [D]
- The kernel reverses prologues using the `.pdata` table (next).

### 7.5 `.pdata` unwind data and exceptions
- One 8-byte entry per function: `BeginAddress` (32 bits), `FunctionType` (2 bits), `FunctionLength` (22 bits, **in instructions**, max 4,194,304), `PrologLength` (8 bits, **in instructions**, max 255). [D]
- Types: save-millicode, no-handler, restore-millicode, handler. Save-millicode bodies may contain only `stw/std/stvx/stfd/addi` and end in `blr`; restore-millicode only `lwz/ld/lvx/ld-FP/addi` and end in `blr`. A handler-type function has an 8-byte `{ExceptionHandler, HandlerData}` record directly before it in `.text`. [D]
- C++ exceptions are described as not reliably supported by this compiler; the game guidance is to disable them. [D]

### 7.6 No-TOC addressing (code model)
- There is **no TOC and no function descriptors**. Function pointers are raw code addresses. No `..`-style symbol prefix. No NOP after external calls. [D]
- Address of a global: `lis rX, hi_adj(sym)` then `addi rY, rX, lo(sym)`; loads/stores fold the low part: `lis r11, hi(sym); stw r14, lo(sym)(r11)` (REFHI/REFLO relocations; the `hi` form is the *adjusted* high part). [D]
- The inline-assembler pseudo-ops `lau`/`lal` instead build an address with `addis` + **`ori`**; this is an assembler convenience, not the compiler's normal form. [D]
- Imported (DLL) functions are called through a `bl` to a thunk: `addis r12,0,hi(__imp_f); lwz r12,lo(__imp_f)(r12); mtctr r12; bctr`. [D]
- No GP-relative addressing in DLLs. [D]

### 7.7 Assembler/compiler details worth knowing
- `nopalign 8` (assembler pseudo-op) pads to an 8-byte boundary so code starts a dispatch pair. [D]
- The standalone assembler (`ml`) lacks VMX128; only the inline assembler and intrinsics do. [D]
- Intrinsics map one-to-one to instructions; vector intrinsic names are the mnemonic with a `__` prefix. [D]

---

## 8. Cache and memory-system parameters (for `TargetTransformInfo` / tuning)

| Parameter | Value | Tag |
|---|---|---|
| Cache line size, all levels | **128 bytes** | [D] |
| L1 I-cache | 32 KB, **2-way**, true LRU | [D] |
| L1 D-cache | 32 KB, **4-way**, pseudo-LRU, **write-through, no-write-allocate** (a write only updates L1 if the line is already present); 256 lines = four ways of 8 KB (64 lines each) | [D] |
| L2 | 1 MB, **8-way** (8 × 128 KB ways), shared by 3 cores, write-back + write-allocate, pseudo-LRU, runs at half core clock, 64-byte path to L1 | [D] |
| Coherence | L2 tracks L1 contents, invalidates L1 copies on remote writes and L2 evictions; I-caches are never invalidated by L2 | [D] |
| Store-gather buffers | 8 × 64 bytes per core, shared by both threads | [D] |
| L2 gateways ("RC machines") | 8, each tied up ≈570 cycles per DRAM miss → ≈5.6 GB/s read ceiling (≈4.4 GB/s single thread) | [D] |
| Prefetch | `dcbt` (L1 and L2); up to **8** in flight per core, so prefetch ≈8 lines (1 KB) ahead; no hardware **data** prefetch; hardware **instruction** prefetch exists | [D] |
| Write-combined memory rules | writes ≥4 bytes, naturally aligned, sequential, no gaps or overlap (≤ 2 streams per core); violating them can cut bandwidth by 50× | [D] |
| Cache locking | part of L2 (in 128 KB ways) can be locked and mapped for GPU streaming; L1 cannot be locked | [D] |
| Snooping | the south bridge snoops the L2; the **GPU does not**; GPU-visible data must be flushed (`dcbf`/`dcbst`) or written to write-combined memory | [D] |
| TLB | 1,024 entries per core; ERATs: separate I and D, each 64 entries (32 sets × 2 ways), each entry covers 4 KB regardless of page size | [D] |
| Page sizes | 4 KB, 64 KB (default for code/data), 16 MB (physical) | [D] |
| FSB | 10.8 GB/s each direction (≈10.2 usable); DRAM 22.4 GB/s aggregate | [D] |
| Set-associativity hazard | streams separated by multiples of 8 KB collide in the 4-way L1D; 128 KB multiples collide in L2 | [D] |
| Code placement | 16-byte aligned branch targets, taken branches at end of 16-byte blocks | [D] |

**Cache-control instruction semantics** [D]
- `dcbz128`: allocates and zeroes the 128-byte line containing EA **without reading memory**; illegal (alignment exception) on non-cacheable memory; the SDK calls the plain `dcbz` a 32-byte, deprecated form, but on a console booted through XeLL it also clears 128 bytes (24/24 cases, §14), so the size is configuration-dependent.
- `dcbf`: write back and invalidate (frees L1 and L2); `dcbst`: write back, keep.
- `dcbt`: prefetch into L1+L2; no-op on caching-inhibited blocks; TLB-miss prefetches are dropped, ERAT-miss prefetches flush.
- An "extended" touch (`xdcbt`, which skipped the L2) existed but was **removed from the SDK for unpredictable behaviour**; the docs say to use `dcbst` instead. Do not generate it. [D]

---

## 9. Errata and hazards (collected)

| # | Item | Detail | Workaround in docs |
|---|---|---|---|
| 1 | **`mftb` 64-bit read** | the low and high 32 bits update four cycles apart, so a 64-bit read can be wrong | read only the low 32 bits (`__mftb32`), or use `QueryPerformanceCounter` | 
| 2 | **`lwarx`/`stwcx.` (and 64-bit forms)** | "bugs in the CPU implementation" (unspecified) | use the OS Interlocked functions; **[G]** details unknown |
| 3 | **`xdcbt`** | unpredictable behaviour | removed; use `dcbst` |
| 4 | Dot product (`vmsum*`) | 1-bit LSB errors; overflow yields NaN rather than ∞ | accept, or avoid for exact cancellation cases |
| 5 | Integer divide | no trap on divide-by-zero; undefined result | compiler emitted explicit checks (switch to remove) |
| 6 | Load-hit-store false positives | only low 12 address bits compared | avoid 4 KB-aliased store→load pairs |
| 7 | FP exceptions enabled | ≥5× slowdown | keep disabled |
| 8 | Misaligned FP | alignment exception | keep ≥4-byte aligned; `__unaligned` makes the compiler go via integer load + store + float load |
| 9 | VMX partial loads/stores on non-cacheable memory | alignment exception | use only on cacheable memory |
| 10 | `dcbz128` on non-cacheable memory | alignment exception | cacheable only |
| 11 | Prefetch ERAT miss | flushes the pipeline | use large pages |
| 12 | `mtvscr` | flushes pipelines | avoid |
| 13 | Original dev-kit hardware | doc notes the pipeline animator models the *final* CPU, not alpha kits | n/a for baremetal on retail hardware |

---

## 10. What the original compiler did (useful as a heuristic checklist) [D]

- Pre-allocation scheduling pass (`/Ou`) plus a built-in pipeline simulator (`/QXSTALLS`). The simulator's annotations name the same hazards as this document: **S** structural, **D** data/dependency, **P** non-pipelined stall, **LHS** load-hit-store; it reports scalar IPC and VMX IPC per function.
- Programmers were told to prefer `fsel`/`vsel`/`vmaxfp`/`vminfp` over compare-and-branch, and `cntlzw` for branchless integer tricks; the compiler turns `cond ? intconst : intconst` into branchless code on its own.
- Programmers unrolled loops to cover 10–14-cycle latencies; `__restrict` was the main aliasing tool because the compiler assumes aliasing by default.
- Vector constants: loaded from the data segment with 3 instructions (`lis; addi; lvx128`) rather than rebuilt; `vspltisw` for small immediates and zero.
- Vector register moves via `vor`; vector negate via `vxor` with a sign-bit constant.
- Optimising for speed avoided microcoded instructions; optimising for size used them.
- Bit-field insert/extract used the CPU's rotate-and-mask instructions.
- Loads of vector data used `lvx128` (with address in a register pair; PPC vector loads are indexed only, so one to three instructions per access).
- Floating-point contraction on by default; whole-program optimisation and profile-guided optimisation recommended for release builds.
- Struct-by-value passing and operator-overload temporaries were flagged as sources of load-hit-stores.
- `volatile` accesses were given acquire/release semantics (MSVC extension) — not a hardware property.

---

## 11. Gaps: not in the docs

| Gap | Why it matters for the subtarget | Where to look instead |
|---|---|---|
| Binary encodings (scalar and VMX128, incl. how VMX128 splits 7-bit register fields) | instruction definitions, assembler, disassembler | independent open-source Xenon toolchains/decoders; hardware disassembly |
| Full scalar PowerPC instruction list and exact ISA level | feature flags | PowerPC ISA 2.0x books; probe on hardware |
| System/privileged architecture (MSR, HID/SPRs, hypervisor, SMT/thread control, interrupts, MMU details) | baremetal bring-up | other sources / experiments |
| Integer multiply/divide latency, `fdiv`/`fsqrt` latency, `fres`/`frsqrte` pipe and latency | scheduling model | **now measured on hardware (§14)** |
| FP load→FP use latency; vector store data timing; CR-logical and compare→branch latencies | scheduling model | FP load→use and compare→branch now measured (§14); vector store data timing and ALU→store-data forwarding still open |
| Branch predictor sizes, hint-bit behaviour, link-stack depth | tuning | link stack depth 4 measured (§14); predictor capacity not measurable with straight-line code; hint bits open |
| Exact `lwarx`/`stwcx.` erratum | atomics lowering | other sources / experiments |
| Thread-priority instruction encodings | intrinsics | standard PPC "or rX,rX,rX" conventions (verify on hardware) |
| `icbi`/`isync` behaviour and cost | JIT, loaders | `isync` and `icbi` costs now measured (§14); semantics not in the SDK |
| Unaligned-access cost for `lfd`/`stfd` that don't cross 32 B | cost model | partial: only the 32 B-crossing case is quantified |
| Exact microcode expansion and cost per microcoded instruction | cost model | the only numbers: 11-cycle fetch; variable shifts ≈16 |
| Which `rotate/shift` immediates avoid microcode when the CR is recorded | peephole | partial list only |

---

## 12. Validation plan (to turn [I] into measured facts)

1. **Timing source.** Time base ticks at 50 MHz = 64 cycles/tick; read only **TBL** (erratum #1) and repeat the loop long enough that ±1 tick is negligible.
2. **Latency chains.** For each class, build a loop of N dependent instructions (e.g. 1024-deep `add r3,r3,r4`; `lwz r3,0(r3)` pointer chain; `vmaddfp vN,…` accumulate chain) and divide. Expected: ALU 2, load→load 5, `fadd` 10, `vmaddfp` 12, `vrefp` 14, `vmsum4fp128` 14, `vperm` 4, `vaddubm` 4.
3. **Cross-consumer cases.** ALU→address (5), load→ALU (2), ALU→store data (2 vs 5), FP→store (0), vector→store (12).
4. **Pairing rules.** Test A/B slot alignment by offsetting code by one instruction; test same-setup-set VSU pairs (e.g. `vmaddfp` + `fmul` vs `vmaddfp` + `vperm`).
5. **Unknowns from section 11.** `mullw`, `mulld`, `divw`, `divd`, `fdiv`, `fsqrt` (latency and blocking), `fres`, `frsqrte`, FP-load latency, `cmp`→`bc`, `mtctr`→`bctr`.
6. **Microcode.** Time `rlwnm`/`slw` and recording logicals; confirm the ≈16-cycle claim and which record forms are microcoded.
7. **Performance counters.** The CPU counter library (PMCPB) exposes IS2 stall cycles, IS2 dependency stalls, u-code instructions committed, load-hit-stores, pipeline flushes, branch flushes, VIQ stall breakdown (denorm, div/sqrt, NaN, dependency) and per-class VMX instruction counts. They are the right tool to confirm stalls without guessing (pages `system_overview_perf_dmpmcpb_*.htm`).

---

## 13. Source index (pages inside `xbox360sdk.chm`)

| Topic | Page(s) |
|---|---|
| Pipelines, hazards, SMT, microcode, branch costs | `xenon_cpu_pipelines.htm`, figure `cpu_pipeline.jpg` |
| CPU best practices, costs | `xbox_360_cpu_best_practices.htm` |
| CPU overview | `xenon_cpu_overview.htm` |
| Caches, prefetch, LHS, `lwarx`/`stwcx.` | `xenon_cpu_caches.htm`, `CPU_cache_overview.htm` |
| Data alignment | `xenon_cpu_data_alignment.htm` |
| Page sizes, TLB/ERAT | `xbox_360_memory_page_sizes.htm` |
| Memory model | `lockless_programming.htm`, `MemoryBarrier.htm`, `AcquireLockBarrier.htm`, `ReleaseLockBarrier.htm` |
| VMX128 inventory/semantics | `vmx128_new_instructions.htm`, `vmx128_instruction_summary.htm`, `vmx128_overview*.htm`, `vmx128_<mnemonic>.htm` (245 pages) |
| Scalar instruction pages | `ppc_*.htm`, `atoc_ref_ppc.htm`, `dev_compiler_ppc_intrinsics.htm`, `ppc_trap_intrinsics.htm` |
| ABI | `callstd_introduction.htm`, `callstd_stack_frame_layout.htm`, `callstd_no_toc.htm`, `callstd_compressed_pdata.htm`, `callstd_floating_point_emulation.htm` |
| Compiler behaviour | `xenon_compiler_technology.htm`, `dev_compiler_qxstalls.htm`, `dev_compiler_inline_assembly.htm`, `dev_compiler_pragma_fp_contract.htm`, `dev_compiler_pragma_bitfieldorder.htm`, `dev_compiler_QVMXReserve.htm` |
| CPU performance counters | `system_overview_perf_dmpmcpb*.htm` |
| Hardware overview | `xenon_hw_overview.htm` |

---

## 14. Hardware cross-check (numbers from OpenXenos, not from the SDK)

Source: `OpenXenos/isa` at commit `279d34c` (`xenon_execution_model.md`, `SHORTCOMINGS.md`, `hwtest/findings/*`). One console, PVR `0x00710500`, full speed, booted through XeLL/libxenon; timebase 49.875 MHz at 64 cycles per tick; the core's other hardware thread ran libxenon's idle loop (low priority, `or r1,r1,r1` inside the poll loop). Latencies from dependent chains hold to about 2 %; **throughput figures below 1 cycle per instruction, the `pairing` matrix and the `ldst` store/FP-load issue costs are known to be unreliable** (OpenXenos `SHORTCOMINGS` §8.2) and are not reproduced here. See the cross-check document for how each item compares with the SDK.

**Scalar and system latencies (cycles)**

| Item | Measured |
|---|---|
| Simple integer (add, logical, extend, `cntlz`, immediate rotate) | 2 |
| `mulli` | 8 |
| `mullw`, `mulhw`, `mulhwu` | 11, not pipelined |
| `mulld`, `mulhd`, `mulhdu` | 14–15, not pipelined |
| `divw`, `divwu`, `divd`, `divdu` | 18 with small operands; `divw/divwu` 32–48 and `divd/divdu` 46–78 with wide operands |
| Variable shifts/rotates (`slw srw sraw sld srd srad rlwnm rldcl rldcr`) | 16, not pipelined |
| `andi.`, `andis.` | 14 |
| Algebraic loads (`lha lhau lhaux lhax lwa lwaux lwax`) | 21–24 |
| GPR load-use (pointer chase) | 5.0; ALU consumer sees ≈2 |
| ALU → address | ≈5 |
| Scalar FP (add, mul, madd family, `frsp`, converts, `fres`, `frsqrte`, `fsel`, `fabs`, `fmr`) | 10, ≈1 per cycle |
| `fdiv`, `fdivs` | 74, not pipelined (24 with NaN/inf operands, 94 denormal) |
| `fsqrt`, `fsqrts` | 84, not pipelined (24 with NaN/inf, 104 denormal) |
| Scalar FP with denormal operands | ≈47 per op; NaN/inf operands ≈36 for add/sub/mul/madd |
| `cmp*`→`bc` extra cost | 0 |
| `lwarx` / `stwcx.` alone | ≈65 / ≈76 |
| `sync` / `ptesync` / `lwsync` | 165 / 164 / 33 |
| `eieio` alone / extra after a store | 92 / +30 |
| `isync` alone | 28 |
| `mflr`, `mfctr` | ≈3 |
| `mfxer`, `mfcr`/`mfocrf`, `mtcrf` | 25, 26–27, 20 |
| `mftb` | 44 |
| `mffs`, `mfvscr`, `mtvscr` | 30, 40, 40 |

**Vector latencies (cycles)**

| Item | Latency | Issue |
|---|---|---|
| Simple integer, logical, permute, splat, pack, merge, compares, min/max | 4 | ≈1 |
| Float add/sub/mul/madd/converts/rounds, `vexptefp`, `vlogefp` | 12 | ≈1 |
| `vrefp`, `vrsqrtefp` (+128) | 14 | **≈2** |
| `vmsum3fp128`, `vmsum4fp128` | 14 | ≈1 |
| Denormal operands with `VSCR[NJ]=0` | ≈58–62 per op (`vrefp` ≈67) | |
| Denormal operands with `VSCR[NJ]=1` | full speed | |

**Memory and branches**

| Item | Measured |
|---|---|
| L1 / L2 / memory (random chase) | 5.0 / 45.2 / 637.7 |
| Memory, sequential 128-byte-stride chase; streaming load | 611.0; 610.4 per line |
| L1: knee, ways, set stride | 32 KB, 4-way, 8 KB |
| L2: knee | 1 MB (8 lines at a 128 KB stride fit, a 9th does not) |
| Extra step between 256 KB and 512 KB | 45 → 58 cycles (ERAT reach, 64 entries × 4 KB) |
| Store→load round trip | GPR 49–54, VR 63, FPR 80 |
| Store miss | consistent with no-write-allocate |
| `dcbz`, `dcbz128` | **both clear 128 bytes** (24/24 cases) |
| Cache-op stream cost per line (minus control) | `dcbt` 97, `dcbtst` 98, `xdcbt` 97, `dcbz` 39, `dcbz128` 40, `dcbst` 10, `dcbf` 10, `icbi` 70 |
| `dcbf` on a resident line: alone / after a store | 92 / 832 |
| Branch taken, empty `bdnz` loop | 8.2 |
| Mispredict | ≈24.7 |
| `bctr`/`blr` after `mtctr`/`mtlr` | ≈19–25 extra regardless of distance |
| Link stack depth | 4 entries |

**Architectural facts**
- Vector FMA operand roles (256/256 lanes each): `vmaddfp: D=A*C+B`, `vnmsubfp: D=-(A*C-B)`, `vmaddfp128: D=A*B+D`, `vmaddcfp128: D=A*D+B`, `vnmsubfp128: D=-(A*B-D)`. FMA ops are single-rounding fused.
- Estimate accuracy: `fres` 2⁻⁸·⁰⁰, `frsqrte` 2⁻⁵·⁰⁰, `vrefp` 2⁻¹³·⁰, `vrsqrtefp` 2⁻¹²·⁴, `vexptefp` 2⁻⁸·⁵.
- Illegal on the console (trap): the 22 dropped legacy VMX ops, `lvsl128`, `lvsr128`, `mcrxr`, `popcntb`, `dcba`, `fre`, `frsqrtes`. So **do not rely on `popcntb`**; only `fres` (not `fre`) and `frsqrte` (not `frsqrtes`) exist.
- Invalid forms that trap: update-form indexed loads/stores with RA=0 or RA=RT, `stwcx.`/`stdcx.` with Rc=0, `mulh*` with OE=1 and Rc=1.
- `ldbrx`/`stdbrx` (outside PowerPC 2.02) are executed by hardware and emitted by XDK compilers from 2571 on, although the SDK docs list only the halfword and word byte-reverse forms.
- Alignment faults: FP loads/stores and `lwarx`/`stwcx.` trap if EA % 4 ≠ 0; `ldarx`/`stdcx.` if EA % 8 ≠ 0; integer and vector accesses never trap.
- Reading the SDK's dot-product description against hardware: overflow gives a quiet NaN, results can be 1 ulp low even for exactly representable sums, `vmsum3` ignores W.
- ABI evidence from real XDK helper tables: non-volatile `v14–v31` and `v64–v127`, volatile `v32–v63`; the XDK ABI uses a red zone. XeLL/libxenon's GCC (9.2, `--with-cpu=cell`) builds ELF32 System V code, never allocates `v32+`, and treats `v20–v31` as callee-saved.

**Caveats:** one console; XeLL environment (HV configuration may differ from the retail kernel, as the `dcbz` size shows); sibling thread was spinning, not parked; boot `VSCR`/`FPSCR` were never read (the harness loads zeroed values, so "NJ=0" is the harness state); memory latency may vary by console revision.

## Appendix A: VMX128 instruction classes (names only)

Per-instruction rows (operands, intrinsic, notes, register reach) are in the CSV.

**Vector/Scalar Load, 2 cycles, upper setup set (18):** lvebx lvehx lvewx lvewx128 lvlx lvlx128 lvlxl lvlxl128 lvrx lvrx128 lvrxl lvrxl128 lvsl lvsr lvx lvx128 lvxl lvxl128

**Vector/Scalar Store, no register result, upper setup set (16):** stvebx stvehx stvewx stvewx128 stvlx stvlx128 stvlxl stvlxl128 stvrx stvrx128 stvrxl stvrxl128 stvx stvx128 stvxl stvxl128

**Vector Permute, 4 cycles, upper setup set (54):** vmrghb vmrghh vmrghw vmrghw128 vmrglb vmrglh vmrglw vmrglw128 vperm vperm128 vpermwi128 vpkd3d128 vpkpx vpkshss vpkshss128 vpkshus vpkshus128 vpkswss vpkswss128 vpkswus vpkswus128 vpkuhum vpkuhum128 vpkuhus vpkuhus128 vpkuwum vpkuwum128 vpkuwus vpkuwus128 vsldoi vsldoi128 vslo vslo128 vspltb vsplth vspltisb vspltish vspltisw vspltisw128 vspltw vspltw128 vsro vsro128 vupkd3d128 vupkhpx vupkhsb vupkhsb128 vupkhsh vupkhsh128 vupklpx vupklsb vupklsb128 vupklsh vupklsh128

**Vector Simple, 4 cycles, lower setup set (111, incl. 2 inferred):** mfvscr mtvscr vaddcuw vaddsbs vaddshs vaddsws vaddubm vaddubs vadduhm vadduhs vadduwm vadduws vand vand128 vandc vandc128 vavgsb vavgsh vavgsw vavgub vavguh vavguw vcmpbfp vcmpbfp128 (and `.` forms) vcmpeqfp vcmpeqfp128 vcmpequb vcmpequh vcmpequw vcmpequw128 vcmpgefp vcmpgefp128* vcmpgtfp vcmpgtfp128 vcmpgtsb vcmpgtsh vcmpgtsw vcmpgtub* vcmpgtuh vcmpgtuw (and `.` forms for all compares) vmaxfp vmaxfp128 vmaxsb vmaxsh vmaxsw vmaxub vmaxuh vmaxuw vminfp vminfp128 vminsb vminsh vminsw vminub vminuh vminuw vnor vnor128 vor vor128 vrlb vrlh vrlimi128† vrlw vrlw128 vsel vsel128 vsl vslb vslh vslw vslw128 vsr vsrab vsrah vsraw vsraw128 vsrb vsrh vsrw vsrw128 vsubcuw vsubsbs vsubshs vsubsws vsububm vsububs vsubuhm vsubuhs vsubuwm vsubuws vxor vxor128   (\* class inferred; † documentation conflict)

**Vector Float, 12 cycles, lower setup set (34):** vaddfp vaddfp128 vcfpsxws vcfpsxws128 vcfpuxws vcfpuxws128 vcfsx vcfsx128 vcfux vcfux128 vcsxwfp vcsxwfp128 vctsxs vctsxs128 vctuxs vctuxs128 vcuxwfp vcuxwfp128 vmaddcfp128 vmaddfp vmaddfp128 vmulfp128 vnmsubfp vnmsubfp128 vrfim vrfim128 vrfin vrfin128 vrfip vrfip128 vrfiz vrfiz128 vsubfp vsubfp128

**Vector Float "estimates", listed as 14 cycles by the SDK, lower setup set (8):** vexptefp vexptefp128 vlogefp vlogefp128 vrefp vrefp128 vrsqrtefp vrsqrtefp128   (hardware: only vrefp/vrsqrtefp and their 128 forms are 14; vexptefp/vlogefp and their 128 forms measure 12)

**Dot product, 14 cycles, lower setup set (4):** vdot3fp vdot4fp vmsum3fp128 vmsum4fp128

**128-only operations (8):** vmaddcfp128 vmsum3fp128 vmsum4fp128 vmulfp128 vpermwi128 vpkd3d128 vrlimi128 vupkd3d128
