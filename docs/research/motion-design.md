# tubamp motion design spec

Scope: motion language for the React+TypeScript rebuild of tubamp's editor, running
inside a fixed 1120×700 WKWebView canvas. Library: **Motion for React** (`motion/react`,
npm package `motion`) — not GSAP. Aesthetic target: Vercel dark-mode — fast, subtle,
spring-based, no cartoon bounce. Companion to `docs/UI-REDESIGN.md` (palette, block
accents, layout) and `tubamp-chain-ui` memory (chain-order invariants).

References studied (techniques only, translated to Motion idioms below): osmo.supply
(hover/press feel, easing variety), motion.dev docs (`/react`, `/react-gestures`,
`/react-layout-animations`, `/react-animate-presence`, `/react-use-animate`), Codrops
guiding-animation article (flying-element eye-guidance, two-phase easing), Codrops Luma
morph article (image-sequence morph — explicitly NOT applicable here, see §3.6), Codrops
3D audio visualizer (AnalyserNode + rAF, smoothingTimeConstant pattern → informs our
meter spec).

---

## 1. Motion tokens

All values live in one module, e.g. `src/ui/motion/tokens.ts`, imported everywhere —
never inline a raw ms/bezier value in a component.

### 1.1 Durations (tween-based animations: opacity, color, small UI chrome)

| token      | ms  | use                                                              |
|------------|-----|-------------------------------------------------------------------|
| `instant`  | 100 | power-dim, LED toggle, colour swaps, meter-peak flash             |
| `fast`     | 150 | hover/press feedback, tooltip, connector highlight                |
| `base`     | 220 | panel content morph, param-panel swap, block select ring          |
| `slow`     | 320 | modal/overlay open, board entrance stagger step, A/B flip          |
| `deliberate` | 420 | zoom-to-fit, preset crossfade full transition                   |

Vercel-like ceiling: nothing user-facing and reversible should exceed ~300ms except
one-shot orientation moments (modal open, zoom-to-fit, entrance) which may reach 420ms.

### 1.2 Eases (cubic-bezier, for tween transitions — opacity/color/non-physical props)

```ts
export const ease = {
  // Vercel-esque "out" curve: snappy start, soft landing. Default for entrances.
  out: [0.16, 1, 0.3, 1] as const,
  // Symmetric, for crossfades / colour swaps with no directional intent.
  inOut: [0.65, 0, 0.35, 1] as const,
  // Sharp in, for exits/dismissals — gone fast, no lingering.
  in: [0.7, 0, 0.84, 0] as const,
  // Micro-interactions (hover/press) — near-linear, just enough curve to not feel robotic.
  micro: [0.4, 0, 0.2, 1] as const,
};
```

Use `ease.out` as the default for anything entering or growing; `ease.in` for anything
leaving or shrinking; `ease.inOut` for state crossfades (preset switch, A/B); `ease.micro`
for hover/press tweens where springs aren't used.

### 1.3 Springs (physical properties: x, y, scale, rotate, layout, drag)

```ts
export const spring = {
  // Hover/press micro — tiny travel, must settle within ~120ms, zero overshoot.
  micro: { type: "spring", stiffness: 500, damping: 40, mass: 0.5 } as const,
  // Default UI spring — block select ring, knob snap-back, small chrome.
  ui: { type: "spring", stiffness: 400, damping: 32, mass: 0.8 } as const,
  // Drag pick-up (block lift) — a little livelier, still no bounce.
  pickup: { type: "spring", stiffness: 380, damping: 28, mass: 1 } as const,
  // Drop / settle after reorder — slightly underdamped for a single tiny settle-tick,
  // NOT a bounce: damping ratio ≈ 0.86.
  settle: { type: "spring", stiffness: 300, damping: 26, mass: 1 } as const,
  // Layout reflow of siblings when a block moves/added/removed.
  reflow: { type: "spring", stiffness: 350, damping: 34, mass: 1 } as const,
  // Canvas pan/zoom inertia — heavier mass, generous damping, feels like a real surface.
  canvas: { type: "spring", stiffness: 220, damping: 30, mass: 1.4 } as const,
  // Meter needle response — fast attack via decay function, not this spring (see §2.11).
};
```

