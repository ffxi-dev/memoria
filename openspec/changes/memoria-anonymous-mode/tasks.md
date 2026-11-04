# Tasks

## 1. Recording

- [x] 1.1 Pseudonymize each packet before the writer, the session file and the live feed, and the header character, under the anonymous policy and signing seed; verify "an anonymous capture names no player", which scans every file Memoria left, every packet read back and every live frame for the real names and ids
- [x] 1.2 Never name a capture of another mode as the previous one; verify "a capture never names one of another mode as the capture it continues"
- [x] 1.3 The account's key from Mogsafe, or one derived from the seed; `/memoria mode anonymous` on an unlinked install; the live hello's name
- [x] 1.4 Run the core tests on Linux (lsb)

## 2. Release

- [ ] 2.1 Pin the xicap release that holds the pseudonymizer
- [ ] 2.2 Set the header mode field once xicap adds it
- [ ] 2.3 Record in anonymous mode on a live client, when the owner schedules it, and check the capture on Mogsafe
