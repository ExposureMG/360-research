# Xenon execution model (scheduling-model inputs)

Every row says **what it describes** (Xenon vs. Cell PPE) and **where it comes from**. Nothing here is from memory.

Sources:
* **[FPF]** IBM developerWorks, *"Just like being there: Papers from the Fall Processor Forum 2005: Application-customized CPU design"* (the Xbox 360 CPU article, archived 2009-02-02), `isa/source1/`. This is the only Xenon-specific microarchitecture source we hold.
* **[AB06]** Andrews & Baker, *"Xbox 360 System Architecture"*, IEEE Micro, Mar-Apr 2006, `isa/source2/xbox-system.pdf`. Microsoft-authored, Xenon-specific.
* **[IBMR]** IBM Research, *"Using data-parallel SIMD architecture in video games and supercomputers"*, `isa/source1/`.
* **[CBE]** *Cell Broadband Engine Programming Tutorial v1.1*, repo root. **Cell PPE, not Xenon.**
* **[PEM]** Vector/SIMD Multimedia Extension PEM v2.07c (Cell), `isa/source1/`.

## Core (per Xenon core; the chip has three identical cores)

| Fact | Value | Applies to | Source |
|---|---|---|---|
| Clock | 3.2 GHz | Xenon | [FPF] |
| Architecture | 64-bit PowerPC, "complete PowerPC ISA available" | Xenon | [FPF] |
| Threads | 2 simultaneous hardware threads per core ("instruction unit is multithreaded") | Xenon | [FPF] |
| Issue | 2-issue, **in-order**: two instructions issued at a time, execution within a unit is in sequential order | Xenon | [FPF] |
| Delayed execution | Issue queue delays execution to cover the load-use penalty without stalling the pipeline | Xenon | [FPF] |
| Execution pipes (5) | Branch (part of the instruction unit), Load/Store, Fixed Point, Floating Point, VMX | Xenon | [FPF] |
| Fixed-point pipe | Two units: simple (add/sub, cmp, logical, rotate) and complex (multiply/divide) | Xenon | [FPF] |
| Microcode | "Difficult instructions" are microcoded: cracked at dispatch into multiple micro-ops | Xenon | [FPF] |
| FP / VMX unit | Combined; has two simultaneous threads for VMX and two for FPU | Xenon | [FPF] |
| FP/VMX load latency | Delayed-execution queue reduces load latency to **2 cycles** | Xenon | [FPF] |
| LSU vs FPU ordering | LSU may run out of order with respect to the FPU; final results are correct | Xenon | [FPF] |
| Pipeline stage depth | 11 FO4 per stage in FP/VMX ("quite deep") | Xenon | [FPF] |
| Scalar DP FP latency | **10 cycles** | Xenon | [FPF] |
| VMX latency | **4 or 14 cycles, depending on the operation** (the article does not say which ops) | Xenon | [FPF] |
| Branch history table | 4 KB, 2-way set-associative, **per thread** | Xenon | [FPF] |

## Caches and translation

| Fact | Value | Applies to | Source |
|---|---|---|---|
| L1 I-cache | 32 KB, 2-way, **128-byte lines**, parity | Xenon | [FPF] |
| L1 D-cache | 32 KB, 4-way, parity, **write-through ("store through")**, non-blocking (a miss does not hold up a later hit) | Xenon | [FPF] |
| **Cache line size** | **128 bytes in all CPU L1 and L2 caches** (larger lines improve FSB and memory efficiency) | Xenon | [AB06] (resolves the L1D question; [FPF] only stated it for the I-cache) |
| Store miss | A store to an address not in L1 does **not** allocate a line | Xenon | [FPF] |
| Outstanding loads | 8 outstanding loads/prefetches per core (hit-under-miss) | Xenon | [FPF], [AB06] |
| L2 | 1 MB, **shared by all three cores**, 8-way, MESI, ECC (single-bit correct, double-bit detect) | Xenon | [FPF] |
| L2 clock / path | Crossbar at full core frequency; rest of L2 at half frequency | Xenon | [FPF] |
| Store gathering | 8 store-gathering buffers per core on the cacheable path | Xenon | [FPF] |
| Uncached path | One uncached unit per core, each with 4 non-cacheable write-gathering buffers (sequential, to order non-cacheable operations) | Xenon | [FPF], [AB06] |
| L2 set locking | Configurable set locking so streaming through a locked set does not thrash the rest | Xenon | [FPF] |
| `xdcbt` | "Extended Data Cache Block Touch": a custom prefetch that delivers data to the requesting core's L1 D-cache and **never puts it in L2**, so read streaming does not thrash the shared L2. **Encoding is inferred from compiled code, not documented**: `dcbt` with the reserved IBM bit 6 set, `0x7E00022C` (see `encoding_evidence` in the JSON and `observed/README.md`) | Xenon | [FPF], [AB06] |
| GPU access | GPU can read modified data directly from L2 without a castout or state change | Xenon | [FPF] |
| ERAT | 64-entry, 2-way (both instruction and data side) | Xenon | [FPF] |
| TLB | 1K-entry, 4-way, software- or hardware-managed | Xenon | [FPF] |

