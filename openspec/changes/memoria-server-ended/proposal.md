# Proposal

## Why

Players kill game clients all the time. Mogsafe kept a dead client's upload "recording" for 24 hours, and its `/cap` sessions with it. The owner decided on 2026-11-03 that Mogsafe ends a recording after 5 silent minutes and Memoria starts a new one instead (vault's `end-silent-recordings`). Memoria answered an ended upload by sending the whole file again as a new capture, which would now duplicate every packet Mogsafe already kept.

## What Changes

- Mogsafe answers every request for an ended upload with `410` and `Mogsafe-Ended` (`complete`, `cut_off`, or `refused`). Memoria never sends that file again. `uploads.txt` marks it `ended` in place of the closed flag and drops its URL, so a restart remembers. The local file stays.
- When the ended file is the one still recording (the client lived through a network gap), the recorder closes it and starts a new capture at the next packet, marked mid-session and naming the old one as the capture it continues. The player sees once: "The server ended the last recording; continuing in a new one." A `/memoria session` file goes on across the change.
- `404` now means Mogsafe never kept the upload, so sending the whole file again duplicates nothing, as before. A close that fails to reach Mogsafe no longer drops the upload.
- `410` with `Mogsafe-Ended: complete` after a lost close answer counts as closed.

## Capabilities

### New Capabilities
- `ended-uploads`: what Memoria does when Mogsafe ends an upload.

### Modified Capabilities

## Impact

- **Memoria:** `src/core/uploader.*`, `src/core/recorder.*`, `src/plugin/session.cpp` (shared by the Ashita and Windower hosts), core tests.
- **Mogsafe:** vault's `end-silent-recordings`. What a client recorded after the last byte Mogsafe kept stays only in the old local file.
