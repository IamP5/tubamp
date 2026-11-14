# In-plugin updates

tubamp tells you when a newer release exists and, after one click, installs it while the host keeps running. The new version takes effect when the host next starts. This page explains how version 1 works and why. It stays off until the maintainer creates the signing keys (`src/update/keys/README.md`).

## What you see

**First install.** Both release installers, Apple Silicon and Intel, install for the current user only. Installer offers "Install for me only" and asks for no password. The component goes to `~/Library/Audio/Plug-Ins/Components/tubamp.component`, where tubamp can later replace itself without privileges. Each macOS account that uses tubamp runs the installer once.

**The first time you open the plugin**, a toast says that tubamp checks GitHub once a day for a new version. It has a **Turn off** action, and it does not come back. The same switch lives in Settings. tubamp sends no request before that toast was shown: the first automatic check runs at a later editor open, and only if checks are still on. **Check now** checks at once.

**When a release is out:**

1. You open the plugin. If no check ran on this account in the last 24 hours, tubamp downloads one small signed file in the background. Audio and the editor never wait for it.
2. A toast says "tubamp 0.4.0 is available" with an **Update** action, once per version. Settings always shows the version row.
3. You click **Update**. Settings shows the download progress, then "Installing". You can cancel until the swap starts. Audio keeps running on the old version.
4. Settings asks you to restart the host. Nothing forces the restart.
5. After the restart, Logic validates the new version once and the plugin opens as 0.4.0.

The click is required because tubamp never installs code you did not ask for. The restart is required because tubamp never loads new code into a running host.

A copy in `/Library/Audio/Plug-Ins/Components` cannot replace itself. It shows the new version with **Get the installer**, which opens the release page. If a copy in `/Library` exists beside your own, Settings warns you, because two copies make it unclear which one Logic loads.

## When something goes wrong

| Situation | What you see | Disk afterwards |
| --- | --- | --- |
| Offline, or GitHub down | Nothing. Only **Check now** reports "Couldn't check for updates." | Unchanged |
| The signed file fails its signature, its format, or the replay check | Nothing on an automatic check, "Couldn't verify the update information." on a manual one. No offer is made. | Unchanged |
| The download fails or you cancel it | "Couldn't download 0.4.0." with **Try again**. A cancel returns to the offer. | Unchanged |
| The archive's size or hash differs from the signed values, an entry breaks the extraction rules, or the bundle is not tubamp 0.4.0 | "The download didn't match the release. Nothing was installed." | Unchanged |
| This copy cannot replace itself | The offer with **Get the installer** | Unchanged |
| The disk is full or a folder is read-only | The reason, with **Get the installer** | Unchanged |
| The host quits or crashes mid-update | Nothing then. The next time the plugin opens, it is either still the old version with the offer back, or already the new one. | Cleaned up by the next `reconcile()` |
| Two hosts update at once | One installs. The other says the update is installed and asks for a restart. | One swap |
| Logic rejects the new version after the restart | Nothing in the plugin, because no tubamp code runs. The README's "If an update goes wrong" section gives the command that restores the previous bundle. | New version live, previous kept |

No in-plugin design can close the last row. Keeping the previous bundle makes the fix one command.

## What lives on disk

| Path | Meaning |
| --- | --- |
| `~/Library/Audio/Plug-Ins/Components/tubamp.component` | The installed version. Its `Info.plist` is the only record of what is installed. |
| `~/Library/Audio/Plug-Ins/.tubamp/previous/tubamp.component` | The highest version below the installed one, kept as found for a manual restore. |
| `~/Library/Audio/Plug-Ins/.tubamp/update.XXXXXX/` | One attempt's staging directory, created by `mkdtemp` with mode 0700 and held by a `flock`. `unpack` writes into its `unpacking/` folder and renames the finished bundle to `update.XXXXXX/tubamp.component`. |
| `~/Library/Application Support/tubamp/update/latest.signed` | The newest signed file this account accepted, byte for byte. |
| `~/Library/Application Support/tubamp/update/prefs.json` | `autoCheck`, `disclosed`, `lastCheckAt`, `announced`. |

`.tubamp` sits outside every folder that Logic scans, and on the same volume as the live bundle. If it is on another volume, the copy counts as one that cannot replace itself, because the swap must never cross volumes.

There is no journal and no "restart pending" flag. Restart pending means that the version on disk differs from the version compiled into the running code. The offer after a restart comes from re-opening `latest.signed` through the same code as a fresh download. The replay floor is the version inside `latest.signed`. A crash therefore cannot leave two records that disagree.

