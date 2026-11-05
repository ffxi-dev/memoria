# upload-resume Specification

## Purpose
Keeps a crash or restart from sending a capture or captain log twice.

## Requirements

### Requirement: No byte goes before the upload's URL is saved
Before Memoria sends the first byte of an upload, it SHALL have written the upload's URL to its state file (`uploads.txt` for captures, `imports.txt` for captain imports), flushed the file to disk, closed it, and renamed it into place with the rename itself on disk (a write-through rename on Windows; the folder flushed on POSIX), each without error, so that neither a crash nor a power cut can leave the file empty or half written. While a save has failed, Memoria SHALL retry it before sending any byte, even for an upload whose URL it already holds, and SHALL report why it failed.

#### Scenario: The capture state cannot be saved
- **WHEN** Mogsafe creates an upload for a capture and `uploads.txt` cannot be replaced
- **THEN** Memoria sends none of the capture, says why in `/memoria status`, and sends nothing on later passes until the save works

#### Scenario: A flush fails
- **WHEN** flushing the state file, or after the rename its folder, fails
- **THEN** Memoria reports the save as failed and sends nothing that depends on it until a save succeeds; after a failed folder flush the new state stays in place

#### Scenario: A restart after a failed save
- **WHEN** a capture's state save failed and the game restarts with the disk writable again
- **THEN** Memoria uploads the capture once, and the upload whose URL was lost holds no bytes

#### Scenario: The import state cannot be saved
- **WHEN** Mogsafe creates an upload for a captain log and `imports.txt` cannot be replaced
- **THEN** Memoria sends none of the log, says the import stopped and why, keeps `imports.txt`, and goes on at the next start or link

#### Scenario: The import cannot mark its run
- **WHEN** a player runs `/memoria import` and `imports.txt` cannot be written
- **THEN** Memoria sends nothing and says the import stopped and why
