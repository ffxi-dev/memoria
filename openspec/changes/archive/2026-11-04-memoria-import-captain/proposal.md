# Proposal

## Why

Players who used captain before Memoria have old capture folders, each with a `manifest.txt` and a `PacketLogger/full.log`, that never reached Vault. Mogsafe's `/import` page takes them, but only if the player finds the page and drops the folders by hand. Memoria already runs in the client, knows where captain keeps its captures, and holds an upload token, so it can send them itself.

## What Changes

- Memoria finds captain capture folders not yet uploaded under `<Ashita>/addons/captain/captures/`.
- Once, at a login or link while the install is linked and such folders exist, Memoria says in chat: "Run /memoria import to upload existing Captain captures". It never says it again on that install.
- `/memoria import` uploads them to Mogsafe in the background as captain imports, with the install's token, so they belong to the linked account. Chat shows each capture's result; `/memoria status` shows progress.
- A folder whose captain also ran SearchLog sends its `SearchLog/search.log` ahead of `full.log` in the same upload, so the search server's traffic reaches Vault in the same capture. A folder Vault already holds without its search log gains it.
- The first release waits for the search log: no folder moves aside without it.
- An upload stopped by a crash, a restart, or a network failure resumes; one asked for and not finished resumes at the next start.
- Once Mogsafe holds a log, whether this import sent it or it was there already, Memoria moves its folder to `<Ashita>/addons/captain/captures-uploaded/`, which neither Memoria nor captain reads.
- A folder Mogsafe refuses stays where it is and is reported with Mogsafe's reason.

Out of scope: PacketViewer logs, captain folders outside `<Ashita>/addons/captain/captures/`, uploading the per-opcode logs or any file but `full.log`, `search.log`, and `manifest.txt`, a `search.log` in a folder with no `full.log`, and changes to Mogsafe or captain.

## Capabilities

### New Capabilities
- `captain-import`: Memoria finding a player's captain captures, reminding once, uploading them to Mogsafe, and moving uploaded folders aside.

### Modified Capabilities

## Impact

- **Memoria:** a captain importer in `src/core` (scan, CRC-32 and zstd staging, tus upload, move), driven from the plugin by `/memoria import`, the one-time reminder, and an import line in `/memoria status`. README, CHANGELOG, and AGENTS.md (which now points here for changes inside Memoria).
- **Mogsafe:** none here. Its upload route already takes `format=captain` uploads with an install token and records them under the linked account (`ingest.go` `created`, `import.go` `importMeta`); duplicates already answer `409` with `Mogsafe-Capture`. Vault's `import-search-traffic` adds the search log to a captain upload, and the replacement of a capture archived without one.
- **infra/:** none. Memoria uses the token `/memoria link` already gives it.
- **Captain:** none. It never reads `captures/` and does not load PacketLogger any more, so no new `full.log` appears there.
