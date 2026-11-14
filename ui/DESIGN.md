# tubamp UI design system

The look and the rules every screen follows. `docs/REACT-UI.md` is the
behaviour and bridge contract; this file is the skin. Enforced by
`node tools/design-lint.mjs src/<path>` (run from `ui/`).

The owner's bar: **a modern, professional audio plugin. Clean, minimalist,
restrained.** An earlier colourful look was rejected as "vibe coded". References
for the feel: FabFilter and Valhalla for the effect editors, Ableton Live's
device view for density, Linear and Vercel's dashboard for chrome.

## What is banned (and why the old UI read as generated)

- A different saturated colour per block. Coloured shadows, glows, neon wires,
  drifting chevrons, dotted grids, decorative gradients, blur.
- Motion that is not data: anything that loops, drifts, pulses or shimmers on
  its own. (Colour and motion themselves are not banned; see "Colour" and
  "Alive, not decorated" for the kind that is wanted.)
- Tracked uppercase mono labels as the default text style.
- Unicode characters as icons.
- Permanent hint sentences, captions, count badges, status pills.
- The same value shown twice in one view.
- Boxes inside boxes inside boxes.
- Cute nouns.

## The system

Tokens are in `src/theme/tokens.css`. **Use a colour, font, radius, shadow or
duration only through a `--ds-*` token.** The older names (`--bg-surface`,
`--border`, `--block-accent`, ...) are aliases kept only so unmigrated files
render; never use them in code you write, and replace them in code you touch.
Inline SVG takes token colours from a CSS class (`stroke: var(--ds-accent)`).

### Colour

- Neutral greys on a near-black window: `--ds-bg`, `--ds-surface-1..3`, lines
  `--ds-line` / `--ds-line-strong`, text `--ds-text` / `-2` / `-3` / `-disabled`.
- **One accent** (`--ds-accent`). It marks what is live: the selected block, a
  knob's value arc, a switch that is on, focus, the curve in an effect display.
- **The accent's hue follows the block's family** — what a block does, not
  which block it is. Five hues at one lightness and a matched chroma
  (OKLCH L 0.78), so none shouts over another:

  | Family | Blocks | Hue |
  | --- | --- | --- |
  | Gain | Drive, Amp, Amp 2 | amber `#f0a63a` (the house accent) |
  | Dynamics | Gate, Compressor | blue `#85bcf5` |
  | Tone | Cab, EQ | teal `#5ecbcb` |
  | Modulation | Modulation | rose `#eb9dbb` |
  | Time | Delay, Reverb | violet `#bfaaef` |

  Plugins and routing (Split, Mixer) have no family and stay neutral. Green and
  red are not in the set: they are status colours.
- How it is applied: `data-family` (from `BLOCK_INFO[id].family`) on a block's
  tile and on the editor band re-points the `--ds-accent*` tokens inside it
  (`theme/tokens.css`). Components never name a hue; they use `--ds-accent` and
  get the right one. The app's own chrome (header, drawer, settings) is amber.
- Where the hue shows at rest: the tile's glyph chip (glyph in the accent on
  `--ds-accent-soft`; neutral when bypassed), the selection ring, knob value
  arcs, the display's curve and its fill. Text, wires, surfaces and borders are
  never tinted.
- Status colours (`--ds-ok`, `--ds-warn`, `--ds-danger`) for state only. Meters
  use `--ds-meter`, `--ds-meter-hot`, `--ds-meter-clip`, never the accent.
- **No per-block colour**, and no hue outside the table. `BLOCK_ACCENT`,
  `blockCssVars` and `--block-*` are legacy: remove every use you meet.
- The primary action of a view is an inverse (light) button. At most one per
  view.

### Type

- Geist Sans (`--ds-font`) for everything a person reads. Geist Mono
  (`--ds-mono`, with `font-variant-numeric: tabular-nums`) only for numeric
  values and file names.
- Sizes: 11 caption, 12 label, 13 body, 15 title, 20 heading, 28 display.
  Weights 400, 500, 600. Nothing under 11px.
