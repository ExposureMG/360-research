# XRPC

XRPC is a remote function-call mechanism for Xbox 360 development and hacked consoles. It has no network protocol of its own. A small system plugin on the console (`XRPC.xex`, or its later build `RPC.xex`) runs a thread that checks a 4 KiB mailbox in RAM every 10 ms. The PC writes a function address and up to nine integer arguments into that mailbox using ordinary [XBDM](xbdm.md) memory writes, then reads a 32-bit result back with an XBDM memory read. So an XRPC client is an XBDM client that knows the mailbox layout.

> Status: reconstructed from third-party clients and from analysis of the two server plugin binaries (decrypted, decompressed and disassembled). Nothing here has been verified against hardware. Items marked "Unverified", "Inferred" or "Unknown" are exactly that; the final section collects the open questions.

## Overview

### At a glance

| Item | Value |
|---|---|
| What it is | Call an arbitrary console function by address from a PC, get a 32-bit return value |
| Used for | Running game-engine functions (console commands, server commands, HUD allocation), calling kernel/system exports, popping notification toasts, from trainers and runtime-editing tools |
| Parties | PC tool, XBDM on the console, XRPC plugin thread on the console |
| Transport | None of its own. XBDM over TCP port 730 (`getmem` / `setmem`) |
| Framing | A fixed-layout mailbox in console RAM (section "Mailbox layout"); no messages |
| Endianness | Big-endian (console native) for every field |
| Text encoding | ASCII for C strings; UTF-16BE for wide strings (notification text) |
| Authentication | None beyond what XBDM itself requires |
| Argument slots | 9 slots of 8 bytes each; the server uses only the low 32 bits of each |
| Return value | One 32-bit word (low half of PowerPC `r3`) |
| Floating point | Not supported as argument or return value |
| Poll period (server) | 10 ms |
| Client wait | Fixed 50 ms in the common client; see "Timeouts and timing" |
| Completion signal | None explicit; the command word reads back as 0 once the call has finished (inferred) |
| Error reporting | None. No status field; failures look like stale or zero results |
| Concurrency | One request at a time; no queue |
| Server versions | Build 1 `XRPC.xex` (PE timestamp 2013-01-08), build 2 `RPC.xex` (2013-05-26) |

### Relation to the sibling protocols

| | XRPC (this document) | [JRPC](jrpc.md) | [JRPC2](jrpc2.md) | [XDRPC](xdrpc.md) |
|---|---|---|---|---|
| Carrier | XBDM `setmem`/`getmem` into a fixed mailbox; no command of its own | Own TCP server | XBDM text command `consolefeatures ...` | XBDM, via Microsoft's `xdevkit` / `Microsoft.Test.Xbox.XDRPC` |
| Request encoding | Binary: 9 x 64-bit big-endian slots plus a 32-bit command word in console RAM | See the JRPC document | ASCII `params="A\..."` tokens | See the XDRPC document |
| Result | One 32-bit word at mailbox + 0xFFC | See the JRPC document | Text in the XBDM status line | See the XDRPC document |
| Float / array / 64-bit results | No | - | Yes (type ids 3 and 5-9) | Yes |
| Completion signal | None explicit | - | The XBDM response | The XBDM response |

One thing that is easy to confuse with XRPC: the XBDM text command `consolefeatures ver=2 type=... params="A\..."` is the JRPC2 wire format. It shares no bytes with XRPC.

Some libraries support both XRPC and XDRPC and route typed calls (those returning a value of a given type) to XDRPC, keeping XRPC only for fire-and-forget calls and for the title-ID query. Many tools use XRPC as a fallback when JRPC2 is not installed.

## Background

- **XBDM** (Xbox Debug Monitor) is the debugging service on development kits (and on hacked retail consoles that load `xbdm.xex`). It listens on TCP 730 and offers text commands to read and write memory, list modules and so on. XRPC uses only its memory commands (`getmem`, `getmemex`, `setmem`; exact forms are in the [XBDM](xbdm.md) document). Which commands the Microsoft `xdevkit` library sends for its `GetMemory` / `SetMemory` calls is not visible from client code.
- **xdevkit** is Microsoft's COM library for talking to XBDM from Windows (`IXboxDebugTarget.GetMemory` / `SetMemory`). Most XRPC clients are .NET programs that go through it. A client written from scratch does not need it.
- **Title**: the running game or application. "Title functions" are code inside the running game's image (addresses around 0x82000000 and up). All real-world XRPC calls target such addresses or kernel/system exports.
- **System plugin**: a `.xex` executable loaded at boot alongside the dashboard (on hacked consoles, via the plugin list of a loader such as DashLaunch). The XRPC server is one. It imports only kernel exports (`xboxkrnl.exe`); it does not import `xbdm.xex` or any network API, so it registers no XBDM command and opens no socket. The mailbox is its only input.
- **Kernel export / ordinal**: system modules (`xam.xex`, `xboxkrnl.exe`, ...) publish functions by ordinal number. Build 2 can look up an export address from module name plus ordinal.
- **Calling convention**: the console CPU is a 64-bit big-endian PowerPC. The first eight integer arguments go in registers `r3`-`r10`, further ones on the stack, the integer result comes back in `r3`, floating-point arguments in `f1`-`f13`. XRPC loads integer registers (plus one stack word) and never touches the floating-point registers.

