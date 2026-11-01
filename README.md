# Memoria

An Ashita v4 plugin for the retail FFXI client. It records every packet of every session to an xicap capture and uploads it to Vault's archive, Mogsafe, as it records. Nothing to start or stop: once loaded, it records.

It replaces the earlier Memoria plugin (`Ashita4-Memoria`) and captain's PacketLogger.

Memoria records every packet on retail, uploads it to Mogsafe as it goes, and streams it live to Lens. The plan is in `ffxi-dev/vault`, `openspec/changes/add-memoria/`.

## Live

`/memoria live` opens Lens on the capture as it records, for anyone with the link. It needs `/memoria link` first. Memoria asks Mogsafe for a link, opens it in your browser, and sends packets over a WebSocket only while someone watches. Viewers see what the capture holds, so chat stays stubbed. `/memoria live` again reopens the link; `/memoria live off` ends it, as do `/memoria unlink`, `/memoria upload off`, and unloading. A link lasts at most 12 hours. The requirements are in `ffxi-dev/vault`, `openspec/specs/live-viewing/`, and the protocol in `openspec/changes/archive/2026-10-31-add-memoria-live/design.md`.

## Build

From an MSVC x86 developer shell, with `ASHITA4_SDK_PATH` pointing at Ashita's `plugins/sdk`:

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DMEMORIA_OUTPUT_DIR=<Ashita>/plugins
cmake --build build --target memoria
```

Then `/load memoria` in game.

CI builds the same DLL on Linux with clang-cl, in the `vault-msvc` image from `ffxi-dev/vault`'s `msvc/` folder.

The core's tests build anywhere:

```
cmake -S . -B build -G Ninja -DMEMORIA_BUILD_PLUGIN=OFF -DMEMORIA_BUILD_TESTS=ON
cmake --build build --target memoria_core_tests && ctest --test-dir build
```