## Issue structure (additional)

| Fact | Value | Applies to | Source |
|---|---|---|---|
| Issue rate | Two per cycle, in-order | Xenon | [AB06] |
| Vector/scalar issue queue (VIQ) | A separate VIQ decouples issue between integer and vector instructions for non-dependent work | Xenon | [AB06] |
| Threads | Two symmetric, fine-grained SMT hardware threads per core; six per chip | Xenon | [AB06] |
| Scalar FPU | IEEE-754, single and double precision, full throughput of one operation per cycle for most operations | Xenon | [AB06] (note: [FPF] gives a 10-cycle scalar DP *latency*; these are throughput vs. latency) |
| VMX128 units | Three four-way SIMD units: floating-point, permute, simple | Xenon | [AB06] |
| Shared L2 | Cache lines allocated dynamically among the six threads | Xenon | [AB06] |

## Vector register file

* 128 registers of 128 bits **per hardware thread** [AB06]; all directly addressable; the original 32 are the **first 32 of the 128** and "compatible with the original PowerPC ISA" [FPF].
* Encoding: register fields grow 5 → 7 bits by using otherwise unused instruction bits; the original five bits did not move [IBMR]. This matches the field extraction in `xenon_isa.json` (`VD128`, `VA128`, `VB128`).
* Xenon-specific additions named by [AB06]: a **dot-product instruction** (rounding simplified so it adds little latency over a multiply-add; maps to `vmsum3fp128`/`vmsum4fp128`) and **Direct3D compressed data formats** (the formats the GPU supports) for packing/unpacking, typically ~50% bandwidth and footprint saving (maps to `vpkd3d128`/`vupkd3d128`).
* VMX128 is "binary-compatible with a **subset** of VMX": a few vector FP and vector integer ops are illegal-instruction traps [FPF]. See `legacy_vmx_not_implemented` in the JSON.

## Cell PPE facts, for contrast only (do not use as Xenon numbers)

| Fact | Cell PPE | Xenon | Source |
|---|---|---|---|
| Threads | 2 simultaneous | 2 simultaneous | [CBE], [FPF] |
| L1 I / D | 32 KB / 32 KB | 32 KB / 32 KB | [CBE], [FPF] |
| L2 | 512 KB, **per PPE** | 1 MB, **shared by 3 cores** | [CBE], [FPF] |
| Vector registers | 32 | 128 | [CBE], [FPF] |
| Instruction latencies | not in [CBE] for the PPE (its latency table is for the SPU) | 4/14 (VMX), 10 (DP FP) | [FPF] |

## Measured on a real console (`hwtest`, PVR 0x00710500, full speed)

Source: `hwtest/findings/latency.json`, produced by `hwtest/host/findings.py` from the `latency` plan; every instruction's figure is in
`xenon_isa.json` (`timing`) and the `Cycles lat/issue` column of `xenon_isa.md`. Method: cycles per instruction from the slope between two
unroll depths of a 20,000-iteration loop, converted from the timebase at 64 core cycles per tick. *Latency* is a dependent chain; *issue* is
16 overlapped chains. Everything ran alongside libxenon's idle loop on the core's other hardware thread, with interrupts off.

