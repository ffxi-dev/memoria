# Spec Delta

## Purpose

Keeps a long network gap from losing the recording it covers.

## ADDED Requirements

### Requirement: A cut-off recording's tail goes up as a capture of its own
When Mogsafe has cut off a file's upload and the file has stopped changing, Memoria SHALL write the file's frames past the bytes Mogsafe holds as a new capture that names the cut-off one as the capture it continues, and SHALL upload it. It SHALL do so once per file, across restarts, and never for an upload Mogsafe refused.

#### Scenario: An hour without network
- **WHEN** a client records through an hour-long outage and Mogsafe cut the upload off after 5 minutes
- **THEN** once the network returns, Vault receives the outage's packets in a new capture that continues the cut-off one

#### Scenario: Tail cannot be written
- **WHEN** xicap cannot write the tail, because the file is damaged
- **THEN** the tail stays on the PC and `/memoria status` counts it as not in Vault
