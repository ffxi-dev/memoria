# Proposal

## Why

Memoria should also run as a native Windower 4 plugin, from the same DLL. Reading Windower's binaries leaves questions that only a test on a live client can answer.

## What Changes

- The Ashita plugin splits into a `Session` that holds everything Memoria does (recording, uploads, linking, live, commands, status, the FFXiMain hooks) and a thin Ashita host that turns Ashita's callbacks into `Session` calls and gives it paths, chat, and a log.
- `src/windower/interface.h`: Windower 4's plugin interface (version `0x4070300`), recovered from Hook.dll 4.7.9.3 and LuaCore 2.6.8.4, each slot noted with its evidence. `docs/windower-interface.md` holds the trace, with addresses.
- A Windower host in `Memoria.dll`, behind the exports `CreateInstance` and `GetInterfaceVersion`, which checks the host's shape before it starts a `Session`.

Out of scope: Windower 5, Lua addons, and captain on Windower.

The session split shipped on main. The interface and host wait on a test on a live Windower client, in a draft PR from the `windower` branch.

## Capabilities

### New Capabilities
- `windower-host`: how `Memoria.dll` runs under Windower 4, and how one `Session` serves both frameworks.

### Modified Capabilities

## Impact

- **Memoria:** `src/plugin/` (the session and the Ashita host), `src/windower/` (interface and host), `docs/windower-interface.md`, `CMakeLists.txt`.
