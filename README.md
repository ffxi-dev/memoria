# Memoria

An Ashita v4 plugin for the retail FFXI client. It records every packet of every session to an xicap capture and uploads it to Vault's archive, Mogsafe, as it records. Nothing to start or stop: once loaded, it records.

It replaces the earlier Memoria plugin (`Ashita4-Memoria`) and captain's PacketLogger.

Memoria records every packet on retail, uploads it to Mogsafe as it goes, and streams it live to Lens. The plan is in `ffxi-dev/vault`, `openspec/changes/add-memoria/`.

## Live

`/memoria live` opens Lens on the capture as it records, for anyone with the link. It needs `/memoria link` first. Memoria asks Mogsafe for a link, opens it in your browser, and sends packets over a WebSocket only while someone watches. Viewers see what the capture holds, so chat stays stubbed. `/memoria live` again reopens the link; `/memoria live off` ends it, as do `/memoria unlink`, `/memoria upload off`, and unloading. A link lasts at most 12 hours. The requirements are in `ffxi-dev/vault`, `openspec/specs/live-viewing/`, and the protocol in `openspec/changes/archive/2026-10-31-add-memoria-live/design.md`.

## Modes

Memoria records in one of three modes. A linked install follows the mode its account chose on Mogsafe's Memoria page (`/link`), which imports follow too; it asks Mogsafe at load, at each login and when it links, and a change applies from the next capture. An unlinked install keeps its own, set with `/memoria mode default|everything`. `/memoria status` shows the mode.

- `default` cuts chat, tells and linkshell board posts, with xicap's default exclusion policy.
- `everything` cuts nothing: captures keep all chat, other players' included. Mogsafe keeps such a capture only from an account in everything mode.
- `anonymous` replaces names and player ids with pseudonyms made with the account's key. It is not built yet: Memoria refuses it and records as default.

## Captain imports

`/memoria import` uploads the captures captain's PacketLogger left in `addons/captain/captures/`, any folder there holding `manifest.txt` and `PacketLogger/full.log`, to Mogsafe under the linked account. It needs `/memoria link` first, and runs in the background: chat shows each capture's result, and `/memoria status` shows progress. It sends only `full.log` and `manifest.txt`, and `SearchLog/search.log` when captain's SearchLog wrote one, compressed. A folder Vault holds without its search log gains it under the same capture. A log changed in the last 10 minutes waits for a later import. Once Mogsafe holds a log, whether this import sent it or it was there already, Memoria moves its folder to the same path under `addons/captain/captures-uploaded/`. A folder Mogsafe refuses stays where it is. An import cut short by a crash, a restart, or a lost connection goes on by itself at the next start or link; `/memoria upload off` pauses it.

While linked and captures wait, Memoria says once, at a login or link, to run `/memoria import`. It never says it again on that install.

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
