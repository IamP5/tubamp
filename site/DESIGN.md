# tubamp site design

The contract for anyone building a section of the landing page. `README.md`
covers running and deploying; this file covers what the page is, how it looks
and how it moves.

## What the page has to do

A guitarist who has never heard of tubamp lands here. Within one screen they
know what it is (an Audio Unit for Logic Pro that plays neural captures of
real amps in a chain you build yourself, with room for your own plugins). By
the end they have seen the real editor move, reordered a chain with their own
hand, tried the editor in the page, and clicked Download for their Mac.

The owner asked for a page that gets attention and feels modern, with 3D,
real motion and parallax, and whose job is one thing: the visitor downloads
the plugin. An earlier version borrowed its look from the owner's
`jd-landing-demo` (a wordmark the size of the screen, a tilted amber marquee,
film interludes behind blinds, an amber field, a giant footer wordmark). The
owner asked for that to go. Do not bring those back; this page has its own
look, led by the product.

The owner has also rejected "vibe coded" work: a different saturated colour on
everything, neon glows, gradients as decoration, tracked uppercase mono labels
everywhere, emoji, hint sentences, badges. And then found an all-grey static
version lifeless. The rule that came out of it: **colour with a meaning, motion
with a cause.**

## Conversion rules

- **The product is above the fold.** The hero shows the real editor next to
  the headline, the Download button and the reassurances under it ("No
  account" always, then the version when there is one, macOS, both
  architectures). Nothing has to be scrolled to know what this is and how to
  get it.
- **One action.** Every primary button is Download. The only secondary action
  is "Try it in the browser", which leads to the live editor and back to
  Download.
- **The button is always in reach.** The nav never hides and carries
  Download, and the page ends on the download card. There is no third ask in
  between.
- **"Free" is a reassurance, not the pitch.** TONE3000 ships a free NAM plugin
  of its own. The word appears in three places only: the hero button, the
  line under the download card's buttons, and one question. The pitch is what
  the others lack: your own Audio Units inside the chain, typed effects and
  TONE3000 browsing in one Logic insert.
- **A device that is not a Mac gets a way to the Mac.** When the platform is
  plainly something else (`notAMac` in `src/lib/platform.ts`), the primary
  button reads "Send the link to my Mac" and shares or copies the page's
  address. No email capture.
- **One home per clip.** A clip of the editor plays in one chapter and
  nowhere else: `chain-build` and `presets` in the hero, `t3k` and `amp` in
  Captures, the `fx-*` clips in the effects drum, `split` and `compare` in
  More. A claim repeats only at a decision point (next to a button, in the
  questions). The sentence that says what a block does appears only in the
  effects drum.
- **A pin budget.** At 1440 by 900 the page is about eleven and a half
  viewports, of which about two and a half are pinned (the capture film at 0.9,
  the effects drum at 1.5). Keep the pinned total under about 2.8, never put
  two pins next to each other, and pin nothing under 768 px. A new pin has to
  take its length out of another.
- **A headline is never scrubbed.** No headline's opacity or position follows
  scroll progress. It is revealed once, on time, when it enters (`Reveal`), or
  it is simply there.
- **Tell the truth about the installer.** While a release is unsigned, or
  there is no release to read (`unsigned()` in `download/format.ts`), the
  install steps, the after-click guide and the first question all carry the
  Gatekeeper step, and the card says the installer has no Apple Developer ID
  yet and links the source.
- **After the click, say what happens next.** `InstallGuide` appears on any
  click of a `/download/` link with the remaining install steps.

## Concept: the signal, and the product in depth

- **One signal colour.** Amber `signal` is the page's only loud colour: the
  primary button, the line that carries the signal, the small dot in front of
  a label. Everything else is ink and bone.
- **A chapter can borrow its block's family hue** (`data-family` re-points
  `--accent`): gain amber, dynamics blue, tone teal, modulation rose, time
  violet. Use `var(--accent)`, never a hue by name. Hues appear side by side
  only where the chain itself is drawn.
- **Depth is real UI at real distances.** The hero stacks two captures of
  the editor at two depths; the chain is nine tiles on a tilted board, and
  the one in the hand lifts off it; the effects are eight editors on the
  faces of a drum. 3D is CSS transforms on real product footage and on the
  page's own controls, and one WebGL canvas (`SignalField`) for the signal
  itself.
- **Motion has a cause:** the scroll, the pointer, or the signal. The signal
  field runs on its own because a signal does; it stops off screen. Video of
  the product loops. The chain board swaps Drive and Reverb once, shortly
  after it enters view, so a visitor who touches nothing still sees that the
  order changes the signal; it never plays again and never over a change the
  visitor made. Nothing else moves by itself.