One `flock` on `Application Support/tubamp/update/lock` serializes every short write on the account: prefs, the cache, creating a staging directory, the swap, and `reconcile()`. A holder keeps it for milliseconds. A waiter tries with `LOCK_NB` every 5 ms and gives up after 2 s, or within 5 ms of the `Updater` shutting down, so a host stopped in a debugger while it holds the lock cannot freeze another host. An operation that gives up does nothing, and the next editor open tries again. `prefs.json` and `latest.signed` are written to a `.tmp` beside them and renamed, so each is always a complete old or a complete new file. Each staging directory also has its own `lock`, held for the life of the attempt. A directory whose lock anyone can take, or whose lock file is missing, belongs to a process that died. Any other error opening that lock, such as `EMFILE` or `EACCES`, says nothing about the owner, so `reconcile()` leaves the directory alone.

## How the bundle is replaced

`InstallSite::detect` decides from the running image's own path whether this copy may replace itself. It must be `~/Library/Audio/Plug-Ins/Components/tubamp.component` and a real directory. `Components`, the bundle, `.tubamp` and the support folder must be writable. `.tubamp` must be on the same device as `Components`, and that volume must report `VOL_CAP_INT_RENAME_SWAP`. Anything else makes the copy one that cannot replace itself, with the reason. The tests attach a FAT image to prove the last two checks.

The install then runs one step per type in the chain:

1. `ArchiveSink` writes the download into a file in the staging directory that is unlinked as soon as it is opened. It hashes each chunk as it writes it and refuses the byte past the signed size. `finish` yields a `VerifiedArchive` only when the size and the SHA-256 equal the signed values.
2. `unpack` reads the zip through that same descriptor. It reads the central directory, runs `planExtraction` over every entry before it writes one byte, then creates exactly the planned files with `openat(O_EXCL | O_NOFOLLOW)` under the staging directory's `unpacking/` folder. Each file is inflated to its declared size and must match its CRC-32. Only a complete bundle is renamed to the staging directory's `tubamp.component`. It accepts what `ditto -c -k` writes: stored or deflated entries, with no encryption, no zip64, and one disk.
3. `inspect` checks the Info.plist pins, the offered version in `CFBundleShortVersionString` and in the `AudioComponents` integer, a CPU slice for every type the live bundle has and for the running one, and the code seal.
4. `commit` takes the support lock, re-reads the installed version, and swaps the staged bundle with the live one in one `renamex_np(RENAME_SWAP)`. The old bundle is now in the staging directory.
5. `reconcile()` moves the old bundle to `previous/` and deletes the staging directory.

tubamp unpacks the zip itself, with the SDK's zlib, for two reasons. `ditto -x -k` would be a process spawned from inside the host, and it cannot be held to the entry policy before it writes. The SDK ships no libarchive headers. The ad-hoc seal survives the unpacker: the tests unpack a `ditto`-made zip and check the result with both `SecStaticCodeCheckValidity` and `codesign --verify --deep --strict`.

The seal check uses Security.framework (`codeSealIntact` in `Trust.cpp`) rather than running `codesign`. It spawns no process, reads no output, and keeps every `Sec*` call in one file.

The release archive must contain nothing but `tubamp.component/`. `planExtraction` refuses a `__MACOSX/` entry, and an AppleDouble `._` file inside the bundle breaks the seal. Zip the bundle with `ditto -c -k --norsrc --keepParent`.

## What a crash leaves

Every stage of the install ends in `reconcile()`, and so does every editor that opens. `reconcile()` reads the disk and nothing else, so it does not need to know what an earlier run was doing.

| The process dies during | The disk afterwards | The next `reconcile()` |
| --- | --- | --- |
| A prefs or cache write | A `.tmp` beside the complete old file | Deletes the `.tmp` |
| The download | A staging directory holding only `lock`. The archive has no name, so the kernel frees it | Deletes the staging directory |
| The unpack | A staging directory with a partial bundle in `unpacking/` | Deletes it. Nothing in `unpacking/` is ever kept |
| The inspect, or before the swap | A staging directory with the complete new bundle | Deletes it, because the bundle is not below the installed version |
| The swap | Either the row above or the row below. `renamex_np` is atomic | |
| After the swap | The new bundle live, the old one in the staging directory | Moves the old bundle to `previous/`, replacing the older previous one, then deletes the staging directory |
| `reconcile()` itself | Any mix of the rows above | Does the same steps again |

