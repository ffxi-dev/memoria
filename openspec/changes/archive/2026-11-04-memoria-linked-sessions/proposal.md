# Proposal

## Why

A `/cap` session is a named time range of the full recording: Mogsafe finds it from captain's `cap start` and `cap stop` notes, shows it as the recording's page on that range, and no longer cuts a file for it. A linked install's second `.xicap` of the same packets then duplicates what Vault holds. Players who use captain without Vault still need the file.

## What Changes

- `/memoria session start <file>` writes the session file only when the install is not linked. Linked, it writes nothing and says the session is in Vault.
- `/memoria session stop` on a linked install prints the recording's Mogsafe link on the session's range, `<server>/captures/<id>?from=<start>&to=<stop>` in Unix ms as the capture page reads it, where `<id>` ends the recording's upload URL. Before the recording has an upload it says the session reaches Vault once the recording uploads.
- `Uploader::recordingId` gives that id, from memory in the client that sends and from `uploads.txt` in the others.

## Capabilities

### New Capabilities
- `session-files`: a `/cap` session writes a local file only when unlinked.

### Modified Capabilities

## Impact

- **Memoria:** `src/core/uploader.*`, the plugin's commands, core tests.
- **Mogsafe:** none; its capture page already opens on `?from=&to=`.
