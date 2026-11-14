# In-plugin updates

tubamp tells you when a newer release exists and, after one click, installs it while the host keeps running. The new version takes effect when the host next starts. This page explains how version 1 works and why. It stays off until the maintainer creates the signing keys (`src/update/keys/README.md`).

## What you see

**First install.** The installer offers "Install for me only" and asks for no password. The component goes to `~/Library/Audio/Plug-Ins/Components/tubamp.component`, where tubamp can later replace itself without privileges.

**The first time you open the plugin**, a toast says that tubamp checks GitHub once a day for a new version. It has a **Turn off** action, and it does not come back. The same switch lives in Settings.

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
| Logic rejects the new version after the restart | Nothing in the plugin, because no tubamp code runs. The README gives the one command that restores the previous bundle. | New version live, previous kept |

No in-plugin design can close the last row. Keeping the previous bundle makes the fix one command.

## What lives on disk

| Path | Meaning |
| --- | --- |
| `~/Library/Audio/Plug-Ins/Components/tubamp.component` | The installed version. Its `Info.plist` is the only record of what is installed. |
| `~/Library/Audio/Plug-Ins/.tubamp/previous/tubamp.component` | The highest sealed version below the installed one, kept for a manual restore. |
| `~/Library/Audio/Plug-Ins/.tubamp/update.XXXXXX/` | One attempt's staging directory, created by `mkdtemp` with mode 0700 and held by a `flock`. |
| `~/Library/Application Support/tubamp/update/latest.signed` | The newest signed file this account accepted, byte for byte. |
| `~/Library/Application Support/tubamp/update/prefs.json` | `autoCheck`, `disclosed`, `lastCheckAt`, `announced`. |

`.tubamp` sits outside every folder that Logic scans, and on the same volume as the live bundle. If it is on another volume, the copy counts as one that cannot replace itself, because the swap must never cross volumes.

There is no journal and no "restart pending" flag. Restart pending means that the version on disk differs from the version compiled into the running code. The offer after a restart comes from re-opening `latest.signed` through the same code as a fresh download. The replay floor is the version inside `latest.signed`. A crash therefore cannot leave two records that disagree.

## How a release is trusted

Each release carries `update-v1.signed`, beside the site's `manifest.json`, which stays unsigned and unchanged:

```text
tubamp-update-v1
sig <16 hex key id> <base64 DER ECDSA P-256 signature>    (1 to 4 lines)
.
{"product":"com.t0audio.tubamp","version":"0.4.0","minimumMacOS":"11.0","archive":{"sha256":"<64 hex>","size":17345678}}
```

Each signature covers `tubamp-update-v1`, a newline, and the payload, so a signature this key made for anything else is not an update. The key id is the first 8 bytes of SHA-256 over the 65-byte public point. The file is at most 16 KiB and the payload at most 8 KiB. The format version is in the file name, so a breaking change ships as `update-v2.signed` beside v1. Inside v1, fields can only be added, and the client ignores fields it does not read. It never reads a URL from the payload.

The trusted keys are the `*.pub.pem` files in `src/update/keys`, compiled into the plugin. The release key is held in a GitHub Actions environment secret that only `main` can use. The recovery key is kept offline. Security.framework verifies the signature.

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
| T3 | **Check now** | no job | A manual check, phase `checking` |
| T4 | Check: not due | | The cached offer stays |
| T5 | Check: verified | | `available`, `manual`, or `idle` |
| T6 | Check: problem | | Reported only if the check was manual. A cached offer stays. |
| T7 | **Update** for version v | no job, copy can update, v is the offer, v not installed | Install, phase `downloading` |
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

Every other pair of state and event returns the same model with no effects. A build with updates off ignores every event.

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

`src/update/README.md` maps the modules. `tubamp_update_tool test` runs every table. `tubamp_update_tool verify-envelope` opens a signed file exactly as the plugin does, so the release pipeline can prove the shipped client accepts what it publishes.