| Class | Latency | Issue | Notes |
|---|---|---|---|
| Simple integer (add, logical, extend, `cntlz`, rotate by immediate, `addi`) | 2 | ~1 | 35 instructions. Dependent chains run at one result per 2 cycles |
| Carry-chained integer (`adde`, `addze`, `subfe`, ...) | 2 | 2 | not overlapped |
| `mulli` | 8 | 7 | |
| `mullw`, `mulhw`, `mulhwu` | 11 | 11 | not pipelined |
| `mulld`, `mulhd`, `mulhdu` | 14-15 | 13-14 | not pipelined |
| `divw`, `divwu`, `divd`, `divdu` | 18 | 18 | `divd`/`divdu` take **48** with wide operands; the `divw` data set did not exercise wide 32-bit operands |
| Variable shifts and rotates (`slw`, `srw`, `sraw`, `sld`, `srd`, `srad`, `rlwnm`, `rldcl`, `rldcr`) | 16 | 16 | not pipelined: these behave as microcoded instructions |
| `andi.`, `andis.` | 14 | 14 | record-form immediates |
| Vector simple: integer add/sub/compare/min/max, logical, permute, splat, pack, merge (140 instructions) | 4 | ~1.1 | |
| Vector floating point: add, sub, mul, madd, convert, `vexptefp`, `vlogefp` (30) | 12 | ~1.1 | |
| `vrefp`, `vrsqrtefp` (+128) | 14 | ~2.1 | |
| `vmsum3fp128`, `vmsum4fp128` (dot products) | 14 | ~1.1 | |
| Scalar floating point: add, sub, mul, madd family, `frsp`, `fctid*`, `fctiw*`, `fres`, `frsqrte` | 10 | ~1.1 | matches IBM's published 10 cycles |
| `fdiv`, `fdivs` | 74 | 74 | not pipelined |
| `fsqrt`, `fsqrts` | 84 | 84 | not pipelined |

**IBM's "VMX latency is 4 or 14 cycles" is three tiers on this chip: 4 (simple), 12 (floating point arithmetic and conversions), 14 (reciprocal and
reciprocal-square-root estimates, dot products).**

**Data-dependent slowdowns** (cycles per instruction, fixed operands; `normal` ~1.14 for FP):

| Operands | Scalar FP add/mul/madd/convert | Vector FP | `fdiv` / `fsqrt` |
|---|---|---|---|
| denormal | ~47 | ~58-62 (`vrefp` ~67) | 94 / 104 (normal 74 / 84) |
| NaN and infinity | ~36 for add/sub/mul/madd; conversions stay fast | fast | **24** (faster than normal) |
| denormal with VSCR[NJ] set | n/a | **fast again** (1.14) | n/a |

So vector code that can see denormals pays about 50 extra cycles per instruction unless the non-Java bit is set, and scalar FP always pays
for denormals. `divd`/`divdu` are also operand dependent (18 to 48). Multiplies and everything else tested were not.

**Calibration.** A dependent `add` chain measures 2.04 cycles and independent adds 0.98: the PPE's 2-cycle dependent latency and one
instruction per cycle per thread. A wrong timebase scale would move both away from those, so 64 cycles per tick holds to about 2%. Scalar FP
add measuring 10.00 independently matches the IBM article. `verify_isa.py` fails if these anchors move.

Caveats: the sibling hardware thread was running libxenon's idle loop, so these are not single-thread-alone numbers; "issue" of about 1.14 for
FP/vector (against 0.98 for integer) may include that. Tied-accumulator ops (`vmaddfp128`, `vnmsubfp128`, ...) show 1.5 in the fixed-operand
runs because they feed themselves; use their `issue`.

## Timing plans (dual issue, forwarding, caches, branches, loads/stores, system ops)

The plans exist (`hwtest/README.md`, "Timing plans"); the sections below are **generated** by `build_isa.py` from
`hwtest/findings/<plan>.json` between the `BEGIN/END GENERATED` markers (do not edit inside them). A section that says "Not yet
measured" names the command to run. `python3 -I hwtest/run_suite.py status` lists every plan's state; `hwtest/COVERAGE.md` says what
is left. The analysers and decision rules are in `hwtest/host/analysis_timing.py`; until a plan has run on a console, its
analyser has only been checked against synthetic data with a planted answer (`hwtest/host/synth.py`) and against the fake console.
The expected values the checks compare with are the [FPF]/[AB06] facts above and the latency results in this file.

