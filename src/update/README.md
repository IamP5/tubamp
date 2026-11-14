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
| `detail/Fetch.h` | The `Fetcher` interface and its contract, the redirect rule as a value (`HopPolicy`), the cancel token | Implemented; the system fetcher is `Fetch.mm` (NSURLSession, ARC) |
| `detail/Environment.h` | What the shell takes from outside: version, site, keys, fetcher, clock, OS, how to reach the message thread, how to open the browser; the fetch limits | Implemented |
| `Updater.h` | The service: `view`, `perform`, `watch`; one worker thread runs the effects | Implemented in `Updater.cpp` |
| `Bridge.h` | `UiState.update` / `updateChanged` as JSON (`viewJson`) and the `updateCommand` parse (`parseCommand`) | Implemented, pure; held to `ui/src/bridge/update-contract.json` |
| `plugin/Production.cpp` | The production `Environment` and `Updater()`: JUCE's message thread and browser, `dladdr`, the account's home | Plugin only |
| `update.cmake` | Sources, the release-key guard, the generated key table, `tubamp_update_tool`, the disk-test fixture module | |

Nothing outside `src/update`, `tests/update` and `tools/update` includes a `detail/` header.

The update sources join the plugin only with `-DTUBAMP_UPDATES=ON`, which also needs at least two keys in `keys/`. They link Security.framework, CoreFoundation, Foundation and the SDK's zlib. With it off, `PluginProcessor` and `WebEditor` compile none of their update lines and the page reads no `update` field. `tubamp_update_tool` is always built: `tubamp_update_tool test` runs the tables in `tests/update`, and building it checks the cases in `tests/update/must-not-compile`. `tests/update/make-fixtures.sh` regenerates the openssl-signed fixtures.

## The disk tests

The disk tables (`tests/update/DiskTests.cpp`) run in temporary directories under `$TMPDIR`. Every root is injected: the home, the system `Components` folder, the running image. They never read or write the real `~/Library` or `/Library`.

- By default they build a synthetic `tubamp.component` around `tubamp_update_fixture_component`, a universal module built from `tests/update/fixture-component.c`. They sign it ad hoc with `codesign`, zip it with `ditto -c -k --keepParent`, and make the broken variants with `PlistBuddy` and `lipo`.
- With `TUBAMP_UPDATE_COMPONENT=<path to a built tubamp.component>`, the same tables run on copies of that bundle, re-versioned and signed ad hoc. The source bundle is only read. The slice test and the arm64-only variant always use the synthetic module, because the real component has a single slice.
- Two `detect` cases attach a small FAT disk image with `hdiutil` at a folder inside the temporary home. If `hdiutil` cannot attach it, they print `SKIP`.
- Process death is real. `tubamp_update_tool child <command>` is the process the tests race and kill. It `_exit`s at a fault point, dies of `SIGXFSZ` mid-unpack under `RLIMIT_FSIZE`, or gets `SIGKILL`.
- `TUBAMP_UPDATE_KEEP=1` keeps the temporary directories. `TUBAMP_UPDATE_CHILD_OUTPUT=1` shows the children's output.

## The service tests

`tests/update/ServiceTests.cpp` runs the real `Updater` with a scripted `Fetcher` and a temporary home, and the test thread plays the message thread: `Environment::post` queues jobs that the test pumps. It covers the check and offer, a download with progress, the swap and restart pending, every install failure, cancel mid-download, two services on one home, a copy that cannot replace itself, and destruction mid-download. Cancel and destruction print their measured times.

`tests/update/BridgeTests.cpp` holds `viewJson` and `parseCommand` to `ui/src/bridge/update-contract.json` (embedded through the fixture table) and runs `HopPolicy` over hop sequences.

## Development roots

`-DTUBAMP_UPDATE_DEV_ROOTS=ON` makes the plugin take its home from `TUBAMP_UPDATE_HOME` and the system Components folder from `TUBAMP_UPDATE_SYSTEM_COMPONENTS` (default `$TUBAMP_UPDATE_HOME/SystemComponents`). Without `TUBAMP_UPDATE_HOME` the updater is off. A build without the option has no `getenv` for either, so a release build always uses the account's real folders.

## The network check

`tubamp_update_tool fetch-check [--path <path>] [--cancel-after <ms>]` makes one real GET of `https://github.com/IamP5/tubamp/<path>` (default `releases/latest`) through the system fetcher and prints each hop with its verdict, then the outcome. It is never part of `test`.

To build and run:

```sh
cmake -S . -B build/upd -DTUBAMP_COPY_AFTER_BUILD=OFF -DTUBAMP_BUNDLED_UI=OFF -DTUBAMP_WEBUI_DEV=OFF
cmake --build build/upd --target tubamp_update_tool
build/upd/tubamp_update_tool test
```
