# Proposal

## Why

Mogsafe ends a live recording after 5 minutes without bytes and refuses its bytes from then on (vault's `end-silent-recordings`, `refuse-duplicate-captures`). After a long network gap, everything the client recorded since Mogsafe's last byte stayed only on the PC: an hour's outage lost an hour from Vault. The owner decided on 2026-11-04: send that tail as a new capture that names the old one, signed by the install's key.

## What Changes

- Once a file Mogsafe cut off stops changing, the uploader writes its frames past the bytes Mogsafe holds to `<name>-rest.xicap`, a new capture that continues it, and uploads it like any capture. `uploads.txt` marks the old file `rest`, so a restart writes nothing twice.
- The writing goes through `TailWriter`, which xicap's library will supply. Until it does, the uploader writes nothing and `/memoria status` counts the tail as not in Vault.
- A failed write is tried again after a restart, not on every pass.

## Capabilities

### New Capabilities
- `cut-off-tail`: the tail of a cut-off recording reaches Vault as a capture of its own.

### Modified Capabilities

## Impact

- **Memoria:** `src/core/uploader.*`, core tests; `src/plugin/session.cpp` once xicap supplies the writer.
- **xicap:** a public C++ call that writes the frames of a capture from a byte offset as a new capture continuing it (see tasks).
- **Mogsafe:** none; the tail is an ordinary capture with its own id.