### Dual issue (`pairing`)
Which instruction classes issue in the same cycle: each of 22 classes alone and every unordered pair, interleaved.
<!-- BEGIN GENERATED pairing (build_isa.py) -->
Source: `hwtest/findings/pairing.json` (results13, console not recorded (no hello.json); 548 of 550 tests with a value).

| Variant | Issue together (confident) | Partial | Serial (confident) | Ambiguous | Anomalies | Diagonal |
|---|---|---|---|---|---|---|
| tput | cr+nop, fp+nop, int+nop, lbz+nop, ld+nop, lfd+stfd, lwz+nop, stfd+nop, vfp+nop, vint+nop, vlogic+nop, vperm+nop, vsldoi+nop | fp+vperm, fp+vsldoi, int+lfd, int+vint, int+vlogic, int+vperm, int+vsldoi, lfd+cr, stfd+stfd, stvx+br_nt, stvx+br_t, stw+br_nt, stw+br_t, vint+cr, vint+vperm, vint+vsldoi, vlogic+cr, vlogic+vperm, vlogic+vsldoi, vperm+cr, vperm+vfp, vsldoi+cr, vsldoi+vfp | 20 | 112 | br_nt+br_nt, br_nt+cr, br_nt+nop, br_t+br_nt, br_t+br_t, br_t+cr, br_t+nop, int+stfd, lbz+fp, lbz+lfd, lbz+vfp, lbz+vint ... (63 in all; full list in `xenon_isa.json`) | ok |
| chain | cr+nop, div+fp, div+vfp, fp+br_nt, fp+br_t, fp+fp, fp+vfp, fp+vint, fp+vlogic, fp+vperm, fp+vsldoi, int+int, int+lfd, int+nop, int+stfd, int+stw, int+vint, int+vlogic, int+vperm, int+vsldoi, lbz+nop, ld+fp, ld+ld, ld+stfd ... (96 in all; full list in `xenon_isa.json`) | cr+cr, int+cr, int+lbz, lfd+cr, lvx+cr | 16 | 120 | br_nt+cr, br_t+cr, lbz+lfd, lfd+br_nt, lfd+br_t, lfd+lfd, lfd+nop, lfd+stvx, lfd+stw, lvx+lvx, lvx+nop, lvx+stvx ... (16 in all; full list in `xenon_isa.json`) | ok |

Self-checks: `alone:int:chain` 2.00 (expected 2.04, latency.json addx latency): ok; `alone:fp:chain` 10.00 (expected 10.0, latency.json faddx latency): ok; `alone:int:tput` 1.09 (expected 0.98, latency.json addx issue): ok
<!-- END GENERATED pairing -->

### Cross-class forwarding (`forwarding`)
Producer->consumer penalties (`dep - ind`) and loop-carried sequences against the sum of their parts' latencies.
<!-- BEGIN GENERATED forwarding (build_isa.py) -->
Source: `hwtest/findings/forwarding.json` (results12, console not recorded (no hello.json); 50 of 50 tests with a value).

| Producer -> consumer | dep | ind / control | penalty | verdict | producer latency |
|---|---|---|---|---|---|
| `add.>bc` | 5.07 | 5.00 | 0.07 | none | 2.04 |
| `add>stw` | 6.28 | 3.45 | 2.83 | penalty | 2.04 |
| `addi>lwz_addr` | 6.28 | 1.46 | 4.82 | penalty | 2.04 |
| `cmp>bc` | 5.07 | 5.07 | 0.00 | none | - |
| `fadd>stfd` | 2.67 | 2.67 | -0.00 | none | 10.00 |
| `lfd>fadd` | 4.22 | 2.31 | 1.91 | penalty | - |
| `lvx>vaddfp` | 2.95 | 2.31 | 0.64 | penalty | - |
| `lvx>vand` | 3.17 | 2.31 | 0.86 | penalty | - |
| `lwz>add` | 3.38 | 1.46 | 1.92 | penalty | 5.01 |
| `mfctr>add` | 5.49 | 4.00 | 1.49 | penalty | - |
| `mflr>add` | 5.30 | 4.00 | 1.30 | penalty | - |
| `mtctr>bctr` | 32.00 | 9.12 | 22.88 | penalty | - |
| `mtlr>blr` | 32.00 | 9.14 | 22.86 | penalty | - |
| `vaddfp>stvx` | 13.11 | 3.33 | 9.77 | penalty | 12.00 |
| `vand>stvx` | 5.05 | 3.33 | 1.71 | penalty | 4.00 |

