# upload-resume Specification

## Purpose
Keeps a crash or restart from sending a capture twice.

## Requirements

### Requirement: No byte goes before a capture's upload URL is saved
Before Memoria sends the first byte of a capture's upload, it SHALL have saved the upload's URL to `uploads.txt`: written, flushed, closed, and renamed into place with the rename flushed too, each without error, so a crash or power cut cannot leave the file empty or half written. While a save has failed, Memoria SHALL retry it before sending any byte, even for an upload whose URL it already holds, and SHALL report why it failed.

#### Scenario: The capture state cannot be saved
- **WHEN** Mogsafe creates an upload for a capture and `uploads.txt` cannot be replaced
- **THEN** Memoria sends none of the capture, says why in `/memoria status`, and sends nothing on later passes until the save works

#### Scenario: A flush fails
- **WHEN** flushing the state file fails, or the rename's own flush does (write-through on Windows, the folder's flush on POSIX)
- **THEN** Memoria reports the save as failed and sends nothing that depends on it until a save succeeds; after a failed folder flush the new state stays in place

#### Scenario: A restart after a failed save
- **WHEN** a capture's state save failed and the game restarts with the disk writable again
- **THEN** Memoria uploads the capture once, and the upload whose URL was lost holds no bytes
