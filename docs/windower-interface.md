# Windower 4's plugin interface

Windower 4 has no plugin SDK. This is how its native interface was recovered, statically, for Memoria's `memoria-windower` change. `src/windower/interface.h` is the result; this page is the evidence. What only a live client can settle is marked **(live)**.

No code was copied from any other project. Every claim below rests on a call site in Hook.dll or an implementation in a plugin Windower ships, at the address given.

## Build examined

`E:/Program Files (x86)/Windower`, files dated 2024-10-28, read only:

| File | Version | SHA-256 |
|---|---|---|
| `Hook.dll` | 4.4.2.0 | `58cf5ac8f04b050200586bb7b9c6fbca9029c0f069a5907f9277fa91e1fe1241` |
| `plugins/LuaCore.dll` | 2.6.3.1 | `1feadf2bd09e5238800e7754ae8327e2e3c95e16b39cafc349df2231980396b1` |
| `plugins/PacketFlow.dll` | 0.0.0.8 | `dac12170b0297bebbf8ba1c6e8db5e7b36c2fd7b01bdf1e3cc1ad774e7083fd4` |
| `plugins/Timers.dll` | | `8e3a7122cfc69276b0c808c13500638f0d06b92573bcce2b1467306311befef9` |

All load at their preferred base, `0x10000000`, so every address here is a virtual address in that image. Tools: Ghidra 11.4.1 headless (decompile of every function), capstone 5 with pefile (disassembly, vtables from MSVC RTTI, `ret N` per slot), and the binaries' own RTTI and mangled names.

## Entry points

Every plugin exports exactly `CreateInstance` and `GetInterfaceVersion`.

- `GetInterfaceVersion` is `mov eax, 0x40004; ret` in PacketFlow (`0x1002f410`) and Timers (`0x10050b40`); `0x40002` in ChatMon (`0x100846d0`, 2017), which this Hook.dll would refuse.
- `CreateInstance` takes no arguments and returns the plugin object. PacketFlow `0x1002f420`: `operator new(0x3c0)`, the constructor `0x1002f1c0`, `ret` with nothing popped and no argument read.
- No plugin imports Hook.dll: the host reaches a plugin only through `Attach` (below).

## Hook.dll's load path

`load <name>` reaches `0x10053d40` with the name:

1. Lowercases it, strips `.dll`, and builds `<Windower>\plugins\<name>.dll` from the Windower folder in its state object (`0x1018667c` + `0x2148`); sets the current directory to `plugins` (`0x10053df8`).
2. Refuses a missing file ("File does not exist"), one of 255 bytes or less ("File is not a plugin"), or one it cannot open ("Unable to read plugin", `0x1014224c`). It reads the whole file into a buffer (`0x100540a1`) and never uses the buffer again: `edi`, which holds it, is overwritten at `0x10054107`. No hash, signature, or allow list: Hook.dll imports no crypto or trust function.
3. Reads the file's version resource (`0x10054101`, `0x1005418c`) for the load message.
4. Refuses a name already loaded ("Plugin already loaded", `0x10142270`).
5. `LoadLibraryA(name)` from the `plugins` folder (`0x100542c2`), then restores the current directory.
6. `GetProcAddress("GetInterfaceVersion")` (`0x1005433b`) and calls it. `cmp eax, 0x40004` at `0x1005434b`: above is "This plugin requires a newer version of Windower to run. (%d.%d)" (`0x10142308`), below is "This plugin is out of date. Please download an updated version." (`0x10142358`). Either frees the DLL. Only an exact `0x40004` goes on.
7. `GetProcAddress("CreateInstance")` (`0x10054386`), calls it (`0x10054396`); null is "Failed to create instance".
8. `plugin->GetInfo(buffer)` (vtable `+0xc`, `0x1005445d`) and copies 0x328 bytes from what it returns (`0x10054473`).
9. Prints "Plugin: %s (author: %s, version: %d.%d.%d.%d)" (`0x100544bc`): the file name, `info + 0x100`, the version resource.
10. `plugin->Attach(pluginManager)` (vtable `+0`, `0x100544f5`), then appends the plugin to its list (`0x10054680`).