## How it works

### Session lifecycle

1. **Connect XBDM.** Open an XBDM session to the console (TCP 730). Common .NET clients also do `ConnectAsDebugger("XRPC", Force)` if no debugger is attached; the plugin only polls RAM and does not need a debugger, so this is a client habit and not a protocol step (inferred).
2. **Find the mailbox.** Read 4 bytes at **0x91C088AE** (big-endian). That value is the mailbox base address `B`. There is no handshake message. The first time, clients also write 100 zero bytes at `B` and at `B+0x5DC`.
3. **Prepare the request.** Write any string / float-vector / byte data into the scratch areas of the mailbox, then the nine argument slots, then optionally the argument count.
4. **Fire.** Write the function address (the command word) to `B+0` last. The server thread ignores the mailbox while the word is 0 (or below 0x80000000), so writing it last is what triggers the call.
5. **Server executes.** Within 10 ms the poll sees the word, loads the slots into registers, calls the function on its own system thread, stores the 32-bit result at `B+0xFFC`, then zeroes `B[0..0xFFA)`, which also clears the command word.
6. **Read the result.** The common client sleeps 50 ms and reads 4 bytes at `B+0xFFC`. A more careful client polls `B+0` until it reads 0, then reads the result.

### Annotated exchange

Calling `0x824015E0(0, "set x 1")` (an engine "add console command text" function in one game; the address is illustrative). `B` = 0x9A001000 is made up; read the real value.

```
> getmem addr=0x91c088ae length=4             (first call only)  -> 9A001000
> setmem addr=0x9a001000 data=00..00 (100 B)  (first call only)
> setmem addr=0x9a0015dc data=00..00 (100 B)  (first call only)
> setmem addr=0x9a0015dc data=73657420782031            "set x 1", no NUL written
> setmem addr=0x9a001008 data=0000000000000000 000000009a0015dc 0000000000000000 x7
> setmem addr=0x9a001004 data=00000002                  count (server ignores it)
> setmem addr=0x9a001000 data=824015e0                  command word: go
  ... <=10 ms: plugin calls 0x824015E0(r3=0, r4=0x9A0015DC), stores r3 at +0xFFC, zeroes 0x000-0xFF9
> getmem addr=0x9a001ffc length=4                       (50 ms later) -> e.g. 00000000
```

XBDM line shapes shown are inferred; the xdevkit's exact commands are not visible.

## Specification

### Preconditions on the console

| Requirement | Notes |
|---|---|
| XBDM running | A devkit, or the `xbdm.xex` plugin on a hacked retail console |
| `XRPC.xex` **or** `RPC.xex` loaded as a system plugin | Via the loader's plugin slot (for example `Hdd:\XRPC.xex`). Documentation of tools disagrees on which file to install (see Quirks) |
| Only one of the two at a time | Both link at the fixed base 0x91C00000, so they cannot both load at their base (inferred) |
| A running title, if title functions are to be called | Practically every real call target is a title address |

### Mailbox location

| Location | Contents | Note |
|---|---|---|
| 0x91C088AE (unaligned) | `B`, u32 BE | The address clients read. Written by the server on thread start. Lies inside the plugin's image, past the end of its data section |
| 0x91C075C0 (aligned) | `B`, u32 BE | The copy the server's own loop reads. No client uses it; usable as a cross-check (inferred) |
| `B` itself | A region inside the stack frame of the polling thread (frame of 0x1080 bytes in build 1, 0x10C0 in build 2; mailbox at `r1+0x60`) | The mailbox is **stack memory** of a long-lived system thread, so its address depends on where the kernel put that stack (inferred) |

