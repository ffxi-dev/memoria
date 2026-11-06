# Windower 4's plugin interface

Windower 4 has no plugin SDK. This is how its native interface was recovered, statically, for Memoria's `memoria-windower` change. `src/windower/interface.h` is the result; this page is the evidence. What only a live client can settle is marked **(live)**.

No code was copied from any other project. Every claim below rests on a call site in Hook.dll or an implementation in LuaCore, the plugin Windower ships with it, at the address given.

## Builds examined

Memoria targets Hook.dll 4.7.9.3, which the Windower 4.7.9.3 launcher installs:

| File | Version | SHA-256 |
|---|---|---|
| `Hook.dll` | 4.7.9.3 | `68d059e8a9e332b274464d6b744a0732bc4a09a49c67d9c07c301c06122ccaa7` |
| `plugins/LuaCore.dll` | 2.6.8.4 | `e22c9b7ee347f0b106e03dabf1961cb2aebf6003997f2d606a7c21033e11815f` |

The first trace was of the 2024-10-28 build, Hook.dll 4.4.2.0 (`58cf5ac8f04b050200586bb7b9c6fbca9029c0f069a5907f9277fa91e1fe1241`) with LuaCore 2.6.3.1 (`1feadf2bd09e5238800e7754ae8327e2e3c95e16b39cafc349df2231980396b1`). Its interface differs in every table, so Memoria no longer supports it; [Changes since 4.4.2.0](#changes-since-4420) lists what moved.

Both load at their preferred base, `0x10000000`, so every address here is a virtual address in that image. Tools: capstone 5 with pefile (disassembly, vtables from MSVC RTTI, `ret N` per slot), and the binaries' own RTTI and mangled names.

## Changes since 4.4.2.0

| | 4.4.2.0 | 4.7.9.3 |
|---|---|---|
| Interface version | `0x40004` | `0x4070300` |
| Plugin vtable | 21 slots; `Attach` first, info through `GetInfo` into a 0x328-byte struct | 17 slots; `GetAuthor` and `GetCommands` return strings, then `Attach` |
| Unload | `Unload`, then `Release` | `Unload` alone; the plugin deletes itself |
| Command | `HandleCommand(args, keyword)` | `HandleCommand(args)` |
| Gone | `DeviceReset1`, `DeviceReset2`, `unknown14`, `unknown16` | |
| New | | `IsCore` (slot 4), which replaces `KeepOnUnloadAll` |
| Host (`IWindower`) | 11 slots, `GetVersion` first, `GetConsole` 5, `GetFFXI` 10 | 8 slots, no `GetVersion`, `GetConsole` 3, `GetFFXI` 7 |
| Console | 8 slots, `Write` 4, `RunCommand(source, line, flags)` 6 | 6 slots, `Write` 3, `RunCommand(line, source)` 5 |
| Game (`IFFXI`) | 3 slots, `AddToChat` 1 | 1 slot, `AddToChat` 0 |

The chunk, text, mouse, keyboard, and item events keep their arguments and moved up the table. LuaCore's class tree lost `PluginBase`: it is now `IPlugin` <- `WindowerPlugin` <- `LuaCore`.

## Entry points

LuaCore exports `CreateInstance` and `GetInterfaceVersion`, as every plugin did before.

- `GetInterfaceVersion` (`0x10096c40`) is `mov eax, 0x4070300; ret`. LuaCore 2.6.3.1's was `0x40004`.
- `CreateInstance` (`0x10080bd0`) takes no arguments: `operator new(0x2d8)`, the constructor `0x10089e30`, `ret` with nothing popped.
- LuaCore imports nothing from Hook.dll: the host reaches a plugin only through `Attach`.

## Hook.dll's load path

`load <name>` reaches `0x1006eb70`:

1. Appends `.dll` if missing and builds the path under `<Windower>\plugins` (`0x100730b0`).
2. Refuses a missing file ("File does not exist.", `0x1006ed30`). The 4.4.2.0 checks for a file of 255 bytes or less and for an unreadable file are gone. No hash, signature, or allow list: the new imports are `getpeername` and two wide registry calls, no crypto or trust function.
3. Reads the file's version resource (`0x1006ee00` to `0x1006ee62`) for the load message.
4. Refuses a name already loaded ("Plugin already loaded.", `0x1006f012`).
5. `LoadLibraryW` (`0x1006efe3`); on failure it prints the system message for `GetLastError` (`0x1006eff3`).
6. `GetProcAddress("GetInterfaceVersion")` (`0x1006f309`) and calls it. `cmp eax, 0x4070300` at `0x1006f322`: above is "This plugin requires a newer version of Windower to run. (" with the two halves of the number, below is "This plugin is out of date. Please download an updated version." (`0x1006f5de`). Either frees the DLL. Only an exact `0x4070300` goes on.
7. `GetProcAddress("CreateInstance")` (`0x1006f5eb`), calls it (`0x1006f5fc`); null is "Failed to create instance."
8. `plugin->GetAuthor()` (vtable `+0`, `0x1006f709`), kept as a string; `plugin->GetCommands()` (`+4`, `0x1006f77d`), split on `|` into a list.
9. `plugin->Attach(pluginManager)` (`+8`, `0x1006f9b4`), then appends the plugin to its list under a lock (`0x1006f9ed`) and prints "Plugin: {}" (`0x1006fa37`).

The list is a vector of 0x64-byte entries at `PluginManager + 0xa14a8`: `HMODULE`, the plugin, two name strings, the author, the file version, and the command list at entry `+0x58`. Every dispatch walks it in load order, holding the lock at `PluginManager + 0xa14b8`.

Unload: when the Direct3D device is released, `0x1005f2e0` calls `Unload` (`+0xc`, `0x1005f386`) on every plugin and then `FreeLibrary` on its module. "unload all" (`0x10070ba0`) first asks `IsCore` (`+0x10`) and keeps a plugin that answers true; a single "unload" of such a plugin prints " - Core plugins cannot be manually unloaded." (`0x1007064b`).

## The plugin object

LuaCore's `WindowerPlugin` vtable (`0x101a4c34`, 34 slots) starts with `IPlugin`'s 17. Each ends in `ret 4 * (arguments + 1)` with `this` read from the stack, so `__stdcall`, and most forward to a `__thiscall` virtual of `WindowerPlugin`'s own (slots 18 to 33), which `LuaCore` (`0x101a4918`) overrides. Slot 17 is the deleting destructor. LuaCore's local functor classes keep the mangled names of the overrides, which give their signatures, and the event names they raise give their meaning.

| Slot | Offset | `interface.h` | `ret` | Evidence | Confidence |
|---|---|---|---|---|---|
| 0 | `+0x00` | `const char* GetAuthor()` | 4 | `0x1006f709`; WindowerPlugin `0x10098620` returns `this + 0x11c`, "Windower Team" from the constructor (`0x10097e27`) | high |
| 1 | `+0x04` | `const char* GetCommands()` | 4 | `0x1006f77d`, split on `|`; `0x10098610` returns `this + 0x120`, "lua" | high |
| 2 | `+0x08` | `Attach(IWindower*)` | 8 | `0x1006f9b4` passes the PluginManager; `0x10098510` stores it at `+4`, calls the host's slot 3 and keeps the result at `+8`, then its own init (slot 18) | high |
| 3 | `+0x0c` | `Unload()` | 4 | `0x1005f386`, then `FreeLibrary`; `0x100984f0` calls its own unload (slot 19, "Unloaded all addons.") and then the deleting destructor with 1 | high |
| 4 | `+0x10` | `bool IsCore()` | 4 | `0x1007064b` and `0x10070c3c`, see Unload; LuaCore's slot 20 is `mov al, 1` | high |
| 5 | `+0x14` | `Prerender()` | 4 | `BeginScene` wrapper `0x1005e5b0` -> `0x1005efe6`; LuaCore `0x10085e60` raises "prerender" | high |
| 6 | `+0x18` | `Postrender()` | 4 | `EndScene` wrapper `0x1005e630` -> `0x1005f070` -> `0x1005f247`; LuaCore `0x10085da0` raises "postrender" | high |
| 7 | `+0x1c` | `HandleCommand(args)` | 8 | `0x10042280`, see Commands; `0x100984a0` skips one leading space; `Callback_HandleCommand(const char*)` prints the `lua` help | high |
| 8 | `+0x20` | `bool HandleUnhandledCommand(line)` | 8 | `0x1004255f`, stops on true; LuaCore raises "unhandled command" and "addon command" | high |
| 9 | `+0x24` | `IncomingText(original, modified, int* mode)` | 16 | `0x10036336`; `Callback_IncomingText(const char*, char*, int&)`, "incoming text" | high |
| 10 | `+0x28` | `OutgoingText(original, modified, source)` | 16 | `Callback_OutgoingText(const char*, char*, id::command_source)`, "outgoing text" | high |
| 11 | `+0x2c` | `bool IncomingChunk(original, modified, injected, blocked)` | 20 | `0x100710c4`, see Packets; `Callback_IncomingChunk(const void*, void*, bool, bool)`, "incoming chunk" | high |
| 12 | `+0x30` | `bool OutgoingChunk(...)` | 20 | `0x10071093`; the same, "outgoing chunk" | high |
| 13 | `+0x34` | `bool Mouse(type, x, y, delta, blocked)` | 24 | `0x10062238`; `Callback_HandleMouse` is `bool(int, int, int, int, bool)`, "mouse" | high |
| 14 | `+0x38` | `bool Keyboard(dik, pressed, blocked)` | 16 | `0x100619fe`; `Callback_HandleKeyboard` is `bool(int, char, bool)`, "keyboard" | high |
| 15 | `+0x3c` | `AddItem(bag, index, id, count)` | 20 | `0x10071192`; LuaCore raises "add item" | medium |
| 16 | `+0x40` | `RemoveItem(bag, index, id, count)` | 20 | `0x10071242`; LuaCore raises "remove item" | medium |

`Memoria.dll` was checked against this: each of its 17 slots ends in the same `ret N` as LuaCore's `WindowerPlugin`.

## The host

`Attach` gets Hook.dll's `PluginManager`, whose vtable (`0x10185d70`, 9 slots) starts with `IWindower`. Every slot is `__stdcall`; the state slots read the object whose pointer is at `0x101cdf64`.

| Slot | `interface.h` | Returns | Evidence | Confidence |
|---|---|---|---|---|
| 0 | `GetSettings()` | state `+0x10` | `0x10072a80`; WindowerPlugin (`0x10096ca0`) reads three paths from it, at `+0x150`, `+0x1150`, and `+0x2150`, and builds `<+0x2150>\plugins\` from the last | high |
| 1 | `unknown1()` | `[state + 4]` | `0x10072a70` | low |
| 2 | `unknown2()` | `[this + 0xa14b4]` | `0x10072a90` | low |
| 3 | `IConsole* GetConsole()` | `[state + 8]` | `0x10072aa0`; the loader prints "Plugin: {}" through the same object; LuaCore's `windower.send_command` (`0x100796b0`) calls its slot 5 | high |
| 4 | `GetTextHandler()` | `[state + 0x3190]` | `0x10072ab0` | medium (type) |
| 5 | `GetPrimitiveHandler()` | `[state + 0x3194]`, a `PrimitiveHandler` (vtable `0x10185e1c`, stored at `0x1005ed9d`) | `0x10072ac0` | high (type) |
| 6 | `GetPacketStreamHandler()` | `this + 8`, a `PacketStreamHandler` (vtable `0x10185a68`, installed at `0x1006eb23`) | `0x10072ad0` | high (type) |
| 7 | `IFFXI* GetFFXI()` | a static `ffxi` (vtable `0x10178a38`) | `0x10072ae0`; LuaCore's `windower.add_to_chat` (`0x10079550`) calls it, then the result's slot 0 | high |
| 8 | | the deleting destructor, `__thiscall` | `0x1006eae0` | |

`IConsole` (class `Console`, vtable `0x10184bbc`, 6 slots): `SetVisible(bool)`, `IsVisible()`, `SetPosition(float, float)`, `Write(const char*)`, `unknown4()`, `RunCommand(const char*, int)`. `Write` (`0x1003e7f0`, `ret 8`) adds one line through `0x1003e8b0`, the function the loader prints with. `RunCommand` (`0x10041a20`, `ret 0xc`) is what LuaCore's `send_command` calls, as `(text, 0)`.

`IFFXI` (class `ffxi`, vtable `0x10178a38`, 1 slot): `AddToChat(const char* text, int mode)` (`0x10036690`, `ret 0xc`) takes a lock, queues `(mode, text)`, and signals an event; the queue is drained at `0x1003659a`. LuaCore calls it with the addon's mode.

## Packets

The game's receive and send hooks hand Hook.dll each chunk. For each game packet in it, `0x10070f20` (`__thiscall(outgoing, packet, injected)`), holding the plugin lock:

- copies the packet, with a timestamp and its sync counter, into a 0x210-byte record in a table per id: incoming at `PluginManager + 0x1d4a8`, outgoing at `+0x5f4a8`;
- calls every plugin, in load order, with `(the copy, the packet in the chunk, injected, blocked)`, where `blocked` is the OR of what earlier plugins returned, and returns that OR. A plugin returning true blocks the packet.

Injected packets queue through the packet stream handler's slots 0 and 1 (`0x1006d520`, `0x1006d660`) and come through the same function with `injected` set (`0x1006cf4a`).

Hook.dll 4.4.2.0 skipped the plugins for a resend of ten incoming ids it had already seen. 4.7.9.3 has a `PluginManager::CheckDuplicate(bool, const Chunk&)` (RTTI, lambda at `0x10071466`), so it likely still does; which ids it covers is not traced. **(live)**: how often this hides a packet Ashita would record.

So, under Windower:

| Flag | Source |
|---|---|
| injected | the argument |
| blocked | the argument: blocked by a plugin loaded earlier |
| modified | `original` differs from `modified`: changed by a plugin loaded earlier |
| first in chunk | not passed; Memoria uses a new sync counter (bytes 2-3) |

The size is the header's, `(byte 1 & 0xfe) * 2`. No priority exists: Memoria sees what plugins loaded before it did, so it should load last. Whether the packet callbacks run on the thread that calls `Prerender` is **(live)**; Memoria's Windower host assumes it, as Ashita's does.

## Commands

The console's command processor calls `0x10042120` with the line's first word and the rest. Under the plugin lock, for each plugin it compares the first word with each of its `GetCommands` keywords (`0x10037780`). On a match it calls `HandleCommand(rest of the line)` (`0x10042280`) and stops; the keyword is not passed. A line no plugin and no built-in took goes to each plugin's `HandleUnhandledCommand(line)` until one returns true (`0x1004255f`). In the chat line, `//` sends the rest to the console, so `//memoria status` is `memoria status`. Whether the keyword match ignores case, and which thread runs commands, are **(live)**; Memoria queues commands for the next `Prerender`.

## What Memoria's Windower host does with this

`src/windower/host.cpp`:

- `GetInterfaceVersion` answers `0x4070300`; `GetAuthor` is `sruon`, `GetCommands` is `memoria`, `IsCore` is false.
- Hook.dll has already matched the interface version, and the host has no `GetVersion`, so `Attach` checks the shape alone: the host's slots 3 and 7, the console's slot 3, and the game's slot 0 must point inside Hook.dll's image. Otherwise Memoria stays loaded and does nothing, saying why on the console if the console passed, or to the debugger if not.
- Then it starts a `Session` with `<Windower>/memoria/config` and `<Windower>/memoria/captures`, found from the DLL's own path, and writes its log line to the console.
- Chat lines and commands are queued and run on `Prerender`. Chat goes out with `AddToChat(text, 0x8d)`, the mode Hook.dll 4.4.2.0 listed binds with; a command named in a chat line reads `//memoria`. A command reaches the session as `memoria <rest of the line>`.
- `IncomingChunk` and `OutgoingChunk` hand the session the copy as the packet, the chunk's packet as the modified one, and the two flags, with no chunk; the session then marks a chunk's first packet by a new sync counter. They never block.
- `Unload` deletes the plugin, which stops the session first, as Ashita's `Release` does, before Hook.dll frees the DLL.

Known gaps:

- No late login: Hook.dll exposes no player state, so Memoria loaded in a zone waits for the next zone-in to mark a login.
- The resends Hook.dll may hide from plugins (Packets, above) are not recorded.
- Memoria.dll needs the Visual C++ 2015-2022 x86 runtime; Windower's own DLLs link it statically, so a Windower player may lack it. Hook.dll 4.7.9.3 now prints the system error when `LoadLibraryW` fails, which names a missing DLL. The README must say so, or the build must link the runtime statically.
- Memoria.dll has no version resource, so Windower prints version 0.0.0.0.
