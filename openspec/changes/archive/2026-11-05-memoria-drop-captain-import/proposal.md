# Proposal

## Why

Mogsafe's import page takes captain's PacketLogger logs straight from the browser. Memoria's `/memoria import` did the same job from inside the game, at the cost of a scanner, a compressor, a second tus client and its own state file. One way in is enough.

## What Changes

- `/memoria import`, its line in `/memoria status` and its login reminder go.
- Memoria no longer reads `addons/captain/`. `captures/memoria/imports.txt`, its staged logs under `captures/memoria/imports/` and `config/memoria/import-reminded` are left alone; players can delete them.
- `src/core/captain/` goes, with its tests. Memoria no longer links zstd itself; xicap still builds its own.
- `/memoria session` files, which captain's `/cap` uses, stay as they are.

## Capabilities

### New Capabilities

### Modified Capabilities
- `captain-import`: removed.
- `upload-resume`: `uploads.txt` is the only state file; its requirement is renamed for captures alone.

## Impact

- **Memoria:** `src/core/captain/` removed; `src/plugin/session.*`, `commands.cpp`, `ashita.cpp`, `src/core/install.*`, core tests, README.
- **Mogsafe:** none; its import page already takes captain logs.
