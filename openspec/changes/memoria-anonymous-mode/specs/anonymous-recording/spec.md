# Spec Delta

## Purpose

What Memoria writes and sends in anonymous mode.

## ADDED Requirements

### Requirement: An anonymous capture names no player
In anonymous mode Memoria SHALL replace every player name and player id with its pseudonym before any byte is written to disk or sent live: in the packets, the header's character, the capture's file name, and its state file.

#### Scenario: Scanning what an anonymous recording left
- **WHEN** Memoria records a zone-in and another player's update in anonymous mode
- **THEN** no file it wrote, no packet read back, and no live frame holds either real name or id

### Requirement: Anonymous captures stand apart from the install's others
An anonymous capture SHALL be signed with a key derived from the install's seed, never the install's own key, and SHALL never name a capture of another mode or key as the one it continues.

#### Scenario: Switching to anonymous mid-session
- **WHEN** the mode changes to anonymous and the next capture opens
- **THEN** its previous capture id is all zero and its signing key is not the install's
