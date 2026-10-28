# XDRPC

XDRPC is Microsoft's remote procedure call for the Xbox 360 development kit. A PC tool uses it to run an arbitrary function on the console (a kernel export, a system-library export, or any code address), passing up to 8 integer-class arguments and some floating-point arguments, and getting back the return value plus the contents of every buffer, string and struct the function was allowed to write. It has no socket of its own: each call is one text command, `rpc`, sent to the console's debug monitor ([XBDM](xbdm.md), TCP 730), followed by one binary upload (the call buffer) and one binary download (the same buffer, after the call).

> **Status.** This document was reconstructed from third-party tools, from analysis of the SDK's .NET client library (`xdrpc.dll`, namespace `Microsoft.Test.Xbox.XDRPC`, version 1.7.1.0), and from a disassembly of one third-party console-side implementation (an `XDRPC.xex` plugin). Nothing has been verified against hardware. Statements marked "inferred" are deduced from client code, not observed; "plugin" means that one console-side implementation, not Microsoft's `xbdm.xex`. Unknowns are collected in [Open questions](#open-questions).

## Overview

### What it is for

Calling console functions that XBDM has no command for: asking the kernel to load a module, reading the current title id from `xam.xex`, driving the party / friends APIs, running a small stub of PowerPC code that was previously written into memory, and so on. Third-party tools mostly use it for these one-off calls; the SDK library also uses it internally to allocate and write console memory.

### Who talks to whom

```
PC tool (xdrpc.dll or any re-implementation)          Console
        |                                               |
        |-- TCP 730, new connection -------------------|  xbdm.xex (or xbdm + XDRPC.xex plugin)
        |<-- 201- connected                             |
        |-- rpc <system|title> version=4 buf_size=... ->|
        |<-- 204- buf_addr=<hex>                        |
        |-- <buf_size bytes: call buffer> ------------->|
        |<-- 203- <text>                                |
        |<-- 8-byte status qwords: 997 ... then 0       |   (function runs meanwhile)
        |<-- <buf_size bytes: call buffer, updated>     |
        |-- (bye, close) -------------------------------|
```

### At a glance

| Item | Value |
|---|---|
| Transport | [XBDM](xbdm.md) over TCP, port 730 |
| Carrier command | `rpc` (text), then raw binary blobs using XBDM's `204` (client uploads) and `203` (console sends) binary framing |
| Connection use | One fresh XBDM connection per call; never reused for a second call |
| Protocol version | 4 (`version=4`, major version only) |
| Framing of the binary parts | No length prefix: both sides know the length from `buf_size` |
| Byte order | Big-endian everywhere (slots, integers, floats, UTF-16 strings) |
| Slot size | 8 bytes; header is 8 slots (0x40 bytes) |
| Max integer-class argument slots | 8 (r3-r10) |
| Max floating-point arguments | 8 by value (at most f1-f8 are loaded by the plugin) |
| Return value | 64-bit r3, or f1 as an IEEE-754 double |
| Text encoding | ASCII for module / function names and default strings; UTF-16 is sent big-endian |
| Authentication | None (XBDM itself has none on a devkit) |
| Default call timeout (SDK client) | 5 minutes, checked only when a `997` status arrives |
| Connect retry (SDK client) | Only on "cannot connect": 3 retries, 100 ms apart |
| Default processor | 5 (hardware thread index) |
| Server implementations | Native `rpc` command in some Microsoft `xbdm.xex` builds (inferred); the `XDRPC.xex` plugin that patches an `rpc` entry into a running xbdm on RGH/JTAG consoles |

### Relation to the sibling protocols

| Protocol | Transport | How a call travels | Relation to XDRPC |
|---|---|---|---|
| **XDRPC** (this document) | XBDM TCP 730, command `rpc`, one connection per call | Binary call buffer (8-byte big-endian slots) uploaded after `204`, returned after `203` | - |
| [JRPC2](jrpc2.md) | XBDM TCP 730, command `consolefeatures ver=2` | Arguments as text tokens in the command line, result in the status line | Different command and encoding. The same `XDRPC.xex` plugin also contains a `consolefeatures` handler (section 7.4). Both protocols use the word `buf_addr`, with different meanings (section 7.4) |
| [JRPC](jrpc.md) (`JRPC.xex`) | Its own TCP server | Own framing | Unrelated transport |
| [XRPC](xrpc.md) (`XRPC.dll` / `XRPC.xex`) | Own plugin protocol | Own framing | Unrelated. Some wrapper libraries support XRPC and XDRPC side by side; XRPC-only back ends there report generic "call function" as unsupported |

## Background

**Development kit, XBDM.** An Xbox 360 development kit runs a debug monitor, `xbdm.xex`, which listens on TCP 730 and accepts line-based text commands (memory read/write, file transfer, module listing, reboot...). Some commands transfer binary data after the command line. See [XBDM](xbdm.md) for the framing; the parts XDRPC needs are summarised in section 1.1. Retail consoles with the RGH / JTAG exploits can run an XBDM plugin that adds extra commands; the `XDRPC.xex` plugin is such a plugin.

**Modules, exports, ordinals.** Code on the console is organised as modules: `xboxkrnl.exe` (the kernel), `xam.xex` (the system application layer), `xbdm.xex` (the debug monitor), title executables, and so on. A module exports functions by ordinal number (and sometimes by name). XDRPC addresses a function as `(module name, ordinal)`, `(module name, export name)` or a raw 32-bit address.

**System vs title context.** The console runs the foreground *title* (the game or app) and the system software. A call can be executed on a thread in the title's context (`title`) or on a system-process thread (`system`). The distinction matters for APIs that depend on the running title.

**Hardware threads.** The console CPU has 3 cores with 2 hardware threads each, so 6 hardware threads (indices 0-5). XDRPC's `processor` parameter selects one of them for the worker thread.

**PowerPC calling convention (as relevant).** Xbox 360 code is big-endian PowerPC. Integer and pointer arguments go in registers r3-r10 in order, floating-point arguments in f1-f13, and the return value comes back in r3 (integer, up to 64 bits) or f1 (floating point). Pointers are 32-bit console addresses. XDRPC loads 8 integer slots into r3-r10 and its floating-point values into f1 onward, calls the target, and stores r3 or f1.

**Call buffer.** XDRPC copies everything the call needs (header, argument slots, strings, struct images) into one contiguous console-memory buffer, rewrites argument pointers so they point *into that buffer*, runs the function, and sends the buffer back. Output parameters are simply regions of that buffer that the called function wrote to.

**HRESULTs and XBDM status codes.** XBDM reports errors as a three-digit text code (`4xx`) that clients map to `0x82DA0000 + (code - 400)` (facility `0x2DA`). Win32-style codes (`0x8007007E` module not found, `0x8007007F` procedure not found) also appear, inside the call buffer.

## How it works

### Session lifecycle

1. **Describe the call** (client-side): target, mode (`system` / `title`), arguments, return type, optional post-call, timeout.
2. **Size the buffer**: header + argument slots + float values + one data area per argument + one reference area per argument; the total is `buf_size`. Pointers cannot be written yet because the console address of the buffer is not known.
3. **Connect** to TCP 730 and read the `201- connected` greeting.
4. **Send the command**: `rpc <system|title> version=4 buf_size=<n> processor=<n> thread=<name>`.
5. **Console answers `204- buf_addr=<hex>`**: it has allocated a buffer of that size at that address and waits for exactly `buf_size` raw bytes.
6. **Client fills the buffer**: every pointer slot becomes `buf_addr + offset`; the client then uploads the buffer.
7. **Console answers `203- <text>`** (binary response follows) and starts the call on a worker thread.
8. **Console streams 8-byte big-endian status qwords**: `997` ("still running") repeatedly, then `0` ("finished").
9. **Console sends the whole buffer back**, `buf_size` bytes. The call status is at offset 0, the return value at offset 8, and the by-reference areas hold what the function wrote.
10. **Client closes** the connection (optionally after `bye`).

### Annotated exchange

```
C: rpc title version=4 buf_size=88 processor=5 thread=\r\n
S: 204- buf_addr=8D0A1000\r\n                 ready for 88 bytes at 0x8D0A1000
C: <88 bytes>                                 header, 3 argument slots (raw, no framing)
S: 203- <text>\r\n                            binary response follows (text not significant)
S: 00 00 00 00 00 00 03 E5                    997: still running (zero or more times)
S: 00 00 00 00 00 00 00 00                    0: finished
S: <88 bytes>                                 call buffer after the call
```

A command the console refuses is answered with a text status line instead of `204`:

```
C: rpc title version=3 buf_size=88 processor=5 thread=
S: 200- error=Version mismatch, expected 4 but got 3
```

### What the console does (plugin behaviour)

In the plugin implementation, the `rpc` handler allocates `buf_size + 104` bytes (pool tag `'drpc'`), creates the worker thread suspended, and returns `buf_addr` pointing 104 bytes into the allocation. The 104 bytes before the buffer are the plugin's private context. XBDM delivers the upload in chunks straight into the buffer; when the last byte arrives the plugin resumes the worker thread. The worker resolves the target, loads r3-r10 and f1-f8, calls the function, and stores the result. Meanwhile XBDM's "send back" callback emits a `997` qword about every 100 ms; once the worker is done it emits `0` and then the whole buffer, and frees the allocation. Details are in section 7.

## Specification

Section numbers below are subsections of this specification.

### 1. Transport and framing

#### 1.1 Connection lifetime

The XBDM facts XDRPC relies on: a connection starts with the console sending `201- connected`; every command is a text line terminated `\r\n`; each answer is a status line `NNN- text`. A `204` answer means "send binary data now" and is followed by the client's raw bytes; a `203` answer means "binary data from the console follows" and the data comes right after the line. A `4xx` answer is an error (XDevkit raises it as a COM error). `bye` closes politely and is answered `200- bye`. See [XBDM](xbdm.md) for details.

| Step | Client action | Wire | Notes |
|---|---|---|---|
| 1 | Open a new XBDM connection | New TCP connection to port 730, `201- connected` greeting | **A fresh connection per call.** On "cannot connect" (COM error 0x82DA0100) the SDK client retries at most 3 times, 100 ms apart |
| 2 | Send the `rpc` command line | Command line, answer `204- buf_addr=<hex>` | Section 1.3 |
| 3 | Send the call buffer | Exactly `buf_size` raw bytes | XBDM binary framing; no length prefix |
| 4 | Read the status line | `203- ...` | The code must be 203; the text is not checked |
| 5 | Read 8-byte units in a loop | 8-byte big-endian status qwords: 997 while running, then 0 | Section 1.4 |
| 6 | Read `buf_size` bytes | The whole call buffer, updated | A short read is an error |
| 7 | Close the connection | Whether the SDK client sends `bye` first is unknown | Closed on success and on failure |

There is no keepalive and no pipelining: one `rpc` per connection, and the connection is never reused for a second call. Calls are synchronous: the client blocks in steps 5-6 until the function returns or the timeout fires.

**Recommended client behaviour.** Use a dedicated XBDM connection for each call (not the connection used for file and memory work), read the greeting, run steps 2-6, then send `bye` and close without waiting long for `200- bye`.

#### 1.2 Command line

```
rpc <system|title> version=4 buf_size=<decimal> processor=<decimal> thread=<name>
```

The SDK client builds it with the format `rpc {0} version={1} buf_size={2} processor={3} thread={4}`:

| Token | Value | Console (plugin) |
|---|---|---|
| `rpc` | Command name | The plugin writes the string "rpc" and its handler address into the command table of a running xbdm (section 7.5) |
| `system` / `title` | Bare flag. The SDK sends `title` for title mode and `system` for anything else | `system` present: the worker is created as a system-process thread. Otherwise `title` must be present. Neither: error `423` (0x82DA0017, INVALIDARG) |
| `version=4` | Protocol major version | Missing: `200- error=Version is not specified, expecting major version 4`. Not 4: `200- error=Version mismatch, expected 4 but got <n>` |
| `buf_size=` | Total call-buffer bytes, decimal | Missing: `423`. The plugin allocates `buf_size + 104` bytes (pool tag `'drpc'`); failure gives `428` (0x82DA001C, NOMEMORY) |
| `processor=` | Hardware thread index, default 5 | Missing: 5. Used as affinity bit `1 << n` in the top byte of the thread-creation flags |
| `thread=` | Thread name, or empty. **The SDK always sends the key**; value unquoted | Read into a 64-byte buffer (max 64 bytes). Empty: a new thread runs the call. Non-empty: a different path is taken, see below |

- Every value is decimal: no `0x`, no quotes. A thread name containing a space would break the line (no quoting; inferred).
- The SDK library (1.7.1.0) **always sends `processor=5`**: every constructor overload that takes a `processor` argument forwards the constant 5 instead (inferred library bug). A client may send other values, but no client does.
- The SDK's thread-name constructors force `system` mode, so `thread=<name>` is always sent with `system`.
- **Named threads.** In the plugin, a non-empty thread name changes how the end of the upload is handled: the worker is resumed and its handle closed, and the disassembly notes do not show the result-streaming callback being installed in that branch. Whether a named-thread call can complete on the plugin is unclear; Microsoft's semantics are unknown.
- `processor` accepted range: 0-5 is natural. Whether 6 and 7 are refused is unknown.

**Recommended client behaviour.** Send exactly this token order, lower case, with `thread=` empty and `processor` from the call options (default 5).

#### 1.3 Answer to the command line

| Answer | Meaning | SDK client behaviour | Console (plugin) |
|---|---|---|---|
| `204- buf_addr=<hex>` | Ready for `buf_size` bytes; `<hex>` is the console address of the call buffer | Regex `^(\d+)-\s+buf_addr=([a-f\d]+)` (case-insensitive), hex without `0x`; the code must be 204, else "not supported" | `buf_addr=%p` with the pointer 104 bytes into the allocation, status 0x02DA0004 (the code behind `204`, "ready for binary") |
| `200- error=<text>` | Refused with a reason | When the code is 200: invalid-response error carrying `<text>` | Version texts (section 1.2); status 0x02DA0000 with an error text becomes a `200-` line |
| `2xx- buf_addr=<hex>` with a code other than 204 | Not a usable answer | "Not supported" error with the line | Not seen |
| Any other line that does not match the `buf_addr=` regex | Not a usable answer | Invalid-response error with the line | Not seen |
| `407` (COM error 0x82DA0007) | `rpc` is not a known command | "Not supported" error: "RPC command not registered in Xbox's XBDM." | An xbdm without `rpc`, or an RGH/JTAG console without the plugin loaded |
| `423` (0x82DA0017) | Invalid argument | Invalid-response error: "Invalid argument." | Mode or `buf_size` missing |
| `428` (0x82DA001C) | No memory | Invalid-response error: "Requested buffer size (<n>) too large for XBDM." | Call-buffer allocation failed |
| Any other `4xx` | Other XBDM error | The COM exception is rethrown unchanged (not an XDRPC exception) | Not seen |

- XDevkit turns a `4xx` line into a COM error; the three constants the SDK knows fit `0x82DA0000 + (code - 400)` (407 gives 0x07, 423 gives 0x17, 428 gives 0x1C; inferred).
- `buf_addr` is parsed as `[a-f\d]+` without `0x`. A server that sent `buf_addr=0x8D0A1000` would match only the `0`, and the client would build every pointer relative to address 0 (inferred). More than 8 hex digits overflow the SDK's `uint.Parse` and throw a plain .NET `OverflowException` (inferred).
- **The upload is built only after this answer**: every pointer slot is an absolute console address `buf_addr + offset`, so the client sizes the buffer before the command and fills it after.
- The SDK has an offline test switch (`NoDevkit`): it opens no connection and fakes `204- buf_addr=80000000` and `203- OK`; it never polls or downloads, so the result is always status 0, return 0.

**Recommended client behaviour.** Accept `204- buf_addr=<1 to 8 hex digits>` (tolerate an optional `0x`); map `200- error=<text>` to an error carrying the text, `407` to "XDRPC not supported", `423` and `428` to typed errors, other `4xx` to a generic XBDM error, and treat anything else as a protocol error that closes the connection.

#### 1.4 Completion: status polling, timeouts, crashes

After the upload the console answers one status line, then streams 8-byte status qwords until the call has finished, then the whole buffer.

| Step | Wire (console to client) | SDK client behaviour | Console (plugin) |
|---|---|---|---|
| a | `203- <text>` | Code must be 203, text unchecked. A failing receive (HRESULT < 0) gives an "invocation failed" error ("Receive Status Response failed <hr:X8>"); another code or a line not shaped `DDD- ` gives an invalid-response error | Status 0x02DA0003 (the code behind `203`, "binary response") is returned when the last upload byte arrives. The exact text is not known |
| b | 8 bytes, big-endian `997` = `00 00 00 00 00 00 03 E5` (`ERROR_IO_PENDING`) | Call still running: check the timeout, read again | While the worker runs, the send-back callback waits up to about 100 ms (10 ms sleeps) and then emits one `997` qword. Repeated until the worker finishes. A call that finishes before the first pass may produce no `997` at all |
| c | 8 bytes, big-endian `0` (`ERROR_SUCCESS`) | Leave the loop, go to step e | Emitted on the pass after the worker has set its completion flag |
| d | 8 bytes, any other value | Invalid-response error "Received invalid status from console: <decimal>"; the buffer is **not** read | Not seen; which values occur is unknown |
| - | Fewer than 8 bytes in one receive | Invalid-response error "Unexpected binary chunk size received from console" | - |
| e | Exactly `buf_size` bytes: the call buffer after the call | Fewer bytes: "invocation failed" error "Buffer returned is of unexpected size" | The following pass sends `buf_size` bytes from the start of the call buffer (in chunks), then frees the allocation and ends the transfer (internal code 0x82DA0104) |
| f | - | Connection closed in a `finally`, success or not | - |

The polled qword (steps b-d) is a transport status. The **call** status is the qword at offset +0 of the returned buffer, checked only after step e (section 6).

| Timer | Value |
|---|---|
| Connect retry | On COM error 0x82DA0100 only: 3 retries, 100 ms apart, then an error "Unable to connect to console. Verify console is not crashed." |
| Execution timeout | `ExecutionTimeoutPeriod`, default **5 minutes**; every constructor overload with a timeout sets it |
| Timer start | When polling starts (after the upload and the `203`), not at connect |
| When it is checked | **Only when a `997` qword arrives**: elapsed > timeout gives a timeout error (message suggests a longer `executionTimeoutPeriod`; carries start time, current time and period) |
| Console silent | The client blocks inside XDevkit's receive; XDevkit's own socket timeout applies. Its value is unknown |
| Console-side pacing | About one `997` per 100 ms while the function runs (plugin), which is what drives the client's timeout check |

| Situation | What the SDK client sees |
|---|---|
| Timeout fires | Timeout error, connection closed. Nothing tells the console to stop; whether the called function keeps running is unknown (the plugin has no cancel path in the handler that was disassembled) |
| Called function hangs and the console stops sending `997` | No client-side timeout beyond XDevkit's socket timeout (inferred) |
| Called function crashes the title or console | Connection drops: the receive fails with a COM error, rethrown unwrapped (inferred); the next call's connect retries 3 times and reports "Verify console is not crashed" |
| Exception text containing `0x82DA0101` | Some tools retry the whole XDRPC call sequence in a loop on that substring (the same idiom wraps their JRPC2 code). The code's XBDM name is not known |

**Recommended client behaviour.** Give every call one deadline (default 5 minutes, settable per call) enforced by the socket reads themselves, so a console that goes silent also times out. `997` continues the loop; `0` moves on to the buffer read under the same deadline; any other value is an error carrying that value; a short qword is a protocol error. After any failure close the connection and do not reuse it. Retry only the connect (3 x 100 ms). **Never retry a call automatically**: it may already have had side effects.

### 2. Execution options, modes and post-call

The SDK's `XDRPCExecutionOptions` says *what* to call and *how*; the arguments are separate (section 4).

#### 2.1 Fields and their wire effect

| Property | Wire effect | Default |
|---|---|---|
| `Mode` (`XDRPCMode`: `System` = 0, `Title` = 1) | `system` / `title` token (section 1.2) | `System` (parameterless constructor); thread-name constructors force `System` |
| `ModuleName` | ASCII string in a data area, its address at +0x30. Empty: +0x30 = 0 and the call is by address | `""` |
| `Ordinal` (int) | +0x38, sign-extended, when a module is given and `FunctionName` is empty | 0 |
| `FunctionName` | ASCII string in a data area, its address at +0x38 | `""` |
| `FunctionAddress` (uint) | +0x38, zero-extended, when `ModuleName` is empty | 0 |
| `Processor` | `processor=` | Always 5 (section 1.2) |
| `ThreadName` | `thread=` | `""` |
| `PostMethodCall` (`None` = 0, `GetLastError` = 1) | +0x10 (section 2.4) | `None` |
| `ExecutionTimeoutPeriod` | None (client only, section 1.4) | 5 minutes |

In the SDK only `Mode` has a public setter; everything else is fixed by the constructor.

#### 2.2 Target selection

The SDK offers constructor families `(mode | threadName)` x `(module, ordinal | module, functionName | functionAddress)` plus optional `postMethodCall` and `timeout` (and a `processor` argument that is ignored). The selection rule, applied in the last private constructor:

| `ModuleName` | `FunctionName` | Call kind | +0x30 | +0x38 |
|---|---|---|---|---|
| non-empty | empty | By ordinal | address of module string | ordinal |
| non-empty | non-empty | By export name | address of module string | address of name string |
| empty | (ignored; inferred) | By address | 0 | function address |

- Nothing on the wire tells the console whether +0x38 is an ordinal or a name pointer. A console could only tell them apart by value (inferred: the Win32 `GetProcAddress` convention, a value below 0x10000 is an ordinal). **The plugin does not do this**: whenever a module is given it treats +0x38 as an ordinal and resolves it with the kernel's procedure-address lookup, so call-by-name appears not to work on the plugin (it would fail with 0x8007007F; inferred).
- Module + ordinal 0 + no name is a call of ordinal 0 (inferred); the client does not validate ordinals, names or addresses.

#### 2.3 Modes

| Mode | Token | Used for | Meaning |
|---|---|---|---|
| `System` | `system` | The SDK library's own helpers into `xbdm.xex`; module calls into the kernel and `xam.xex` that do not depend on the title; every named-thread call | Plugin: the worker thread is created as a system-process thread (thread-creation flag 2) |
| `Title` | `title` | Party and friends APIs; calls by address into title code; hypervisor stubs | Plugin: ordinary thread; inferred: the call runs in the running title's context |

The SDK's `AllocateRPC(size, mode)` passes its mode on, so pool memory can be taken in either context.

#### 2.4 Post-method call

| `PostMethodCall` | +0x10 on upload | +0x10 after the call | SDK returns |
|---|---|---|---|
| `None` (0) | 0 | Not read | `postMethodCallReturn = 0` |
| `GetLastError` (1) | 1 | The 64-bit value left there by the console | Through the `out ulong postMethodCallReturn` overloads |

The convenience overloads that take only an `XDRPCMode` have no `out` form: the post-call value is reachable only through a thread name or an explicit options object. In the plugin, when +0x10 equals 1 it calls `GetLastError()` right after the function returns and stores the result at +0x10. No known tool uses the feature.

#### 2.5 Events (client only)

The SDK's `Invoke` raises, in this order: `OnConnectRetry(n)` per connect retry, `OnConnected(connectionId)`, `OnRPCConnectionReady(buf_addr)`, `OnBufferPrepared(buf_size)`, `OnDataSent`, `OnWaitComplete`, `OnDataReceived`, `OnConnectionClosed`, then `OnStatusReceived(low 32 bits of +0x00)` **before** the status is validated. No wire effect.

**Recommended client behaviour.** Expose the target (ordinal, export name or address), the mode (`system` / `title`, required, not inferred from the target), the processor (default 5), a deadline (default 5 minutes) and the post-call option (`none` / `getlasterror`). Named threads and events are not needed.

### 3. Call buffer layout

One contiguous buffer, built by the client, uploaded after `204`, executed in place, downloaded after the final `0` status. All slots are **8-byte big-endian**; the buffer is zero-initialised before anything is written, so padding and string terminators are zero bytes.

#### 3.1 Regions

| Region | Offset | Size |
|---|---|---|
| Header | 0x00 | 64 (8 slots, section 3.2) |
| Integer argument slots | 0x40 | 8 x N |
| Floating-point argument values | 0x40 + 8N | 8 x F |
| Data areas: module name, function name, then one per integer argument | D0 = 8 x (8 + N + F) | Each area starts at the next multiple of 8 |
| Reference areas: one per integer argument | After the last data area | Each starts at the next multiple of 8 |
| Total = `buf_size` | | End of the last area, rounded up to 8 |

- **N** = the sum of the argument slot counts of the integer arguments, where every float argument has been replaced by a null placeholder that counts 1, a struct passed by value counts `ceil(size/8)`, and a struct return adds a hidden first argument (section 4). N <= 8.
- **F** = number of float/double by-value arguments. Because each also holds an integer slot, F <= N <= 8.
- An area of size 0 takes no space; its address equals the start of the next area.

#### 3.2 Header and slots

| Offset | Client writes | Console writes | Client reads back |
|---|---|---|---|
| +0x00 | 0 | Call status: 0 on success; 0x8007007E (module not found), 0x8007007F (procedure not found), 0x82DA0004 (target address not valid); see section 6 | Yes, low 32 bits |
| +0x08 | 0 | Return value: r3, or f1 as IEEE-754 double bits (section 5.2) | Yes |
| +0x10 | `PostMethodCall` (0/1) | Post-call result (`GetLastError`) when 1 | Only when not `None` |
| +0x18 | Call flags: bit 0 = 0 integer return, 1 floating-point return | - | No |
| +0x20 | N, integer slot count | - | No |
| +0x28 | F, float count | - | No |
| +0x30 | Module-name address `buf_addr + off`, or 0 (call by address) | - | No |
| +0x38 | Ordinal, export-name address, or function address | - | No |
| +0x40 + 8i | Integer slot i (i = 0..N-1), in argument order | Not modified (the plugin copies slots to its stack before the call; the returned buffer therefore holds the uploaded slots) | No |
| +0x40 + 8N + 8j | Float j (j = 0..F-1) as a double | Not modified (same) | No |

In the plugin, the count fields (N at +0x20, F at +0x28) are used directly: it copies `N x 8` bytes of integer slots and `F x 8` bytes of float values, both starting at +0x40. No validation of N or F against 8 was noted in the disassembly; a server-side implementer should validate them, since more than 8 slots would overrun an 8-register image (unverified).

#### 3.3 Data and reference areas

| Area | Size | Upload content | Read back after the call |
|---|---|---|---|
| Module name | `len + 1` (ASCII) or 0 | ASCII bytes, terminator from the zero fill | No (options areas are not decoded) |
| Function name | `len + 1` or 0 (ordinal or address call) | ASCII bytes | No |
| Argument i, data | Required buffer size (section 4) | Big-endian value, string bytes, array elements or struct image | Yes for `ByRef`/`Out` |
| Argument i, reference | What pointers *inside* a struct or array of structs point to | Pointee data | Yes |

```
D0             = 8 * (8 + N + F)
off(area 0)    = D0                       areas in order: module, function,
off(area k)    = align8(off(k-1) + size(k-1))   data 0..n-1, reference 0..n-1
buf_size       = align8(off(last) + size(last))
pointer to k   = buf_addr + off(k)        (32-bit, zero-extended into the slot)
```

- `n` counts the argument objects (float placeholders included), `N` the slots they occupy; they differ only for by-value structs.
- Float values: the SDK loop that writes them passes the *options'* area array as their buffer address. Harmless, because a by-value number ignores the address (inferred), but a re-implementation should not copy the indexing.
- The whole buffer comes back, by-value data areas included; only `ByRef`/`Out` areas and reference areas are decoded.
- The SDK imposes no limit on `buf_size`; the plugin answers `428` if its allocation of `buf_size + 104` bytes fails.
- Unknown: whether `buf_addr` is 8-byte aligned (the client's 8-byte area alignment is relative to it, not absolute).

**Recommended client behaviour.** Produce this layout byte for byte, so that for the same call and the same `buf_addr` an upload from your client and one from `xdrpc.dll` are identical. A server implementation should locate data only through the header counts and the slot pointers, never by assuming area sizes.

### 4. Argument marshalling

#### 4.1 Nothing about types is on the wire

The console receives N, F, the slots, the float values and raw bytes. The SDK's `ArgumentType`, `CountType`, element types and struct layouts exist only in the client: a by-value string, a `ByRef` buffer and an `Out` buffer all arrive as one 32-bit address in a slot (inferred). Everything in this section is therefore a client convention that a re-implementation may reproduce or replace freely, as long as the bytes match.

#### 4.2 Enums

| Enum | Value | Name | Meaning |
|---|---|---|---|
| `ArgumentType` | 0 | `ByValue` | Numbers and by-value structs: the value is in the slot(s). Strings and arrays: address of an in-only copy |
| | 1 | `ByRef` | Address of a data area that is sent and read back |
| | 2 | `Out` | Same, sized by the given maximum; the current value is **still sent** |
| `CountType` | 0 | `Byte` | A string maximum counts bytes |
| | 1 | `Element` | A string maximum counts characters (converted with `Encoding.GetMaxByteCount`) |

#### 4.3 Argument classes

| Class | Slot value | Data area | Reference area | Read back |
|---|---|---|---|---|
| `XDRPCArgumentInfo<T>` (primitive), `ByValue` | The value (section 4.4) | 0 | 0 | No |
| `XDRPCArgumentInfo<T>`, `ByRef`/`Out` | Address of data area | `SizeOf(T)` | 0 | Yes |
| `float`/`double` `ByValue` | 0 (placeholder) + double in the float area | 0 | 0 | No |
| `XDRPCStringArgumentInfo` | Address, or 0 when the area size is 0 | Section 4.5 | 0 | `ByRef`/`Out` |
| `XDRPCArrayArgumentInfo<T[]>` | Address, or 0 when `MaxArrayLength` is 0 | `MaxArrayLength x SizeOf(T)` | Pointees of struct elements | `ByRef`/`Out` |
| `XDRPCStructArgumentInfo<T>`, `ByRef` (its default) / `Out` | Address | Struct size | Pointees | Yes |
| `XDRPCStructArgumentInfo<T>`, `ByValue` | `ceil(size/8)` slots of the struct image | 0 | Pointees | Pointees only |
| `XDRPCNullArgumentInfo` | 0 | 0 | 0 | - |
| `XDRPCSizeOfArgumentInfo(target)` | `target`'s required data-area size | 0 | 0 | - |

Plain objects passed as `params object[]` are converted automatically:

| Object | Becomes |
|---|---|
| `null` | `XDRPCNullArgumentInfo`, slot 0 |
| An `XDRPCArgumentInfo` | Used as is (both styles can be mixed in one call) |
| `bool`, `byte`, `short`, `ushort`, `int`, `uint`, `long`, `ulong`, `float`, `double` | `XDRPCArgumentInfo<T>(v, ByValue)` |
| `string` | `XDRPCStringArgumentInfo(v, ASCII, ByValue, 0, Element)`: address of `len + 1` bytes |
| One-dimensional array of a valid value type | `XDRPCArrayArgumentInfo<T[]>(v, ByValue, 0)` |
| A struct | `XDRPCStructArgumentInfo<T>(v, ByValue)`: **split into slots**, unlike `new XDRPCStructArgumentInfo<T>(v)`, which is `ByRef` |
| An enum | Not primitive, so it takes the struct path and fails for lack of `[XDRPCStruct]` (inferred); tools cast enums to `uint` |
| `char`, multi-dimensional arrays, classes | `XDRPCInvalidArgumentTypeException(type, index)` |
| `sbyte`, `IntPtr`, `UIntPtr` | Accepted as primitives, but missing from the size map and from the integer conversion, so they marshal as 0 with no data area (inferred) |

#### 4.4 Primitive types

| .NET type | `SizeOf` (data area) | By-value slot | By-ref bytes | As a return type (section 5.2) |
|---|---|---|---|---|
| `bool` | 4 | 0 or 1 | 4-byte big-endian 0/1; read back as `!= 0` | `(v & 0xFFFF) != 0` |
| `byte` | 1 | Zero-extended | 1 byte | `(byte)v` |
| `short` / `ushort` | 2 | Sign- / zero-extended | 2 bytes big-endian | Truncated |
| `int` / `uint` | 4 | Sign- / zero-extended | 4 bytes big-endian | Truncated |
| `long` / `ulong` | 8 | As is | 8 bytes big-endian | As is |
| `float` | 4 | Float area, widened to double | **4-byte single**, big-endian | `(float)` of the double |
| `double` | 8 | Float area | 8 bytes big-endian | The double |

#### 4.5 Strings

| Item | Rule |
|---|---|
| Encoding | Any .NET `Encoding`, ASCII by default. `Encoding.Unicode` (UTF-16) is sent **big-endian** (bytes swapped in pairs); UTF-8 and ASCII as is |
| Bytes written | `GetBytes(value)` only, **no terminator**: the zero-filled buffer supplies it |
| Area size, `ByValue` | Bytes of `value + "\0"` (ASCII +1, UTF-16 +2) |
| Area size, `ByRef` | `max(bytes of value + "\0", cap)` |
| Area size, `Out` | `cap` only |
| `cap` | `CountType.Byte`: the maximum in bytes. `Element`: `Encoding.GetMaxByteCount(max)`, which on .NET Framework is `max + 1` for ASCII, `2 x (max + 1)` for UTF-16, `3 x (max + 1)` for UTF-8 (inferred: runtime behaviour) |
| Default `CountType` | `Byte` for UTF-8, `Element` for everything else |
| Slot | Address of the area; **0 when the area size is 0** (null `ByValue` string, or `Out` with cap 0) |
| Null value | `ByValue`: NULL pointer. `ByRef`/`Out`: a zero-filled area of `cap` bytes |
| Read back (`ByRef`/`Out`) | UTF-16 swapped back; text up to the first NUL unit (1 or 2 bytes, unit-aligned), else the whole area |

- An `Out` string whose current value is longer than its cap is still written in full: it overruns into the following areas, or the copy throws at the end of the buffer (inferred).
- The SDK's `XDRPCReference.Get/Set` of a string use `CountType.Byte` with the allocation size, rounded down to even for UTF-16.

#### 4.6 Arrays

- Rank 1; element type a valid value type (any primitive except `char`, or an `[XDRPCStruct]` struct)
- Elements are packed as `ByRef` element infos, back to back, stride `SizeOf(element)` (`bool` 4, `float` 4 as a single, a struct its padded size)
- `MaxArrayLength`: `ByValue` the length; `ByRef` `max(length, max)`; `Out` the given max, which must be > 0 (else an `XDRPCException`)
- Elements beyond the value's length are `default(T)`
- Slot: address, 0 when `MaxArrayLength` is 0 (null or empty array)
- Reference area: each element's pointees, each element's block 8-aligned
- Read back (`ByRef`/`Out`): the value becomes a new array of `MaxArrayLength` elements

#### 4.7 Structs

- Every struct type needs `[XDRPCStruct]` (`System.Guid` is exempt), else `XDRPCInvalidTypeException`
- Fields in reflection order (`GetFields(DeclaredOnly \| Instance \| Public \| NonPublic)`); declaration order in practice, though .NET does not guarantee it (inferred)
- Packing: `LayoutKind.Sequential` (the C# default) gives **8 whatever `Pack` says**; another layout gives `StructLayout.Pack`. `FieldOffset` of an explicit layout is ignored; fields are still placed one after another (inferred)
- Field offset aligned to `min(pack, field alignment)`; struct alignment `min(pack, largest field alignment)`; size rounded up to the struct alignment
- Nesting: at most 3 levels below the top struct
- Pointer fields are 4 bytes (`uint`), absolute console addresses into the argument's reference area

| Field | `MarshalAs` | Placement | Size / alignment |
|---|---|---|---|
| Primitive | none | Inline | `SizeOf` / `min(pack, SizeOf)` |
| Primitive | `LPStruct` | 4-byte pointer; value in the reference area | 4 / 4 |
| `string` | `ByValArray`, `SizeConst = n`, `ArraySubType = LPStr` (ASCII) / `LPWStr` (UTF-16BE) / `LPTStr` (UTF-8) | Inline, n characters, truncated to fit, **no terminator when full** | n x 1 or n x 2 / character size |
| `string` | `LPArray`, same sub-types | 4-byte pointer; text in the reference area, `max(n x char, text + NUL)` bytes | 4 / 4 |
| `T[]` | `ByValArray`, `SizeConst = n` (required) | Inline, n elements | n x `SizeOf(T)` / element alignment |
| `T[]` | `LPArray`, `SizeConst = n` | 4-byte pointer; `max(n, length)` elements in the reference area | 4 / 4 |
| `LPArray` of size 0 | - | Pointer field written as 0 | 4 / 4 |
| `string` or array without `MarshalAs`, or another `UnmanagedType` | - | `XDRPCInvalidTypeException` | - |
| Nested struct | none | Inline | Its size / its alignment |
| Nested struct | `LPStruct` | 4-byte pointer; nested layout in the reference area at an 8-aligned offset | 4 / 4 |
| Nested struct field marked `[XDRPCUnion("member")]` | - | The nested struct is a **union**: all its fields at offset 0, only `member` is written, size = largest member; an unknown name throws | Largest member |
| Class or other reference type | - | `XDRPCInvalidTypeException` | - |

| Passing | Slots | Data area |
|---|---|---|
| `ByRef` (default of the constructor) / `Out` | 1: the address | Struct size, read back |
| `ByValue` (also every struct passed as a plain object) | `ceil(size/8)`: slot i holds bytes 8i..8i+7 of the big-endian image, the last slot zero-padded on the right | 0 |
| As the return type | A hidden **first** argument `XDRPCStructArgumentInfo<T>(default, Out)`: r3 = its address, the result is its value, +0x08 is ignored | - |
| Pointees | In the argument's reference area (rounded to 8), read back for every passing mode | - |

- The struct-return path calls the inner `ExecuteRPC` without a type argument, so its return flag is not recoverable (inferred: integer, flag 0).
- The reference area of a struct holds pointees only. When the struct comes back, the client reads strings and arrays from **its own** reference area, never through the pointer value the console left in the struct (inferred): a function that redirects a pointer is not seen.
- Size check on real structs used by third-party tools: an `XPARTY_USER_INFO` (Xuid 0-7, GamerTag[16] 8-23, dwUserIndex 24, NatType 28, dwTitleId 32, dwFlags 36, SessionInfo[60] 40-99, CustomData[16] 100-115, end 116) rounds to 8 as **120**, which is the stride a tool uses to decode a party list. An `XONLINE_FRIEND` comes to 196, rounded to **200**.

#### 4.8 Null, SizeOf, floats, limits

| Item | Rule |
|---|---|
| `null` / `XDRPCNullArgumentInfo` | Slot 0, no area. Also used internally as the float placeholder and as the module of a by-address call |
| `XDRPCSizeOfArgumentInfo(target)` | Slot = the target's **data-area** size (string bytes including NUL, array bytes, struct size), not rounded; no target gives 0. For "buffer, size" pairs |
| Float by value | Only `XDRPCArgumentInfo<float/double>` with `ByValue`; moved to the float list in argument order, integer slot stays 0, value written as a double |
| Float by reference | An ordinary 4- or 8-byte data area, no float slot |
| Slot limit | N <= 8, else `XDRPCTooManyArgumentsException`, whose text warns that a struct return and by-value structs take extra slots |
| `char` | Rejected as a plain object; `XDRPCArgumentInfo<char>` is accepted but marshals 0 (inferred); an array of `char` throws |

**Recommended client behaviour (for a client that does not reproduce .NET reflection).** Use a simple argument model: a 64-bit integer slot (the caller sign- or zero-extends), a double (a `float` is widened by the caller), an in / out / in-out byte buffer with an explicit size (empty gives a NULL pointer), an ASCII or UTF-16BE string with an explicit capacity in bytes, and a by-value blob split into `ceil(size/8)` slots. Model a struct as a caller-built big-endian image plus a list of "4-byte pointer at offset k points to buffer b" fix-ups that land in the reference area. Refuse more than 8 slots before connecting.

#### 4.9 Console memory helpers (`XDRPCReference`)

The SDK library's allocation helpers are themselves XDRPC calls into `xbdm.xex`:

| Operation | Call | Arguments | Return / check |
|---|---|---|---|
| `AllocateRPC(size, mode)` | `xbdm.xex` ordinal **1**, given mode (default `System`) | `(int size)` | `uint` address; 0 gives `XDRPCException("Failed allocation: null pointer was returned.")`; size 0 refused |
| `Free()` | Ordinal **9**, `System` | `(uint address)` | Ignored |
| `Set<T>(v)`, `Set(string, enc)` | Ordinal **40**, `System` | `(uint addr, int size, data ByRef, Out int done)` | `int` HRESULT: < 0 throws; `done != size` throws |
| `Get<T>()`, `Get(enc)` | Ordinal **10**, `System` | `(uint addr, int size, data Out, Out int done)` | Same |
| `SupportsRPC()` | Ordinal **117**, `System` | `(int 0)` | Return ignored; any `XDRPCException` gives `false`. A raw `COMException` still escapes |

- Ordinal 40 matches the SDK's `DmSetMemory(LPVOID, DWORD, LPCVOID, LPDWORD)` returning `HRESULT`, as other tools declare it; 10 is inferred to be `DmGetMemory` by symmetry; 1 and 9 are inferred allocate and free from their use; 117 is unknown.
- Data larger than the allocation, or a struct with pointees, is refused by the helper. `Dispose` frees; the finalizer does not. `XDRPCReferenceMonitor` keeps live addresses per console and `CheckLeakedXDRPCReferences(cleanup)` lists and optionally frees them.
- `SupportsRPC` is the only helper third-party tools use (some call it before every call, which costs two round trips per operation).

### 5. Call targets and return types

#### 5.1 Targets

| Kind | SDK call | +0x30 | +0x38 | Not-found errors |
|---|---|---|---|---|
| Module + ordinal | `ExecuteRPC<T>(mode, "xam.xex", 2831, ...)` | Address of `"xam.xex\0"` | Ordinal (int, sign-extended) | Module: 0x8007007E. Ordinal: 0x8007007F, reported as "Function ordinal not found: `<module>@<ordinal>`" |
| Module + export name | `ExecuteRPC<T>(mode, "m.xex", "Name", ...)` | Address of module name | Address of `"Name\0"` (ASCII) | Same codes; "Function name not found: `<module>@<name>`". No known tool uses it, and the plugin appears not to support it (section 2.2) |
| Address | `ExecuteRPC<T>(mode, 0x82D077C8u, ...)` | 0 | Address (uint, zero-extended) | None by construction (inferred): a wrong address runs whatever is there. The plugin does check the address first (0x82DA0004, section 7.2) |

- Module names are passed exactly as given (`xam.xex`, `xboxkrnl.exe`, `xbdm.xex` in known uses). The plugin resolves the module with the kernel's module-handle lookup by name (so presumably only already-loaded modules; case sensitivity is not known) and the export with the kernel's procedure-address lookup (handle, ordinal).
- The SDK's not-found message chooses the ordinal form when `Ordinal != 0`, else the name form; a module + ordinal 0 call that fails therefore prints the (empty) name form (inferred).
- Nothing is cached: every call resolves its target again on the console.

#### 5.2 Return types

| `T` | Flag at +0x18 | Decoding of the qword at +0x08 |
|---|---|---|
| `bool` | 0 | `(v & 0xFFFF) != 0` (only the low 16 bits) |
| `byte`, `short`, `ushort`, `int`, `uint` | 0 | Truncated to the type |
| `long`, `ulong` | 0 | All 64 bits (r3 is a 64-bit register) |
| `float` | 1 | `(float)` of the IEEE double in the slot |
| `double` | 1 | The IEEE double |
| A struct (non-primitive value type) | 0 (inferred) | Ignored; the struct comes back through the hidden first argument (section 4.7) |
| `string`, classes | - | `XDRPCInvalidReturnTypeException` |
| `sbyte`, `char`, enums | - | Take the struct path and fail on the missing `[XDRPCStruct]` (inferred) |

- There is no `void`. The SDK's `CreateDelegate` maps a `void` delegate to `uint`; tools ask for `uint` or `Int64` and ignore it.
- The client always reinterprets the 64 bits as a double for flag 1. The plugin stores f1 as a 64-bit value; PowerPC floating-point registers hold values in double format even for single-precision results, so a `float` function's result is expected to read correctly as a double (inferred).
- Order after the download: data and reference areas are decoded **before** the status check, so `ByRef`/`Out` objects can already hold new values when the call then throws (inferred).

**Recommended client behaviour.** Return the raw 64-bit value at +0x08 and the post-call qword, and let the caller choose the view (`u64`, `i32`, `double`, `float`); a `bool` view uses the low 16 bits. Make a struct return an explicit out-buffer argument in first position rather than a hidden one.

### 6. Errors and status mapping

#### 6.1 From stage to error (SDK client)

| Stage | Condition | Client result |
|---|---|---|
| Before connecting | More than 8 slots | `XDRPCTooManyArgumentsException(count, 8)` |
| | Plain object of an unsupported type | `XDRPCInvalidArgumentTypeException(type, index)` |
| | Unsupported type inside an argument info (non-primitive `T`, bad array, struct rules) | `XDRPCInvalidTypeException` |
| | `Out` array without a max length | `XDRPCException("The max array length must be specified for an Out type array.")` |
| | Unsupported return type | `XDRPCInvalidReturnTypeException` |
| Connect | COM error 0x82DA0100, 4 times (initial try + 3 retries) | `XDRPCException("Unable to connect to console. Verify console is not crashed.")` |
| | Any other COM error | Raw `COMException` |
| `rpc` command | Section 1.3 | `XDRPCNotSupportedException`, `XDRPCInvalidResponseException`, raw `COMException` |
| Upload, `203`, polling, download | Section 1.4 | `XDRPCInvocationFailedException`, `XDRPCInvalidResponseException`, `XDRPCTimeoutException` |
| Call status (+0x00, low 32 bits) | 0 | Success |
| | 0x8007007E (`HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND)`, 126) | `XDRPCModuleNotFoundException(module)`: "Module not found: `<module>`" |
| | 0x8007007F (`ERROR_PROC_NOT_FOUND`, 127) | `XDRPCFunctionNotFoundException` (ordinal or name form, section 5.1) |
| | Anything else | `XDRPCInvocationFailedException("RPC call failed: <status:X8>")` |
| Helpers (section 4.9) | Null allocation, size mismatch, `hr < 0`, misuse | `XDRPCException`, `XDRPCInvalidOperationException` |

- Only the low 32 bits of +0x00 are tested; the upper 32 bits are ignored.
- The function's own return value is never interpreted by the library; callers check it (the helpers test `hr < 0` themselves).
- Status values a console can put at +0x00: 0, 0x8007007E, 0x8007007F, and (plugin) 0x82DA0004 when the resolved or given address fails the address-validity check. What happens when the called function raises an exception is unknown.

#### 6.2 Status and result codes seen on the wire

| Value | Where | Meaning |
|---|---|---|
| `201` | Greeting | Connected |
| `203` (0x02DA0003) | After the upload | Binary data from the console follows |
| `204` (0x02DA0004) | After `rpc` | Ready for `buf_size` bytes from the client; carries `buf_addr` |
| `200- error=<text>` | After `rpc` | Refused with a reason (version problems on the plugin) |
| `407` (0x82DA0007) | After `rpc` | Command not registered |
| `423` (0x82DA0017) | After `rpc` | Invalid argument (mode or `buf_size` missing) |
| `428` (0x82DA001C) | After `rpc` | No memory for the call buffer |
| 997 (0x3E5, `ERROR_IO_PENDING`) | 8-byte poll | Call still running |
| 0 | 8-byte poll | Call finished |
| 0x00000000 | Call status at +0x00 | Success |
| 0x8007007E | Call status | Module not found |
| 0x8007007F | Call status | Procedure (ordinal) not found |
| 0x82DA0004 | Call status (plugin) | Target address not valid |
| 0x82DA0104 | Internal (plugin) | Returned by the plugin's send-back callback after the whole buffer has been sent, ending the transfer |
| 0x82DA0100 | COM error | Cannot connect (SDK retries) |
| 0x82DA0101 | COM error text | Name unknown; some tools retry the sequence on it |

#### 6.3 SDK exception types

All derive from `XDRPCException : Exception`.

| Exception | Extra properties | Message |
|---|---|---|
| `XDRPCFunctionNotFoundException` | `ModuleName`, `Ordinal`, `FunctionName` | "Function ordinal not found: m@o" / "Function name not found: m@n" |
| `XDRPCInvalidArgumentTypeException` | `ArgumentType`, `ArgumentIndex` | "Type (t) for argument i is not valid" |
| `XDRPCInvalidOperationException` | - | Given text, default "Invalid operation" |
| `XDRPCInvalidResponseException` | `Response` | "Invalid response received from console: `<response>`" |
| `XDRPCInvalidReturnTypeException` | `ReturnType` | "Invalid return type t" |
| `XDRPCInvalidTypeException` | `Type` | Given text, default "Invalid type t" |
| `XDRPCInvocationFailedException` | - | Given (formatted) text |
| `XDRPCModuleNotFoundException` | `ModuleName` | "Module not found: m" |
| `XDRPCNotSupportedException` | `Response` | Always "XDRPC not supported by console" (the line or reason is in `Response`) |
| `XDRPCTimeoutException` | `TimeoutStartTime`, `TimeoutCurrentTime`, `TimeoutPeriod` | Text plus the three values |
| `XDRPCTooManyArgumentsException` | `ArgumentCount`, `MaxArgumentsSupported` | "n arguments specified when only 8 are supported. Note: ..." |

**Not wrapped:** `COMException` from XDevkit (connect errors other than 0x82DA0100, unknown `4xx`, transport failures), and plain .NET exceptions (`OverflowException`, `ArgumentException`) (inferred).

**Recommended error classification for a new client.**

| Error kind | From |
|---|---|
| `NotSupported` | `407` to `rpc`, or a non-204 `buf_addr` answer |
| `Refused(text)` | `200- error=<text>` |
| `InvalidArgument` | `423` |
| `NoMemory` | `428` |
| `Xbdm(code, text)` | Any other `4xx` |
| `Protocol(detail)` | Bad line, `203` missing, short qword, polled value not 0/997, short buffer, bad `buf_addr` |
| `Timeout` | Call deadline (section 1.4) |
| `Disconnected` | Socket closed or reset during the call |
| `ModuleNotFound(module)` | +0x00 = 0x8007007E |
| `FunctionNotFound(module, ordinal or name)` | +0x00 = 0x8007007F |
| `CallFailed(status)` | Any other non-zero low 32 bits at +0x00 (keep all 64 bits) |
| `TooManySlots`, `BadArgument` | Client checks, before connecting |

### 7. Console side

All of this section describes the `XDRPC.xex` plugin, from a disassembly of one copy; it has not been compared with Microsoft's own `rpc` implementation.

#### 7.1 Plugin call context

The plugin allocates `buf_size + 104` bytes with pool tag `'drpc'`. The call buffer starts at +104 in the allocation (so "client offset N" is "context offset 104 + N"), and `buf_addr` is that address. The first 104 bytes are the plugin's private context:

| Context offset | Content |
|---|---|
| +0 | 0 |
| +4 | 0; a spin gate in the worker that nothing was seen to set |
| +8 | Mode flag: system = 1, title = 0 |
| +12 | Worker thread handle |
| +16 | `buf_size` |
| +24 | The 8-byte poll qword: initialised to 997, rewritten to 0 when the worker finishes |
| +32 | Thread name (64 bytes) |
| +96 | Completion flag (0 while running, 1 when done) |
| +104 | Start of the call buffer (client offset 0) |

#### 7.2 Flow

1. **`rpc` handler**: parses tokens (section 1.2), allocates, initialises the context, creates the worker thread **suspended** (thread-creation flags: `(1 << processor)` in the top byte, OR 2 if system, OR 0x81; the 0x81 base is read as "create suspended" plus one more bit, not proven), and tells XBDM to deliver the upload straight into the call buffer. It answers `buf_addr=%p` with status 0x02DA0004 (`204`).
2. **Receive callback**: for each upload chunk it advances the destination pointer and decrements the remaining count. When the count reaches 0 and no thread name was given, it resumes the worker, installs the send-back callback, and returns status 0x02DA0003 (`203`, binary response). With a thread name it resumes the worker and closes the handle and takes a different path.
3. **Worker** reads the header:
   - Module pointer (+0x30, low word) is 0: raw-address path with the address from +0x38 (low 32 bits).
   - Otherwise: module handle lookup by name (not found: status 0x8007007E), then procedure lookup with the ordinal from +0x38 (not found: 0x8007007F).
   - Address validity check on the final address; invalid: status 0x82DA0004.
   - Copies the N integer slots (N x 8 bytes) and the F float values (F x 8 bytes) from +0x40 to its stack; loads **r3-r10** from the integer slots and **f1-f8** from the float values (in order), then calls the address (`mtctr` / `bctrl`).
   - Stores f1 and r3 to temporaries, picks float or integer by bit 0 of +0x18, writes the 64-bit result to +0x08.
   - If +0x10 equals 1, calls `GetLastError` and stores the result at +0x10.
   - Writes status 0 to +0x00 and sets the completion flag.
4. **Send-back callback**: while the completion flag is 0, waits up to about 100 ms (10 ms sleeps) and returns the poll qword (997) with length 8. Once the flag is set it rewrites the qword to 0 and returns it (length 8). The pass after that returns the whole call buffer (`buf_size` bytes), then frees the allocation with the pool-free export and returns 0x82DA0104.

Kernel exports (xboxkrnl.exe ordinals) the plugin imports for this: 10 pool allocate, 13 thread create, 15 pool free, 405 module-handle lookup, 407 procedure-address lookup (certain from usage); 191 address-validity check, 146 thread resume, 261 handle close (inferred from argument shape).

Not known for the plugin: what happens if the called function raises an exception; what happens if the client disconnects mid-call (the allocation is freed only on the final send-back pass); behaviour after a client timeout; whether two calls can run at once on two connections (each call has its own context).

#### 7.3 PowerPC register mapping

| Buffer | Register | Basis |
|---|---|---|
| Integer slot i, i = 0..7 | r3 + i (r3..r10) | Plugin loads r3-r10 from the 8 slots |
| Float j, j = 0..F-1 | f1 + j | Plugin loads f1-f8 consecutively from the float values; the 13 FP argument registers f1-f13 are not all reachable (only f1-f8) |
| The integer slot of a float argument | Holds 0 | Matches the 64-bit PowerPC convention in which every argument takes one GPR position whether or not it travels in an FPR; that the Xbox 360 compiler does this is unverified |
| By-value struct | Consecutive GPRs, image left-justified, big-endian | Inferred |
| Struct return | r3 = address of the result; the declared arguments start at r4 | Client convention |
| Stack arguments | Never produced: there is no 9th slot | - |
| Return | r3 (64 bits) into +0x08 when the flag is 0; f1 as a double when the flag is 1 | Plugin |
| r0, r2, r11-r13, CTR, LR | Not set by the plugin beyond CTR for the call | - |

Example: `f(int a, float b, int c, double d)` gives N = 4, F = 2, slots `a, 0, c, 0`, floats `b, d`, hence r3 = a, r4 = 0, r5 = c, r6 = 0, f1 = b, f2 = d.

Pointers are 32-bit console addresses zero-extended into 64-bit slots. A 64-bit value passes intact: the hypervisor stub example in "Calls seen in the wild" sends 0x8000020000020600 in r5.

#### 7.4 The `consolefeatures` handler in the plugin

The same plugin installs a JRPC2-style `consolefeatures` handler (see [JRPC2](jrpc2.md)) that expects `ver=2` (not 4). Parameters:

| Key | Meaning |
|---|---|
| `ver` | DWORD, required, must be 2; otherwise version-not-specified / mismatch text (expected 2), status 0x02DA0000 |
| `type` | DWORD return-type selector; drives a format table (values 0-9 give `%X`, `%i`, `%f`, `%llX`, string...); missing: 0x82DA0017 |
| `params` | Argument string; missing: `error=The paramaters were not found` (spelling as in the plugin) |
| `system` | Token; makes the spawned thread a system thread (flag 2) |
| `module` | String; if present requires `ord`. Resolved via module-handle and procedure-address lookups |
| `ord` | DWORD; `module` without `ord`: `error=Module param was found but the ordinal was not`; unresolved: `error=Could not resolve function address, params = %s, %d` |
| raw address | If no `module`, the address is validated; invalid: `error=Call address is invalid` |
| async flag | If set, the feature context pointer is stored in a global pending slot instead of running inline; otherwise a thread is created, started, and its handle closed |
| `buf_addr` | The answer carries `buf_addr=%p`, the address of the call context from which the client later reads the result |

There is a 40-slot async-call array (`error=The limit of 40 has been reached`) and a ` VM ` token. The exact spelling of the async key and the VM token behaviour were only partially traced. In `consolefeatures` `buf_addr` names a pending **result**; in `rpc` it is the address of the **call buffer** given in the `204` answer. Same word, unrelated meanings.

#### 7.5 How the plugin installs `rpc`

The installer first confirms that xbdm is present by resolving ordinal 1 of `xbdm.xex` (or `xbdm2.xex`); if neither resolves it fails with "xbdm.xex not loaded!". It then validates a fixed xbdm code address and fingerprints the xbdm build by comparing two known pointers. Two builds are supported; others give "Current xbdm.xex is unsupported". For a matched build it writes directly into xbdm's internal command table: the string `rpc` into one slot and the `rpc` handler address into the adjacent slot. It saves the previous command-dispatcher pointer in a plugin global (for chaining), installs its own dispatcher (the `consolefeatures` handler region), calls a secondary init (xam / HUD setup) and returns 1. No restore of the `rpc` entry on unload was found; the hook appears to be left in place at unload (or the teardown lives elsewhere). Loading is usually done from the dashboard's launch configuration; a banner "XDRPC Version %d loaded" is printed at load.

#### 7.6 The two server implementations

| Server | Where | Evidence |
|---|---|---|
| Microsoft `xbdm.xex` on development kits | Native `rpc` command (inferred) | The SDK ships the client library, and its own error text for `407` is "RPC command not registered in Xbox's XBDM", so some xbdm builds lack it. |
| `XDRPC.xex` plugin on RGH/JTAG retail consoles | Patches an `rpc` entry into a running xbdm's command table | Tools that depend on it ask the user to install it and add it to their launch configuration |

The wire format described here is the client's, and every element of it is consistent with the plugin handler. Where Microsoft's xbdm might differ from the plugin, the text says so.

## Implementing a client

A client outside .NET needs only the sections above; there is no need to reproduce the SDK's type system.

1. **Describe** the call: target (ordinal / export name / address), mode, arguments (section 4), return view, post-call, deadline.
2. **Validate** before any I/O: N <= 8, F <= N, sizes and capacities known.
3. **Size pass**: compute D0, every area offset and `buf_size` (section 3.3). No pointers yet.
4. **Connect** a dedicated XBDM socket and read the `201` greeting. Retry only a refused connect.
5. **Command**: send the section 1.2 line, parse the answer (section 1.3), keep `buf_addr`.
6. **Fill pass**: write the header, the slots with `buf_addr + offset` pointers, the floats, and the data and reference areas into a zeroed buffer of `buf_size` bytes; send it raw.
7. **Status**: read one line, require `203`.
8. **Poll**: read 8-byte big-endian qwords until not `997` (section 1.4); require 0.
9. **Download**: read exactly `buf_size` bytes.
10. **Close**: send `bye`, close without waiting long.
11. **Decode**: +0x00 (section 6), +0x08, +0x10, then every `out` / in-out area.

| Decision | Recommendation | Why |
|---|---|---|
| Connection | One dedicated XBDM connection per call, never reused, never the main command connection | The console serves one `rpc` per connection (section 1.1) |
| Retries | Connect: 3 x 100 ms on refusal. Call: never | A call may already have had side effects (section 1.4) |
| Deadline | One per call, default 5 minutes, enforced on every socket read | The SDK checks only on `997`; a silent console would otherwise block forever |
| Command | Lower case, fixed token order, `thread=` empty, `processor` 0-5 default 5 | Section 1.2 |
| `buf_addr` | 1-8 hex digits, optional `0x` tolerated | Section 1.3 |
| Byte order | Big-endian slots and data; UTF-16 strings big-endian; strings without terminator in a zero-filled area | Sections 3, 4.5 |
| Layout | Byte-identical to `xdrpc.dll`, including float placeholders and area order | Section 3 |
| Arguments | Integer slot, double, byte buffer (in / out / in-out), string with byte capacity, by-value blob, struct image with pointer fix-ups | Section 4.8 |
| Struct return | Explicit out buffer as the first argument | Section 4.7 |
| Status | Error on non-zero low 32 bits of +0x00 (as the SDK does); keep the full 64 bits in the error | Section 6 |
| Return | Raw 64 bits plus typed views | Section 5.2 |
| Probe | No automatic probe. `407` on the first call means "not supported"; an explicit probe mirroring `SupportsRPC` (`xbdm.xex` ordinal 117, argument 0, `system`) only on request | Section 4.9 |
| Scope | Named threads, events and the `XDRPCReference` helpers are optional; console memory can be read and written with XBDM's own memory commands | Sections 1.2, 2.5, 4.9 |

For testing a client, the dumps in "Worked examples" (with `buf_addr` = 0x8D0A1000) are useful byte-exact layout fixtures. A test server needs to: parse the command line, answer `204- buf_addr=<fixed>`, read `buf_size` bytes, check N <= 8, F <= N and that every pointer slot lies inside `[buf_addr, buf_addr + buf_size)`, answer `203`, send some number of `997` qwords, then `0` and the buffer with the configured status, return value and area writes. Useful fault injections: `407`, `423`, `428`, `200- error=`, a bad polled value, a short qword, a short buffer, silence.

## Worked examples

Derived by hand from the layout rules and checked with a short script that re-implements the layout. Conventions for all examples:

- `buf_addr` is a made-up **0x8D0A1000** (B). Every pointer below is B + offset.
- Return values and out values from the console are made up and marked *illustrative*; the client side of every byte is derived from the rules.
- The exchange is always the same; only the sizes and bytes differ:

```
C: rpc <mode> version=4 buf_size=<S> processor=5 thread=\r\n
S: 204- buf_addr=8D0A1000\r\n
C: <S bytes: the upload>
S: 203- <text>\r\n                      (text not checked by the client)
S: 00 00 00 00 00 00 03 E5              (997, zero or more times)
S: 00 00 00 00 00 00 00 00              (0: finished)
S: <S bytes: the download>
```

### Integer call by address

`ExecuteRPC<uint>(Title, 0x82001234u, 5, 0xFFFFFFFFu, (short)-2)`, a hypothetical `uint32 f(int32, uint32, int16)`.

Arithmetic: N = 3, F = 0, D0 = 8 x (8 + 3) = 88; no data areas (address call, all by value), so S = **88**. Command: `rpc title version=4 buf_size=88 processor=5 thread=`.

```
upload                                   download (illustrative)
+00  00 00 00 00 00 00 00 00  status     00 00 00 00 00 00 00 00  0 = success
+08  00 00 00 00 00 00 00 00  return     00 00 00 00 00 00 00 2A  r3 = 42
+10  00 00 00 00 00 00 00 00  post-call  (unchanged)
+18  00 00 00 00 00 00 00 00  int return
+20  00 00 00 00 00 00 00 03  N = 3
+28  00 00 00 00 00 00 00 00  F = 0
+30  00 00 00 00 00 00 00 00  no module
+38  00 00 00 00 82 00 12 34  address
+40  00 00 00 00 00 00 00 05  int 5, sign-extended
+48  00 00 00 00 FF FF FF FF  uint 0xFFFFFFFF, zero-extended
+50  FF FF FF FF FF FF FF FE  short -2, sign-extended
```

Result: status 0, `(uint)0x2A` = 42.

### String argument, call by module and ordinal

`ExecuteRPC<uint>(System, "xboxkrnl.exe", 409, new object[] { "Hdd:\\XDRPC.xex", 8, 0, 0 })`: a third-party tool's call that loads an executable module from disk.

| Area | Size | Offset |
|---|---|---|
| Header + 4 slots | 8 x (8 + 4) = 96 | 0x00-0x5F |
| Module `xboxkrnl.exe` (12 chars + NUL) | 13 | 0x60-0x6C, next align8(0x6D) = 0x70 |
| Function (ordinal: none) | 0 | 0x70 |
| Arg 0 `Hdd:\XDRPC.xex` (14 chars + NUL, ASCII, `ByValue`) | 15 | 0x70-0x7E, next 0x80 |
| Args 1-3 (by value), all reference areas | 0 | 0x80 |
| S | **128** | |

Command: `rpc system version=4 buf_size=128 processor=5 thread=`. 409 = 0x199.

```
+00  00 00 00 00 00 00 00 00   status
+08  00 00 00 00 00 00 00 00   return
+10  00 00 00 00 00 00 00 00   post-call
+18  00 00 00 00 00 00 00 00   int return
+20  00 00 00 00 00 00 00 04   N = 4
+28  00 00 00 00 00 00 00 00   F = 0
+30  00 00 00 00 8D 0A 10 60   B + 0x60: "xboxkrnl.exe"
+38  00 00 00 00 00 00 01 99   ordinal 409
+40  00 00 00 00 8D 0A 10 70   r3 = B + 0x70: the path
+48  00 00 00 00 00 00 00 08   r4 = 8
+50  00 00 00 00 00 00 00 00   r5 = 0
+58  00 00 00 00 00 00 00 00   r6 = 0
+60  78 62 6F 78 6B 72 6E 6C   "xboxkrnl"
+68  2E 65 78 65 00 00 00 00   ".exe" NUL pad
+70  48 64 64 3A 5C 58 44 52   "Hdd:\XDR"
+78  50 43 2E 78 65 78 00 00   "PC.xex" NUL pad
```

Download: the same 128 bytes with +0x00 and +0x08 filled in. The string was `ByValue`, so its area is downloaded but not decoded.

### By-reference out buffers

A party-state query: options `(Title, "xam.xex", 2831)`, arguments `XDRPCArgumentInfo<uint>(0u, Out)` twice (inferred `uint`).

| Area | Size | Offset |
|---|---|---|
| Header + 2 slots | 8 x (8 + 2) = 80 | 0x00-0x4F |
| Module `xam.xex` (7 + NUL) | 8 | 0x50-0x57 |
| Arg 0 data (`Out uint`) | 4 | 0x58-0x5B, next 0x60 |
| Arg 1 data (`Out uint`) | 4 | 0x60-0x63, next 0x68 |
| S | **104** | |

Command: `rpc title version=4 buf_size=104 processor=5 thread=`. 2831 = 0xB0F.

```
upload                                   download (illustrative)
+00  00 00 00 00 00 00 00 00  status     00 00 00 00 00 00 00 00
+08  00 00 00 00 00 00 00 00  return     00 00 00 00 00 00 00 00
+10  00 00 00 00 00 00 00 00
+18  00 00 00 00 00 00 00 00
+20  00 00 00 00 00 00 00 02  N = 2
+28  00 00 00 00 00 00 00 00
+30  00 00 00 00 8D 0A 10 50  "xam.xex"
+38  00 00 00 00 00 00 0B 0F  ordinal 2831
+40  00 00 00 00 8D 0A 10 58  r3 = &state
+48  00 00 00 00 8D 0A 10 60  r4 = &error
+50  78 61 6D 2E 78 65 78 00  "xam.xex" NUL
+58  00 00 00 00 00 00 00 00  state = 0  00 00 00 01 00 00 00 00  state = 1
+60  00 00 00 00 00 00 00 00  error = 0  00 00 00 00 00 00 00 00  error = 0
```

The client decodes 4 big-endian bytes at +0x58 and +0x60: state 1, error 0.

### Float arguments and a float return

`ExecuteRPC<float>(Title, 0x82001000u, 1.5f, 2, -0.25)`, a hypothetical `float f(float x, int n, double y)`.

- The two floats move to the float list; their integer slots become 0: slots `0, 2, 0`, N = 3, F = 2.
- D0 = 8 x (8 + 3 + 2) = 104, no areas, S = **104**; flag = 1 (float return).
- 1.5 = 1.1b x 2^0: sign 0, exponent 1023 = 0x3FF, fraction 0x8000000000000, giving `3FF8000000000000`. -0.25 = -1.0b x 2^-2: sign 1, exponent 1021 = 0x3FD, giving `BFD0000000000000`. (A `float` is widened to double first.)

Command: `rpc title version=4 buf_size=104 processor=5 thread=`.

```
upload                                   download (illustrative)
+00  00 00 00 00 00 00 00 00  status     00 00 00 00 00 00 00 00
+08  00 00 00 00 00 00 00 00  return     40 06 00 00 00 00 00 00  f1 = 2.75
+10  00 00 00 00 00 00 00 00
+18  00 00 00 00 00 00 00 01  float return
+20  00 00 00 00 00 00 00 03  N = 3
+28  00 00 00 00 00 00 00 02  F = 2
+30  00 00 00 00 00 00 00 00  no module
+38  00 00 00 00 82 00 10 00  address
+40  00 00 00 00 00 00 00 00  slot 0: placeholder for x  (r3 = 0)
+48  00 00 00 00 00 00 00 02  slot 1: n = 2              (r4)
+50  00 00 00 00 00 00 00 00  slot 2: placeholder for y  (r5 = 0)
+58  3F F8 00 00 00 00 00 00  float 0: 1.5               (f1)
+60  BF D0 00 00 00 00 00 00  float 1: -0.25             (f2)
```

Decoding: `4006000000000000` has exponent 0x400 = 1024 (2^1) and fraction 0x6000000000000 / 2^52 = 0.375, so 1.375 x 2 = 2.75; the client returns `(float)2.75`. Registers per section 7.3.

### Struct with an inline string and a pointer

```csharp
[XDRPCStruct]
struct TAGGED                       // LayoutKind.Sequential -> pack 8
{
    public ushort Id;               // +0,  2 bytes, align 2
    [MarshalAs(UnmanagedType.ByValArray, SizeConst = 6, ArraySubType = UnmanagedType.LPStr)]
    public string Tag;              // +2,  6 bytes inline, align 1
    public float Scale;             // +8,  4 bytes, align 4
    [MarshalAs(UnmanagedType.LPArray, ArraySubType = UnmanagedType.LPWStr)]
    public string Label;            // +12, 4-byte pointer into the reference area
}                                   // alignment min(8, 4) = 4, size 16
var v = new TAGGED { Id = 0x0102, Tag = "ab", Scale = 0.5f, Label = "Hi" };
console.ExecuteRPC<uint>(XDRPCMode.Title, 0x82002000u,
                         new XDRPCStructArgumentInfo<TAGGED>(v), 1);   // ByRef
```

| Area | Size | Offset |
|---|---|---|
| Header + 2 slots | 80 | 0x00-0x4F |
| Arg 0 data: the struct image | 16 | 0x50-0x5F |
| Arg 1 data | 0 | 0x60 |
| Arg 0 reference area: `"Hi"` as UTF-16BE + NUL = 6, rounded to 8 | 8 | 0x60-0x67 |
| S | **104** | |

Struct image: `Id` = `01 02`; `Tag` = `61 62` then 4 zero bytes (no terminator is written; the zero fill provides one here because the text is shorter than 6); `Scale` 0.5f = `3F 00 00 00` (single, not double: a struct field is by-reference data); `Label` = B + 0x60 = `8D 0A 10 60`.

Command: `rpc title version=4 buf_size=104 processor=5 thread=`.

```
upload                                   download (illustrative)
+00  00 00 00 00 00 00 00 00  status     00 00 00 00 00 00 00 00
+08  00 00 00 00 00 00 00 00  return     00 00 00 00 00 00 00 00
+10  00 00 00 00 00 00 00 00
+18  00 00 00 00 00 00 00 00
+20  00 00 00 00 00 00 00 02  N = 2
+28  00 00 00 00 00 00 00 00
+30  00 00 00 00 00 00 00 00  no module
+38  00 00 00 00 82 00 20 00  address
+40  00 00 00 00 8D 0A 10 50  r3 = &struct
+48  00 00 00 00 00 00 00 01  r4 = 1
+50  01 02 61 62 00 00 00 00  Id, Tag    03 04 61 62 00 00 00 00  Id = 0x0304
+58  3F 00 00 00 8D 0A 10 60  Scale, ptr (unchanged)
+60  00 48 00 69 00 00 00 00  "Hi" NUL   00 59 00 6F 00 00 00 00  "Yo"
```

After the call the client decodes the struct area, then the reference area, and rebuilds the value: `Id = 0x0304`, `Tag = "ab"`, `Scale = 0.5`, `Label = "Yo"`. The label is read from the client's own reference area at +0x60, not through the pointer at +0x5C (section 4.7).

**By-value variant.** The same `v` passed as a plain object (or with `ArgumentType.ByValue`) is split into slots: N = 2 + 1 = 3, D0 = 8 x (8 + 3) = 88, no data area, reference area 0x58-0x5F, S = 96, and the pointer becomes B + 0x58:

```
+20  00 00 00 00 00 00 00 03   N = 3
+40  01 02 61 62 00 00 00 00   struct bytes 0-7   (r3)
+48  3F 00 00 00 8D 0A 10 58   struct bytes 8-15  (r4)
+50  00 00 00 00 00 00 00 01   1                  (r5)
+58  00 48 00 69 00 00 00 00   "Hi" NUL
```

## Calls seen in the wild

Calls that third-party tools make through XDRPC. Purposes are the tools' own method names; "inferred" marks a purpose read from the call pattern only.

| Target | Mode | Arguments | Return | Purpose |
|---|---|---|---|---|
| `xam.xex` 2817 | Title | `uint` user index, `ulong` XUID | Ignored | Join party |
| `xam.xex` 2818 | Title | `ulong` XUID | Ignored | Kick user from party |
| `xam.xex` 2831 | Title | `Out uint`, `Out uint` | Ignored | Get party state (state, error code) |
| Address 0x818301E8 (one dashboard build; another build uses 0x8182F8E8) | Title | `(uint)userIndex`, friend index, 1, `Out uint`, `Out uint` | `uint`, 0 = OK | Get next friend, step 1 (inferred: open an enumerator) |
| `xam.xex` 592 | Title | handle, 0, `Out XONLINE_FRIEND` (200 bytes), `uint`, `Out uint`, 0 | `uint` | Step 2 (inferred: read one friend) |
| `xam.xex` 1044 | Title | handle | Ignored | Step 3 (inferred: close the handle) |
| `xboxkrnl.exe` 40 | System | `int` 7 | Ignored | Shutdown |
| `xam.xex` 463 | System or Title | none | `uint` | Get current title id |
| `xboxkrnl.exe` 409 | System | path string, 8, 0, 0 | Ignored | Load module (worked example above) |
| `xam.xex` 1102, then `xboxkrnl.exe` 417 | System | module name; then the handle | `uint` handle | Unload module; between the calls the tool writes 1 at handle + 0x40 through XBDM |
| Address 0x81B44688 | Title | 0x48565050, 3, `ulong` | `UInt64` | Read CPU key bytes (below) |
| User-supplied module + ordinal, or address | System / Title | Always 8 arguments (unused ones `int` 0): `UInt64`, string or `float` per field | `Int64`, `float`, or a string read from the returned address | Generic "call by ordinal / address" UI |
| Per-title `CBuf_AddText`, `CBuf_Execute` addresses | Title | `(uint 0, string, uint 0)`, then none | `uint` | Run a console command inside a Source-engine game |
| `xbdm.xex` 1, 9, 10, 40, 117 | System (1: given) | Section 4.9 | | SDK allocation, memory, probe |

Patterns:

| Pattern | Consequence for a client |
|---|---|
| Plain objects for inputs, argument-info objects for `Out` values and strings, mixed in one list | Both styles end in the same layout (section 4.3) |
| An `Out` value fed into the next call | Multi-call sequences; each call is its own connection |
| Enums cast to `uint` before passing | Section 4.3 |
| The struct marshaller used as a **parser** for memory read through another protocol | XDRPC layout rules double as a memory-format description |
| Always 8 arguments, unused ones 0 | Every such call has N = 8 |
| Write code with XBDM, call it by address, erase it | Needs memory write + XDRPC in one tool |
| Returned pointer then a 32-byte XBDM read for a string result | Longer strings are cut at 32 bytes |
| `SupportsRPC` before **every** call | Two XDRPC round trips per operation |
| Retry the whole sequence on `0x82DA0101` | Section 1.4 |
| Ask the user to install `XDRPC.xex` | Section 7.6 |
| Hard-coded addresses per title id | Call by address is title-build specific |
| Swallow every exception | Failures are invisible to the user |

### Example: a hypervisor syscall stub

The only PowerPC code seen in these tools is a 16-byte stub used to read the CPU key:

```
38 00 00 71  44 00 00 02  4E 80 00 20  00 00 00 00
```

Decoded by hand (PowerPC bit numbering, bit 0 is the most significant):

| Offset | Word | Bits | Fields | Instruction |
|---|---|---|---|---|
| +0x0 | `38000071` | `001110 00000 00000 0000000001110001` | Primary opcode 14 (`addi`), rD = 0, rA = 0 (reads as the constant 0), SIMM = 0x0071 | `li r0, 0x71` |
| +0x4 | `44000002` | `010001 00000 00000 0000 0000000 000 1 0` | Primary opcode 17 (`sc`), LEV (bits 20-26) = 0, bit 30 = 1 as `sc` requires | `sc` |
| +0x8 | `4E800020` | `010011 10100 00000 00000 0000010000 0` | Primary opcode 19, BO = 0b10100 (branch always), BI = 0, extended opcode 16 (`bclr`), LK = 0 | `blr` |
| +0xC | `00000000` | all zero | Primary opcode 0: not a valid instruction | Padding, never reached (the `blr` returns first) |

How the tool uses it:

| Step | Action | Transport |
|---|---|---|
| 1 | Write the 16 bytes to 0x81B44688 (old contents not saved) | XBDM memory write, not XDRPC |
| 2 | Four calls by address 0x81B44688, `Title` mode, arguments `(0x48565050, 3, 0x8000020000020600 + 0x200 x k)`, k = 0..3, return `UInt64` | XDRPC |
| 3 | Zero the 16 bytes again | XBDM memory write |
| 4 | Key text = `(r1 \| r2).ToString("X8")` + `(r3 \| r4).ToString("X8")` | Client |

At the `sc`: r0 = 0x71 (from the stub), r3 = 0x48565050 (ASCII `HVPP`), r4 = 3, r5 = the 64-bit address; the result comes back in r3. The first call's upload (`rpc title version=4 buf_size=88 processor=5 thread=`, N = 3, F = 0, 88 = 8 x (8 + 3)):

```
+00  00 00 00 00 00 00 00 00   status
+08  00 00 00 00 00 00 00 00   return
+10  00 00 00 00 00 00 00 00   post-call: none
+18  00 00 00 00 00 00 00 00   flags: integer return
+20  00 00 00 00 00 00 00 03   N = 3
+28  00 00 00 00 00 00 00 00   F = 0
+30  00 00 00 00 00 00 00 00   no module: call by address
+38  00 00 00 00 81 B4 46 88   address of the stub
+40  00 00 00 00 48 56 50 50   r3 = "HVPP" (int, sign-extended, positive)
+48  00 00 00 00 00 00 00 03   r4 = 3
+50  80 00 02 00 00 02 06 00   r5 = 0x8000020000020600 (ulong)
```

Interpretation, all inferred and none of it stated by the tools:

- `li r0, N; sc; blr` is the shape of a system-call stub; from kernel or title code on the Xbox 360, `sc` enters the hypervisor, and r0 = 0x71 selects the call. What call 0x71 is on a stock hypervisor is unknown.
- The `HVPP` magic, the selector 3 and a 64-bit physical-style address look like a peek service added by RGH/JTAG hypervisor patches, 3 meaning "read 64 bits".
- The four addresses step by 0x200 from 0x8000020000020600: the fuse rows that hold the CPU key, combined pairwise with OR.
- `"X8"` pads only to 8 digits, so a half whose top nibble is 0 prints fewer than 16 digits and the 32-digit key loses characters (a bug in that tool).

## Quirks and known inconsistencies

- **`processor` is always 5** in the SDK library 1.7.1.0 even where an overload takes a processor value.
- **Thread-name calls** always send `system`, and their behaviour on the plugin is unclear: the plugin takes a different end-of-upload path (worker resumed, handle closed) and the result-streaming callback is not obviously installed there.
- **Timeout is checked only when a `997` arrives.** With a plugin that emits `997` about every 100 ms, this works; a console that never sends `997` is only bounded by XDevkit's own socket timeout.
- **`buf_addr` must be bare hex.** A `0x` prefix would make the SDK parse `0`.
- **`buf_addr` meaning differs** between `rpc` (call buffer address) and `consolefeatures` (pending result address).
- **Call-by-name** exists in the client but is probably unsupported by the plugin, which treats +0x38 as an ordinal whenever a module is given.
- **Float registers.** The call header allows up to 8 floats, but the PowerPC convention has 13 FP argument registers; the plugin loads only f1-f8.
- **Float slots.** Each float argument also holds an (empty) integer slot, so the 8-slot budget is shared: `F <= N <= 8`.
- **Struct pointers on return.** The client re-reads strings and arrays from its own reference area, so a function that redirects a pointer inside a struct is not seen.
- **Strings.** A string `Out` buffer whose input value is longer than its capacity is written in full and overruns the next area.
- **Decoded before status.** `ByRef`/`Out` data are decoded before the call status is checked, so output objects may be changed even when the call is reported as failed.
- **Misc SDK.** `sbyte`, `IntPtr`, `UIntPtr` and `char` marshal as 0; `bool` return uses only the low 16 bits; the float write loop passes the wrong buffer array (harmless); finalizers do not free `XDRPCReference` allocations.
- **Errors not wrapped.** COM errors from the transport and unknown `4xx` codes come out as raw `COMException` rather than an XDRPC exception.
- **Analyses disagree on named threads.** One reading of the plugin says the worker is never resumed for named-thread calls; another says it is resumed and its handle closed. Treat named threads as unverified either way.

## Open questions

| # | Question |
|---|---|
| 1 | Does XDevkit send `bye` before closing an XDRPC connection, and does the console care? |
| 2 | On Microsoft's xbdm: how often is `997` sent, and is it sent at all for a call that returns at once? (The plugin sends about one per 100 ms while running.) |
| 3 | Console behaviour when the called function throws or hangs, when the client disconnects mid-call, and after a client timeout. |
| 4 | Named threads on Microsoft's xbdm and on the plugin. |
| 5 | Are `processor=6` and `7` accepted; does `processor` change anything on Microsoft's xbdm? |
| 6 | Which XDK / xbdm versions have `rpc`; does a retail console without the plugin answer `407`? |
| 7 | Do Microsoft's xbdm and the plugin produce identical buffers and status values for the same call? |
| 8 | How does Microsoft's xbdm tell an ordinal from a name pointer at +0x38; does call-by-name work at all? |
| 9 | Status values at +0x00 other than 0, 0x8007007E, 0x8007007F and (plugin) 0x82DA0004. |
| 10 | Is f1 stored as a double for `float` functions on Microsoft's xbdm? |
| 11 | Does the Xbox 360 compiler give a float argument a GPR position, and pass by-value structs in GPRs? |
| 12 | Alignment of `buf_addr` (the client's 8-byte area alignment is relative to it). |
| 13 | Largest `buf_size` before `428`. |
| 14 | Can two `rpc` calls run at once on two connections? |
| 15 | What `xbdm.xex` ordinals 1, 9, 10 and 117 are exactly. |
| 16 | What `0x82DA0101` is, and when it occurs during XDRPC. |
| 17 | Which kernels answer hypervisor call 0x71 with the `HVPP` magic. |
| 18 | XDevkit's receive timeout while polling. |
| 19 | Is the text of the `203` line fixed? |
| 20 | Does Microsoft's xbdm leave the argument slots untouched in the returned buffer? (The plugin does.) |
| 21 | Does the plugin's `rpc` entry get removed when the plugin unloads? |
| 22 | What is the plugin's worker-thread spin gate (context offset +4) for? |
