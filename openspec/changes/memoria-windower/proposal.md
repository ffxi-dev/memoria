# Proposal

## Why

This change makes Memoria a native Windower 4 plugin, from the same DLL: the interface header, the split of the Ashita plugin, the Windower host, and a probe plugin that settles on a live client what reading Windower's binaries could not.

## What Changes

- The Ashita plugin splits into a `Session` that holds everything Memoria does (recording, uploads, linking, live, imports, commands, status, the FFXiMain hooks) and a thin Ashita host that turns Ashita's callbacks into `Session` calls and gives it paths, chat, and a log. Ashita players see no change.
- `src/windower/interface.h`: Windower 4's plugin interface (version `0x40004`), recovered from Hook.dll 4.4.2.0 and its own plugins, each slot noted with its evidence. `docs/windower-interface.md` holds the trace, with addresses.
- A probe plugin, `MemoriaProbe.dll`, built from that header alone: it loads in Windower, logs every call and its arguments to a file, and prints to the chat and console.
- A Windower host in `Memoria.dll`, behind the exports `CreateInstance` and `GetInterfaceVersion`, which checks the host's version and shape before it starts a `Session`.

Out of scope: Windower 5, Lua addons, and captain on Windower.

The session split shipped on main. The interface, probe and host wait on a test on a live Windower client, in draft PR #5.

## Capabilities

### New Capabilities
- `windower-host`: how `Memoria.dll` runs under Windower 4, and how one `Session` serves both frameworks.

### Modified Capabilities

## Impact

- **Memoria:** `src/plugin/` (the session and the Ashita host), `src/windower/` (interface, host, probe), `docs/windower-interface.md`, `CMakeLists.txt` (a probe target), `build.yml` (the probe's artifact).
