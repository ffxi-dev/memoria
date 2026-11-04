# Proposal

## Why

Players kill game clients all the time. Mogsafe kept a dead client's upload "recording" for 24 hours, and its `/cap` sessions with it. Mogsafe now ends a recording after 5 silent minutes, and Memoria starts a new one instead. Memoria answered an ended upload by sending the whole file again as a new capture, which would now duplicate every packet Mogsafe already kept.

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

- **Memoria:** `src/core/uploader.*`, `src/core/recorder.*`, `src/plugin/session.cpp`, core tests.
- **Mogsafe:** ends silent recordings. What a client recorded after the last byte Mogsafe kept stayed only in the old local file, until `memoria-cut-off-tail` sent it as a capture of its own.