`reconcile()` keeps in `previous/` the highest pinned bundle below the installed version, as found. It reads the `Info.plist` pins and the version, and it does not check the seal or hash any file. The previous bundle is the user's working plugin as it was, never loaded by tubamp, only offered for a manual restore, so a `.DS_Store` that Finder wrote into it does not cost the user the rollback copy. Every candidate is complete, because a partial unpack never reaches a staging directory's `tubamp.component`. `reconcile()` never touches the live bundle. When the installed version cannot be read, it deletes the staging directories and leaves `previous/` alone. `tubamp_update_tool test` runs each row with a real child process that exits at that point, dies of `SIGXFSZ` inside a file, or gets `SIGKILL`, then reconciles twice and checks that the second run changes nothing.

## How a release is trusted

Each release carries `update-v1.signed`, beside the site's `manifest.json`, which stays unsigned and unchanged:

```text
tubamp-update-v1
sig <16 hex key id> <base64 DER ECDSA P-256 signature>    (1 to 4 lines)
.
{"product":"com.t0audio.tubamp","version":"0.4.0","minimumMacOS":"11.0","archive":{"sha256":"<64 hex>","size":17345678}}
```

Each signature covers `tubamp-update-v1`, a newline, and the payload, so a signature this key made for anything else is not an update. The key id is the first 8 bytes of SHA-256 over the 65-byte public point. The file is at most 16 KiB and the payload at most 8 KiB. The format version is in the file name, so a breaking change ships as `update-v2.signed` beside v1. Inside v1, fields can only be added, and the client ignores fields it does not read. It never reads a URL from the payload.

The trusted keys are the `*.pub.pem` files in `src/update/keys`, compiled into the plugin. Each file must hold exactly one PEM block with an uncompressed P-256 public key. CMake checks every file at configure time with `tools/update/keycheck.cpp`, which runs the plugin's own parser (`parsePublicKeyPem`), so CMake and the plugin cannot disagree about a file. If a file ever failed to parse at run time, the plugin would trust no key at all rather than a smaller set. The release key is held in the `TUBAMP_UPDATE_SIGNING_KEYS` secret of the GitHub Actions environment `release`, which only `main` can use. The recovery key is kept offline. Security.framework verifies the signature.

The release workflow signs the envelope in its `publish` job with `scripts/release/envelope.mjs`, then runs `tubamp_update_tool verify-envelope --archive --expect-keys <committed key count>` on the signed file and the archive before it uploads either. With no key committed, releases ship without the updater. With two or more keys committed, every release ships the plugin, the archive and the envelope together, and a missing secret stops the run. `docs/RELEASING.md` gives the pipeline, the one-time key setup, rotation, and what a lost key means.

## The chain of types

Every step of an install consumes the value the previous step produced. Each value has a private constructor and one producer, so a shorter path that skips a rule does not compile.

```text
bytes -> SignedEnvelope -> VerifiedPayload -> Manifest -> Offer
Offer -> VerifiedArchive -> UnpackedBundle -> InspectedBundle -> Committed
```

- `TrustRoot::verify` is the only producer of `VerifiedPayload`, and `parseManifest` takes nothing else. JSON is never parsed before the signature holds.
- `evaluate` is the only producer of `Offer`. An offer exists only if the version is above the running one and not below the highest seen, the product is pinned, and this Mac's macOS is supported. The size and hash in the offer come from the signed payload alone.
- `Url` can only be built from compiled-in constants and a version. Every hop of a download must pass `hostAllowed`, an exact list of three GitHub hosts in `src/update/detail/Hosts.def`.
- `planExtraction` accepts only regular files and directories under `tubamp.component/`, refuses `..`, absolute paths, symlinks, duplicates, and other roots, and assigns every mode itself.
- `Version` keeps the major in 16 bits and the minor and patch in 8 bits each, so its order is the order of the `AudioComponents` integer that Logic compares.
- The disk stages are move-only, and `commit` takes only an `InspectedBundle`.

`tests/update/must-not-compile` holds one file per rule that breaks it. Building `tubamp_update_tool` compiles each file twice. As written it must compile, and with `-DVIOLATE` it must fail with the expected error.

## The state machine

The machine is two pure functions in `src/update/detail/Machine.h`. `step(Model, Event)` returns the next model and the effects to run. `project(Model)` returns the `View` the editor shows. One model exists per host process, so every plugin instance shares one attempt. The shell runs the effects on a worker thread and feeds their results back as events.

