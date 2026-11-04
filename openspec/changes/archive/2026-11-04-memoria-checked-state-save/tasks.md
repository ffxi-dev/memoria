# Tasks

## 1. Checked saves

- [x] 1.1 `replaceFile` in `src/core/tus.*`: write `.tmp`, check write and close, rename, and say why on failure; `Uploader::save` and `Importer::save` use it; verify the existing upload and import tests pass
- [x] 1.2 Uploader: a create marks the state dirty, a failed save keeps it dirty, and no step sends a byte until a save succeeds; verify a core test where the save fails across two passes with nothing sent, then either works again or restarts, and the capture arrives once
- [x] 1.3 Importer: stop, say why, and keep `imports.txt` when the run's first save or the save after a create fails; verify a core test where the first save fails with no request sent, then a save after a create fails with no byte sent, then a new importer uploads the log once and moves the folder
