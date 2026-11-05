# Changelog

## 0.6.0-rc.4

**Captures**
- Memoria writes xicap format 5 (xicap 7.2), which has no signatures: Vault knows who sent a capture from the install's upload token. Only a Mogsafe that reads format 5 accepts these captures. Chat text is zeroed in place, so chat packets keep their length.
- After a long network gap, the part of a recording Vault cut off goes up as a capture of its own, which names the one it continues. Before, it stayed on the PC.
- A `/memoria note` in Japanese reaches the capture as readable text.

**Linking**
- Memoria no longer has a signing key. `/memoria link` asks Vault for a code and a secret handle, and polls with the handle until you enter the code. `/memoria key` is gone. The old `key` file is no longer read; delete it if you like.

**Game**
- A login no longer reads or writes Memoria's files on the game thread: the link and import reminders check them on a thread of their own.
- When the disk cannot keep up, Memoria's packet queue holds at most 64 MB in all. Before, the batch being written did not count, so the queue could reach twice that.

**Live**
- `/memoria live` sends its link to Vault in a header, not in the address, so it stays out of proxy and request logs. Mogsafe must read the link from that header.

**Install**
- Memoria creates its upload token so that only the Windows user running the game can open it; before, it took the Ashita folder's permissions, which often let every user of the PC read it. A token saved before keeps its old permissions. Linking now writes the token whole and swaps it in, so other clients never read half a token.

**Chat**
- When your account turns on Save chat on Vault's Memoria page, your captures keep all chat, other players' included; off, the default, chat is cut as before. A change applies from the next capture. `/memoria status` says which.

**Docs**
- The README lists every command and explains recording, uploads, live and imports.

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
