# Tasks

## 1. Memoria

- [x] 1.1 A cut-off file that stopped changing gets `<name>-rest.xicap` once, marked `rest` in `uploads.txt`, and uploads; a failed write waits for a restart; verify core tests with a stand-in writer
- [x] 1.2 The uploader writes the tail with xicap's `continueFrom`, and the stand-in goes; verify core tests on real captures

## 2. xicap

- [x] 2.1 xicap's `continueFrom(source, fromByte, out)`: the frames of a capture from the first frame not wholly within its first `fromByte` bytes, as a new capture continuing it (a new capture id, the source's id as previous, the source's metadata), frames cut off at the end left out; no packets carried from before `fromByte` (xicap 7.0.0)
