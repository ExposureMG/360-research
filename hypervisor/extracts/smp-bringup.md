# Secondary hardware-thread bring-up & IPIs

Source: the HV's processor-startup entry point, its reset-vector (`0x100`)
handler, and its thread-park routine in `../17559_hv.c`. This is the
highest-confidence document in this set: the HV's hardware writes **match, byte
for byte, the magic sequences that libxenon and XeLL already perform** — so the
decompile does not just corroborate those sequences, it explains their shape.

The Xenon PPE has 3 cores × 2 SMT threads = **6 hardware threads**, identified by
`PIR` (SPR 1023). The issuing thread is identified via SPR `0x3FF` (its low bits
give the thread's core/thread position); the HV's startup routine runs its body
only on the lead thread (`SPR 0x3FF == 0`).

## The thread-enable register: CTRL (SPR 152 / `0x98`)

Writing `CTRL` controls SMT thread run state:

- **`0x00C00000` → enable** (sets the two Thread-Enable bits). This is the value
  that actually "starts" the sibling threads.
- **`0x0` → park/disable** the current thread (used by the sleep loop below).

This is confirmed three ways: HV startup writes `0xC00000` to `CTRL`; libxenon's
`wakeup_secondary()` is `mtspr(ctrlwr, 0xc00000)` (`drivers/xenon_soc/
xenon_power.c`); XeLL does `mtspr(152, 0xc00000)` on its cold path
(`source/lv1/main.c`). libxenon's `cpusleep.S` parks with `mtspr ctrlwr, 0`.
The decompile confirms `0xC00000` is the enable pattern and `0` is the park
pattern — libxenon/XeLL carry no comment explaining either.

## The wake sequence (interrupt-controller pokes)

Before enabling the threads, a short, fixed sequence is written into the SoC
per-CPU interrupt controller (SoC MMIO base `0x8000020000000000`; see
`soc-interrupt-controller.md` for the register block itself). The HV performs
exactly the writes below, and **they are identical** to libxenon's
`wakeup_cpus()` (`xenon_power.c`, which uses the virtual alias `0x20050000`) and
to XeLL's cold-boot path (`source/lv1/main.c`):

| SoC offset | Value | Role (from context) | libxenon `xenon_power.c` | XeLL `main.c` |
|------------|-------|---------------------|--------------------------|---------------|
| `+0x52070` | `0x7C`     | arm thread-block A | `+0x2070 = 0x7c` | `+0x2070 = 0x7c` |
| `+0x52008` | `0`        | thread-block A     | `+0x2008 = 0`    | `+0x2008 = 0`    |
| `+0x52000` | `0x04`     | thread-block A kick| `+0x2000 = 4`    | `+0x2000 = 4`    |
| `+0x54070` | `0x7C`     | arm thread-block B | `+0x4070 = 0x7c` | `+0x4070 = 0x7c` |
| `+0x54008` | `0`        | thread-block B     | `+0x4008 = 0`    | `+0x4008 = 0`    |
| `+0x54000` | `0x10`     | thread-block B kick| `+0x4000 = 0x10` | `+0x4000 = 0x10` |
| `+0x50010` | `0x140078` | IPI trigger        | `+0x10 = 0x140078` | `+0x10 = 0x140078` |
| `CTRL`     | `0xC00000` | enable threads     | `mtspr(152, 0xc00000)` | `mtspr(152, 0xc00000)` |

The `0x52000`/`0x54000` blocks (stride `0x2000` from the `0x50000` controller
base) are two per-thread-block control regions; `+0x70` arms, `+0x08`/`+0x00`
kick, and `+0x10` on the global block triggers the inter-processor interrupt
that pulls a parked thread out of its loop. The agreement across all three
code-bases makes these values trustworthy even though the decompile does not
label each bit.

## The secondary-hold table (on-chip SRAM)

Before the pokes, the HV seeds two 64-bit words near the **top of the SoC
on-chip SRAM** ("OCR land"), at SoC offsets `0x1FFF8` and `0x1FFF0`, each set to
`0x03FFFC0003FFFC00`. This is the same region XeLL calls the "1BL secondary-hold
table" (`0x800002000001FF80`) and seeds on its cold path. The on-chip SRAM
region (SoC offsets roughly `0x10000`–`0x20000`) is addressable before main RAM
is trained, which is why the hold table lives there rather than in DRAM.

## The reset-vector handshake (`0x100`) — cold boot vs. IPI wake

The decompile's reset handler distinguishes two cases using the per-thread state
pointer in `HSPRG1` (SPR 305 / `0x131`):

- **`HSPRG1 == 0` → cold boot.** The thread has no per-thread state yet, so it
  falls into full power-on initialization. This is the path a bare-metal loader
  effectively replaces.
- **`HSPRG1 != 0` → IPI wake.** A previously parked thread was kicked by the IPI
  above. The handler checks a one-byte "suspended" flag in the thread-state
  block, clears it, restores the thread's `SRR0`/`SRR1` from the values passed in
  (registers r5/r6 at wake), and returns into them. In other words, the IPI
  wake resumes the thread at a caller-chosen address — the wake register value
  plus this `SRR0`/`SRR1` restore is the actual "where does the woken thread go"
  mechanism.

For a bare-metal SMP bring-up this is the template: park sibling threads in a
spin/sleep loop, publish the target entry (libxenon uses a `secondary_lock` /
`wait[]` table indexed by `PIR`; the HV uses the on-chip hold table plus
`SRR0/1`), then issue the IPI to release them.

## The park / sleep side (what `CTRL = 0` actually does)

The HV's thread-park routine shows the full park protocol, which libxenon's
`cpusleep.S` implements only partially:

1. Record the thread's return context and set the **"suspended" byte** in the
   per-thread state block (the flag the reset handler later clears).
2. For a thread that is not the core's lead thread, adjust `HID` so the core can
   drop into a low-power nap when both threads are idle.
3. Loop, writing `CTRL = 0` repeatedly to keep the thread disabled until an IPI
   wakes it.

The suspended-flag + `SRR0/1` restore + `CTRL` toggle together form a clean
wake/park handshake. libxenon parks (`mtspr ctrlwr,0`) and wakes
(`mtspr ctrlwr,0xc00000`) but does not use a suspended-flag handshake; the HV
model is a reference if a more robust park/resume is ever wanted.

## Per-thread state pointers (design note)

The HV keeps each thread's context via **`HSPRG0`** (SPR 304 / `0x130` → a
`HV_THREAD_STATE` structure) and **`HSPRG1`** (SPR 305 / `0x131` → the thread's
stack/state base). libxenon achieves the same end with the **ordinary `SPRG1`**
(per-thread base, `0x40000` bytes per thread, set at startup) and `SPRG0`,
because in real mode it uses the non-H SPRs. This is a deliberate difference, not
a gap — noted so the two code-bases' per-thread conventions aren't confused.

### Actionable for libxenon

- The undocumented block in `xenon_power.c` (`wakeup_cpus()`) and XeLL's cold
  path are the **SoC interrupt-controller wake sequence**; the table above is a
  field key for those exact writes.
- `0xC00000`/`0` on `CTRL` are the thread **enable**/**park** patterns — safe to
  comment as such.
- If a woken thread ever needs a chosen entry point rather than a fixed one, the
  HV's `SRR0/1`-restore-on-wake pattern is the mechanism to copy.
