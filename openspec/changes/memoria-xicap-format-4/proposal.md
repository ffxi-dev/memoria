# Proposal

## Why

xicap format 4 stores a capture in about half the space of format 3, and its signatures cover the packets, so Mogsafe can repack a capture at archive size and keep Memoria's signature (vault's `add-xicap-format-4`). Theater, Lens, Scholar, and Mogsafe read format 4 since 2026-11-03, so Memoria can write it.

## What Changes

- xicap's pin moves from v3.5.0 to v4.0.0. Memoria writes format 4 with xicap's live preset: zstd level 7, a 2 MiB window, and a 2^19 match table, about 5.5 MB of compressor per open writer. Nothing else in Memoria's code changes: the writer still puts each frame on disk, decodable, when it ends it, so the uploader sends the file as it grows.
- A `/memoria session` file is a second writer with the same preset, about 11 MB for both. If the owner's measurement in the game shows that too much, session files move to level 3 (about 3.7 MB) and the main recording stays at 7 (vault decision 11).
- Release 0.6.0-rc.3, a pre-release for the owner's captain test branch.

## Capabilities

### New Capabilities
- `capture-writing`: the xicap format and preset Memoria writes.

### Modified Capabilities

## Impact

- **Memoria:** `CMakeLists.txt` (pin, version), `AGENTS.md` (format 4's writer rules).
- **Mogsafe:** archives Memoria's format-4 uploads sealed at level 19; an older Memoria's format-3 uploads are re-signed.
