# Tasks

## 1. Unload close

- [x] 1.1 `Uploader::finish(name, limit)`: the upload thread stops its pass, sends the named file's last bytes and closes its upload in up to three steps; the caller waits at most `limit`; verify core tests where the tail and footer arrive and close within the limit, and where a hung request holds the caller for the limit and no longer
- [x] 1.2 `~Session` calls `finish` with 3 seconds after the recorder stops; verify the plugin builds in CI
- [ ] 1.3 A run on a live client: quit the game and check Mogsafe lists the capture complete (the owner schedules it)
