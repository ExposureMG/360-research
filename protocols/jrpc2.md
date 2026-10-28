# JRPC2

JRPC2 is a console-side plugin (`JRPC2.xex`) that adds one extra command, `consolefeatures`, to the Xbox 360 debug monitor ([XBDM](xbdm.md), TCP port 730). The command lets a PC call arbitrary functions on a running console (by address, or by kernel/system module export and ordinal), pass arguments, get a typed result back, and use a few fixed helpers (CPU key, temperatures, LEDs, on-screen notification, shutdown, persistent memory pokes). JRPC2 has no framing or transport of its own: a call is one XBDM command line out and one status line back.

Status: this document was reconstructed from third-party client tools and from analysis of the plugin binary. It has not been verified against hardware. Facts below are "what the clients send and what the server code appears to do". Where sources disagree or nothing is known, the text says so, and the unknowns are collected under [Open questions](#open-questions).

## Overview

### At a glance

| Item | Value |
|---|---|
| What it is | One XBDM extension command, `consolefeatures`, implemented by a plugin that hooks `xbdm.xex` on the console |
| Used for | Remote function calls on the console (run code, read return values), plus convenience helpers (CPU key, temperature, LEDs, XNotify toast, shutdown, constant memory pokes) |
| Who talks to whom | PC client -> XBDM on the console (TCP 730) -> JRPC2 plugin inside the console's XBDM process |
| Transport | XBDM's TCP connection, port 730 (no separate socket) |
| Framing | One ASCII command line terminated by `\r\n`; one `200-` status line in reply. No `202-` multiline and no `203-` binary replies |
| Endianness | Not applicable on the wire (all values are text). Array payloads sent as hex byte streams have their own byte order, see [Argument tokens](#argument-tokens) |
| Text encoding | ASCII |
| Protocol version | `ver=2`, sent on every line; the server rejects any other value |
| Authentication | None in JRPC2 itself; whoever can reach XBDM can use it |
| Max arguments per call | 37 (client-enforced; server limit unknown) |
| Max constant-memory pokes | 40 (server-enforced) |
| Typical timeouts | Client widens the XBDM conversation/connect timeouts while a call runs, then restores 2000 / 5000. Buffered-result polling interval 250 ms. See [Timeouts](#timeouts) |

### JRPC versus JRPC2

Two different plugins are in circulation and the names are easy to mix up.

| | JRPC2 (this document) | JRPC ([jrpc.md](jrpc.md)) |
|---|---|---|
| Plugin file | `JRPC2.xex` (internal module name `JRPC2`; the file is also seen under the name `JRPC.xex`) | `JRPC.xex`, a separate plugin |
| Transport | Command `consolefeatures` inside [XBDM](xbdm.md), port 730 | Its own TCP server |
| Client | Needs only an XBDM connection (typically through Microsoft's XDK COM wrapper `IXboxConsole.SendTextCommand`) | Opens its own socket |

The most widely distributed .NET client library is a class named `JRPC` (namespace `JRPC_Client`, constant `JRPCVersion = 2`). Despite the name it speaks JRPC2. Every client surveyed reaches the console through the XDK's XBDM text-command call; none opens a raw socket for `consolefeatures`. A client that opens its own TCP socket to issue calls is talking to JRPC, not JRPC2.

Tools sometimes combine several remote-call mechanisms (for example [XRPC](xrpc.md) and [XDRPC](xdrpc.md) alongside JRPC2). The presence of another mechanism in a tool does not tell you which one a given call uses; check what the client actually sends.

### Relation to XBDM

Everything about connecting, the greeting (`201- connected`), `\r\n` line framing, status line syntax (`200-`, `4xx`) and XBDM-level timeouts is [XBDM](xbdm.md) behaviour and is not repeated here. JRPC2 only defines the contents of one command line and the contents of its `200-` reply.

JRPC2 has no memory read/write command. Reading and writing console memory is done with ordinary XBDM memory commands, alongside JRPC2 (see [Memory access](#memory-access)).

## Background

**Devkit and XBDM.** Development consoles ("devkits") run the Xbox Debug Monitor, `xbdm.xex`, which listens on TCP port 730 and accepts text commands from a PC: memory access, module listings, file operations and so on. XBDM is the only network entry point JRPC2 uses.

**XEX, module, title, kernel export.** `.xex` is the Xbox 360 executable and module format (a PE image wrapped in an encrypted/compressed container). Games and dashboards are "titles" and each has a title ID. System modules such as `xboxkrnl.exe` (the kernel) and `xam.xex` (a system library) export functions by ordinal number; an "export" is a function reachable by (module name, ordinal). Calling by export means the plugin looks up the function's address at call time.

**RPC plugin.** A plugin loaded into the console (for example by a loader such as DashLaunch) that hooks XBDM, so that a PC can ask the console to run a function it chooses. Here the PC supplies a target address (or module and ordinal), a list of arguments, and a return type; the plugin performs the call on the console's CPU and formats the result as text.

**PowerPC calling convention.** The Xbox 360 CPU is a big-endian PowerPC. Function arguments travel in general-purpose registers (integers and pointers) and floating-point registers (floats and doubles) and the result comes back in a register. The wire format therefore tags each argument with a type so the plugin can place it in the right kind of register; there is no explicit register index on the wire.

**Title thread versus system thread.** A call can be made on the running title's thread (the default) or on a system thread (the `system` flag).

**By-value only.** There is no pointer or by-reference argument type. To pass a buffer or structure, a client first writes its bytes into console memory (with the XBDM memory-write command) and passes the address as an integer argument.

**HRESULT.** A 32-bit Windows-style status code. The XDK COM wrapper reports XBDM failures as HRESULTs; two appear in the client-side behaviour described below.

## How it works

1. **Connect.** The client opens the XBDM connection to port 730 and reads the `201- connected` greeting (all per [XBDM](xbdm.md)). The JRPC2 plugin must already be loaded on the console; there is no handshake of its own.
2. **Send one command line.** Everything is in a single `consolefeatures` line: version, a `type` selecting a return type or helper, optional thread/module selectors, an array size, and a quoted `params` string holding the call address, argument count and one token per argument.
3. **Read one status line.** The plugin answers `200- <text>`. The client discards everything up to and including the first space and interprets the rest according to the `type` it sent. Failures are also carried in a `200-` line, as text starting `error=`.
4. **Optional polling.** If the text is `buf_addr=<ptr>` the result is not ready; the client sleeps 250 ms and asks again with `consolefeatures buf_addr=0x<PTR>` until the answer no longer contains `buf_addr=`. This is the only multi-round-trip case.

Annotated exchanges (`>` is client to console, `<` the reply):

```
> consolefeatures ver=2 type=17 params="A\0\A\0\"
< 200- Falcon
```
Console type helper. `A\0\` is address 0 (unused), `A\0\` is zero arguments.

```
> consolefeatures ver=2 type=0 as=0 params="A\81A2B3C4\A\3\1\0\7/2\4869\1\4\"
< 200- S_OK
```
Generic call, void return, to address `0x81A2B3C4` with three arguments: integer 0, the 2-byte string "Hi" (hex `4869`), integer 4.

```
> consolefeatures ver=2 type=2 as=0 params="A\81B00000\A\0\"
< 200- buf_addr=0x8E000040
  (client waits 250 ms)
> consolefeatures buf_addr=0x8E000040
< 200- some returned string
```
A call whose result was not ready: the plugin answers with a buffer pointer, the client polls.

## Specification

### Command line

Generic call:

```
consolefeatures ver=2 type=<T>[ system][ module="<MOD>" ord=<ORD>] as=<AS> params="A\<ADDR>\A\<ARGC>\<tok0><tok1>...\"
```

Poll for a buffered result:

```
consolefeatures buf_addr=0x<PTR>
```

| Field | Form | Meaning |
|---|---|---|
| `ver=2` | literal `ver=` + protocol major version (2) | The server rejects a missing or different value (see [Errors](#errors)) |
| `type=<T>` | decimal 0-18 | For 0-9: the return-type id of a generic call. For 10-18: the fixed helper operation (`type` doubles as the operation selector) |
| ` system` | literal word, present only for system-thread calls | Run the call on a system thread. Absent means the title thread |
| ` module="<MOD>" ord=<ORD>` | present only when calling by export | Resolve ordinal `<ORD>` (decimal) of module `<MOD>` and call it. `ADDR` is then `0` |
| ` as=<AS>` | decimal | Array size: number of elements expected from an array-returning call; `0` for scalar and void calls |
| `params="..."` | quoted string | Call address, argument count, argument tokens |

Rules:

- Field order is fixed as shown. XBDM key matching is case-insensitive, so ordering and case are the only things clients take care over (inferred from XBDM behaviour).
- `ADDR` is hexadecimal, upper-case, no `0x` prefix. When calling a raw address, `module`/`ord` are omitted and `ADDR` is the address; when calling by export, `ADDR` is `0`.
- The helper operations (types 10-18, and module resolution) build their own `params` and omit `as=` and `module=`.
- The `buf_addr=` poll is the exception to the upper-case/no-prefix rule: the pointer there is sent with a `0x` prefix. The pointer is parsed as hex from the previous reply and re-sent upper-case.

### The `params` string

```
params="A\<ADDR>\A\<ARGC>\<tok0><tok1>...<tokN-1>"
```

- Literal `A\`, the call address (hex), `\A\`, the argument count `<ARGC>` (decimal), `\`, then one token per argument. Each token ends with `\`, so the string ends with `\` before the closing quote.
- The server parses the two leading fields as the call address and the argument count.
- The whole value is one XBDM quoted string. The backslashes are literal payload bytes, not XBDM escape characters (inferred).

### Argument tokens

Each argument is `<typeid><sep><value>\`. The separator is `\` for scalars and `/` for length-prefixed values (strings and all arrays). Bool is the one scalar that uses `/`.

Argument type ids (these are the same numbers as the return-type ids below):

| Id | Name |
|---|---|
| 1 | Int (32-bit, signed decimal) |
| 3 | Float (decimal text) |
| 7 | ByteArray (length-prefixed hex) |
| 8 | Uint64 (decimal) |

Ids 2, 4, 5, 6 and 9 exist only as return types, except that strings are sometimes tagged 2; see the string-tag note under [Quirks](#quirks-and-known-inconsistencies).

| Source value | Id | Token form | Encoding detail |
|---|---|---|---|
| `uint` | 1 | `1\<int>\` | Bits reinterpreted as a signed 32-bit integer, printed decimal |
| `int` | 1 | `1\<int>\` | Signed decimal |
| `bool` | 1 | `1/<0 or 1>\` | Note the `/` separator (anomaly, produced by the common clients) |
| `byte` | 1 | `1\<int>\` | Promoted to int |
| `short`, `ushort` | 8 or 1 | `8\<u64>\` or `1\<int>\` | Implementations disagree: one widely used client encodes them as Uint64 (id 8), another as Int (id 1) |
| `long`, `ulong` | 8 | `8\<u64>\` | Unsigned 64-bit decimal |
| `float` | 3 | `3\<text>\` | Decimal text. One client formats using the current culture (a bug on locales with a non-`.` decimal separator); another uses the invariant culture |
| `double` | 3 | `3\<text>\` | Same as float |
| string | 7 | `7/<charLen>\<ASCIIHEX>\` | `charLen` is the character count. Payload is the hex of each ASCII byte (two hex digits per char). No NUL terminator |
| `byte[]` | 7 | `7/<len>\<HEX>\` | Upper-case hex, two digits per byte; `len` is the byte count |
| `int[]`, `uint[]` | 7 | `7/<byteLen>\<HEX>\` | Elements flattened to bytes (little-endian, host order, in the common client) and sent as a ByteArray, not as id 5 |
| `float[]` | 7 | `7/<len*4>\<HEX>\` | Each float written big-endian (byte-reversed) |
| any other value | 8 | `8\<u64>\` | Converted to unsigned 64-bit (bool becomes 1/0, a float becomes its IEEE bit pattern as an integer) |

Rules:

- **Maximum 37 arguments** (`0x25`) per call. Both major clients refuse more. Whether the server also enforces 37 is unknown. (The server's separate limit of 40 applies only to the constant-memory table, see [Constant memory poke](#constant-memory-poke-type18).)
- **No by-reference scalars.** All scalars are by value. To pass a pointer, write the bytes to console memory first and pass the address as a `uint`.
- **Arrays and byte buffers are passed by value in the stream**, hex-encoded; the plugin reconstructs them in console-side buffers (inferred; the server allocates per-call buffers and rejects a malformed stream with `error=The paramaters were not found`).
- **Strings are ASCII**, one byte per character.
- **Wide strings** are sent as a byte array containing each character as a 16-bit big-endian unit (`00 c 00 c ...`) followed by `00 00`. Some clients use this to call an export that takes a wide-character string (for example the XNotify export by ordinal).
- **Registers.** The plugin places arguments into integer or floating-point registers by tag: Int is 32-bit, Uint64 is 64-bit, Float loads a floating-point register. There is no register index on the wire. The maximum register/stack-argument layout is not exposed by any client (unknown).
- **Structures** are not a wire type. Write the structure's bytes to memory and pass the address.

### Response

Success:

```
200- <formatted result>
```

The client takes the text after the first space of the status line. Errors are carried inside a `200-` line as `error=<reason>`. XBDM `4xx` status lines indicate a transport or command-level failure.

Classification, in this order:

1. XBDM status 4xx: transport/command error (see [XBDM](xbdm.md)).
2. Text starts with `error=`: call-level failure; the rest of the text is the message. Never parse a value from an `error=` line.
3. Text contains `buf_addr=`: result pending, poll.
4. Otherwise: success, decode per the return type sent.

### Return types

The reply text is decoded according to the `type=` the client sent.

| Return type id | Name | Wire text | Parsed as |
|---|---|---|---|
| 0 | void | `S_OK` | Ignored |
| 1 | int | Hex | Unsigned 32-bit, then reinterpreted as `int`/`short`/`ushort` as requested |
| 2 | string | Raw text | String |
| 3 | float | Decimal | `float` / `double` |
| 4 | byte | Hex | `byte` / `char` |
| 5 | int[] | `h,h,...;` hex elements, comma separated, `;` terminates | 32-bit array |
| 6 | float[] | `f,f,...;` decimal | `float[AS]` |
| 7 | byte[] | `d,d,...;` decimal | `byte[AS]` |
| 8 | uint64 | Hex | 64-bit unsigned |
| 9 | uint64[] | `d,d,...;` decimal | `ulong[AS]` |

Server side: type 0 prints `S_OK`; types 1 and 4 use `%X`; type 2 copies the result string; type 3 uses `%f`; type 8 uses `%llX`. Array types (5, 6, 7, 9) emit a pre-built delimited string. The server's string table contains the format pairs `%i,`/`%i;`, `%f,`/`%f;`, `%X,`/`%X;`, `%llX,`/`%llX;`, plus `%02X`, `%s` and `%d`.

Radix by type: the common client parses type 5 as hex but types 7 and 9 as decimal. Do not use a single radix for all arrays. The server's format-string list also contains a hex 64-bit array pair (`%llX,`/`%llX;`), and which return type uses which pair has not been mapped (see [Open questions](#open-questions)).

### Operations

Types 0-9 are the generic call (`T` = return-type id). Types 10-18 are fixed helpers.

| T | Operation | Syntax | Args | Response text |
|---|---|---|---|---|
| 0 | Call, void | `consolefeatures ver=2 type=0[ system][ module="M" ord=O] as=0 params="A\<ADDR>\A\<N>\...\"` | any | `S_OK` |
| 1 | Call, int | same with `type=1` | any | hex uint32 |
| 2 | Call, string | `type=2` | any | raw text |
| 3 | Call, float | `type=3` | any | decimal |
| 4 | Call, byte | `type=4` | any | hex byte |
| 5 | Call, int[] | `type=5 as=<K>` | any | `h,h,...;` hex |
| 6 | Call, float[] | `type=6 as=<K>` | any | `f,f,...;` decimal |
| 7 | Call, byte[] | `type=7 as=<K>` | any | `d,d,...;` decimal bytes |
| 8 | Call, uint64 | `type=8` | any | hex uint64 |
| 9 | Call, uint64[] | `type=9 as=<K>` | any | `d,d,...;` decimal |
| 9 | ResolveFunction | `consolefeatures ver=2 type=9 params="A\0\A\2\2/<len>\<MODHEX>\1\<ORD>\"` | module name string + ordinal | hex uint32 address |
| 10 | GetCPUKey | `consolefeatures ver=2 type=10 params="A\0\A\0\"` | none | key as 32 hex digits (128-bit key) |
| 11 | ShutDownConsole | `consolefeatures ver=2 type=11 params="A\0\A\0\"` | none | none (console powers off) |
| 12 | XNotify | `consolefeatures ver=2 type=12 params="A\0\A\2\2/<len>\<TEXTHEX>\1\<LOGO>\"` | string + uint logo | `S_OK` |
| 13 | GetKernelVersion | `consolefeatures ver=2 type=13 params="A\0\A\0\"` | none | decimal |
| 14 | SetLeds | `consolefeatures ver=2 type=14 params="A\0\A\4\1\<TL>\1\<TR>\1\<BL>\1\<BR>\"` | 4 x uint | `S_OK` |
| 15 | GetTemperature | `consolefeatures ver=2 type=15 params="A\0\A\1\1\<WHICH>\"` | 1 x int | hex uint32 |
| 16 | XamGetCurrentTitleId | `consolefeatures ver=2 type=16 params="A\0\A\0\"` | none | hex uint32 |
| 17 | ConsoleType | `consolefeatures ver=2 type=17 params="A\0\A\0\"` | none | motherboard name text |
| 18 | ConstantMemorySet | `consolefeatures ver=2 type=18 params="A\<ADDR>\A\5\1\<VAL>\1\<useIf>\1\<IFVAL>\1\<useTID>\1\<TID>\"` | 5 x int | `ok` |

ResolveFunction reuses id 9, the only helper that shares a generic type id; its params are not an array call. The server's error text `Could not resolve function address, params = %s, %d` formats a string and an integer, consistent with a (module name, ordinal) pair.

#### Helper details

- **GetCPUKey (10).** Returns the console's CPU key as 32 hex digits (a 128-bit key). Tools commonly use it as a licence/identity fingerprint.
- **ShutDownConsole (11).** The server calls `HalReturnToFirmware(5)`; no reply is expected.
- **XNotify (12).** Shows an on-screen notification. The logo argument is a value from a roughly 70-entry enumeration of notification icons (values not enumerated in the sources). Some clients fall back to resolving `xam.xex` ordinal `0x282` (decimal 642) and calling it directly if `type=12` fails.
- **GetKernelVersion (13).** Returns the kernel build number as a decimal.
- **SetLeds (14).** Four quadrant LEDs: top-left, top-right, bottom-left, bottom-right. Values: `OFF=0`, `RED=8`, `GREEN=0x80` (128), `ORANGE=0x88` (136); sent as decimal ints.
- **GetTemperature (15).** `WHICH`: `0` = CPU, `1` = GPU, `2` = EDRAM, `3` = motherboard. The value is a hex integer; the unit is not established (clients label it as a Fahrenheit-style temperature but the value appears to be a raw reading).
- **XamGetCurrentTitleId (16).** Server calls `XamGetCurrentTitleId`; returns the running title's ID as hex.
- **ConsoleType (17).** Returns a motherboard name read from a console/processor-revision record: `Xenon`, `Zephyr`, `Falcon`, `Jasper`, `Trinity`, `Corona`, or `Unknown`. Side-effect-free.
- **Kernel exports.** Types 10 and 16 use kernel exports resolved when the plugin installs its hooks (the server resolves imports from `launch.xex`, `xam.xex` and `xboxkrnl.exe`).

#### Constant memory poke (type=18)

Registers a persistent poke that the plugin keeps re-applying. The five int arguments are, in order: `VAL` (the value to write), `useIf` (flag: apply only when the current memory equals `IFVAL`), `IFVAL`, `useTID` (flag: apply only for a given title), `TID` (title ID). The target address is the first params field (`A\<ADDR>\`, hex). The server's table holds up to **40** entries, each stored as `(address, value, applyIfEqual, ifValue, titleId)`. A forty-first yields `error=The limit of 40 has been reached`. Whether pokes can be removed, and the width of the write, are not described by the sources.

### Errors

| Condition | Response text | Notes |
|---|---|---|
| OK with value | `200- <value>` | Decode per return type |
| OK, void / LED / XNotify | `200- S_OK` | |
| OK, constant memory | `200- ok` | |
| `ver=` missing | `error=Version is not specified, expecting major version %d` | |
| `ver=` not 2 | `error=Version mismatch, expected %d but got %d` | |
| `type=` missing | An XBDM-style HRESULT whose low word is `0x17` | Text not known |
| `params=` missing or malformed | `error=The paramaters were not found` | Spelling as in the server |
| Module found, ordinal not | `error=Module param was found but the ordinal was not` | |
| Export could not be resolved | `error=Could not resolve function address, params = %s, %d` | |
| Call address invalid | `error=Call address is invalid` | |
| Constant-memory table full | `error=The limit of 40 has been reached` | |
| Plugin not installed | Reply contains `DEBUG` (XBDM answered an unknown command), or XDK COM HRESULT `0x82DA0007` | Clients report "not installed" |
| Open connection busy | XDK COM HRESULT `0x82DA0100` | One client retries up to 3 times, then fails |
| Unsupported `xbdm.xex` build | Plugin refuses to hook (log strings `Current xbdm.xex is unsupported`, `xbdm.xex not loaded!`) | Seen by the client only as a missing or erroring command (inferred) |

Clients strip the leading `200- error=` (11 characters) and use the remainder as the exception message. On the hard `error=` paths the server also loads the number 730 (`0x2DA`) as its XBDM return value; no client relies on it because they key on the `error=` text.

### Timeouts

| What | Value |
|---|---|
| While a call is outstanding | One client sets the XDK conversation and connect timeouts to `0x3D0900` (4 000 000 ms, about 67 minutes). The unit is the XDK's, inferred to be milliseconds because the restored values below set the same properties. Unverified |
| After the call | Restored to conversation `0x7D0` (2000) and connect `0x1388` (5000) |
| Other client | Default conversation and connect timeouts 4000 ms, widened for the call then restored |
| `buf_addr` poll | 250 ms between polls; no cap on iterations in the clients |

A call runs code on the console and can take seconds, so the budget is generous.

### Constants and limits

| Name | Value |
|---|---|
| Protocol version | 2 |
| Max call arguments | 37 (`0x25`) |
| Constant-memory table | 40 entries |
| `buf_addr` poll interval | 250 ms |
| Return-type ids | 0 void, 1 int, 2 string, 3 float, 4 byte, 5 int[], 6 float[], 7 byte[], 8 uint64, 9 uint64[] |
| Arg type ids | 1 Int, 3 Float, 7 ByteArray, 8 Uint64 (string tag: see [Quirks](#quirks-and-known-inconsistencies)) |
| LED values | OFF 0, RED 8, GREEN 0x80, ORANGE 0x88 |
| Temperature selectors | CPU 0, GPU 1, EDRAM 2, motherboard 3 |
| Motherboard names | Xenon, Zephyr, Falcon, Jasper, Trinity, Corona, Unknown |
| Plugin config string | `Hdd:\JRPC.ini`, sections `Settings` and `Plugins`, keys `plugin%i` (from the plugin's string table; purpose not established) |
| Server image | Encrypted/compressed XEX2 containing a big-endian PowerPC PE; image base `0x91900000`; module name `JRPC2` |

### Preconditions and detection

- The plugin must be loaded on the console (for example through DashLaunch). If it is absent, XBDM answers an ordinary line; clients detect this by `DEBUG` in the reply or COM HRESULT `0x82DA0007`.
- The plugin hooks a specific `xbdm.xex` build and refuses unknown ones (log strings `Initializing hooks for %s`, `Hooks setup`, `Current xbdm.xex is unsupported`, `xbdm.xex not loaded!`).
- At least one XBDM client uses a single `type=15` call as a presence probe and treats any non-`200` answer as a broken connection.

### Memory access

JRPC2 has no memory command. Clients use XBDM's memory commands (`getmemex`/`setmem`; see [XBDM](xbdm.md)). Typed reads and writes (`ReadInt32`, `WriteFloat`, ...) are client-side wrappers that byte-swap around the raw big-endian memory contents. The typical by-reference pattern:

1. Write a code stub or structure into console memory (`setmem`).
2. Pass its address as a `uint` argument to a JRPC2 call, or call the stub's address directly.
3. Read results back from memory (`getmem`).

For instance, one tool resolves two kernel exports, calls the first to allocate, calls the second with the returned handle, writes a 16-byte PowerPC stub to a scratch address, and calls that address.

### Client implementation differences

- Common client builds differ internally but not on the wire.
- `short`/`ushort`: Uint64 (id 8) in one client; Int (id 1) in another.
- Floats: current culture in one client; invariant culture in another.
- `type=5` decoding: one client fills a fixed 8-element `uint[8]` regardless of `as=`; another uses a growable list.
- One client uses static state and a single global connection id; another has per-instance state and timeouts.
- The 64-bit memory-array readers in one client truncate through 32-bit conversion; this is client-side and not on the wire.

## Implementing a client

1. Reuse an existing XBDM client: JRPC2 is one command on that socket.
2. Build the line per [Command line](#command-line) and [The params string](#the-params-string). Hex is upper-case without `0x`, except the `buf_addr=0x...` poll.
3. Marshal arguments per [Argument tokens](#argument-tokens). Prefer fixed-width types (`uint32`, `uint64`, `float`, `double`, string, byte/float/uint arrays); refuse more than 37 arguments. For pointers and structures, `setmem` first and pass the address.
4. Send, read one status line. If the text starts `error=`, fail with that text. If it contains `buf_addr=`, poll. Otherwise decode by the return-type id you sent, watching the hex/decimal split.
5. Expose the helper operations (types 10-18) as typed methods. Implement memory read/write over XBDM, not JRPC2.
6. Probe availability once with `type=17` (no arguments, no side effects). Map `DEBUG` in the reply or HRESULT `0x82DA0007` to "plugin not installed".

Recommended client behaviour:

- Classify replies in the order given under [Response](#response).
- Cap `buf_addr` polling iterations (the existing clients have no cap); for example 40, and apply the per-response idle timeout.
- Use the XBDM per-response idle timeout, raised to at least 5 s for `consolefeatures` since it executes remote code, with no total cap while bytes keep arriving; 250 ms between polls.
- Use the invariant culture for float/double text, tag 16-bit values as Int, and size array results by `as=`, not a fixed count.
- Keep the exact field order and separators; document 37 as the hard argument cap.

## Worked examples

Resolve `xam.xex` ordinal `0x282` (decimal 642) and then call the address. Request and reply for ResolveFunction (string token `2/7` = 7-character string; `78616D2E786578` = "xam.xex"; ordinal in decimal):

```
> consolefeatures ver=2 type=9 params="A\0\A\2\2/7\78616D2E786578\1\642\"
< 200- 81A2B3C4
```

Void call to that address on the title thread with `(uint 0, string "Hi", uint 4)` (illustrative argument list; the real export expects a wide string, see [Argument tokens](#argument-tokens)):

```
> consolefeatures ver=2 type=0 as=0 params="A\81A2B3C4\A\3\1\0\7/2\4869\1\4\"
< 200- S_OK
```

Helpers and a call by export:

```
> consolefeatures ver=2 type=10 params="A\0\A\0\"
< 200- 1234567890ABCDEF1122334455667788
> consolefeatures ver=2 type=15 params="A\0\A\1\1\0\"
< 200- 35                          (CPU, hex)
> consolefeatures ver=2 type=17 params="A\0\A\0\"
< 200- Falcon
> consolefeatures ver=2 type=12 params="A\0\A\2\2/5\48656C6C6F\1\4\"
< 200- S_OK                        (toast "Hello", logo 4)
> consolefeatures ver=2 type=1 system module="xboxkrnl.exe" ord=1 as=0 params="A\0\A\0\"
< 200- 00000002
> consolefeatures ver=2 type=18 params="A\82000000\A\5\1\1\1\0\1\0\1\0\1\0\"
< 200- ok                          (keep *0x82000000 = 1, no guards)
```

Buffered call:

```
> consolefeatures ver=2 type=2 as=0 params="A\81B00000\A\0\"
< 200- buf_addr=0x8E000040
  (sleep 250 ms)
> consolefeatures buf_addr=0x8E000040
< 200- some returned string
```

These exchanges are inferred from client code, not captured from a console.

## Quirks and known inconsistencies

- **String tag.** The argument-token table gives strings as type id 7 (`7/<len>\<hex>\`), and the generic call example uses that. The module-name argument of ResolveFunction and the text argument of XNotify are written with id 2 (`2/<len>\...`). Which tag the server requires for strings is unverified; the two forms may both be accepted.
- **Bool separator.** Bool uses `/` rather than `\`.
- **16-bit arguments.** Tagged Uint64 by one client and Int by another (see above). Pass `int`/`uint` explicitly to avoid depending on it.
- **Float formatting.** Locale-dependent in one client.
- **Hex versus decimal arrays.** Type 5 is hex, types 7 and 9 decimal; type 6 (float[]) is decimal.
- **Fixed 8-element int array.** One client's decoder writes into an 8-element array regardless of `as=`.
- **Array byte order.** In the common client, `int[]`/`uint[]` arguments are flattened little-endian while `float[]` arguments are written big-endian. The console is big-endian, so the int-array order looks inconsistent with the float-array order (inferred); not verified.
- **Handler naming.** The client method for kernel version is spelled "Kernal" in common libraries; the wire is unaffected.
- **Error carried in a `200-` line.** `error=` text arrives with a success status code. The server also sets 730 (`0x2DA`) as an internal return value on these paths; how XBDM renders it on the wire is untested.
- **`bool`/other fallbacks** go through `8\<u64>\`.
- **Folder/label unreliability.** Tools that advertise some other remote-call mechanism frequently also embed a JRPC2 client; do not infer the mechanism from a tool's label.
- **Spelling.** `paramaters` is misspelled in the server's error string.
- **Temperature unit.** Clients label values with a Fahrenheit sign but the value appears to be a raw reading.

## Open questions

- The exact XBDM status code accompanying an `error=` reply: `200` with `error=` text (as clients assume) or a `4xx`? How is the server's return value 730 rendered on the wire?
- `buf_addr` lifecycle: who frees the buffer, and does a second, unrelated call invalidate a pending `buf_addr`? Clients only poll.
- Whether the 37-argument cap is enforced by the server or only by clients, and the real per-call register/stack limit.
- The exact mapping behind `type=17` beyond the seven names (the server reads a console/processor-revision record).
- Temperature units and scaling.
- Behaviour of `type=5` on arrays longer than 8 elements against a real plugin (one client truncates to 8).
- Whether the real plugin accepts 16-bit arguments tagged both as Uint64 and as Int; both clients are in use, so at least one works, possibly both.
- The string tag (2 or 7) the server expects for string arguments, and the token form it uses for ResolveFunction (the client form uses `type=9`).
- Which array return types use the hex `%X`/`%llX` format pairs and which use the decimal `%i` pairs, especially for 64-bit arrays.
- Constant-memory pokes: write width, removal, and the exact meaning of the `useIf`/`useTID` flags.
- The unit of the widened XBDM timeout `0x3D0900` (milliseconds is inferred, not confirmed).
- Purpose of the `Hdd:\JRPC.ini` configuration strings in the plugin.
