# capture-commands Specification

## Purpose
Lets a player mark a capture in game, in Ashita and in Windower, and see it on Mogsafe. The plan across bricks is the vault's `captures-from-recordings` change.

## Requirements

### Requirement: One capture at a time
`/memoria cap start [<name>] [<folder>]` SHALL start a capture, and `/memoria cap stop` SHALL stop it, with `//memoria` in Windower. The name is the words before the first one that holds a slash, and a start with no name SHALL get `cap_<UTC yyyymmdd-hhmm>`. A second start SHALL be refused with "A capture is running: <name>. <stop> first.", where `<stop>` is `/cap stop` for a capture captain started with its folder, and `/memoria cap stop` otherwise. `/memoria session` SHALL no longer exist.

#### Scenario: A second start
- **WHEN** a player runs `/memoria cap start other` while captain's `crabs` runs
- **THEN** chat says "A capture is running: crabs. /cap stop first.", and `crabs` goes on

### Requirement: Markers and an id of Memoria's own
At start, Memoria SHALL make the capture's id, 12 characters of URL-safe base64, and write the note `cap start <name> id=<id>` into the recording. At stop it SHALL write `cap stop id=<id>`. A logout or a login by another character SHALL end the capture with no stop marker, and a capture SHALL never resume.

#### Scenario: Logout
- **WHEN** a player logs out while `crabs` runs
- **THEN** chat says the capture ended, and the recording holds its start marker and no stop marker

### Requirement: Mogsafe hears of it at once
Linked, Memoria SHALL call `POST /v1/caps` with the id, name, recording, character id and start time, and `POST /v1/caps/<id>/stop` with the end time, off the game thread and in order. It SHALL retry a call that fails for a while, then drop it, since Mogsafe reads the markers. Linked, chat SHALL say "Live at <server>/captures/<id>" at start and "<name> (<length>) ready on Mogsafe in about a minute: <link>" at stop, or that the capture appears once uploads resume when they are paused.

#### Scenario: Start and stop
- **WHEN** a linked player runs `/cap start crabs`, plays four minutes and runs `/cap stop`
- **THEN** chat shows "Live at <link>" and later "crabs (4m 12s) ready on Mogsafe in about a minute: <link>"

### Requirement: A local copy that never uploads
Memoria SHALL write each capture to a file of its own: `memoria.xicap` in the folder given, or a `.xicap` path given, or else `<name>_<character>_<UTC yyyymmdd-hhmm>.xicap` in its `caps` folder, adding `-2`, `-3` and on while that name is taken. It SHALL never upload these copies.

#### Scenario: Windower
- **WHEN** a Windower player stops `//memoria cap start crabs`
- **THEN** `<Windower>/memoria/caps/` holds `crabs_Nouuurs_20261107-0736.xicap`
