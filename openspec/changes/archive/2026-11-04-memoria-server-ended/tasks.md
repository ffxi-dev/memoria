# Tasks

## 1. Uploader

- [x] 1.1 A `410` marks the file ended (or closed, for `Mogsafe-Ended: complete`) and `uploads.txt` keeps it; `Uploader::ended` reads it from memory or, for a client not sending, from disk; verify core tests for a killed client's file across restarts and a lost close answer
- [x] 1.2 A `404` whose close also answers `404` starts the whole file over; a close that fails keeps the upload; verify the core test for an upload Mogsafe never kept

## 2. Recorder

- [x] 2.1 `RecorderOptions::ended`, asked at each flush: an ended capture closes, and the next packet opens a mid-session capture naming it, keeping any session; verify the core test where the server ends a live recording and the new capture uploads alone, once, naming the old one
- [x] 2.2 Session wires it to the uploader and says so once in chat
