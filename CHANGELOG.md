# Changelog

## Unreleased

**Modes (new)**
- Memoria records in your account's privacy mode, chosen on Vault's Memoria page: default cuts chat as before, everything keeps all chat, other players' included. A change applies from the next capture. An unlinked install sets its own with `/memoria mode`. `/memoria status` shows the mode.
- Anonymous mode: default, and every player's name and id, yours included, becomes a stand-in that stays the same across your captures. The file on your PC, its name, the live view and the upload hold only stand-ins. Every packet keeps its layout; only text players write, such as proposals and bazaar messages, is blanked.

## 0.6.0-rc.3

**Captures**
- Memoria writes xicap format 4 (xicap 4.0): about half the size of format 3 on disk and in upload, with the same packets. Vault reads both formats, and keeps Memoria's signature when it repacks a capture for its archive.

## 0.6.0-rc.2

**Linking**
- The first `/memoria link` of a session prints the address of Vault's privacy policy, which the link page asks you to accept.

**Uploads**
- When Vault ends a recording that has been silent too long, Memoria never sends it again. If the game is still running, Memoria says so and goes on in a new recording.

## 0.6.0-rc.1

**Captain imports (new)**
- `/memoria import` uploads earlier captain captures from `addons/captain/captures/` to Vault, then moves each one Vault holds to `addons/captain/captures-uploaded/`. A folder's `SearchLog/search.log` goes in the same upload, and a capture already in Vault without it gains it.
- Memoria says once per install, at a login or link, when such captures wait.
- An import goes on after a crash or restart, and pauses with `/memoria upload off`.

**Uploads**
- When Memoria cannot save upload progress, for example on a full disk, it sends nothing until it can, so a crash never sends a capture twice.

## 0.5.4

Never tagged: these changes first ship in 0.6.0-rc.1.

**Status line**
- `/memoria status` now answers in three lines: what is recording, on which client, and whether Vault holds every byte so far.
- It shows uploads made by any client sharing the Ashita folder, not only its own.
- Loading Memoria after login no longer leaves the capture without a client version.

**Live mode (new)**
- `/memoria live` opens a link in your browser that shows your packets in Lens as they happen. `/memoria live off` ends it.
- Viewers see the new character's name after a character switch.
- `/memoria live off` ends the link even while the connection is down.

**Uploads**
- Pause, unlink, and re-link apply to every client sharing the install.
- A crash no longer makes a capture upload twice.
- A failed save of upload progress shows in `/memoria status`.

**Fixes**
- Unloading Memoria no longer waits on the network.
- A failed `/memoria session` no longer stops the main capture, and a session never overwrites an existing file.
- Clients starting together on a fresh install all use the same signing key.
- Re-linking while another client's token is refused keeps the new link.
