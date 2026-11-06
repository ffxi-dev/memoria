# Memoria

Memoria is an [Ashita v4](https://ashitaxi.com) plugin for the retail Final Fantasy XI client. Once loaded, it records every packet of every session to an [xicap](https://github.com/ffxi-dev/xicap) capture under `<Ashita>/captures/memoria/`. When you link it to an account on [Mogsafe](https://mogsafe.sruon.dev), it also uploads each capture as it records. There is nothing to start or stop.

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
| `/memoria note <text>` | Adds a note to the capture at this moment. |
| `/memoria session start <file>` and `stop` | Marks a stretch of play. See below. |

Several clients can share one Ashita folder. They share its link and pause, and one of them uploads for all. While unlinked, Memoria reminds you once a day at login that your captures stay on this PC.

## Recording

A capture starts at the first login and runs until another character logs in or Memoria unloads; logging out and back in on the same character continues it. If Memoria loads while you are already in game, it records from there and marks the capture as starting mid-session. Search server traffic is recorded as well.

Chat text is zeroed in place, so chat packets keep their length. A linked account that turns on **Save chat** on Mogsafe's Memoria page keeps all chat instead, other players' included. A change applies from the next capture, and `/memoria status` says which applies. An unlinked install never saves chat.

`/memoria session start <file>` and `/memoria session stop` mark a stretch of play, as captain's `/cap` does. Linked, the session is a range of the recording on Mogsafe, and `stop` prints its link. Unlinked, Memoria writes the session to `<file>` as a second capture; it never replaces an existing file.

## Uploads

Memoria sends each capture to Mogsafe as it grows, and after a crash, a restart or a lost connection resumes from the bytes Mogsafe holds. When Mogsafe has cut a recording off, its tail goes up as a new capture that starts at a frame boundary, so a few packets may reach Mogsafe twice. It closes a capture's upload once the capture is complete. At unload it gives the last bytes three seconds, then waits for any request still in flight to finish or time out: seconds as a rule, and up to a minute when Mogsafe is slow to answer an upload request.

Mogsafe ends a recording that has sent nothing for five minutes. The part it never got goes up later as a capture of its own that names the first.

To use another Mogsafe server, put `server = https://...` in `<Ashita>/config/memoria/upload.ini`.

## Live

`/memoria live` needs a linked install. Mogsafe gives Memoria a link that anyone can open while it lasts, at most 12 hours, and Memoria opens it in your browser. Packets go out only while someone watches, and viewers see what the capture holds, so chat stays zeroed unless you save it. `/memoria live` again reopens the page. `/memoria live off`, `/memoria unlink` and `/memoria upload off` end the link; unloading leaves it to expire.

## Build

The plugin needs 32-bit MSVC and the Ashita v4 SDK. From an MSVC x86 developer shell, with `ASHITA4_SDK_PATH` pointing at Ashita's `plugins/sdk`:

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DMEMORIA_OUTPUT_DIR=<Ashita>/plugins
cmake --build build --target memoria
```

The core (recording, uploads, linking) has no Ashita code, and its tests build on Windows and Linux:

```
cmake -S . -B build -G Ninja -DMEMORIA_BUILD_PLUGIN=OFF -DMEMORIA_BUILD_TESTS=ON
cmake --build build --target memoria_core_tests && ctest --test-dir build
```

## Layout

- `src/core/`: everything that needs only the standard library and its dependencies, built and tested on Windows and Linux. Each folder is a namespace: `capture/` (the event queue, clock, recorder, and the login and logout packets), `upload/` (the uploader, its state file, and what it does when Mogsafe ends an upload), `account/` (linking, and the chat choice tied to the token that made it) and `live/` (the live feed). At the top sit what several of them use: `install.*` (the config folder that clients sharing an install read: the token, the pause, upload.ini), `worker.*` (the short jobs a packet hook must not wait on), `http.h`, `tus.*`, `file.*` and `units.*`.
- `src/plugin/`: the Ashita plugin. `ashita.*` hands Ashita's callbacks to `session.*`; `commands.cpp` handles `/memoria`; `client.*` reads and hooks FFXiMain.dll; `live.*` and `winhttp.*` talk to Mogsafe; `text.*` formats chat lines.
- `tests/`: one file per module, with shared fakes in `support.h`. They cover the core, and on Windows also the plugin's network and text code.
- `openspec/`: the specs written so far for what Memoria does, in `specs/`, and the reasoning behind each change, in `changes/`.
- `cmake/FindAshitaSDK.cmake`: the Ashita team's CMake helper, unchanged.

## Contributing

- Packet hooks never wait: they only copy into the queue. Compression, writing, flushing and uploading run on Memoria's own threads.
- Captures are written only through xicap's library, with its live preset.
- A function that can fail and has nothing else to return gives `std::optional<std::string>`: why not, or nothing on success.
- Upload credentials and the link handle are never printed or logged.
- A change that alters a capability in `openspec/specs/` updates that spec in the same commit, and a larger change records its reasoning under `openspec/changes/`.
- CI builds the plugin with MSVC, runs the tests on Linux and as 32-bit Windows programs, runs clang-tidy, and checks formatting with clang-format 23.

## Licence

Copyright (C) 2026 sruon. Memoria is free software under the GNU General Public License, version 3 only; see `LICENSE`.
`cmake/FindAshitaSDK.cmake` is the Ashita team's, under GPL version 3 or later. `THIRD_PARTY_NOTICES.md` lists the code from other projects that `Memoria.dll` includes, with their licences.
