# upload-limits Specification

## Purpose
Keeps Memoria from retrying what Mogsafe's upload limits refuse.

## Requirements

### Requirement: A capture past a size limit stops for good
When Mogsafe answers an append with `413`, Memoria SHALL stop sending that capture, in this run and after a restart, and SHALL keep the file. It SHALL say once in chat that Mogsafe refused the capture, why, and that it stays on this PC, and `/memoria status` SHALL list it as refused with the reason. The recorder SHALL go on writing the same file.

#### Scenario: A live recording passes 2 GiB
- **WHEN** Mogsafe answers `413` to an append of the capture still recording
- **THEN** Memoria says "Mogsafe refused <name>: a live recording may hold at most 2.0 GiB. Memoria stopped sending it; it stays on this PC.", sends no more of it, and counts none of it as left to send

### Requirement: A full Mogsafe disk waits half an hour
When Mogsafe answers a create or an append with `507`, Memoria SHALL end the pass, send nothing for 30 minutes, then try again, and keep every file. It SHALL print no chat line, and `/memoria status` SHALL show it as the uploads' error without naming Mogsafe's disk.

#### Scenario: Mogsafe runs short of disk
- **WHEN** Mogsafe answers `507`
- **THEN** Memoria says nothing in chat, `/memoria status` shows "retrying, Failed to upload, will try again later", and uploads resume on the first try after Mogsafe has room