The list lives in `PluginManager` (its count at `+4`), one 0x44c-byte entry per plugin from `+8`: `HMODULE`, `CreateInstance`, the plugin, its name (0x100), the info copy (0x328, so the command list sits at entry `+0x414`), the file version, a disabled flag at entry `+0x43c`. Every dispatch walks it in load order and skips disabled entries.

Unload (`0x10054d50`): sets the disabled flag, calls `Unload` (`+4`), then `Release` (`+8`), prints the unload line, and `FreeLibrary`. "unload all" first asks `KeepOnUnloadAll` (`+0x3c`, `0x1003bacd`) and skips a plugin that answers true. When the Direct3D device is released, `0x10043730` calls `Unload` and `Release` on every plugin.

## The plugin object

RTTI in every shipped plugin gives the hierarchy `IPlugin` <- `PluginBase` <- `WindowerPlugin` <- the plugin. In PacketFlow, `PluginBase`'s vtable (`0x1004a2d4`) has 22 slots: `IPlugin`'s 21, each ending in `ret 4 * (arguments + 1)` with `this` read from the stack, so `__stdcall`, and the deleting destructor at slot 21, which is `__thiscall` and which Hook.dll never calls. `WindowerPlugin`'s vtable (`0x1004a438`, 70 slots) implements the 21 by turning each into a `__thiscall` virtual of its own (slots 22 to 69), which the plugins override. LuaCore's local functor classes keep the mangled names of those overrides, which gives their signatures, and the event names they raise give their meaning.

| Slot | Offset | `interface.h` | `ret` | Evidence | Confidence |
|---|---|---|---|---|---|
| 0 | `+0x00` | `Attach(IWindower*)` | 8 | Hook.dll `0x100544f5` passes the PluginManager; PluginBase stores it at `+4`; WindowerPlugin (`0x10036a70`) stores it at `+8`, calls the host's slot 5 and keeps the result at `+0xc`, and slot 1 for the settings path | high |
| 1 | `+0x04` | `Unload()` | 4 | `0x10054d50`, before `Release`; LuaCore's (`0x100853a0`) stops its threads | high |
| 2 | `+0x08` | `Release()` | 4 | PluginBase `0x100013f0` calls the deleting destructor with 1 | high |
| 3 | `+0x0c` | `GetInfo(PluginInfo*)` | 8 | `0x1005445d`; WindowerPlugin `0x100393d0` writes the author (`this + 0x10`, "Byrth") to `out + 0x100` and the name (`this + 0x14`, "PacketFlow") to `out + 0x308` | high |
| 4 | `+0x10` | `HandleCommand(args, keyword)` | 12 | `0x1005511a`, see Commands; WindowerPlugin `0x100375c0` skips one leading space and calls slot 63; LuaCore's slot 63 is `Callback_HandleCommand(char*, char*)` and prints the `lua` help | high |
| 5 | `+0x14` | `bool HandleUnhandledCommand(line)` | 8 | `0x1003c7f1`; LuaCore's slot 64 raises "unhandled command" and "addon command" | medium |
| 6, 7 | `+0x18`, `+0x1c` | `DeviceReset1()`, `DeviceReset2()` | 4 | `IDirect3DDevice8::Reset` wrapper (`0x100428e0`) calls `0x10043960` (slot 6) and `0x10043b00` (slot 7); order **(live)** | medium |
| 8 | `+0x20` | `Prerender()` | 4 | `BeginScene` wrapper `0x10042990` -> `0x100432a0`; LuaCore `0x1006fdc0` raises "prerender" | high |
| 9 | `+0x24` | `Postrender()` | 4 | `EndScene` wrapper `0x10042a10` -> `0x10043490`; LuaCore `0x1006fd00` raises "postrender" | high |
| 10 | `+0x28` | `IncomingText(original, modified, int* mode)` | 16 | `0x10032600`; WindowerPlugin -> slot 66, LuaCore's raises "incoming text", functor `Callback_IncomingText(const char*, char*, int&)` | high |
| 11 | `+0x2c` | `OutgoingText(original, modified, source)` | 16 | `0x10031f10`; slot 65, "outgoing text", `Callback_OutgoingText(const char*, char*, ID::CommandSource)` | high |
| 12 | `+0x30` | `bool IncomingChunk(original, modified, injected, blocked)` | 20 | `0x100551a0`, see Packets; WindowerPlugin `0x10037400` -> slot 56; LuaCore `0x100715a0` raises "incoming chunk", functor `Callback_IncomingChunk(const void*, void*, bool, bool)` | high |
| 13 | `+0x34` | `bool OutgoingChunk(...)` | 20 | the same, slot 58, "outgoing chunk" | high |
| 14 | `+0x38` | `unknown14(void*)` | 8 | WindowerPlugin -> slot 67, empty in LuaCore | low |
| 15 | `+0x3c` | `bool KeepOnUnloadAll()` | 4 | "unload all" (`0x1003bacd`) skips a plugin answering true | medium |
| 16 | `+0x40` | `unknown16()` | 4 | WindowerPlugin `0x100374c0` clears its cached player fields; caller unknown | low |
| 17 | `+0x44` | `bool Mouse(type, x, y, delta, blocked)` | 24 | LuaCore `0x100704a0` raises "mouse"; mangled `HandleMouse@LuaCore@@UAG_NHHHH_N@Z`: `__stdcall bool(int, int, int, int, bool)` | high |
| 18 | `+0x48` | `bool Keyboard(dik, pressed, blocked)` | 16 | Hook.dll `0x10045490`; LuaCore `0x100701c0`; mangled `UAG_NHD_N@Z`: `__stdcall bool(int, char, bool)` | high |
| 19 | `+0x4c` | `AddItem(bag, index, id, count)` | 20 | called from the packet parser (`0x10055de0` -> `0x100573e0`); slot 68 | medium |
| 20 | `+0x50` | `RemoveItem(bag, index, id, count)` | 20 | `0x100574d0`; slot 69, where LuaCore raises "remove item" | medium |