- **Sentence case. No `text-transform: uppercase`, no letter-spacing, no
  italics.** Labels go through `paramLabel()` and values through `paramValue()`
  from `src/theme/words.ts` ("Threshold", "Low cut", "1.60 s", "35%", "C").
- Hierarchy comes from the three text tokens and weight, not size jumps.

### Shape, depth, space

- Radius: 4 (chips), 6 (controls), 8 (cards, displays), 12 (overlays). Pills
  only for switches and meter bars.
- 1px hairlines. Separate regions with a line or a surface step, never both.
  Prefer whitespace to a border.
- **Shadow only on floating layers** (`--ds-shadow-float`: menus, popovers,
  dialogs, toasts, the item being dragged). No glow, coloured shadow, inner
  shadow, gradient, blur, background pattern.
- 4px grid: 4, 8, 12, 16, 24, 32. Control heights 24, 28, 32. Every clickable
  thing is at least 24x24 (pad the hit area invisibly if the mark is smaller).

### Components (`src/components`, one look everywhere)

- **Button.** 28 high (24 compact), radius 6, 12px/500. Primary: inverse fill.
  Secondary: `surface-2` with a hairline. Ghost: no fill, `surface-2` on hover.
  Destructive: ghost with danger text, only where it deletes.
- **Icon.** 16px box, 1.5px stroke, round caps, `currentColor`. SVG components
  only.
- **Segmented control.** Track `surface-1` with a hairline, selected segment
  `surface-3` with primary text. No accent fill.
- **Menu / select.** Trigger is a secondary button with a chevron. Menu is
  `surface-2`, radius 8, float shadow, 28px items, `surface-3` on hover, a check
  icon on the current item.