Rule: **damping / (2 * sqrt(stiffness * mass)) should stay ≥ 0.75** for every token
above — this keeps all springs critically-to-slightly-underdamped, i.e. one soft
settle-tick at most, never a visible bounce. Verify any new spring against this ratio
before adding it.

### 1.4 Stagger

- Board entrance: `staggerChildren: 0.035`, `delayChildren: 0.05` (see §2.1).
- List-like reveals (picker menu items, TONE3000 result grid): `staggerChildren: 0.02`,
  cap total stagger spread at ~150ms regardless of item count (compute
  `staggerChildren = min(0.02, 0.15 / itemCount)` for long lists).

---

## 2. Choreography per interaction

### 2.1 App entrance (staggered board build-in)

Board mounts once per editor open — this is the one place a longer, orchestrated
sequence is acceptable.

- Outer canvas container: `initial={{ opacity: 0 }} animate={{ opacity: 1 }}`,
  `transition={{ duration: 0.15 }}` — canvas chrome (grid background, rails) fades in
  first, near-instant, so nothing pops on a blank frame.
- Header bar: `initial={{ y: -12, opacity: 0 }}`, `animate={{ y: 0, opacity: 1 }}`,
  `transition={{ ...spring.ui, delay: 0.02 }}`.
- Chain tiles: parent `motion.div` with `variants` container
  (`staggerChildren: 0.035, delayChildren: 0.08`), each tile
  `variants={{ hidden: { opacity: 0, y: 8, scale: 0.96 }, show: { opacity: 1, y: 0, scale: 1 } }}`,
  `transition={spring.ui}`. Left-to-right order = signal-chain order (reads as the
  chain "assembling" itself, reinforcing signal flow).
- Param panel: fades/slides in last (`delay` ≈ tiles-done + 0.05s), `ease.out`,
  `duration: slow`.
- Total entrance budget: ≤ 500ms end-to-end even with 8 blocks (guarded by the stagger
  cap in §1.4 applied to the tile count).
- Do NOT restagger on every re-render — gate the container `animate` on a
  `hasMounted` ref/flag so prop changes after mount don't replay entrance variants.

### 2.2 Block drag (reorder)

Manual drag on a canvas, not `Reorder.Group` (see §3.2 for why). Per tile:

- Idle: no transform.
- Pick-up (`onDragStart` handled by Motion internally when `drag="x"` — chain is
  horizontal, lock the axis): animate to
  `{ scale: 1.04 }`, `boxShadow` swapped via **class change**, not animated boxShadow
  (see §4.2), `zIndex: 10`. Use `whileDrag={{ scale: 1.04, cursor: "grabbing" }}` with
  `transition: spring.pickup`.
- Shadow: two stacked pseudo-elements (`.tile-shadow-rest`, `.tile-shadow-lift`),
  cross-faded via `opacity` (`animate={{ opacity: dragging ? 1 : 0 }}`,
  `duration: instant`) — never animate `box-shadow`'s blur/spread directly.
- Siblings: each tile has `layout` prop; as the dragged tile's index changes in the
  underlying order array (computed from pointer x vs tile midpoints, on
  `onDrag`), React reorders the array, and Motion's layout animation slides the other
  tiles into their new slots automatically. Transition for siblings:
  `spring.reflow`.
- Drop (`onDragEnd`): commit new order to state (chain order is STATE, per
  tubamp-chain-ui invariant — this write is what publishes `ChainOrder`, same contract
  as the existing packed-atomic path, just now originating from JS state instead of a
  C++ UI event). Animate the dropped tile back to `scale: 1, zIndex: auto` with
  `spring.settle` — this is the one "settle-tick" moment.
- Drag threshold: `dragElastic={0}` (chain reorder shouldn't rubber-band past
  neighbours — snappy, precise, Miro-like, not squishy). `dragMomentum={false}`
  (tile should not coast after release; it snaps to its slot immediately).
- Cross-position detection: compare dragged tile's live `x` (via `useMotionValue`,
  not React state) against sibling midpoints on every `onDrag` frame — reordering the
  backing array only when a threshold (50% overlap) is crossed, to avoid flicker.

### 2.3 Connector redraw (animated SVG path between tiles)

Connectors are thin lines/chevrons between adjacent tiles showing signal flow.

