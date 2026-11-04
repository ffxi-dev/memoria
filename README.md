# Memoria

Memoria is an [Ashita v4](https://ashitaxi.com) plugin for the retail Final Fantasy XI client. Once loaded, it records every packet of every session to an [xicap](https://github.com/ffxi-dev/xicap) capture under `<Ashita>/captures/memoria/`. When you link it to an account on [Mogsafe](https://mogsafe.sruon.dev), Vault's capture archive, it also uploads each capture as it records. There is nothing to start or stop.

## Install

Download `Memoria.dll` from the [releases](https://github.com/ffxi-dev/memoria/releases), put it in `<Ashita>/plugins/`, and run `/load memoria` in game, or add it to your Ashita boot script.

## Commands

| Command | What it does |
| --- | --- |
| `/memoria` or `/memoria status` | Shows what is recording, the client version, and how far uploads have got. |
| `/memoria link` | Links this install to your Mogsafe account. Memoria opens the link page and shows a code to enter there. |
| `/memoria unlink` | Stops uploading. Captures stay on this PC. |
| `/memoria upload off` and `on` | Pauses and resumes uploads. |
| `/memoria live` | Streams the capture to a web page as it records. `/memoria live off` ends it. |
| `/memoria import` | Uploads the captures captain's PacketLogger left behind. |
| `/memoria note <text>` | Adds a note to the capture at this moment. |
| `/memoria session start <file>` and `stop` | Marks a stretch of play. See below. |

Several clients can share one Ashita folder. They share its link and pause, and one of them uploads for all. While unlinked, Memoria reminds you once a day at login that your captures stay on this PC.

## Recording

A capture starts at the first login and runs until another character logs in or Memoria unloads; logging out and back in on the same character continues it. If Memoria loads while you are already in game, it records from there and marks the capture as starting mid-session. Search server traffic is recorded as well.

Chat text is zeroed in place, so chat packets keep their length. A linked account that turns on **Save chat** on Mogsafe's Memoria page keeps all chat instead, other players' included. A change applies from the next capture, and `/memoria status` says which applies. An unlinked install never saves chat.

`/memoria session start <file>` and `/memoria session stop` mark a stretch of play, as captain's `/cap` does. Linked, the session is a range of the recording on Mogsafe, and `stop` prints its link. Unlinked, Memoria writes the session to `<file>` as a second capture; it never replaces an existing file.

## Uploads

Memoria sends each capture to Mogsafe as it grows and resumes after a crash, a restart or a lost connection without sending a byte twice. It closes a capture's upload once the capture is complete. At unload it waits at most three seconds for the last bytes.

Mogsafe ends a recording that has sent nothing for five minutes. The part it never got goes up later as a capture of its own that names the first.

To use another Mogsafe server, put `server = https://...` in `<Ashita>/config/memoria/upload.ini`.

## Live

`/memoria live` needs a linked install. Mogsafe gives Memoria a link that anyone can open while it lasts, at most 12 hours, and Memoria opens it in your browser. Packets go out only while someone watches, and viewers see what the capture holds, so chat stays zeroed unless you save it. `/memoria live` again reopens the page. `/memoria live off`, `/memoria unlink`, `/memoria upload off` and unloading all end the link.

## Captain imports

`/memoria import` uploads the captures captain's PacketLogger left in `addons/captain/captures/` to Mogsafe under your account. Each folder that holds `manifest.txt` and `PacketLogger/full.log` is one capture. Memoria compresses and sends `full.log` with the manifest, and `SearchLog/search.log` when there is one; a capture already on Mogsafe without its search log gains it. A log changed in the last 10 minutes waits for a later import.

The import runs in the background: chat shows each capture's result, and `/memoria status` shows progress. Once Mogsafe holds a log, Memoria moves its folder to the same path under `addons/captain/captures-uploaded/`. A folder Mogsafe refuses stays where it is. An import cut short goes on by itself at the next start or link, and `/memoria upload off` pauses it.

While such captures wait, Memoria reminds you once per install, at a login or link.

## Build

The plugin needs 32-bit MSVC and the Ashita v4 SDK. From an MSVC x86 developer shell, with `ASHITA4_SDK_PATH` pointing at Ashita's `plugins/sdk`:

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DMEMORIA_OUTPUT_DIR=<Ashita>/plugins
cmake --build build --target memoria
```

The core (recording, uploads, linking, imports) has no Ashita code, and its tests build on Windows and Linux:

```
cmake -S . -B build -G Ninja -DMEMORIA_BUILD_PLUGIN=OFF -DMEMORIA_BUILD_TESTS=ON
cmake --build build --target memoria_core_tests && ctest --test-dir build
```

## Layout

- `src/core/`: the event queue, clock, recorder, uploader, linking, live feed and captain importer. No Ashita headers.
- `src/plugin/`: the Ashita plugin. `ashita.*` hands Ashita's callbacks to `session.*`; `commands.cpp` handles `/memoria`; `client.*` reads and hooks FFXiMain.dll; `live.*` and `winhttp.*` talk to Mogsafe.
- `tests/`: the core's tests, one file per module, with shared fakes in `support.h`.
- `openspec/`: what Memoria does, in `specs/`, and the reasoning behind each change, in `changes/`.
- `cmake/FindAshitaSDK.cmake`: the Ashita team's CMake helper, unchanged.

## Licence

Copyright (C) 2026 sruon. Memoria is free software under the GNU General Public License, version 3; see `LICENSE`.
`cmake/FindAshitaSDK.cmake` is the Ashita team's, under GPL version 3 or later. `THIRD_PARTY_NOTICES.md` lists the code from other projects that `Memoria.dll` includes, with their licences.
