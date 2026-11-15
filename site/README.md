# tubamp site

The landing page for the tubamp plugin. Next.js 16 (App Router, Cache
Components), React 19, Tailwind 4, GSAP with ScrollTrigger, Lenis, and Motion.
Vercel hosts it.

`DESIGN.md` is the design contract: concept, voice, type, colour, motion rules.
Read it before you change how the page looks or moves.

## Run it

Node 22.18 or later: the tests rely on Node's built-in type stripping.

```sh
cd site
npm install
npm run dev          # http://localhost:3000
```

Checks, the same ones the pull request workflow runs:

```sh
npm run lint
npm run typecheck    # generates route types, then tsc
npm test             # node --test on src/**/*.test.ts
npm run build
```

Tests run on Node's own test runner with type stripping, so a test imports its
subject with the `.ts` extension and without the `@/` alias.

## How the page learns about a release

The page never needs a deploy when the plugin ships a new version.

1. The release workflow publishes a GitHub Release with the two installers and
   a `manifest.json` asset (`docs/RELEASING.md` in the repository root).
2. `getLatestRelease()` in `src/lib/release.ts` fetches
   `https://github.com/IamP5/tubamp/releases/latest/download/manifest.json`.
   The result is cached and refreshed in the background once it is five
   minutes old. Only a 404 means "no release yet". Any other failure (network
   error, 5xx, 429, a body that is not a manifest) throws, so it never replaces
   a good cached release, and a cold cache falls back to the "no release" state.
3. `parseManifest()` in `src/lib/release-schema.ts` is the only place manifest
   JSON becomes a `Release`. It rejects a manifest unless each download's URL is
   exactly `https://github.com/IamP5/tubamp/releases/download/<tag>/<name>`.
4. The download field links to `/download/arm64` and `/download/x86_64`
   (`src/app/download/[arch]/route.ts`), which redirect to the latest
   installer, or to the GitHub releases page when no release exists yet. The
   "Download for Mac" buttons in the hero and nav link to an installer only
   when the browser's architecture was detected; otherwise they scroll to
   `#download`, because an installer for the wrong architecture installs a
   plugin that never loads. On a device that is plainly not a Mac
   (`notAMac()` in `src/lib/platform.ts`) those two buttons share or copy the
   page's address instead.

A new release reaches the page within about five minutes of the next request
after the cache goes stale: with no visitors, nothing refreshes it. To see a
new release sooner, redeploy from the Vercel dashboard.

## Languages

The page is in English (`/`) and Brazilian Portuguese (`/pt-br`).

- `src/i18n/en.ts` and `src/i18n/pt-br.ts` hold every sentence. They have the
  same shape, and `src/i18n/i18n.test.ts` fails when one has a string, or a
  `{slot}`, the other lacks. Server Components read them with
  `getDictionary()`; Client Components with `useDict()`.
- Both pages are prerendered from `src/app/[lang]/`. English is rendered as
  `/en` and `src/proxy.ts` serves it at `/`; a request for `/en` redirects
  to `/`.
- `src/proxy.ts` decides what `/` shows (`pickLocale()` in
  `src/i18n/config.ts`). A visitor whose request comes from Brazil
  (`x-vercel-ip-country: BR`, a header Vercel sets) is redirected to `/pt-br`.
  Everyone else gets English. The header is absent in local development; to
  try it, `curl -I -H "x-vercel-ip-country: BR" http://localhost:3000/`.
- The footer links to the other language and sets a `locale` cookie. From then
  on `/` shows that language, whatever the country.
- `/pt-br` always shows Portuguese and never redirects, so a shared link opens
  in the language it was shared in.
- The in-page demo and the clips show the plugin, which is in English only.

To add a language: add it to `LOCALES`, `LANG_TAG`, `OG_LOCALE` and
`LOCALE_NAME` in `src/i18n/config.ts`, write its dictionary, register it in
`src/i18n/dictionaries.ts`, and map its countries in `BY_COUNTRY`.

## Media

The page uses two kinds of footage.

- `public/media/ui/` holds 14 clips recorded from the real plugin editor. Each
  is an `.mp4`, a `.webm` and a `.jpg` poster. `src/data/media.ts` lists them
  (`CLIPS`), and `scripts/capture/clips.mjs` says what each one does on screen.
  Each clip plays in one chapter of the page and nowhere else; `DESIGN.md`
  says which.
- `public/media/film/` holds generated footage, used only as atmosphere and
  listed in `src/data/media.ts` as `FILMS`. `speaker` sits behind the download
  card. `capture` (an amp that breaks up into a signal) is played by the
  scroll in the captures chapter. A video cannot seek as fast as a wheel
  turns, so on wide screens it is 121 AVIF stills per tier under
  `public/media/film/capture/<width>/` (`CAPTURE_SEQUENCE` in
  `src/data/media.ts`), drawn on a canvas. It was generated at 720p and
  upscaled to 2560x1440; that master is not in the repository. To rebuild the
  stills and the poster from a master: `npm run film:frames -- master.mp4`.
  `capture-720.mp4` is the loop phones play instead.
  `tubes` and `strings` are from the earlier design and are not on the page.

