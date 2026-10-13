# Xbox 360 SDK (cl-side) notes

Everything observed about the Microsoft Xbox 360 SDK toolchain while building this tour, with where each fact comes from. "Observed"
means it came from output on the user's machine; "inferred" means deduced from that output; "unknown" means nobody has checked.
Raw evidence: `probe_res/` (inline-asm spelling probe), `sdk_results/` (full `build.bat asm` run), `sdk_results/sdk_compare.csv`.

## Environment (observed)

| | |
|---|---|
| SDK folder | `C:\Program Files (x86)\Microsoft Xbox 360 SDK` (`%XEDK%`) |
| Tools | `bin\win32\cl.exe`, `link.exe`, `imagexex.exe`; `dumpbin.exe` was on `PATH` once `bin\win32` was added |
| Headers | `include\xbox` **before** `include\win32`. `include\win32\xnamath.h` is the PC copy and stops with `C1189 ... does not support Xbox 360` |
| Libraries | `lib\xbox`; `xapilib.lib xboxkrnl.lib` was enough to link a console title that calls `OutputDebugStringA` |
| Compile flags that built | `/nologo /c /Od /W3 /D_XBOX /DNDEBUG`, C (`/TC`) |
| Outputs | `link` -> `tour.exe` (+ `.pe`, `.xdb`), `imagexex /OUT` -> `tour.xex` with no extra config |
| Versions | **unknown**: SDK build number, `cl`, `link`, `imagexex`, `dumpbin` versions were not recorded (see "To record" below) |

## Compiler (observed)

* `__emit(0x7C642A14);` is accepted and emits exactly that word. (`probe_res`, variant 5; also the whole `__emit` tour built.)
* Mnemonic inline assembler: `__asm { add r3, r4, r5 }` and `__asm add r3, r4, r5` are accepted and emit `7C642A14` (variants 1, 2).
* `__asm__ volatile("...")` is rejected (`C2065 '__asm__' : undeclared identifier`). `asm("add 3,4,5")` is **not** inline assembly:
  it compiles as a call to an undeclared function (`lis/addi/bl` in the dump), so "accepted" there is meaningless.
* `tour_main.c` compiles as C with `__try` / `__except (... GetExceptionInformation())`, `EXCEPTION_POINTERS`, `OutputDebugStringA`, `<xtl.h>`.
  (That it *behaves* as intended on a console is not verified, see below.)
* At `/Od` every function with a named parameter starts with `stw r3,14h(r1)` (spill of the parameter) and ends `blr` + one zero pad word
  (2 of 3 functions). Nothing else is added around `__emit` words: byte-exact (`sdk_compare.py`, 635/635).
* The compiler reads registers named in inline asm: functions using r31 (`lmw`/`stmw` tests) got `std r31,-8(r1)` before and `ld r31,-8(r1)` after.
* `-DTOUR_ONLY`-style per-function compiles work; an unused `static` function would be discarded, so the generated asm file makes them external there.
* A `build.bat asm` compile of all 635 mnemonic tests in one file fails on the first rejected mnemonic; per-test isolation (`/DTOUR_ONLY=n`) finds all 80.

## Inline assembler syntax (observed; spelling matches the SDK disassembler, 593 of 623 decodable words, `check_host.py` step 9)