## Voice

Plain, short, concrete, a little dry. Say what the thing does. "Your next amp
is a file." "Build the chain. Any order." No "unleash", "revolutionary",
"seamless", "next-level", no exclamation marks, no emoji. Sentence case
everywhere. Every factual claim must be true of the plugin today; the facts
live in `src/data/site.ts` and came from the repository's README and docs. Do
not invent numbers, model counts, latency figures, testimonials or awards.
"Powered by TONE3000" must stay visible wherever TONE3000 is shown (licence).

## Type

- **Geist Sans** for everything. Headlines use the `display` utility with
  `text-display` (hero, captures, download) or `text-h2`. They are large, not
  enormous: the product is the biggest thing on a screen, not the type.
- **Two-tone headlines.** The half that sets up the line takes `lead-in`
  (dimmed); the half that lands stays bone. No second typeface.
- **Geist Mono** (`label`) only for numbers, versions, file names, and the
  small counters in the effects list. Not for sentences. A chapter label is a
  sentence-case word after an accent dot (`ChapterLabel`).
- Body copy is 16 to 20px, `text-bone-2`, max 34rem wide. Headlines do the
  talking; a chapter has one headline, one short paragraph, and the moving
  thing.

## Colour and surface

Tokens are Tailwind theme colours in `src/app/globals.css`: `ink`, `ink-1..3`,
`bone`, `bone-2`, `bone-3`, `line`, `line-strong`, `signal`, `on-signal`, and
the five family names. Use them through Tailwind classes (`bg-ink-1`,
`text-bone-2`, `border-line`) or `var(--accent)`.

- No colour literals in components. No gradients except a fade to ink over
  video or an image so text stays readable. No coloured shadows, no glow, no
  blur as decoration, no glassmorphism.
- Product imagery is real: video captured from the real editor
  (`src/data/media.ts`) or the live editor in an iframe. Never draw a fake
  screenshot of the UI. Frame real UI in a plain window: `rounded-xl`, 1px
  `border-line-strong`, on `ink-1`.
- Generated footage (the amp that becomes a signal, the speaker cone) is
  atmosphere, used large and dark, never as a fake product shot.
- Shadow only under a layer that floats above another (the hero stack, the
  install guide), and only black.

## Motion system

Libraries: GSAP 3 with ScrollTrigger (`@/lib/motion` registers it and sets
defaults), `@gsap/react` (`useGSAP` with a `scope`), Lenis (already wired in
`SmoothScroll`), Motion (`motion/react`) for component-level and
scroll-linked transforms. Depth comes from CSS 3D transforms; `SignalField` is
raw WebGL with no library behind it.

- Import `gsap` and `ScrollTrigger` from `@/lib/motion`, never from the
  package directly, and call `setupGsap()` before first use in a component.
- Easing: entrances `expo.out` (the GSAP default here) or `EASE.out` in
  Motion; UI responses `EASE.ui`, 120 to 240 ms; scrubbed timelines use
  `ease: "none"` inside and let the scroll be the curve. Things that enter are
  already moving when they appear and land dead. Things that leave accelerate
  away. No bounce, no elastic.
- Scroll-linked work is `scrub: true` (or a small number such as 0.6 for a
  trailing feel), pinned with `pin: true`. Use `100svh`, not `100vh`.
- Every animated component gates on reduced motion. With GSAP use
  `gsap.matchMedia()` and the `MQ` conditions from `@/lib/motion`; with Motion
  use `useReducedMotion` from `@/components/SmoothScroll`. Under reduced motion
  the content is laid out in its final state, readable, with no pin and no
  scrub. On touch and narrow screens prefer the simpler version of an effect
  to a broken version of the full one.
- Animate transform and opacity. Do not animate layout properties or filters
  on large areas. Canvas and WebGL: cap `devicePixelRatio` at 2, stop the loop
  when the element is off screen (IntersectionObserver), dispose on unmount.
- Text reveals split by line or word, not by character, unless the headline is
  under four words.
- A film the scroll plays is a sequence of stills on a canvas
  (`captures/sequence.ts`), never a video with its `currentTime` set: a seek
  is slower than a wheel. Measured on this page, the video ran a median 1.3
  frames behind the scroll and 5.5 at the 95th percentile; the stills run 0.3
  and 0.9.
- Video: `muted loop playsInline preload="none"`, a poster, source set when
  the element nears the viewport, paused when it leaves. Use the shared
  `VideoLoop` component.

## Page