Both folders are committed.

### Re-record the editor clips

1. Serve the editor on its mock bridge: `cd ../ui && npm run dev` (port 5173).
2. Run `npm run capture` to record all 14. To record a subset, add the names:
   `npm run capture -- fx-eq presets`.

| Setting | Meaning |
| --- | --- |
| `--url <url>` or `TUBAMP_UI_URL` | Editor address. Default `http://localhost:5173`. |
| `--out <dir>` | Output folder. Default `public/media/ui`. |
| `PLAYWRIGHT_CORE` | Path to a `playwright-core` folder. Default: the first one in `~/.npm/_npx`. |
| `CHROME_PATH` | A `chrome-headless-shell` or Chrome binary. Default: the newest in `~/Library/Caches/ms-playwright`. |

`ffmpeg` must be on `PATH`. If the editor, Playwright, Chromium or `ffmpeg` is
missing, the script stops and says which. To get Playwright without adding a
dependency, run `npm i --prefix /tmp/pw playwright-core && npx --prefix /tmp/pw playwright-core install chromium-headless-shell`
and set `PLAYWRIGHT_CORE=/tmp/pw/node_modules/playwright-core`.

The script opens a 1280x800 window at device scale 2 and drives it with
scripted pointer moves, eased and with pauses. It draws a cursor into the page
and hides the "mock bridge" status text. It grabs frames as 2560x1600
screenshots, about 20 to 30 a second depending on machine load, and `ffmpeg`
retimes them to a constant 30 fps. Record on an idle machine: under load the
frame rate drops and the motion gets choppy.

What it writes:

- Board clips: 1920x1200, H.264 `yuv420p` with `+faststart` (CRF 24), VP9
  webm (CRF 34), no audio.
- Editor clips (`fx-*`): the 296 px editor band, cropped from the bottom of
  the window above the status bar, at 2560x592 (CRF 18 and 30).
- Poster: the first frame, as JPEG.

Each clip runs 5 to 9 seconds and ends in the state it began in, so the loop
does not jump. Two clips do not: `chain-build` keeps the blocks it added, and
`t3k` keeps the capture it loaded.

### Build the in-page demo

`npm run build:demo` runs `npm ci` and `vite build` in `../ui` and copies the
build to `public/demo/`. It builds into a temporary folder, so `ui/dist` is
left alone. The site serves the demo at `/demo/index.html`. It is a build
product and git ignores it. To build another copy of the editor, pass
`-- --ui <dir>`. To skip `npm ci`, pass `-- --skip-install`.

## Where things are

| Path | What it holds |
| --- | --- |
| `src/app/[lang]/page.tsx` | the page: the chapters in order |
| `src/components/hero/`, `src/components/sections/` | one file or folder per chapter |
| `src/components/ui/` | shared pieces: `ButtonLink`, `VideoLoop`, `Window`, `ChapterLabel` |
| `src/i18n/` | every sentence the page says, per language, and the locale rules |
| `src/proxy.ts` | picks the language for `/` |
| `src/data/site.ts` | what reads the same in every language: block names, families, addresses |
| `src/data/media.ts` | the clips of the real editor and the generated film |
| `src/lib/motion.ts` | GSAP setup, media-query conditions, easings |
| `src/lib/release*.ts`, `src/lib/platform.ts` | release data and architecture detection |
| `public/media/ui/` | clips recorded from the real editor (committed) |
| `public/media/film/` | generated atmosphere footage (committed) |
| `public/demo/` | the plugin editor built for the web (a build product, not committed) |
| `scripts/` | `capture.mjs` records the clips, `build-demo.mjs` builds the demo |

## Deploy on Vercel

One-time setup, in the Vercel dashboard:

1. Import the `IamP5/tubamp` repository as a new project.
2. Set **Root Directory** to `site`. Keep **Include files outside the root
   directory in the Build Step** on: the build also compiles the plugin editor
   in `../ui` for the in-page demo.
3. Framework preset: Next.js. No environment variables are required.

After that, Vercel deploys every push to `main` and gives every pull request a
preview URL. `vercel.json` skips a deploy when nothing in `site/` or `ui/`
changed since the last successful deployment of that branch
(`VERCEL_GIT_PREVIOUS_SHA`), so a push of several commits deploys if any of
them touched those folders. Vercel treats exit code 0 from the command as
"skip" and 1 as "build". On a branch's first deployment the variable is empty
and the command exits 1, so the first deployment of a branch always builds.
If the previous commit is missing from the clone, `git diff` fails with exit
code 128, which is not 0, so the deploy runs. Vercel's documentation does not
describe that case. Check the deployment log if a deploy is skipped when you
expected one.

To use your own domain, add it under **Settings, Domains** in the project. The
Open Graph URLs follow the production domain automatically
(`VERCEL_PROJECT_PRODUCTION_URL`).
