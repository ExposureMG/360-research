# Hypervisor extracts — baremetal-relevant platform notes

Behavioral documentation derived from studying the decompiled Xbox 360 retail
hypervisor (`../17559_hv.c`, kernel build 17559), scoped to what is useful for
**bare-metal / libxenon homebrew and Linux** on the Xenon CPU.

## Scope & ground rules

- **No code is reproduced** from the hypervisor, from libxenon, from XeLL, or
  from xedreno. These documents describe *behavior* and *hardware interface
  facts* — register addresses, bit meanings, and operation sequences — in
  original prose. Addresses and register semantics are facts about the
  hardware, not copyrightable expression.
- Each document states its **prior-art status** against the three reference
  projects and only exists where the decompile **improves on or fills a gap in**
  that prior art. Areas already well-covered are listed under *Skipped* below
  with the reason, so the decision is auditable.
- Reference checkouts used for the prior-art comparison (read-only, outside this
  repo): `Free60Project/libxenon`, `Free60Project/xell-reloaded`,
  `buddyjojo/xedreno`.

## The one fact that frames everything

libxenon, XeLL, and xedreno **do not run as guests of the retail hypervisor**.
libxenon and XeLL take over the machine and run in **hypervisor real mode**
themselves (they set `HRMOR`/`RMOR` and reload the TLB in software); xedreno is
a userland GPU driver on top of libxenon. None of the three issues a hypervisor
`sc` call — a grep for HV-call sites across all three returns nothing.

Consequence: the retail HV's *syscall ABI* and its *guest interrupt-reflection*
machinery are **not actionable** for this audience. What **is** valuable is the
HV as a "known-good oracle" for the **SoC hardware sequences** that libxenon and
XeLL currently perform with unexplained magic constants — because the HV does
the same hardware pokes, and the decompile shows their structure.

## Documents

| File | Area | Verdict vs. prior art |
|------|------|-----------------------|
| [`memory-map.md`](memory-map.md) | Physical / real-mode address map, MMIO window bases | **Improves** — exact 40-bit SoC/flash/PCI/host-bridge bases; precise RAM window; names libxenon's host-bridge reg |
| [`smp-bringup.md`](smp-bringup.md) | Secondary hardware-thread wake handshake | **Improves (strongest)** — explains the exact magic poke sequence libxenon/XeLL share; adds the reset-vector IPI handshake and park/sleep side |
| [`soc-interrupt-controller.md`](soc-interrupt-controller.md) | Per-CPU interrupt controller @ `0x50000`, FSB IRQ acknowledge | **Improves / fills gap** — libxenon's IRQ handler is a stub; names the controller register block |
| [`l2-lock-and-timebase.md`](l2-lock-and-timebase.md) | L2 cache-as-RAM lock; timebase enable/zero; HDEC | **Fills gap** — all three lack L2 locking and timebase enable (TB is only *read*) |

## Skipped areas (already covered, or not actionable) — with reasons

- **HV syscall interface & dispatch** — *Skipped.* All three projects replace
  the HV and make no `sc` HV-calls, so the dispatch ABI (`sc` → `_SyscallTable`
  → thunk jump table → SRR reflection) is not something they invoke. The syscall
  *table* was still read in full and informs the topical docs (e.g. which SoC
  operations the HV performs), but there is no useful standalone "how to call
  the HV" document for a project that *is* the supervisor.
- **Exception vectors & SPR layout** — *Skipped.* The architected vector offset
  table (`0x100`…`0x1700`) and a full named SPR dictionary already exist in both
  libxenon (`drivers/ppc/xenonsprs.h`, `drivers/ppc/except.S`) and XeLL
  (`source/lv1/xenonsprs.h`, `source/lv1/main.h`). The parts the HV uniquely
  exercises — the H-variant SPRs (`HSPRG0/1`, `HSRR0/1`, `HDSISR`, `HDAR`) and
  the reflect-to-guest-kernel convention — are **hypervisor/guest** concepts.
  Real-mode baremetal deliberately uses the ordinary `SRR0/1`/`DSISR`/`DAR`/
  `SPRG0/1` instead, so the H-SPR semantics are not actionable here. The two
  genuinely useful nuggets were relocated: the **reset-vector (`0x100`) cold-vs-
  IPI-wake handshake** → `smp-bringup.md`; the **hypervisor-decrementer (HDEC)**
  behavior → `l2-lock-and-timebase.md`.
- **Power / reboot / DVFS / SoC-register get-set** — *Skipped.* Power and reboot
  on these systems go through the **SMC** (System Management Controller), which
  libxenon covers well (`drivers/xenon_smc/xenon_smc.c`: shutdown, reboot,
  sensors, LEDs, RTC). The HV does not use the SMC for this; its
  `HvxGetSocRegister`/`HvxSetSocRegister` are privilege-gated wrappers that only
  permit a couple of specific clock registers, which is less capable than
  libxenon's direct MMIO accessors. `HvxSetPowerMode` (CPU clock/voltage/thermal
  throttle) overlaps libxenon's working `xenon_set_speed`, and the HV version is
  deeply entangled with thermal-sensor state and build-version gating — not
  worth the risk of mis-documenting when a working implementation already
  exists. The one real libxenon gap in this area — **FSB interrupt routing**
  (its handler is a stub) — is covered in `soc-interrupt-controller.md`.

## A note on accuracy

Everything here is derived from a *decompilation*, which can misname a register
or invert a bit. Where a value is cross-confirmed by an independent poke in
libxenon or XeLL, that is noted — those are the highest-confidence facts.
Single-source claims from the decompile alone are hedged as such.
