# Proposal

## Why

Mogsafe ends a live recording after 5 minutes without bytes and refuses its bytes from then on. After a long network gap, everything the client recorded since Mogsafe's last byte stayed only on the PC: an hour's outage lost an hour from Vault. The tail now goes up as a new capture that names the old one.

## What Changes

- Once a file Mogsafe cut off stops changing, the uploader writes its frames past the bytes Mogsafe holds to `<name>-rest.xicap`, a new capture that continues it, and uploads it like any capture. `uploads.txt` marks the old file `rest`, so a restart writes nothing twice.
- xicap's `continueFrom` writes the tail. A tail it cannot write stays on the PC, and `/memoria status` counts it as not in Vault. (Since xicap 7.2, `continueFrom` lives in Memoria's uploader.)
- A failed write is tried again after a restart, not on every pass.

## Capabilities

### New Capabilities
- `cut-off-tail`: the tail of a cut-off recording reaches Vault as a capture of its own.

### Modified Capabilities

## Impact

- **Memoria:** `src/core/uploader.*`, core tests.
- **xicap:** `continueFrom(source, fromByte, out)`, from xicap 7.
- **Mogsafe:** none; the tail is an ordinary capture with its own id.
