# Memoria

Memoria is a plugin for the retail Final Fantasy XI client. It runs in [Ashita v4](https://github.com/AshitaXI/Ashita-v4beta) and in [Windower 4](https://www.windower.net) 4.7.9.3. It records every packet of every session, search server traffic included, to an [xicap](https://github.com/ffxi-dev/xicap) capture. Linked to an account on [Mogsafe](https://mogsafe.sruon.dev), it uploads each capture as it records, and resumes after a crash or a lost connection.

## Install

Download `Memoria.dll` from the [releases](https://github.com/ffxi-dev/memoria/releases). The same file works in both:

- Ashita: put it in `<Ashita>/plugins/`, and run `/load memoria` in game or add it to your boot script.
- Windower: put it in `<Windower>/plugins/`, and run `load memoria` in the console or add it to `scripts/init.txt`.

## Commands

In Windower, type `//memoria` in chat or `memoria` in the console.

| Command | What it does |
| --- | --- |
| `/memoria` or `/memoria status` | Shows the capture, the client version, and upload progress. |
| `/memoria link` | Links this install to your Mogsafe account. Memoria opens the link page and shows a code to enter there. |
| `/memoria unlink` | Stops uploading. Captures stay on this PC. |
| `/memoria upload off` and `on` | Pauses and resumes uploads. |
| `/memoria live` | Streams the capture to a web page anyone with the link can watch, for up to 12 hours. `/memoria live off` ends it. |
| `/memoria note <text>` | Adds a note to the capture. |
| `/memoria cap start [<name>] [<folder>]` and `/memoria cap stop` | Marks a capture, as captain's `/cap` does, one at a time. With no name it is `cap_<UTC yyyymmdd-hhmm>`. Memoria copies it to `memoria.xicap` in `<folder>`, or else to its `caps` folder. Linked, chat gives the capture's Mogsafe link at start and at stop. A logout or another character ends it. |

A capture runs from the first login until another character logs in or Memoria unloads. Chat text is zeroed in place, in captures and on the live page, unless the linked account turns on **Save chat** on Mogsafe's Memoria page. `/memoria status` shows which applies. Clients that share an Ashita or Windower folder share its link and pause, and one of them uploads for all.

## Files

In `<Ashita>/captures/memoria/`, or `<Windower>/memoria/captures/`:

- `*.xicap`: the captures. A `-rest.xicap` holds the part of a recording Mogsafe cut off, and uploads as a capture of its own.
- `uploads.txt`: how far each capture's upload has got.
- `state-<character id>.txt`: whether the character's last capture closed, so the next one can name a capture a crash left open.

In `<Ashita>/captures/memoria/caps/`, or `<Windower>/memoria/caps/`: each `/memoria cap` started with no folder, as `<name>_<character>_<UTC yyyymmdd-hhmm>.xicap`. Memoria never uploads these.

In `<Ashita>/config/memoria/`, or `<Windower>/memoria/config/`:

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

Copyright (C) 2026 sruon. Memoria is free software under the GNU General Public License, version 3 only (see `LICENSE`). `cmake/FindAshitaSDK.cmake` is the Ashita team's, under GPL version 3 or later. `THIRD_PARTY_NOTICES.md` lists the code from other projects in `Memoria.dll`, with their licences.
