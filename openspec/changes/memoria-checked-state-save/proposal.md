# Proposal

## Why

Both uploaders save their upload's URL before the first byte, so a crash resumes the upload instead of sending the file again. Neither checks the save. The capture uploader ignored the error it got back; the captain importer's `save()` returned nothing and checked no write, close, or rename. With a full disk, Memoria could send gigabytes to an upload whose URL lived only in memory, and after a crash start a new one. The uploader's next pass skipped the save too, since the URL was already set. Codex's review of 2026-11-02 (finding 2) found it.

## What Changes

- One helper, `replaceFile`, writes a state file to `.tmp`, checks the write and close, renames it over the old one, and says why if any step failed. `uploads.txt` and `imports.txt` both use it.
- The capture uploader sends no byte while its state file lacks what memory holds. A failed save leaves the state dirty; every later step retries the save before sending, even for an upload whose URL is set. `/memoria status` shows the error, as before.
- The captain importer stops when it cannot save: at the start, before it sends anything, and after a create, before the first byte. It says why, keeps `imports.txt`, and goes on at the next start or link.

## Capabilities

### New Capabilities
- `upload-resume`: no byte goes to Mogsafe until the upload's URL is on disk.

### Modified Capabilities

## Impact

- **Memoria:** `src/core/tus.*` (`replaceFile`), `src/core/uploader.*`, `src/core/captain.*`, core tests.
- **Mogsafe, infra/:** none. A save that fails after a create leaves an empty upload, which Mogsafe's sweep deletes after 24 hours.
