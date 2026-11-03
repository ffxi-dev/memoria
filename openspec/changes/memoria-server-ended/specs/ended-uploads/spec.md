# Spec Delta

## Purpose

Keeps a capture Mogsafe ended from being sent twice, and keeps a live client recording after Mogsafe ends its upload.

## ADDED Requirements

### Requirement: An ended upload is never sent again
When Mogsafe answers `410` for an upload, Memoria SHALL never send that file's bytes again, in this run or after a restart, and SHALL keep the file. With `Mogsafe-Ended: complete` it SHALL count the file as closed. A `404` SHALL still start a new upload of the whole file, since Mogsafe holds none of it.

#### Scenario: A killed client's file
- **WHEN** the game starts again after a kill and Mogsafe has ended the last recording's upload
- **THEN** Memoria marks the file ended in `uploads.txt`, sends none of it, keeps it on disk, and `/memoria status` counts nothing left to send

#### Scenario: A lost close answer
- **WHEN** Mogsafe closed a complete capture but its answer never arrived
- **THEN** the next `410` with `Mogsafe-Ended: complete` marks it closed

### Requirement: A live recording goes on in a new capture
When Mogsafe ends the upload of the capture still recording, Memoria SHALL close that capture and start a new one at the next packet, marked mid-session, naming the ended one as the capture it continues, with its own upload from its first byte. It SHALL say so once in chat. A `/memoria session` SHALL go on. This SHALL hold whichever client sharing the install sends the uploads.

#### Scenario: A network gap longer than Mogsafe waits
- **WHEN** a client keeps playing through a 10-minute network gap, and Mogsafe ended its upload after 5
- **THEN** Memoria says "The server ended the last recording; continuing in a new one.", the next packets go to a new capture that names the old one, and Mogsafe receives no byte of the old capture again