`PluginInfo`, 0x328 bytes: `author` at `+0x100` (0x100 bytes), `commands` at `+0x308` (0x20 bytes, `|`-separated). Hook.dll hands an uninitialised buffer, so a plugin clears it. The rest is unread by the code traced.

`Memoria.dll` was checked against this: each of its 21 slots ends in the same `ret N` as PacketFlow's `PluginBase`.

## The host

`Attach` gets Hook.dll's `PluginManager`, whose vtable (`0x1014259c`) starts with `IWindower`. Every slot is `__stdcall`; all but slot 3 read the state object at `0x1018667c`, whose constructor (`0x10046160`) installs the objects named below.

| Slot | `interface.h` | Returns | Evidence | Confidence |
|---|---|---|---|---|
| 0 | `double GetVersion()` | `262148.0` (`0x40004`), `fld [0x10145ae8]` | `0x10057270` | high |
| 1 | `GetSettings(out)` | copies 0x4164 bytes from state `+0x18`, returns `out` | `0x10057290`; WindowerPlugin builds `<out + 0x2130>\plugins\settings\` from it, and `0x2130 + 0x18` is the `0x2148` the loader uses as the Windower folder | medium |
| 2 | `GetWindow()` | state `+8`, the game window | used with `ShowWindow`, `SetWindowPos` | high |
| 3 | `unknown3()` | `this + 0x44c08` | `0x100572c0` | low |
| 4 | `unknown4()` | thread-local initialisation | `0x100572d0` | low |
| 5 | `IConsole* GetConsole()` | state `+0x10` | `0x10057380`; LuaCore's `windower.send_command` (`0x10065e40`) calls its slot 6 | high |
| 6 | `GetTextHandler()` | state `+0x4180`, a `TextHandler` (vtable `0x1014275c`) | `0x10057390` | high (type) |
| 7 | `GetPrimitiveHandler()` | state `+0x4184`, a `PrimitiveHandler` | `0x100573a0` | high (type) |
| 8 | `GetPacketStreamHandler()` | state `+0x6a80`, a `PacketStreamHandler` (vtable `0x101421b8`) | `0x100573b0` | high (type) |
| 9 | `unknown9()` | null | `0x100573c0` | low |
| 10 | `IFFXI* GetFFXI()` | state `+0x23fb0`, an `FFXI` (vtable `0x1013af2c`) | `0x100573d0`; LuaCore's `windower.add_to_chat` queue drains through slot 10, then the result's slot 1 (`0x10075d16`) | high |

`IConsole` (class `Console`, vtable `0x1013bfa4`, 8 slots): slot 4, `Write(const char*)`, is how Hook.dll prints "FFXI Windower" and its credits (`0x10038856`, `ret 8`); slot 6, `RunCommand(int, const char*, int)`, handles `alias` and `bind`, and LuaCore calls it as `(0, text, 0)`.

`IFFXI` (vtable `0x1013af2c`, 3 slots): slot 1, `AddToChat(const char* text, int mode)`, `ret 0xc`, which Hook.dll calls with mode `0x8d` to list binds and LuaCore with the addon's mode.

## Packets

The game's receive and send hooks (`0x100327e0`, `0x10032850`) hand Hook.dll each chunk (`0x100553c0`): incoming from offset 0x1c, past the chunk header. For each game packet in it, in order:

- Ten incoming ids (`0x009`, `0x00A`, `0x00B`, `0x017`, `0x028`, `0x029`, `0x02D`, `0x030`, `0x06F`, `0x0DC`) first go through `0x10055610`, which keeps the last 64 of them and skips the plugins for a packet already seen; `0x00B` clears the list. So plugins never see the server's resends of these. **(live)**: how often this hides a packet Ashita would record.
- `0x100551a0` copies the packet into a table per id (incoming at state `+0x24330`, outgoing at `+0x27330`) and calls every enabled plugin, in load order, with `(that copy, the packet in the chunk, injected, blocked)`, where `blocked` is the OR of what earlier plugins returned. A plugin returning true blocks it; Hook.dll then removes it from the chunk.
- Injected packets come through the same function with `injected` set: the packet stream handler's slot 0 (`0x100527c0`) for outgoing, slot 1 (`0x10052910`) for incoming.

So, under Windower:

| Flag | Source |
|---|---|
| injected | the argument |
| blocked | the argument: blocked by a plugin loaded earlier |
| modified | `original` differs from `modified`: changed by a plugin loaded earlier |
| first in chunk | not passed; Memoria uses a new sync counter (bytes 2-3) |

The size is the header's, `(byte 1 & 0xfe) * 2`. No priority exists: Memoria sees what plugins loaded before it did, so it should load last. Whether the packet callbacks run on the thread that calls `Prerender` is **(live)**; Memoria's Windower host assumes it, as Ashita's does.

## Commands

The console's command processor (`0x10038700`) copies the line and, before any built-in command, calls `0x10054f50`: for each plugin it splits `PluginInfo.commands` on `|` and compares each keyword with the line's first word, ignoring case (`0x100a5f96`). On a match it calls `HandleCommand(rest of the line, past spaces; keyword)` and stops. A line no plugin and no built-in took goes to each plugin's `HandleUnhandledCommand(line)` until one returns true (`0x1003c7f1`). In the chat line, `//` sends the rest to the console, so `//memoria status` is `memoria status`. Which thread runs commands is **(live)**; Memoria queues them for the next `Prerender`.

