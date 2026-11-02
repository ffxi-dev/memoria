# Tasks

## 1. Finding captures

- [ ] 1.1 In `src/core`, find captain captures under a given root: folders at any depth holding `manifest.txt` and `PacketLogger/full.log`, not descending into a match, skipping a `full.log` changed in the last 10 minutes, in path order; verify with core tests for the nested, session-slice-only, growing-log, and per-addon-folder cases
- [ ] 1.2 Add CRC-32 (IEEE) over a file with a stop flag; verify a core test matches the CRC-32 and size Mogsafe's `browse.html` computes for the same test log

## 2. Uploading and moving

- [ ] 2.1 Move the token and pause checks, the `401` handling, and the chunked append out of `Uploader` into code the importer shares; verify the existing uploader tests still pass
- [ ] 2.2 Importer: create with captain metadata, compress to the staged `.zst` through `.tmp` and rename, append from `HEAD`'s offset, close with `{}`, delete the staged file, and keep `imports.txt` as design.md says; verify core tests against the fake `Http` for a clean upload, a `409` with `Mogsafe-Capture`, a `400` refusal, a `422` first append, `HEAD` `404` and append `410` recreating, `401` deleting the token, a pause between chunks, and backoff on `5xx`
- [ ] 2.3 Move an uploaded folder to `captures-uploaded/` at the same relative path, adding `-2` on a clash and removing emptied parents; verify core tests for the move, the clash, and a failed rename leaving the folder and reporting it
- [ ] 2.4 Crash recovery: verify core tests that start a new importer on the files left after each row of design.md's crash table and end with the folder moved and no log byte sent twice
- [ ] 2.5 Stop promptly: verify a core test that destroys the importer mid-append and mid-compression and returns within a second with the fake `Http` blocking

## 3. Plugin

- [ ] 3.1 `/memoria import`: refuse when unlinked, take the named import mutex or say another client imports, create `import-reminded`, start the importer thread at below-normal priority, and print a running import's progress when asked again; verify by a recorded run on the owner's test client (not Pilot) showing the start line, one result per capture, and the summary
- [ ] 3.2 Resume at load and when a link arrives if `imports.txt` exists and the mutex is free; verify by a recorded run that closes the game mid-upload and sees the upload continue from Mogsafe's offset after restart
- [ ] 3.3 The reminder: at login and link, while linked and `import-reminded` is absent, scan on a short-lived thread and post the line only if the `O_EXCL` create wins; verify a core test for the once-only create and a recorded run showing the line once across two logins and a restart
- [ ] 3.4 `/memoria status` shows the import line while a run lasts; verify in the recorded run of 3.1
- [ ] 3.5 Game thread never waits: verify by a recorded run importing a log of at least 1 GB while playing, where `/memoria status` shows no dropped packets and unloading mid-import returns at once
- [ ] 3.6 Build with MSVC and clang-cl (CI's `build.yml`) and run clang-format on changed files; verify CI passes

## 4. Docs and check against Mogsafe

- [ ] 4.1 README: `/memoria import`, the reminder, and where uploaded folders go; CHANGELOG entry; AGENTS.md says changes inside Memoria start in its own `openspec/`; verify the README's commands match the plugin's help line
- [ ] 4.2 Recorded run against `mogsafe.sruon.dev` with a linked install: one new captain folder converts to a capture owned by the linked account, and a folder imported earlier through `/import` comes back as already in Vault and moves; record the capture ids in design.md's Implementation Notes

## Workflow follow-up

- Archive the change once every task is checked, and sync `captain-import` into `openspec/specs/`.
