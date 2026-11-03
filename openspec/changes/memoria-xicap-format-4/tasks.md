# Tasks

## 1. Format 4

- [x] 1.1 Pin xicap v4.0.0; `AGENTS.md` names format 4's writer rules; verify the core tests and the 32-bit plugin build in CI
- [x] 1.2 A core test reads a recording back as format 4, version 4.0, dictionary id 0; verify it passes

## 2. Release

- [x] 2.1 Version 0.6.0-rc.3; tag `v0.6.0-rc.3`; verify the release workflow publishes a pre-release with Memoria.dll
- [ ] 2.2 The owner measures the game's memory with one writer and with a `/memoria session` open (vault's task 6.2); if two level-7 writers do not fit, session files use level 3; verify by recording the numbers in vault's Implementation Notes