| Chapter | File | What happens |
| --- | --- | --- |
| Nav | `components/Nav.tsx` | wordmark, anchors, Download; always visible |
| Hero | `components/hero/` | headline, what it is, Download, reassurances; two layers of the real editor in 3D (`chain-build`, `presets`) that turn with the pointer and lie flat with the scroll; the signal field behind |
| Facts | `components/sections/FactStrip.tsx` | four true facts: TONE3000, 9 blocks, A2, 3 slots |
| Chain | `components/sections/Chain.tsx` | not pinned: the nine blocks as tiles on a tilted board the visitor reorders, and the signal that order gives drawn under it (`chain/ChainBoard.tsx`) |
| Captures | `components/sections/Captures.tsx` | pinned for 0.9 of a viewport: a film of an amp that breaks up into a signal, played by the scroll (`captures/ScrubFilm.tsx`); then the amp editor and the TONE3000 browser at two depths beside four facts |
| Effects | `components/sections/Effects.tsx` | pinned for 1.5 viewports: the eight editors on a drum that turns a face per step (`effects/EffectsDrum.tsx`); a swipe row on touch and under reduced motion |
| Try | `components/sections/Demo.tsx` | the real editor running in the page; a still on a narrow screen |
| More | `components/sections/More.tsx` | one row of three: your own Audio Units in the chain, split lanes, presets with A/B |
| Download | `components/sections/Download.tsx` | the card with both installers, size, checksum, install steps, requirements; the questions (`#faq`); the speaker film behind, moving slower than the page |
| Footer | `components/Footer.tsx` | links and credits, small |
| After a click | `components/InstallGuide.tsx` | what to do with the file that just downloaded |

Copy, block names, families and facts are in `src/data/site.ts`. Media paths
are in `src/data/media.ts`. Read both before writing a string.

### The chain board

The order of the blocks is the whole state (`chain/board.ts`, a reducer with
four phases: waiting, demo, idle, dragging). The tiles, the waveform
(`chain/signal.ts` through `chain/draw.ts`) and the caption are drawn from it.
With a mouse on a screen at least 1024 px wide the tiles are one row on a
tilted plane and drag with Motion's `Reorder`; anywhere else they are a
wrapping grid with a pair of move buttons on each tile, and nothing drags. The
CSS variant `board:` and the constant `ROW` in `ChainBoard.tsx` are the same
media query. A focused tile moves with the arrow keys, and the caption is the
live region that says where it went. Under reduced motion there is no
demonstration and a reorder lands at once. The caption states only what is
true of the picture: which block now follows which, and that a reverb ahead of
the drive distorts its tail.

## Engineering rules

- Next.js 16 with Cache Components. It differs from older versions: read the
  relevant guide in `node_modules/next/dist/docs/` before using an API you
  have not used in this version (see `AGENTS.md`).
- Server Components by default. A component is a Client Component only when it
  animates or handles input; keep the `"use client"` boundary at that
  component, not at the section.
- Release data comes only from `getLatestRelease()` in `src/lib/release.ts`
  (cached, refreshed every five minutes). It can be `null`: there is no
  release yet, or GitHub is unreachable. Every place that shows a version or a
  download must have a real design for `null` (the button links to
  `/download/arm64`, which falls back to the GitHub releases page).
- Download links are `/download/arm64` and `/download/x86_64`, never a
  hard-coded GitHub URL.
- No new dependency without a reason written in your report. No UI kit, no
  icon pack (draw the few icons as inline SVG, 1.5px stroke, round caps,
  `currentColor`).
- Accessibility: one `h1`, ordered headings, real `button` and `a` elements,
  visible focus, `aria-hidden` on decorative canvases and duplicated marquee
  content, text alternatives for video, colour contrast at least 4.5:1 for
  body text. The page must be usable with the keyboard alone and with
  JavaScript animations disabled by reduced motion.
- Performance: nothing heavy above the fold blocks first paint; a heavy
  chapter loads with `next/dynamic`;
  images through `next/image`; no layout shift when media loads (set
  dimensions or aspect ratio).
- Comments only for a non-obvious why. No narration.

## Checks before you say a section is done

1. `npm run lint`, `npm run typecheck`, `npm test` from `site/`. Other people
   edit other files at the same time; only errors in your files are yours.
2. Look at it. Screenshot at 1440x900 and 390x844 at several scroll
   positions, read the images, and judge them as a senior designer would
   against this file. Fix what you see. Do this at least twice.
3. Check it with reduced motion emulated: content readable, nothing pinned or
   stuck half-animated.
4. No console errors.
