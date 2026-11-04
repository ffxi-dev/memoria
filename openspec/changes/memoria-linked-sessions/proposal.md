# Proposal

## Why

The owner decided on 2026-11-04 that a `/cap` session is a named time range of the full recording: Mogsafe finds it from captain's `cap start` and `cap stop` notes, shows it as the recording's page on that range, and no longer cuts a file for it. A linked install's second `.xicap` of the same packets then duplicates what Vault holds. Players who use captain without Vault still need the file.

## What Changes

- `/memoria session start <file>` writes the session file only when the install is not linked. Linked, it writes nothing and says the session is in Vault.
- `/memoria session stop` on a linked install prints the recording's Mogsafe link, `<server>/captures/<id>`, where `<id>` ends the recording's upload URL. Before the recording has an upload it says the session reaches Vault once the recording uploads.
- `Uploader::recordingId` gives that id, from memory in the client that sends and from `uploads.txt` in the others.

## Capabilities

### New Capabilities
- `session-files`: a `/cap` session writes a local file only when unlinked.

### Modified Capabilities

## Impact

- **Memoria:** `src/core/uploader.*`, `src/plugin/session.*`, core tests.
- **Mogsafe:** none; a range parameter on the capture page, once it has one, can join the link.
