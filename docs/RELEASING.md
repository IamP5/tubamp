# Releasing tubamp

A release is a GitHub Release with a `vX.Y.Z` tag and these assets:

- `tubamp-X.Y.Z-macos-arm64.pkg`, the installer for Apple Silicon Macs.
- `tubamp-X.Y.Z-macos-x86_64.pkg`, the installer for Intel Macs.
- `SHA256SUMS`, the checksum of each installer, in `shasum -a 256 -c` format.
- `manifest.json`, which the landing site reads.
- With updates on, `tubamp-X.Y.Z-macos-universal.zip`, the bundle the in-plugin updater downloads.
- With updates on, `update-v1.signed`, the signed file that tells the updater about the release.

Both installers install for the current user only. They put `tubamp.component` in `~/Library/Audio/Plug-Ins/Components` and ask for no password.

Updates stay off until the maintainer has created the signing keys, as [Turn on in-plugin updates](#turn-on-in-plugin-updates) describes. Until then a release has the first four assets only, and the plugin is built without the updater.

Nobody cuts a release by hand. Every push to `main` runs `.github/workflows/release.yml`, and that workflow decides from the commit messages, and from the files each commit changed, whether a release is due.

## How a push to main becomes a release

`release.yml` runs four jobs, one run at a time, and never cancels a run that has started.

1. `signing-key` runs in the `release` environment and reports only whether the environment holds the `TUBAMP_UPDATE_SIGNING_KEYS` secret. It checks out no code.
2. `plan` runs the release-script tests, then `scripts/release/plan.mjs`. The planner reads every commit since the highest `vX.Y.Z` tag that `HEAD` contains and picks the next version. If no commit that changes the installer asks for a release, the run stops here. The planner also decides, once for the whole run, whether updates are on. They are on only when `HEAD` has at least two `*.pub.pem` files committed in `src/update/keys` and the secret exists. The run summary says which, and why.
3. `build` calls `.github/workflows/installers.yml`, which builds both installers on a `macos-15` (Apple Silicon) runner with `scripts/release/build-installer.sh`. The x86_64 installer is cross-compiled. For each CPU type the script builds `tubamp_AU` and `tubamp_smoke` in Release with the version baked in and `TUBAMP_UPDATES` set from the plan, checks the CPU type with `lipo -archs`, runs `tubamp_smoke` against NAM's `example_models/A2.nam` (under Rosetta for x86_64), ad-hoc signs the component, and packages it. The arm64 job then installs its package for the runner's user with `installer -target CurrentUserHomeDirectory` and no `sudo`, checks that the component is in the runner's `~/Library/Audio/Plug-Ins/Components` and that nothing was written to `/Library/Audio/Plug-Ins/Components`, and runs `auval -v aufx Tamp Tuba`. The smoke test has one bound measured in wall-clock time. Hosted runners deliver timers late, so the workflow sets `TUBAMP_TEST_WALLCLOCK_SLACK=8`, which multiplies that bound. A local run without the variable uses the real bound. With updates on, a third job, `update-archive`, builds the universal archive, as [The update archive and the envelope](#the-update-archive-and-the-envelope) describes.
4. `publish` runs on `macos-15` in the `release` environment. It writes the release notes (`scripts/release/notes.mjs`), `SHA256SUMS` and `manifest.json` (`scripts/release/manifest.mjs`). With updates on, it signs `update-v1.signed` and checks it with the plugin's own code. Then it runs `scripts/release/publish.mjs`. That script creates the release as a draft and uploads the assets in this order: the installers, the archive and `SHA256SUMS`; then `update-v1.signed`; then `manifest.json`. Only then does it publish the release and mark it latest. The job ends by fetching `releases/latest/download/manifest.json` and checking that it names the new version. With updates on, it also checks that `releases/latest/download/update-v1.signed` is byte for byte the file it signed.

The version lives only in git tags. No file in the repo carries it, and the workflow pushes no commit back to `main`. `CMakeLists.txt` takes the version as `-DTUBAMP_VERSION=X.Y.Z` and defaults to `0.1.0` for local builds.

## How the site picks up a release

The site in `site/` reads `https://github.com/IamP5/tubamp/releases/latest/download/manifest.json` at runtime and revalidates it every few minutes. A new release therefore shows up on the site without a site deploy. Vercel deploys the site from `site/` through its Git integration, and the release workflow never deploys it. The Vercel setup is documented in `site/README.md`.

`manifest.json` has this shape:

```json
{
	"schema": 1,
	"repo": "IamP5/tubamp",
	"version": "0.2.0",
	"tag": "v0.2.0",
	"publishedAt": "2026-11-14T16:58:00Z",
	"url": "https://github.com/IamP5/tubamp/releases/tag/v0.2.0",
	"notes": "markdown release notes",
	"downloads": {
		"arm64": { "name": "tubamp-0.2.0-macos-arm64.pkg", "url": "https://github.com/IamP5/tubamp/releases/download/v0.2.0/tubamp-0.2.0-macos-arm64.pkg", "size": 12345678, "sha256": "hex" },
		"x86_64": { "name": "tubamp-0.2.0-macos-x86_64.pkg", "url": "https://github.com/IamP5/tubamp/releases/download/v0.2.0/tubamp-0.2.0-macos-x86_64.pkg", "size": 12345678, "sha256": "hex" }
	},
	"minimumMacOS": "11.0",
	"signed": false
}
```

`publishedAt` is the time `manifest.mjs` ran, a few seconds before the release is published. `minimumMacOS` is the `minos` that `vtool` reads from the built binary, which comes from `CMAKE_OSX_DEPLOYMENT_TARGET`. Both downloads are always present. A missing installer fails the publish.

## Write commit messages that release

The planner reads [Conventional Commits](https://www.conventionalcommits.org/en/v1.0.0/). Only these types count, and only in lowercase:

| Type | Release |
| --- | --- |
| `feat` | minor |
| `fix`, `perf` | patch |
| `docs`, `ci`, `chore`, `refactor`, `test`, `build`, `style` | none |

A breaking change is a `!` after the type or scope, or a `BREAKING CHANGE:` footer. While the major version is 0, a breaking change bumps the minor version. From 1.0 on, it bumps the major version. Subjects that are not Conventional Commits, such as the older `ui: ...` and `dsp: ...` subjects in this repo, release nothing.

The first release is `0.1.0`, as soon as one releasable commit lands on `main`. After that, the highest bump among the commits since the last tag wins.

### Only commits that change the installer count

A commit counts only if it changes at least one path outside these:

- `site/`, `docs/`, `.github/`, `.audit/`, and `scripts/release/`;
- any `*.md` file, at the top level or in a subdirectory;
- `.gitignore` files.

The planner decides by the files a commit changed, not by its scope. `fix(site): typo` that changes only `site/` releases nothing, and `fix(site): ...` that also changes `CMakeLists.txt` releases a patch. A squash or merge commit that changes both `site/` and the plugin counts. The release notes list exactly the commits that counted. The list is `NOT_SHIPPED` in `scripts/release/plan.mjs`.

Examples:

```text
feat(reverb): add a plate type                    0.4.2 -> 0.5.0
fix: stop the gate chattering at 44.1 kHz         0.4.2 -> 0.4.3
perf(nam): skip the resampler at the native rate  0.4.2 -> 0.4.3
feat(presets)!: store chain order as a list       0.4.2 -> 0.5.0, and 1.4.2 -> 2.0.0
docs: explain split paths                         no release
```

A breaking change written as a footer:

```text
fix(presets): reject presets without a chain order

BREAKING CHANGE: presets saved before 0.3 must be saved again.
```

The release notes group the commits under Breaking changes, Features, Fixes, and Performance, with a link to each commit.

### Pull requests

What lands on `main` depends on the merge button, and so does the release:

| Merge button | What `main` gets | Outcome in the `pr.yml` summary |
| --- | --- | --- |
| **Create a merge commit** | Every commit in the PR, plus a merge commit ("Merge pull request #7 ...") that releases nothing. | "Merged as a merge commit" |
| **Rebase and merge** | Every commit in the PR, without the merge commit. | "Merged as a merge commit" |
| **Squash and merge** | One commit. With two or more commits, its subject is the PR title. With one commit, it keeps that commit's subject. Its body holds the PR's commit messages, so their `BREAKING CHANGE:` and `Release-As:` footers still count. It changes the union of the PR's files. | "Squashed with this title" |

`.github/workflows/pr-title.yml` checks that the PR title is a Conventional Commit, because a squash merge of two or more commits uses the title as the commit subject on `main`. A `chore:` title over `feat:` commits releases nothing when squashed.

`.github/workflows/pr.yml` runs the same steps as a release without publishing:

- the release-script tests and `plan.mjs`, run twice: once for a merge commit, and once with `--squash-title` for a squash. Both results appear in the job summary. `pr.yml` does not run again when you edit the title, so push a commit or rerun the workflow to see the new squash outcome;
- both installer builds, with the smoke tests and `auval`, uploaded as the `installer-arm64` and `installer-x86_64` artifacts;
- the universal update archive, always, even while updates are off, checked by `tubamp_update_tool` as in a release;
- the release notes, `SHA256SUMS`, and `manifest.json`, uploaded as the `release-preview` artifact, and `publish.mjs --dry-run` when either outcome would release. The installers and notes follow the squash outcome when it releases, and the merge-commit outcome otherwise;
- an `update-v1.signed` signed with a key that the job generates and throws away, checked with `tubamp_update_tool verify-envelope` against the archive. A pull request never sees the release key, so a pull request from a fork runs every step too. The planner decides updates from the committed keys alone and labels the result as a dry run;
- the site checks: `npm ci`, `npm run lint`, `npm run typecheck`, `npm test`, and `npm run build` in `site/`.

## Run a release again

Every step of `release.yml` reads the current state and does only what is missing, so running it again is safe. A draft left for another commit is deleted and created again for the commit being released.

- To retry a failed run, use **Re-run failed jobs** on the run.
- To finish a release for a commit that is already tagged, run `release.yml` from the **Actions** tab (**Run workflow** on `main`). When `HEAD` carries a `vX.Y.Z` tag, `plan` reports that version again, and `publish` completes the release.

What a second run does depends on where the first one stopped:

| First run stopped | Second run |
| --- | --- |
| Before the draft existed | Creates the draft and continues. |
| With a draft for the same commit, with some or no assets | Uploads every asset with `--clobber`, in the publish order, then publishes. |
| With a draft for another commit, because a later push planned the same version | Deletes that draft, creates a new one for this commit, uploads every asset, then publishes. A draft has no tag yet, so nothing public changes. |
| With a draft that holds an asset this run does not publish, such as `update-v1.signed` from a run with updates on when this run has updates off | Deletes that draft and starts again, so the release never mixes the two settings. |
| After publishing, with every asset | Changes nothing. The envelope signature is deterministic, so a rerun signs the same bytes. |
| With a published release missing an asset | Uploads every asset again, so `SHA256SUMS` and `update-v1.signed` match what is uploaded. |

A new release stays a draft until `manifest.json` is uploaded, so `releases/latest` never points at it before every asset is there. The exception is the repair of a release that is already published and missing an asset. That run uploads the assets to the live release, so for a few seconds `releases/latest` can serve an installer that does not match `SHA256SUMS` or `manifest.json` yet.

Two cases stop the run on purpose:

- The planned tag already exists on another commit. `plan.mjs` and `publish.mjs` both refuse to publish over it, published release or not. Find out why the tag is there before you move or delete it.
- More than one release carries the planned tag. Delete the extra drafts by hand.
- `publish.mjs` refuses a half-armed release before it calls GitHub. That covers an installer whose `.build.json` says it was built with the other updates setting, an update asset in a run with updates off, a missing one in a run with updates on, or an `update-v1.signed` that names another version or archive. It also stops when a published release carries `update-v1.signed` and this run has updates off. Removing the envelope from a live release would leave installed copies without the release they were told about, so fix forward with a higher version instead.

If GitHub drops a pending run because a newer push queued behind it, nothing is lost. The newer run plans from the last tag and sees the dropped run's commits.

## Cut 1.0

Add a `Release-As` footer to any Conventional Commit:

```text
chore: release 1.0.0

Release-As: 1.0.0
```

`Release-As: X.Y.Z` releases that version even when the commit type would release nothing, and even when the commit changes no file. The version must be above the last release, or `plan` fails. If more than one commit carries the footer, the newest one wins. A footer that is not `X.Y.Z`, such as `Release-As: 1.0`, prints a warning and is ignored. From 1.0 on, breaking changes bump the major version.

## The update archive and the envelope

The in-plugin updater (`docs/UPDATES.md`) reads two assets, and their names are compiled into every copy that is already installed:

- `releases/latest/download/update-v1.signed`, at most 16 KiB.
- `releases/download/vX.Y.Z/tubamp-X.Y.Z-macos-universal.zip`.

Never rename them, never re-tag a version, and never delete a release that was latest. Installed copies cache the envelope, and an envelope whose archive is gone offers an update that cannot download.

The `update-archive` job in `installers.yml` runs after both installer jobs, which hand it their signed components:

1. `scripts/release/build-update-archive.sh` checks that the two bundles hold the same files and only regular files and directories. It merges each Mach-O file with `lipo -create` and requires every other file to be byte-identical, apart from `Contents/_CodeSignature`. It signs the merged bundle ad hoc, checks it with `codesign --verify --deep --strict`, and zips it with `ditto -c -k --norsrc --keepParent`. It refuses a zip with an entry outside `tubamp.component/`, a `__MACOSX` entry, or a `._` file, because the client refuses them.
2. The job builds `tubamp_update_tool` from the same commit, with the keys committed in `src/update/keys`.
3. It signs a test envelope with a key it generates and deletes, and runs `tubamp_update_tool verify-envelope --archive`. That runs the plugin's own download, unpack and inspect code on the zip, once with the arm64 component as the installed copy and once with the x86_64 one. It then flips one byte of the zip and requires the same check to refuse it.

`publish` then writes the real envelope with `scripts/release/envelope.mjs`. The payload holds the version, the minimum macOS from the installers' `.build.json`, and the archive's size and SHA-256:

```text
tubamp-update-v1
sig <16 hex key id> <base64 DER ECDSA P-256 signature>
.
{"product":"com.t0audio.tubamp","version":"0.4.0","minimumMacOS":"11.0","archive":{"sha256":"<64 hex>","size":17345678}}
```

Each signature covers `tubamp-update-v1`, a newline, and the payload. The script signs with `openssl dgst -sha256 -sign <key> -sigopt nonce-type:1`, the deterministic nonce of RFC 6979, which needs OpenSSL 3.2 or later. The publish job uses Homebrew's `openssl@3`, because the system `openssl` on macOS is LibreSSL and Ubuntu's OpenSSL 3.0 has no `nonce-type`. The script signs twice and fails if the two signatures differ. The key comes from the `TUBAMP_UPDATE_SIGNING_KEYS` environment variable and is written only to a 0600 temporary file that the script deletes. Before the upload, the job runs `tubamp_update_tool verify-envelope` with the keys compiled into the tool, which are the keys the plugin in this release trusts, and with `--archive` on the zip it is about to upload.

## Turn on in-plugin updates

Updates turn on by themselves at the first release after two public keys are committed and the secret exists. Do these steps once, in this order.

1. Check by hand in Logic the three behaviors that `docs/UPDATES.md` lists under "Checked by hand in Logic before the feature is switched on". None of them needs a key. For the first two, swap a higher-version `tubamp.component` into `~/Library/Audio/Plug-Ins/Components` yourself while Logic runs, the way the updater does. For the third, install a per-user package from a pull request's `installer-arm64` artifact on a clean macOS account. If one fails, stop here. Updates stay off, and releases go on as before.
2. On a machine you trust, generate the release key and the recovery key, and write their public halves into the repository:

	```sh
	openssl ecparam -name prime256v1 -genkey -noout -out release.pem
	openssl ec -in release.pem -pubout -out src/update/keys/release.pub.pem
	openssl ecparam -name prime256v1 -genkey -noout -out recovery.pem
	openssl ec -in recovery.pem -pubout -out src/update/keys/recovery.pub.pem
	```

3. In the repository's **Settings** > **Environments**, open `release`, or create it with **New environment** if no release run has created it yet. Under **Deployment branches and tags**, choose **Selected branches and tags** and add `main`. Add no required reviewer, because a reviewer would make every release wait for a person.
4. In the same environment, add the secret `TUBAMP_UPDATE_SIGNING_KEYS` with the full contents of `release.pem`.
5. Encrypt both private keys. Keep `recovery.pem` in two offline places and never on GitHub or a CI runner. Keep one encrypted copy of `release.pem`. Delete the plaintext files.
6. Commit `src/update/keys/release.pub.pem` and `src/update/keys/recovery.pub.pem` through a pull request. Its `pr.yml` summary says "Updates **on**". CMake refuses the keys if a file holds a private key or a test key.
7. Merge. The next release builds the plugin with `TUBAMP_UPDATES=ON` and publishes the archive and `update-v1.signed`. Its run summary says "Updates **on**".

If the secret is missing when keys are committed, or the other way round, the release goes out with updates off and the summary says why. A release is never built with the updater and published without its envelope, or the reverse.

### Rotate the release key

1. Generate a new release key and commit its public key beside the other two. Release as usual. Copies that take this release trust the new key.
2. When few users still run a version from before step 1, replace the secret's contents with the new private key. From then on, copies older than step 1 cannot verify an update and must reinstall from the site.
3. Delete the old public key from `src/update/keys` in a later release.

The secret may hold up to four PEM keys. Each one adds a `sig` line, so during step 2 you can sign with the old and the new key at once.

### Lose a key

- **The release key is lost.** The secret and your copy are both gone. Put `recovery.pem` into the secret. Every installed copy already trusts it. Then generate a new recovery key, commit its public key, and delete the lost key's public file in the same pull request.
- **The release key leaked.** Put `recovery.pem` into the secret at once, then open the same pull request. Copies that never update keep trusting the leaked key. To use it, an attacker also needs to publish a release in this repository, so rotate your GitHub credentials too.
- **The recovery key is lost.** Generate a new one and commit its public key in a normal release.
- **Both keys are lost.** The update channel is dead. Commit two new public keys and put the new release key in the secret. Every installed copy needs one manual install from the site, and updates work again from that version on.

## Build an installer locally

```sh
scripts/release/build-installer.sh arm64 0.2.0
scripts/release/build-installer.sh x86_64 0.2.0   # needs Rosetta for the smoke test
```

The script builds in `build/rel-<arch>` with `-DTUBAMP_COPY_AFTER_BUILD=OFF`, so it never touches `~/Library/Audio/Plug-Ins/Components`. It writes `dist/tubamp-<version>-macos-<arch>.pkg` and a `.build.json` file beside it for `manifest.mjs`. The `.build.json` also records whether the build has the updater (`"updates": true`), which `publish.mjs` checks. Extra arguments go to CMake. Pass `-DTUBAMP_UPDATES=ON` to build with the updater, which needs two keys in `src/update/keys`. To reuse dependency sources that are already on disk, pass `-DFETCHCONTENT_SOURCE_DIR_JUCE=<dir>` and `-DFETCHCONTENT_SOURCE_DIR_NEURALAMPMODELERCORE=<dir>`.

The package is a product archive whose distribution enables only the current user's home (`installer -dominfo` prints `CurrentUserHomeDirectory`, and the script checks that). Installer offers "Install for me only", runs as the user, asks for no password, and puts `tubamp.component` in `~/Library/Audio/Plug-Ins/Components`. The package has no scripts. Its component property list sets `BundleIsRelocatable` to false, so Installer does not write over another copy of the bundle that it finds on disk, such as one in a build tree.

The package does not install over a newer version. If `~/Library/Audio/Plug-Ins/Components` already holds a higher version, macOS treats the package as a downgrade and installs nothing. To go back to an older version, delete `tubamp.component` from that folder first.

A copy that an older, system-wide package put in `/Library/Audio/Plug-Ins/Components` stays there, because a per-user package cannot remove it. Two copies of one Audio Unit make it unclear which one a host loads, so delete the one in `/Library` (Finder asks for your password once).

To build the update archive from two built components:

```sh
scripts/release/build-update-archive.sh 0.2.0 build/rel-arm64/tubamp_artefacts/Release/AU/tubamp.component \
	build/rel-x86_64/tubamp_artefacts/Release/AU/tubamp.component
```

The release scripts are tested with `node --test 'scripts/release/test/*.test.mjs'`. The tests need Node 22 and git, and no npm packages.

## The installers are not signed yet

The component is ad-hoc signed (`codesign --sign -`) and the package is not signed. When a user opens the package, macOS blocks it once. They allow it in **System Settings** > **Privacy & Security** > **Open Anyway**. The release notes and `"signed": false` in the manifest say so.

Signing needs an Apple Developer ID, which this project does not have yet. With one, the follow-up is:

1. Store the Developer ID Application and Developer ID Installer certificates, and an App Store Connect API key, as repository secrets.
2. In `build-installer.sh` and `build-update-archive.sh`, sign the component with the Developer ID Application identity and the hardened runtime instead of `--sign -`, and pass `--sign "Developer ID Installer: ..."` to `productbuild`.
3. Notarize each package with `xcrun notarytool submit --wait` and staple it with `xcrun stapler staple`.
4. Set `signed` to true in `manifest.mjs`.