| Loop-carried sequence | cycles | components | extra |
|---|---|---|---|
| `gpr>mem>fpr>mem>gpr` | 120.00 | - | - |
| `ld>add>addr` | 7.38 | 7.05 | 0.33 |
| `ld>addi>addr` | 7.38 | 7.05 | 0.32 |
| `ld>addr` | 5.01 | - | - |
| `ld>ldx_index>addr` | 10.04 | 10.03 | 0.01 |
| `lwz>addr` | 5.01 | - | - |
| `mtctr>mfctr` | 5.90 | - | - |
| `mtlr>mflr` | 5.17 | - | - |
| `stb>lbz` | 53.99 | 5.09 | 48.91 |
| `stb>lwz_wider` | 48.67 | 5.01 | 43.66 |
| `std>ld` | 48.67 | 5.01 | 43.66 |
| `stfd>lfd` | 80.00 | - | - |
| `stfs>lfs` | 80.00 | - | - |
| `sth>lhz` | 48.67 | 5.09 | 43.58 |
| `stvx>lvx` | 63.20 | - | - |
| `stw>lbz_inside` | 54.00 | 5.09 | 48.91 |
| `stw>lwz` | 54.00 | 5.01 | 48.99 |
| `stw>lwz_otherline` | 6.40 | 5.01 | 1.39 |
| `stw>lwz_sameline` | 7.21 | 5.01 | 2.20 |
| `vr>mem>gpr>mem>vr` | 112.00 | - | - |
<!-- END GENERATED forwarding -->

### Caches and memory (`memlat`, runner v2)
Load-to-use latency against working set (knees, L1/L2/memory), conflict strides (associativity), store miss, streams, `xdcbt`.
<!-- BEGIN GENERATED memlat (build_isa.py) -->
Source: `hwtest/findings/memlat.json` (results22, console not recorded (no hello.json); 83 of 83 tests with a value).

| Level | Load-to-use (cycles, random chase) | Fits up to | Cited |
|---|---|---|---|
| L1 | 5.01 | 32 KB | 32 KB [FPF] |
| L2 | 45.18 | 1 MB | 1 MB [FPF] |
| memory | 637.71 | - | - |

Levels: three levels: L1, L2, memory.
* L1 associativity (conflict strides): 4-way, set stride 8K (=> 32K)
* L2 associativity (conflict strides): 8-way, set stride 64K (=> 512K)
* Store miss (no-write-allocate, [FPF]): **consistent** - a load after a store miss pays 73.8 cycles extra (~ one L2 access, L2-L1 = 40.2): the missed store did not leave its line in L1 and was not forwarded from a store buffer
* Streams (cycles per 128-byte line): `ld_stream` 610.44, `st_stream_dcbz_stvx` 148.92, `st_stream_std_full` 257.36, `st_stream_std_sparse` 117.82, `st_stream_stvx_full` 209.61
* Cache-op streams (minus control): `dcbf` 10.18, `dcbst` 10.21, `dcbt` 97.25, `dcbtst` 97.92, `dcbz` 39.39, `dcbz128` 39.60, `icbi` 69.69, `xdcbt` 97.45
* `xdcbt` placement ([FPF]/[AB06]: bypasses L2): **undetermined** - dcbt reference reloads from L2 (expected L2): the 6 loads did not evict the line from L1 as assumed
<!-- END GENERATED memlat -->

### Branches (`branch`)
Taken / not taken / mispredicted, `bctr`/`blr` against the distance from `mtctr`/`mtlr`, link-stack depth, loop overhead.
<!-- BEGIN GENERATED branch (build_isa.py) -->
Source: `hwtest/findings/branch.json` (results14, console not recorded (no hello.json); 46 of 46 tests with a value).

