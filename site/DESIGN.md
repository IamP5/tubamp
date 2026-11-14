# tubamp site design

The contract for anyone building a section of the landing page. `README.md`
covers running and deploying; this file covers what the page is, how it looks
and how it moves.

## What the page has to do

A guitarist who has never heard of tubamp lands here. Within one screen they
know what it is (a free Audio Unit for Logic Pro that plays neural captures of
real amps in a chain you build yourself). By the end they have seen the real
editor move, tried it in the page, and clicked Download for their Mac.

The owner asked for a page that is disruptive, gets attention and curiosity,
and feels modern, with real motion. The owner's own reference is
`jd-landing-demo` (Next.js, GSAP, Lenis, Motion, Tailwind 4): very large
display type, one loud colour field, tilted marquees that speed up with the
scroll, pinned chapters where one thing morphs into the next, a giant wordmark
in the footer, easing curves chosen with a reason. Match that level of craft.

The owner has also rejected "vibe coded" work: a different saturated colour on
everything, neon glows, gradients as decoration, tracked uppercase mono labels
everywhere, emoji, hint sentences, badges. And then found an all-grey static
version lifeless. The rule that came out of it: **colour with a meaning, motion
with a cause.**

## Concept: the signal path

The page is a signal chain. A visitor enters at the input (the hero), passes
through the blocks (the chapters), and leaves at the output (the download).
Three consequences:

- **One signal colour.** Amber `signal` is the page's only loud colour. It is
  the live wire: the primary button, the line that carries the signal, the
  download field. Everything else is ink and bone.
- **A chapter can borrow its block's family hue** (`data-family` on the
  section re-points `--accent`): gain amber, dynamics blue, tone teal,
  modulation rose, time violet. Use `var(--accent)` in a chapter, never a hue
  by name. One hue per chapter. Hues never appear side by side except where the
  chain itself is drawn.
- **Motion is the signal moving or the visitor's own scroll.** Things move
  because the page is scrolled or because a pointer touched them. Nothing loops on its own except video of the real product and
  a marquee.

## Voice

Plain, short, concrete, a little dry. Say what the thing does. "Your next amp
is a file." "Build the chain. Any order." No "unleash", "revolutionary",
"seamless", "next-level", no exclamation marks, no emoji. Sentence case
everywhere. Every factual claim must be true of the plugin today; the facts
live in `src/data/site.ts` and came from the repository's README and docs. Do
not invent numbers, model counts, latency figures, testimonials or awards.
"Powered by TONE3000" must stay visible wherever TONE3000 is shown (licence).

## Type

- **Geist Sans** for everything. Display sizes use the `display` utility
  (weight 600, tight tracking, line-height 0.88) with `text-mega`,
  `text-display`, `text-h2`.
- **Instrument Serif italic** (`serif-accent`) for the one or two words in a
  headline that carry the turn: "Your next amp *is a file.*" At most one use
  per headline, never for body text.
- **Geist Mono** (`label`) only for numbers, versions, file names, and the
  small index labels on chapters ("01 Input"). Not for sentences.
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
- Generated footage (macro tubes, strings, speaker cone) is atmosphere, used
  large and dark, never as a fake product shot.

## Motion system

Libraries: GSAP 3 with ScrollTrigger (`@/lib/motion` registers it and sets
defaults), `@gsap/react` (`useGSAP` with a `scope`), Lenis (already wired in
`SmoothScroll`), Motion (`motion/react`) for component-level and
scroll-linked transforms, three with `@react-three/fiber` only where a chapter
needs real 3D or a shader.

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
- Video: `muted loop playsInline preload="none"`, a poster, source set when
  the element nears the viewport, paused when it leaves. Use the shared
  `VideoLoop` component.

## Page

| # | Chapter | File | What happens |
| --- | --- | --- | --- |
| 00 | Nav | `components/Nav.tsx` | wordmark left, three anchors, Download button right; hides on scroll down, returns on scroll up |
| 01 | Input (hero) | `components/hero/` | headline "Your next amp is a file.", one line of what it is, the Download button with the live version, the first sight of the signal |
| 02 | Marquee | `components/sections/Marquee.tsx` | block names on a tilted amber band, speed follows scroll velocity |
| 03 | Chain | `components/sections/Chain.tsx` | pinned: blocks land in the chain one at a time as you scroll, the signal line changes shape as each is added, the real board video beside it |
| 04 | Captures | `components/sections/Captures.tsx` | NAM A2 captures and the TONE3000 browser inside the plugin |
| 05 | Effects | `components/sections/Effects.tsx` | the block editors, each in its family hue, each a loop of the real editor |
| 06 | More | `components/sections/More.tsx` | your own Audio Units in the chain, split lanes, presets with A/B |
| 07 | Try | `components/sections/Demo.tsx` | the real editor running in the page |
| 08 | Output | `components/sections/Download.tsx` | amber field, both installers, version, size, checksum, install steps, requirements |
| 09 | Footer | `components/Footer.tsx` | giant wordmark, licences, GitHub |

Copy, block names, families and facts are in `src/data/site.ts`. Media paths
are in `src/data/media.ts`. Read both before writing a string.

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
- Performance: nothing heavy above the fold blocks first paint; three.js and
  any chapter that needs it load with `next/dynamic` and only on the client;
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