- **Knob.** Flat. Sizes: sm 32, md 40, lg 52. A disc in `surface-3` with a
  hairline, a 1.5px pointer line, and a 2px value arc just outside the disc
  (track `line-strong`, value accent at 80%). The pointer joins the accent and
  the arc goes to full while the knob is hovered or dragged. A value that
  arrives from outside (a preset, a type's starting points, a reset) sweeps the
  dial there in 180 ms. A bipolar parameter (range symmetric around zero)
  fills its arc from the centre. Label below (12px, `text-2`, sentence case),
  value under the label (11px mono). `−` / `+` step buttons flank the value and
  show on hover of the knob cell and while dragging; one click is one step,
  wrapped in a gesture. Vertical drag, double-click resets, wheel nudges. No
  scale-on-press, no bevel, no shading.
- **Switch.** 28x16, on is accent.
- **Power button** (block bypass). A power icon button: on is primary text, off
  is disabled text and the block's content drops to about 45% opacity. No LED,
  no pill with the word "ON".
- **Meter.** A 4px bar: track `meter-track`, fill `meter`, `meter-hot` above
  −6 dB, `meter-clip` at clip, a 1px peak tick. No gradient, no segments.
- **Toast.** `surface-2`, radius 8, float shadow, 13px, an optional ghost
  action ("Undo").
- **Tooltip.** `surface-3`, 11px, radius 4, after 400 ms. For icon-only buttons
  and truncated text.
- **Empty state.** One 13px/500 line, one `text-2` line, one action. No large
  icon in a box.
- **Loading.** Skeleton blocks in `surface-2`. A progress bar is 2px, accent.
- **Wordmark.** `tubamp` in Geist Sans 13px/600, primary text. No dot, no logo.

### Words

Plain nouns: Chain, Block, Split, Lane A, Lane B, Mixer, Preset, Capture (an
amp model), IR, TONE3000, Plugin (a hosted Audio Unit). Block names come from
`BLOCK_INFO[token].displayName` / `.shortName` (already sentence case: Gate,
Compressor / Comp, Drive, Amp, Cab, EQ, Modulation / Mod, Delay, Reverb,
Plugin 1). Buttons are verbs ("Add block", "Load", "Save as"). A/B slots are
"A" and "B"; storing is "Save to A". Engine status reads `48 kHz · 1.9 ms`
(`engineStatus()` in `theme/words.ts`), once, in `text-3`. "Powered by
TONE3000" and each tone's creator stay visible in the catalog (licence).

### Less

- No permanent instructions. Nothing explains itself in a sentence; a tooltip
  on an icon button is the limit.
- No duplicate readouts. One place per value per view.
- No decorative badges or counts. A badge exists only when it changes what the
  user does next.
- A view has one focal point. Squint at the screenshot: if more than one thing
  shouts, quiet the rest.
- Rarely used controls go behind a "More" menu, not into the layout.

### Motion

- Hover and press 120 ms. Selection, expand, reorder settle 180 ms. Overlays in
  and out 240 ms with a 4-8px translate plus fade. One easing (`--ds-ease`;
  `theme/motion.ts` for `motion` transitions).
- A dragged item gets the float shadow and a 1.02 scale; neighbours slide to
  open the gap; it settles with no visible bounce.
- Animate transform and opacity (and a tile chip's colour on bypass). No
  looping animation except meters, a progress bar, a loading skeleton and a
  live effect display. Honour `prefers-reduced-motion`.

### Alive, not decorated

The UI moves for two reasons only, and both are information.

- **The signal.** Things that carry or shape the sound respond to it. The
  wires lift (a second, brighter neutral stroke whose opacity is the meter
  level) while you play and settle when you stop. An effect display shows the
  effect answering what was just played: a delay's repeats light one delay time
  apart, a reverb's tail holds for its decay, a point rides a compressor's or a
  drive's curve. Source: `useMeters`, `useDynMeters` and `useSignal` (the input
  level's last 8 s) from `src/hooks`; written to the DOM per frame through
  `useLive` (`features/panel/display.tsx`), never through React state.
- **A value changing without a hand on it.** A type pick, a preset or
  automation moves the knobs and redraws the curves over 180 ms
  (`useEase(value)` for anything a display draws; the Knob does its own), so
  the picture travels to its new shape instead of cutting. While a pointer is
  down everything follows it exactly: no easing, no lag.

Rules: nothing moves in silence with idle hands. Live marks use the neutral
(`p.dot`), the accent as an area (`p.energy`) or meter ink; never a glow, a
trail or a particle. The one gradient in the system is the fade under a
display's curve (`PlotDefs`). Under reduced motion live marks are absent and
eased values cut.

## Layout

```
┌ header 44 ─ tubamp · preset ‹ name › Save as ········ A|B · TONE3000 · ⚙ ┐
│                                                                          │
│  board: the chain, wrapped into rows (fills)            ┌ TONE3000 ────┐ │
│                                                          │ drawer 400   │ │
├ editor 296 (only while a block is selected) ────────────┤              │ │
│  ⏻ Reverb  ‹ Plate ›                       ‹ ›  🗑  ✕  │              │ │
│  [ display ]   Mix  Decay  Pre-delay │ Size Damp Mod …  │              │ │
├ status bar 36 ─ Input ▬▬ 0.0 dB ···· 48 kHz · 1.9 ms ···· 0.0 dB ▬▬ Output ┤
```

The dominant pattern in amp sims (AmpliTube, Helix Native, Quad Cortex, Genome)
is the chain on top and the selected block's editor below. Ours is the same.

## Editors (the band under the board)

What the best effect plugins do, and what ours takes from them:

- **One central display with the controls beside it** (FabFilter Pro-G / Pro-C /
  Pro-R, Ableton's devices). The display draws what the effect is doing from the
  same parameters the knobs edit.
- **Direct manipulation on the picture** for the edits that are easier there
  (Pro-Q's nodes, EQ Eight's dots): a handle on the threshold, the decay, a
  filter corner.
- **Two or three controls carry the effect** and get the visual weight
  (Valhalla: Mix, Decay, Pre-delay; Decapitator: Drive). The rest are small.
- **The type is stepped with arrows** beside its name (ValhallaFutureVerb), so
  algorithms can be auditioned without opening a menu.
- **Only what the type reads is shown** (ValhallaDelay, Helix): a control the
  current type ignores is dimmed in place (so the layout does not jump), and a
  whole group that never applies is absent.
- **A synced time shows its note value** in place of the number.
- **Flat, dense, one highlight colour** (Ableton).

The grammar, in code (`src/features/panel/ui.tsx`, `display.tsx`; read both, and
`ReverbBody.tsx`, the worked example):

```tsx
<HeaderSlot>            // beside the block's name, in the band's header
  <TypeStepper id="reverb_type" label="Type" onPick={...} />   // the type
  <ChoiceSegment ... /> <ChoiceMenu ... /> <SwitchField ... /> // mode, sync, switches
</HeaderSlot>
<Editor lead={...} display={<><Plot>…</Plot><PlotSplit /><CutPlot … /></>}>
  <Row> <K id=… label="Decay" size="lg" /> … </Row>     // the 2-3 that matter
  <Row> <Group label?> <K id=… label="Size" /> … </Group> … </Row>   // the rest
</Editor>
```

Rules:

- The band is 296px: 44px header, then a body 236px tall. Two knob rows fit (a
  large row over a small row). Never three rows, never a scrolling body.
- **Every parameter has exactly one control.** A handle on the display may edit
  the same parameter, but the display never prints its number.
- Knob sizes: `lg` for the two or three that define the effect, `sm` for the
  rest. Labels are sentence case, real words ("Threshold", "Sidechain HPF",
  "Low cut"); no abbreviations unless the word does not fit a 68px cell.
- The display: THE curve is `p.live` (the family's accent) with `p.fill` under
  it (fading to the baseline); a reference is `p.ghost`; scaffolding is
  `p.grid` / `p.axis` with `p.tick` labels; measured signal (levels, gain
  reduction) uses meter ink, never accent. What the effect does with the input
  is `p.energy` (the fill's area, brighter) or `p.dot` (a point on the curve).
- Every number a display draws goes through `useEase` first, handles included,
  so a type pick or a preset moves the picture rather than replacing it.
  Draw honestly: use the engine's real law where it is cheap (read `src/dsp`
  when unsure), and an explicit "picture of the direction" where it is not.
  No decoration: every mark is data.
- A live display (a level trace, an LFO) animates through `onFrame` from
  `src/hooks` (one rAF loop for the app) and writes to the DOM directly; it does
  not re-render React per frame. Honour reduced motion with a static picture.
- At `data-narrow` (the band under ~780px) the display is dropped by the
  toolkit; your rows must then fit ~560px without scrolling.
- Keep every behaviour the editor has today: per-type starting points on a type
  pick, which controls a type ignores, sync behaviour, meters, instance
  awareness (`<token>_…` ids for the 2nd and 3rd instance).
- Hint sentences are deleted. If a fact changes what the user does, it becomes a
  tooltip on the control it is about.

Per editor (display → large knobs → small knobs):

| Block | Header | Display | Large | Small |
| --- | --- | --- | --- | --- |
| Gate | mode stepper | level trace over time with the threshold as a draggable line; gain reduction as a meter-ink bar | Threshold, Range | Hold, Release, Sidechain HPF |
| Compressor | type stepper, Auto makeup switch | transfer curve (in vs out) from threshold / ratio / knee with the unity line as ghost and a handle on the threshold; gain reduction as a meter-ink bar | Threshold, Ratio, Makeup | Attack, Release, Knee, Mix, Sidechain HPF |
| Drive | type stepper | waveshaper transfer curve (shape follows type and gain) beside the tone response | Gain, Tone, Level (named per type) | — |
| EQ | type stepper, voice menu | the response curve; parametric: handles for the mid bell, the two shelves and the cuts; graphic: seven faders with the response line through them | Bass, Mid, Treble | Mid freq, Mid Q, Low cut, High cut, Tilt, Level |
| Modulation | type stepper, voicing menu, sync menu, colour switch | the LFO: wave and shape drawn over two cycles, depth as amplitude, a phase marker moving at the rate | Rate, Depth, Mix | Width, Tone, Shape, Wave |
| Delay | type stepper, mode segment, sync menu | the taps on a time axis (spacing = time, heights fall by feedback, left above the axis and right below in the stereo modes) beside the low/high cut response | Time, Feedback, Mix | Duck, Color, Mod, Ratio, Width, Low cut, High cut |
| Reverb | type stepper | the tail over time with pre-delay and damping, beside the low/high cut response | Decay, Pre-delay, Mix | Size, Damping, Mod, Width, Low cut, High cut |
| Amp / Amp 2 | — | (lead) the capture: its title, creator, file controls, Browse TONE3000 | Input, Output, Bass, Mid, Treble | Cal level, Slim; output mode menu; switches |
| Cab | — | (lead) the IR; display: low/high cut response | Low cut, High cut, Mix | — |
| Split | mode segment | the crossover (two curves) in X-Over mode; nothing otherwise | Crossover | — |
| Mixer | — | none | A level, B level, Master | A pan, B pan; B phase switch |
| Plugin | plugin menu | (lead) the hosted plugin and "Open editor"; its parameters as small knobs | — | — |

## Do not regress

- Every action works with a pointer alone: Logic gives the WebView no keyboard.
  Text entry goes through native prompts. Hit targets are at least 24x24.
- Keep every `aria-label`, `role`, `title`, `data-block`, `data-param-index`
  and other `data-*` attribute. Test drivers and Logic's touch-to-select find
  controls by them. If a visible label changes, keep the aria-label stable
  unless it would now be wrong.
- Do not change the bridge, the store's actions, parameter ids, drag/layout
  logic or chain semantics. This is a redesign of the skin and of how things
  are arranged, not of behaviour. If a visual change forces a logic change,
  keep it minimal and re-drive the flow you touched.
- CSS floor is Safari 14 (macOS 11 WKWebView): no `color-mix()`, no container
  queries, no `:has()`, no `aspect-ratio`, no `inset` inside `@supports`
  tricks. Flex `gap` is fine.
- The window is resizable; both 1180x720 and 1000x600 must work, and the
  editor must stay usable at about 600px wide (the TONE3000 drawer open at the
  minimum window).
- Removing a block, a split or clearing a slot keeps its toast with Undo where
  one exists today.

## Process for any redesign task here

1. Screenshot "before" (see Tools). Read the CSS you own.
2. Decide the composition first: the focal point, what is deleted, what moves
   behind a menu.
3. Rewrite. Prefer rewriting a CSS module from scratch over patching it. Delete
   markup that only existed for decoration.
4. `node tools/design-lint.mjs <your files>` until clean. A real exception
   takes a `ds-allow: <reason>` comment on that line; list each in your report.
5. `npx tsc --noEmit` and `npx eslint <your files>` from `ui/`. Other people
   are editing other files at the same time, so only errors in your files are
   yours.
6. Screenshot at 1180x720 and 1000x600, look at the result, and critique it as
   a senior product designer would against this file. Fix what you see. Do at
   least two such rounds.
7. Drive the flows your area owns once more to prove nothing broke.

## Tools

The dev server runs at `http://localhost:5173/` with hot reload and a mock
bridge (do not start or stop it). Mock flags: `?t3kfail`, `?t3kfavfail`,
`?t3kslow`, `?t3k429`, `?t3koffline`, `?t3kprompt=text`, `?missing`.

Headless driving (the chrome-devtools MCP may be busy): a Node script importing
the driver named in your task prompt: `open({query,width,height})` returns
`{page, errors, close}` (a Playwright page), `controls(page)` lists the visible
interactive elements with labels and boxes. Block cards are
`[role=group][data-block=<token>]`; menu items are `[role=menuitem]`; "Add a
block to the chain" opens the picker. Write scripts and screenshots only in the
scratch directory your prompt names, and view screenshots with the Read tool.