* Registers `r3`, `fr11` (not `f11`), `vr11` (not `v11`), CR fields `cr5`, CR bits `crb21`, FPSCR fields `fpscr1`, `mffs fr11`.
* `mtcrf 3Ch,r4` shown hex with an `h` suffix by the disassembler; immediates elsewhere accepted as decimal or `0x`.
* `vperm128`'s 4th operand is a register (`vr5`); `mftb r3` takes no TBR; `sc` takes no operand.
* Branch targets must be labels (`bc`, `bca`, `bcl`, `bcla`, `ba`, `bla`: `Undefined label`); numeric `.+4` / `0` fail. `b` and `bl` reported *Unknown opcode*.
* `vupkd3d128`'s immediate is not the raw 5-bit field: the disassembler prints `1<NORMSHORT2>` for field value 5, and `vupkd3d128 vr35,vr77,5` assembles to a different word.
* The assembler appends a divide-by-zero check: `twllei r5,0` after `divw`/`divwu`, `tdllei r5,0` after `divd`/`divdu`.
* Rejections come as `C2759: in-line assembler reports: <reason>`:
  * *Unknown opcode* (35): the 22 legacy VMX ops Xenon dropped, `popcntb`, `dcba`, `fre`, `frsqrtes` (and `.` forms), `hrfid`, `tlbiel`, `dstt`, `dststt`, `dssall`, `b`, `bl`.
  * *Opcode not supported by backend* (26): `mcrf`, `crnor crandc crxor crnand crand creqv crorc cror`, `mtcrf`, `isync`, `eieio`, `mtfsb0/1` (+`.`), `mtfsfi` (+`.`),
    `mcrfs`, `lswi`, `stswi`, `rfid`, `slbia`, `tlbia`, `tlbsync`, `mcrxr`.
  * *Unsupported instruction form* (13): `sync`, `dst`, `dstst`, `dss`, `tlbie`, `mtsr`, `mtsrin`, `mfsr`, `mfsrin`, `slbmte`, `slbie`, `slbmfev`, `slbmfee`.
  * Full per-instruction list: `sdk_results/asm_errors.txt` and the "SDK inline assembler" column of `REFERENCE.md`.
* The assembler accepts `lvsl128`/`lvsr128` but encodes them as `0x10642801`/`0x10642849`; the table has `...03`/`...4B` (see README).

## dumpbin /disasm (observed)

* Format `  AAAAAAAA: WWWWWWWW mnemonic    operands`, immediates also in hex with an `h` suffix and `; 0A5A5h` style comments, tabs inside.
* Prints extended mnemonics: `tdeqi`, `tweq`, `cmpdi`, `cmpld`, `beq cr1,...`, `beqlr`, `mfxer`, `mtctr`, vector registers `vr11`, FP `fr11`.
* `???` for words it does not know: `popcntb`, `dcba`, `fre`, `frsqrtes`, `dst`, `dstt`, `dstst`, `dststt`, `dss`, `dssall`.
* Prints `stvlxl`/`stvrxl` as `stvlx`/`stvrx` (keeps `lvlxl`/`lvrxl` and the `128` forms).
* Prints the function name as a label only for some functions; others show `$M<number>:` (e.g. `tour_t0542`). `sdk_compare.py` handles it.
* Branch targets print as absolute addresses or symbol names (`beq cr1,000030A0`, `beqa cr1,tour_vmx_save`).

## Not verified

* **Running `tour.xex` on a console.** Nobody has reported its output. Assumed: exceptions arrive as SEH codes `0xC000001D` (illegal instruction),
  `0xC0000096` (privileged), `0xC0000005` (access violation); function pointers are plain code addresses (used by the opcode-present scan).
* `build.bat` modes `cpp`, `data`, `nopriv`, `sc` (only the default, `probe` and `asm` have been run).
* Behaviour at `/O1`/`/O2` (the tour deliberately uses `/Od` so test bodies start at their first word).
* Whether the title runs from a retail-signed or only devkit image, and what `imagexex` defaults to for the XEX configuration.

## To record (a one-minute job on the SDK machine)

Run each of these once and paste the banner lines into this file or send them over, so results can be tied to a toolchain version:

```
cl
link
imagexex
dumpbin
```
(`cl` with no arguments prints its version line; the others print theirs when run without arguments.) Also the SDK build number, e.g. from the
SDK's `Xbox 360 SDK` entry in Add/Remove Programs or `%XEDK%\bin\win32\xbdm.dll` file properties.
