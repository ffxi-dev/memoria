# Tasks

Vault's `add-memoria-windower` tasks.md holds the plan; each task here names the vault task it serves.

## 1. Session split (vault 2.1)

- [ ] 1.1 Move everything the Ashita plugin does into `Session` (`src/plugin/session.*`), with no Ashita header, and leave `src/plugin/ashita.*` as the host; verify the core tests and CI's plugin build pass
- [ ] 1.2 An Ashita retail run on the split reads as before in xicap_dump (owner's test session)

## 2. Interface (vault 1.1, 1.2, 1.4)

- [ ] 2.1 Trace `CreateInstance`, `GetInterfaceVersion`, Hook.dll's load path and version check, and the plugin and host vtables through LuaCore's events, with addresses, in `docs/windower-interface.md`; verify each address against the 2024-10-28 Hook.dll 4.4.2.0
- [ ] 2.2 `src/windower/interface.h` from the trace, each slot noted with its evidence and confidence; verify the probe builds against it alone

## 3. Probe (vault 1.3)

- [ ] 3.1 `MemoriaProbe.dll` reports `0x40004`, logs every call with its arguments and thread to `<Windower>/memoria-probe.log`, and prints once to the chat and console; verify CI builds it
- [ ] 3.2 Run it on the owner's Windower client (owner's test session)

## 4. Windower host (vault 2.2)

- [ ] 4.1 `CreateInstance` and `GetInterfaceVersion` in `Memoria.dll`; the host checks the version and shape, starts a `Session` with paths under `<Windower>/memoria/`, queues chat and commands for the frame, and hands packets to the session; verify CI builds it
- [ ] 4.2 Loads in Windower, prints to the chat, records, and refuses an older interface number cleanly (owner's test session)

## Owner test session

The steps live in `docs/windower-interface.md`, "Owner test session".
