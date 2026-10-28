# SoC per-CPU interrupt controller & FSB interrupt acknowledge

Source: the SoC MMIO accesses in the HV's processor-startup and FSB-interrupt
routines in `../17559_hv.c`, cross-referenced with libxenon's `wakeup_cpus()`
(`drivers/xenon_soc/xenon_power.c`) and its interrupt handler
(`drivers/ppc/except.S`, which is only a stub), and XeLL's `source/lv1/main.c`.

All offsets are relative to the SoC MMIO base (`0x8000020000000000` real-mode /
`0x20000000000` physical — see `memory-map.md`).

## Why this document exists

libxenon's external-interrupt handler (`ex_interrupt`) is a **stub**: it saves
registers, prints `"Irq"` over the UART, and returns — it does **no** interrupt
decode or routing. The `0x50000`-block writes libxenon *does* make (in
`wakeup_cpus()`) are entirely uncommented magic. XeLL is the same. So the
interrupt controller is a genuine gap/magic area, and the HV touches these exact
registers with enough structure to name the block.

## Interrupt controller block @ `+0x50000` (high confidence)

This block is cross-confirmed by three independent code-bases, so the layout
below is trustworthy even where individual bits are unnamed.

- **Controller base: `+0x50000`.**
- **Per-CPU IRQ-enable regions: stride `0x1000`** (`+0x50000 + cpu*0x1000`).
  libxenon enables delivery to each of the 6 threads by writing, per region,
  `+0x70 = 0x7C`, `+0x08 = 0x7C`, `+0x00 = (1 << cpu)`. The HV does not perform
  this per-CPU enable in its startup path (it is the supervisor enabling IRQ
  *delivery*, which libxenon does for itself), so this part is libxenon-sourced;
  it is recorded here to complete the block's picture.
- **Thread-wake sub-blocks: stride `0x2000`** at `+0x52000` (block A) and
  `+0x54000` (block B). The HV's startup writes `+0x..070 = 0x7C` (arm),
  `+0x..008 = 0`, `+0x..000 = {0x04 | 0x10}` (kick). See the Rosetta-stone table
  in `smp-bringup.md` — these match libxenon and XeLL exactly.
- **IPI trigger: `+0x50010`.** Writing an encoded word here raises the
  inter-processor interrupt. The HV writes `0x140078`; libxenon and XeLL write
  the same `0x140078` to `+0x10` on the cold path. XeLL's JTAG path uses the
  per-block alias `+0x52010` with a different encoded value (`0x3E0078`),
  implying the written word encodes a target-thread/vector selector (the low
  byte `0x78` is constant across both). The decompile does not break out these
  sub-fields, so treat the *encoding* as empirical: reuse the known-good
  constants rather than synthesizing new ones.

### Register role summary (`+0x50000` block)

| Offset | Observed write | Apparent role |
|--------|----------------|---------------|
| `+cpu*0x1000 +0x00` | `1<<cpu` (libxenon) | per-CPU IRQ delivery enable |
| `+cpu*0x1000 +0x08` | `0x7C` (libxenon) | per-CPU priority/ack |
| `+cpu*0x1000 +0x70` | `0x7C` | per-CPU control/arm |
| `+0x52000 / +0x54000` | `0x04 / 0x10` | thread-wake block kick |
| `+0x52008 / +0x54008` | `0` | thread-wake block |
| `+0x52070 / +0x54070` | `0x7C` | thread-wake block arm |
| `+0x50010` | `0x140078` | IPI trigger (target/vector encoded) |

## FSB / bridge interrupt-acknowledge blocks @ `+0x30000` and `+0x40000` (decompile-only — hedged)

The HV's FSB-interrupt routine services front-side-bus / bridge interrupts by
reading and clearing status in two further SoC blocks. These correspond to the
blocks libxenon and XeLL label `soc_29 = +0x30000` and `soc_31 = +0x40000` in
their `init_soc()` but otherwise treat opaquely. **This section is sourced from
the decompile alone and is not cross-confirmed, so treat the specific offsets as
leads to verify, not settled fact.**

Observed accesses (offsets from the SoC base):

| Offset | Observed operation | Apparent role |
|--------|--------------------|---------------|
| `+0x43000` | read, test bit `0x400000000000` | pending/status gate for the bridge block |
| `+0x43010` | read-modify-write (mask in `0x180000000000`) | enable/mask bits |
| `+0x43200` | written `~(value at +0x43208-ish)` then `0` | acknowledge: write-ones-to-clear, then clear |
| `+0x47100` | poll until low nibble `== 0xE` | handshake/ready wait |
| `+0x47120` | write `0x40000000000` | trigger/acknowledge |
| `+0x33000` / `+0x33010` / `+0x33200` | read / mask / clear (`0x30000` block) | the second controller's status/ack, same shape |

The recurring idiom is **write-the-complement-then-zero** at the `+0x3200`
status registers (`reg = ~pending; reg = 0`), i.e. acknowledge all currently
pending sources and then re-arm — a standard write-1-to-clear controller. The
routine is gated on the SoC build/version (it checks a stored version word and
takes different sub-paths below/above `0x710700`), so exact behavior varies by
console revision.

### Actionable for libxenon

- The `0x50000` block in `wakeup_cpus()` is the **interrupt controller**; the
  table above is a starting field key. The per-CPU enable stride is `0x1000`;
  the thread-wake blocks are at `+0x52000`/`+0x54000`.
- If libxenon ever wants a real `ex_interrupt` handler instead of the current
  stub, the acknowledge idiom to implement is **write-ones-to-clear at the
  `+0x3200` status registers of the `+0x30000`/`+0x40000` blocks**, after
  gating on the pending bit at `+0x3000`. Verify the exact offsets against
  hardware on a target revision before relying on them — this half is
  decompile-only.
