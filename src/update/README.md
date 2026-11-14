# src/update: module map

The design is in `docs/UPDATES.md`. Each module owns one body of knowledge.

| File | Owns | State |
| --- | --- | --- |
| `UpdateTypes.h` | The public values: `Version`, `Phase`, `View`, `Command` | Implemented |
| `detail/Rules.h` | The `update-v1.signed` format, the payload, the offer policy (`evaluate`), every URL, the host allowlist (`Hosts.def`), the archive entry policy (`planExtraction`) | Implemented, pure |
| `detail/Trust.h` | Trusted keys (generated from `keys/*.pub.pem`), signature verification, SHA-256, `openEnvelope`, the code-seal check (`codeSealIntact`) | Implemented, Security.framework |
| `detail/Machine.h` | `Model`, `Event`, `Effect`, `step`, `project` | Implemented, pure |
| `detail/Prefs.h` | The settings value and its two rules; the store | Implemented, store in `Prefs.cpp` |
| `detail/EnvelopeCache.h` | `latest.signed`, the offer cache and replay floor | Implemented in `EnvelopeCache.cpp` |
| `detail/Bundle.h` | Where this copy lives, staging, the archive and bundle stages, `commit`, `reconcile` | Implemented: `Site.cpp` (`detect`, `readDisk`), `Archive.cpp` (`ArchiveSink`, the zip reader, `unpack`), `Inspect.cpp` (`inspect`), `Swap.cpp` (`Staging`, `commit`, `reconcile`) |
| `detail/Files.h` | POSIX plumbing for the disk files: descriptors, the support lock, capped reads, replace-by-rename writes, and what a bundle on disk says about itself (Info.plist, CPU types) | Implemented in `Files.cpp` and `Inspect.cpp` |
| `detail/Fetch.h` | The `Fetcher` interface and its contract | `CancelToken` implemented; the rest declared |
| `update.cmake` | Sources, the release-key guard, the generated key table, `tubamp_update_tool`, the disk-test fixture module | |

Nothing outside `src/update`, `tests/update` and `tools/update` includes a `detail/` header.

The update sources join the plugin only with `-DTUBAMP_UPDATES=ON`, which also needs at least two keys in `keys/`. They link Security.framework, CoreFoundation and the SDK's zlib. `tubamp_update_tool` is always built: `tubamp_update_tool test` runs the tables in `tests/update`, and building it checks the cases in `tests/update/must-not-compile`. `tests/update/make-fixtures.sh` regenerates the openssl-signed fixtures.

## The disk tests

The disk tables (`tests/update/DiskTests.cpp`) run in temporary directories under `$TMPDIR`. Every root is injected: the home, the system `Components` folder, the running image. They never read or write the real `~/Library` or `/Library`.

- By default they build a synthetic `tubamp.component` around `tubamp_update_fixture_component`, a universal module built from `tests/update/fixture-component.c`. They sign it ad hoc with `codesign`, zip it with `ditto -c -k --keepParent`, and make the broken variants with `PlistBuddy` and `lipo`.
- With `TUBAMP_UPDATE_COMPONENT=<path to a built tubamp.component>`, the same tables run on copies of that bundle, re-versioned and signed ad hoc. The source bundle is only read. The slice test and the arm64-only variant always use the synthetic module, because the real component has a single slice.
- Two `detect` cases attach a small FAT disk image with `hdiutil` at a folder inside the temporary home. If `hdiutil` cannot attach it, they print `SKIP`.
- Process death is real. `tubamp_update_tool child <command>` is the process the tests race and kill. It `_exit`s at a fault point, dies of `SIGXFSZ` mid-unpack under `RLIMIT_FSIZE`, or gets `SIGKILL`.
- `TUBAMP_UPDATE_KEEP=1` keeps the temporary directories. `TUBAMP_UPDATE_CHILD_OUTPUT=1` shows the children's output.

To build and run:

```sh
cmake -S . -B build/upd -DTUBAMP_COPY_AFTER_BUILD=OFF -DTUBAMP_BUNDLED_UI=OFF -DTUBAMP_WEBUI_DEV=OFF
cmake --build build/upd --target tubamp_update_tool
build/upd/tubamp_update_tool test
```
