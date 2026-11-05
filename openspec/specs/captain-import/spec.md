# captain-import Specification

## Purpose
Lets Memoria send a player's old captain captures to Mogsafe, under the account the install is linked to, and move each one aside once Vault holds it.

## Requirements

### Requirement: Memoria finds captain captures waiting to upload
A captain capture waiting to upload SHALL be any folder, at any depth under `<Ashita>/addons/captain/captures/`, that holds `manifest.txt` and `PacketLogger/full.log`. A folder whose `full.log` changed in the last 10 minutes SHALL be left for later, since captain may still be writing it.

#### Scenario: Nested folders
- **WHEN** captain's captures folder holds `Mining/test_2026-8-4/Mirabeth/manifest.txt` and `Mining/test_2026-8-4/Mirabeth/PacketLogger/full.log`
- **THEN** Memoria counts one capture waiting, for Mirabeth

#### Scenario: A session made with Memoria
- **WHEN** a character folder holds `manifest.txt` and `memoria.xicap` but no `PacketLogger/full.log`
- **THEN** Memoria does not count it

#### Scenario: A log still growing
- **WHEN** a folder's `full.log` changed 2 minutes ago
- **THEN** `/memoria import` skips it and says it is still being written

### Requirement: Memoria reminds once per install
At a login or a completed `/memoria link`, while the install is linked and at least one captain capture waits, Memoria SHALL say "Run /memoria import to upload existing Captain captures" if it has never said it on this install. It SHALL remember across restarts and across clients sharing the install that it did, and SHALL never say it again, nor once the player has run `/memoria import`.

#### Scenario: First login after linking
- **WHEN** a linked player with three captain captures logs in for the first time since this change
- **THEN** Memoria shows the reminder once

#### Scenario: Later logins
- **WHEN** the same player logs in again, restarts the client, and logs in on a second client sharing the install
- **THEN** Memoria does not show the reminder again

#### Scenario: Two clients at once
- **WHEN** two clients sharing an install log in at the same moment with captures waiting
- **THEN** only one of them shows the reminder

#### Scenario: Not linked
- **WHEN** an unlinked player with captain captures logs in
- **THEN** Memoria shows no import reminder

### Requirement: Importing needs a link
`/memoria import` SHALL send nothing from an install that is not linked, and SHALL say to run `/memoria link` first.

#### Scenario: Unlinked import
- **WHEN** an unlinked player runs `/memoria import`
- **THEN** Memoria says to run `/memoria link` first and Mogsafe receives nothing

### Requirement: Each capture uploads as a captain import of the linked account
`/memoria import` SHALL send each waiting folder's `full.log` with its `manifest.txt`, its character folder's name, and the CRC-32 and size of the plain log, through Mogsafe's resumable upload marked as a captain import, with the install's token. When the folder holds a non-empty `SearchLog/search.log`, Memoria SHALL send it compressed ahead of `full.log` in the same upload, with its compressed length and the CRC-32 and size of the plain search log. No other file in the folder SHALL be sent.

#### Scenario: One capture
- **WHEN** a linked player runs `/memoria import` with one captain capture waiting
- **THEN** Mogsafe lists one converting capture uploaded by the account the install is linked to

#### Scenario: Other files in the folder
- **WHEN** a character folder also holds `memoria.xicap` and `PacketLogger/incoming/0x00A.log`
- **THEN** only `full.log` and `manifest.txt` are sent

#### Scenario: A folder with a search log
- **WHEN** a character folder holds `SearchLog/search.log` beside `PacketLogger/full.log`
- **THEN** one upload carries the search log, then `full.log`, and Mogsafe lists one capture

#### Scenario: An empty search log
- **WHEN** a character folder's `SearchLog/search.log` is empty
- **THEN** only `full.log` and `manifest.txt` are sent

### Requirement: Importing never holds up the game or recording
The import SHALL read, compress, and send on Memoria's own threads. Packet hooks and frames SHALL not wait on it, recording and capture uploads SHALL go on while it runs, and unloading Memoria SHALL not wait for the network.

#### Scenario: A large log
- **WHEN** a player imports a 5 GB `full.log` while playing
- **THEN** the session records every packet and frames keep their pace

