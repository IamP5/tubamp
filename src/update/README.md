# src/update: module map

The design is in `docs/UPDATES.md`. Each module owns one body of knowledge.

| File | Owns | State |
| --- | --- | --- |
| `UpdateTypes.h` | The public values: `Version`, `Phase`, `View`, `Command` | Implemented |
| `detail/Rules.h` | The `update-v1.signed` format, the payload, the offer policy (`evaluate`), every URL, the host allowlist (`Hosts.def`), the archive entry policy (`planExtraction`) | Implemented, pure |
| `detail/Trust.h` | Trusted keys (generated from `keys/*.pub.pem`), signature verification, SHA-256, `openEnvelope` | Implemented, Security.framework |
| `detail/Machine.h` | `Model`, `Event`, `Effect`, `step`, `project` | Implemented, pure |
| `detail/Prefs.h` | The settings value and its two rules; the store | Value and rules implemented; store declared |
| `detail/EnvelopeCache.h` | `latest.signed`, the offer cache and replay floor | Declared |
| `detail/Bundle.h` | Where this copy lives, staging, the archive and bundle stages, `commit`, `reconcile` | Values and `siteOf` implemented; the rest declared |
| `detail/Fetch.h` | The `Fetcher` interface and its contract | `CancelToken` implemented; the rest declared |
| `update.cmake` | Sources, the release-key guard, the generated key table, `tubamp_update_tool` | |

Nothing outside `src/update`, `tests/update` and `tools/update` includes a `detail/` header.

The update sources join the plugin only with `-DTUBAMP_UPDATES=ON`, which also needs at least two keys in `keys/`. `tubamp_update_tool` is always built: `tubamp_update_tool test` runs the tables in `tests/update`, and building it checks the cases in `tests/update/must-not-compile`. `tests/update/make-fixtures.sh` regenerates the openssl-signed fixtures.