- Represent each connector as a `motion.path` (or `motion.line` for straight runs)
  whose `d`/`x1,x2` are driven by the tiles' measured positions (via `layoutId`-free
  manual measurement using `useMotionValue` synced from each tile's drag/layout, or
  simpler: recompute on every layout animation frame via `onLayoutAnimationStart` +
  a `requestAnimationFrame` reader — see §3.1 for why not to trigger this from React
  render).
- On reorder: animate the path's endpoints with `spring.reflow` (same token as tile
  reflow, so line and tiles move in lockstep — critical for the "connected chain"
  illusion).
- On add: new connector segment draws in with a `pathLength` animation —
  `initial={{ pathLength: 0, opacity: 0 }}`, `animate={{ pathLength: 1, opacity: 1 }}`,
  `transition={{ duration: 0.22, ease: ease.out }}` (pairs with block-add timing,
  §2.4). Cap concurrent path animations — see §4.5.
- On remove: fade only (`opacity → 0`, `duration: instant`), do not reverse-draw the
  path — reverse-draw reads as slower/laggier than a fade for a fast delete action.
- Bypassed block: its inbound/outbound connector segments dim to 35% opacity
  (`duration: fast`) rather than dashing/animating pattern — animated `stroke-dasharray`
  is expensive per §4, avoid it.

### 2.4 Block add (picker → chain position)

- Picker item is a small list entry (not a large card), so no full flying-element
  animation like the Codrops cart example — that technique is for large product
  imagery over long distance; here source and destination are close together in a
  1120px canvas, so a flying clone would be gratuitous. Instead:
- On confirm, the picker closes (`AnimatePresence` `exit`, `duration: fast`,
  `ease.in`) while the new tile enters at its target slot directly:
  `initial={{ opacity: 0, scale: 0.85, y: 6 }}`, `animate={{ opacity: 1, scale: 1, y: 0 }}`,
  `transition: spring.pickup`.
- Siblings that shift right/left to make room animate via their existing `layout`
  prop with `spring.reflow` — same mechanism as drag reflow, so add/remove/reorder
  all feel like one consistent physical system.
- New tile gets a one-shot accent-ring "arrival" pulse: `boxShadow`-as-opacity trick
  (a `::after` ring element, `opacity: [0, 1, 0]` over `duration: base`) instead of
  animating a real shadow — see §4.2.

### 2.5 Block remove (exit + chain close-up)

- Wrap the tile list in `<AnimatePresence mode="popLayout">` (see §3.3) so the
  exiting tile is pulled out of flow immediately and remaining tiles reflow without
  waiting for the exit animation to finish — this matches "the chain closes the gap
  right away" rather than leaving a hole.
- Exit variant: `exit={{ opacity: 0, scale: 0.85, y: 6, transition: { duration: fast, ease: ease.in } }}`.
- Remaining tiles slide into the closed gap via `layout` + `spring.reflow`
  (identical feel to add/drag — one physical vocabulary for all chain-topology
  changes).
- If the removed block was selected, param panel content-morphs to the fallback
  selection (amp → first block → empty state) using the same AnimatePresence content
  swap as §2.6, run concurrently (do not sequence chain-close-up before panel swap —
  both are short enough to overlap without feeling busy).

### 2.6 Select (accent ring + param panel content morph)

- Selection ring: not a mount/unmount, a **shared element** — one ring `motion.div`
  with `layoutId="selection-ring"` that Motion animates between whichever tile is
  selected. Moves with `spring.ui`. This is cheaper and reads better than fading a
  ring in/out per-tile.
- Tile itself: selected state adds `scale: 1.02` via `animate` (variant swap keyed off
  `isSelected`), `transition: spring.micro`.
