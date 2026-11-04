# Tasks

## 1. Memoria

- [x] 1.1 `TailWriter` and `Uploader::continueTail`: a cut-off file that stopped changing gets `<name>-rest.xicap` from the writer once, marked `rest` in `uploads.txt`, and uploads; a failed write waits for a restart; verify core tests with a stand-in writer
- [ ] 1.2 `Session` passes a writer built on xicap's call, with the install's seed

## 2. xicap

- [ ] 2.1 A public call that writes the frames of a capture starting with the first frame not wholly within its first `from` bytes as a new capture continuing it: a new capture id, the source's id as previous, start context mid-session, the source's header metadata, signed with the given seed, the live frame policy, the scene the first frame needs carried as Mogsafe's cuts carry it, and frames cut off at the end left out
