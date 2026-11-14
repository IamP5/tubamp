# Releasing tubamp

A release is a GitHub Release with a `vX.Y.Z` tag and four assets:

- `tubamp-X.Y.Z-macos-arm64.pkg`, the installer for Apple Silicon Macs.
- `tubamp-X.Y.Z-macos-x86_64.pkg`, the installer for Intel Macs.
- `SHA256SUMS`, the checksum of each installer, in `shasum -a 256 -c` format.
- `manifest.json`, which the landing site reads.

Nobody cuts a release by hand. Every push to `main` runs `.github/workflows/release.yml`, and that workflow decides from the commit messages, and from the files each commit changed, whether a release is due.

## How a push to main becomes a release

`release.yml` runs three jobs, one run at a time, and never cancels a run that has started.

1. `plan` runs the release-script tests, then `scripts/release/plan.mjs`. The planner reads every commit since the highest `vX.Y.Z` tag that `HEAD` contains and picks the next version. If no commit that changes the installer asks for a release, the run stops here.
2. `build` calls `.github/workflows/installers.yml`, which builds both installers on a `macos-15` (Apple Silicon) runner with `scripts/release/build-installer.sh`. The x86_64 installer is cross-compiled. For each CPU type the script builds `tubamp_AU` and `tubamp_smoke` in Release with the version baked in, checks the CPU type with `lipo -archs`, runs `tubamp_smoke` against NAM's `example_models/A2.nam` (under Rosetta for x86_64), ad-hoc signs the component, and packages it. The arm64 job then installs its package on the runner and runs `auval -v aufx Tamp Tuba`. The smoke test has a timing bound. Under Rosetta on a loaded runner it can fail without a real fault, so rerun the job once before you look for a cause.
3. `publish` writes the release notes (`scripts/release/notes.mjs`), `SHA256SUMS` and `manifest.json` (`scripts/release/manifest.mjs`), and runs `scripts/release/publish.mjs`. That script creates the release as a draft, uploads the installers and `SHA256SUMS`, uploads `manifest.json` last, and only then publishes the release and marks it latest. The job ends by fetching `releases/latest/download/manifest.json` and checking that it names the new version.

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
- the release notes, `SHA256SUMS`, and `manifest.json`, uploaded as the `release-preview` artifact, and `publish.mjs --dry-run` when either outcome would release. The installers and notes follow the squash outcome when it releases, and the merge-commit outcome otherwise;
- the site checks: `npm ci`, `npm run lint`, `npm run typecheck`, `npm test`, and `npm run build` in `site/`.

## Run a release again

Every step of `release.yml` reads the current state and does only what is missing, so running it again is safe. A draft left for another commit is deleted and created again for the commit being released.

- To retry a failed run, use **Re-run failed jobs** on the run.
- To finish a release for a commit that is already tagged, run `release.yml` from the **Actions** tab (**Run workflow** on `main`). When `HEAD` carries a `vX.Y.Z` tag, `plan` reports that version again, and `publish` completes the release.

What a second run does depends on where the first one stopped:

| First run stopped | Second run |
| --- | --- |
| Before the draft existed | Creates the draft and continues. |
| With a draft for the same commit, with some or no assets | Uploads all four assets with `--clobber`, `manifest.json` last, then publishes. |
| With a draft for another commit, because a later push planned the same version | Deletes that draft, creates a new one for this commit, uploads all four assets, then publishes. A draft has no tag yet, so nothing public changes. |
| After publishing, with all four assets | Changes nothing. |
| With a published release missing an asset | Uploads all four assets again, so `SHA256SUMS` matches the installers. |

A new release stays a draft until `manifest.json` is uploaded, so `releases/latest` never points at it before all four assets are there. The exception is the repair of a release that is already published and missing an asset. That run uploads the assets to the live release, so for a few seconds `releases/latest` can serve an installer that does not match `SHA256SUMS` or `manifest.json` yet.

Two cases stop the run on purpose:

- The planned tag already exists on another commit. `plan.mjs` and `publish.mjs` both refuse to publish over it, published release or not. Find out why the tag is there before you move or delete it.
- More than one release carries the planned tag. Delete the extra drafts by hand.

If GitHub drops a pending run because a newer push queued behind it, nothing is lost. The newer run plans from the last tag and sees the dropped run's commits.

## Cut 1.0

Add a `Release-As` footer to any Conventional Commit:

```text
chore: release 1.0.0

Release-As: 1.0.0
```

`Release-As: X.Y.Z` releases that version even when the commit type would release nothing, and even when the commit changes no file. The version must be above the last release, or `plan` fails. If more than one commit carries the footer, the newest one wins. A footer that is not `X.Y.Z`, such as `Release-As: 1.0`, prints a warning and is ignored. From 1.0 on, breaking changes bump the major version.

## Build an installer locally

```sh
scripts/release/build-installer.sh arm64 0.2.0
scripts/release/build-installer.sh x86_64 0.2.0   # needs Rosetta for the smoke test
```

The script builds in `build/rel-<arch>` with `-DTUBAMP_COPY_AFTER_BUILD=OFF`, so it never touches `~/Library/Audio/Plug-Ins/Components`. It writes `dist/tubamp-<version>-macos-<arch>.pkg` and a `.build.json` file beside it for `manifest.mjs`. Extra arguments go to CMake. To reuse dependency sources that are already on disk, pass `-DFETCHCONTENT_SOURCE_DIR_JUCE=<dir>` and `-DFETCHCONTENT_SOURCE_DIR_NEURALAMPMODELERCORE=<dir>`.

The package installs `tubamp.component` into `/Library/Audio/Plug-Ins/Components`. Its component property list sets `BundleIsRelocatable` to false, so Installer does not write over another copy of the bundle that it finds on disk, such as one in a build tree.

The package does not install over a newer version. If `/Library/Audio/Plug-Ins/Components` already holds a higher version, macOS treats the package as a downgrade and installs nothing. To go back to an older version, delete `tubamp.component` from that folder first.

The release scripts are tested with `node --test 'scripts/release/test/*.test.mjs'`. The tests need Node 22 and git, and no npm packages.

## The installers are not signed yet

The component is ad-hoc signed (`codesign --sign -`) and the package is not signed. When a user opens the package, macOS blocks it once. They allow it in **System Settings** > **Privacy & Security** > **Open Anyway**. The release notes and `"signed": false` in the manifest say so.

Signing needs an Apple Developer ID, which this project does not have yet. With one, the follow-up is:

1. Store the Developer ID Application and Developer ID Installer certificates, and an App Store Connect API key, as repository secrets.
2. In `build-installer.sh`, sign the component with the Developer ID Application identity and the hardened runtime instead of `--sign -`, and pass `--sign "Developer ID Installer: ..."` to `pkgbuild`.
3. Notarize each package with `xcrun notarytool submit --wait` and staple it with `xcrun stapler staple`.
4. Set `signed` to true in `manifest.mjs`.
