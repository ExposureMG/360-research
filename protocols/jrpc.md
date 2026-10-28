# JRPC (TCP)

JRPC is a small server plugin (`JRPC.xex`) that runs on a modified Xbox 360 and lets a PC call functions on the console over a plain TCP connection. The PC sends one text line that names a function (by address, or by module and export ordinal), its arguments and the kind of result expected. The console runs the function and replies with one text line. A handful of built-in actions (read the CPU key, show a notification, shut down, set the LEDs, ...) are selected by the same line using a numeric `type`.

Status: this protocol was reconstructed from third-party tools and from analysis of the plugin binary. It has not been verified against hardware or an emulator. Where something is a guess, it says so in plain words, and the unknowns are collected in [Open questions](#open-questions).

Related documents: [XBDM](xbdm.md), [XRPC](xrpc.md), [XDRPC](xdrpc.md), [JRPC2](jrpc2.md).

## Overview

### JRPC is not JRPC2

The names are easy to mix up, and almost every public client library gets this wrong in practice.

| | JRPC (this document) | [JRPC2](jrpc2.md) |
|---|---|---|
| Plugin file | `JRPC.xex` | `JRPC2.xex` |
| What it is | A standalone TCP server | A plugin that hooks `xbdm.xex` and adds a command to the debug monitor ([XBDM](xbdm.md)) |
| Transport | Its own listening socket, TCP 1409 | The XBDM connection, TCP 730 |
| Command | `consolefeatures ...` as a bare line on the socket | `consolefeatures ...` sent as an XBDM command |
| Greeting | `JRPC2 connected` (sic) | The normal XBDM greeting (`201- connected`) |
| Reply framing | Bare value, no status prefix | XBDM status prefix (`200- `, ...) |
| Memory read/write | Not part of the protocol | Done by the client over XBDM |
| Typical client | None known publicly | The `JRPC.dll` / `JRPC.cs` class shipped in nearly every public tool |

Points that cause confusion:

- The banner of the TCP server literally reads `JRPC2 connected`, and the `ver=2` field in every command is the protocol version number. Neither means the server is JRPC2. This document does not call the TCP protocol "v1".
- The widely distributed `JRPC.dll` / `JRPC.cs` client sends its commands through XBDM (`SendTextCommand`, `DebugTarget.GetMemory`) and opens no TCP socket at all. It is a JRPC2 client.
- The command text is shared: the line built by those clients is also what the TCP server parses. That is why the examples below look like the text any `JRPC.dll` produces. Only the transport and reply framing differ.
- The TCP server's own code uses sockets provided by the console's network library (`xam.xex`); it is not layered on XBDM.

### At a glance

| Item | Value |
|---|---|
| Role | Remote procedure call plus a few system actions on a console |
| Direction | PC (client) connects to the console (server) |
| Transport | TCP, IPv4 |
| Port | 1409 (`0x0581`) |
| Listen address | `INADDR_ANY`, backlog `0x7FFFFFFF` |
| Concurrent clients | Up to 8 (fixed slot table); a 9th connection waits for a free slot |
| Session | Persistent; many commands per connection |
| Server greeting | `JRPC2 connected\r\n` (17 bytes) |
| Request framing | One text line, terminated by LF (`\n`, 0x0A); CRLF tolerated |
| Reply framing | One text line, terminated by CRLF; no status prefix |
| Max request | Server receive buffer is 8500 bytes per connection |
| Max arguments | 37 |
| Text encoding | ASCII; binary data is hex-encoded inline |
| Byte order | Console is big-endian (PowerPC); wire is text, so it only matters for hex blobs and for memory the called function touches |
| Authentication | None |
| Server timeouts | None; `recv` blocks |
| Close | Client sends the line `Bye`, or just closes the socket |
| Config file | `Hdd:\JRPC.ini` (`\Device\Harddisk0\Partition1\JRPC.ini`) |

## Background

Only what is needed to read the rest.

- **Console, devkit, retail.** Xbox 360 hardware comes as retail consoles and as development kits ("devkits"). Both can be modified to run unsigned code. JRPC is a homebrew-style plugin: it is code loaded onto a console that has been set up to run it.
- **XEX.** The Xbox 360 executable format. `JRPC.xex` is a plugin module; `xam.xex` and `xbdm.xex` are system modules; `xboxkrnl.exe` is the kernel. A loaded executable is generally called a *module*.
- **Plugin loading.** Plugins like this are loaded at boot by a loader/patch tool. `JRPC.xex` is expected at `Hdd:\JRPC.xex`, typically alongside `XRPC.xex` (see [XRPC](xrpc.md)). Its config file is `Hdd:\JRPC.ini`.
- **XBDM.** The Xbox Debug Monitor, a built-in debugging service that listens on TCP 730 and speaks a line-based text protocol (see [XBDM](xbdm.md)). JRPC is separate from it.
- **Title.** A running game or application. Each has a numeric title ID. The "title thread" is the context in which title code runs; a "system thread" is one the system owns. JRPC can run a call in either.
- **Kernel export / ordinal.** A module exposes functions to others as numbered exports ("ordinals"). `module="xam.xex" ord=436` means export number 436 of `xam.xex`. This lets a client call system functions without knowing their address, which can vary.
- **PowerPC calling convention (as used by the server).** Integer and pointer arguments go in general-purpose registers `r3`-`r10`, then on the stack. Floating-point arguments go in `f1`-`f8`. An integer result comes back in `r3`, a float in `f1`. The server fills these before jumping to the target and reads the result registers afterwards. The CPU is big-endian.
- **CPU key.** A per-console secret value. Opcode 10 returns it.
- **Risk.** A call runs an arbitrary function in the console's own address space with arguments you supply. A wrong address or argument can crash or hang the console. There is no sandboxing.

## How it works

### Session lifecycle

1. The client opens a TCP connection to `<console IP>:1409`.
2. The server's accept thread assigns the socket to one of 8 slots and immediately sends the banner `JRPC2 connected\r\n`. If all 8 slots are in use, the connection is not serviced and no banner arrives until a slot frees.
3. The client sends one command line ending in `\n`. The server accumulates received data into an 8500-byte per-connection buffer, scans for `\n`, and treats everything up to and including it as one command. Leftover bytes stay for the next read, so two commands in one packet are expected to work (unverified).
4. The server executes the command and writes one reply line ending in `\r\n`. The reply is the bare value; there is no `200-` style prefix.
5. Steps 3-4 repeat on the same socket.
6. The client sends `Bye` to end the session (the server compares each line to the literal `Bye\r\n`), or simply closes the socket. On `Bye` the server closes that socket and frees the slot. It sends nothing back.

### Example exchange

```
S: JRPC2 connected\r\n                                           banner on accept
C: consolefeatures ver=2 type=13 params="A\0\A\0\"\r\n           GetKernelVersion (opcode 13)
S: 17559\r\n                                                     decimal (%d); value illustrative
C: consolefeatures ver=2 type=17 params="A\0\A\0\"\r\n           ConsoleType (opcode 17)
S: Corona\r\n
C: consolefeatures ver=2 type=1 as=0 params="A\82010000\A\2\1\5\1\16\"\r\n
S: 2A\r\n                                                        int result in hex: 0x2A = 42
C: Bye\r\n
```

### Call versus opcode

The `type=` field is overloaded:

- `type` 0-8: a function call. The server resolves the entry point, builds the register frame from `params`, jumps to it, and formats the return value according to the type tag (void, int, string, float, byte, int/float/byte array, int64).
- `type` 9-18: a built-in system opcode with no user function. Opcode 19 also exists but is not understood.

The server checks for opcodes before the generic call path. Because of that, `type=9` is always ResolveFunction, which means a 64-bit array return (which some clients would encode as 9) cannot be reached over this transport.

### Function resolution

- **By address:** the address in `params` is the effective address of the function and is called directly.
- **By ordinal:** ` module="<name>" ord=<n>` makes the server resolve export `n` of that module and call it. The address field is sent as `0`.
- **Thread context:** with the bare flag ` system` the call runs on a system thread; without it, on the title thread. Whether a thread is created per call or reused is unknown.

## Specification

### Request line

```
consolefeatures ver=2 type=<T>[ system][ module="<name>" ord=<ord>] as=<arraysize> params="A\<addrHex>\A\<argc>\<arg>*"\n
```

| Field | Meaning | Type | Notes |
|---|---|---|---|
| `consolefeatures ` | Fixed verb | ASCII | Same verb as the JRPC2 plugin |
| `ver=2` | Protocol version | decimal | Always 2 |
| `type=<T>` | Return-type tag (0-8) or system opcode (9-19) | decimal | See below |
| ` system` | Run on a system thread instead of the title thread | bare flag | Optional |
| ` module="<name>" ord=<n>` | Resolve entry point by module export ordinal | quoted string, decimal | Optional; address field is then `0` |
| `as=<n>` | Expected element count for array returns | decimal | `0` for scalars |
| `params="..."` | Argument block | quoted | See grammar |
| `\n` | Terminator | LF | A preceding `\r` is tolerated |

The examples in the "Worked examples" section for opcodes 10, 12 and 13 omit `as=`. Whether the server requires `as=` for opcodes is unverified.

### Argument block grammar

```
params = A \ <addrHex> \ A \ <argc> \ arg*
arg    = <tag> \ <decimal> \            scalar   (tags 1, 3, 4, 8)
       | <tag> / <count> \ <hex> \      blob     (tags 2, 7)
```

`argc` is the number of `arg` items that follow.

- `addrHex`: target function address in uppercase hex without `0x` (for example `82010000`). For a by-ordinal call it is `0`.
- The two literal `A` characters delimit the address and argument-count sections (the server scans for `A`, then reads digits). `argc` is decimal.
- Each argument is the tag character, a separator, the value, and a closing `\`. The separator is `\` for scalars and `/` for length-prefixed data (strings and byte blobs), for example `2/5\48656C6C6F\` or `1/1\` for a bool. The server keys only off the first character (the tag), and the choice of `/` or `\` is believed not to change decoding (inferred).
- Maximum 37 arguments (`0x25`); the server's call frame is sized for that.
- A line must stay under the 8500-byte server buffer including everything; see [Limits](#limits-and-timeouts).

### Argument tags

| Tag | Name | On-wire form | Server decode | Passed to target as |
|---|---|---|---|---|
| `1` | Int | `1\<decimal>\` (bool: `1/<0\|1>\`) | `sscanf %i` | 32-bit value in a GPR (sign- or zero-extended) |
| `2` | String | `2/<charcount>\<asciiHexPerChar>\` | hex-decode, `malloc` scratch, copy, NUL-terminate | pointer to scratch buffer |
| `3` | Float | `3\<decimalText>\` | `sscanf` float | FPR `f1`-`f8` |
| `4` | Byte | `4\<decimal>\` | `sscanf %i` | 32-bit value in a GPR |
| `5` | IntArray | sent as tag `7` | as byte array | pointer to scratch buffer |
| `6` | FloatArray | sent as tag `7` | as byte array | pointer to scratch buffer |
| `7` | ByteArray | `7/<bytecount>\<hexBytes>\` | hex-decode, `malloc`, copy | pointer to scratch buffer |
| `8` | Uint64 | `8\<decimal>\` | `sscanf %lli` | 64-bit value in a GPR |

Notes:

- Only tags `1`, `2`, `3`, `4`, `7` and `8` ever appear on the wire. Tags `5` and `6` (and a `9`, 64-bit array, used by some clients) are client-side names; clients re-encode int/float arrays as a tag-`7` byte blob before sending.
- Array elements are written big-endian into the blob (4-byte elements for int and float arrays). Single source.
- The server places integer/pointer arguments into `r3`-`r10` then the stack, and float arguments into `f1`-`f8`, then jumps to the target. It has 9 call stubs, selected by the number of float arguments (0 to 8). When a stub needs a float in a GPR slot, the server converts it with `fctidz`. Both are inferred from the server code.
- Scratch buffers for strings and blobs are allocated by the server before the call and freed after it. The caller neither supplies nor learns an address. There is no `buf_addr=` handshake on this transport (that exchange exists in the JRPC2 build).
- The string "charcount" equals the byte count for ASCII text.

### Recommended client behaviour: encoding arguments

These choices are not dictated by the server code. They are conservative and unverified on hardware.

- **Integers.** Send int32, uint32 and bool as tag `1` and byte as tag `4`, in decimal. The server reads both into a signed int with `%i`, so send a uint32 above `INT32_MAX` as the equal negative int32 (a decimal outside the range might be clamped by the C runtime). Bools are `1/0\` and `1/1\`.
- **64-bit.** Send int64 and uint64 as tag `8` in decimal; send a uint64 above `INT64_MAX` as the equal negative int64.
- **Floats.** Send float and double as tag `3`, formatted as `%.9g` (float) and `%.17g` (double) in the C locale, never the process locale. Refuse NaN and infinities, since the text would not parse back. Whether the server keeps a `float` or a `double` is unknown; the digit counts above suit either.
- **Strings.** `2/<byte count>\<hex of the bytes>\`. Refuse embedded NULs, since the string becomes a C string on the console. An empty string would be `2/0\\`; whether the server accepts it is unknown.
- **Blobs and arrays.** Use tag `7` for byte, int32 and float arrays, with big-endian 4-byte elements for ints and floats. An empty blob is encodable, with the same caveat as the empty string.
- **Hex case.** Examples use uppercase hex. Whether lowercase is accepted is unverified.
- **Separators.** `\` after the tag for scalars, `/` for strings and blobs, as in the examples.
- **Targets.** Refuse a call by address with address `0`, because `0` marks a by-ordinal call. A module name should be non-empty printable ASCII without space, quote or backslash (it sits inside a quoted field). Opcode 18 carries its target address in the address field; every other opcode sends `0`.
- **Pre-checks.** Check argument count (at most 37), total line length, and `as` (1 to 8 for array returns, `0` otherwise) before sending anything.

### Function-call types (`type` 0-8)

| type | Name | Return read as | Reply line format | Notes |
|---|---|---|---|---|
| 0 | Void | `r3` ignored | `%X\r\n` of `r3`, or `S_OK\r\n` for some operations | Uncertain which; accept both |
| 1 | Int | `r3`, as int/uint/short/ushort | `%X\r\n` | Unpadded hex, no `0x` |
| 2 | String | `r3` = `char*`, copied out | `%s\r\n` | |
| 3 | Float | `f1` | `%f\r\n` | Six decimals, so tiny values print as 0 |
| 4 | Byte | low 8 bits of `r3` | `%X\r\n` or `%02X` | The two forms are both seen in the server; accept 1 or 2 digits |
| 5 | IntArray | `as` ints from the pointer in `r3` | `%i,%i,...,%i;\r\n` | Signed decimal; at most 8 shown |
| 6 | FloatArray | `as` floats | `%f,%f,...,%f;\r\n` | At most 8 shown |
| 7 | ByteArray | `as` bytes | `%X,%X,...,%X;\r\n` | At most 8 shown |
| 8 | Int64 | `r3` as 64-bit | `%llX\r\n` | Unpadded hex |

The array formats have exactly 8 `%` specifiers. What happens for `as` greater than 8 is unknown.

### System opcodes (`type` 9-19)

| type | Name | Request fields | Reply | Server implementation |
|---|---|---|---|---|
| 9 | ResolveFunction | `addr=0`; `params` carries a tag-`2` module name (hex) then a tag-`1` ordinal | `%X\r\n` (the address) | Resolves the export. On failure the JRPC2 build replies `error=Could not resolve function address, params = %s, %d`; the TCP wording is unknown |
| 10 | GetCPUKey | none (`A\0\A\0\`) | `%X%X\r\n` (high then low 32 bits, concatenated, unpadded) | Reads the CPU key halves |
| 11 | ShutDownConsole | none | none; connection ends | `HalReturnToFirmware(6)` (kernel export 41) |
| 12 | XNotify | `2/<len>\<textHex>\` then `1\<type>\` | none documented | `XNotifyQueueUICustom` (`xam.xex` export 656) |
| 13 | GetKernelVersion | none | `%d\r\n` (signed decimal) | Reads the kernel version halfword |
| 14 | SetLeds | four ints: `1\<tl>\1\<tr>\1\<bl>\1\<br>\` | none documented | LED set via kernel export 40 (a `HalWriteSMBusByte`-style call) |
| 15 | GetTemperature | `1\<which>\` where 0=CPU, 1=GPU, 2=EDRAM, 3=motherboard | `%X\r\n` | Reads the SMC temperature table |
| 16 | GetCurrentTitleId | none | `%X\r\n` | `XamGetCurrentTitleId` (`xam.xex` export 463) |
| 17 | ConsoleType | none | one of `Xenon`, `Zephyr`, `Falcon`, `Jasper`, `Trinity`, `Corona`, `Unknown`, then `\r\n` | Reads the motherboard ID and maps it to a name |
| 18 | constantMemorySet | address in the address field; five ints (below) | none documented | Registers a background task that keeps writing a value to the address |
| 19 | (byte-blob operation) | Builds a byte string from `params` | none known | Internal; likely a raw memory write. Inferred |

Opcode 18 request:

```
params="A\<addrHex>\A\5\1\<value>\1\<useIf>\1\<ifValue>\1\<useTitle>\1\<titleId>\"
```

The argument names suggest two optional guards: write only if the current content equals `ifValue` (enabled by `useIf`), and only while the running title is `titleId` (enabled by `useTitle`). A guard that is not wanted sends `0` and `0`. Write width, interval and how a task is stopped are unknown; all of this is inferred from the field names.

Opcode 12's `type` argument is the notification type; the values are not documented here. The meaning of the four SetLeds values is not documented.

Opcode 17 is useful as a harmless read-only command (see [Implementing a client](#implementing-a-client)).

### Reply formats

| Situation | Reply line |
|---|---|
| Int, uint, byte return | `<hex>\r\n` (`%X`) |
| Int64 return | `<hex>\r\n` (`%llX`) |
| Float return | `<decimal>\r\n` (`%f`) |
| String return | `<text>\r\n` (`%s`) |
| Int array | `v,v,...,v;\r\n` (signed decimal, max 8 elements in the format) |
| Float array | `v,v,...,v;\r\n` (`%f`) |
| Byte array | `v,v,...,v;\r\n` (hex) |
| Void / success | `<hex>\r\n` or `S_OK\r\n` |
| Kernel version | `<decimal>\r\n` (`%d`) |
| Console type | One of the seven names, then `\r\n` |
| Function address not resolvable | JRPC2 build: `error=Could not resolve function address, params = %s, %d`; TCP wording unknown |
| Unsupported XBDM version | JRPC2 build only: `Current xbdm.xex is unsupported!...` |
| Other JRPC2-build error strings | `Version mismatch`, `The paramaters were not found` (the typo is the server's); not known to exist on the TCP server |

The TCP server emits no status-code prefix. JRPC2 clients strip the XBDM `200- ` prefix by cutting the reply at the first space; applying that to a bare TCP reply would corrupt it. A TCP client parses the value from column 0.

### Recommended client behaviour: parsing replies

The wording of failures on the TCP server is not known, so these rules are conservative.

- **Order of checks.** A line starting `error=` is checked first, then a line containing `DEBUG` anywhere (JRPC2 clients treat that as "plugin not installed"), then the line is parsed according to the `type` that was sent. A string result that happens to contain `DEBUG` would be misread, so apply the `DEBUG` rule with that in mind.
- **Terminator.** CRLF; tolerate a bare LF and strip exactly one trailing CR. A lone CR is not a terminator. Cap the line length (64 KiB is a reasonable ceiling).
- **Numbers.** `%X` is 1 to 8 hex digits (types 1, 9, 15, 16), `%llX` is 1 to 16 (type 8), with no sign, no `0x` and no whitespace, padded or unpadded. For a byte (type 4) accept 1 to 8 digits and keep the low 8 bits, since the server may or may not mask. For void accept `S_OK` or 1 to 16 hex digits. For float, expect six decimals; accept `inf` and `nan` spellings. For kernel version, a signed decimal that fits an int32. For console type, exactly one of the seven names, case sensitive.
- **Strings.** The whole line. A NUL, CR or LF inside is a protocol error.
- **Arrays.** `v,v,...,v;` with nothing after the `;`, exactly `as` values. Any other count, a missing `;`, an empty list or a trailing comma is a protocol error. Whether the server really prints `as` values, fewer, or loops past 8 is unknown.
- **Wrong shape.** If a reply does not parse for the `type` sent, the position in the stream is unknown: close the connection. Do the same on timeout, over-long line, a `DEBUG` line, bytes that arrive when none were expected, and a drop. An `error=` line does not require closing.

### Limits and timeouts

| Item | Value | Basis |
|---|---|---|
| Port | 1409 | Server code |
| Simultaneous clients | 8 | Slot array of length 8 |
| Server receive buffer | 8500 bytes per connection | Server code |
| Behaviour when a line exceeds the buffer before `\n` | Scan overflows; the line is dropped | Inferred |
| Suggested max request line | 8191 bytes, excluding the terminator | Safe margin under 8500 |
| Max arguments | 37 (`0x25`) | Server frame size |
| Array elements printed | 8 | Reply format |
| `as` | 1 to 8 for array returns, 0 otherwise | Client-side limit following from the above |
| Server socket timeout | None | `recv` blocks |
| Numeric field overflow | Fields such as `as=` and lengths are clamped at `0x7FFFFFFF` on overflow | Inferred; exact behaviour unclear |

Timeouts a client should choose itself:

- **Connect:** short.
- **Banner:** short, but remember that a 9th client waits without a banner while all 8 slots are in use, so a banner timeout may mean the server is full.
- **Call:** long, because the called function may run arbitrarily long. One workable arrangement is a bound of about 60 seconds on the first byte of a reply (0 for no bound), with the whole call also bounded so a trickling reply cannot stretch it, then about 10 seconds between bytes once the reply has begun. These figures are suggestions, not protocol values.
- **JRPC2 comparison:** the XBDM-based clients set the XBDM conversation timeout to `0x3D0900` (4,000,000 ms, about 66 minutes) while a call runs and restore `0x7D0` (2 s) or `0x1388` (5 s) afterwards. That is an XBDM-client concern, not part of this wire protocol.

### Configuration

The server reads `Hdd:\JRPC.ini` (`\Device\Harddisk0\Partition1\JRPC.ini`) at startup. A sample file seen with the plugin has a `"Settings"` block containing `"KV Stealer Protection"`, and a `"Plugins"` block listing `"plugin1"` to `"pluginN"`. The semantics of these keys are not documented here.

## Implementing a client

1. `connect()` a TCP socket to `<console IP>:1409`.
2. Read one line and expect exactly `JRPC2 connected` followed by CRLF. Do not treat it as a version; it is a banner. A line containing `DEBUG` suggests JRPC is not installed. Any other line is a protocol error. No banner means a timeout, possibly because all 8 slots are in use.
3. For each call, format the command text described in the Specification, end it with `\n` (CRLF is fine) and send it. Keep each line under about 8 KB. Check argument count and length first.
4. Read one CRLF-terminated reply line and parse it by `type` (no status prefix). Lines starting `error=` are failures.
5. Reuse the socket for further calls. When finished, send `Bye\r\n` once on a healthy idle connection. Do not wait for an answer. The server's comparison string includes CRLF, so send CRLF; whether a bare `Bye\n` matches is unknown. Do not send `Bye` after a failure or a cancel; just close.
6. Encode arguments as described above. Byte-reverse multi-byte scalars you later read from console memory (big-endian).
7. Memory read and write are not JRPC commands on this transport. Either resolve and call a console copy/poke routine, use opcode 18, or pair the JRPC connection with an XBDM connection.

Further recommendations:

- **One command in flight.** Send a command only after reading the previous reply. A call that timed out may still be running on the console, so do not reuse that connection.
- **Treat every command as potentially state-changing.** Calls and opcodes can both change the console. Do not blindly retry after a timeout or drop, since the command may have run.
- **Opcodes that might not reply.** It is not documented whether opcodes 12 (XNotify), 14 (SetLeds) and 18 (constantMemorySet) send a reply. One technique is to send opcode 17 (ConsoleType, read-only) immediately behind each of them and read lines until one of the seven console names appears. At most one line may precede it, and that line is the opcode's own answer (`S_OK` or hex accepted; `error=` is a failure reported after the sentinel is read). Two lines before the sentinel, an unrecognised line or a `DEBUG` line means the stream is out of step: close. A console name is the best sentinel because an answer of `0` or other digits could not be told apart from a numeric reply. This relies on the server accepting two lines in one `recv`, which is unverified. Without a barrier, a late reply to such an opcode will be read as the answer to the next command.
- **ShutDownConsole (opcode 11).** The connection ends. Send the command and close without waiting and without `Bye`.
- **CPU key.** The reply is two unpadded `%X` halves run together, so it can only be split reliably if all digits are present. Accept exactly 16 hex digits (the 8-byte example) or 32 (a 16-byte key); report anything else as a protocol error with the raw text. A hex-only reply of other length is ambiguous but does not indicate lost framing, so the connection can stay open.
- **Not reachable over this transport.** Opcode 19, a 64-bit array return, and memory access.

## Worked examples

All lines are ASCII. `·` marks the single trailing `\n` the server scans for; a client may send `\r\n`.

**Call `void f(void)` at `0x82000000`, title thread**

```
consolefeatures ver=2 type=0 as=0 params="A\82000000\A\0\"·
```

Reply: `0\r\n` (or `S_OK\r\n`).

**Call `int g(int a=5, int b=0x10)` at `0x82010000`**

```
consolefeatures ver=2 type=1 as=0 params="A\82010000\A\2\1\5\1\16\"·
```

`0x10` is sent as decimal 16. Reply e.g. `2A\r\n` (42).

**Call by ordinal on a system thread: `ulong h()` in `xam.xex`, ordinal `0x1B4`**

```
consolefeatures ver=2 type=8 system module="xam.xex" ord=436 as=0 params="A\0\A\0\"·
```

Reply e.g. `248173A00\r\n` (`%llX` does not pad, but accept a padded reply too).

**Call `void notify(char *s)` with string `"hi"`**

```
consolefeatures ver=2 type=0 as=0 params="A\82020000\A\1\2/2\6869\"·
```

The server hex-decodes `6869` to `hi`, copies it to a scratch buffer and passes the pointer.

**XNotify("Hello", type 0) (opcode 12; "Hello" is `48656C6C6F`)**

```
consolefeatures ver=2 type=12 params="A\0\A\2\2/5\48656C6C6F\1\0\"·
```

No reply body is documented.

**GetCPUKey (opcode 10)**

```
consolefeatures ver=2 type=10 params="A\0\A\0\"·
```

Reply e.g. `A1B2C3D4E5F60718\r\n` (two `%X` halves concatenated).

**ResolveFunction (opcode 9) for `xam.xex` ordinal 436 (inferred layout)**

```
consolefeatures ver=2 type=9 as=0 params="A\0\A\2\2/7\78616D2E786578\1\436\"·
```

`78616D2E786578` is `xam.xex`. Reply: the resolved address as `%X`. Whether the `module=`/`ord=` attributes are also sent for this opcode is unverified (other clients send the module name in plain ASCII in the quoted field and again as the hex tag-`2` argument).

**constantMemorySet (opcode 18) with no guards (inferred)**

```
consolefeatures ver=2 type=18 as=0 params="A\82001234\A\5\1\4919\1\0\1\0\1\0\1\0\"·
```

Keeps writing the value `4919` (`0x1337`) to address `0x82001234`. No reply documented.

## Quirks and known inconsistencies

- **Naming.** The banner says `JRPC2 connected` and every command carries `ver=2`, yet this is not JRPC2. See [the comparison](#jrpc-is-not-jrpc2).
- **Public clients are not TCP clients.** Nearly every `JRPC.dll` / `JRPC.cs` found in public tool bundles drives the console over XBDM and therefore needs [JRPC2](jrpc2.md). Only the plugin and its config file are the TCP JRPC; there is no known public client for it.
- **No status prefix.** Unlike XBDM and JRPC2, TCP replies have no `200-` or `4xx-` prefix. Client code that cuts at the first space will mangle them.
- **Mixed number bases in replies.** Ints, int64, bytes and byte-array elements are hex (`%X`, no padding); int-array elements and kernel version are signed decimal; floats are `%f` with six decimals.
- **Type 0 reply.** A void call replies either with the hex of `r3` or `S_OK`; which one depends on the operation.
- **Type 4 reply.** The server code has both `%X` and `%02X` forms for bytes.
- **`type=9` collision.** ResolveFunction occupies `type=9`, which some clients would also use for a 64-bit array return.
- **Opcode range.** Opcodes run 9-18 in the documented set; opcode 19 exists in the dispatcher but no client uses it.
- **`as` versus 8 slots.** Array reply formats hold 8 elements even though `as` is an unconstrained decimal field.
- **`DEBUG` heuristic.** JRPC2 clients infer "JRPC not installed" from a reply containing `DEBUG`. A string-returning call whose text contains `DEBUG` could trip it.
- **Uint32 above INT32_MAX.** The server reads tag-`1` values with `%i` into a signed int; large unsigned values should be sent as negative.
- **Memory access gap.** Memory read and write are not in the protocol (JRPC2 clients use XBDM for them).
- **JRPC2 error strings.** `error=...` texts, `Version mismatch`, `The paramaters were not found` and `Current xbdm.xex is unsupported!...` are documented for the JRPC2 build; they may not appear on the TCP server.
- **Float handling.** Floats are decimal text in both directions with `%f` on the return path, so values smaller than 0.0000005 print as 0.
- **Imports.** The server imports `NetDll_*` socket functions plus `XamShowMessageBoxUI` (ordinal 714), `XNotifyQueueUICustom` (656) and `XamGetCurrentTitleId` (463) from `xam.xex`, and HAL/kernel exports from `xboxkrnl.exe`. Server behaviour in this document was derived from analysing the plugin binary.

## Open questions

Everything here needs a console or a trace to settle.

1. **Arrays longer than 8 elements.** The reply formats have exactly 8 slots. Does the server loop the format, truncate at 8, or send `as` elements some other way?
2. **64-bit array returns.** `type=9` is ResolveFunction, so a 64-bit array return cannot reach the generic call path. Is it simply unsupported over TCP, or special-cased?
3. **Exact `setsockopt`.** Level `0xFFFF` with value 1 is assumed to be `SO_REUSEADDR`.
4. **Opcode 19.** Payload and purpose of the byte-blob operation; probably a raw memory write.
5. **`buf_addr=` on TCP.** Absent from the server's strings. Confirm that the server never needs a client-visible scratch address for very large blobs.
6. **CPU key reply.** Two unseparated `%X` values. Is it 8 bytes (16 digits) or a full 16-byte key (32 digits), and is the split fixed 8+8?
7. **Line-length ceiling.** The real maximum command size accepted before the server drops the line, given the 8500-byte buffer and the 37-argument limit.
8. **Thread context.** Does ` system` create a new system thread per call or reuse one? What are the lifetime and blocking guarantees?
9. **Multiple clients.** Behaviour at the 9th concurrent connection, and whether replies can interleave across sockets.
10. **Empty string and empty blob.** Whether `2/0\\` and `7/0\\` are accepted.
11. **Replies from opcodes 12, 14, 18.** Whether any reply is sent, and what the arguments (notification type, LED values, guard semantics, write width, how to cancel a task) mean.
12. **Failure wording on TCP.** What the TCP server says when a call fails or a function cannot be resolved, if anything.
13. **Pipelining.** Whether two commands in one `recv` are both processed.
14. **`Bye` framing.** Whether `Bye` without the CR is recognised.
15. **Hex case.** Whether lowercase hex is accepted in addresses and blobs.
