# Proposal

## Why

Mogsafe ends a recording that receives no bytes for 5 minutes (vault's `end-silent-recordings`). At unload, which a normal game exit is, the recorder writes the capture's last frame and footer, but the uploader stopped at once and sent neither. Unless the game started again within 5 minutes, Vault kept every capture cut off: no footer, and without the last seconds when the player quit while in a zone. The owner decided on 2026-11-04: at unload, send the tail and footer, giving up after 3 seconds, never longer.

## What Changes

- At unload, once the recorder has closed its capture, the client that uploads the install's captures sends that capture's last bytes and closes its upload. The game thread waits for it at most 3 seconds; then any request in flight is cancelled and nothing more is sent.
- A pass under way when unload starts stops between files and chunks, so the 3 seconds go to the capture that just closed.
- A client that does not upload for the install waits for nothing: the uploading client sends the file on its next pass.

## Capabilities

### New Capabilities
- `unload-close`: unloading sends a capture's end within a bound.

### Modified Capabilities

## Impact

- **Memoria:** `src/core/uploader.*` (`finish`), `src/plugin/session.cpp`, core tests.
- **vault:** `add-memoria`'s `capture-upload` says unloading never waits; 0.5.4's "Unloading Memoria no longer waits on the network" now means "waits at most 3 seconds".
