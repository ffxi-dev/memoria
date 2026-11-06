# session-files Specification

## Purpose
Keeps a `/cap` session from writing a second copy of what Mogsafe already holds.

## Requirements

### Requirement: A session file only when unlinked
`/memoria session start <file>` SHALL write the session's capture to `<file>` only while the install is not linked. While linked it SHALL write no file, and `/memoria session stop` SHALL print the Mogsafe link of the recording that holds the session.

#### Scenario: Unlinked
- **WHEN** an unlinked player runs `/cap start test` and `/cap stop`
- **THEN** captain's folder holds the session's `.xicap`, as before

#### Scenario: Linked
- **WHEN** a linked player runs `/cap start test` and `/cap stop`
- **THEN** no session file is written, and chat shows the recording's link on Mogsafe
