# tubamp site

The landing page for the tubamp plugin. Next.js 16 (App Router, Cache
Components), React 19, Tailwind 4, GSAP with ScrollTrigger, Lenis, and Motion.
Vercel hosts it.

`DESIGN.md` is the design contract: concept, voice, type, colour, motion rules.
Read it before you change how the page looks or moves.

## Run it

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
   The result is cached and refreshed in the background every five minutes.
3. `parseManifest()` in `src/lib/release-schema.ts` is the only place manifest
   JSON becomes a `Release`. It rejects a manifest whose downloads point
   outside this repository's release assets.
4. The download buttons link to `/download/arm64` and `/download/x86_64`
   (`src/app/download/[arch]/route.ts`), which redirect to the latest
   installer, or to the GitHub releases page when no release exists yet.

A new release therefore reaches the page within about five minutes. To see a
new release sooner, redeploy from the Vercel dashboard.

## Media

The page uses two kinds of footage.

- `public/media/ui/` holds 14 clips recorded from the real plugin editor. Each
  is an `.mp4`, a `.webm` and a `.jpg` poster. `src/data/media.ts` lists them
  (`CLIPS`), and `scripts/capture/clips.mjs` says what each one does on screen.
- `public/media/film/` holds three macro clips (`tubes`, `strings`,
  `speaker`) in the same three formats, 1920x1080 and 8 seconds long. They were
  generated, not captured, and the page uses them only as atmosphere: the hero
  and the two interludes around the chain. They are listed in `src/data/media.ts`
  as `FILMS`.

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
| `src/app/page.tsx` | the page: the chapters in order |
| `src/components/hero/`, `src/components/sections/` | one file or folder per chapter |
| `src/components/ui/` | shared pieces: `ButtonLink`, `VideoLoop`, `Window`, `ChapterLabel` |
| `src/data/site.ts` | every string and fact the page states about the plugin |
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
preview URL. `vercel.json` skips a deploy when a push touches neither `site/`
nor `ui/`.

To use your own domain, add it under **Settings, Domains** in the project. The
Open Graph URLs follow the production domain automatically
(`VERCEL_PROJECT_PRODUCTION_URL`).
