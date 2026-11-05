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

Memoria sends each capture to Mogsafe as it grows, and after a crash, a restart or a lost connection resumes from the bytes Mogsafe holds. When Mogsafe has cut a recording off, its tail goes up as a new capture that starts at a frame boundary, so a few packets may reach Vault twice. It closes a capture's upload once the capture is complete. At unload it gives the last bytes three seconds, then waits for any request still in flight to finish or time out: seconds as a rule, and up to a minute only when Mogsafe is slow to answer a close.

Mogsafe ends a recording that has sent nothing for five minutes. The part it never got goes up later as a capture of its own that names the first.

To use another Mogsafe server, put `server = https://...` in `<Ashita>/config/memoria/upload.ini`.

## Live

`/memoria live` needs a linked install. Mogsafe gives Memoria a link that anyone can open while it lasts, at most 12 hours, and Memoria opens it in your browser. Packets go out only while someone watches, and viewers see what the capture holds, so chat stays zeroed unless you save it. `/memoria live` again reopens the page. `/memoria live off`, `/memoria unlink` and `/memoria upload off` end the link; unloading leaves it to expire.

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

- `src/core/`: everything without Ashita code, built and tested on Windows and Linux. Each folder is a namespace: `capture/` (the event queue, clock and recorder), `upload/` (the uploader and its state file), `account/` (linking, and the chat choice tied to the token that made it), `live/` (the live feed) and `captain/` (captain imports). `http.h`, `tus.*`, `file.*` and `units.*`, which several of them use, sit at the top.
- `src/plugin/`: the Ashita plugin. `ashita.*` hands Ashita's callbacks to `session.*`; `commands.cpp` handles `/memoria`; `client.*` reads and hooks FFXiMain.dll; `live.*` and `winhttp.*` talk to Mogsafe; `worker.*` runs the short jobs a packet hook must not wait on.
- `tests/`: the core's tests, one file per module, with shared fakes in `support.h`.
- `openspec/`: what Memoria does, in `specs/`, and the reasoning behind each change, in `changes/`.
- `cmake/FindAshitaSDK.cmake`: the Ashita team's CMake helper, unchanged.

## Contributing

- Packet hooks never wait: they only copy into the queue. Compression, writing, flushing and uploading run on Memoria's own threads.
- Captures are written only through xicap's library, with its live preset.
- Upload credentials and the link handle are never printed or logged.
- A change that alters a capability in `openspec/specs/` updates that spec in the same commit, and a larger change records its reasoning under `openspec/changes/`.
- CI builds the plugin with MSVC, runs the core's tests on Linux and as 32-bit Windows programs, and checks formatting with clang-format 23.

## Licence

Copyright (C) 2026 sruon. Memoria is free software under the GNU General Public License, version 3 only; see `LICENSE`.
`cmake/FindAshitaSDK.cmake` is the Ashita team's, under GPL version 3 or later. `THIRD_PARTY_NOTICES.md` lists the code from other projects that `Memoria.dll` includes, with their licences.
