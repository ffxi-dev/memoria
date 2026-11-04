# Proposal

## Why

Captures are public once uploaded. The owner chose an anonymous mode, beside default and everything (`memoria-mode`, ffxi-dev/memoria#40): a capture that names no player, the recorder included, but still lets a reader follow each player, because the same player keeps the same stand-in.

## What Changes

- In anonymous mode the recorder runs every packet through xicap's pseudonymizer (ffxi-dev/xicap#25) before the writer, the `/cap` session file, and the live feed. The header's character gets the same pseudonyms, and the capture carries xicap's anonymous policy: default, plus stubs for what the pseudonymizer cannot place.
- The pseudonym key is the account's, from Mogsafe's `GET /v1/install/mode`, so live captures and imports agree. Without one, unlinked or before Mogsafe answers, it is derived from the install's seed with the label `xicap pseudonym key`.
- Anonymous captures sign with a key derived from the seed (`xicap anonymous signing`), so the header's signing key does not link them to the install's other captures. Mogsafe binds that key to the install the first time an anonymous capture arrives with the install's token.
- A capture never names one of another mode as the capture it continues; the state file and the capture's file name use the pseudonym.
- `/memoria mode anonymous` works on an unlinked install, and the live hello names the pseudonym.

## Impact

- Pins xicap at the `anonymous-pseudonyms` commit until it is released.
- Mogsafe accepts the anonymous signing key and makes anonymous imports in ffxi-dev/mogsafe's `anonymous-imports` branch.