* Per branch: taken to next 5.00, not taken 4.50, `b .+4` 8.21, taken over one word 8.19 (taken - not taken 0.50)
* Mispredict penalty: 24.70 cycles (50% mispredicts (the predictor cannot learn xorshift64's sign bit); addi cost = latency.json addi issue)
* `bctr` penalty by distance from mtctr: d0 22.53, d2 19.33, d4 21.34, d8 21.61, d16 22.57; hidden from distance None
* `blr` penalty by distance from mtlr: d0 22.79, d2 22.70, d4 21.63, d8 23.91, d16 25.14; hidden from distance None
* Link stack: link stack of 4 entries
* Loop iteration: 7.21 + 0.81 cycles per body instruction (empty loop 8.21)
<!-- END GENERATED branch -->

### Branch history capacity (`bhtcap`, runner v2)
[FPF] gives the BHT as 4 KB, 2-way, per thread; this measures how many distinct branches keep their history.
<!-- BEGIN GENERATED bhtcap (build_isa.py) -->
Source: `hwtest/findings/bhtcap.json` (results20, console not recorded (no hello.json); 33 of 33 tests with a value).

* Knee: none found up to 32736 branches
* [FPF] branch history table 4 KB, 2-way set-associative, per thread. Its capacity in branches depends on the entry format, which no held source gives (e.g. 16K 2-bit counters or 32K 1-bit); the knee is reported in branches, not converted.
<!-- END GENERATED bhtcap -->

### Loads and stores (`ldst`)
Every load form's issue, load-use latency, use stall and store->load round trip; every store form's issue; reservations; cache ops.
<!-- BEGIN GENERATED ldst (build_isa.py) -->
Source: `hwtest/findings/ldst.json` (results16, console not recorded (no hello.json); 314 of 321 tests with a value).

| Kind | load issue | load-use (L1) | use stall | store->load round trip |
|---|---|---|---|---|
| FPR | 11.03 | - | 1.90 | 80.00 |
| GPR | 1.00 | 5.09 | 1.32 | 51.34 |
| VR | 11.04 | - | 0.85 | 63.20 |

Medians over the forms; every form is in `xenon_isa.json` `timing`. Reservation: `stdcx.` with load 126.99, alone 76.22, `stwcx.` with load 126.98, alone 76.19. Resident-line cache ops (alone / after a store): `dcbf` 92.12/831.97, `dcbst` 92.16/188.09, `dcbt` 0.76/2.32, `dcbtst` 0.76/2.31, `dcbz` 55.25/90.98, `dcbz128` 54.88/90.98, `icbi` 72.00/72.01, `xdcbt` 0.76/2.32
<!-- END GENERATED ldst -->

### Compares, CR, SPR moves, FPSCR/VSCR, barriers (`sysops`)
<!-- BEGIN GENERATED sysops (build_isa.py) -->
Source: `hwtest/findings/sysops.json` (results15, console not recorded (no hello.json); 99 of 99 tests with a value).

* cmp_L0 compare->branch penalty: 0.01
* cmp_L1 compare->branch penalty: 0.00
* cmpi_L0 compare->branch penalty: 0.00
* cmpi_L1 compare->branch penalty: 0.00
* cmpl_L0 compare->branch penalty: -0.00
* cmpl_L1 compare->branch penalty: 0.00
* cmpli_L0 compare->branch penalty: -0.00
* cmpli_L1 compare->branch penalty: -0.00
* eieio: extra after a load: 0.02
* eieio: extra after a store: 30.28
* isync: extra after a load: 21.51
* isync: extra after a store: 35.94
* mcrf latency (chain of 2 / 2): 1.15
* sync: extra after a load: 15.22
* sync: extra after a store: 25.30
* every test's value is per instruction in `xenon_isa.json` `timing`
<!-- END GENERATED sysops -->

### `lwsync` / `ptesync` (`syncprobe`)
Both encodings are UNVERIFIED aliases in `xenon_isa.json` until this runs: legal or illegal, and their cost against `sync`.
<!-- BEGIN GENERATED syncprobe (build_isa.py) -->
Source: `hwtest/findings/syncprobe.json` (results18, console not recorded (no hello.json); 9 of 9 tests with a value).

* `lwsync`: executes (32.6 cycles alone = 0.20 x sync; cheaper than sync)
* `ptesync`: executes (163.6 cycles alone = 0.99 x sync; costs like sync)
* `sync`: executes (164.8 cycles alone)
<!-- END GENERATED syncprobe -->

### FMA operand order with every lane discriminating (`fmastat`)
The `semantic` run decided the three VMX128 FMA orders on 12-22 lanes each; this plan gives 256 lanes per instruction (and the legacy forms).
<!-- BEGIN GENERATED fmastat (build_isa.py) -->
Source: `hwtest/findings/fmastat.json` (results17, console not recorded (no hello.json); 320 of 320 tests with a value).

| Instruction | verdict | formula | lanes | exact matches |
|---|---|---|---|---|
| `vmaddcfp128` | DECIDED | `D = A*D + B` | 256 | D = A*B + D: 0, D = A*D + B: 256, D = B*D + A: 0 |
| `vmaddfp` | DECIDED | `D = A*C + B` | 256 | D = A*B + C: 0, D = A*C + B: 256, D = B*C + A: 0 |
| `vmaddfp128` | DECIDED | `D = A*B + D` | 256 | D = A*B + D: 256, D = A*D + B: 0, D = B*D + A: 0 |
| `vnmsubfp` | DECIDED | `D = -(A*C - B)` | 256 | D = -(A*B - C): 0, D = -(A*C - B): 256, D = -(B*C - A): 0 |
| `vnmsubfp128` | DECIDED | `D = -(A*B - D)` | 256 | D = -(A*B - D): 256, D = -(A*D - B): 0, D = -(B*D - A): 0 |
<!-- END GENERATED fmastat -->

### Divide by operand set (`divwide`)
<!-- BEGIN GENERATED divwide (build_isa.py) -->
Source: `hwtest/findings/divwide.json` (results11, PVR 00710500, runner v1; 17 of 17 tests with a value).

* `divdux`: small 18.02, w32 45.98, wide 78.02; operand-dependent: w32 45.98, wide 78.02
* `divdx`: small 17.98, w32 46.02, wide 78.02; operand-dependent: w32 46.02, wide 78.02
* `divwux`: by_one 48.02, quot0 17.98, small 18.02, wide 46.02, wide_wide 32.02; operand-dependent: by_one 48.02, wide 46.02, wide_wide 32.02
* `divwx`: by_one 45.98, neg_wide 44.02, quot0 18.02, small 18.02, wide 45.98, wide_wide 32.02; operand-dependent: by_one 45.98, neg_wide 44.02, wide 45.98, wide_wide 32.02
<!-- END GENERATED divwide -->

### Runner v2 regression (`divwide` on runner v2)
The same plan on runner v2: equal numbers mean the v2 kernel buffer/expansion path times kernels like v1.
<!-- BEGIN GENERATED divwide_v2 (build_isa.py) -->
Source: `hwtest/findings/divwide_v2.json` (results19b, PVR 00710500, runner v2; 17 of 17 tests with a value).

* runner v2 minus v1 (cycles): worst relative difference [0.0024, 'divwx:small']
* `divdux`: small 17.98, w32 46.02, wide 78.02; operand-dependent: w32 46.02, wide 78.02
* `divdx`: small 18.02, w32 46.02, wide 78.02; operand-dependent: w32 46.02, wide 78.02
* `divwux`: by_one 48.02, quot0 17.98, small 18.02, wide 46.02, wide_wide 32.02; operand-dependent: by_one 48.02, wide 46.02, wide_wide 32.02
* `divwx`: by_one 45.98, neg_wide 43.98, quot0 18.03, small 17.98, wide 46.01, wide_wide 32.02; operand-dependent: by_one 45.98, neg_wide 43.98, wide 46.01, wide_wide 32.02
<!-- END GENERATED divwide_v2 -->

## Not planned (needs a decision; no plan measures it)

* Thread-switch policy and two-thread interference (SMT), and whether any figure changes with the sibling thread parked: every number
  above runs alongside libxenon's idle loop on the other hardware thread. The experimental runner code for the second thread exists but
  is outside the current scope.

Candidate sources: the Cell PPE Programming Handbook / PPE user manual (closest published pipeline description; Xenon is PPE-derived but not identical), and the Microsoft XDK documentation (not in the repo).
