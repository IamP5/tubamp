# tubamp site

The landing page for the tubamp plugin. Next.js 16 (App Router, Cache
Components), React 19, Tailwind 4, GSAP with ScrollTrigger, Lenis, Motion, and
three with `@react-three/fiber`. Vercel hosts it.

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

## Where things are

| Path | What it holds |
| --- | --- |
| `src/app/page.tsx` | the page: the chapters in order |
| `src/components/hero/`, `src/components/sections/` | one file or folder per chapter |
| `src/components/ui/` | shared pieces: `ButtonLink`, `VideoLoop`, `Window`, `ChapterLabel` |
| `src/data/site.ts` | every string and fact the page states about the plugin |
| `src/data/media.ts` | the clips of the real editor |
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
