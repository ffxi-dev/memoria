# Memoria repository rules

These rules apply to every contributor, human or agent. `CLAUDE.md` imports this file.

## Source of truth

`ffxi-dev/vault`'s `openspec/changes/add-memoria/` is the working specification. Code, specs, design, and tasks agree at every commit: a change in behaviour updates the spec, `design.md`, or `tasks.md` there in a matching vault commit. Check a task only when it is implemented and verified by a test or a recorded run.

## Layout

- `src/core/`: the event queue, clock, install key, and recording. No Ashita headers: it builds and is tested on Linux and Windows.
- `src/plugin/`: the Ashita v4 plugin, a thin layer that hands the game's events to the core. 32-bit Windows only.
- `cmake/FindAshitaSDK.cmake`: the Ashita team's helper, unchanged.

## Before every commit

```bash
cmake -S . -B build -G Ninja -DMEMORIA_BUILD_PLUGIN=OFF -DMEMORIA_BUILD_TESTS=ON
cmake --build build --target memoria_core_tests && ctest --test-dir build --output-on-failure
```

The plugin builds from an MSVC x86 developer shell with `ASHITA4_SDK_PATH` set. CI runs both on the org's runner lsb: `test.yml` through vault's shared `cmake.yml`, which also runs the core's tests as 32-bit Windows programs under Wine, and `build.yml`, which builds the DLL with clang-cl in vault's `vault-msvc` image. Code must build with both MSVC and clang-cl. Run clang-format (v22) on every C++ file you change. Commit in small steps; the message says why.

## Rules

- The game thread never waits: packet hooks only copy into the queue. Compression, writing, signing, flushing, and uploading run on Memoria's own threads.
- Captures follow xicap format 3's writer rules (`ffxi-dev/xicap` `FORMAT.md`), and are written only through xicap's library. Never write the format by hand here.
- The install key is never sent, printed, or replaced. Upload credentials are never printed or logged.
- `pilot` on the owner's PC runs five live clients. Test there only when the owner schedules it.

## Part of Vault

One brick of Vault, the owner's platform for capturing retail FFXI and keeping the captures. The platform repository is `ffxi-dev/vault`: its `infra/memoria.tf` owns the deploy key that lets builds fetch the private xicap. On the owner's PC the repositories sit side by side in `C:/Users/sruon/Documents/GitHub/Vault/`.

- **Private.** Never commit credentials, and never make the repository public without the owner.
- **Writing** (docs, commit messages, UI text) follows `ffxi-dev/vault`'s AGENTS.md: short plain words, active voice, no filler.
