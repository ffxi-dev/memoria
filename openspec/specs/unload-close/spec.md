# unload-close Specification

## Purpose
Keeps a normal game exit from leaving its capture cut off on Mogsafe.

## Requirements

### Requirement: Unloading sends the capture's end, within 3 seconds
When Memoria unloads in the client that uploads the install's captures, it SHALL send the capture it just closed up to its footer and close its upload, and SHALL hold the game thread for at most 3 seconds doing so. When the time runs out, it SHALL cancel any request in flight and send nothing more.

#### Scenario: A normal exit
- **WHEN** the player quits the game with Mogsafe reachable
- **THEN** Mogsafe holds the capture whole, footer included, and keeps it complete

#### Scenario: A dead connection at exit
- **WHEN** the player quits while a request to Mogsafe hangs
- **THEN** the game closes after at most about 3 seconds, and the capture stays on the PC as before
