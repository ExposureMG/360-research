# L2 cache-as-RAM lock, timebase enable/zero, and HDEC

Source: the HV's L2-lock, timebase-enable, timebase-zero, and
hypervisor-decrementer routines in `../17559_hv.c`. These are areas that **all
three reference projects lack**: libxenon and XeLL have excellent cache-*flush*
and TLB code, but neither locks the L2 as RAM, and both only *read* the
timebase — neither enables or zeroes it. (`l2sr`/`buscsr` SPRs are defined but
unused in both; `SPR 284/285` TB-write is never issued.)

What is **not** re-documented here, because it is already well-covered:

- **D-cache / I-cache flush** — libxenon `drivers/ppc/cache.S` and XeLL
  `source/lv1/cache.S` both implement `dcbst`/`dcbf`/`icbi` loops with the
  correct 128-byte line size. The HV uses the identical primitives; nothing to
  add.
- **TLB / SLB management** — libxenon has a full software-TLB-reload page
  handler and SLB identity map (`drivers/ppc/except.S`, `vm.c`); the HV's
  `tlbie`-based single/entire-TLB flush adds nothing beyond it.

All SoC offsets below are relative to the SoC MMIO base `0x8000020000000000`
(real-mode) / `0x20000000000` (physical).

## L2 cache lock (cache-as-RAM) — `+0x60300`

Early boot, before main DRAM is trained, needs a scratch RAM. The Xenon can lock
ways of its L2 cache so they behave as addressable SRAM. The HV exposes this as
a single guarded register write:

- **Register: SoC `+0x60300`** (in the `0x60000` block libxenon/XeLL call
  `soc_30`). Written as a 64-bit value.
- **The written value is a lock key whose high byte (bits 56–63) is the way
  mask.** The HV validates that the way mask has **1, 2, or 3 bits set** — you
  may lock one to three of the four L2 ways, but not zero and not all four
  (leaving at least one way for normal cache operation).
- The write is bracketed by `sync` before and `eieio; sync` after, and the whole
  operation is taken under a spinlock (it is global SoC state).

This is enough to implement cache-as-RAM on bare metal: pick a 1–3-way mask,
form the key, write it to `+0x60300` with the barriers above, and the
corresponding L2 ways are available as RAM at their mapped address. **This
register and its way-mask semantics are decompile-only (not cross-confirmed), so
validate the key encoding on hardware before depending on it.**

## Timebase enable — `+0x611A0`, bit 8

The per-core timebase is gated by a bit in the clock-control block:

- **Register: SoC `+0x611A0`** (in the `0x61000` clock block — the same block
  libxenon pokes for clock/voltage in `xenon_set_speed`).
- **Bit `0x100` (bit 8) = timebase run enable.** Set it to start the timebase,
  clear it to stop. The HV's enable routine takes a boolean and sets bit 8 when
  true, clears it when false, leaving the other bits untouched, followed by
  `eieio; isync`.

libxenon and XeLL assume the timebase is already running (they only `mftb`).
This register is what *makes* it run — useful if you ever need to stop/start or
resynchronize it.

## Timebase zero — `SPR 284` / `SPR 285`

To reset the timebase value, the HV writes the timebase SPRs directly:

- **`SPR 284` (`0x11C`, TBL write)** ← 0
- **`SPR 285` (`0x11D`, TBU write)** ← 0

Writing these is privileged and only possible while running at HV/real-mode
privilege — which bare-metal/libxenon/Linux do. Combined with the enable bit
above, the recipe to get a **synchronized timebase across all 6 threads** is:
stop the timebase (clear `+0x611A0` bit 8), zero `TBU`/`TBL` on each thread,
then enable on each thread. Neither libxenon nor XeLL does this today.

### Note on timebase frequency

The decompile shows the *enable/zero mechanism* but not the frequency (the TB is
derived by a divider from the bus clock). It therefore does **not** resolve the
constant disagreement in the reference projects — libxenon uses
`3192000000/64 ≈ 49.875 MHz` (`drivers/ppc/timebase.h`), XeLL carries both
`45 MHz` (`source/lv1/processor.h`, marked `/* checkme */`) and `50 MHz`
(`source/lv1/time.c`, matching its device tree). That resolution needs a
hardware measurement, not this code.

## Hypervisor decrementer (HDEC) — `SPR 310`

libxenon pokes `HDEC` in two places (`drivers/xenon_soc/xenon_power.c`, and
`drivers/ppc/atomic.S` with the comment *"reset the hypervisor decrementer to
fix a lock issue"*) without explaining why. The HV's dedicated
hypervisor-decrementer interrupt handler explains it:

- The handler simply **writes `HDEC` (`SPR 310` / `0x136`) = `0x7FFFFFFF`** (the
  maximum positive count) and returns with `hrfid`.
- `HDEC` is a free-running privileged down-counter that raises a hypervisor
  decrementer interrupt when it goes negative. Setting it to the maximum pushes
  that interrupt as far into the future as possible — effectively **defusing**
  it. Since bare-metal/libxenon code does not want periodic HV-decrementer
  interrupts firing, setting `HDEC` to a large value (as libxenon does) is
  exactly the right move; the HV does the same thing in its handler. libxenon's
  "fix a lock issue" is precisely this: a pending/expired `HDEC` was raising
  interrupts, and maxing it out stops them.

### Actionable for libxenon

- **L2 cache-as-RAM** is implementable: write a 1–3-way-mask key to SoC
  `+0x60300` under a lock, with `sync`/`eieio` barriers (verify key encoding on
  hardware first).
- The **timebase enable bit** is SoC `+0x611A0` bit `0x100`; the TB can be
  zeroed via `SPR 284/285`. This gives libxenon a path to a cross-thread
  synchronized timebase if desired.
- The `HDEC` pokes in `xenon_power.c`/`atomic.S` are correct and the reason is
  now documented: maxing `HDEC` defuses the hypervisor-decrementer interrupt.