- Param panel: content keyed by `blockId`. Wrap in
  `<AnimatePresence mode="wait">` — but **wait mode adds a gap** (old exits, THEN new
  enters), which reads as sluggish for frequent clicking through blocks. Prefer
  `mode="popLayout"` with a very short crossfade instead: outgoing panel
  `exit={{ opacity: 0 }}` (`duration: instant`), incoming
  `initial={{ opacity: 0 }} animate={{ opacity: 1 }}` (`duration: fast`, slight
  overlap since popLayout doesn't block). No x/y travel on panel swap — pure opacity
  crossfade keeps knob positions visually stable so users don't lose their place.
- Panel header accent underline: `layoutId="panel-accent"` shared element, animates
  colour via a discrete class swap (accent colour is a CSS custom property scoped to
  the panel root, not an animated `background-color` per knob — one recolor point).

### 2.7 Bypass toggle (power dim)

- LED dot: `animate={{ backgroundColor: on ? accent : "transparent" }}`,
  `duration: instant` (100ms) — this is a state flip, should feel like a physical
  switch, not a fade-y transition.
- Tile body: bypassed state drops icon/label to `textFaint` and the whole tile to
  `opacity: 0.55`, `transition: { duration: instant, ease: ease.micro }`. Instant
  because bypass is a frequent, deliberate on/off action (like a stompbox) — anything
  slower than ~120ms starts to feel laggy under repeated clicking.
- No scale/shake feedback on toggle — restraint here matters, this control gets
  clicked constantly during A/B comparison.

### 2.8 Param knob / slider feedback

- Value fill (rotary sweep / slider fill): driven by a `useMotionValue` bound
  directly to the control's numeric value — **not** re-rendering React on every
  drag-move tick. Wire the knob's pointer handler to call `motionValue.set(newVal)`
  and separately debounce/throttle the React-state (APVTS bridge) update.
- On drag start: knob thumb `whileTap`-style `scale: 1.08`, `spring.micro`.
- On value change from an external source (automation, preset load): animate the
  motion value with `animate(motionValue, newValue, { type: "spring", ...spring.ui })`
  rather than snapping, so automation reads as motion, not a jump-cut — except during
  preset switch (§2.9) where a snap is correct (crossfade communicates the change,
  a second knob-sweep animation would be redundant/busy).

### 2.9 Preset switch (crossfade content)

- Whole param panel (and any per-block visual state) crossfades: current panel
  `exit={{ opacity: 0 }}` `duration: base`, next panel `initial={{ opacity: 0 }}`
  `animate={{ opacity: 1 }}` `duration: base`, `ease: ease.inOut`, run inside
  `AnimatePresence mode="sync"` (both allowed to overlap — a true crossfade, not a
  sequential wait) — deliberately the one place `sync` mode is preferred over
  `popLayout`, because there's no layout reflow involved, just a content swap.
- Knob positions snap (no animate-from-old-value sweep) the instant the crossfade
  starts — see §2.8 exception.
- Chain tiles do not replay entrance stagger on preset switch — only their bypass/
  accent states update via the instant tokens in §2.7.

### 2.10 A/B flip

- Treat as a specialized preset switch (§2.9) but faster and with a directional cue:
  panel content does a combined `opacity` + tiny `x` shift (`±6px`, direction = which
  side A/B toggled from/to) using `duration: slow` (320ms), `ease.inOut`. The x-shift
  is the only place this app uses directional travel for a content swap — it's
  meant to communicate "flipping between two slots," distinct from preset switch's
  pure crossfade.
- A/B pill indicator: `layoutId="ab-indicator"` shared element sliding between A and
  B positions, `spring.ui`.

### 2.11 TONE3000 browser overlay (modal scale/fade)

- Backdrop: `initial={{ opacity: 0 }} animate={{ opacity: 1 }}`,
  `duration: base`, plain opacity — backdrop is a large surface, must stay
  transform/opacity-only (§4).
- Modal panel: `initial={{ opacity: 0, scale: 0.96, y: 8 }}`,
  `animate={{ opacity: 1, scale: 1, y: 0 }}`, `transition: { ...spring.ui }`, exit is
  the reverse at `duration: fast` with `ease.in` (fast dismissal, slower deliberate
  entrance — standard modal asymmetry).
- Result grid items (bounded Select-flow buckets only, per ToS constraint in
  tubamp-project memory): staggered entrance per §1.4 formula, capped so a 40-item
  grid doesn't take longer to reveal than a 6-item one.
- Focus trap / scroll lock handled outside Motion (standard modal a11y), not a
  motion concern but must not be skipped.

### 2.12 Meters (spring-smoothed levels)

- Input/output/gain-reduction meters update at audio-callback rate conceptually, but
  the UI only needs ~30–60Hz visual updates. Drive each meter's fill via a
  `useMotionValue` updated inside a `requestAnimationFrame` loop that reads the
  latest level from a ref (populated by whatever bridge delivers meter data from the
  plugin, e.g. a polled APVTS/host callback) — **never** via `useState`/re-render
  (§4.6).
- Attack: near-instant response to rising levels — set the motion value directly, or
  spring with high stiffness or with `attack ≈ 0.9` if using an exponential smoothing
  function instead of a spring (see below).
- Release: slower decay so peaks are readable, mirroring the audio visualizer
  reference's `analyser.smoothingTimeConstant = 0.8` pattern — implement as an
  asymmetric exponential smoother rather than a symmetric spring:
  ```ts
  // per rAF tick, dt in seconds
  const targetLevel = readLatestLevelFromRef();
  const coef = targetLevel > current ? attackCoef : releaseCoef;
  // attackCoef ≈ 1 - exp(-dt / 0.02)  (~20ms time constant)
  // releaseCoef ≈ 1 - exp(-dt / 0.35) (~350ms time constant, readable decay)
  current += (targetLevel - current) * coef;
  meterMotionValue.set(current);
  ```
  This is not one of the §1.3 springs — meters need independent attack/release time
  constants, which a single symmetric spring config can't express.
- Peak-hold tick: separate motion value, snaps up instantly on new peak, decays
  linearly over ~1.5s (matches the existing UI-REDESIGN.md peak-hold spec), reset by
  a plain `setTimeout`-free rAF countdown (avoid timer drift under WKWebView
  throttling by driving from the same rAF loop, not `setInterval`).
- Meter segments (10-segment style per UI-REDESIGN.md): render as a `transform:
  scaleY` clip on a single fill rect driven by the motion value, not 10 discrete
  React-rendered segment elements toggling — one motion value, one transform, CSS
  gradient handles the segment look via `background` (static, not animated).

### 2.13 Pan/zoom (wheel/pinch/space-drag, zoom-to-fit)

- Canvas transform (`translate(x, y) scale(z)`) lives entirely in `useMotionValue`s
  (`panX`, `panY`, `zoom`), applied via a single `style={{ x: panX, y: panY, scale:
  zoom }}` on the canvas content layer — zero React re-renders during pan/zoom.
- Wheel (trackpad pan) and pinch (trackpad zoom, `ctrlKey` wheel events) update the
  motion values directly in the event handler (`panX.set(panX.get() - dx)`), with no
  spring on the raw input path — 1:1 tracking feels correct for direct manipulation,
  matching Figma/Miro convention.
- Release inertia: on wheel-end / pointer-up after a pan gesture, apply
  `animate(panX, target, { type: "inertia", velocity: vx, power: 0.7, timeConstant: 300,
  bounceStiffness: 300, bounceDamping: 40, min: boundsMinX, max: boundsMaxX })` (Motion's
  built-in `inertia` transition type) so the board keeps drifting briefly and bounces
  softly off canvas bounds — this is the one intentional soft-bounce in the whole
  spec, appropriate because it's mimicking physical momentum on a large free surface,
  not UI chrome.
- Space-drag-to-pan: same `panX`/`panY` motion values, driven by a `motion.div`
  with `drag` enabled only while spacebar is held (toggle the `drag` prop based on
  key state), `dragMomentum={true}`, `dragTransition={{ power: 0.7, timeConstant: 300 }}`
  so it shares the same inertia feel as wheel-pan.
- Zoom-to-fit (button or shortcut): compute target `panX/panY/zoom`, animate all
  three with `spring.canvas` simultaneously (`animate(panX, tx, spring.canvas)` etc,
  or a single `animate` call across a combined value) — `duration` implicit from
  spring, expect ~400-500ms settle given the token's stiffness/damping. No overshoot
  (damping ratio compliant with §1.3 rule) — content should not jiggle past the fit
  bounds.
- Zoom limits: clamp motion-value writes at the source (`Math.min(Math.max(...))`)
  rather than fighting an out-of-range spring afterward.

---

## 3. Motion-for-React implementation notes

### 3.1 layout / layoutId

- Use the bare `layout` prop on every chain tile, connector-adjacent wrapper, and
  panel accent underline for automatic FLIP-style position/size animation on reorder,
  add, remove.
- Use `layoutId` for genuinely shared singleton elements that move between hosts:
  selection ring (§2.6), panel accent underline (§2.6), A/B indicator pill (§2.10).
  Do not use `layoutId` for the chain tiles themselves — they're a homogeneous list
  where plain `layout` + key-based React reconciliation is correct and cheaper.
- `LayoutGroup` (from `motion/react`) should wrap the whole chain + connector region
  so layout animations across the tile list and the SVG connector layer are measured
  in the same batch and stay in sync — without it, connectors can visually lag tiles
  by a frame.
- Connector path recomputation (§2.3) should read tile positions via
  `onLayoutAnimationStart`/`onLayoutAnimationComplete` callbacks on tiles plus a
  driving `useMotionValue` updated per-frame with `useAnimationFrame` (from
  `motion/react`) during the transition window — do not recompute path `d` from
  React state on every render, that would defeat the point of transform-only layout
  animation.

### 3.2 Reorder.Group vs manual drag

**Do not use `Reorder.Group`/`Reorder.Item`.** They're designed for linear
list-in-normal-flow reordering (think a to-do list) with y-axis (or x-axis) drag
constrained to the list's own flow. tubamp's chain lives on a pan/zoom canvas where:
- the list itself can be panned/zoomed underneath an active drag,
- tile drag must coordinate with connector redraw (a concern Reorder.Item has no hook
  for),
- selection ring and other `layoutId` shared elements need to interoperate with the
  drag, which is easier to reason about with manual `drag="x"` + `dragConstraints`
  + `onDrag`-computed index swapping (as specified in §2.2) than by fighting
  Reorder's own reordering opinions.

Manual approach: each tile is `motion.div` with `layout`, `drag="x"`,
`dragConstraints={chainTrackRef}`, `dragElastic={0}`, `onDrag` computing/publishing
index swaps into the ordering array in React state, `onDragEnd` committing the final
order (the actual `publishChainOrder` write). This mirrors what `Reorder.Group` does
internally but gives control over the canvas-coordinate interactions above.

### 3.3 AnimatePresence modes, applied

- `popLayout`: chain tile removal (§2.5), param-panel content swap (§2.6, chosen for
  responsiveness over `wait`'s correctness-but-latency).
- `sync`: preset crossfade (§2.9) — both old/new panel content intentionally overlap.
- `wait`: reserved for cases with exactly one interchangeable child where a visible
  gap is acceptable/desirable — e.g. TONE3000 overlay's internal step transitions
  (auth step → browse step), if those turn out to need strict sequencing. Default to
  `popLayout` elsewhere; only reach for `wait` when a hard sequencing requirement is
  identified, since it adds latency by design.

### 3.4 useSpring / useMotionValue for bypassing React re-render

- Meters (§2.12), pan/zoom (§2.13), and knob live-drag value (§2.8) are the three
  hot paths that MUST NOT touch `useState`/React re-render per frame. Pattern:
  ```ts
  const level = useMotionValue(0);
  useAnimationFrame(() => {
    level.set(nextSmoothedValue());
  });
  return <motion.div style={{ scaleY: level }} />;
  ```
- `useSpring(motionValue, springConfig)` wraps a source motion value to produce a
  second, spring-smoothed motion value — useful for zoom-to-fit's continuous read-out
  (if any UI displays live zoom %) without re-rendering React on every tick.
- React state is still the source of truth for anything that must survive
  serialization (chain order, param values reaching APVTS, bypass flags) — motion
  values are a *rendering* fast path layered on top, always eventually reconciled
  into state on gesture-end (drag stop, knob release), never a replacement for the
  state that drives audio.

### 3.5 Drag gestures with dragControls

- Use `useDragControls()` + `dragControls.start(event)` wherever drag must be
  initiated from a specific sub-element rather than the whole tile — e.g. if a tile
  gets a dedicated "grip" affordance distinct from its clickable body (click-to-select
  vs click-to-drag disambiguation). This avoids the default whole-element drag
  swallowing click/select gestures.
- Space-drag-to-pan (§2.13) also uses `dragControls`, started programmatically on
  `keydown(Space)` + `pointerdown` and stopped on `keyup`, rather than toggling the
  `drag` prop reactively — cleaner lifecycle, avoids drag-prop flicker mid-gesture.

### 3.6 On the Luma morph technique — explicitly not adopted

The Codrops Luma article's morph is a pre-rendered image-sequence flipbook (Canvas2D
`drawImage` over a progress scrub), not a DOM/shared-element morph. It doesn't
translate to Motion and isn't appropriate here anyway — tubamp has no continuous
shape-morph moments (nothing warps from a circle to a card, etc.). Where tubamp
*does* need shared-element continuity (selection ring, A/B pill, panel accent), that's
served correctly and far more cheaply by `layoutId` (§3.1). Noting this explicitly so
it isn't revisited later as a gap.

---

## 4. Performance guardrails (WKWebView, 60fps target)

WKWebView on the plugin's host has no browser-chrome overhead but also no devtools
GPU profiling shortcuts most people reach for — be conservative rather than
discover-by-profiling in production.

1. **Animate transform and opacity only.** Every token/choreography above is scoped
   to `x`, `y`, `scale`, `rotate`, `opacity`, and motion-value-driven `scaleY`/`scaleX`
   (meters). No animation in this spec targets `width`, `height`, `top`, `left`,
   `background-color` as a *continuous* tween on a large surface — colour flips
   (§2.7 LED, §2.9 accent) are short (`instant`/`fast`) discrete state changes, not
   sustained animations, and are cheap enough on small elements.

2. **No animated `filter`/`box-shadow` blur on large surfaces.** Tile lift shadow
   (§2.2), arrival ring (§2.4), and any glow effects use the **pseudo-element
   opacity-crossfade trick**: pre-render both the rest-state and lifted-state
   shadow/glow as separate absolutely-positioned layers (`::before`/`::after` or
   sibling `motion.div`s) with the expensive `box-shadow`/`filter` baked in as a
   *static* CSS value, and animate only their `opacity` between 0 and 1. Never put
   `boxShadow` or `filter` inside a Motion `animate={{ }}` target.

3. **`will-change` / layer promotion discipline.** Apply `will-change: transform` (via
   a class, not inline-styled per-frame) only to elements actively mid-gesture:
   the currently-dragged tile, the canvas content layer during pan/zoom, the
   currently-crossfading panel. Remove it (class toggle off) on gesture/animation
   end — leaving `will-change` on permanently forces persistent compositor layers
   across potentially a dozen idle tiles, which costs memory/compositing budget for
   no benefit. Motion's `layout` animations already promote elements during the
   animation window automatically; don't double up with manual `will-change` on
   `layout`-only elements.

4. **Avoid re-render-driven animation.** Anything that would otherwise update on
   every pointer-move or every audio-callback tick (drag position, meters, pan/zoom,
   live knob value) goes through `useMotionValue`/`useSpring`/`useAnimationFrame`
   (§3.4), never `useState`. A `setState` call belongs only at gesture-end
   (commit-to-chain-order, commit-to-APVTS) — mid-gesture updates must never re-render
   React.

5. **Cap simultaneous layout animations.** A reorder/add/remove touches at most the
   moved tile + its connectors + however many siblings shift — with ≤8-block chains
   (realistic ceiling for this UI) this is fine, but explicitly do not trigger a
   *global* layout recompute (e.g. re-keying the entire tile list, or wrapping
   unrelated UI in the same `LayoutGroup`) on every chain edit. Scope `LayoutGroup`
   to just the chain+connector region (§3.1), not the whole editor, so panel/header/
   modal layout is never incidentally swept into a chain-edit's layout pass.

6. **SVG path animation cost.** Connector `pathLength` draw-in (§2.3) is fine at the
   scale of ≤8 short segments, but: (a) don't animate `stroke-dasharray`/`dashoffset`
   for the bypass-dim effect — use opacity (already specified in §2.3); (b) don't
   recompute/re-set `d` attributes every React render — only on layout-animation
   frames via the rAF-driven approach in §3.1; (c) keep each connector a single
   `path`/`line`, not a multi-path composite, to keep the SVG's render tree flat.

7. **Meter updates via rAF + motion values, not state — with an explicit rAF budget.**
   All meters (input, output, gain-reduction, any per-block level indicators) share
   one `requestAnimationFrame` driver, not one rAF loop per meter — a single loop
   reads all pending levels and calls `.set()` on each meter's motion value per
   frame. This keeps the per-frame JS cost O(1 loop) regardless of meter count and
   avoids N separate rAF callbacks competing for the same frame budget.

8. **Reduced-motion.** Respect `prefers-reduced-motion`: gate the board-entrance
   stagger (§2.1), zoom-to-fit spring (§2.13), and A/B directional shift (§2.10) behind
   a check that collapses them to instant/opacity-only fallbacks — Motion's
   `useReducedMotion()` hook — since this is a professional tool used for long
   sessions, not a marketing site; motion should never become friction for users who
   disable it.
