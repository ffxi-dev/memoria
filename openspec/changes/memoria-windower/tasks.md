# Tasks

The numbers in brackets name the steps of the Windower plan each task serves.

## 1. Session split (plan 2.1)

- [x] 1.1 Move everything the Ashita plugin does into `Session` (`src/plugin/session.*`), with no Ashita header, and leave `src/plugin/ashita.*` as the host; verify the core tests and CI's plugin build pass
- [ ] 1.2 An Ashita retail run on the split reads as before in xicap_dump (test session on a live client)

## 2. Interface (plan 1.1, 1.2, 1.4)

- [x] 2.1 Trace `CreateInstance`, `GetInterfaceVersion`, Hook.dll's load path and version check, and the plugin and host vtables through LuaCore's events, with addresses, in `docs/windower-interface.md`; verify each address against the 2024-10-28 Hook.dll 4.4.2.0
- [x] 2.2 `src/windower/interface.h` from the trace, each slot noted with its evidence and confidence; verify the host builds against it

## 3. Windower host (plan 2.2)

- [x] 3.1 `CreateInstance` and `GetInterfaceVersion` in `Memoria.dll`; the host checks the version and shape, starts a `Session` with paths under `<Windower>/memoria/`, queues chat and commands for the frame, and hands packets to the session; verify CI builds it
- [ ] 3.2 Loads in Windower, prints to the chat, records, and unloads with the game still running (test session on a live client)

## Test session

The steps are in the description of the `windower` branch's draft PR. Checked so far: Memoria builds with MSVC, the core tests pass, and each of the host's 21 slots pops the same bytes as Windower's own `PluginBase`.