| # | Event | Guard | Result |
| --- | --- | --- | --- |
| T1 | An editor opens | | Reconcile, plus an automatic check if no job runs |
| T2 | `Reconciled` | | Disk, settings, and the cached offer replace the model's |
| T3 | **Check now** | no job | A manual check, phase `checking`. During an automatic check, T19 |
| T4 | Check: not due | | The cached offer stays |
| T5 | Check: verified | | `available`, `manual`, or `idle` |
| T6 | Check: problem | | Reported only if the check was manual. A cached offer stays. |
| T7 | **Update** for version v | no job, copy can update, v is the offer, v not installed | Install, phase `downloading`. During a check, T19 |
| T8 | Progress | downloading | Bytes received |
| T9 | Stage entered | a later stage | `downloading` becomes `installing` |
| T10 | **Cancel** | before the swap | Cancel the job |
| T11 | Job: installed | | `restartPending` |
| T12 | Job: cancelled | | The offer again |
| T13 | Job: failed | | `failed`, with the reason |
| T14 | Automatic checks on or off | | Save, and count the disclosure as seen |
| T15 | Toast for v shown | | Save, so v is never announced again |
| T16 | Disclosure toast shown | | Save |
| T17 | Settings saved | | The saved settings replace the model's |
| T18 | Open release page | | Open the page for the offered, or else the running, version |
| T19 | **Check now** during an automatic check, or **Update** during any check that T7's other guards allow | | The click is kept, phase `checking`. A later click replaces it. When the check finishes, the kept click runs against the model the check produced |

Every other pair of state and event returns the same model with no effects. A build with updates off ignores every event.

## The service

`Updater` (`src/update/Updater.h`) is the shell around the machine. The plugin processor holds it through `juce::SharedResourcePointer`, so every instance and editor in a host shares one. Its surface is `view()`, `perform(command)`, and `watch(listener)`, which returns a handle that unsubscribes when it is destroyed. An editor opening a watch is event T1.

The model lives on the message thread. One worker thread runs the effects in order (reconcile, check, install, save settings) and posts each result back as an event. Listeners run on the message thread, and the editor's handle is destroyed first in its destructor, so no listener runs into a dead editor. A result that arrives after the `Updater` is gone is dropped. Every command republishes the current view, even one the machine ignored, so a stale page corrects itself.

The destructor cancels the running fetch and any wait for the support lock, then joins the worker. The tests measure about 3 ms mid-download, and 5 ms while another process holds the support lock. The system fetcher notices a cancel within its 20 ms poll; `fetch-check --cancel-after` measured 11 to 19 ms on a real GET. A lock wait notices it within 5 ms.

A click on **Update** or **Check now** while a check is still running is not dropped. The machine allows one job at a time, so it keeps the latest click and shows `checking` until the check finishes (T19). Then the click runs as if it had just arrived. A kept **Check now** runs a manual check, which skips the daily claim, so it fetches even when the automatic check was not due. A kept **Update** installs only if the check left the same version on offer. If the check found a newer version, the page shows that offer and installs nothing the user did not see.

## The network

The only network code is `makeSystemFetcher` (`src/update/detail/Fetch.mm`). It makes a plain GET through `NSURLSession` with an ephemeral configuration that has no cookie store, no credential store and no cache. It sends `User-Agent: tubamp/<running version>`, a fixed `Accept-Language: en` and `Accept-Encoding: identity`, and nothing else from the account. It answers every authentication challenge except the TLS server-trust check with a refusal.

Before the first request and before every redirect, it asks `HopPolicy`: https, a host listed exactly in `Hosts.def`, and at most four redirects. A refused hop ends the transfer. It also checks the host of the final response, requires status 200, and refuses a `Content-Length` above the cap. The cap counts bytes as they arrive: 16 KiB for the envelope, the signed size for the archive. The idle timeout, which covers connecting, is 15 s for the envelope and 30 s for the archive. The total timeout is 30 s and 15 minutes.

Body bytes reach the sink on the worker thread, never on the message or audio thread. When the sink refuses a byte, the install asks `ArchiveSink::finish()` for the reason. A drop, a refused redirect or a bad status is `network`; more bytes than signed is `integrity`.

## The editor bridge

