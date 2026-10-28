# Physical / real-mode address map

Source: behavior of the address-window helpers and the range checks that appear
throughout `../17559_hv.c`. Cross-checked against libxenon's `drivers/ppc/vm.c`
and XeLL's `source/lv1/main.c` / `source/lv2/linux/xenon.dts`.

## Real-mode addressing convention

The Xenon addresses everything through a 64-bit real-mode space. Bit 63
(`0x8000_0000_0000_0000`) selects the real-mode (MMU-off) view of an address;
the low bits carry a 40-bit physical address. The decompile's physical-to-real
helper is simply "validate the range, then OR in bit 63." Both libxenon
(`hrmor = 1<<63`, and `oris …,0x8000` when dropping to real mode) and XeLL
(`0x8000000000000000 | sh_addr` ELF loads) already use this convention, so it is
not new — it is restated here only because the window bases below are given in
both forms.

For an SoC offset `off`, the two equivalent forms are:

```
real-mode address = 0x8000020000000000 + off      (bit63 set)
physical address  =       0x20000000000 + off      (40-bit)
```

## Main RAM

- **Valid physical RAM window the HV enforces: `[0x00040000, 0x20000000)`** — a
  512 MB space with the **bottom 256 KB (`0x40000`) reserved**. Every HV
  relocate/zero/flush entry point range-checks against exactly this window.
- The reserved low 256 KB is consistent with the console reserving the very
  bottom of RAM; libxenon and XeLL instead describe RAM by *their own* layout
  (XeLL's device tree declares `memory = <0 0 0x1e000000>` = 480 MB and parks
  per-thread stacks at the top of `0x1e000000`). The HV's hard `0x40000` floor
  and `0x20000000` ceiling are the authoritative hardware-level bounds.
- **Per-thread scratch / secondary-hold area lives in the reserved low region**
  near `0x1FFF0`–`0x1FFF8` (see `smp-bringup.md`).

## SoC MMIO and bus windows

The decompile exposes four window-base helpers. Converting each helper's
constant fold gives the following bases (all confirmed against the reference
projects where noted):

| Window | Physical base | Real-mode base | Cross-check |
|--------|---------------|----------------|-------------|
| **SoC MMIO** | `0x20000000000` | `0x8000020000000000` | libxenon `vm.c` maps `0x20000000000` as "CPU stuff", guarded; XeLL comments `0x8000020000000000` |
| **NAND / flash** | `0x200C8000000` | `0x80000200C8000000` | libxenon reads NAND at `0x80000200C8000000` (`xb360.c`); expects logical-NAND signature `0xFF4F` there |
| **PCI config space** | `0x200D0000000` | `0x80000200D0000000` | libxenon `vm.c` nibble `0xd0000000` guarded = "PCI config space" |
| **Host-bridge registers** | `0x200E0000000` | `0x80000200E0000000` | libxenon `vm.c` nibble `0xe0000000` = "PCI space" |

### What this clarifies for libxenon

- libxenon's `vm.c` region table is written at 256 MB (high-nibble) granularity.
  The decompile pins the **exact 40-bit bases** for each window, confirming the
  nibble layout is correct and giving the full-width constants to use when a
  real-mode address must be formed directly.
- libxenon reads the **RAM-size register at `0xE1040000`** (`xb360.c`,
  `xenon_get_ram_size`, with a good comment) but treats the base as a bare
  constant. That address is **host-bridge base `0x200E0000000` + `0x01040000`** —
  i.e. it is a host-bridge register, which names the block libxenon is poking.
- The **southbridge `0xEA00xxxx` device window** (SMC `0xEA001000`, SFCX/NAND
  controller `0xEA00C000`, UART, ATA, OHCI, sound) that libxenon and XeLL use is
  *not* part of the HV's window helpers — the HV reaches those devices by other
  means — so the decompile neither confirms nor documents the `0xEA00xxxx` map.
  That map remains libxenon's own (well-covered) territory.

## Fine-grained SoC MMIO sub-blocks

The SoC MMIO window at `0x20000000000` is internally divided into blocks the
reference projects poke as raw offsets. The decompile gives meaning to several;
each is documented in the topical file where it is used:

| Offset (from SoC base) | Block | Documented in |
|------------------------|-------|---------------|
| `+0x50000` | Per-CPU interrupt controller (IPIs, IRQ routing) | `soc-interrupt-controller.md` |
| `+0x30000`, `+0x40000` | FSB / bridge interrupt-acknowledge blocks | `soc-interrupt-controller.md` |
| `+0x60000` (`+0x60300`) | L2 cache control (way-lock key) | `l2-lock-and-timebase.md` |
| `+0x61000` region | Clock / timebase-enable control | `l2-lock-and-timebase.md` |

## Encrypted / protected address aliases (retail-HV only — informational)

The HV also exposes alternate views of the *same* physical page selected by high
address bits above bit 63's companion field: an "encrypted" view (adds a bit
pattern around bits 40–41 plus a per-page random value in bits ~30–39 acting as
a tweak) and a key-slot-indexed "protected" view (a 6-bit key-slot index shifted
into the address). These drive the Xenon's transparent RAM
encryption/integrity engine.

**This is not usable by baremetal/libxenon/Linux and is listed only so you don't
chase it:** a system that replaces the HV and maps RAM flat never sets these
alias bits, and none of the three reference projects model them (confirmed
absent in all three). They matter only to the retail security model. The cipher
itself is in silicon and is not recoverable from the HV code (which only feeds
the engine a random key and the alias bits).
