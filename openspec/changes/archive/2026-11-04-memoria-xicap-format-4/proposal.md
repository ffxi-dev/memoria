# Proposal

## Why

xicap format 4 stored a capture in about half the space of format 3, so Memoria moved to it in 0.6.0-rc.3. Format 5 (xicap 7) later replaced it in 0.6.0-rc.4: the same framing and compression, without signatures, since Vault knows who sent a capture from the install's upload token. The spec below states the current rule.

## What Changes

- Memoria writes with xicap's live preset: zstd level 7, a 2 MiB window, and a 2^19 match table, about 5.5 MB of compressor per open writer. The writer puts each frame on disk, decodable, when it ends it, so the uploader sends the file as it grows.
- A `/memoria session` file is a second writer with the same preset, about 11 MB for both. If that proves too much for the game's memory, session files move to level 3 (about 3.7 MB) and the main recording stays at 7.

## Capabilities

### New Capabilities
- `capture-writing`: the xicap format and preset Memoria writes.

### Modified Capabilities

## Impact

- **Memoria:** `CMakeLists.txt` (the xicap pin and version), `AGENTS.md` (the format's writer rules).
- **Mogsafe:** reads the format Memoria writes.