#### Scenario: Unload mid-import
- **WHEN** the player unloads Memoria while a chunk is in flight
- **THEN** Memoria unloads without waiting for Mogsafe to answer

### Requirement: The import reports progress in chat
When an import starts, Memoria SHALL say how many captures and bytes it will send. It SHALL report each capture's result as uploaded, already in Vault, refused with the reason, or skipped, and a summary at the end. `/memoria status` and `/memoria import` while one runs SHALL show the capture being sent and its progress, and SHALL NOT start a second import.

#### Scenario: Asking again
- **WHEN** the player runs `/memoria import` while an import is sending its second of four captures
- **THEN** Memoria says it is sending capture 2 of 4 and how far along, and starts nothing new

#### Scenario: Another client imports
- **WHEN** a second client sharing the install runs `/memoria import` while the first imports
- **THEN** the second says another client is importing and sends nothing

### Requirement: An import resumes where it stopped
An upload cut off by a network failure, crash, or restart SHALL resume from the bytes Mogsafe holds. An import asked for and not finished SHALL resume by itself at the next start or link while the install is linked. `/memoria upload off` SHALL pause it as it pauses capture uploads.

#### Scenario: Game closed mid-upload
- **WHEN** the player closes the game halfway through a capture's upload and starts it again the next day
- **THEN** Memoria resumes that upload from Mogsafe's offset without being asked, then sends the rest

#### Scenario: Closed inside the search log
- **WHEN** the game closes after part of a folder's search log was sent
- **THEN** Memoria resumes from Mogsafe's offset, sends the rest of the search log and then `full.log`, and no byte twice

#### Scenario: Paused
- **WHEN** the player runs `/memoria upload off` during an import
- **THEN** the import sends nothing more until `/memoria upload on`

### Requirement: A log already in Vault counts as uploaded
When Mogsafe answers that a log is already in Vault, Memoria SHALL report the capture as already in Vault, naming the capture Mogsafe gave, and treat its folder as uploaded. When Mogsafe instead takes the upload to give an archived capture its search log, Memoria SHALL send it and report the capture it replaced.

#### Scenario: Imported before on the web page
- **WHEN** the player imported a folder through Mogsafe's `/import` page last week and now runs `/memoria import`
- **THEN** Memoria sends no bytes of that log, says it is already in Vault, and moves the folder

#### Scenario: Imported before without its search log
- **WHEN** the player's folder holds a search log and its `full.log` is in Vault as a capture without one
- **THEN** Memoria sends both logs, says it replaced that capture, and moves the folder

### Requirement: Refused captures stay where they are
A capture Mogsafe refuses SHALL stay in captain's captures folder and SHALL be reported with Mogsafe's reason; the import SHALL go on with the next one. A refused token SHALL stop the import and leave the install unlinked, as it does for capture uploads.

#### Scenario: Manifest without a start time
- **WHEN** Mogsafe refuses a folder because its `manifest.txt` has no `StartTime`
- **THEN** Memoria reports the reason, leaves the folder in place, and sends the next capture

#### Scenario: Revoked install
- **WHEN** the player revokes the install on Mogsafe's link page during an import
- **THEN** the import stops and Memoria says the install is no longer linked

### Requirement: Uploaded folders move aside
Once Mogsafe holds a capture's log, Memoria SHALL move its folder to the same relative path under `<Ashita>/addons/captain/captures-uploaded/`, remove parent folders this leaves empty, and never consider it again. Memoria SHALL never read that folder, and SHALL never delete a capture.

#### Scenario: After upload
- **WHEN** `2026-8-4_23_54/Mirabeth` finishes uploading
- **THEN** it is at `addons/captain/captures-uploaded/2026-8-4_23_54/Mirabeth` and later imports and reminders do not count it

#### Scenario: Crash between upload and move
- **WHEN** the game crashes after Mogsafe accepted a log but before its folder moved
- **THEN** the next import finds it already in Vault, sends no bytes of it, and moves it

#### Scenario: Folder in use
- **WHEN** another program holds a file in the folder open and the move fails
- **THEN** Memoria reports that the capture is in Vault but its folder could not move, and leaves it for the next import
