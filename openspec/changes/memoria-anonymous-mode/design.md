# Design

The mapping, the field map, and what is zeroed live in xicap: FORMAT.md's Anonymous captures, `pseudonym_fields.tsv`, and `openspec/changes/anonymous-pseudonyms/design.md` there. This change only decides when Memoria uses them.

## Where the rewriting runs

On the writer thread, in `Recorder::write`, on a copy of the packet, before the writer, the session writer and the live feed take it. The packet hooks stay a copy into the queue. The signatures cover the rewritten bytes, and no real name or id reaches the disk: the capture, its file name, and `state-<id>.txt` all use the pseudonym.

## When a capture is anonymous

`RecorderOptions::pseudonymKey` is asked as each capture opens, beside `policy`. A key makes the capture anonymous: xicap's anonymous policy, whatever `policy` says, and the anonymous signing seed. The open capture keeps its mode; a change applies from the next one. One pseudonymizer serves every capture while the key stays the same, so its record of names already met carries over, and a new key or mode starts afresh. When the mode or key changes, the next capture names no previous capture: an anonymous capture that named a default one would link the two.

## The key

`pseudonymKeyFor` takes the account's 64-hex key from Mogsafe, or derives one from the seed when there is none or it is malformed. Either way the capture stays anonymous: a capture made with the derived key just won't match the account's imports.

## How it is marked

The policy name, `xicap-anonymous`, until xicap's header mode field lands; then the header says so as well.