## What Memoria's Windower host does with this

`src/windower/host.cpp`:

- `GetInterfaceVersion` answers `0x40004`; `GetInfo` gives author `sruon` and command `memoria`.
- `Attach` checks the shape and version before anything else: the host's slots 0, 5, and 10, the console's slot 4, and the game's slot 1 must point inside Hook.dll's image, and `GetVersion()` must be `262148.0`. Otherwise Memoria stays loaded and does nothing, saying why on the console if the console passed, or to the debugger if not.
- Then it starts a `Session` with `<Windower>/memoria/config` and `<Windower>/memoria/captures`, found from the DLL's own path, and writes its log line to the console.
- Chat lines and commands are queued and run on `Prerender`, with `AddToChat(text, 0x8d)`, the mode Hook.dll uses; a command named in a chat line reads `//memoria`.
- `IncomingChunk` and `OutgoingChunk` hand the session the copy as the packet, the chunk's packet as the modified one, and the two flags, with no chunk; the session then marks a chunk's first packet by a new sync counter. They never block.
- `Unload` stops the session, as Ashita's `Release` does, before Hook.dll frees the DLL.

Known gaps:

- No late login: Hook.dll exposes no player state, so Memoria loaded in a zone waits for the next zone-in to mark a login.
- The resends Hook.dll hides from plugins (Packets, above) are not recorded.
- Memoria.dll needs the Visual C++ 2015-2022 x86 runtime; Windower's own DLLs link it statically, so a Windower player may lack it. The README must say so, or the build must link the runtime statically.
- Memoria.dll has no version resource, so Windower prints version 0.0.0.0.
