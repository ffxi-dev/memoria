# Design

## Context

- Captain writes each capture to `<Ashita>/addons/captain/captures/<session>/<character>/`: `manifest.txt` from `/cap start` and, from its PacketLogger, `PacketLogger/full.log` plus per-opcode logs. Since captain bundled Memoria, PacketLogger sits in `addons/extras/`, which captain does not load, so new sessions hold `memoria.xicap` and no `full.log`. Captain never lists `captures/`; its addons write their own data under `captures/<addon>/`.
- Mogsafe's `/v1/uploads/` already takes captain imports from an install token. `bearer` turns the token into `clerk:<user id>` and the install's key; `created` checks the import's metadata (`importMeta`) and answers `409` with `Mogsafe-Capture` when a log with the same CRC-32 and size is converting or complete; the signing-key check in `accept` runs only for xicap closes, and conversion signs with Mogsafe's own key. A log may be plain, raw deflate (`encoding=deflate`), or zstd, which Mogsafe spots by its magic. With `crc32` and `size` set, the close needs no hash and conversion checks the plain log against them. An import idle for 24 hours is deleted.
- Memoria's `Uploader` (`src/core/uploader.cpp`) already speaks this tus dialect for `.xicap` files: 1 MiB appends, the upload URL saved before the first byte, token and pause files read before each chunk, `401` deletes the token. Clients sharing an install share `config/memoria/` and `captures/memoria/`, and a named mutex picks the one that uploads.

## Goals / Non-Goals

**Goals:** one reminder per install, never more; no byte of a log Mogsafe already holds sent twice; a crash at any point ends, at the next import, in the same place as no crash.

**Non-Goals:** a parser or converter for `full.log` in Memoria (Mogsafe converts); checking a manifest before Mogsafe does; deleting anything.

## Decisions

### Mogsafe needs no change

The proposal's import is a captain upload like the `/import` page's, sent with an install token instead of a Clerk session. Both upload as `clerk:<user id>`. Rejected: a Memoria-only route or token on Mogsafe, which would duplicate what exists. A recorded run against Mogsafe (task 4.2) confirms this before release.

### Finding captures

A worker thread walks `<Ashita>/addons/captain/captures/` recursively and takes each folder holding `manifest.txt` and `PacketLogger/full.log`, as Mogsafe's page does in a zip; it does not descend into a match. The character is the folder's name; `folder` metadata is the path relative to `captures/`, with forward slashes. A `full.log` written to in the last 10 minutes is skipped: a player who loads PacketLogger by hand may still be writing it. Folders go in path order.

### What marks a capture as done

The folder's place on disk is the only record of done: under `captures/` it waits, under `captures-uploaded/` it is done. Nothing else needs to survive a crash for correctness, because Mogsafe's CRC-32 and size check tells Memoria, before it sends a byte, that a log is already there.

To resume without resending, `<Ashita>/captures/memoria/imports.txt` holds the run: its existence means an import was asked for and not finished, and each line holds a folder, its log's CRC-32 and size, and its upload URL, written before the first byte, as `uploads.txt` does. The file is replaced by write-then-rename and deleted when a run ends. The `Uploader` reads only `*.xicap` there, so neither this file nor the staging folder below disturbs it.

A crash at each point:

| Crash after | Next import |
|---|---|
| CRC pass, before create | CRC pass again |
| create, before first byte | URL in `imports.txt`; `HEAD` gives offset 0 |
| some appends | resumes from `HEAD`'s offset |
| close accepted, before move | `HEAD` and close again answer 200, or `HEAD` 404 and a new create gets `409`; either way the folder moves, no bytes sent |
| move | nothing left to do |

### Upload steps per capture

