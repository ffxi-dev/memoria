# Memoria

Memoria is an [Ashita v4](https://ashitaxi.com) plugin for the retail Final Fantasy XI client. It records every packet of every session, search server traffic included, to an [xicap](https://github.com/ffxi-dev/xicap) capture. Linked to an account on [Mogsafe](https://mogsafe.sruon.dev), it uploads each capture as it records, and resumes after a crash or a lost connection.

## Install

Download `Memoria.dll` from the [releases](https://github.com/ffxi-dev/memoria/releases), put it in `<Ashita>/plugins/`, and run `/load memoria` in game or add it to your Ashita boot script.

## Commands

| Command | What it does |
| --- | --- |
| `/memoria` or `/memoria status` | Shows the capture, the client version, and upload progress. |
| `/memoria link` | Links this install to your Mogsafe account. Memoria opens the link page and shows a code to enter there. |
| `/memoria unlink` | Stops uploading. Captures stay on this PC. |
| `/memoria upload off` and `on` | Pauses and resumes uploads. |
| `/memoria live` | Streams the capture to a web page anyone with the link can watch, for up to 12 hours. `/memoria live off` ends it. |
| `/memoria note <text>` | Adds a note to the capture. |
| `/memoria session start <file>` and `stop` | Marks a stretch of play, as captain's `/cap` does. Linked, `stop` prints its link on Mogsafe. Unlinked, Memoria writes it to `<file>`, which must not exist. |

A capture runs from the first login until another character logs in or Memoria unloads. Chat text is zeroed in place, in captures and on the live page, unless the linked account turns on **Save chat** on Mogsafe's Memoria page; `/memoria status` says which applies. Clients that share an Ashita folder share its link and pause, and one of them uploads for all.

## Files

In `<Ashita>/captures/memoria/`:

- `*.xicap`: the captures. A `-rest.xicap` holds the part of a recording Mogsafe cut off, and uploads as a capture of its own.
- `uploads.txt`: how far each capture's upload has got.
- `state-<character id>.txt`: whether the character's last capture closed, so the next one can name a capture a crash left open.

In `<Ashita>/config/memoria/`:

- `token`: the link to your Mogsafe account, readable only by your Windows user.
- `paused`: there while uploads are paused.
- `reminded`: the date Memoria last reminded you to link.
- `upload.ini`: optional. A `server = https://...` line points Memoria at another Mogsafe server.

## Build

The plugin needs 32-bit MSVC and the Ashita v4 SDK. From an MSVC x86 developer shell, with `ASHITA4_SDK_PATH` set to Ashita's `plugins/sdk`:

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DMEMORIA_OUTPUT_DIR=<Ashita>/plugins
cmake --build build --target memoria
```

The core in `src/core/` has no Ashita code, and it and its tests build on Windows and Linux:

```
cmake -S . -B build -G Ninja -DMEMORIA_BUILD_PLUGIN=OFF -DMEMORIA_BUILD_TESTS=ON
cmake --build build --target memoria_core_tests && ctest --test-dir build
```

## Contributing

- Packet hooks copy into the queue and return. Compression, writing and uploads run on Memoria's own threads.
- Write captures through xicap's library, with its live preset.
- A function that can fail and returns nothing else returns `std::optional<std::string>`: the error, or nothing on success.
- Never print or log the upload token or the link handle.
- A change to a capability in `openspec/specs/` updates that spec in the same commit. A larger change starts under `openspec/changes/`.
- CI builds the plugin with MSVC, runs the tests on Linux and as 32-bit Windows programs, runs clang-tidy, and checks formatting with clang-format 23.

## Licence

Copyright (C) 2026 sruon. Memoria is free software under the GNU General Public License, version 3 only; see `LICENSE`. `cmake/FindAshitaSDK.cmake` is the Ashita team's, under GPL version 3 or later. `THIRD_PARTY_NOTICES.md` lists the code from other projects in `Memoria.dll`, with their licences.