Plugin image facts (both builds): load base 0x91C00000, image size 0x9000, entry 0x91C027B8, internal PE name `tr01.dll`, imports `xboxkrnl.exe` only (build 2 additionally uses `XexGetModuleHandle` #405 and `XexGetProcedureAddress` #407). Import names are inferred from ordinals and call shapes, not from symbols.

An older client field `0x91C0ADCA` lies outside both images and is unused; it probably belongs to an older plugin layout (inferred).

### Mailbox layout (offsets from `B`)

| Offset | Size | Field | Writer | Reader |
|---|---|---|---|---|
| 0x000 | 4 | **Command word** (u32 BE): a function address >= 0x80000000, or a magic value (see Command words) | client, last | server polls |
| 0x004 | 4 | Argument count (i32 BE) | client | **nobody**: neither server build reads it |
| 0x008 | 72 | Argument slots 0-8, 8 bytes each, BE `int64` | client | server reads the **low word only** of each (offsets +0x0C, +0x14, +0x1C, ... +0x4C) |
| 0x050 | 0x58C | unused | - | - |
| 0x5DC | 0x4B0 (1200) | String scratch | client | callee, through a pointer in a slot |
| 0xA8C | 0x1F4 (500) | Float scratch | client | callee |
| 0xC80 | 0x37C (892) | Byte-array scratch | client | callee |
| 0xFFA | 2 | not cleared by the server | - | - |
| 0xFFC | 4 | **Return value** (u32 BE, low word of `r3`) | server | client |
| 0x1000 | - | end of mailbox; beyond it is the server thread's stack frame | - | - |

The scratch regions are not delimited on the wire. They are client conventions; the server just passes slot values through as pointers.

### Server behaviour

| Behaviour | Build 1 `XRPC.xex` | Build 2 `RPC.xex` |
|---|---|---|
| Thread creation | From `DllMain(DLL_PROCESS_ATTACH)`: `ExCreateThread` (#13) with flags 2 (system thread, inferred); entry 0x91C06A48 | Same; entry 0x91C06B58 |
| CPU | Affinity set to hardware thread 4 (`KeSetAffinityThread(1<<4)`, inferred name) | Same |
| Poll period | `Sleep(10)` after every pass, idle or not (`KeDelayExecutionThread(UserMode, FALSE, -ms*10000)`) | Same |
| Idle test | Command word `== 0` or `< 0x80000000` (unsigned): do nothing | Same |
| Command < 0x80000000 | Never executed **and never cleared** (the zeroing runs only after a call) | Same |
| After a call | `B+0xFFC` <- result; `memset(B, 0, 0xFFA)` | Same |
| Concurrency | One request at a time; no queue | Same |
| Initial mailbox contents | Uninitialised stack; the loop starts polling at once | Same |

There is **no completion flag**. The command word goes back to 0 only by the `memset`, which runs after the result is stored. A client can therefore read "word 0 is zero" as "done, result valid" (inferred). The common client does not do this; it sleeps a fixed 50 ms.

### Command words

| Command word at `B+0` | Build 1 `XRPC.xex` | Build 2 `RPC.xex` | Client operation |
|---|---|---|---|
| `0` | idle | idle | - |
| `0x00000001`-`0x7FFFFFFF` | ignored, never cleared | ignored, never cleared | `Call` with a low address (misuse) |
| **`0x82000000`** (SystemCall) | **Not special**: treated as a function address, so it branches into the title image header at 0x82000000. Undefined; expect a crash (inferred) | Calls `slot0` as a function with `slot1`...`slot5` as `r3`...`r7` (**5 arguments max**), on a **new thread**; waits 50 ms; returns its `r3` | `SystemCall`, `CallSysFunction`, `Notify` |
| **`0x82000001`** (ResolveFunction) | **Not special**: same crash path (inferred) | `slot0` = pointer to an ASCII module name, `slot1` = ordinal. Runs `XexGetModuleHandle` (#405) then `XexGetProcedureAddress` (#407) on a new thread; returns the address, or 0 on failure | `ResolveFunction` |
| **`0x816A91F8`** (notify special case) | Runs a fixed function at 0x816A91F8 on a new thread with `(0x22, 0, 2, slot3, 0)`. **The type in slot 0 is ignored; it is always 0x22.** | Resolves `xam.xex` ordinal 656 (0x290, `XNotifyQueueUI`) at run time and calls it with `(slot0, 0, 2, slot3, 0)` | None of the surveyed clients; reached only by calling 0x816A91F8 with `(type, 0, 2, wtext)` |
| any other value >= 0x80000000 | Called directly: `r3`...`r10` = low words of slots 0-7, slot 8's low word on the stack (9th argument word at `84(r1)`); runs **on the plugin's own system thread**; result = low 32 bits of `r3` | Same | `Call` |

0x816A91F8 is presumably where `XNotifyQueueUI` sits in one dashboard version's `xam.xex`; build 2 replaced the hard-coded address with an ordinal lookup (inferred). Build 2's 50 ms wait and build 1/2's notify `Sleep(100)` are described under timing.

### Slot-to-register mapping

| Slot (offset; low word at +4) | Generic call | Build 2 `0x82000000` | Build 2 `0x82000001` | Notify special case |
|---|---|---|---|---|
| 0 (`B+0x08`, low word `+0x0C`) | `r3` | function address | module name pointer | build 1: ignored (type forced to 0x22); build 2: `r3` = type |
| 1 (`+0x10`/`+0x14`) | `r4` | `r3` | ordinal | ignored (`r4` = 0) |
| 2 (`+0x18`/`+0x1C`) | `r5` | `r4` | - | ignored (`r5` = 2) |
| 3 (`+0x20`/`+0x24`) | `r6` | `r5` | - | `r6` = text pointer |
| 4 (`+0x28`/`+0x2C`) | `r7` | `r6` | - | ignored (`r7` = 0) |
| 5 (`+0x30`/`+0x34`) | `r8` | `r7` | - | - |
| 6 (`+0x38`/`+0x3C`) | `r9` | dropped | - | - |
| 7 (`+0x40`/`+0x44`) | `r10` | dropped | - | - |
| 8 (`+0x48`/`+0x4C`) | stack, 9th argument word at `84(r1)` of the dispatcher frame | dropped | - | - |
| FPRs `f1`-`f13` | never loaded | never loaded | - | - |

### Client operations

| Operation | Wire form (writes in order, then read) | Arguments | Response |
|---|---|---|---|
| Locate mailbox | `getmem 0x91C088AE,4` -> `B` | - | `B` |
| Read memory | `getmem addr,len` (plain XBDM; the plugin is not involved) | `uint addr, uint len` | big-endian bytes (one tool reads 48989 bytes in a single call) |
| Write memory | `setmem addr,data` (plain XBDM) | `uint addr, byte[] data` | none |
| Call by address | scratch writes; `setmem B+8` (72 bytes); `setmem B+4` (count); `setmem B+0` = address; wait; `getmem B+0xFFC,4` | up to 9 | u32 |
| System call (build 2 only) | as above with `slot0` = function, command `0x82000000` | function + up to 5 | u32 |
| System call, address separate (build 2 only) | identical wire form to System call, but the count written at `B+4` excludes the address | address + up to 5 | u32 |
| Resolve export (build 2 only) | module name at the current string cursor; `setmem B+8` (**16 bytes**: slot 0 = name pointer, slot 1 = ordinal); `setmem B+0` = `0x82000001`; wait; `getmem B+0xFFC,4`. No count write | module string, ordinal | u32 address, 0 on failure |
| Notify (build 2 only) | resolve `("xam.xex", 0x290)`, then System call `(addr, type, 0, 2, WideChar(text), 0)` | icon type, text | none (return discarded) |

Memory read/write are ordinary XBDM operations and are not XRPC operations proper.

No operation reads the scratch regions back. The server zeroes them right after the call, so **output buffers inside the mailbox are lost**. A function that fills a caller-supplied buffer must be given a pointer outside the mailbox and read back with `getmem` (inferred).

### Argument marshalling

Behaviour of the common .NET client, by argument type (type tests run in this order):

| Argument | What the client does | What the callee receives | Works? |
|---|---|---|---|
| `int`, `uint`, `short`, `ushort`, `sbyte`, `long`, `ulong`, `bool`, `char`, enum, `null` | Converted to a 64-bit integer -> slot (BE int64) | Low 32 bits, zero-extended to 64. `-1` arrives as `0x00000000FFFFFFFF` | Yes for 32-bit values; **the upper half of 64-bit values is dropped** |
| `double`, `decimal` | Converted to integer: **rounded** | An integer, not a float | **No** (silent) |
| `float` | `BitConverter.GetBytes(float)` (**little-endian**, not swapped) written to float scratch; slot = pointer; cursor += 6 | Pointer to 4 byte-swapped float bytes; nothing in `f1` | **No**: wrong byte order, and passed by pointer |
| `float[]` | First **3** elements, each byte-swapped to BE, 12 bytes into float scratch; slot = pointer; cursor += **2** | Pointer to a BE `float[3]` (a vec3) | Yes for vec3 pointers; a second `float[]` overlaps the first (cursor +2); fewer than 3 elements throws |
| `string` | ASCII bytes into string scratch; **no NUL written**; slot = pointer; cursor += len+1 | Pointer to an ASCII C string. The NUL comes from the zeroed mailbox (server zeroes after each call; client zeroes 100 bytes on first use) | Yes, up to 1200 bytes total per call; non-ASCII becomes `?` |
| `byte[]` | Raw bytes into byte scratch; slot = pointer; cursor += len+2 | Pointer to the bytes, followed by zeros | Yes, up to 892 bytes total per call |
| `byte` (single) | Cast to `byte[]` | - | **Throws `InvalidCastException`** |
| `int[]`, `uint[]`, structs, other objects | Converted to a 64-bit integer | - | **Throws** |

Scratch cursors start at `B+0x5DC` (strings), `B+0xA8C` (floats) and `B+0xC80` (bytes) at the beginning of every call.

By-reference / out parameters work only as raw pointers; a pointer into the scratch area is useless for output (zeroed before the client can read it). By-value structs are not supported. Varargs: whatever fits in `r3`-`r10` plus one stack word.

Recommended client behaviour for a fresh implementation:

- Accept only: 32-bit integers (reject 64-bit values that do not fit in 32 bits), NUL-terminated ASCII C strings (append the NUL explicitly), NUL-terminated UTF-16BE wide strings, raw byte buffers, 3-float vectors written as big-endian floats, and caller-owned console pointers.
- Reject `float` / `double` by value with a clear error (the protocol passes no floating-point registers).
- Lay out scratch data yourself and fail before sending anything if a region would overflow.
- Allow 9 arguments for a plain call and 5 for a system call.

### Limits

| Limit | Value | Enforced by |
|---|---|---|
| Arguments, `Call` | 9 (slots 0-8) | Client array of 9; a 10th throws `IndexOutOfRangeException` |
| Arguments, system call (incl. function) | 9 on the client; **function + 5** used by build 2, the rest silently dropped | Server |
| Arguments, system call with separate address | 8 + address on the client; 5 used by build 2 | Server |
| Argument width | 32 bits (low word of each 64-bit slot) | Server |
| String scratch | 1200 bytes (0x5DC-0xA8B), including the +1 gaps | Nothing |
| Float scratch | 500 bytes (0xA8C-0xC7F) | Nothing |
| Byte scratch | 892 bytes (0xC80-0xFFB) | Nothing |
| Overflow | Strings spill into float/byte scratch; byte arrays past 0xFFC clobber the result slot, then (past 0x1000) the **plugin thread's stack frame** (inferred) | Nothing |
| Return | 32 bits | Server |

### Return values

| Requested return | How it is obtained |
|---|---|
| unsigned 32-bit | `getmem B+0xFFC,4`, big-endian |
| signed 32-bit | cast the same word |
| pointer / handle / index | used as the 32-bit word |
| float / double (`f1`) | **Impossible**: the server stores only `r3` |
| 64-bit | **Impossible**: only the low 32 bits are stored |
| string / buffer | Only via a returned pointer plus a separate `getmem` |
| void | Read anyway; callers ignore it |

### Notification icon types

The notify type is passed as an ordinary integer slot. The common client defines 66 named values; the notable ones:

| Value | Name |
|---|---|
| 0 | `XBOX_LOGO` |
| 3 | `NEW_MESSAGE` |
| 14 | `FLASHING_HAPPY_FACE` |
| 27 | `ACHIEVEMENT_UNLOCKED` |
| 31 | `READY_TO_PLAY` |
| 34 (0x22) | `FLASHING_XBOX_CONSOLE` (the value build 1 forces) |
| 76 | `UPDATING` |

Notification text is a UTF-16BE string built by interleaving a 0x00 byte before every character (`00 c 00 c ... 00 00`). It is ASCII only; characters above U+00FF make the client throw.

### Errors and status

The protocol has **no status field**: no error codes, no acknowledgement, no "plugin present" reply. Every failure looks like a stale or zero return value, or an exception from the transport.

| Condition | What happens | What a client sees |
|---|---|---|
| XBDM unreachable / session open fails | - | Common client swallows the error and marks itself not connected; a later call proceeds anyway and throws from the transport |
| Plugin not loaded | 0x91C088AE is unmapped or reads 0 | `B` = 0 -> writes to `0x8`, `0x5DC`, `0x0` ... -> XBDM error (`404 memory not mapped`, see [XBDM](xbdm.md)) surfaced as a transport exception (inferred) |
| Command word < 0x80000000 | Never executed, never cleared | Previous `B+0xFFC` |
| Build 1 given `0x82000000` / `0x82000001` | Branch to 0x82000000 / 0x82000001 | Console or title crash (inferred) |
| Callee crashes | Exception on the plugin's system thread | Console hang or crash (Unknown) |
| Callee runs > 50 ms | Client reads the previous result; the next request's arguments may be written while this one runs, and the later `memset` wipes the new command | Stale value; lost request |
| Build 2 system call / resolve worker runs > 50 ms | Server stores the stale global result; the worker later overwrites the global, so the **next** build-2 system call can return this one's result | Stale value |
| Resolve: module not loaded / ordinal missing | `XexGetModuleHandle` fails or the address is 0 | 0 |
| More than 9 args / single `byte` arg / array arg | - | Client-side exception before anything is sent (except earlier scratch writes) |
| Second client object calls Resolve first | Its mailbox pointer is still 0 | Writes to address 0x5DC etc. |

Suggested client-side error classes: not connected (XBDM), plugin missing (pointer 0 or unreadable), unsupported (system call or resolve against build 1), timeout (command word not cleared in time), bad argument (marshalling rules), not found (resolve returned 0).

### Timeouts and timing

| Item | Value |
|---|---|
| Client wait after the command write (common client) | `Sleep(50)`, then read the result. No retry, no completion check |
| Server pickup latency | 0-10 ms (poll period) |
| Build 2 system call / resolve | Work runs on a new thread; the loop waits a **fixed 50 ms**, closes the handle, then stores whatever is in the result global |
| Notify special case (both builds) | New thread, fixed `Sleep(100)` |
| Race | If the function takes longer than the client's 50 ms (or, on build 2, longer than the server's own 50 ms), the client reads the **previous** result. With build 2's 50 ms plus up to 10 ms pickup, a 50 ms client sleep is borderline; Windows' ~15.6 ms sleep granularity usually stretches it to about 62 ms (inferred) |
| XBDM socket timeouts | Whatever the XBDM client library uses; not visible |

Recommended client behaviour: after writing the command word, poll `getmem B length=4` (for example every 20 ms) until the word reads 0, then read `B+0xFFC`; give up after a configurable timeout (2 s is a reasonable default). For build 2 system calls and resolves the word clears about 50 ms after pickup even if the worker is still running, so the result is only as good as the server's 50 ms window; document that to users.

### Connection lifetime

| Topic | Behaviour |
|---|---|
| Lifetime | The common client keeps one XBDM connection per client object and never closes it explicitly |
| Reconnect | Re-runs the connect routine if the debugger-connected flag turns false |
| Shared state | In the common client the "first run" flag is `static` (shared by all instances) while the mailbox pointer is per instance; a second instance relies on a `B == 0` re-read, which the resolve operation lacks |
| Two clients at once | Nothing arbitrates. Two PCs (or two client objects) writing the same mailbox corrupt each other's requests (inferred) |
| After console reboot | `B` may change; the common client keeps the old one for the life of the process (inferred) |

## Implementing a client

| # | Step | Detail |
|---|---|---|
| 1 | XBDM session | Open a normal XBDM session (see [XBDM](xbdm.md)). No debugger attach is needed |
| 2 | Locate | `getmem 0x91C088AE` (4 bytes, BE) -> `B`; optionally cross-check with 0x91C075C0 (inferred). Re-read `B` for every new XBDM session and after any reconnect or console reboot; do not cache it across sessions |
| 3 | Validate | `B != 0` and `getmem B length=4` succeeds; otherwise the plugin is missing. This is the only "is XRPC installed" test there is |
| 4 | Detect build | `getmem 0x91C06A78 length=4`: `0x63DE88AE` (`ori r30,r30,0x88AE`) = build 1; `0x7D8802A6` (`mflr r12`) = build 2; anything else = unknown, treat as build 1 (plain calls only). Inferred from the two images; assumes the plugin loaded at its base |
| 5 | Wait idle | Poll `B+0` until 0 (a previous request may still be running) |
| 6 | Marshal | Lay out strings at `B+0x5DC`, vec3/float data at `B+0xA8C`, bytes at `B+0xC80`, each with an explicit terminator; build 9 BE `u64` slots with the high word 0 |
| 7 | Write | Scratch data, then slots (`B+8`; large writes may be split, one implementation used at most 64 bytes per `setmem`), then the count (`B+4`, optional), then the command word (`B+0`) **last, in one 4-byte `setmem`** |
| 8 | Complete | Poll `B+0` every ~20 ms until 0, or time out |
| 9 | Result | `getmem B+0xFFC length=4` -> u32 BE |

Further recommendations:

- Offer plain call on both builds; offer system call and resolve only when step 4 found build 2 and never send `0x82000000` / `0x82000001` to build 1.
- Notify = resolve + system call on build 2; refuse it on build 1 (the `0x816A91F8` path depends on the dashboard version).
- Keep one request in flight per console, serialised behind a mutex.
- Results are u32 only; do not promise float, 64-bit or buffer returns.
- Document that plain calls run on the plugin's system thread, not the title's thread. Title functions that rely on title thread-local state may misbehave (inferred).
- Do not write a command word below 0x80000000: it will never be cleared and a completion poll will time out.

### Emulating the server (for testing)

A test double needs an XBDM-compatible memory-backed `getmem`/`setmem` plus a worker that imitates the plugin:

| Obligation | Why |
|---|---|
| Map 0x91C00000-0x91C08FFF; put `B` (BE) at 0x91C088AE and 0x91C075C0; put the build signature word at 0x91C06A78 | Locate and detect steps |
| Allocate 0x1000 bytes at `B`; poll every 10 ms | Matches the real poll period |
| Ignore words 0 and < 0x80000000; never clear them | Real behaviour |
| Dispatch per the command-word table for the chosen build, using a table of fake functions keyed by address; read only the low word of each slot | Real behaviour |
| Store the 32-bit result at `B+0xFFC`, **then** zero `B[0..0xFFA)` | Real ordering |
| Optional: a delay knob to reproduce the >50 ms races; an "unloaded" mode where 0x91C088AE reads 0 | Error paths |

## Worked examples

`B` = 0x9A001000 is **made up** in all examples; the XBDM line forms are inferred.

### Plain call: `Cbuf_AddText`-style engine command

See the annotated exchange in "How it works". Slots: `[0, ptr("set x 1"), 0, 0, 0, 0, 0, 0, 0]`, command word = function address.

### Notify on build 2 (`FLASHING_HAPPY_FACE`, "Hi")

Round trip 1, resolve `("xam.xex", 0x290)`:

```
> setmem addr=0x9a0015dc data=78616d2e786578                        "xam.xex"
> setmem addr=0x9a001008 data=000000009a0015dc 0000000000000290     only 16 bytes
> setmem addr=0x9a001000 data=82000001
  ... plugin: worker thread -> XexGetModuleHandle("xam.xex"), XexGetProcedureAddress(h, 656); Sleep(50); store
> getmem addr=0x9a001ffc length=4                                   -> A (XNotifyQueueUI)
```

Round trip 2, system call `(A, 14, 0, 2, WideChar("Hi"), 0)`:

```
> setmem addr=0x9a001c80 data=004800690000                         UTF-16BE "Hi\0"
> setmem addr=0x9a001008 data=<A as int64> 000000000000000e 0000000000000000 0000000000000002 000000009a001c80 0000000000000000 0000000000000000 x3
> setmem addr=0x9a001004 data=00000006
> setmem addr=0x9a001000 data=82000000
  ... plugin: worker thread -> A(14, 0, 2, 0x9A001C80, 0); Sleep(50); store; memset
> getmem addr=0x9a001ffc length=4
```

On build 1 the first round trip already branches to 0x82000001 (see Command words). Notify through this route needs build 2.

### Notify without system call (special-case address)

`Call(0x816A91F8, 0, 0, 2, WideChar("Hi"), 0)` reaches the special case: on build 1 a worker thread runs `0x816A91F8(0x22, 0, 2, B+0xC80, 0)` then `Sleep(100)`. The icon is always 0x22 and the address only fits the dashboard version it was built for (inferred). On build 2 the same request honours the type in slot 0 and resolves the address itself. No surveyed client sends it.

### Call that returns a pointer

`Call(elem_alloc, client, 0)` -> slots `[client, 0]`, command = allocator address; the u32 read from `+0xFFC` is the new HUD element's address, which the tool then fills in with ordinary memory writes.

### Typical call patterns in real tools

All targets are game functions, so addresses are per game build.

| Pattern | Arguments | Return used? |
|---|---|---|
| Engine console command `Cbuf_AddText(localClient, text)` | `0, string` | no |
| `Cbuf_Execute` with one dummy argument | `{ null }` -> slot 0 = 0 | no |
| Server command to client(s) `SV_GameSendServerCommand(client or -1, 0 or 1, text)` | int (-1 = everyone), int, string | no |
| Stat commands as strings | `0, "setStatFromLocString ..."` / `"statWriteDDL ..."` | cast to int, discarded |
| Index lookup by name (`G_MaterialIndex`, `G_LocalizedStringIndex`) | one string | yes (index) |
| Allocate object (`HudElem_Alloc(client, 0)`) | two ints | yes (pointer) |
| Zero-argument call | none | no |
| Title ID (`XamGetCurrentTitleId`) | system call on `resolve("xam.xex", 463)` | yes |
| Toast on connect | `Notify(type, text)` | no |
| Memory only (no RPC at all) | `getmem` / `setmem` | - |
| Large blobs | one `getmem` / `setmem` of ~49 KB | - |

Many trainers use only the memory primitives and never call anything. Nobody passes `float` or `double` to a call; float data (positions, HUD sizes) is written with plain memory writes instead, which fits the server's lack of floating-point register support.

### Typed memory helpers (add-on library)

One add-on library layers typed access on the plain memory primitives: reads and writes of 1/2/4/8 bytes with byte reversal, `ReadString` reading a fixed buffer (default 32 bytes) and cutting at the first NUL, an optional cache invalidation before reads, and read-modify-write `XOR_` / `AND_` / `OR_` helpers. These are pure XBDM memory traffic, not XRPC. Obfuscated copies of the same helpers appear in several tools.

## Quirks and known inconsistencies

| Quirk | Detail |
|---|---|
| Single `byte` argument | Throws (boxed `byte` is cast to `byte[]`) |
| `float` argument | Written little-endian into big-endian memory and passed by pointer, so it can never work |
| `float[]` cursor | Advances by 2 instead of 12; only 3 elements are used |
| `double` | Silently rounded to an integer |
| Strings | Not NUL-terminated by the client; relies on pre-zeroed memory |
| Fixed `Sleep(50)` | Instead of a completion check |
| Resolve operation | Neither connects nor resets the string cursor; repeated calls walk the cursor through the mailbox toward the server's stack |
| Static `firstRan` | Shared across client instances while the mailbox pointer is per instance |
| Connect | Succeeds without checking that the plugin is present |
| Cache invalidation | The common client invalidates the xdevkit cache **after** a read; the add-on library does it **before**, optionally |
| One wrapper library | Its `ReadMemory` over XRPC discards the result and returns an empty buffer |
| Add-on library `XOR_Uint16` | Writes the operand instead of the XOR result |
| Build 2 50 ms join | `SystemCall` / `ResolveFunction` can return the previous call's result |
| Argument count | Written by clients, read by no server |
| Build 1 vs. the current client | The client's system call, resolve and notify need build 2; build 1 treats those command words as addresses (inferred) |
| Deployment advice | Implementations disagree: one set of trainers tells users to install `XRPC.xex` (their trainers only need memory access, so build 1 is enough); a plugin-set config loads `XRPC.xex` while its readme says `RPC.xex`; another tool tells the user to install `RPC.xex` |
| Version | The common client carries `currentVersion = "1.7"` and an assembly name of "JtagRPC" (2013); the version is never sent and there is no version handshake |
| Embedded interop | The client DLL embeds its own copy of the xdevkit COM interop types so tools can reach the debug target without referencing the Microsoft library; the xdevkit COM server must still be installed |
| COM activation | Implementations differ (`Activator.CreateInstance` on the XboxManager CLSID versus constructing the managed wrapper directly); irrelevant on the wire |
| Console selection | The common client opens the "default console" configured in the Microsoft tooling; no IP address is passed |

## Open questions

| # | Question | Why it matters |
|---|---|---|
| 1 | Where does the polling thread's stack (`B`) land, and is it stable across boots and plugin load orders? Unknown | Whether `B` may be cached at all |
| 2 | Does XBDM `setmem` write a 4-byte aligned word atomically, or byte by byte? If byte by byte, the plugin could see a half-written command word (for example `0x82000000` while an ordinary address is being written). Unknown | Possible misfire on build 2 |
| 3 | What happens when a called function faults on the plugin's system thread (title crash, console hang, XBDM exception notification)? Unknown | Error reporting |
| 4 | Which process does build 2's worker thread (created without the system flag) belong to, and can it call title code safely? Unknown | System-call semantics |
| 5 | Is 0x816A91F8 `XNotifyQueueUI` on a specific dashboard, and which one? Unknown | Whether build 1's notify path is usable at all |
| 6 | Can both builds be listed as plugins (same base 0x91C00000)? What does the loader do with the second? Unknown | Build detection |
| 7 | Do the kernel imports really have the names inferred here (#13 `ExCreateThread`, #90 `KeDelayExecutionThread`, #151 `KeSetAffinityThread`, #405 / #407 `XexGetModuleHandle` / `XexGetProcedureAddress`)? Unverified | Only affects the explanations, not the wire format |
| 8 | Which XBDM memory commands does xdevkit actually send for `GetMemory` / `SetMemory`, and in what chunk sizes? Unknown | Only for reproducing existing client traffic byte for byte |