1. Read `full.log` once for its CRC-32 (IEEE) and size; read `manifest.txt`, a few hundred bytes.
2. `POST /v1/uploads/` with `Upload-Defer-Length: 1` and `Upload-Metadata`: `format captain`, `manifest`, `character`, `crc32` (8 hex digits), `size`, `folder`. No `zip`. `201` saves the URL; `409` with `Mogsafe-Capture` is already in Vault; any other `4xx` but `401` is a refusal with Mogsafe's text.
3. Compress `full.log` with zstd (level 3, xicap's zstd, one thread) to `<Ashita>/captures/memoria/imports/<crc32>-<size>.zst`, written to a `.tmp` name and renamed when whole, so a crash leaves either a whole file or none.
4. Send the staged file from `HEAD`'s offset in 1 MiB appends. `HEAD` `404` or `410`, or append `410`: forget the URL and create again (step 2), keeping the staged file. Append `422`: refusal.
5. `POST <url>/close` with `{}`. `200`: done. `4xx` but `409`, `423`: refusal. `409`, `423`, `5xx`, and transport failures: retry.
6. Delete the staged file and move the folder.

Retries back off from 5 s, doubling to 5 minutes, as the `Uploader` does, and never give up while the run lasts. Rejected: sending the plain log, several times the bytes of hex text; recompressing on resume and skipping bytes as the page does with deflate, since zstd's output is not promised to match across versions and a Memoria update between two runs would send a log that fails Mogsafe's CRC check. Staging costs disk space of a fraction of the log, for one capture at a time.

### Moving aside

`std::filesystem::rename` of the character folder to the same relative path under `<Ashita>/addons/captain/captures-uploaded/`, after creating its parents; `-2`, `-3` and so on are added if the target exists. Then each parent left empty up to `captures/` is removed. Rename within one volume is atomic on NTFS. A failed rename (a file held open) is reported and left; the next import moves it after Mogsafe's `409`. Rejected: a folder inside `captures/` such as `captures/.uploaded`, which a player dropping `captures/` on Mogsafe's page would send again as 409s, and which sits where captain's addons create folders.

### The reminder

At a login (the zone-in that calls `remind()`) and when a link arrives, if the install is linked and `config/memoria/import-reminded` does not exist, a short-lived thread scans. If it finds a capture waiting, it creates `import-reminded` with `O_EXCL` and, only if that create succeeded, posts the line through `post()`. Clients logging in together race for the create, and one wins. `/memoria import` creates the same file. Nothing removes it, so the reminder comes once per install, ever; a scan that finds nothing writes nothing.

### Threads and the game

- `/memoria import` on the game thread only checks the link and starts the run; scanning, hashing, compressing, sending, and moving run on an importer thread with its own `WinHttp`, at below-normal priority so compression does not take frames from the game.
- Messages go through `post()`, printed on the next frame. Progress is read from atomics.
- `Release()` cancels the importer's request and joins it, as for the `Uploader`; compression checks a stop flag between blocks.
- The token and pause files are read before each step and each chunk, sharing the `Uploader`'s code for that and for `401`, which deletes the token and unlinks the install.

### Clients sharing an install

A named mutex `Local\Memoria-import-<hash of the captures path>` is taken for the run. A client that cannot take it says another client is importing. At load, and when a link arrives, a client that finds `imports.txt` and can take the mutex resumes the run.

### Chat

- Start: "Uploading 4 captain captures (3.1 GB) in the background."
- Each capture: "Captain 2/4 2026-8-4_23_54/Audithor: uploaded." or "already in Vault (<capture id>).", "refused: <reason>.", "still being written; skipped.", "in Vault, but its folder could not move: <error>."
- End: "Captain import done: 2 uploaded, 1 already in Vault, 1 refused. Uploaded folders are in addons/captain/captures-uploaded."
- `/memoria status` adds `Import: 2/4 Audithor, 340/900 MB sent` while a run lasts, and `/memoria import` during a run prints the same line.

## Risks / Trade-offs

- [Staging fills a nearly full disk] → one capture staged at a time; a write failure ends the run with the error and deletes the partial file.
- [Compression slows the game on a weak PC] → below-normal thread priority, one zstd thread.
- [Two logs share a CRC-32 and size] → Mogsafe's own rule; the second would be reported as already in Vault and moved, not lost.
- [A refused folder is retried on every import] → it costs one CRC pass and one request; it never makes a run resume by itself, since the run ends after trying every folder.
- [Captain lives elsewhere than `addons/captain`] → not found; see Open Questions.

## Migration Plan

Ship in a Memoria release; nothing on the server changes. Rolling back to an older Memoria leaves `imports.txt` and staged files unused and moved folders where they are; the abandoned upload is deleted by Mogsafe's sweep after 24 hours.

## Open Questions

None. The owner settled them on 2026-11-02:
- `/memoria import` scans only `<Ashita>/addons/captain/captures/`; old installs go through Mogsafe's `/import` page.
- Uploaded folders move to `addons/captain/captures-uploaded/`.
- Memoria's own changes live in its `openspec/` (task 4.1).

## Implementation Notes

Where the code differs from the decisions above, and why:

- **The import mutex is taken on the importer's thread, not the game thread.** A Windows mutex belongs to the thread that took it: one taken by the game thread stays held after Memoria unloads, since the game thread lives on, and only that thread can release it. The importer's thread takes it, releases it when the run ends, and Windows hands it on if the client dies. So "another client is importing" reaches chat on the next frame, and a resume at load or link says nothing when another client holds the run. A core test covers it on Windows.
- **With a saved upload URL, `HEAD` comes before staging.** A log whose close was accepted before a crash then meets `404` and a `409` without being compressed again. If the staged file is gone while Mogsafe holds some of its bytes, the importer starts a new upload instead of compressing again, since another compression may not match those bytes.
- **Unlinked means the token file is gone, not "not allowed".** Reading the paused file and then the token raced with `/memoria upload on`, which ended a paused import as unlinked; a test caught it.
- **Backoff** also covers `HEAD` `404`/`410`, and starts again from 5 s after an append that sent bytes.
- **The reminder counts only logs not changed in the last 10 minutes**, so it never points at a capture the import would skip.
- **A `full.log` that cannot be read** is reported ("cannot read its full.log; skipped") and the run goes on. A staging failure ends the run with its error, without a summary.
- **The failed move test** puts a file where `captures-uploaded` goes, so the move fails on any file system, Wine included; a file held open in the folder takes the same report path but is not tested.
- The plugin's byte formatter moved to the core as `sizeText`, for the import's chat lines.

Recorded runs (tasks 3.x, 4.2) are still to do; capture ids go here.
