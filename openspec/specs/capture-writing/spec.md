# capture-writing Specification

## Purpose
Which xicap format Memoria writes, and how much of the game's memory its writers take.

## Requirements

### Requirement: Memoria writes xicap format 5 at the live preset
Memoria SHALL write every capture, the main recording and any `/memoria session` file, as xicap format 5 through xicap's library with its live preset: zstd level 7, a window of at most 2 MiB, and a match table capped at 2^19. A session file MAY use level 3 instead, if two level-7 writers do not fit the game's memory. Each frame SHALL reach disk, decodable, when the writer ends it.

#### Scenario: A recording
- **WHEN** Memoria built on xicap 7.1 records a session
- **THEN** the file reads as format 5, and the main writer's compressor holds about 5.5 MB

#### Scenario: Upload while recording
- **WHEN** Memoria has ended a frame and the network is up
- **THEN** the next upload pass sends it, and a reader of Mogsafe's copy reads its packets