`src/update/Bridge.cpp` writes the view as the JSON of `UpdateView` in `ui/src/bridge/types.ts` and parses `updateCommand`'s argument. It refuses any shape that is not exactly a documented command. `WebEditor` adds the `update` field to `getUiState`, emits `updateChanged`, registers `updateCommand`, and supplies the host name ("Logic Pro", "GarageBand", "MainStage", "tubamp", or the host's own description). "Open the release page" opens a URL that C++ builds from the offered version. `openExternal` still accepts only tone3000.com.

`ui/src/bridge/update-contract.json` is the contract as data. `tubamp_update_tool test` checks the C++ against it, and `npm run typecheck` checks the TypeScript against it, so a shape that changes on one side fails a check.

With `TUBAMP_UPDATES=OFF`, none of this is compiled. `UiState` has no `update` field and no `updateCommand` exists. The page then shows no update row.

For development, `-DTUBAMP_UPDATE_DEV_ROOTS=ON` takes the home and the system Components folder from `TUBAMP_UPDATE_HOME` and `TUBAMP_UPDATE_SYSTEM_COMPONENTS`. Without `TUBAMP_UPDATE_HOME`, the updater stays off. A build without the option contains no such `getenv` call. A Release build with `TUBAMP_UPDATES=ON` fails to configure when `TUBAMP_UPDATE_DEV_ROOTS` or `TUBAMP_WEBUI_DEV` is on, so an installer can never carry either. Develop the updater in a Debug build.

## Threat model

The envelope is trusted for what its signature covers: the product, the version, the minimum macOS, and the archive's size and SHA-256. An attacker without a signing key cannot make the plugin download anything else, and cannot make it offer a version below the highest one this account has seen.

The envelope has no expiry. Someone who can serve `releases/latest/download/update-v1.signed`, a network attacker or a stale mirror, can replay an older genuine envelope whose version is at or above the account's highest seen version. The plugin then offers that version, which is a real signed release, and installs it only after the user clicks **Update**. It cannot replay a version below the floor, and it cannot change a byte of the archive. Version 1 accepts this: an expiry field would make every installed copy refuse updates whenever releases pause.

## Tradeoffs

- Each macOS account installs its own copy. That is the price of updates with no password and no installer.
- Someone who controls the repository or its workflow can sign an update. The click requirement and key rotation limit the damage. An attacker with that access can already replace the installer on the site, and a signature made offline for every release would end automatic releases.
- The update archive is universal, 17 MB instead of 8.5 MB, so there is one code path and Rosetta hosts never lose a slice.
- A failed download starts over. Resume would add a partial state on disk.
- The redirect allowlist is exact. If GitHub renames its asset hosts, older copies keep showing the offer and **Get the installer** still works.
- Checks are on by default and disclosed by a one-time toast, not a first-run question. A modal question inside a plugin editor breaks `ui/DESIGN.md`.

## Synthesis decision

Two designs were written from the same research and judged. Version 1 takes the second design's module shape, type chain, single `reconcile()`, cross-process `flock`, fetch interface, and its public interface (`view`, `perform`, `watch`, one native function `updateCommand`, one event `updateChanged`). It takes from the first design the separate signed envelope, so `manifest.json` keeps schema 1 for the site; the trusted keys generated from committed public keys; the disclosure toast; the warning about a second copy in `/Library`; and tests on the real built component with faults injected mid-download and mid-unpack.

It changes the base in five ways. It has no validation step after the swap, no automatic revert, and no **Go back** action, because two facts are not yet established. Nobody knows whether `auval` spawned from Apple's Audio Unit host sees the swapped bundle, or whether Logic accepts a lower version after it scanned a higher one. Staging and the previous bundle live in `~/Library/Audio/Plug-Ins/.tubamp`, never inside `Components`. The host, owner, and repository are compiled-in constants with no override. A build with updates on and fewer than two trusted keys, or with a test key, fails to configure. Settings, the envelope cache, and the bundle operations are separate files.

## Checked by hand in Logic before the feature is switched on

1. After the swap in a live session, a second instance inserted in the same session runs the old version without a crash.
2. After a restart, Logic rescans the higher version without being asked and lists exactly one tubamp.
3. The per-user package installs with no password and creates `Components` on a clean account.

## Where the code is

`src/update/README.md` maps the modules and explains the disk and service tests. `tubamp_update_tool test` runs every table without the network. `tubamp_update_tool fetch-check` makes one real GET through the system fetcher and prints each hop. `tubamp_update_tool verify-envelope` opens a signed file exactly as the plugin does. With `--archive` and `--installed`, it also runs the plugin's download, unpack and inspect stages on the zip in a temporary home, so the release pipeline can prove the shipped client accepts what it publishes.
