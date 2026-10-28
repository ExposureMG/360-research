# XBDM

XBDM (Xbox Debug Monitor) is the service on an Xbox 360 development console that lets a PC inspect and control the machine over the network. It is a line-based text protocol on TCP port 730, with a handful of commands that switch to raw binary transfers, plus a small UDP protocol on the same port for finding consoles on the LAN. Through it a PC can browse and copy files, read and write memory, list modules and threads, set breakpoints, reboot or launch titles and receive debug events. Emulators and retail consoles running an XBDM plugin speak the same protocol.

> Status: this was reconstructed from the behaviour of third-party tools (file managers, memory editors, debuggers, a test server) and has not been checked against a console. Wherever tools disagree, or a fact rests on one tool only, the text says so. Everything described as "unknown" needs hardware to settle; those items are collected under [Open questions](#open-questions).

Related protocols that run on the same console: [XRPC](xrpc.md), [XDRPC](xdrpc.md), [JRPC](jrpc.md), [JRPC2](jrpc2.md).

## Overview

### What it is for

XBDM is the transport for most PC-side development and homebrew tooling on the 360: "neighborhood" style file browsers, memory scanners and editors, debuggers, screenshot grabbers and reboot/launch tools. A PC (the client) connects to the console (the server); the console never initiates a connection except to deliver notifications on a connection the client opened.

### At a glance

| Item | Value |
|---|---|
| Transport | TCP, port 730 (the original Xbox uses 731; out of scope here) |
| Discovery | UDP, port 730, broadcast name query |
| Addressing | IPv4 only in every tool seen |
| Framing | ASCII lines ended by CR LF; some commands switch to raw binary after a status line |
| Greeting | Console sends `201- connected` on accept |
| Request form | `command key=value key="quoted value" flag` |
| Response form | `DDD- text`, a 3-digit status code, `-`, a space, text |
| Auth | None. The protocol has no authentication or encryption |
| Memory byte order | Big-endian (PowerPC) |
| Binary length headers | Little-endian (`getfile`, `getmemex`) |
| Numbers in text | `0x` hex for 32-bit, `0q` hex for 64-bit, decimal for some ids |
| Commands in flight | One per connection; no pipelining |
| Concurrent connections | Several allowed; limit unknown (a `401` is returned when exceeded) |
| Timeouts seen in clients | 1 s to 10 s per read; 30 s for screenshots (see [Timeouts](#timeouts)) |
| Keepalive | None in any tool |

Every kind of target (devkits, retail consoles with an XBDM plugin, emulators) is treated as speaking the same protocol. Where a tool was written for one kind of console this is a fact about the tool, not a known protocol difference.

## Background

**Development console (devkit).** A special Xbox 360 whose system software includes a debug monitor. Devkits are driven from a PC with Microsoft's tooling or third-party clients. A devkit has two IP addresses, a debug address and a title address. XBDM answers on the debug address. One client's notes say that on a developer kernel the debug IP must be used (seen in that client only).

**XBDM.** The debug monitor service itself, shipped as `xbdm.xex` in the console software. Retail consoles modified with exploits (for example RGH/JTAG machines) can load an XBDM plugin and then speak the same protocol; one such build identifies itself with the string `Natelx` in the answer to `whomadethis` (see [Console information](#console-information)).

**Title.** The game or application currently running. A "dashboard" is the console's system UI, which is also a title. Titles are `.xex` executables (the 360's PowerPC executable format; the original Xbox used `.xbe`). Titles load around address `0x82000000` in the console's 32-bit address space.

**Modules, threads, sections.** A title is made of loaded modules (executables and libraries), each with sections; code runs in threads. XBDM can enumerate and inspect all three.

**Drives and paths.** XBDM exposes the console's storage as named drives such as `HDD:`, `DEVKIT:`, `GAME:`, `FLASH:`, `E:` and `Z:`. Paths look like `HDD:\folder\file`, with backslashes. The hard drive uses FATX, the Xbox's FAT-derived file system (case-insensitive, names up to 42 characters, restricted character set, timestamps stored as local time according to some FATX research).

**FILETIME.** Windows' 64-bit time format: 100 ns ticks since 1601-01-01. XBDM sends it as two 32-bit hex halves.

**Kernel exports / ordinals.** Executables import functions from other modules by number (ordinal). Some official tooling reads system information by calling into `xbdm.xex` ordinals through [XDRPC](xdrpc.md) rather than through the text protocol; this document covers only the text protocol.

**Notifications.** Besides command/response, the console can push event lines (breakpoint hit, thread created, debug string printed, reboot starting). They arrive on a connection the client has put into notification mode.

## How it works

### Session lifecycle

1. **Find the console** (optional). A UDP broadcast on port 730 returns the console's debug name and address. Alternatively the user types an IP address.
2. **Connect** TCP to the console's port 730. The console sends `201- connected`.
3. **Send commands**, one at a time. Each answer starts with a status line. Depending on the command, the status line is the whole answer (`200`), is followed by a text body ended by a lone `.` (`202`), or is followed by a command-specific binary payload (`203`). `sendfile` runs the other way: the console says `204` and the client then sends raw bytes.
4. **End** with `bye` (answered `200- bye`, then the console closes the socket), or just close the TCP connection.
5. **Optionally** open a second connection and send `notify`, which turns it into a one-way event stream (`205`).

### Single-line answer

```
> dbgname
< 200- MyDevkit
```

### Multi-line answer

```
> drivelist
< 202- multiline response follows
< drivename="DEVKIT"
< drivename="HDD"
< drivename="Z"
< .
```

### Binary download

```
> getfile name="HDD:\file.bin"
< 203- binary response follows
< [4 bytes: length, unsigned 32-bit little-endian]
< [length bytes, then nothing else]
```

### Binary upload

```
> sendfile name="HDD:\file.bin" length=0x2a
< 204- send binary data
> [0x2a raw bytes]
< 200- OK
```

### Error

```
> getfile name="HDD:\nope.bin"
< 402- file not found
```

The error text is free-form (the wording above is illustrative). The code is what matters; after a 4xx the connection stays usable.

### Notification connection

```
> debugger connect override name="mytool" user="host"
< 200- connected
> notify reconnectport=0x3039 reverse
< execution stopped            (may come before the 205 line)
< 205- now a notification channel
< debugstr thread=0xfb000010 lf string=Hello world
```

The texts after the codes in this example are illustrative; no tool quotes them.

## Specification

### Transport and addresses

- TCP port **730**. Port 731 is the original Xbox's XBDM and is out of scope.
- A devkit's debug IP is the one XBDM answers on. The `altaddr` command returns the title address.
- IPv4 only: tools validate IPv4 addresses and skip IPv6 interfaces during discovery.

### Greeting

On accept the console sends exactly one line:

```
201- connected\r\n
```

Clients differ in how strictly they read it: one compares the whole line byte for byte, one lower-cases it first, one just skips to the first `\n`. One client writes its first command before reading the greeting, and that worked, so the console evidently buffers a command sent early (inferred).

Recommended client behaviour: wait for the greeting before the first command; accept `201-` followed by any text (warn if the text is not `connected`, case-insensitively); fail the connection on any other status. A test server should send the greeting immediately on accept and not lose bytes the client sends before it.

### Command lines (client to console)

```
command[ argument]*\r\n
argument = key=value | key="quoted value" | flag
```

- ASCII, terminated by CR LF.
- Values: unquoted decimal or `0x` hex for 32 bits, `0q` hex for 64 bits, quoted strings for names and paths (`name="HDD:\x"`), and bare flags such as `dir`, `running`, `cold`, `clear`, `override`.
- An empty line counts as a command: one client's comment says "Sending duplicate CRLF results in effectively two commands being sent" (seen in that client only). Never send one.
- Key matching is case-insensitive in at least one client's parser. Command names are sent in mixed case by different tools: `BYE` and `bye`; `magicboot COLD` and `magicboot cold`. So command names and flags are probably case-insensitive (inferred).
- Argument order and a trailing space appear not to matter (inferred): one library emits flags, then strings, then numbers, each followed by a space, so one debugger sends `setmem data="0102" addr=0x82000000 ` and `break clear addr=0x...`, where another tool sends `break addr=0x... clear`.
- Lines have a maximum length on the console; the value is unknown. Evidence: one tool writes memory in 0x80-byte pieces "because xbdm caps the command line length"; one debugger splits `setcontext` "due to the size limit"; the error table has `406` and `446` for over-long lines.

Recommended client behaviour: ASCII only; never pipeline; never send an empty line; write command names and flags in lower case; keep `setmem` data to 64 bytes or less per line (the smaller of the two figures in use, 64 and 128).

### Response status line (console to client)

```
DDD- text\r\n
```

Three decimal digits, `-`, a space, then text. Parsers take the code from the first three characters and the text from index 5 (one parser takes the text after index 4 and strips it; another requires more than 4 characters).

| Code | Meaning | Text |
|---|---|---|
| 200 | OK, single-line result | `OK`, or the result itself (`dbgname` gives the name; `bye` gives `bye`) |
| 201 | Connected (greeting only) | `connected` |
| 202 | Multi-line response follows | `multiline response follows` |
| 203 | Binary response follows | `binary response follows` |
| 204 | Ready for binary data from the client | `send binary data` |
| 205 | Connection dedicated, now a notification stream | text not quoted by any tool |

Clients disagree on how exactly they match the 203 and 204 header text; matching on the code is safer.

#### Error codes (4xx)

The names are Microsoft's `XBDM_*` constants as listed by one client (that table is the only source for most names; treat it as unverified). The third column notes where tools have actually met a code.

| Code | XBDM name | Meaning | Where met |
|---|---|---|---|
| 400 | UNDEFINED | Unspecified error | |
| 401 | MAXCONNECT | Too many connections | `401- max number of connections exceeded` |
| 402 | NOSUCHFILE | No such file | `modsections` with an unknown module |
| 403 | NOMODULE | No such module | |
| 404 | MEMUNMAPPED | Memory not mapped | `setmem`; one client tolerates it |
| 405 | NOTHREAD | No such thread | `threadinfo` and related thread commands |
| 406 | CLOCKNOTSET | Clock not set (that client's own note: "linetoolong or clocknotset") | |
| 407 | INVALIDCMD | Unknown command | |
| 408 | NOTSTOPPED | Not stopped | `go` when running |
| 409 | MUSTCOPY | File must be copied, not moved | |
| 410 | ALREADYEXISTS | Already exists | `mkdir` |
| 411 | DIRNOTEMPTY | Directory not empty | |
| 412 | BADFILENAME | Invalid file name | |
| 413 | CANNOTCREATE | Cannot create file | `sendfile` |
| 414 | CANNOTACCESS | Access denied; also a directory given to a file command | `getfile`, `sendfile`, `delete`, `dirlist` |
| 415 | DEVICEFULL | Device full | |
| 416 | NOTDEBUGGABLE | Title not debuggable | |
| 417, 418 | BADCOUNTTYPE, COUNTUNAVAILABLE | Performance counters | |
| 420 | NOTLOCKED | Console not locked | |
| 421 | KEYXCHG | Key exchange required | |
| 422 | MUSTBEDEDICATED | Needs a dedicated connection | |
| 423 | INVALIDARG | Invalid argument | one client's JRPC2 call returns null on it |
| 424, 425 | PROFILENOTSTARTED, PROFILEALREADYSTARTED | Profiling | |
| 426 | ALREADYSTOPPED | Already stopped | `stop` |
| 427-445 | FASTCAPNOTENABLED ... PMCSESSIONNOTACTIVE | Profiling; also NOMEMORY (428), TIMEOUT (429), NOSUCHPATH (430), screen formats (431, 432), FIELDNOTPRESENT (437, `xexfield`) | |
| 446 | LINE_TOO_LONG | Command line too long | |
| 480, 481 | D3D_DEBUG_COMMAND_NOT_IMPLEMENTED, D3D_INVALID_SURFACE | Direct3D | |
| 496, 497 | (Vx) task pending, too many sessions | Voice | |

- The error text is free-form and may carry the reason: one client detects a protected directory by the substring `access denied` in the `dirlist` error line (seen in that client only).
- No tool has seen a 5xx from a console.
- Microsoft's XDevkit library returns HRESULT `0x02DA0000` for success, so XBDM's HRESULT facility is 0x2DA (seen in one source). The mapping of 4xx codes to HRESULTs is unknown.

Recommended client behaviour: parse `DDD-` and an optional space, keep the code and the text, and classify: 2xx is success; 4xx is an error and the connection remains usable; anything else (5xx, 1xx, 3xx, non-digits) is a protocol error and the connection should be dropped. A 2xx other than the one the command should produce (for example 203 where 200 was expected) should also drop the connection, since the payload framing cannot be known.

### Multi-line responses (202)

After the `202-` line come zero or more lines, ended by a line that is exactly `.`. An empty listing is:

```
202- multiline response follows\r\n.\r\n
```

(Clients have had bugs with exactly this case, so test it.)

- Body lines are usually `key=value` lists (`dirlist`, `drivelist`, `walkmem`, `modules`). Others are bare values: decimal thread ids (`threads`), hex strings (`getmem`, the value line of `xexfield`), or `Key: Value` pairs (`hwinfo`).
- No tool handles dot-stuffing (a body line beginning with `.`). None of the commands described here can produce one (inferred).

### Binary responses (203)

There is no generic length field. Each command frames its own payload, and the client must know the framing before it sends the command.

| Command | Framing after the `203-` line |
|---|---|
| `getfile` | 4-byte little-endian unsigned length, then exactly that many bytes, no trailer |
| `getmemex` | Blocks: 2-byte little-endian header (bits 0-14 byte count, bit 15 last block), then that many bytes; repeat |
| `screenshot` | One text line of `key=value` geometry, then `framebuffersize` raw bytes |

One tool has a generic "show me any 203" path that assumes the `getmemex` block framing; that is a guess, not a rule.

### Client-to-console binary (204)

For `sendfile` the console answers `204- send binary data`; the client then sends exactly the announced number of raw bytes, and the console answers once with a status line, `200- OK` on success. Two tools never read that last line, which leaves it in the stream to be misread as the answer to the next command. Implementations disagree here; reading it is correct. See [`sendfile`](#upload-sendfile).

### Dedicated connections (205)

`notify` turns its connection into a one-way notification stream and answers `205`. From then on the console sends notification lines (see [Notifications](#notifications)) and, in every tool seen, the connection takes no further commands (inferred). When a debugger is attached, notification lines may arrive before the 205 line on that connection because the console sends events in the background (seen in one client only).

### Line terminators

Both directions use CR LF. Readers differ: one reads to CR and discards one more byte, one ends a line at LF and drops CRs, one ends at the LF after a CR, one splits on CR LF.

Recommended client behaviour: end a line at LF and strip one trailing CR. A test server should always send CR LF.

### Ending a session; no pipelining

- `bye` is answered `200- bye`, then the console closes the connection. Other tools send `bye` and close without reading the answer.
- One client originally sent every command as `command\r\nbye\r\n` in a single write. That failed on a devkit and was changed to send `bye` only after the response was read (seen in one client only). Do not pipeline.

Recommended client behaviour: one command in flight per connection. Write the next command only after the previous response has been read completely, including any binary payload and trailing status. A server (or test server) should still read robustly: a command split over several TCP segments, or two commands in one segment, are handled in order.

### Timeouts

What existing clients use:

| Client | Value |
|---|---|
| A C++ client | Receive timeout of 10 ms on Linux and 5 s on Windows, used to detect the end of a response. Do not copy this design |
| A C# client | 1 s for connect, send and receive |
| A TypeScript client | 3 s socket timeout |
| A memory editor | 4 s for the greeting; 5 s without progress while reading a line or binary data, then the connection is closed. One reader loop says 5 s but computes 5000 s (a bug) |
| A Python live editor | 8 s per request; 30 s for `screenshot` ("the console can take a few seconds to hand over a frame") |

The protocol itself defines no timeouts and has no end-of-response marker except the status line, the `.` terminator and the per-command binary lengths. Never detect the end of a response by a quiet socket.

Suggested client behaviour: connect 5 s; greeting 5 s; afterwards an idle timeout (no byte received) of 10 s per response, 30 s for `screenshot` and for the status line after `sendfile` data. No total time limit for file data: a large `getfile` may take minutes as long as bytes keep arriving. Make all three settings injectable for tests. Bound short answers as a whole as well as by idle time (otherwise a console trickling one byte just inside the idle timeout can hold a call for the idle timeout times the line limit): the greeting, the answer to `bye`, and every command's answer, including the status after `sendfile` data, get an overall command timeout; only `getfile` and `sendfile` bodies are exempt. Read the greeting, the answer to `bye` and the status after `sendfile` data with a line limit of 512 bytes.

### Several connections

- Consoles accept several connections at once. One debugger keeps a command connection and a notification connection and opens a third for every memory read or write. A memory editor keeps a command connection and an event connection. A web file manager opens a connection per command.
- There is a limit: opening a connection per file of a large folder led to `401- max number of connections exceeded` (seen in one client only). The number is unknown.
- One tool deliberately uses a single connection with a request queue (seen in that tool only).

Recommended client behaviour: one command connection per console and at most one notification connection; do not open a connection per file. On `401`, report "too many connections" and retry once after closing the client's own idle connections.

### Reconnecting, reserving and keepalive

- `notify reconnectport=N` is the only reconnect-related argument seen. One debugger sends `reconnectport=0x00000001`; one editor sends `reconnectport=12345` with `reverse`, or without it. Both get 205 and notifications on the same socket. That tool's own comment: "no idea what reconnectport does". The meaning of `reconnectport` and of `reverse` is unknown.
- `boxid`, `dedicate` (as a command) and any keepalive command appear in no tool. Unknown.
- No tool sends keepalives. One tool polls its own socket state every 2 s, which detects nothing on an idle TCP connection.
- State on the console survives a dropped connection: one tool remembers it stopped the game and sends `go` on the next connection (seen in that tool only).

Recommended client behaviour: no keepalive traffic. If a command connection is found dead when the next command is sent, reopen it once transparently, unless a transfer was in progress. Remember whether this client stopped execution and offer to resume after a reconnect. The greeting is required again after reconnecting; assume no state persists except what the client itself changed (execution stopped, debugger attached).

### Debugger attach

`debugger connect override name="<tool>" user="<host>"` answers 200. One editor sends it before every `break` and before `notify`; one debugger sends it once after connecting. `override` takes over an existing debugger session. Microsoft's XDevkit uses a `ConnectAsDebugger(..., Force)` call for the same purpose. The text of the matching disconnect command is unknown (XDevkit exposes `DisconnectAsDebugger`).

### Discovery (UDP port 730)

#### Packets

All on UDP port 730, one datagram each. The format is commonly called the Name Answering Protocol.

| Type | Direction | Bytes |
|---|---|---|
| 1, forward lookup | client to broadcast | `01`, name length (1 byte), name (ASCII, no terminator) |
| 2, reply | console to client | `02`, name length (1 byte), name (ASCII) |
| 3, wildcard | client to broadcast (or one address) | `03 00` (a 16-bit little-endian 3) |

- Replies come from the console's address, which is the address to connect to.
- Validation differs between tools. One requires `len + 2 == datagram size` in discovery; its parser only requires `len <= size - 2`; another takes everything after byte 2 as the name; another checks only the type byte. A strict sender can rely on `len + 2 == size`; a receiver should be lenient but never read past the datagram.
- One tool checks that the reply to a type-1 lookup carries the name it asked for. Whether the console matches names case-insensitively is unknown.

#### Procedure

- Destination `255.255.255.255:730`. One tool binds one socket per IPv4 interface address so the broadcast leaves on every network (seen in that tool only).
- Wait times: 1 s (one tool); 500 ms with 3 tries (another); 1.5 s per interface and 3 s overall (another).
- Several consoles may answer a wildcard; one tool collects until the timeout, another keeps only the first.
- A unicast type 3 to one address checks that a console is there (seen in one tool only).
- A reply gives the name only. One tool then connects over TCP and asks `dbgname`.
- Whether every console answers UDP name queries is unknown.

#### When UDP is blocked

Every tool also accepts a typed IP address. No tool scans a subnet over TCP. Microsoft's own tooling resolves names through the XDK Neighborhood's default console, which is Windows-only and out of scope.

Recommended client behaviour: broadcast type 3 on every IPv4 interface, collect replies for 1.5 s, retry twice, de-duplicate by address, then ask each console `dbgname` over TCP (its answer wins over the UDP name). Since anyone on the network can send replies, cap the number of addresses kept (for example 256) and put a shared time budget (for example 10 s) on all `dbgname` queries of one search; consoles not asked in time keep their UDP names. To add a console by name, send type 1 and accept a reply whose name matches case-insensitively. When nothing answers, ask the user for an address and remember consoles that have been reached.

### Command support matrix

"Evidence" is how many independent tools use the command.

| Command | Evidence | Section |
|---|---|---|
| `dbgname` | 5 tools | Console information |
| `consoletype` | 2 tools | Console information |
| `getconsoleid` | 1 tool | Console information |
| `xbeinfo` | 5 tools | Console information |
| `getexecstate` | 2 tools | Execution control |
| `altaddr`, `getpid`, `hwinfo` | 1 tool | Console information |
| `whomadethis` | 1 tool | Console information |
| `help` | 1 tool | Console information |
| `consolefeatures` | 1 tool | Console information |
| `systeminfo`, `dmversion` | none | Console information |
| `drivelist` | 4 tools | Drives |
| `drivefreespace` | 4 tools | Drives |
| `dirlist` | 4 tools | Directory listing |
| `getfileattributes` | 1 tool | `getfileattributes` |
| `getfile` | 4 tools | Download |
| `sendfile` | 4 tools | Upload |
| `sendvfile` | none | `sendvfile` |
| `mkdir` | 3 tools | New folder |
| `delete` | 3 tools | Delete |
| `rename` | 3-4 tools | Rename and move |
| `setfileattributes` | none | `setfileattributes` |
| `setsystime` | 2 tools | Clock |
| `screenshot` | 2 tools (one through a library whose source was not examined) | Screenshot |
| `magicboot` | 6 tools | Reboot, launch, shutdown |
| `shutdown` | 3 tools | Reboot, launch, shutdown |
| `dvdeject` | 1 tool | Reboot, launch, shutdown |
| `stop`, `go` | 3 tools | Execution control |
| `suspend`, `resume` | 2 tools | Execution control |
| `halt`, `continue`, `step` | none (only the `singlestep` notification) | Execution control |
| `getmem` | 2 tools | Memory and process state |
| `getmemex` | 2 tools | Memory and process state |
| `setmem` | 3 tools | Memory and process state |
| `setmemex` | none | Memory and process state |
| `walkmem` | 3 tools | Memory and process state |
| `modules`, `modsections`, `xexfield` | 1 tool | Memory and process state |
| `threads`, `threadinfo` | 1 tool | Memory and process state |
| `getcontext`, `setcontext` | 2 tools | Memory and process state |
| `break`, `stopon` | 2 tools | Breakpoints |
| `debugger` | 2 tools | Debugger attach |
| `notify` | 2 tools | Notifications |
| `bye` | 4 tools | Ending a session |

Examples below use `>` for client lines and `<` for console lines; CR LF is implied.

### Console information

**`dbgname`** returns the console's debug name as the text of a 200 line.

```
> dbgname
< 200- MyDevkit
```

`dbgname name=<new>` sets it (unquoted in the one client that does it; seen in that client only).

**`consoletype`** returns the type as text, taken from after `200- `. The set of values is unknown; a test server answers `reviewerkit`. Microsoft's XDevkit has numeric types for development kit, test kit and reviewer kit.

**`getconsoleid`**: `200- consoleid=<id>`; the one client that uses it strips the first 10 characters. The id's format (hex digits, length) is unknown.

**`xbeinfo running`** describes the running title; **`xbeinfo name="<path>"`** a file. Response 202 with `key=value` lines; the `name` value is the path:

```
> xbeinfo running
< 202- multiline response follows
< timestamp=0x00000000 checksum=0x00000000
< name="\Device\Harddisk0\SystemExtPartition\20449700\dash.xex"
< .
```

This body comes from a test server and is illustrative. The real field set, and whether `name` is a device path or a drive path, is unknown. Every tool that uses `xbeinfo` reads only `name`. One tool's author notes `xbeinfo` did not work for modules (seen in that tool only).

**`getexecstate`**: 200 with one word: `start`, `stop`, `pending`, `reboot`, `pending_title`, `reboot_title`. One debugger tests for `start`.

**`altaddr`**: `200- addr=0x<hex>`, the title IP address (seen in one tool only). That tool byte-swaps the number into its IP address type, so the value reads as the address in network order: `0xC0A80102` is 192.168.1.2 (inferred).

**`getpid`**: `200- pid=0x<hex>` (seen in one tool only). That tool byte-swaps it for display; why is unclear.

**`hwinfo`**: 202 with `Key: Value` lines, in order: flags, number of processors, PCI bridge revision, reserved bytes, boot loader magic, boot loader flags (seen in one tool only). Exact text is unknown.

**`whomadethis`**: on the RGH XBDM the text contains `Natelx`; one debugger treats any other answer as devkit XBDM (seen in that tool only). What a console that does not know the command answers (perhaps 407) is unknown.

**`help`**: 202 listing the commands the console knows (seen in one tool only). The line format is unknown. It is the only capability probe in any tool.

**`consolefeatures ver=2 type=<n> params="A\0\A\..."`** is the [JRPC2](jrpc2.md) extension: type 15 reads a temperature, 10 the CPU key, 13 the dashboard version, 16 the title id, 17 the motherboard. One memory editor probes JRPC2 with type 15 and, if the answer is not 200, considers the connection corrupted and closes it (seen in that tool only). Not otherwise covered here; see [JRPC2](jrpc2.md).

**`systeminfo`, `dmversion`**: not in any tool. Unknown whether they exist. Microsoft's XDevkit reads system information (kernel, XDK version, flags, console revision) through [XDRPC](xdrpc.md) calls into `xbdm.xex` ordinals 161 and 140, not through text commands.

Recommended client behaviour: show `dbgname`, `consoletype`, `getconsoleid` and `xbeinfo running` when they answer, and hide each field that errors. Try `systeminfo` and `dmversion` at most once per connection; a 4xx means they are absent.

### Drives

**`drivelist`**: 202, one `drivename="<NAME>"` line per drive, name without colon or backslash.

```
> drivelist
< 202- multiline response follows
< drivename="DEVKIT"
< drivename="HDD"
< drivename="Z"
< .
```

The original Xbox instead answers one 200 line of drive letters; that is not 360 behaviour.

**`drivefreespace name="<NAME>:\"`** with the trailing backslash. Fields, each a 32-bit hex half of a 64-bit byte count: `freetocallerhi/lo`, `totalbyteshi/lo`, `totalfreebyteshi/lo`.

```
> drivefreespace name="HDD:\"
< 202- multiline response follows
< freetocallerhi=0x00000003 freetocallerlo=0x1a2b0000 totalbyteshi=0x00000004 totalbyteslo=0x00000000 totalfreebyteshi=0x00000003 totalfreebyteslo=0x1a2b0000
< .
```

Implementations disagree on the response form: three tools read a 202 with one line; a test server answers a single `200-` line; one tool parses whatever it gets. 202 is the likely real form by majority (inferred). One tool records a drive without sizes when the command fails, so it may fail on some drives.

Friendly names used by file browsers (sourced from the official Neighborhood tooling's naming): `DEVKIT` and `E` Game Development Volume, `HDD` Retail Hard Drive Emulation, `Y` Xbox360 Dashboard Volume, `Z` Devkit Drive, `D` and `GAME` Active Title Media, anything else Volume.

Microsoft's XDevkit library shows that XBDM keeps a table of drive names (up to 35 characters, A-Z and 0-9) mapped to device paths starting with `\`, at most 42 entries, each browsable or not (seen in one source).

Recommended client behaviour: total = `totalbytes`, free = `freetocaller`, used = total - free. Accept 200 or 202. List a drive even if its `drivefreespace` fails, just without sizes. Ignore repeated `drivelist` names (compare case-insensitively) and consider a sane cap (for example 64 drives).

### Directory listing (`dirlist`)

```
> dirlist name="HDD:\Content\"
< 202- multiline response follows
< name="0000000000000000" sizehi=0x0 sizelo=0x0 createhi=0x01d11fb5 createlo=0x59683c00 changehi=0x01d11fb5 changelo=0x59683c00 directory
< name="default.xex" sizehi=0x0 sizelo=0x0004f000 createhi=0x01d11fb5 createlo=0x59683c00 changehi=0x01d11fb5 changelo=0x59683c00
< .
```

- One line per entry; no `.` or `..` entries in any example (inferred).
- Fields: `name` (quoted), `sizehi`/`sizelo` (64-bit size in two 32-bit hex halves), `createhi`/`createlo` and `changehi`/`changelo` (FILETIME halves, see [FILETIME](#filetime)), and a bare `directory` flag for folders. The field order in the example follows the order clients read them; one test server writes `name`, times, then size.
- One tool detects folders with `line.endsWith(" directory")`, so on its console the flag ends the line (inferred); another searches for the flag anywhere in the line.
- Other flags (`readonly`, `hidden`, ...) appear in no tool. Unknown.
- Errors: a path that does not exist gives some non-202 answer; a protected path carries `access denied` in its text. The exact codes (402? 430? 414?) are unknown.
- Trailing backslash: two clients always add one; one client sends drive roots with one (`HDD:\`) and subfolders without. Implementations disagree. A drive root without the backslash (`HDD:`) is untested anywhere.

Recommended client behaviour: always send `dirlist` with exactly one trailing backslash; parse fields by name in any order; treat a missing size as 0 but report it as unknown, and missing times as unknown; ignore unknown flags. Drop entries named `.` or `..`. Skip (and count) entries whose names cannot be sent back unchanged (see [Paths](#paths)).

### `getfileattributes`

`getfileattributes name="<path>"` (seen in one tool only). That tool reads the line after the first as `sizehi sizelo createhi createlo changehi changelo`; a test server answers 202 with one such line. Whether the real reply is 200 or 202, whether it carries the `directory` flag, and what a missing path returns are unknown (a test server says 404, which is wrong for a file, see [Download](#download-getfile)). The one tool uses it to test existence before uploading a folder.

Recommended client behaviour: parse 200 or 202 alike and treat any 4xx as "does not exist or not accessible". Prefer it for existence/size checks but fall back to listing the parent folder when it answers 407 or returns no `sizehi`/`sizelo`. Never take a missing size as 0.

### Download (`getfile`)

```
> getfile name="HDD:\file.bin"
< 203- binary response follows
< [4 bytes: length, unsigned 32-bit little-endian]
< [length bytes]
```

- The length prefix is little-endian in every tool, although the console is big-endian (one tool's comment: "this value is always LE...???"). Unverified on hardware.
- Nothing follows the data: the console neither closes the connection nor sends a status line (three tools read exactly `length` and carry on). One tool drains the socket afterwards "just in case". Implementations disagree on whether anything trails.
- No checksum.
- No chunking in the protocol: the whole file follows as one stream. Clients read it in pieces of 1 KiB, 64 KiB, or the stream's own size.
- Errors come instead of the 203: 414 for a directory or access denied, 402 for no such file (inferred; a test server says 404).
- Files of 4 GiB or more cannot be described by the 32-bit length; what the console does then is unknown. FATX files are smaller than 4 GiB, so this matters only for other file systems.
- Partial transfer and cancel: there is no way to stop a download in the protocol. The only way out is to close the connection.

Recommended client behaviour: read exactly `length` bytes into the sink and only then treat the connection as idle. To cancel, close the connection (reopen it for the next command) and discard the partial local file. Refuse, by closing the connection before reading the body, a `length` larger than the size a listing reported or larger than the sink allows. When no listing gave the size, ask `getfileattributes` first and refuse a file of 4 GiB or more before sending `getfile`: a console might announce such a size modulo 2^32, and the download would end early looking complete. If `getfileattributes` is refused with 407, take the size from a listing of the parent folder; if neither gives one, ask for the file without that bound. Stream into the sink in pieces (for example 64 KiB) and never allocate from the announced length.

### Upload (`sendfile`)

```
> sendfile name="HDD:\file.bin" length=0x2a
< 204- send binary data
> [0x2a raw bytes]
< 200- OK
```

- `length` is hex with `0x` (one client emits `0x%08X`, another `0x2a`). Decimal is untested.
- Errors before 204: 413 cannot create (one client's note: "Does a directory with the same name exist?"); 414 when the path is a directory.
- After the data the console answers one status line; 200 on success. Errors after the data (a full device midway: 415?) are unknown.
- Data is sent as fast as TCP allows (one tool writes 64 KiB pieces; another pipes a stream). One client sleeps 20 ms after every 1 KiB; that is a workaround for its own receive loop, not a protocol need (inferred).
- An existing file of the same name is replaced (inferred from one tool that deletes only clashing folders, not files, before uploading).
- Lengths of 4 GiB or more: unknown. One tool passes the length as a signed 32-bit integer.
- Cancel and drop: one tool removed cancelling an upload because it "corrupted the file on the console" (seen in that tool only). What remains after a dropped connection (a truncated file under the final name? nothing?) is unknown.
- No checksum.
- Two clients never read the `200` after the data, leaving it in the stream.

Recommended client behaviour (safe upload):

1. Upload to a temporary name in the target folder (for example `<name>.<random>.part`, a FATX-valid name of at most 42 characters), then rename to the final name after the `200`.
2. Look the final name up before `sendfile` (a folder there makes the upload fail before any data is sent) and again after `200`.
3. Replace an existing file only if one existed at the first lookup, deleting it right before the rename (after the user has confirmed the replacement). A file that appeared during the upload is left alone: delete the temporary file and fail. A file that appears between the second lookup and the rename cannot be detected on a console whose `rename` replaces an existing target (unknown, see [Open questions](#open-questions)).
4. From the delete on, the temporary file may be the only copy of either version. If the rename fails, or the delete's answer is lost after its line went out, keep the temporary file, do not delete it, and report its name.
5. A cancel or a drop closes the connection; on the next connection delete the temporary file, best effort. That includes a connection that fails after the `sendfile` line was sent but before the 204 was read, because the console may already have created the file.
6. Send exactly the announced number of bytes, never more or fewer.

### `sendvfile`

Not in any tool. Unknown.

### New folder (`mkdir`)

`mkdir name="<path>"`, 200 on success, `410` if it exists. A test server answers 400 for an existing folder; implementations disagree and 410 is the answer two real clients expect. One tool's interface claims "all subdirectories will be created" (seen in that tool only), while another tool creates each level itself when uploading a tree.

Recommended client behaviour: create one level per `mkdir` and map 410 to "already exists".

### Delete

`delete name="<path>"` for a file; `delete name="<path>" dir` for a folder.

- Not recursive: every client empties folders itself, depth first.
- A folder without `dir` gives 414; one client tries without and retries with `dir`. A non-empty folder presumably gives 411 (inferred).
- Deleting a missing path: a test server says 404; the real code is unknown.

Recommended client behaviour: know from the listing whether a path is a folder and send `dir` accordingly; to remove a folder, delete its contents first, depth first, and stop at the first error.

### Rename and move

`rename name="<old full path>" newname="<new full path>"`, 200 on success. It also moves between folders: two tools implement cut-and-paste or move as `rename` to another folder.

- Renaming onto an existing name: one tool deletes the target first, suggesting the console refuses (inferred); a test server refuses with 400. Real behaviour is unknown.
- Across drives: the error table has 409 "must copy, not moved", so a cross-volume rename is probably refused with 409 (inferred).

Recommended client behaviour: keep `rename` inside one folder and one drive; check that the new name does not exist before sending. A new name that differs from the old one only in case is sent without that check (FATX ignores case, so the check would find the file itself). If the console refuses a case-only rename with 410 or 400, rename through an intermediate `<name>.<random>.ren` in the same folder, rename back if the second step fails, and report where the file is if that fails too.

### `setfileattributes`

Not in any tool. Unknown.

### Clock (`setsystime`)

`setsystime clockhi=0x<hex> clocklo=0x<hex>`, a FILETIME. A `tz` argument exists in one tool but its format is unknown (seen in one tool only). 406 "clock not set" probably belongs to reading the clock (inferred).

### Screenshot

```
> screenshot
< 203- binary response follows
< pitch=0x00000c00 width=0x00000500 height=0x000002d0 format=0x... offsetx=0x0 offsety=0x0, framebuffersize=0x...
< [framebuffersize bytes]
```

Framing seen in one tool only (itself a port of a Go client). Fields: `pitch width height format offsetx offsety framebuffersize`; a value may be followed by a comma, which the parser strips. The pixels are the GPU's tiled 32x32 layout and must be untiled; that tool supports only format 8888, 8-in-32 endianness, tiled, unsigned, and refuses others. The console can take seconds to answer. The exact field order and spacing are unknown.

Recommended client behaviour: read the geometry line, check `framebuffersize` against `pitch * height` (allowing tiling slack) and a sane cap (for example 64 MiB), then read the bytes.

### Reboot, launch, shutdown

| Command | Effect | Response |
|---|---|---|
| `magicboot` | Warm reboot. Two tools call it "go to dashboard"; two others call it "restart title" / "soft reboot". Implementations disagree | 200 expected by three tools; one waits 20 ms first because the answer is slow |
| `magicboot cold` / `COLD` | Cold reboot | Implementations disagree: one tool expects the console to close the connection with no answer; one expects 200; one reads a response, then closes |
| `magicboot title="<path>" directory="<dir>"` | Launch an executable | Implementations disagree: one expects 200; one reads nothing ("Nothing should be received but just in case"). `directory` is given without a trailing backslash by two tools and with one by another. A file in the root of `FLASH:` is launched with `title` only (seen in Microsoft's library) |
| `shutdown` | Power off | Connection closed with no answer |
| `dvdeject` | Open the tray | 200 (seen in one tool only) |

One test-server author removed argument checks from its `magicboot` "because the actual XBDM server on an Xbox 360 doesn't do any" (seen in that source only). After any reboot all connections drop; one debugger treats an `execution rebooting` notification as the signal to disconnect.

Recommended client behaviour: treat these as user actions that need confirmation. Send the command and accept either a 2xx or the connection closing within the idle timeout as success; afterwards show the console as rebooting and reopen the command connection on demand.

### Execution control

- `stop`: 200, or 426 when already stopped.
- `go`: 200, or 408 when not stopped.
- `suspend thread=0x<id>` / `resume thread=0x<id>`, 200. One debugger has the two swapped (a bug).
- `getexecstate` (see [Console information](#console-information)).
- `halt`, `continue`, `step`: in no tool. Unknown. One memory editor's `StepThread` is a TODO. Single-stepping shows only as the `singlestep` notification.
- Speed: while a title runs, XBDM gets little CPU time (about 100 KB/s, 28 ms per request); stopped, about 1-1.4 MB/s (seen in one tool only). That tool, and the scanner, dumper and viewer of a memory editor, stop the console around large reads and resume it afterwards, but only if they stopped it.

Recommended client behaviour: offer "pause while reading" (default off for small reads, on above 256 KiB; one tool's threshold is 0x40000). Remember whether this client sent the `stop`, send `go` afterwards only in that case, and do so also after a reconnect.

### Memory and process state

Addresses are 32-bit. Titles load around `0x82000000`.

#### `getmem`

`getmem addr=0x<hex> length=0x<hex>`:

```
> getmem addr=0x82000000 length=0x8
< 202- multiline response follows
< 4D5A9000????????
< .
```

- 202, then lines of hex digit pairs, two per byte, in ascending address order; lines are concatenated.
- An unreadable byte is `??`; one tool zero-fills it and uses a `?` anywhere in the data to call a range protected. Whether `?` comes in pairs is inferred.
- Bytes per line: one tool's comment says "typically 128 when reading big chunks" (seen in that tool only). Clients must not depend on it.
- The total must equal `length`; one tool throws otherwise.
- Hex digit case is unknown; parse both.
- Slow: about 7 KB/s according to one tool (seen in one tool only). Another uses it only up to 128 bytes, where it is faster than `getmemex`.

#### `getmemex`

`getmemex addr=0x<hex> length=0x<hex>`. Details disagree between tools.

```
> getmemex addr=0x82000000 length=0x800
< 203- binary response follows
< [2 bytes LE: 0x0400] [0x400 bytes]
< [2 bytes LE: 0x8400] [0x400 bytes]
```

- Blocks of "up to 0x400 bytes" (seen in one tool only), each after a 2-byte little-endian header: bits 0-14 the byte count; bit 15 is set on the last block (one reading) or is a "status" meaning "most likely reading invalid/protected memory" (another reading).
- When bit 15 comes before `length` bytes have arrived, the rest is unreadable: one tool reports the first unreadable address, the other zero-fills the rest.
- A block larger than what remains: one tool throws; another reads it and keeps only what was asked. Implementations disagree.
- The end: every tool stops as soon as `length` bytes have arrived, whether or not that block has bit 15 set, and reads no further header. A console that sent an empty last block after the data would leave it for the next command.
- Request sizes used successfully: up to 0xC000 ("3/4th of the receive buffer"), 0x10000 in a dumper, 0x8000 while running and 0x20000 while stopped in one other tool. The console's maximum is unknown.

#### `setmem`

`setmem addr=0x<hex> data=<hex digits>`:

```
> setmem addr=0x82000000 data=DEADBEEF
< 200- set 4 bytes
```

- 200 on success; 404 when memory is not mapped, which one tool accepts without error. The 200 text is not quoted by any tool (the one above is an example only).
- Data per command: 64 bytes in one tool, 128 in another; implementations disagree. Both are under the line limit. One debugger sends the data quoted (`data="..."`; seen in that tool only).
- Hex case: one tool uppercase, one lowercase (inferred); both accepted.
- Whether a partly unmapped range is partly written, and what the 200 text says about it, is unknown.

#### `setmemex`

Not in any tool. Unknown.

#### `walkmem`

202, one line per committed region:

```
< base=0x82000000 size=0x00a40000 protect=0x00000004 phys=0x00000000
```

One memory editor skips regions it must not touch, because touching them can freeze the console: for reading, protections with `0x280` (NoCache, ExecuteWriteCopy); for writing, those with `0x1222`. Its comment: "we cannot clear memory caches via XBDM ... without risking freezing the console" (seen in that tool only).

#### Modules and threads

**`modules`**: 202, lines with `name="..." base=0x.. size=0x.. check=0x.. timestamp=0x.. osize=0x..` (seen in one tool only); that tool reads `timestamp` as Unix seconds.

**`modsections name="<module>"`**: 202, lines `name="..." base= size= index= flags=`; 402 when unknown (seen in one tool only).

**`xexfield module="<module>" field=0x10100`**: 202, `fieldsize=0x..` then the value as hex; 437 when the field is absent (seen in one tool only).

**`threads`**: 202, one decimal thread id per line (seen in one tool only).

**`threadinfo thread=0x<id>`**: 202 with one line `suspend= priority= tlsbase= base= limit= slack= nameaddr= namelen= proc= lasterr=`, or 405 (seen in one tool only). The thread name is read from memory at `nameaddr`.

**`getcontext thread=0x<id> control int fp`**: 202, lines `Name=0x...` (32-bit) or `Name=0q...` (64-bit): `Cr Msr Xer Iar Lr Ctr Gpr0..Gpr31` and floating point. One debugger reads them as one argument list, another line by line; implementations disagree in layout only.

**`setcontext thread=0x<id> Name=value ...`**, split over several commands because of the line limit.

**`getpid`**: see [Console information](#console-information).

#### Pointer chains and scans

These are tool features built on the commands above, not protocol: read 4 bytes, interpret big-endian, add an offset, repeat (a value of 0, or above 0xFFFFFFFF, ends the chain). Scans read `walkmem` regions in 64 KiB chunks with an overlap for values crossing a chunk border, often with the console stopped.

Recommended client behaviour for a memory viewer: read with `getmemex` in requests of at most 0x8000 bytes (0x20000 while stopped); fall back to `getmem` (at most 0x400 bytes per request) when `getmemex` answers 407; show unreadable bytes as `??`; start read-only. Write with `setmem`, at most 64 bytes per line, refuse regions whose `walkmem` protection has `0x200` (NoCache), and require an explicit user unlock.

### Breakpoints

- `break addr=0x<hex>` sets an execution breakpoint; `break addr=0x<hex> clear` (one tool) or `break clear addr=0x<hex>` (another) removes it.
- `break read|write|readwrite|execute=0x<hex> size=0x<hex> [clear]` for data breakpoints. One tool sends `read` for read-or-write, the other `readwrite`; implementations disagree.
- `break clearall` (seen in one tool only).
- `stopon fce` makes the console stop on exceptions; one debugger sends it only to the Natelx XBDM because it "causes exceptions frequently on dk xbdm" (seen in that tool only).

### Notifications

Opened on a separate connection (example in [How it works](#notification-connection)). One line per event, the first word naming it:

| Event | Fields |
|---|---|
| `execution` | flag `started`, `stopped`, `pending`, `rebooting`, `reboot_title`, `pending_title` |
| `debugstr` | `thread=`, flags `lf`, `stop`, then `string=` running to the end of the line, unquoted. One library's comment: "the 'string' value in debug logs is weird" |
| `break`, `singlestep` | `addr=`, `thread=`, flag `stop` |
| `data` (one tool) / `databreak` (another); implementations disagree | `addr=`, `thread=`, one of `read= write= readwrite= execute=` |
| `exception` | `code= thread= address=`, `read=` or `write=`, flags `first`, `noncont` |
| `rip` | `thread=` |
| `assert` | `thread=`, then `string=` or flag `prompt` |
| `create` / `terminate` | `thread=`, `start=` |
| `modload` | `name=" " base= size= timestamp= checksum=`, flags `tls`, `xbe` |
| `sectload` / `sectunload` | `name= base= size= index= flags=` |
| `fiber` | `id=`, `start=` or flag `delete` |
| a first word containing `!` | external notification from a plugin |

Recommended client behaviour: treat a notification connection as receive-only; parse known events, keep unknown ones as raw text, and tear down all connections on `execution rebooting`.

### Data encodings

#### Paths

- `DRIVE:\folder\file`, backslash separators (all tools). Drive names have one or more letters (`HDD`, `DEVKIT`, `GAME`, `FLASH`, `E`, `Z`); one tool had to write its own path join after its runtime stopped accepting multi-letter drives.
- A drive root is `DRIVE:\`.
- Executables may be named by device path, e.g. `\Device\Harddisk0\SystemExtPartition\...` (seen in a test server's `xbeinfo` and in Microsoft's drive table).
- Whether forward slashes are accepted, whether names compare case-insensitively, and the maximum path length are unknown.

Recommended client behaviour: if exposing a Unix-style virtual tree, map `/HDD/a/b` to `HDD:\a\b` (drives are the top-level folders) and never send `/`. Compare names case-insensitively for clash checks (FATX rules). Names that contain `:`, `*`, `?`, a control character or a byte above 0x7E cannot be safely sent back; skip them.

#### Quoting

String values are wrapped in `"`; no tool escapes anything inside. Parsers differ on embedded quotes (implementations disagree): two end the value at the next `"`; one treats `\"` as an escape, which would break on a path ending in `\` before the closing quote; one treats `""` as a literal quote. Whether the console understands any escape is unknown. Spaces inside quotes are fine.

Recommended client behaviour: refuse, before sending, any name or path containing `"`, CR, LF, NUL or a byte above 0x7E (FATX forbids `"` in names anyway), and end a quoted value at the next `"` when parsing.

#### FILETIME

64-bit count of 100 ns intervals since 1601-01-01, sent as two 32-bit hex halves (`createhi`/`createlo`, `changehi`/`changelo`, `clockhi`/`clocklo`). Unix seconds = FILETIME / 10,000,000 - 11,644,473,600. Two tools treat it as UTC. Whether the console reports UTC or local time is unknown (FATX itself stores local time according to some FATX research).

#### Numbers

- `0x` hex for 32-bit values (all tools), `0q` hex for 64-bit, plain decimal (thread ids, JRPC2 `type=`). Unprefixed values are parsed as decimal by two tools.
- Leading zeros optional: one tool writes `0x2a`, others `0x0000002A`.
- Hex digits in either case.

Recommended client behaviour: write `0x` with lower-case digits, no padding; accept `0x`, `0X`, `0q`, decimal, upper and lower case when parsing; refuse values that overflow their width.

#### Endianness

- Console memory is big-endian. `getmem` and `getmemex` return bytes in address order, so a 32-bit value reads big-endian (inferred: every decoder copies bytes in order).
- Binary length headers are little-endian (`getfile`, `getmemex`).
- Values inside an emulated original-Xbox title are little-endian; out of scope.

#### Limits in one place

| What | Value | Status |
|---|---|---|
| `getfile` length | 32-bit; file under 4 GiB | all tools |
| `sendfile` length | announced in hex; no maximum known | unknown |
| `getmem` per request | one tool uses up to 128 bytes | suggested client limit: 0x400 |
| `getmemex` per request | 0x8000 to 0x20000 used; block size up to 0x400 | one tool for block size; maximum unknown |
| `setmem` per line | 64 or 128 bytes | implementations disagree |
| Command line | limited, value unknown | unknown |
| Connections | limited, value unknown | unknown |
| Drive table | 42 entries, names up to 35 characters | one source (Microsoft's library) |
| UDP name | 1-byte length | all tools |

### Defensive client practices

Beyond the per-command notes above, these keep a client robust against slow, broken or hostile peers:

1. **Framing.** Read lines to LF, strip one CR, cap a line at 64 KiB (longer is a protocol error) and a 202 body at 16 MiB, counting each line as at least 64 bytes (the memory it costs once parsed). Never interpret a byte stream as lines while a binary payload is pending, and never leave payload bytes in the buffer for the next command.
2. **One command at a time** per connection, enforced by a mutex or a per-connection worker.
3. **Status handling** as under [Response status line](#response-status-line-console-to-client).
4. **Timeouts** as under [Timeouts](#timeouts), idle-based, with a cancel that works at any point. A cancel during a response must drop the connection, because the rest of the response cannot be skipped reliably.
5. **Huge or wrong lengths.** Refuse before reading: a `getfile` length above the size from the listing (when known) or above the sink's limit; a screenshot above the cap; a `getmemex` block above 0x7FFF (impossible) or above what remains (drop the connection; do not read past). Never allocate from a length the console sent.
6. **Silence.** A console that stops sending trips the idle timeout; close the connection, fail the job with "the console stopped responding", and fail queued commands for that console at once instead of each waiting for its own timeout.
7. **Drops mid-transfer.** `getfile`: discard the partial local file. `sendfile`: delete the temporary remote file on the next connection, best effort; never touch the final name until the upload has its 200.
8. **Input checks** before sending: no `"`, CR, LF, NUL or non-ASCII in names; one `mkdir` per level; `rename` within one folder; `setmem` at most 64 bytes per line.
9. **Parsing.** Fields by key in any order; ignore unknown keys and flags; default missing optional fields sensibly; a value that does not parse fails that entry, not the whole listing (and report how many entries were skipped).

## Worked examples

### Connect, identify, browse

```
< 201- connected
> dbgname
< 200- MyDevkit
> drivelist
< 202- multiline response follows
< drivename="HDD"
< drivename="DEVKIT"
< .
> drivefreespace name="HDD:\"
< 202- multiline response follows
< freetocallerhi=0x00000003 freetocallerlo=0x1a2b0000 totalbyteshi=0x00000004 totalbyteslo=0x00000000 totalfreebyteshi=0x00000003 totalfreebyteslo=0x1a2b0000
< .
> dirlist name="HDD:\Content\"
< 202- multiline response follows
< name="0000000000000000" sizehi=0x0 sizelo=0x0 createhi=0x01d11fb5 createlo=0x59683c00 changehi=0x01d11fb5 changelo=0x59683c00 directory
< .
> bye
< 200- bye
```

Free space here: 64-bit value `(0x00000003 << 32) | 0x1a2b0000` = 0x31A2B0000 bytes; total `0x400000000` (16 GiB).

### Safe upload

```
> getfileattributes name="HDD:\Games\a.bin"         (does the final name exist?)
< 402- no such file
> sendfile name="HDD:\Games\a.bin.k3j9.part" length=0x1000
< 204- send binary data
> [0x1000 bytes]
< 200- OK
> rename name="HDD:\Games\a.bin.k3j9.part" newname="HDD:\Games\a.bin"
< 200- OK
```

The exact error for a missing path in the first step is unknown; a client treats any 4xx as "absent".

### Reading memory in blocks

```
> getmemex addr=0x82000000 length=0x800
< 203- binary response follows
< [2 bytes LE: 0x0400] [0x400 bytes]
< [2 bytes LE: 0x8400] [0x400 bytes]
```

Two blocks of 0x400 bytes; the second header has bit 15 set (last block). A client stops as soon as `length` bytes have arrived. If bit 15 had appeared before that point, the remaining bytes would be unreadable and the client reports or zero-fills them (the two readings of bit 15 are described under `getmemex`).

### Small read through `getmem`

```
> getmem addr=0x82000000 length=0x8
< 202- multiline response follows
< 4D5A9000????????
< .
```

The first 4 bytes are readable, the rest are not (`??` per byte).

### Debug events

```
> debugger connect override name="mytool" user="host"
< 200- connected
> notify reconnectport=0x3039 reverse
< 205- now a notification channel
< execution started
< break addr=0x82001234 thread=0xfb000010 stop
< exception code=0xc0000005 thread=0xfb000010 address=0x82001234 read=0x00000000 first
```

All notification texts are illustrative.

### UDP name lookup

```
> 01 08 4D 79 44 65 76 6B 69 74       type 1, name length 8, "MyDevkit" (broadcast to port 730)
< 02 08 4D 79 44 65 76 6B 69 74       type 2 reply from the console's address
> 03 00                               type 3 wildcard: every console replies with its name
```

The name is not NUL-terminated; take exactly `length` bytes.

## Quirks and known inconsistencies

- `201` is only the greeting; no command answers with it.
- `drivefreespace` is a 202 with a single line in most tools but some servers answer a single 200 line.
- `getfile` has no trailer, while `sendfile` has a trailing status line. Two clients skip that line and desynchronise the next command.
- The `getfile` length is a little-endian 32-bit value on a big-endian machine; `getmemex` block headers are likewise little-endian.
- `getmemex` bit 15 is read as "last block" by one tool and as "status/unreadable" by another.
- `magicboot` without arguments is called "go to dashboard" by some tools and "restart title" by others. `magicboot cold` may or may not be answered before the connection drops.
- The `directory` argument of `magicboot title=` is given with and without a trailing backslash by different tools.
- `dirlist` on a drive root: with `HDD:\` known to work; `HDD:` untested; subfolders are sent with and without a trailing backslash.
- `suspend` and `resume` have been implemented swapped in one debugger.
- Error `406` is documented as either "clock not set" or "line too long"; `446` is also "line too long".
- Data breakpoint syntax and event names differ between tools (`read` vs `readwrite`; `data` vs `databreak`).
- Several 4xx codes are only known from a table in one tool; real consoles may use others for the same condition.

### Pitfalls in existing clients (do not copy)

- Detecting the end of a response by a short socket timeout, sleeping after every command, or truncating at the first `.\r\n` in a stream.
- Comparing the exact `203-`/`204-` text, reading a header with a fixed-size single read, or reading the 4-byte length with a single read that may return fewer bytes. Stream reads can return short.
- Not reading the `200` after `sendfile` data.
- Pipelining `bye` behind another command (failed on a devkit).
- Computing a 5000-second timeout where 5 s was meant.
- Test servers with known deviations from real consoles: sending `.` after every `dirlist` entry; truncating `sizehi`/`sizelo` to 16 bits (`size & 0xffff0000`, `size & 0xffff`); answering `magicboot` without CR LF; returning 404 (memory not mapped) for a missing file; sending 204 before knowing the file can be created; 400 for `mkdir` onto an existing name. Do not infer protocol behaviour from those.

## Open questions

These need a real console (or an emulator that implements XBDM) to settle.

1. What exactly does `getfileattributes` return (200 or 202, which fields, the `directory` flag), and which error for a missing path?
2. Which codes does `dirlist` give for a missing folder, a file instead of a folder, and a protected folder? Does `dirlist name="HDD:"` (no backslash) work?
3. Does `dirlist` have flags besides `directory` (read-only, hidden, ...), and is the field order always name, size, times, flags?
4. Is `drivefreespace` 200 or 202, and which drives refuse it?
5. Does `sendfile` onto an existing file replace it? Does it need the parent folder to exist (413)?
6. After a connection drops during `sendfile`, what is left on the console: nothing, a truncated file under the final name, or something else? Does a later command on a new connection work at once?
7. What does the console answer after `sendfile` data when the device fills up, and when does it send it?
8. Can `getfile` and `sendfile` handle 4 GiB or more on any console (`DEVKIT:` and other non-FATX volumes)?
9. Does `rename` onto an existing name fail, and with which code (410)? Does a rename across drives give 409?
10. Does `mkdir` create missing parent folders?
11. Which code does deleting a non-empty folder give (411?), and deleting a missing path?
12. Are path names compared case-insensitively? Are forward slashes accepted? What is the maximum path length?
13. Is there any escape for `"` in quoted values?
14. What is the maximum command line length (406 or 446 when exceeded)?
15. How many connections does a console allow at once (401)? Does it count the notification connection? Is a connection over the limit refused instead of the greeting, or after it?
16. Does the console close idle connections, and after how long? Is a keepalive needed?
17. What do `reconnectport` and `reverse` do on `notify`? Do `boxid` or a `dedicate` command exist?
18. What text does the 205 line carry, and does a dedicated connection accept further commands?
19. Are FILETIMEs from `dirlist` UTC or local time? Does `setsystime` take `tz`, in what format?
20. What values does `consoletype` return, and what do `systeminfo`, `dmversion` and `getconsoleid` return, if they exist?
21. What does `whomadethis` return on a devkit?
22. What does `help` list, and in what format? Can it serve as a capability probe?
23. Does `magicboot cold` answer 200 before the connection closes, or close at once? Does plain `magicboot` go to the dashboard or restart the title? Does `magicboot title=` answer, and is `directory` needed with or without a trailing backslash?
24. Is `getmemex` always present? What is its largest accepted `length`, its block size, and the exact meaning of bit 15?
25. `getmem` on unmapped memory: 202 with `??`, or an error? How many bytes per line?
26. Does `setmem` on a partly unmapped range write the mapped part? What is its 200 text?
27. Do `setmemex`, `sendvfile` and `setfileattributes` exist, and in which form?
28. Which `walkmem` protections are unsafe to read (the NoCache warning)?
29. Does sending a command before the greeting, or two commands in one packet, break any target (the devkit `bye` problem)? Does it depend on the console?
30. Does a type-1 UDP lookup match names case-insensitively? Does every console answer UDP name queries?
31. Is the `getfile` length really little-endian?
32. Does the console create the `sendfile` target before it answers 204, so that a drop right after the 204 leaves an empty file?
33. Does every target implement `getfileattributes`?
34. Is `magicboot title=... directory="DRIVE:\"` accepted for a file in a drive root, or is `title` alone needed there, as with `FLASH:`?
35. Are the formats a client would send accepted everywhere: lower-case `0x` hex without padding, `setmem` data as upper-case hex without quotes?
36. Is `bye` always answered `200- bye` before the close?
37. Does a reboot drop every connection, or only the one that asked for it?
38. Screenshot: the field order and separators of the geometry line, and how `framebuffersize` relates to `pitch * height`.
39. Is a `rename` that changes only the case of a name accepted, and does it change the stored case?
40. What is the real text of each 4xx, and which codes does a console actually use beyond the table above (no tool has seen a 5xx)?
