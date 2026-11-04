# Memoria repository rules

These rules apply to every contributor, human or agent.

## Source of truth

A change starts as an OpenSpec change in `openspec/`. Code, specs, design, and tasks agree at every commit: a change in behaviour updates its spec, `design.md`, or `tasks.md` in the same commit. Check a task only when it is implemented and verified by a test or a recorded run.

## Layout

- `src/core/`: the event queue, clock, recorder, uploader (`uploader.*`, with `uploads.txt` in `upload_state.*`), linking, live feed, and captain importer (`captain.*` finds folders, `importer.*` uploads them). No Ashita headers: it builds and is tested on Linux and Windows.
- `src/plugin/`: `session.*`, everything Memoria does in the client, with no framework header; `commands.cpp`, the `/memoria` commands and status; `packets.*` and `client.*`, what Memoria reads from packets and from FFXiMain.dll; and `ashita.*`, the Ashita v4 host that hands Ashita's callbacks to the session. 32-bit Windows only.
- `tests/`: one file per core module, with shared fakes in `support.h`.
- `cmake/FindAshitaSDK.cmake`: the Ashita team's helper, unchanged.

## Before every commit

```bash
cmake -S . -B build -G Ninja -DMEMORIA_BUILD_PLUGIN=OFF -DMEMORIA_BUILD_TESTS=ON
cmake --build build --target memoria_core_tests && ctest --test-dir build --output-on-failure
```

The plugin builds from an MSVC x86 developer shell with `ASHITA4_SDK_PATH` set. CI runs both on GitHub's runners: `test.yml` runs the core's tests on Linux and as 32-bit Windows programs, and `build.yml` builds the DLL with MSVC. Code must build with MSVC. Run clang-format (v23, as CI does) on every C++ file you change. Commit in small steps; the message says why.

## Rules

- The game thread never waits: packet hooks only copy into the queue. Compression, writing, flushing, and uploading run on Memoria's own threads.
- Captures follow xicap format 5's writer rules (`ffxi-dev/xicap` `FORMAT.md`) with the library's live preset, and are written only through xicap's library. Never write the format by hand here.
- Upload credentials and the link handle are never printed or logged.
